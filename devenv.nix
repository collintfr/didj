# Development environment for didj (https://devenv.sh).
#
#   devenv shell        # enter it; then:
#   didj-build          # configure and build (Release, in ./build)
#   didj-dumps [ARGS]   # download and verify the dumps (didj-fetch ARGS)
#   didj-play [ARGS]    # play, e.g. didj-play --cart SNC
#   didj-test           # run the unit tests
#
# Or without entering the shell: devenv shell didj-play --cart SNC
{ pkgs, ... }:

{
  packages = with pkgs; [
    cmake
    ninja
    git
    curl
    pkg-config
    python3
    SDL2
  ];

  scripts.didj-build.exec = ''
    set -e
    cd "$DEVENV_ROOT"
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
  '';

  scripts.didj-dumps.exec = ''
    set -e
    [ -x "$DEVENV_ROOT/build/didj-fetch" ] || didj-build
    "$DEVENV_ROOT/build/didj-fetch" "$@"
  '';

  scripts.didj-play.exec = ''
    set -e
    [ -x "$DEVENV_ROOT/build/didj" ] || didj-build
    exec "$DEVENV_ROOT/build/didj" "$@"
  '';

  scripts.didj-test.exec = ''
    set -e
    [ -d "$DEVENV_ROOT/build" ] || didj-build
    ctest --test-dir "$DEVENV_ROOT/build" --output-on-failure
  '';

  enterShell = ''
    echo "didj: didj-build, didj-dumps [all|os|SNC ...], didj-play [--cart SNC], didj-test"
  '';

  enterTest = ''
    didj-build
    didj-test
  '';
}
