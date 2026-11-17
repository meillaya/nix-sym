{
  pkgs,
  config,
  ...
}:
{
  packages = with pkgs; [
    binutils
    cacert
    clang
    clang-tools
    coreutils
    curl
    elfutils
    elfutils.bin
    file
    gcc
    gdb
    git
    gnumake
    jq
    libarchive
    nix
    pkg-config
    (python3.withPackages (ps: [ ps.jsonschema ]))
    shellcheck
    zstd
  ];

  env = {
    SSL_CERT_FILE = "${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt";
    NIX_SYM_CACHE = "${config.devenv.root}/.cache/nix-sym";
  };

  scripts = {
    build.exec = "make -j$(nproc)";
    test.exec = "make check";
    check.exec = "make clean && make -j$(nproc) && make check";
    integration.exec = "bash scripts/test-integration.sh";
    format.exec = "make format";
    prose.exec = "${pkgs.python3}/bin/python3 -B /home/mei/nixos/tools/check-prose.py README.md docs";
  };

  enterShell = ''
    printf 'nix-sym dev shell\n'
    printf 'gcc    %s\n' "$(gcc --version | head -1)"
    printf 'nix    %s\n' "$(nix --version)"
    printf 'libdw  %s\n' "$(pkg-config --modversion libdw)"
    echo
    echo 'build:       make'
    echo 'tests:       make check'
    echo 'integration: scripts/test-integration.sh'
  '';

  enterTest = ''
    make clean
    make -j$(nproc)
    make check
  '';
}
