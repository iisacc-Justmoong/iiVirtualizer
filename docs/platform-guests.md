# Windows 및 Linux 아키텍처별 게스트

iiVirtualizer의 한 호스트는 ARM64와 x86-64 게스트를 각각 보관하고 제어할 수 있다. Apple Silicon에서 ARM64는 HVF, x86-64는 QEMU TCG를 사용한다. `auto`는 호스트와 게스트 CPU가 같을 때 호스트의 가속기를, 다를 때 TCG를 선택한다. TCG는 CPU를 에뮬레이션하므로 하드웨어 가속 결과와 성능이 다르다.

실제 VM 부팅 시험은 [전용 Linux 컨테이너](container-testing.md) 안에서만 수행한다. 컨테이너에 전체 호스트 작업공간, 기존 VM, 가속 장치나 외부 네트워크를 연결하지 않는다. 전용 입력과 실험용 출력만 컨테이너의 별도 볼륨에 복사하고 QEMU TCG를 명시한다. Apple Silicon에서 컨테이너 자체는 macOS의 가상화와 저장소에 의존하므로 호스트 장애가 불가능하다는 뜻은 아니다. 컨테이너 구성이 확인되기 전에는 VM을 시작하지 않는다.

## 입력과 인스턴스의 위치

재사용할 OS 이미지와 펌웨어, 실제 게스트의 디스크와 소유 미디어는 `resources/platform-guests/`에 둔다. 빌드 결과와 검증 로그는 `build/`에 둔다. `resources/platform-guests/`는 버전 관리에서 제외한다. ISO나 펌웨어를 다른 인스턴스와 직접 쓰기 공유하지 않으며 인스턴스 생성 시 소유 복제본을 만든다. 기존 VM, 활성 프로세스, 원본 설치 미디어는 변경하거나 삭제하지 않는다.

큰 `--base-disk`에 `--clone-base`를 선택하면 macOS에서 파일시스템의 copy-on-write 복제를 먼저 시도하며, 지원하지 않으면 일반 복사로 돌아간다. 이후 게스트의 쓰기는 원본에 반영되지 않는다. 복제할 때 원본의 ctime이 갱신될 수 있으므로 메타데이터까지 그대로 보존해야 하는 원본에는 이 옵션을 사용하지 않는다.

ARM UEFI의 코드와 변수 저장소는 서로 다른 입력이다. `--firmware`와 `--firmware-vars`를 함께 지정하면 인스턴스에 복제한 코드 장치를 읽기 전용으로, 변수 장치를 쓰기 가능하게 연결한다. 변수 파일은 VM마다 독립적이며 부팅 간에 보존된다. 변수만 지정하거나 커널 직접 부팅에 펌웨어를 지정하는 것은 오류이다. 이전 manifest의 필드가 없어도 기존 동작을 유지한다.

