{
  description = "ethreads — C++23 Threading Library with Task Scheduling and Coroutines";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
    exstd = {
      url = "github:RealAstolfo/exstd";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.flake-utils.follows = "flake-utils";
    };
  };

  outputs = { self, nixpkgs, flake-utils, exstd }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = nixpkgs.legacyPackages.${system};
        exstdPkg = exstd.packages.${system}.default;
      in {
        packages.default = pkgs.stdenv.mkDerivation {
          pname = "ethreads";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = with pkgs; [ zig gnumake ];
          buildInputs = with pkgs; [ liburing ];

          postUnpack = ''
            rm -rf $sourceRoot/vendors
            mkdir -p $sourceRoot/vendors
            cp -r ${exstdPkg.passthru.src-with-vendors} $sourceRoot/vendors/exstd
            chmod -R u+w $sourceRoot/vendors
          '';

          buildPhase = ''
            make threading.o threading.a
          '';

          installPhase = ''
            mkdir -p $out/include $out/lib
            cp -r include/* $out/include/
            cp threading.o threading.a $out/lib/
          '';

          passthru.src-with-vendors = pkgs.runCommand "ethreads-src" {} ''
            cp -r ${self} $out
            chmod -R u+w $out
            rm -rf $out/vendors
            mkdir -p $out/vendors
            cp -r ${exstdPkg.passthru.src-with-vendors} $out/vendors/exstd
            chmod -R u+w $out/vendors
          '';
        };

        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            zig
            gcc
            gnumake
            valgrind
            liburing
          ];

          shellHook = ''
            if [ ! -d vendors/exstd ] || [ -L vendors/exstd ]; then
              rm -rf vendors/exstd
              mkdir -p vendors
              cp -r ${exstdPkg.passthru.src-with-vendors} vendors/exstd
              chmod -R u+w vendors/exstd
            fi
            echo "ethreads development environment"
            echo "  Build: make all"
          '';
        };
      }
    );
}
