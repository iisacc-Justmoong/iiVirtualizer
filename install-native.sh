#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${1:-"$root/build/install"}
gateway_openssl_root=${OPENSSL_ROOT_DIR:-}
if [ -z "$gateway_openssl_root" ] && command -v brew >/dev/null 2>&1; then
    gateway_openssl_root=$(brew --prefix openssl@4 2>/dev/null || brew --prefix openssl@3 2>/dev/null || true)
fi
cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSDKCore;$HOME/.local/SDK/iiJson;$HOME/.local/SDK/iiNetwork;$HOME/.local/SDK/iiAccountManager;$HOME/.local/SDK/iiFileProvider" \
    -DOPENSSL_ROOT_DIR="$gateway_openssl_root"
cmake --build "$root/build" --parallel 4
ctest --test-dir "$root/build" --output-on-failure
cmake --install "$root/build" --prefix "$prefix"
