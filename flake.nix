{
  description = "Wayland interception log mirror";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }: let
    systems = [ "x86_64-linux" ];
    forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system);
  in {
    packages = forAllSystems (system: let
      pkgs = import nixpkgs { inherit system; };
    in {
      default = pkgs.stdenv.mkDerivation {
        pname = "scribe-tap";
        version = "0.1.0";
        src = ./.;
        buildPhase = "make";
        installPhase = ''
          make install PREFIX=$out
        '';
        meta = with pkgs.lib; {
          description = "Keystroke mirror for interception-tools on Hyprland";
          license = licenses.mit;
          platforms = platforms.linux;
        };
      };
    });

    devShells = forAllSystems (system: let
      pkgs = import nixpkgs { inherit system; };
    in {
      default = pkgs.mkShell {
        packages = [
          pkgs.gcc
          pkgs.wl-clipboard
          pkgs.xclip
          pkgs.pkg-config
        ];
      };
    });
  };
}
