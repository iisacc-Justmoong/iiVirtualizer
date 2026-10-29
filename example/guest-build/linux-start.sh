#!/bin/sh
set -eu
mkdir -p /opt/iiv/work/build
exec > /opt/iiv/work/build/bootstrap.log 2>&1
IIV_CONSOLE=/dev/ttyAMA0
[ -c "$IIV_CONSOLE" ] || IIV_CONSOLE=/dev/ttyS0
trap 'status=$?; if [ "$status" -ne 0 ]; then tail -100 /opt/iiv/work/build/bootstrap.log > "$IIV_CONSOLE"; fi' EXIT
echo IIV_BUILD_BOOTSTRAP_START > "$IIV_CONSOLE"
export DEBIAN_FRONTEND=noninteractive
resize2fs /dev/vda
tar -xzf /opt/iiv/sources.tar.gz -C /opt/iiv/work
sed -i 's|http://|https://|g' /etc/apt/sources.list.d/debian.sources
apt-get update
apt-get install -y --no-install-recommends g++ cmake ninja-build
echo IIV_BUILD_COMPILER_READY > "$IIV_CONSOLE"
cd /opt/iiv/work
cmake -S source/iiSDKCore -B build/core -G Ninja \
    -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS_RELEASE=-O0 -DNDEBUG' \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DIISDKCORE_BUILD_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX=/opt/iiv/work/build/install
cmake --build build/core --target install --parallel 3
echo IIV_BUILD_CORE_INSTALLED > "$IIV_CONSOLE"
cmake -S source/iiVirtualizer/src/builder -B build/agent -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/iiv/work/build/install
cmake --build build/agent --parallel 3
echo IIV_BUILD_AGENT_READY > "$IIV_CONSOLE"
exec build/agent/iiv-build-agent /opt/iiv/job.json /dev/virtio-ports/org.iisacc.agent
