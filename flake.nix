{
  inputs = {
    zmk-nix.url = "github:lilyinstarlight/zmk-nix";
  };

  outputs =
    {
      self,
      zmk-nix,
    }:
    let
      inherit (zmk-nix.inputs) nixpkgs;

      forAllSystems = nixpkgs.lib.genAttrs (nixpkgs.lib.attrNames zmk-nix.packages);

      src = nixpkgs.lib.sourceFilesBySuffices self [
        ".board"
        ".c"
        ".cmake"
        ".conf"
        ".defconfig"
        ".dts"
        ".dtsi"
        ".h"
        ".json"
        ".keymap"
        ".overlay"
        ".shield"
        ".yml"
        "_defconfig"
        "CMakeLists.txt"
      ];
    in
    {
      legacyPackages = forAllSystems (system: {
        cradio = rec {
          firmware = zmk-nix.legacyPackages.${system}.buildSplitKeyboard {
            name = "cradio-firmware";
            inherit src;
            board = "nice_nano_v2";
            shield = "cradio_%PART%";
            # Run `nix build .#cradio.firmware` to get the actual hash after
            # any west.yml change.
            zephyrDepsHash = "sha256-63E95iZ/uJRgs6YOmEOUrtd9UM22oquof3a+O4wTIgs=";
            meta = {
              description = "ZMK firmware for Cradio (Sweep)";
              license = nixpkgs.lib.licenses.mit;
              platforms = nixpkgs.lib.platforms.all;
            };
          };
          flash = zmk-nix.packages.${system}.flash.override { inherit firmware; };
        };

        toucan =
          let
            inherit (zmk-nix.legacyPackages.${system}) buildKeyboard;
            pkgs = nixpkgs.legacyPackages.${system};
            # Left and right have different shield stacks:
            #   left  — nice_view_gem (defines the display UI, references nice_view_spi
            #           from the toucan_left overlay)
            #   right — no display shield; nice_view_gem must be omitted or the
            #           devicetree fails with "undefined node label 'nice_view_spi'"
            left = buildKeyboard {
              name = "toucan-firmware-left";
              inherit src;
              board = "seeeduino_xiao_ble";
              shield = "toucan_left rgbled_adapter nice_view_gem";
              zephyrDepsHash = "sha256-63E95iZ/uJRgs6YOmEOUrtd9UM22oquof3a+O4wTIgs=";
            };
            right = buildKeyboard {
              name = "toucan-firmware-right";
              inherit src;
              board = "seeeduino_xiao_ble";
              shield = "toucan_right rgbled_adapter";
              zephyrDepsHash = "sha256-63E95iZ/uJRgs6YOmEOUrtd9UM22oquof3a+O4wTIgs=";
              # Reuse the west deps already fetched for the left build.
              westDeps = left.westDeps;
            };
          in
          rec {
            firmware = pkgs.runCommand "toucan-firmware" {
              meta = {
                description = "ZMK firmware for Toucan";
                license = nixpkgs.lib.licenses.mit;
                platforms = nixpkgs.lib.platforms.all;
              };
            } ''
              mkdir $out
              ln -s ${left}/zmk.uf2 $out/zmk_left.uf2
              ln -s ${right}/zmk.uf2 $out/zmk_right.uf2
            '';
            flash = zmk-nix.packages.${system}.flash.override { inherit firmware; };
          };
      });

      packages = forAllSystems (system: {
        default = self.legacyPackages.${system}.cradio.firmware;
        update = zmk-nix.packages.${system}.update;
      });

      apps = forAllSystems (system: {
        update-assets = {
          type = "app";
          program = "${
            nixpkgs.legacyPackages.${system}.writeShellApplication {
              name = "update-assets";
              runtimeInputs = [
                nixpkgs.legacyPackages.${system}.keymap-drawer
                nixpkgs.legacyPackages.${system}.yq-go
              ];
              text = ''
                # Get the repository root
                REPO_ROOT="$(git rev-parse --show-toplevel)"

                if [ -z "$REPO_ROOT" ]; then
                    echo "Error: could not determine repository root. Are you in a git repository?" >&2
                    exit 1
                fi

                # ── Cradio ────────────────────────────────────────────────────
                echo "Parsing cradio keymap..."
                keymap parse -c 10 -z "''${REPO_ROOT}/config/cradio.keymap" \
                    -o "''${REPO_ROOT}/assets/cradio_keymap.yaml"
                echo "Drawing cradio SVG..."
                keymap draw "''${REPO_ROOT}/assets/cradio_keymap.yaml" \
                    -o "''${REPO_ROOT}/assets/cradio_keymap.svg"

                # ── Toucan ────────────────────────────────────────────────────
                echo "Parsing toucan keymap..."
                keymap parse -c 12 -z "''${REPO_ROOT}/config/toucan.keymap" \
                    -o "''${REPO_ROOT}/assets/toucan_keymap.yaml"
                # toucan isn't in keymap-drawer's ZMK keyboard database.
                # Parse emits 42 positions (12-col matrix): 3 main rows of 12 + 1
                # thumb row of 6. Strip outer columns from main rows (.[0] and .[-1]
                # per row) to get 10 per row, then use crkbd LAYOUT_split_3x5_3
                # (5-col corne, 36 keys) so keymap-drawer uses the correct column-
                # stagger and 1u thumb keys. Thumb row keeps all 6 positions
                # (outermost thumbs are &none = blank keys, which is correct).
                yq -i '.layers[] |= [.[0][1:-1], .[1][1:-1], .[2][1:-1], .[3]]' \
                    "''${REPO_ROOT}/assets/toucan_keymap.yaml"
                yq -i '.layout = {"qmk_keyboard": "corne_rotated", "layout_name": "LAYOUT_split_3x5_3"}' \
                    "''${REPO_ROOT}/assets/toucan_keymap.yaml"
                echo "Drawing toucan SVG..."
                keymap draw "''${REPO_ROOT}/assets/toucan_keymap.yaml" \
                    -o "''${REPO_ROOT}/assets/toucan_keymap.svg"

                echo "Done — assets/cradio_keymap.svg and assets/toucan_keymap.svg updated."
              '';
            }
          }/bin/update-assets";
        };
      });

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          # canopen has flaky timing tests on macOS that cause the build to fail.
          # We inject a patched python3 via shell.nix's callPackage arg instead of
          # using zmk-nix.devShells directly, since that is pre-evaluated with
          # zmk-nix's own pkgs and cannot be retroactively patched via overlays.
          python3 = pkgs.python3.override {
            packageOverrides = _: prev: {
              canopen = prev.canopen.overridePythonAttrs (_: {
                doCheck = false;
              });
            };
          };
        in
        {
          default = pkgs.callPackage "${zmk-nix}/nix/shell.nix" { inherit python3; };
        }
      );
    };
}
