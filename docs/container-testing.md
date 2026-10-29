# 격리된 VM 시험 컨테이너

VM을 실제로 부팅하는 모든 검증은 자원 제한이 있는 전용 Linux 컨테이너 안에서만 수행한다. 이 문서는 컨테이너 구성과 검증 절차이며, 컨테이너 생성이나 게스트 실행의 성공 기록이 아니다. Apple Silicon의 Linux 컨테이너 자체는 macOS Virtualization.framework를 이용한다. 컨테이너 격리는 테스트 프로세스와 파일 접근을 분리하지만 호스트 커널, 하이퍼바이저 또는 같은 물리 저장장치의 장애를 완전히 막지는 못한다.

`example/guest-container/Containerfile`은 공식 Debian 13 이미지의 OCI 인덱스 해시를 고정하고 ARM 및 x86 QEMU, TPM 에뮬레이터와 빌드 도구를 설치한다. 패키지 저장소의 개별 패키지 버전은 고정하지 않았다. 이미지를 빌드할 때만 네트워크가 필요하다. 실행할 컨테이너에는 네트워크를 연결하지 않는다.

`tools/guest_test_container.py`는 무작위 이름의 전용 컨테이너와 볼륨을 만든다. 기본값은 CPU 2개와 메모리 2 GiB이다. Apple 런타임의 전용 볼륨은 기본 8 GiB로 제한한다. `--backend nerdctl`을 선택하면 볼륨별 크기 제한을 적용하지 않으며 별도로 제한한 Lima 호스트 디스크가 전체 저장 공간의 상한이다. nerdctl 프로필은 메모리와 동일한 메모리·스왑 상한 및 프로세스 수 256개도 지정한다. 두 프로필 모두 읽기 전용 루트 파일시스템, `/tmp`의 임시 메모리, 쓰기 가능한 전용 `/work` 볼륨, 모든 Linux capability 제거와 네트워크 차단을 지정한다. 호스트 디렉터리, 포트, SSH 에이전트, `/dev/kvm` 또는 중첩 가상화는 연결하지 않는다. 볼륨은 생성한 런타임의 데이터 디렉터리 안에 보관한다. 생성 명령은 자원 이름을 작업 전에 출력하고 실패한 볼륨을 자동 삭제하지 않는다.

nerdctl 프로필은 `--init`을 사용해 종료된 게스트 프로세스를 회수한다. 이 옵션은 전용 Linux 컨테이너 호스트의 `tini` 실행 파일 하나를 컨테이너의 init으로 제공하며, 사용자 디렉터리는 연결하지 않는다. `tini`가 준비되지 않은 호스트에서는 컨테이너를 시작하지 않는다. 기존 컨테이너는 이 변경으로 소급 수정되지 않으므로 새 컨테이너를 만들어 확인한다.

선택적으로 `--io-device /dev/vda --io-mib 4`를 지정하면 전용 Linux 호스트의 해당 블록 장치에 대한 컨테이너 읽기와 쓰기를 각각 초당 4 MiB로 제한한다. 장치 이름은 호스트마다 다르므로 전용 볼륨이 실제로 사용하는 장치를 먼저 확인한다. 이는 I/O 제어 설정이며 해당 장치를 컨테이너에 연결하지 않는다. 실행 후 실제 cgroup의 `io.max`를 검사한다. 커널 I/O 제한은 일시적인 버스트를 허용하므로 물리 호스트의 전체 I/O가 해당 수치를 절대 넘지 않는다는 보장은 아니다.

전용 시험 호스트에서 이 제한값을 실제 cgroup에서 확인하고 컨테이너에 `/dev/vda`가 나타나지 않는 것을 확인했다. 별도 시험 컨테이너는 종료·삭제했고, 관찰 결과는 `build/container-io-smoke.log`에 있다.

2026-10-28에는 별도의 읽기 전용·네트워크 차단 컨테이너에서 `tests/reaper_container.py`로 고아 프로세스가 실제 회수되는 것을 확인했다. 결과와 컨테이너 설정은 각각 `build/container-init-smoke.log`, `build/container-init-smoke-inspect.json`에 보관했으며 해당 컨테이너는 정리했다.

## 준비와 시작

Apple `container` 런타임을 별도 설치 경로에 준비하고 `system start`의 `--app-root`, `--install-root`, `--log-root`가 모두 작업공간을 가리키도록 한다. 다른 작업에서 사용 중인 같은 사용자 서비스가 있다면 중지하거나 설정을 덮어쓰지 않는다. 서비스가 준비되기 전에는 컨테이너 생성이나 VM 시험을 시작하지 않는다.

다음 명령은 런타임 서비스가 이미 준비된 경우의 절차이다. `IIV_CONTAINER_CLI`는 해당 런타임 CLI의 절대 경로이다. 빌더에도 CPU와 메모리 제한을 지정한다.

```sh
"$IIV_CONTAINER_CLI" build --cpus 2 --memory 2G \
  -t iivirtualizer-vm-test:debian13 \
  -f example/guest-container/Containerfile example/guest-container
python3 -B tools/guest_test_container.py --container "$IIV_CONTAINER_CLI" --plan
python3 -B tools/guest_test_container.py --container "$IIV_CONTAINER_CLI"
```

마지막 명령은 중지된 컨테이너를 만든다. 출력한 `container` 식별자를 확인하고 그 컨테이너만 시작한다. 첫 실행에서는 VM을 부팅하지 않고 컨테이너 안에 Linux가 실행되는지, `/dev/kvm`이 없는지, 네트워크 인터페이스가 루프백만 있는지 확인한다. 컨테이너의 검사 결과와 실제 게스트 부팅 결과는 별개의 증거이다.

