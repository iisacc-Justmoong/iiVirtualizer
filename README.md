# iiVirtualizer

A C++23, Qt-free virtual machine runtime for **Linux + iiSDK** guests, Debian and Android-x86 boot, and Windows Server Core build workers. It provides persistent instances, explicit vCPU/memory/disk allocation, lifecycle control, console logs and a guest control channel. The host uses iiSDKCore Process and iiJson; the supplied iiOS guest runs iiSDKCore as Linux PID 1.

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

The optional **Gateway** delegates VM provisioning and lifecycle control over a fixed OpenSSH subsystem. The receiving GatewayAgent verifies the current iisacc account session over HTTPS, binds the verified subject to runtime/instance/action grants, and enforces per-instance creation limits. It requires explicit SSH host trust and keeps session proofs out of argv, environment, logs and plaintext credential files. See the [Korean gateway guide](docs/gateway.md) and nonsecret configuration templates under `example/gateway-*`.

Guest workloads use the mandatory **society-v1** filesystem profile. Writable work, home and temporary files stay in `/data/Society.societycontainer/Files`; a private read-only runtime root, an unprivileged identity, cleared capabilities and a syscall filter prevent changes to the guest OS or direct access to its raw disk. Missing confinement prerequisites block application execution. Existing stopped VMs can receive the new initramfs through `tools/upgrade_guest.py` with a backup and unchanged disk/UUID. See the [filesystem contract](docs/filesystem.md).

`Gateway::files` and `Runtime::files` provide per-VM directory listing, metadata, binary reads, exclusive creation, chunked writes, directory creation, no-replace moves and nonrecursive deletion. Each operation is authorized separately and executes in a confined guest worker with only the `Files` directory mounted as writable data. File contents use bounded binary chunks; SSH and guest frame bindings retain the runtime/VM/request identity. See the [Files API guide](docs/files-api.md) and `example/gateway-file-*.json`.

The initial host adapter supports macOS/HVF and Linux/KVM, plus explicit QEMU/TCG emulation. Hardware acceleration requires matching host and guest architectures. The supplied image recipe builds an aarch64 guest named **iiOS**. The API also describes x86_64 guests when a compatible kernel and initramfs are supplied.

Disk boot also supports ARM64 UEFI code with instance-owned variable storage,
USB installation media, NVMe storage and a RAM framebuffer. Debian cloud-image
preparation and boot probes select the matching ARM64 or x86-64 serial console.
An optional per-instance `--cpu-model` selects a QEMU CPU model and properties;
instances without it keep their existing architecture and accelerator defaults.
See [the platform guest guide](docs/platform-guests.md) for setup, ownership,
execution evidence and the limits of each guest profile.

