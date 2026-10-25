# iiVirtualizer

A C++23, Qt-free virtual machine runtime for **Linux + iiSDK** guests. It provides persistent instances, explicit vCPU/memory/disk allocation, lifecycle control, console logs and a guest control channel. The host uses iiSDKCore Process and iiJson; the guest runs iiSDKCore as Linux PID 1.

One `Runtime` controls multiple concurrently running VMs through one bounded iiSDK worker pool. Each VM owns its QEMU process, UUID, disk, console and control endpoints. Per-instance locks isolate operations; a busy or failed VM does not block unrelated instance controls. `submit` returns an iiSDK Future, `apply` controls several IDs with individual results, and `snapshot` retains failed rows alongside healthy VMs.

```cpp
ii::virtualizer::Runtime runtime(state_directory, ii::virtualizer::qemu_backend(),
                                {.workers=4, .capacity=128});
auto first = runtime.submit(ii::virtualizer::Operation::start, "vm-a");
auto second = runtime.submit(ii::virtualizer::Operation::start, "vm-b");
auto a = first.get();
auto b = second.get();
```

The CLI supports `start-many vm-a vm-b`, `describe-many`, `pause-many`, `resume-many` and `stop-many`, with `--workers` selecting controller concurrency. Batch results retain input order and per-instance errors; partial failures return exit code 1 without rolling back successful VMs.

The initial host adapter supports macOS/HVF and Linux/KVM, plus explicit QEMU/TCG emulation. Hardware acceleration requires matching host and guest architectures. The supplied image recipe builds an aarch64 guest named **iiOS**. The API also describes x86_64 guests when a compatible kernel and initramfs are supplied.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSDKCore;$HOME/.local/SDK/iiJson"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Build the optional external QEMU engine locally with `python3 tools/bootstrap_qemu.py`. All downloads, tools, images, VM state and validation outputs belong under `build/`. An existing QEMU can be selected with `--qemu-aarch64` or `--qemu-x86_64`.

The installed CMake package exports `iiVirtualizer::iiVirtualizer`. Run `./install-native.sh` to stage it under `build/install`. iiSDKCore and iiJson remain explicit package dependencies; QEMU is selected at runtime. `tests/consumer` verifies consumption through the installed package.

See [the Korean architecture guide](docs/architecture.md) and [the operating guide](docs/usage.md) for guest construction and lifecycle commands. This is a single-host runtime; distributed scheduling, public cloud accounts, billing and VPC control belong in an upper layer.

Real ARM64 HVF and TCG boot verification covers iiSDK PID 1, persistent ext4 storage, DHCP/NAT and HTTP forwarding, pause/resume, reboot, and graceful/forced shutdown. See [validation evidence and platform limits](docs/validation.md).