```sh
"$IIV_CONTAINER_CLI" start "$IIV_TEST_ID"
"$IIV_CONTAINER_CLI" exec "$IIV_TEST_ID" /bin/sh -c \
  'test "$(uname -s)" = Linux && test ! -e /dev/kvm && test "$(ls /sys/class/net)" = lo && echo IIV_CONTAINER_ISOLATED'
```

검사에 실패하면 VM을 시작하지 않는다. 필요한 입력과 소스는 전용 `/work` 볼륨으로 명시적으로 복사하고 결과만 다시 회수한다. QEMU 실행과 iiVirtualizer의 `--accel`은 `tcg`로 지정한다. 컨테이너 식별자와 볼륨 이름을 보관하고 사용이 끝나면 그 컨테이너만 정지한다. 다른 컨테이너나 볼륨을 일괄 삭제하지 않는다.

대용량 설치 이미지나 희소 디스크를 호스트 마운트 없이 전용 볼륨으로 복사할 때는 `tools/sparse_transfer.py`의 `send`와 `receive`를 파이프로 연결할 수 있다. `send`는 프로젝트의 `build/` 또는 `resources/` 아래에 있는 입력만 읽고 초당 읽기량을 제한하며 전송 중 원본의 식별자·크기·수정 시간이 바뀌었는지 확인한다. `receive`는 네트워크가 없고 `/dev/kvm`이 없는 시험 컨테이너의 `resources/platform-guests/` 아래에 새 파일을 배타적으로 만든다. 기존 파일이나 심볼릭 링크는 덮어쓰지 않는다. 전송이 중단되어 남은 부분 파일은 사용하지 않고 해당 시험의 파일인지 확인한 뒤 정리한다. 양쪽 전송 해시와 길이를 비교하고 수신자의 동기화 완료를 확인한 후 사용한다. 설치 이미지의 공식 전체 파일 해시도 별도로 검사한다.

파일시스템이 파일 길이보다 큰 할당 크기를 보고할 수 있으므로 전송 헤더의 할당량은 논리 길이 이하로 제한한다. 수신 측의 여유 공간 검사에는 별도로 2 GiB를 남긴다. 이 크기 처리는 희소 파일과 작은 일반 파일로 시험한다.

macOS 송신자는 원본 파일 디스크립터에 캐시를 사용하지 않는 읽기를 요청하고, Linux 수신자는 파일 동기화 이후 깨끗한 파일 캐시 페이지의 해제를 요청한다. 각 요청의 결과는 송신·수신 기록에 남긴다. 파일시스템이나 커널이 요청을 지원하지 않을 수 있고 수신 측 요청은 힌트이므로 실제 메모리 사용량이 제한된다는 뜻은 아니다. 호스트 감시와 전송률 제한은 그대로 필요하다.

중단에 대비해 `tools/chunk_transfer.py transfer`는 입력을 기본 64 MiB 조각으로 나눈다. 각 조각의 송신·수신 해시와 길이가 일치하고 수신 파일이 동기화된 다음에만 `resources/platform-guests/` 아래의 체크포인트를 원자적으로 갱신한다. 재실행할 때는 같은 원본의 장치·파일 식별자·크기·수정 시간, 컨테이너, 조각 크기와 완료 기록을 확인하고 남은 조각부터 전송한다. 실행 중 중복 프로세스는 체크포인트 잠금으로 막는다. 실패한 조각은 새 이름으로 다시 시도하며 기존 파일은 덮어쓰지 않는다. 호스트 감시는 별도로 유지한다.

`--max-chunks`를 지정하면 정해진 개수의 새 조각을 완료한 뒤 중지해 이어받기를 시험할 수 있다. 모든 조각을 받기 전에는 `transfer-paused`를, 모두 받으면 `transfer-complete`를 출력한다.

전용 컨테이너에서 작은 파일의 첫 조각을 저장하고 다음 조각 전송 중 실제 중단 신호를 보낸 뒤, 수신 프로세스가 종료되고 완료 기록이 유지되는 것을 확인했다. 이어서 남은 조각을 전송하고 조합된 파일을 다시 읽어 SHA-256을 검증했으며 시험 파일은 정리했다. 결과는 `build/container-chunk-signal-fixed-smoke.log`에 있다.

모든 조각을 받은 후에는 같은 도구의 `assemble`을 시험 컨테이너 안에서 실행하고 체크포인트를 표준 입력으로 전달한다. 조각 순서와 크기, 전용 디렉터리의 소유 표식을 확인하고 전체 파일 크기와 추가 2 GiB를 위한 공간이 있는지 확인한다. 조각을 읽은 뒤 해당 캐시의 해제를 요청하고, 출력은 약 256 MiB마다 동기화한 뒤 캐시 해제를 요청한다. 이 요청은 커널의 힌트이며 메모리 상한의 대체가 아니다. 새 출력 파일을 동기화한 다음 실제 파일을 다시 읽어 전체 SHA-256이 기대값과 일치할 때에만 성공을 반환한다. 조합 도중 실패한 출력은 완성본으로 사용하지 않는다. 원본이나 다른 작업의 파일을 자동 삭제하지 않는다.

