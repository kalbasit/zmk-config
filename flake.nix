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
        ".cmake"
        ".conf"
        ".defconfig"
        ".dts"
        ".dtsi"
        ".json"
        ".keymap"
        ".overlay"
        ".shield"
        ".yml"
        "_defconfig"
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
            zephyrDepsHash = "sha256-IGyYY6MzYoHzVRlYioVy84GRH7ZN5uyQcarJIo5oHiQ=";
            meta = {
              description = "ZMK firmware for Cradio (Sweep)";
              license = nixpkgs.lib.licenses.mit;
              platforms = nixpkgs.lib.platforms.all;
            };
          };
          flash = zmk-nix.packages.${system}.flash.override { inherit firmware; };
        };

        toucan = rec {
          firmware = zmk-nix.legacyPackages.${system}.buildSplitKeyboard {
            name = "toucan-firmware";
            inherit src;
            board = "seeeduino_xiao_ble";
            # Left: toucan_left + rgbled_adapter + nice_view_gem
            # Right: toucan_right + rgbled_adapter (nice_view_gem compiled
            # in but harmless — right has no display header).
            shield = "toucan_%PART% rgbled_adapter nice_view_gem";
            # Placeholder — run `nix build .#toucan.firmware` to get the
            # real hash from the mismatch error, then set it here.
            zephyrDepsHash = nixpkgs.lib.fakeHash;
            meta = {
              description = "ZMK firmware for Toucan";
              license = nixpkgs.lib.licenses.mit;
              platforms = nixpkgs.lib.platforms.all;
            };
          };
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
              runtimeInputs = [ nixpkgs.legacyPackages.${system}.keymap-drawer ];
              text = ''
                # Get the repository root
                REPO_ROOT="$(git rev-parse --show-toplevel)"

                if [ -z "$REPO_ROOT" ]; then
                    echo "Error: could not determine repository root. Are you in a git repository?" >&2
                    exit 1
                fi

                # Parse the keymap to YAML, then draw the SVG
                echo "Parsing keymap from config/cradio.keymap..."
                keymap parse -c 10 -z "''${REPO_ROOT}/config/cradio.keymap" -o "''${REPO_ROOT}/assets/cradio_keymap.yaml"

                echo "Generating SVG from parsed keymap..."
                keymap draw "''${REPO_ROOT}/assets/cradio_keymap.yaml" -o "''${REPO_ROOT}/assets/my_keymap.svg"

                echo "Successfully updated assets/my_keymap.svg!"
              '';
            }
          }/bin/update-assets";
        };
      });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          # canopen has flaky timing tests on macOS that cause the build to fail.
          # We inject a patched python3 via shell.nix's callPackage arg instead of
          # using zmk-nix.devShells directly, since that is pre-evaluated with
          # zmk-nix's own pkgs and cannot be retroactively patched via overlays.
          python3 = pkgs.python3.override {
            packageOverrides = _: prev: {
              canopen = prev.canopen.overridePythonAttrs (_: { doCheck = false; });
            };
          };
        in
        {
          default = pkgs.callPackage "${zmk-nix}/nix/shell.nix" { inherit python3; };
        });
    };
}
