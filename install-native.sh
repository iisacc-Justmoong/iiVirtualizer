#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${1:-"$root/build/install"}
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSDKCore;$HOME/.local/SDK/iiJson"
cmake --build "$root/build" --parallel 4
ctest --test-dir "$root/build" --output-on-failure
cmake --install "$root/build" --prefix "$prefix"