`assemble --resume`은 새 출력과 짝을 이루는 소유 기록을 만든다. 중단 후 같은 출력 경로, 조각 목록과 기대 해시로 재실행하면 기존 출력의 파일 식별자와 소유 기록을 확인한다. 출력이 일부만 만들어졌다면 기록된 바이트를 원본 조각과 비교한 뒤 나머지를 쓴다. 이미 전체 길이인 소유 출력은 조각을 다시 읽지 않고 출력 파일 자체를 처음부터 읽어 전체 SHA-256을 검증한다. 어느 경우든 실제 출력의 전체 해시가 기대값과 일치해야 성공한다. 소유 기록이 없거나 다르거나 기존 내용이 일치하지 않으면 덮어쓰지 않고 중단한다. 중단 직후 출력과 소유 기록이 모두 만들어지지 못했다면 해당 출력은 자동 복구하지 않으므로 새 출력 경로를 사용한다. 이 기능은 재개 가능성을 높이지만 호스트 장애를 방지하지는 않는다.

전용 컨테이너에서 작은 조각들을 조합하다가 실제 종료 신호로 중단한 뒤, 기존 부분을 확인하고 다시 조합해 출력 파일의 전체 해시를 읽어서 검증했다. 이 시험에 사용한 파일만 정리했으며 결과는 `build/container-assembly-resume-signal-smoke.json`에 있다.

다음은 위에서 설명한 전용 Lima 호스트와 nerdctl 래퍼를 사용하는 예이다. 출력 경로는 컨테이너 내부 `resources/platform-guests/`의 새 파일이어야 한다.

```sh
python3 -B tools/chunk_transfer.py transfer --source "$IIV_INPUT" \
  --container "$IIV_TEST_ID" --checkpoint "$IIV_CHECKPOINT" --chunk-mib 64 --rate-mib 2
cat "$IIV_CHECKPOINT" | "$IIV_NERDCTL_WRAPPER" exec -i "$IIV_TEST_ID" \
  python3 -B /work/iiVirtualizer/tools/chunk_transfer.py assemble \
  --target "$IIV_OUTPUT" --sha256 "$IIV_EXPECTED_SHA256" --resume
```

실제 부팅 시험 도구는 실행 전에 Linux, `IIV_TEST_CONTAINER=1`, `/dev/kvm` 부재와 `tcg` 선택을 확인한다. 호스트에서 실수로 실행되는 것을 막는 확인이며, 환경변수만으로 실제 컨테이너 격리를 증명하지는 않는다. 컨테이너의 네트워크·마운트·가속 장치 상태는 별도로 검사해야 한다. C++ 다중 VM 시험 프로그램에도 같은 사전 조건을 적용한다.

Windows의 실제 설치에는 기본 시험보다 큰 메모리와 볼륨이 필요하므로 별도의 자원 제한을 검토해야 한다. 2 GiB/8 GiB 기본 프로필의 컨테이너가 Windows 설치를 실행할 수 있다는 뜻이 아니다.

Windows 시험은 실행 시간 제한을 두며, 시험 프로세스가 중단 신호를 받으면 게스트 종료와 TPM 정리 절차를 수행한다. Windows x86 시험의 정상 종료 대기 중 추가 중단 신호가 오면 기다림을 줄이고 소유한 게스트를 강제로 정지한다. 실제 강제 정지 및 ARM 정리 중에는 추가 신호가 정리를 중단하지 않도록 한다. 강제 종료나 호스트 장애에서는 이 처리를 보장할 수 없으므로 재접속 후에는 전용 컨테이너와 게스트의 실행 상태를 다시 확인한다.

Debian 등 외부 Linux 게스트 시험도 중단 신호를 처리하며, 정리할 때 인스턴스 UUID를 다시 확인하고 해당 시험의 게스트만 정지한다. 호스트 감시 도구의 `--kind linux`는 지정한 Linux 시험 스크립트와 인스턴스 ID가 모두 일치하는 프로세스에만 중단을 요청한다. 신호 처리 계약 시험은 VM을 실행하지 않는 모의 런타임에 실제 종료 신호를 보내고, 소유 인스턴스의 정지 기록을 확인한다. 이는 별도의 전용 호스트 감시와 함께 사용한다.

중단 신호는 일반적인 입출력 재시도 예외와 구별되는 `TestInterrupted`로 전달한다. Python의 입출력 대기 구현이 `InterruptedError`를 재시도할 수 있어, 이를 그대로 사용하면 전송 하위 프로세스 정리까지 도달하지 못할 수 있다. 실제 신호를 보내 대기 중인 전송과 그 하위 프로세스가 정리되는 회귀 시험을 포함한다.

Windows ARM 시험은 설치 이미지 검증과 게스트 생성보다 먼저 중단 신호 처리기를 설치한다. 생성 중 중단되면 실행되지 않은 임시 생성 디렉터리가 남을 수 있으므로 관련 프로세스가 종료된 것을 확인한 뒤 해당 시험의 파일만 정리한다. 준비 시작 시 실제 종료 신호가 처리되는지는 `iiVirtualizer.WindowsNativeProof`에서 확인한다.