ARM 디스크 부팅은 설치 ISO와 선택적 페이로드를 USB 저장 장치로 연결한다. `--disk-bus nvme`는 NVMe 디스크를, `--graphics ramfb`는 펌웨어 프레임버퍼와 USB 입력 장치를 선택한다. 기본 ARM 부팅 순서는 설치 미디어 다음 OS 디스크다. `--boot-disk-first` 또는 `InstanceSpec::boot_disk_first`를 지정하면 ARM과 x86에서 디스크를 우선하고 설치 미디어를 대체 부팅 대상으로 둔다. 이는 설치 중 재시작할 때 완성된 디스크로 이어가려는 새 인스턴스에 유용하지만, 이미 부팅 가능한 디스크가 있으면 설치 미디어가 자동 실행되지 않는다. 실제 대체 부팅은 사용한 펌웨어의 장치 지원에 좌우되므로 직접 확인해야 한다. [QEMU의 부팅 순서 설명](https://www.qemu.org/docs/master/system/bootindex.html)은 이러한 펌웨어별 제한을 명시한다. 이 옵션은 디스크 부팅과 설치 ISO를 함께 지정해야 하며 기존 manifest에서는 기본적으로 꺼져 있다. Windows ARM 실행 시험은 디스크 우선순위와 미디어의 대체 순위를 `plan`으로 확인한다. `--guest-channel serial`은 게스트가 실제로 해당 직렬 포트를 지원하고 작업자를 실행할 때에만 유효하다. 미디어를 연결하는 것만으로 운영체제 설치, GUI 사용, 게스트 작업자, Files API 또는 프로세스 API가 제공되지는 않는다.

인스턴스 생성 시 `--cpu-model` 또는 `InstanceSpec::cpu_model`로 QEMU CPU 모델과 속성을 지정할 수 있다. 예를 들어 Windows ARM TCG 시험은 `max,pauth-impdef=on`을 사용한다. 값은 인스턴스 manifest에 보존되고 원격 Gateway의 인스턴스 응답에도 포함되며 `plan`에서 실제 QEMU 인자를 확인할 수 있다. CPU 모델이 없는 기존 manifest와 신규 인스턴스는 이전의 아키텍처·가속기별 기본값을 유지한다. 모델과 속성의 지원 여부는 선택한 QEMU 버전과 가속기에 따라 다르며, 지정 자체가 게스트 부팅의 증거는 아니다. Windows ARM의 QEMU 예시는 [virtio-win 안내](https://github.com/virtio-win/virtio-win.github.io/blob/main/Knowledge-Base/Windows-arm64-vm-using-qemu.md)를 참조한다.

## Debian 이미지

`tools/os_sources.json`에 고정한 Debian 13 genericcloud ARM64와 AMD64 아카이브를 공급자 SHA-512와 고정 SHA-256으로 검증한다. 준비 도구는 직접 부팅용 커널, initramfs와 분리된 루트 볼륨을 만든다. AMD64는 `ttyS0`, ARM64는 `ttyAMA0`을 사용한다. 다음은 이미지 준비 명령이다. VM 부팅은 별도로 격리된 Linux 컨테이너 안에서 수행한다.

```sh
python3 -B tools/prepare_os_guests.py debian --arch x86_64 \
  --output resources/platform-guests/linux-x86-prepared
```

검증은 게스트가 출력한 Debian 버전, 실제 `uname -m`, systemd PID 1, multi-user target과 부팅 ID를 함께 확인한다. QEMU 프로세스 시작만으로 통과시키지 않는다. 다른 아키텍처를 검사하려면 이미지와 예상 아키텍처도 바꾸어야 한다. `tools/prepare_linux_build_guest.py`와 `tools/guest_build.py`는 별도로 게스트 안에서 iiSDK 앱을 빌드·실행하고 산출물의 ELF 아키텍처를 확인한다.

## Existing Windows x86 test disk

`tests/windows_x86_boot.py` can test a stopped, task-owned Windows x86 instance after a sparse disk copy has completed. The sender and receiver receipts must agree on the transferred data hash and size before the VM is started. The harness requires the isolated Linux container, TCG, no guest network and at least 512 MiB of container memory beyond the VM allocation. It waits for the installed Windows worker to identify the OS and x86-64 architecture, then confirms that its first native compiler command exits successfully. It checks pause and resume, requests an ACPI shutdown and uses a forced stop only if needed. Successful evidence is saved before the temporary VM is removed.

This startup test verifies the installed Windows worker and one native command. It does not claim that the whole SDK was rebuilt or that the example application was executed. A Windows x86 container result does not exist until the test has actually run and produced its result file.

## Windows ARM64 installation test inputs

`tools/prepare_windows_arm_probe.py` prepares a private payload directory beneath `resources/` from an ARM64 PE probe and an explicitly selected image index. It creates a fresh nonce, a task-only administrator password, a local `IIVProbe` administrator with one automatic sign-in, a bootstrap script, and an answer file with EFI, MSR, and Windows partitions. Creating a local account follows Microsoft's [unattended OOBE guidance](https://learn.microsoft.com/en-us/windows-hardware/customize/desktop/automate-oobe?view=windows-11). The answer file erases guest disk 0: use it only with a newly created disposable disk inside the isolated test container. It does not create an ISO or bypass Windows license, activation, TPM, or firmware requirements. Determine the image index from the verified installation media. The generated password is not printed and the answer file is private to the task.

```sh
python3 -B tools/prepare_windows_arm_probe.py --binary "$IIV_ARM64_PROBE" \
  --output "$IIV_PRIVATE_PAYLOAD" --image-index "$IIV_IMAGE_INDEX"
```

The answer file and bootstrap have structural tests, but the Windows ARM installer, login, serial report, and native execution have not yet been validated. A prepared payload is not an installation or boot result.

`tests/windows_arm_boot.py` is the corresponding isolated execution harness. It refuses host execution, networked containers, and hardware acceleration. Before creating a fresh ARM VM it checks the official installer SHA-256, the payload ISO contents against the nonce and ARM PE manifest, available private disk space, and memory headroom. It starts a dedicated software TPM, boots with TCG and no guest network, accepts only a complete native serial report, and checks pause, resume, and stop. It verifies the created VM's UUID on startup and before and after stopping; if the identity changes, it refuses to stop that instance. It removes its own stopped VM after success and retains evidence or failed state for diagnosis. Supply the QEMU, TPM, ISO inspector and private firmware paths explicitly; guest inputs and instance state must be below `resources/`, with evidence under `build/`.

```sh
python3 -B tests/windows_arm_boot.py --runtime "$IIV_RUNTIME" --qemu "$IIV_ARM_QEMU" \
  --installer "$IIV_WINDOWS_ARM_ISO" --installer-sha256 "$IIV_WINDOWS_ARM_SHA256" \
  --payload-iso "$IIV_PAYLOAD_ISO" --payload-manifest "$IIV_PAYLOAD_MANIFEST" \
  --binary "$IIV_ARM64_PROBE" --firmware "$IIV_ARM_FIRMWARE" --firmware-vars "$IIV_ARM_VARS" \
  --swtpm "$IIV_SWTPM" --isoinfo "$IIV_ISOINFO" --isoinfo-lib-dir "$IIV_ISO_LIB" \
  --root "$IIV_VM_ROOT" --output "$IIV_EVIDENCE" --id "$IIV_VM_ID"
```

The default VM has 4 GiB RAM, a 64 GiB logical disk, and the `max,pauth-impdef=on` CPU model; `--cpu-model` can select another model supported by the chosen QEMU. The container must have at least 512 MiB more memory than the guest, and the private host disk must have enough free space for the entire guest disk, an additional installer copy, the payload and a 2 GiB reserve. An optional `--installer-verification-checkpoint` reuses an actual completed or interrupted SHA-256 calculation only for the same unchanged file and native crypto provider; its private state is checked again after the installer is copied. Without it, the installer is hashed directly each time. The smaller 2 GiB test container and 20 GiB host described in the container guide therefore cannot run this installation profile.

For a lower-memory preparation phase, append `--prepare-only` to the same command. It verifies and copies the inputs into a fresh instance, synchronizes the owned files, and writes a private marker under that instance without starting QEMU or the TPM. It can therefore run with less memory than the eventual guest, provided the disk reserve is available. To boot later, run the command with the same ID, root, input media, CPU and resource settings, a fresh evidence output, and `--resume-prepared`. The second invocation requires the full guest memory headroom and checks the saved UUID, stopped state, file identity, sizes, input hashes and QEMU plan before launching. A changed or missing prepared instance is rejected; a preparation result is not evidence that Windows has booted.

## Windows native serial proof

`example/guest-container/windows-native-probe.cpp` is a Windows x86-64 or ARM64 native test program. After installation it reports the Windows version, system drive, OS and binary architectures, and a 32-digit test nonce on COM1. Its default mode refuses the Windows PE `X:` drive. The optional `--winpe` mode accepts only that drive so the installer environment can be recorded separately before the installed system starts; the harness keeps this in `winpe.json` and never treats it as installed-Windows success. The answer file requests the PE probe during the Windows Setup `windowsPE` pass without blocking installation if the diagnostic fails. A compiled program alone does not prove the guest has booted. [Microsoft documents](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getnativesysteminfo) that architecture queries can reflect an emulated process, so the PE machine code must match the target before the report is accepted.

`tools/windows_native_proof.py` checks complete start and end markers, the exact nonce, OS and binary architectures, installed Windows drive, and the executable PE machine. `tests/windows_native_proof.py` rejects mismatched architectures and nonces, invalid PE files, WinPE drive reports, duplicate fields, and incomplete markers. Collect the console and executable from the same isolated guest test; compilation or synthetic logs do not prove Windows execution.

```sh
python3 -B tools/windows_native_proof.py --console "$IIV_CONSOLE" \
  --binary "$IIV_WINDOWS_PROBE" --architecture aarch64 --nonce "$IIV_NONCE"
```

## Windows ARM 디스크 프로필

Microsoft의 [Windows 11 ARM64 ISO](https://www.microsoft.com/en-us/software-download/windows11arm64)와 QEMU에 맞는 ARM64 UEFI 코드 및 변수 이미지를 준비한다. Windows 설치 및 활성화에는 적절한 라이선스가 필요하다. 기본 QEMU 엔진에는 가상 TPM 백엔드가 없다. 별도로 준비한 TPM 지원 엔진을 호스트 HVF에서 시험했으나 `HV_BAD_ARGUMENT`가 발생했다. Secure Boot와 Windows 실행은 검증하지 않았다. 앞으로는 컨테이너 안의 QEMU TCG와 별도 TPM 에뮬레이터를 사용해 검증해야 한다.

별도 QEMU 엔진을 새로 구성할 때 `tools/bootstrap_qemu.py --with-tpm --with-display --targets aarch64 --output build/platform-arm-qemu`로 TPM과 화면 진단 빌드를 요청할 수 있다. 인스턴스를 만들 때 `--external-tpm`을 지정하면 QEMU는 그 인스턴스 디렉터리의 `tpm.sock`에 연결한다. TPM 에뮬레이터와 상태 디렉터리는 호출자가 소유하고, 시작 전에 같은 인스턴스에만 연결되는 소켓을 준비해야 한다. 에뮬레이터를 실행하거나 정지하는 기능은 이 옵션에 포함되지 않는다.

예를 들어 사용자가 소유한 `swtpm`을 별도 프로세스로 실행할 때에는 인스턴스별 상태를 해당 VM 디렉터리에 보관할 수 있다. `VM_DIR`은 `create` 결과의 `directory`에 해당하는 절대 경로이다.

```sh
mkdir -p "$VM_DIR/tpm-state"
swtpm socket --tpm2 --tpmstate "dir=$VM_DIR/tpm-state" \
  --ctrl "type=unixio,path=$VM_DIR/tpm.sock" --terminate
```

QEMU가 제어 연결을 닫으면 이 예제의 에뮬레이터는 종료된다. QEMU가 소켓에 연결하기 전에 시작이 실패한 경우에는 에뮬레이터를 실행한 호출자가 그 프로세스를 정리해야 한다. 다른 인스턴스에 TPM 상태나 소켓을 공유하지 않는다. 현재 소스의 이 연결 계약을 구현했지만 TPM을 이용한 실제 Windows 실행은 아직 검증하지 않았다.

컨테이너 내부에서 실행과 종료는 같은 `--root`와 컨테이너 내부의 QEMU 경로를 지정한 `start`, `describe`, `stop`으로 제어한다. UEFI, 설치 화면, 실제 Windows 사용자 공간 및 네이티브 ARM64 프로그램 실행은 서로 다른 검증 단계이며 각각 직접 확인해야 한다. 설치 게스트가 iiVirtualizer 전용 작업자나 iiOS 프로토콜을 포함한다고 가정하지 않는다.

2026-10-28의 컨테이너 정책 이전 호스트 실행에서 Debian 13 ARM64/HVF와 x86-64/TCG는 게스트의 실제 아키텍처와 systemd 사용자 공간을 확인했다. ARM64 시험은 일시 정지·재개·종료도 통과했다. x86-64 시험은 종료 명령이 시간 초과로 실패했으나 이후 인스턴스가 정지된 것을 별도로 확인했다. ARM64 UEFI/NVMe/RAMFB 시험은 펌웨어 콘솔과 QMP 실행 상태까지만 확인했으며 TPM이 없는 상태였다. Windows ARM 설치·사용자 공간·프로그램 실행 결과는 없다. 이전 Windows x86-64 게스트 빌드 결과는 [게스트 빌드 기록](guest-build.md)에 별도로 보관되어 있다. 이 기록은 컨테이너 내부 재검증 결과가 아니다.

QEMU 실행 파일 경로는 절대 경로로 지정한다. 기본 QEMU 실행 준비 제한 시간은 30초이며, 저장소가 바쁜 환경에서는 `--startup-timeout 180`처럼 초 단위로 조정할 수 있다. QMP 제어 응답의 기본 시간은 5초이며 `--control-timeout 60`처럼 별도로 조정한다. 시작 대기는 게스트의 OS 부팅 완료를 뜻하지 않는다. 제어 명령이 시간 초과된 뒤에도 작업이 완료될 수 있으므로 `describe`로 상태를 재확인한다.

## 게스트 작업자와 JSON 의존성

호스트 빌드 작업자는 이미 제공된 `iiJson::iiJson` 타깃에 링크한다. iiJson이 아직 설치되지 않은 독립 게스트 빌드에서는 지정한 소스의 CMake 타깃을 사용한다. 따라서 iiJson의 번역 단위 구성은 해당 모듈이 관리하며 작업자가 외부 구현 파일을 직접 열거하지 않는다.