Run tests that boot a VM only inside a dedicated, resource-limited Linux
container. Use QEMU TCG inside that container and expose only the test's own
inputs and outputs. The container runtime itself still relies on the host's
virtualization and storage; it does not eliminate host-level failure risk.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSDKCore;$HOME/.local/SDK/iiJson;$HOME/.local/SDK/iiFileProvider;$HOME/.local/SDK/iiNetwork;$HOME/.local/SDK/iiAccountManager"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
```

Build the optional external QEMU engine locally with `python3 tools/bootstrap_qemu.py`. Dependency builds and validation outputs belong under `build/`; reusable guest images, firmware and VM state belong under `resources/`. An existing QEMU can be selected with `--qemu-aarch64` or `--qemu-x86_64`.

Guest construction and the workspace contract tests reuse the native `DirectoryStorage` implementation from the sibling iiSocietyContainer source tree; `IISOCIETYCONTAINER_SOURCE` can select another checkout. Cross-compiling the guest requires both C and C++ compilers targeting the same Linux sysroot.

The installed CMake package exports `iiVirtualizer::iiVirtualizer`. Run `./install-native.sh` to stage it under `build/install`. iiSDKCore, iiJson and iiFileProviderNative remain explicit package dependencies; QEMU is selected at runtime. `tests/consumer` verifies consumption through the installed package.

Gateway builds additionally use iiNetwork, iiAccountManagerNative and OpenSSL 3 or later. Set `OPENSSL_ROOT_DIR` for a keg-only installation. `-DIIVIRTUALIZER_BUILD_GATEWAY=OFF` excludes the gateway API and executable.

The **VmClient** binds an application runtime to one remote VM with typed process, file, monitoring and lifecycle calls. Managed processes survive SSH disconnects and expose binary stdin/stdout/stderr, cursor-based bounded output, retained exit status, idempotent launch keys and explicit release. Linux cgroups contain descendants and enforce memory/task budgets; application execution retains the mandatory Files-only sandbox. Lightweight VM metrics and process metadata are separate from log payloads. See the [Korean process API guide](docs/processes-api.md) and `example/gateway-process-*.json`.

Guest-native builds can boot Debian and Windows Server Core, compile iiSDKCore plus a C++23 application inside the guest, execute the resulting application, and stream native ELF/PE artifacts back to the host. Foreign CPU architectures select QEMU TCG automatically; matching architectures retain native acceleration. Disk boot profiles support owned installer/payload media, SATA, standard VGA and installation reboots. See the [Korean guest build guide](docs/guest-build.md), `tools/prepare_build_payload.py`, and `tools/guest_build.py`.

Earlier host-based verification on an Apple Silicon Mac covered Debian 13 ARM64/HVF and Windows Server 2025 Standard Evaluation Core x86_64/TCG. Both guests rebuilt iiSDKCore 0.20, compiled the same Runtime/Concurrent application, passed its CTest, executed it with result 5050, and returned native artifacts. The aggregate evidence and ready-to-copy artifact bundles are under `build/build-layer/verification.json` and `build/build-layer/artifacts/`. Those historical runs preceded the container-only testing policy and do not establish container-based verification.

The native build worker is enabled by default and requires iiSDKCore 0.20 and the sibling iiJson source. Set `-DIIVIRTUALIZER_BUILD_BUILDER_AGENT=OFF` to retain the host runtime's iiSDKCore 0.18 minimum without building or installing this worker.

Foreign guests can select a complete kernel command line with `--kernel-args` and an optional headless virtio GPU with `--graphics virtio-gpu`. The default iiOS boot recipe and existing instance manifests remain compatible. See the [Debian and Android boot guide](docs/guest-os-boot.md) for image preparation, actual boot evidence, and guest-agent limits.

See [the Korean architecture guide](docs/architecture.md) and [the operating guide](docs/usage.md) for guest construction and lifecycle commands. Each runtime owns one host's VM execution; Gateway objects connect remote runtimes through account-authorized SSH requests. Distributed scheduling, billing and VPC control belong in an upper layer.

Earlier host-run ARM64 HVF and TCG boot verification covered iiSDK PID 1, persistent ext4 storage, DHCP/NAT and HTTP forwarding, pause/resume, reboot, and graceful/forced shutdown. The multi-VM test kept one C++ controller and one Runtime alive while two guests served HTTP, verified independent resource allocations and lifecycle operations, and isolated a third guest's deliberate port-conflict failure. See [validation evidence and platform limits](docs/validation.md).

Host and guest file creation normally uses iiFileProvider Native, including lock files, boot counters, instance manifests and raw volume staging. An explicit macOS base-disk clone can use the filesystem's copy-on-write operation before falling back to the provider. The Linux guest links the provider statically; cross toolchains must configure both C and C++ compilers. See [file provider contract](docs/FileProvider.md).

<!-- workspace-docs:readme:start -->
## Documentation entry points

- Human overview: this English `README.md`.
- Agent instructions: [AGENT.md](AGENT.md).
- Human documentation: [documentation index](docs/README.md).
- Structured agent documentation: [agent index](docs/agent-index.json).
<!-- workspace-docs:readme:end -->