macOS에서 Windows 시험을 실행할 때는 `tools/vm_test_watchdog.py monitor`를 함께 실행할 수 있다. 기본 설정은 호스트 여유 메모리가 세 번 연속 12% 미만이거나 메모리·스왑 상태를 세 번 연속 읽을 수 없을 때 종료를 요청한다. 약 10초인 한 관찰 간격에 호스트 누적 스왑 출력이 256 MiB 이상 증가한 경우에도 종료를 요청한다. 여유 메모리가 20% 미만일 때는 최근 최대 세 관찰 간격의 증가량을 합산해 같은 기준을 적용한다. `--max-swapout-mib`로 이 기준을 조정할 수 있다. 재부팅 등으로 누적 수치가 줄면 새 기준점을 잡는다. 종료 신호는 전용 컨테이너에서 지정한 시험 스크립트와 VM 식별자가 정확히 일치하는 단일 프로세스에만 보낸다. 감시 기한이 만료될 때에도 같은 종료를 요청한다. Linux pidfd와 재확인으로 PID 재사용을 방지하며, 동일한 대상이 둘 이상이면 신호를 보내지 않는다. 스크립트 자체가 VM을 찾아 무조건 종료하거나 다른 사용자 프로세스에 신호를 보내지는 않는다. `--stop-file`에 `resources/` 아래의 새 파일 경로를 지정했다면 시험 종료 후 그 파일을 만들어 감시 프로세스를 종료한다. 메모리 감시는 자원 제한과 별개이며 하드웨어나 호스트 자체의 장애를 막는 보장은 아니다.

급증 감지 뒤 일시적으로 SSH 요청이 실패하거나 시험 프로세스가 아직 나타나지 않았다면 종료 요청을 유지하고 다시 시도한다. 메모리 수치가 다음 관찰에서 회복되었다는 이유로 요청을 취소하지 않는다.

```sh
python3 -B tools/vm_test_watchdog.py monitor --kind x86 --vm-id "$IIV_VM_ID" \
  --container "$IIV_TEST_ID" --ssh-config "$IIV_LIMA_SSH_CONFIG" \
  --duration 10800 --stop-file "$IIV_MONITOR_STOP"
```

ARM 시험에서는 `--kind arm`을 사용한다. 감시 기한은 시험 제한시간과 준비·종료 시간을 모두 포함하도록 정한다.

실제 Linux 컨테이너에서 두 시험 프로세스를 띄워 지정한 하나만 종료하고 다른 프로세스는 계속 실행되는 것을 확인했다. `build/container-watchdog-tests.log`에 결과가 있다. 호스트에서 존재하지 않는 대상에 대한 감시 경로를 시험한 `build/container-watchdog-monitor-smoke.log`의 `NO_MATCH`와 종료 코드 3 메시지는 예상한 결과이며 어느 프로세스에도 신호를 보내지 않았다.

The isolated Debian container ran `swtpm` 0.7.1 and QEMU 10.0.13 with the ARM TCG CPU model `max,pauth-impdef=on`, a private TPM control socket and `tpm-tis-device`. QMP reported the configured machine paused, and both owned processes stopped. In this `swtpm` version `--terminate` is a top-level option, not a comma-separated `--ctrl` option. The `iiVirtualizer.TpmContainer` regression test runs this check only in the isolated Linux test container; its output is in `build/container-tpm-cpu-preflight.log`. This verifies configuration and process startup, not guest TPM commands or Windows boot.

인스턴스별 CPU 모델 선택을 반영한 소스를 같은 전용 컨테이너에서 다시 빌드하고 계약, CLI, Windows 실행 증거 파서 시험 3개를 통과했다. 기록은 `build/container-linux-cpu-build.log`와 `build/container-linux-cpu-tests.log`에 있다. 이 결과는 코드와 시험 장치의 확인이며 설치된 Windows ARM 사용자 공간 실행 결과와는 별개다.

## Lima 대체 경로

Apple 런타임의 기본 네트워크 서비스가 준비되지 않으면 별도의 Lima 호스트 안에서 system containerd와 nerdctl을 사용할 수 있다. 이 경로의 호스트 인스턴스는 `resources/ct/iivc`에 있고, 작업 디렉터리와 이미지는 Workspace 안에 있다. 호스트는 CPU 2개, 논리 디스크 100 GiB이며 사용자 홈이나 다른 호스트 디렉터리를 마운트하지 않는다. Windows ARM 설치에 필요한 호스트 메모리는 6 GiB였으며, 현재는 매체 검증을 안전하게 재개할 준비 단계에서 정지 상태로 2 GiB로 낮췄다. 게스트를 실행하기 전 필요한 메모리와 안정성을 다시 확인한다. SSH를 제외한 포트 전달과 중첩 가상화도 끈다. 호스트의 한도는 컨테이너의 CPU·메모리 한도와 별개이다.

이 호스트의 실행 도구에는 `LIMA_HOME`을 `resources/ct`의 절대 경로로, `TMPDIR`을 `resources/platform-guests/container-runtime/tmp`의 절대 경로로 지정한다. 전용 래퍼 `resources/platform-guests/container-runtime/lima-nerdctl`은 해당 인스턴스 내부의 `sudo nerdctl`을 호출하며 두 경로를 설정한다. 호스트와 이미지가 실제로 준비된 이후에만 다음 명령으로 컨테이너 계획을 확인하고 생성한다.

```sh
python3 -B tools/guest_test_container.py --backend nerdctl \
  --container "$IIV_NERDCTL_WRAPPER" --plan
python3 -B tools/guest_test_container.py --backend nerdctl \
  --container "$IIV_NERDCTL_WRAPPER"
```

### 전용 Lima 호스트의 부팅 및 포트 설정

