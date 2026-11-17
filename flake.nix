{
  description = "nix-sym - symbolise core dumps from Nix-built processes";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/d5cc6833999746cffdfe0598f7bc801a57b887a9";
    devenv = {
      url = "github:cachix/devenv/b904dcb51fe48c30db250038241507f60752f222";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      devenv,
      ...
    }@inputs:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      version = "0.2.0";

      mkDemoCpp =
        pkgs: src: separate:
        pkgs.stdenv.mkDerivation {
          pname = "nix-sym-demo-cpp";
          version = "0.2.0";
          inherit src;
          separateDebugInfo = separate;
          postPatch = ''
            sed -i 's/ORIGINAL/PATCHED/' src/main.cpp
          '';
          buildPhase = ''
            c++ -O2 -g -fno-omit-frame-pointer -std=c++20 -o demo src/main.cpp
          '';
          installPhase = ''
            install -Dm755 demo $out/bin/demo
          '';
        };

      mkDemoRust =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "nix-sym-demo-rust";
          version = "0.2.0";
          src = ./fixtures/demo-rust;
          nativeBuildInputs = [ pkgs.rustc ];
          separateDebugInfo = true;
          buildPhase = ''
            rustc --edition 2021 -C opt-level=2 -C debuginfo=2 -C strip=none \
              -C link-arg=-Wl,--build-id=sha1 -o demo src/main.rs
          '';
          installPhase = ''
            install -Dm755 demo $out/bin/demo
          '';
        };

      mkDemoLive =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "nix-sym-demo-live";
          version = "0.2.0";
          src = ./fixtures/demo-live;
          separateDebugInfo = true;
          buildPhase = ''
            c++ -O2 -g -fno-omit-frame-pointer -std=c++20 -o demo src/main.cpp
          '';
          installPhase = ''
            install -Dm755 demo $out/bin/demo
          '';
        };

      mkDemoHaskell =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "nix-sym-demo-haskell";
          version = "0.2.0";
          src = ./fixtures/demo-haskell;
          nativeBuildInputs = [ pkgs.ghc ];
          separateDebugInfo = true;
          buildPhase = ''
            ghc -O1 -g -optl-Wl,--build-id=sha1 -o demo Main.hs
          '';
          installPhase = ''
            install -Dm755 demo $out/bin/demo
          '';
        };

      mkNixSym =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "nix-sym";
          inherit version;
          src = self;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [
            pkgs.elfutils
            pkgs.elfutils.dev
            pkgs.libarchive
            pkgs.libarchive.dev
            pkgs.zstd
            pkgs.zstd.dev
          ];

          makeFlags = [ "PREFIX=$(out)" ];
          doCheck = true;
          checkPhase = "make check";

          # Dogfooding: the tool's own crashes should be symbolisable.
          separateDebugInfo = true;

          meta = with pkgs.lib; {
            description = "Symbolise core dumps from Nix-built processes";
            license = licenses.asl20;
            mainProgram = "nix-sym";
            platforms = platforms.linux;
          };
        };
    in
    {
      packages = forAllSystems (pkgs: {
        nix-sym = mkNixSym pkgs;
        default = mkNixSym pkgs;
        fixture-demo-cpp = mkDemoCpp pkgs ./fixtures/demo-cpp true;
        fixture-demo-cpp-tar = mkDemoCpp pkgs ./fixtures/demo-cpp.tar.gz true;
        fixture-demo-nodbg = mkDemoCpp pkgs ./fixtures/demo-cpp false;
        fixture-demo-rust = mkDemoRust pkgs;
        fixture-demo-live = mkDemoLive pkgs;
        fixture-demo-haskell = mkDemoHaskell pkgs;
      });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = "${mkNixSym pkgs}/bin/nix-sym";
        };
      });

      devShells = forAllSystems (pkgs: {
        ci = pkgs.mkShell {
          packages = with pkgs; [
            binutils
            cacert
            coreutils
            elfutils
            elfutils.bin
            elfutils.dev
            file
            gcc
            gdb
            gnumake
            jq
            libarchive
            libarchive.dev
            nix
            pkg-config
            python3
            zstd
            zstd.dev
          ];
        };
        format = pkgs.mkShell { packages = [ pkgs.clang-tools ]; };
        fuzz = pkgs.mkShell { packages = [ pkgs.clang ]; };
        default = devenv.lib.mkShell {
          inherit inputs pkgs;
          modules = [
            ({ lib, ... }: {
              devenv.root = lib.mkForce (
                let
                  pwd = builtins.getEnv "PWD";
                in
                if pwd != "" then pwd else "/tmp/nix-sym-devenv"
              );
            })
            ./devenv.nix
          ];
        };
      });

      checks = forAllSystems (pkgs: {
        default = mkNixSym pkgs;
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
