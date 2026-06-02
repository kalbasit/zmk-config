// Toucan debug instrumentation. Enabled by CONFIG_TOUCAN_DEBUG_INSTRUMENTATION.
//
// Emits four kinds of diagnostic lines on the `toucan_dbg` log module so we
// can correlate the right-half lock-up against actual ZMK state:
//
//   1. ACT  — every ZMK activity-state transition (ACTIVE/IDLE/SLEEP)
//   2. STATE — periodic snapshot every 5 s (current state, ms since the last
//      input event, total input-event counter, system-workqueue heartbeat)
//   3. BLE  — every LE connection parameter update (interval/latency/timeout)
//      reported by the host stack for any connection
//   4. STK  — per-thread free-stack bytes AND run-state (running/pending/...),
//      walked alongside each STATE snapshot
//
// The monitor runs in its own k_thread, NOT on the system workqueue, so a
// sysworkq deadlock (the leading hypothesis for the peripheral-forwarding
// stall) still produces STATE/STK output while sysworkq is wedged.
//
// The sysworkq heartbeat is a self-rescheduling delayable work item on the
// SYSTEM workqueue. The 31-min logs showed every peripheral periodic task
// (kscan @3:03, trackpad @3:33, battery @5:00) wind down one by one while all
// stacks stayed healthy and no thread crashed — the signature of a blocked
// shared work queue. If the heartbeat freezes while this monitor keeps logging,
// the system workqueue is the wedged queue; if it keeps ticking, the block is
// elsewhere (a dedicated kscan/input thread) and we look there next.

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/sys/atomic.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/activity.h>

LOG_MODULE_REGISTER(toucan_dbg, LOG_LEVEL_INF);

static const char *state_name(enum zmk_activity_state s) {
    switch (s) {
    case ZMK_ACTIVITY_ACTIVE:
        return "ACTIVE";
    case ZMK_ACTIVITY_IDLE:
        return "IDLE";
    case ZMK_ACTIVITY_SLEEP:
        return "SLEEP";
    default:
        return "?";
    }
}

// ── Input-event tap ────────────────────────────────────────────────────────
// INPUT_CALLBACK_DEFINE with NULL fires for every input device. Cheap counter
// + last-seen timestamp; the existing activity_input_listener already does
// k_work_submit on every event, so adding atomics here costs nothing.

static atomic_t input_count = ATOMIC_INIT(0);
static atomic_t input_last_ms = ATOMIC_INIT(0);

static void dbg_input_listener(struct input_event *ev) {
    atomic_inc(&input_count);
    atomic_set(&input_last_ms, (atomic_val_t)k_uptime_get_32());
}
INPUT_CALLBACK_DEFINE(NULL, dbg_input_listener);

// ── Activity-state listener ────────────────────────────────────────────────
// Subscribes to zmk_activity_state_changed. Logs every transition with the
// uptime, ms since the last input event, and total input count so we can
// answer the central question: did the system actually enter IDLE while the
// trackpad was in use?

static int dbg_act_listener(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    uint32_t now = k_uptime_get_32();
    uint32_t last_in_ms = (uint32_t)atomic_get(&input_last_ms);
    int32_t since_input = (int32_t)(now - last_in_ms);
    LOG_INF("ACT %s @%ums (last_input %dms ago, inputs=%ld)", state_name(ev->state),
            (unsigned)now, since_input, atomic_get(&input_count));
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(toucan_dbg_act, dbg_act_listener);
ZMK_SUBSCRIPTION(toucan_dbg_act, zmk_activity_state_changed);

// ── BLE connection parameter updates ───────────────────────────────────────
// Confirms or rules out conn-param renegotiation as the trigger. Interval is
// in 1.25 ms units, timeout in 10 ms units — print derived ms for readability.

static void dbg_connected(struct bt_conn *conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("BLE %s connected reason=0x%02x", addr, reason);
}

static void dbg_disconnected(struct bt_conn *conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("BLE %s disconnected reason=0x%02x", addr, reason);
}

static void dbg_le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                                 uint16_t timeout) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    // interval * 1.25 ms = interval * 5 / 4
    unsigned int_us = ((unsigned)interval * 5u * 1000u) / 4u;
    LOG_INF("BLE %s le_param: int=%u (%u.%03ums) lat=%u to=%u (%ums)", addr, interval,
            int_us / 1000u, int_us % 1000u, latency, timeout, (unsigned)timeout * 10u);
}

BT_CONN_CB_DEFINE(toucan_dbg_conn_cb) = {
    .connected = dbg_connected,
    .disconnected = dbg_disconnected,
    .le_param_updated = dbg_le_param_updated,
};

// ── System-workqueue heartbeat ─────────────────────────────────────────────
// A self-rescheduling delayable work item. k_work_reschedule() submits to the
// SYSTEM workqueue, so its handler only runs while that queue is draining work.
// The monitor thread (a separate k_thread) prints this counter; if it stops
// advancing while STATE/STK keep printing, the system workqueue is wedged.

static atomic_t sysworkq_hb = ATOMIC_INIT(0);
static void sysworkq_hb_work(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sysworkq_hb_dwork, sysworkq_hb_work);

static void sysworkq_hb_work(struct k_work *work) {
    ARG_UNUSED(work);
    atomic_inc(&sysworkq_hb);
    k_work_reschedule(&sysworkq_hb_dwork, K_SECONDS(1));
}

// ── Periodic monitor thread + per-thread stack walk ────────────────────────
// Runs at K_LOWEST_APPLICATION_THREAD_PRIO on its own stack, so a deadlocked
// system workqueue does not silence it.

static void thread_walker(const struct k_thread *th, void *udata) {
    ARG_UNUSED(udata);
    size_t unused = 0;
    int err = k_thread_stack_space_get(th, &unused);
    if (err != 0) {
        return;
    }
    const char *name = k_thread_name_get((k_tid_t)th);
    unsigned total = (unsigned)th->stack_info.size;
    char stbuf[32];
    const char *st = k_thread_state_str((k_tid_t)th, stbuf, sizeof(stbuf));
    LOG_INF("  STK %-24s free=%u/%u %s", name && name[0] ? name : "?", (unsigned)unused, total,
            st);
}

static void monitor_thread(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    // Sleep a moment so SYS_INIT and BT init have settled before first log.
    k_sleep(K_SECONDS(2));

    while (true) {
        uint32_t now = k_uptime_get_32();
        uint32_t last_in_ms = (uint32_t)atomic_get(&input_last_ms);
        int32_t since_input = (int32_t)(now - last_in_ms);
        LOG_INF("STATE %s @%ums last_input=%dms ago inputs=%ld sysworkq_hb=%ld",
                state_name(zmk_activity_get_state()), (unsigned)now, since_input,
                atomic_get(&input_count), atomic_get(&sysworkq_hb));
        k_thread_foreach_unlocked(thread_walker, NULL);

        k_sleep(K_SECONDS(5));
    }
}

K_THREAD_STACK_DEFINE(toucan_monitor_stack, 1024);
static struct k_thread toucan_monitor_data;

static int dbg_init(void) {
    atomic_set(&input_last_ms, (atomic_val_t)k_uptime_get_32());
    k_work_reschedule(&sysworkq_hb_dwork, K_SECONDS(1));
    k_thread_create(&toucan_monitor_data, toucan_monitor_stack,
                    K_THREAD_STACK_SIZEOF(toucan_monitor_stack), monitor_thread, NULL, NULL,
                    NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&toucan_monitor_data, "toucan_dbg");
    return 0;
}
SYS_INIT(dbg_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