Lima 2.2.1에서 백그라운드 시작 명령은 지정한 전체 준비 제한시간과 별도로 호스트 에이전트 PID를 짧은 시간에 기다릴 수 있다. 느린 저장소에서는 명령이 실패한 뒤에도 에이전트가 남을 수 있으므로 기존 인스턴스와 프로세스를 먼저 확인하고, 전용 인스턴스는 `limactl start --foreground --timeout 20m --tty=false iivc`로 감시한다. `LIMA_HOME`과 `TMPDIR`은 위에서 지정한 작업공간 내부 경로로 설정한다. 인스턴스가 실제로 준비되었는지는 반환 코드만이 아니라 Linux 부팅 로그, SSH, `containerd`, `buildkit`, `stargz-snapshotter`의 상태로 확인한다.

이 작업의 `resources/ct/iivc` 전용 호스트는 `tools/container_host_guard.py`로 시작과 호스트 메모리를 함께 감시할 수도 있다. 새 `build/` 로그 경로를 `--start-log`로 지정한다. 시작 전 여유 메모리가 25% 이상이어야 하며, 약 3초 간격의 관찰에서 여유 메모리가 한 번이라도 25% 미만이거나 최근 최대 20개 관찰 간격에 스왑 출력이 합계 256 MiB 이상 증가하면 해당 전용 호스트의 종료를 요청한다. 이전에 15% 기준으로 중단되었던 관찰을 바탕으로 더 일찍 종료를 요청하도록 조정했다. 측정값을 세 번 연속 읽지 못하거나 감시 기한이 지나도 같은 처리를 한다. 기한은 최대 12시간이다. 전용 Lima 인스턴스와 이 도구가 만든 시작 프로세스 그룹만 대상으로 하며, 중단 후에는 실제 호스트 상태를 별도로 확인한다. 자원 감시는 호스트 장애를 완전히 막지 못한다.

`--min-free-percent`로 시작 및 중단 기준을 25%보다 높게 정할 수 있다. 25%보다 낮게 설정할 수는 없다. 이 머신의 최근 메모리 급변에 대응한 다음 예시는 60%를 사용한다. 시작 전에도 60% 이상이어야 하며, 실행 중 60% 미만이 한 번 관찰되면 전용 호스트의 종료를 요청한다. 이미 실행 중인 감시에는 설정 변경이 소급 적용되지 않는다.

```sh
python3 -B tools/container_host_guard.py --start-log "$IIV_HOST_START_LOG" --duration 43200 --min-free-percent 60
```

VZ 호스트가 UEFI의 초기 램디스크 로딩 화면에서 진행하지 않을 때, 별도의 검증된 동일 호스트 디스크에서 추출한 커널과 initrd를 전용 인스턴스 디렉터리의 `kernel`, `initrd`로 연결하고 `kernel.cmdline`에 해당 디스크의 실제 루트 PARTUUID와 `console=hvc0`, `rootwait`를 지정하여 직접 부팅할 수 있다. PARTUUID는 인스턴스마다 확인해야 하며 다른 시스템의 값으로 고정하지 않는다. 이 방법으로 부팅되는 것은 우회 경로의 증거이며 UEFI가 멈춘 근본 원인의 증거는 아니다. `ssh.overVsock: false`와 `video.display: none` 설정에서도 직렬 콘솔과 로컬 관리 SSH를 확인한다.

Lima의 기본 포트 전달을 막는 규칙은 게스트 주소 전체를 포함해야 한다. 루프백 주소만 무시하면 게스트의 와일드카드 포트가 호스트에 전달될 수 있다. 이 전용 호스트의 규칙은 다음과 같다. 관리용 SSH와 DNS 소켓은 Lima 자체의 로컬 연결이며 게스트 서비스 포트를 게시하는 규칙과 구분하여 검사한다.

```yaml
portForwards:
  - guestIP: 0.0.0.0
    guestIPMustBeZero: false
    proto: any
    guestPortRange: [1, 65535]
    ignore: true
```

저장소 I/O 지연으로 서비스 시작의 기본 제한시간이 모자랐던 이 전용 호스트에는 `containerd.service`, `buildkit.service`, `stargz-snapshotter.service`의 systemd 시작 제한을 600초로 늘렸고, buildkit은 containerd 이후에 시작하도록 했다. `systemctl daemon-reload` 후 세 서비스가 실제 `active`인지 확인한다. 초기 cloud-init에 실패 기록이 남을 수 있으므로 서비스가 현재 작동한다는 사실과 초기화 전체가 오류 없이 끝났다는 주장을 구분한다. 이 설정은 전용 호스트에만 적용한다.

## 현재 상태

2026-10-28에 해시를 확인한 Apple `container` 1.6.1의 기본 네트워크 초기화가 `vmnet` 오류 1001로 실패했다. 별도의 Lima 2.2.1 호스트는 UEFI 초기 램디스크 화면에서 멈췄으나 직접 커널 부팅으로 Debian ARM64, 관리 SSH와 컨테이너 서비스를 실행했다. 외장 저장소의 I/O 대기가 관찰되었지만 UEFI 정지나 호스트 재시작의 근본 원인으로 단정할 수 없다. 전용 호스트는 CPU 2개, 메모리 6 GiB, 논리 디스크 100 GiB이고 호스트 디렉터리와 중첩 가상화를 연결하지 않는다. 디스크 확장 중 중단된 사본은 보존하고 확장 전 스냅샷에서 전용 인스턴스를 복구한 뒤 다시 확장했다. 재시작 후 루트 파일시스템과 컨테이너 서비스를 확인했으며 복구 내역은 `build/container-host-resize-recovery.json`에 있다. 확장 시 관찰된 메모리 압박의 원인은 확인되지 않았다.

