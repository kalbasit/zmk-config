{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/release-25.11";

    zmk-nix = {
      url = "github:lilyinstarlight/zmk-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      zmk-nix,
    }:
    let
      forAllSystems = nixpkgs.lib.genAttrs (nixpkgs.lib.attrNames zmk-nix.packages);
    in
    {
      packages = forAllSystems (system: rec {
        default = firmware;

        firmware = zmk-nix.legacyPackages.${system}.buildSplitKeyboard {
          name = "firmware";

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

          board = "nice_nano_v2";
          shield = "cradio_%PART%";

          zephyrDepsHash = "sha256-IGyYY6MzYoHzVRlYioVy84GRH7ZN5uyQcarJIo5oHiQ=";

          meta = {
            description = "ZMK firmware";
            license = nixpkgs.lib.licenses.mit;
            platforms = nixpkgs.lib.platforms.all;
          };
        };

        flash = zmk-nix.packages.${system}.flash.override { inherit firmware; };
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

      devShells = forAllSystems (system: {
        default = zmk-nix.devShells.${system}.default;
      });
    };
}