실패 당시의 macOS 통합 로그에서는 `InternetSharing`이 네트워크 생성 요청의 XPC 클라이언트 속성을 가져오지 못해 네트워크 생성에 실패한 기록이 확인됐다. `vmnet`의 1001은 일반 실패 코드이며, 이 기록만으로 속성을 가져오지 못한 근본 원인이나 서명·권한 문제를 확정할 수 없다. 해당 시간대의 발췌는 `build/container-vmnet-unified-previous.log`와 `build/container-vmnet-networksharing-previous.log`에 있다. Apple의 [별도 외장 볼륨 XPC 오류 보고](https://github.com/apple/container/issues/1721)는 증상이 달라 이 머신의 원인으로 확정할 수 없다. Lima 전용 호스트는 이 실패 경로를 우회해 실제 컨테이너를 실행하지만 Apple 런타임 자체를 복구했다는 뜻은 아니다.

2026-10-29에 전용 호스트를 정상 종료하고 APFS 복제 사본을 만든 뒤 메모리를 6 GiB로 변경했다. 직접 커널 부팅 중 전용 디스크의 저널 복구가 끝났고 관리 SSH와 `containerd`, `buildkit`, `stargz-snapshotter`, `cloud-final`이 정상 상태가 됐다. 전용 볼륨을 유지해 다시 만든 시험 컨테이너의 실제 메모리 한도 5 GiB, 스왑 0, CPU 2개, 프로세스 256개, 디스크 읽기·쓰기 각 4 MiB/s, 네트워크 및 capability 차단, 읽기 전용 루트와 `tini`를 확인했다. 설정과 실행 검사는 `build/container-memory5-inspect.json`, `build/container-memory5-smoke.log`에 있다.

이 호스트에서 공식 Debian 이미지로 CPU 1개, 메모리 512 MiB, 프로세스 64개로 제한한 컨테이너를 실제 실행했다. 읽기 전용 루트, 모든 capability 제거, 루프백만 있는 네트워크, `/dev/kvm` 부재를 확인했고 종료 코드가 0이었다. 증거는 `build/container-smoke.log`와 `build/container-smoke-inspect.json`에 있다. 별도의 VM 시험 이미지를 빌드하고 CPU 2개, 메모리 2 GiB, 네트워크 차단, 읽기 전용 루트 및 전용 볼륨으로 시험 컨테이너를 생성했다. 검사 결과는 `build/container-test-inspect.json`과 `build/container-test-smoke.log`에 있다. 이 컨테이너 안에서 iiVirtualizer와 QEMU TCG로 Linux ARM64 및 x86-64 커널을 각각 부팅하고 initramfs 사용자 공간에서 커널 버전, 아키텍처, PID 1과 부팅 식별자를 확인했다. 두 게스트의 일시정지·재개·정지도 확인했으며 증거는 `build/container-initramfs-probes/arm-checked/`와 `build/container-initramfs-probes/x86-checked/`에 있다. 이는 최소 사용자 공간 실행 결과이며 완전한 Debian systemd 부팅의 증거는 아니다.

2026-10-29에는 제한된 컨테이너에서 복사한 Windows x86-64 게스트를 부팅했다. 설치된 Windows의 작업자가 OS와 아키텍처를 보고하고 컴파일러 명령을 시작했다. 이후 호스트 여유 메모리가 일시적으로 13%까지 낮아지고 호스트의 누적 스왑 출력이 급증해 시험 프로세스에 종료 신호를 보냈다. 게스트는 정상 종료 대기 후 강제로 정지되었으며 사용자 작업에는 신호를 보내지 않았다. `build/container-windows-x86-result/`에 시작 이벤트와 중지 상태를 보관했다. 컴파일러의 종료 코드는 받지 못해 이 시험의 전체 성공으로 기록하지 않는다. 관찰된 메모리 변화의 원인은 확인되지 않았다. Windows ARM64의 컨테이너 내부 실행은 아직 확인되지 않았다. 기존 호스트 직접 실행 기록은 [아키텍처별 게스트 안내](platform-guests.md)에 이력으로 구분한다.

이후 Windows ARM 설치 이미지를 복사하던 중에도 호스트 메모리 여유가 14%까지 낮아지고 스왑 출력이 증가했다. 전용 전송 프로세스만 중단했으며 수신자는 미완료 스트림을 거부했다. 이어서 전용 컨테이너와 호스트를 종료했고 ARM 게스트는 시작하지 않았다. 원본 설치 이미지와 수신된 부분 파일은 보존되어 있으며 관찰과 조치는 `build/container-arm-transfer-interruption.json`에 있다. 메모리 급변의 원인은 확인되지 않았다.

조각 단위 전송으로 재개한 뒤에도 호스트 메모리 여유가 13%까지 떨어져 전용 호스트 감시가 종료를 요청했다. 완료 확인을 받은 128개 중 15개 조각의 기록과 원본 파일 식별자는 유지되었으며, 진행 중이던 조각은 완료로 인정하지 않았다. 호스트 에이전트는 정지 대기 시간 초과를 기록했지만 이후 전용 인스턴스가 `Stopped`이고 해당 가상화 프로세스가 종료된 것을 확인했다. ARM 게스트는 시작하지 않았다. 근본 원인은 확인되지 않았으며 관찰 기록은 `build/container-arm-chunked-interruption.json`에 있다.

10분 이상 호스트를 중지해 메모리 여유와 스왑 출력이 안정된 것을 확인한 뒤, 더 이른 메모리 중단 기준과 전송 캐시 억제 요청을 적용해 전용 컨테이너를 재개했다. 재개 후 격리 설정과 작은 파일의 실제 송수신을 다시 확인했다. 이후 전송 89개가 완료된 시점에 호스트의 여유 메모리는 84%였지만 스왑 출력이 약 312 MiB 급증해 감시가 전용 호스트의 정지를 요청했다. 진행 중인 조각은 완료로 기록하지 않았고 이후 호스트가 `Stopped`임을 확인했다. Windows ARM 게스트는 아직 시작하지 않았으며 이 스왑 출력의 원인은 확인되지 않았다. 기록은 `build/container-cache-transfer-smoke.json`과 `build/container-arm-chunked-interruption-2.json`에 있다.

이후 Windows ARM 설치 매체의 전송 조각 128개를 모두 받았고 조립 파일의 전체 길이를 채웠다. 파일을 다시 읽어 해시를 확인하면서 Debian ARM 디스크의 조각 전송을 함께 진행하던 중 관찰된 호스트 메모리 여유가 79%에서 52%까지 떨어지고 다시 크게 변동했다. 스왑 출력 증가는 관찰되지 않았으며 변동의 원인은 확인되지 않았다. 추가 Linux 전송을 먼저 중단하고 전용 호스트도 선제적으로 정지했으며, 해당 호스트의 `Stopped` 상태와 가상화 프로세스 종료를 확인했다. Linux 전송은 확인된 4개 조각을 보존한다. Windows 조립 파일의 최종 재독 해시는 중단되어 아직 검증 성공이 아니며 전송 조각과 부분 결과를 보존한다. 이 구간에는 게스트를 시작하지 않았다. 기록은 `build/container-host-memory-fluctuation-20261029.json`에 있다. 재개 전에는 호스트 안정성을 다시 관찰하고, 더 이른 중단 기준을 적용하며 큰 입출력 작업을 순차적으로 수행한다.

호스트 정지 후 메모리 여유 75% 이상과 스왑 출력 증가가 없는 상태가 10분 이상 지속된 것을 확인하고, 여유가 60% 아래로 떨어지면 중단하는 감시 아래 전용 호스트를 다시 시작했다. Linux 부팅 중 ext4 저널 복구가 완료됐고 이전 시스템 저널은 정리되지 않은 종료로 교체됐다. 네 컨테이너 서비스와 기존 시험 컨테이너의 격리·자원 제한을 다시 확인했으며 기록은 `build/container-resume4-smoke.json`에 있다. 두 전송의 완료 조각과 Windows 조립 파일의 길이·소유 기록은 확인했으나, 파일 내용의 전체 해시는 재검증을 마칠 때까지 미확인으로 둔다. 보존 상태는 `build/container-resume4-artifacts.json`에 기록한다. 완성 길이의 재개 경로는 전용 컨테이너에서 시험 스크립트를 직접 실행해 통과했으며 결과는 `build/container-resume4-assembly-direct-test.log`에 있다.

이때 컨테이너의 이전 CTest 목록에는 조각 전송 시험이 등록되지 않아 처음의 필터 실행은 시험을 찾지 못했다. 소스와 시험 목록을 동기화하고 재구성한 다음 시험이 없으면 실패하도록 설정해 호스트 감시·희소 전송·조각 전송 검사 3개를 모두 통과했다. 결과는 `build/container-resume4-registered-tests.log`에 있다.

설치 후 재시작에 대비해 선택적인 디스크 우선 부팅 정책을 추가한 뒤, 호스트 빌드와 전체 검사에서 25개가 통과했고 컨테이너 전용 검사 2개는 호스트에서 건너뛰었다. 전용 컨테이너에서는 다시 빌드한 뒤 부팅 계획·TPM·감시 등 관련 검사 6개가 통과했다. 이 준비 과정에서는 전송물 해시 읽기를 일시 중지해 큰 입출력이 겹치지 않게 한 뒤 재개했다. `build/boot-disk-first-full-tests.log`와 `build/container-boot-disk-first-tests.log`는 이 단계의 결과이며, Windows ARM 게스트가 부팅되었다는 증거는 아니다.

이후 매체의 단독 해시 재독 중에도 호스트 메모리 여유가 약 78%에서 55%로 급락해 60% 기준 감시가 전용 호스트를 중단했다. 관찰된 스왑 출력 증가는 없으며 원인은 확인되지 않았다. 전용 호스트는 `Stopped`이고 게스트는 시작하지 않았다. 이번에도 전체 해시는 완료되지 않아 파일과 전송 조각을 보존한다. 정지 상태의 디스크를 APFS 복제하고(`build/container-host-before-preparation3-snapshot.json`) 호스트 메모리를 3 GiB로 낮췄다. 호스트가 안정된 뒤 작은 메모리 한도의 컨테이너로 매체 검증을 재개했으며 이 단계에서 Windows ARM을 실행하지 않았다.

안정 관찰 후 3 GiB 호스트와 1 GiB·CPU 1개 준비 컨테이너를 시작하고 실제 격리 한도, 기존 128개 조각과 조립 파일의 크기를 확인했다. 이 컨테이너에서 재개 해시·조각 조립·Windows 증거·부팅 계획 검사 4개가 통과했다(`build/container-resumable-digest-tests.log`). 매체 해시가 진행되던 중 약 549 MiB의 호스트 스왑 출력 증가가 감시 기준을 넘어 다시 종료가 요청됐다. 마지막으로 확인한 저장 지점은 128 MiB이며, 전체 SHA-256과 Windows ARM 실행은 미완료다. 중단 원인은 확인되지 않았다(`build/container-prep3-interruption.json`).

새 ARM 검증 실행 파일과 설치 보조 ISO는 컨테이너에서 만들었다. 생성 명령의 마지막 진단 단계는 셸 인용 오류로 실패했지만, 생성된 ISO에서 프로그램과 응답 파일을 따로 추출해 ARM PE 구조와 manifest 일치를 다시 확인했다(`build/container-arm-winpe-payload-validation.json`). 이후 무인 설치용 로컬 계정을 포함한 새 보조 ISO를 만들고 같은 검사를 수행했으며, 계정 정보가 든 ISO의 접근 권한을 소유자로 제한했다(`build/container-arm-local-payload-validation.json`). 이 확인은 프로그램의 실제 Windows 실행을 뜻하지 않는다.

매체 검증 재개 시 체크포인트의 파일 식별자와 해시 라이브러리가 일치하는 것을 확인했다(`build/container-prep3-resume-checkpoint.json`). 설치 파일 복사와 게스트 부팅은 `--prepare-only` 및 `--resume-prepared` 단계로 나누었고, 작은 컨테이너에서 준비할 때 게스트나 TPM을 시작하지 않는지, 나중에 시작하기 전에 메모리와 준비된 파일 및 검증 체크포인트를 다시 확인하는지 파일 전용 시험으로 검사했다. 호스트와 컨테이너의 관련 시험 결과는 `build/windows-arm-preparation-full-tests.log`와 `build/container-arm-phase-checkpoint-tests.log`에 있다. 이 시험은 실제 VM 부팅이 아니다.

그다음 재개에서도 전용 호스트 메모리 감시가 여유의 급락을 감지해 종료를 요청했다. 마지막으로 확인한 체크포인트는 6,241,124,352바이트이며 전체 검증 결과는 아직 없다. 가상화 프로세스가 종료된 이후에도 호스트 메모리 여유가 60% 미만으로 남아 있는 관찰이 있었고, 당시 여러 시스템·사용자 프로세스도 실행 중이었다. 원인을 특정할 근거는 없으며 다른 작업은 종료하지 않았다. 이 중단과 호스트 관찰은 `build/container-prep3-resume-interruption.json` 및 `build/container-prep3-resume-memory.log`에 있다. 다시 정지한 상태의 디스크를 APFS 복제하고 호스트 메모리를 2 GiB로 낮췄으며(`build/container-host-before-preparation2-snapshot.json`), 재시작 전에는 안정성을 다시 확인한다. Windows ARM 게스트는 시작하지 않았다.

2 GiB 호스트의 다음 시작에서는 Linux 파일시스템 저널 복구 중 가상 디스크 쓰기와 해당 디렉터리 조회가 수 분 동안 대기했다. 이를 확인하고 전용 호스트에 종료를 요청했다. 이후 저널 복구가 약 269초에 완료되고 대기가 풀렸으며, 전용 가상화 프로세스의 종료와 호스트의 `Stopped` 상태를 확인했다. 당시 메모리 여유는 약 76–77%였고 관찰된 스왑 출력 증가는 없었다. 정지 후 디스크를 읽기 전용으로 살펴본 결과 ext4 슈퍼블록의 저널 복구 필요 표시는 해제돼 있었다(`build/container-host-prep2-offline-superblocks.json`). 이 표시는 저장장치의 정상 동작이나 이후 부팅의 안전성을 증명하지 않는다. 쓰기 대기의 근본 원인은 확인되지 않았으며, 중단된 매체 검증의 체크포인트를 이번 시작 후 다시 읽지는 못했다. 진단 기록은 `build/container-host-prep2-io-stall.json`과 `build/container-host-prep2-xpc-sample.txt`에 있다. 같은 환경을 반복해서 부팅하기 전에 저장장치 대기와 호스트 메모리 변동을 해결해야 한다. 설치 매체의 전체 해시와 Windows ARM 실행은 여전히 미완료다.

`tools/resumable_digest.py`는 실제 SHA-256 계산 상태를 기본 64 MiB마다 `resources/` 아래의 전용 체크포인트에 내구성 있게 저장한다. 다음 실행은 같은 파일의 장치·inode·크기·수정 및 변경 시각과 암호 라이브러리 버전을 확인한 뒤 저장 지점부터 읽고, 끝까지 계산한 전체 SHA-256이 기대값과 일치해야 성공한다. 중단이나 파일 변경을 성공으로 처리하지 않는다. 체크포인트는 작업자가 소유한 사설 상태이며 적대적인 사용자 간 인증 수단이 아니다. Linux OpenSSL과 macOS CommonCrypto의 제한된 컨텍스트 형식을 사용하고, 시작 시 실제 SHA-256 결과로 ABI를 검사한다. 지원되지 않거나 상태가 달라진 환경은 거부한다.

전용 컨테이너에서 조립을 재개할 때는 `assemble --resume --verification-checkpoint "$IIV_PRIVATE_CHECKPOINT"`를 사용하고 기존 전송 manifest를 표준 입력으로 전달한다. 완료된 체크포인트는 같은 파일이 변경되지 않았을 때 Windows ARM 시험의 `--installer-verification-checkpoint`에도 사용할 수 있다. 시험은 미디어 복사 전후에 동일성을 다시 확인한다. 기본값은 이전과 같이 전체 파일을 매번 직접 해시한다.
