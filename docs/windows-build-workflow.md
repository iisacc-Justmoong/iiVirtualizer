# Windows 빌드 가상머신의 반복 작업 계약

이 문서는 Windows Server Core x86-64 설치본에 CMake, Ninja, LLVM-MinGW와 빌드 작업자를 두고 프로젝트 소스와 작업을 교체하는 계약이다. Windows 설치 매체와 도구에는 각각의 라이선스 및 사용 조건이 적용된다. 게스트 네트워크와 호스트 디렉터리 공유는 필요하지 않다.

Microsoft의 [Windows Server 2025 평가판 안내](https://www.microsoft.com/en-us/evalcenter/download-windows-server-2025)에 따르면 평가판은 180일 후 만료되며 설치 후 첫 10일 안에 인터넷을 통해 활성화하지 않으면 자동 종료된다. 따라서 네트워크를 차단한 평가판 시험은 지속 사용 가능한 오프라인 라이선스 환경을 입증하지 않는다. 지속 사용에는 적절한 Windows 라이선스와 그 조건에 맞는 활성화가 필요하다.

Microsoft의 [Windows Server 2025 하드웨어 요구 사항](https://learn.microsoft.com/en-us/windows-server/get-started/hardware-requirements?pivots=windows-server-2025)은 Server Core에 최소 2 GiB 메모리를 명시한다. 이는 운영체제 설치의 하한이며 컴파일할 프로젝트에 충분한 메모리의 보증은 아니다.

## 최초 설치 준비

Windows x86-64 설치 ISO, Windows용 `iiv-build-agent.exe`, 그리고 [게스트 빌드 안내](guest-build.md)에 명시된 해시와 일치하는 LLVM-MinGW·CMake·Ninja 압축 파일이 필요하다. 압축 파일 세 개는 `resources/build-layer/downloads/`에 둔다. 작업자는 Windows용 iiSDKCore를 사용해 `src/builder/`에서 별도로 빌드한다. 작업자의 추가 동적 라이브러리는 `resources/build-layer/windows-core/install/bin/`에서 페이로드로 복사하며, LLVM-MinGW의 일부 런타임 라이브러리는 도구 압축 파일에서 복사한다. 최초 설치 페이로드 생성은 현재 macOS의 `hdiutil`을 사용한다.

```sh
python3 -B tools/prepare_build_payload.py \
  --agent build/windows-bootstrap-agent-gcc/iiv-build-agent.exe \
  --output resources/build-layer/windows-initial
```

이 명령은 현재 iiSDK 소스의 스냅샷, 최초 빌드 작업, 무인 설치 응답 파일과 `windows-payload.iso`를 만든다. 관리자 암호와 소스가 담긴 출력은 개인적으로 보관한다. 처음 생성할 때에는 설치 ISO와 생성된 payload ISO를 `create`의 `--boot-iso`, `--payload-iso`에 지정하고 `--boot disk --disk-bus sata --guest-channel serial --graphics vga --allow-reboot --boot-disk-first`를 함께 지정한다. `--memory`, `--cpus`, `--disk`는 실행 환경과 빌드 작업의 요구에 맞춘다. 실제 VM을 시작하는 검증은 전용 Linux 컨테이너에서 QEMU TCG로만 수행한다.

## 작업과 매체

최초 설치 매체는 기존 설치 자동화가 만들며, 설치된 가상머신은 디스크 우선으로 부팅한다. 후속 작업은 별도 ISO에 소스 스냅샷과 JSON 작업 명세를 담는다. 각 작업은 새 식별자와 게스트 내부의 독립 작업 디렉터리를 사용한다. 명세는 인자 배열, 제한 시간, 회수할 상대 경로를 지정하며 셸 명령 문자열로 변환하지 않는다. 소스의 빌드 산출물, Git 메타데이터, 캐시와 심볼릭 링크는 스냅샷에서 제외한다.

일반 프로젝트의 `resources/`에 있는 정적 입력은 스냅샷에 보존하되 `.recovery`, `.state`, `.cache` 디렉터리와 임시 미리보기·잠금 파일을 제외한다. 심볼릭 링크와 일반 파일이 아닌 입력도 복사하지 않는다. 최초 설치용 iiSDKCore와 iiJson 스냅샷에서는 각 SDK 루트의 `resources/` 전체를 제외한다. 이 SDK 빌드에 필요한 소스와 예제는 별도로 포함한다.

게스트 시작 스크립트는 작업 매체가 있으면 그 작업을 준비하고 설치된 작업자로 실행한다. 작업 매체가 없으면 기존 최초 설치 작업을 실행하는 호환 경로를 유지한다. 각 작업의 명령과 소스는 해당 VM을 신뢰하는 호출자가 제공하며 게스트에서 실행된다. 작업자의 이벤트에는 작업 식별자가 포함되고, 완료는 모든 단계와 지정 산출물이 확인된 경우에만 인정한다.

`tools/windows_build_job.py`는 Python의 `stage_job` 함수와 CLI를 제공한다. `--recipe` JSON에는 `steps`와 `artifacts`를 지정한다. 단계마다 `argv` 배열과 1~3,600,000 ms의 `timeout_ms`가 필요하며, 선택적인 `cwd`와 `path`도 사용할 수 있다. 산출물은 작업 디렉터리에서의 상대 경로이다. 단계는 최대 32개, 산출물은 최대 64개이며 각 산출물의 전송 상한은 32 MiB이다. 작업 ID, 게스트 작업 경로와 기본 도구 PATH는 준비 도구가 지정한다.

다음 예제는 최초 설치 작업이 `C:/iiv/work/build/install`에 iiSDKCore를 빌드한 VM을 대상으로 한다. 다른 프로젝트에는 해당 프로젝트의 CMake 인자와 산출물 경로를 담은 별도 recipe를 사용한다. ISO에는 소스가 포함되므로 생성 디렉터리는 소유자만 접근할 수 있게 만든다.

```sh
python3 -B tools/windows_build_job.py \
  --source example/guest-build \
  --recipe example/guest-build/windows-job.json \
  --output resources/build-layer/jobs/example
build/iivirtualizer set-payload win-build --root resources/gb \
  --payload-iso resources/build-layer/jobs/example/windows-job.iso
```

macOS에서는 `hdiutil`, Linux에서는 `genisoimage`가 ISO를 만든다. 생성된 `job.json`은 빌드 이벤트의 식별자 및 예상 단계·산출물을 검증할 때 사용하며 `source-manifest.json`은 전달한 소스의 해시를 기록한다. 이미 존재하는 출력 디렉터리는 덮어쓰지 않는다.

## 인스턴스의 작업 매체 교체

매체는 작업자가 소유한 디스크 부팅 VM에 기존 payload 슬롯이 있고 VM이 정지한 경우에만 교체한다. 인스턴스 잠금을 잡고 백엔드에서 정지 상태를 확인한 뒤 새 일반 파일을 같은 디렉터리에 복사하고 원자적으로 교체한다. 복사나 확인에 실패하면 기존 매체를 유지한다. 실행 중이거나 상태를 확인할 수 없는 VM, payload 슬롯이 없는 VM, 유효하지 않은 입력은 거부한다. 운영체제 디스크와 인스턴스 UUID는 교체하지 않는다.

새 VM의 설치·작업 매체와 펌웨어를 복제할 때 `InstanceSpec::media_copy_mib_per_second` 또는 `create --media-copy-rate`로 초당 1~64 MiB를 지정할 수 있다. 0은 기존 기본 복사 방식이다. 제한 복사는 작은 조각을 동기화하며 macOS의 캐시 우회 또는 Linux의 캐시 해제를 요청한다. 캐시 제어는 운영체제에 대한 요청이므로 실제 호스트 메모리 한도를 보증하지 않는다. 이 설정은 생성 작업에만 적용하고 VM 명세에는 저장하지 않는다. 격리 시험 도구는 기본 4 MiB/s를 사용하며 1~16 MiB/s를 허용한다.

앱에서는 `Runtime::replace_payload(id, path)`를, CLI에서는 `set-payload ID --payload-iso PATH`를 사용한다. 디스크 부팅 게스트의 일반 정지는 iiOS 전용 직렬 프로토콜 대신 QMP의 가상 전원 버튼을 통해 요청하며, 실제 정지 상태를 다시 확인한다. 게스트가 종료하지 않으면 시간 초과로 실패하고 강제 종료는 별도 명시가 필요하다.

설치되는 `ii_virtualizer` Python 모듈의 `Virtualizer`는 실행 파일과 인스턴스 루트를 주입받아 Windows VM 생성, 상태 조회, 매체 교체, 시작과 정지를 제공한다. 설치 접두사의 `share/iiVirtualizer/python`을 `PYTHONPATH`에 추가하여 가져온다. 이 어댑터는 셸 문자열 없이 CLI에 인자 배열을 전달하고 실패를 예외로 반환한다. VM의 권한과 상태 검사는 동일한 런타임 계약을 따른다.

설치되는 문서 묶음에서는 임시 미리보기·잠금 파일과 `.DS_Store`를 제외한다.

Windows를 재사용할 때는 최초 인스턴스 생성 시 `--boot-disk-first`를 지정한다. 설치 매체보다 설치된 디스크를 우선하여 이후 시작에서 설치 프로그램이 다시 실행되는 것을 방지한다. 최초 설치와 작업 결과 확인 방법은 [게스트 빌드 안내](guest-build.md)를 따른다. 기존 시작 스크립트가 이미 설치된 VM에는 새 시작 스크립트가 자동으로 소급 설치되지 않으므로 새 최초 설치 페이로드가 필요하다.

## 검증 범위

계약 시험에서는 중지·실행 상태와 입력 오류에 따른 매체 교체 및 독립 소스 스냅샷을 확인한다. 실제 Windows 테스트는 전용 Linux 컨테이너에서 자원 제한과 QEMU TCG, 네트워크 차단을 확인한 뒤 수행한다. 설치된 Windows 내부에서 컴파일한 실행 파일의 실행과 산출물 회수까지 확인해야 실제 빌드 성공으로 기록한다.

2026-11-24 Apple Silicon 호스트의 전용 Linux 컨테이너에서 Windows Server 2025 x86-64 설치를 세 차례 시작했다. 첫 시도는 게스트 4 GiB, 둘째는 2.5 GiB, 셋째는 2 GiB를 사용했다. 처음에는 Windows Server 설치 화면에서 1%까지, 마지막에는 설치 디스크 검색 화면까지 확인했다. 각 시도는 호스트 가용 메모리가 시험 감시 기준인 40% 아래로 내려가 중단되었다. Windows 설치 완료, 게스트 빌드 작업의 이벤트, 실행 파일과 후속 매체 교체는 확인하지 못했다. 중단 신호 시점에 감시된 스왑 증가량은 세 번 모두 0이었다. 두 재시도는 중단된 시험 전용 디스크를 초기화하고 같은 인스턴스의 메모리 설정을 조정한 시험이다.

세 시도의 인스턴스 명령과 QEMU 인자, 마지막 화면은 `build/windows-proof-20261124/`에 보존했다. 이 기록은 컨테이너에서 Windows 설치 프로그램이 시작된 증거이며 게스트 내 앱 빌드의 성공 증거가 아니다.

`tools/guest_build.py --reuse --payload-iso ... --job ...`은 격리 시험에서 새 작업 매체를 교체하고 실행 결과를 받는다. `--expect-output`과 `--verify-pe`를 지정하면 출력 표식과 지정된 x86-64 PE 산출물도 검사한다. 작업 성공과 실제 앱 실행은 별개의 사실이므로 출력 표식을 요청하지 않은 작업을 앱 실행 검증으로 표시하지 않는다. 이 시험 도구는 정상을 먼저 요청하고 실패할 때만 정지된 상태를 확보하기 위해 강제 종료하며, 강제 여부를 결과에 기록한다. 강제 종료한 설치 디스크의 무결성은 이 도구가 보증하지 않는다.

시작 전 Linux 컨테이너 호스트의 물리 메모리와, cgroup v2 `memory.max`가 존재하고 유한한 경우 그 한도가 각각 게스트 메모리보다 최소 512 MiB 큰지 검사한다. 물리 메모리를 읽을 수 없거나 확인한 한도가 부족하면 VM을 시작하지 않는다. cgroup v1 제한은 이 검사에 포함되지 않는다.

게스트 빌드 시험은 준비 전 중단 신호를 등록하고 실행 중 신호를 받으면 소유한 VM의 정리를 시도한다. 생성 중 강제 중단되면 시작되지 않은 임시 생성 디렉터리가 남을 수 있다. 컨테이너 안의 `vm_test_watchdog.py signal --kind build --vm-id ID`는 `guest_build.py` 경로와 VM ID가 정확히 일치하는 프로세스만 찾아 중단을 요청한다. 메모리와 스왑 감시는 별도로 실행해야 하며 이 신호만으로 호스트 자원 제한이 생기는 것은 아니다.

시험 컨테이너 생성 시 Apple `container`와 nerdctl 모두 `--init`을 지정한다. 실제 고아 프로세스 회수 여부는 별도의 `tests/reaper_container.py` 실행으로 확인한다.

메모리 사용을 제한해야 하는 경우 인스턴스 생성 시 `--direct-io` 또는 `InstanceSpec::direct_io`를 선택한다. QEMU는 데이터 디스크와 부팅·작업 매체의 파일 캐시를 우회하되 플러시 요청은 유지한다. 이 설정은 인스턴스에 저장되어 재시작과 작업 매체 교체 후에도 유지된다. 파일 시스템이 직접 I/O를 지원하지 않으면 시작은 실패하며 자동으로 캐시 모드로 변경하지 않는다. 이 옵션은 QEMU가 실행되는 Linux의 파일 캐시에 적용되고 컨테이너 바깥 호스트의 캐시나 전체 메모리 사용량을 제한하지 않는다.

최초 빌드 작업의 동시 컴파일 수는 `prepare_build_payload.py --build-parallel N`으로 1부터 16까지 지정한다. 기본값은 3이며, 게스트 메모리가 작은 경우 1로 낮춰 Windows와 Linux 작업의 빌드 단계를 직렬화할 수 있다. 각 빌드 단계의 제한 시간은 `--build-timeout-ms`로 1~3,600,000 ms 사이에서 지정하며 기본값은 1,800,000 ms이다. 이 옵션은 다른 단계의 제한 시간을 변경하지 않는다. 후속 작업의 동시성과 제한 시간은 해당 작업 recipe에 지정한다.

2026-11-24 Linux 격리 컨테이너의 ext4 파일에서 QEMU `qemu-io`로 64 MiB를 읽고 `mincore`로 해당 파일의 상주 페이지를 관찰했다. 직접 I/O 읽기 후 상주한 파일 페이지는 0 MiB, 일반 캐시 읽기 후는 64 MiB였다. 별도의 실제 QEMU QMP 조회에서도 데이터 디스크와 두 ISO의 파일 노드가 직접 I/O를 사용하고 플러시를 유지함을 확인했다. 이 측정은 컨테이너 내부 파일 캐시에 한정하며 macOS 전체 메모리나 Windows 앱의 빌드 성공을 보장하지 않는다.

QEMU 소프트웨어 에뮬레이션의 변환 캐시는 `QemuOptions::tcg_cache_mib` 또는 전역 CLI 옵션 `--tcg-cache-mib`으로 1~1,024 MiB 범위에서 제한한다. 0은 QEMU 기본값이고, 0이 아닌 값은 TCG가 아닌 가속기에서 거부한다. 이 설정은 VM에 저장하지 않는 실행 환경 옵션이므로 `plan`과 `start`에 같은 값을 전달한다. Python에서는 `Virtualizer(..., tcg_cache_mib=64)`, 게이트웨이 런타임 설정에서는 `tcg_cache_mib`, 격리 빌드 시험에서는 `tools/guest_build.py --tcg-cache-mib 64`로 지정한다. 변환 캐시의 상한은 전체 프로세스나 호스트 메모리 한도가 아니며 작은 캐시는 에뮬레이션 성능을 낮출 수 있다.

Apple의 [container 기술 안내](https://github.com/apple/container/blob/main/docs/technical-overview.md#releasing-container-memory-to-macos)는 Linux 게스트가 해제한 페이지가 현재 macOS에 즉시 반환되지 않을 수 있다고 설명한다. 따라서 컨테이너 내부의 직접 I/O나 일시 정지만으로 호스트 메모리 회수를 보장할 수 없다. 2026-11-24 추가 Windows 설치 시도는 77%까지 진행한 뒤 호스트 스왑 증가 약 449 MiB가 감지되어 전용 컨테이너가 정지했다. 정상 종료 기록은 남지 않았고 부분 설치 디스크는 부팅할 운영체제를 찾지 못했다. 같은 인스턴스를 설치 매체로 다시 시작한 시도에서도 실행 중인 VM의 일시 정지 후 호스트 스왑이 약 445 MiB 증가해 전용 컨테이너가 정지했다. 이 두 수치는 각 감시 표본 간 호스트 전체 스왑 증가량이며 특정 프로세스에 귀속할 수 없다. 두 시도 모두 Windows 설치 완료나 앱 빌드 성공을 입증하지 않는다.

2026-11-24 별도의 512 MiB Linux 격리 컨테이너에서 실제 QEMU x86-64 인스턴스를 일시 정지 상태로 시작해 변환 캐시 매핑을 조회했다. 256 MiB 설정의 실행 가능 매핑은 268,431,360바이트, 64 MiB 설정은 67,104,768바이트였다. 이는 가상 주소 공간의 예약 크기와 설정 적용을 확인한 결과이며, Windows 실행 시 실제 상주 메모리나 macOS의 스왑 감소를 측정한 결과는 아니다.

메모리가 더 작은 시험에는 [Windows Server 2022 평가판](https://www.microsoft.com/en-us/evalcenter/download-windows-server-2022)의 Server Core를 선택할 수 있다. Microsoft의 [버전별 메모리 요구 사항](https://learn.microsoft.com/en-us/windows-server/get-started/hardware-requirements?pivots=windows-server-2022)은 Server 2022 Core에 최소 1 GiB를 명시한다. 격리 빌드 시험의 Windows 메모리 허용 범위는 1,024~8,192 MiB이지만 이는 시험 도구의 경계이며, Server 2025의 최소 요구나 개별 빌드의 실제 메모리 수요를 완화하지 않는다. Server 2022 평가판도 180일 후 만료되며 첫 10일 안에 활성화해야 자동 종료를 피할 수 있다.

격리 빌드 시험에서 기존 Windows VM을 재사용할 때는 명령행에 입력한 값뿐 아니라 저장된 VM 자체의 CPU, 메모리와 디스크 크기도 시험 범위에 맞는지 확인한다. 값이 누락되었거나 정수가 아니거나 범위를 벗어나면 시작하지 않는다.

2026-11-24 Microsoft에서 받은 Server 2022 평가판 ISO의 크기 5,044,094,976바이트와 SHA-256 `3e4fa6d8507b554856fc9ca6079cc402df11a8b79344871669f0251535255325`를 확인했다. 이 해시는 Microsoft가 게시한 공식 체크섬이 아닌 [별도 공개 목록](https://github.com/rgl/windows-vagrant/blob/master/windows-evaluation-isos.json)의 같은 다운로드 주소 및 크기와 일치한다. ISO 내부 WIM 메타데이터를 읽어 이미지 1번이 x64 `SERVERSTANDARDCORE` 평가판임을 확인했다. 이는 설치와 게스트 빌드 성공을 뜻하지 않는다.

2026-11-24 전용 Linux 컨테이너를 1,792 MiB로 제한하고 Server 2022 Core 게스트에 1,024 MiB를 할당한 실제 설치 시험에서 Windows의 설치 파일 준비 화면 4%까지 확인했다. 실행 중 관찰한 컨테이너 메모리는 약 1.24 GiB였고 해당 표본에서 cgroup 메모리 부족 종료는 없었다. 약 22분의 호스트 감시 표본에서는 추가 스왑이 0이었지만 호스트 메모리 압박이 경고 수준으로 올라가고 가용 비율이 41%까지 내려가 시험 전용 컨테이너가 중단되었다. 정상 종료 기록은 확인하지 못했고 Windows 설치 완료, 앱 빌드·실행 또는 후속 매체 교체를 입증하지 않는다.

같은 인스턴스의 재설치에서는 설치 파일 준비와 기능·업데이트 설치를 지나 Windows 서비스 시작 화면과 부팅 후 소스 454개 복사 화면을 확인했다. 그 뒤 호스트 전체 스왑 증가량이 한 감시 표본 사이에 655,785,984바이트로 관찰되어 작업을 중단했다. 이 수치를 특정 프로세스의 사용량으로 귀속할 수 없다. 정상 종료 요청에 응답하지 않아 강제 정지했으며 `stopped.json`의 `forced_stop`은 `true`였다. 이 시도에서도 게스트 빌드 이벤트와 앱 실행, 후속 매체 교체는 확인되지 않았다.

같은 Windows 디스크를 다시 부팅한 시험에서는 초기 설정 적용 화면까지 도달했지만 게스트 빌드 이벤트는 나오지 않았다. 호스트 가용 메모리 비율이 43%이고 메모리 압박이 경고 수준이 되자 게스트를 일시 정지했으며, 다음 감시 표본에서 가용 비율이 40%로 내려가 시험 전용 컨테이너를 중단했다. 이 구간에서 감지된 추가 스왑은 없었고, 중단 감시기는 VM의 정지 기록 생성을 확인했다. 앱 빌드·실행과 후속 매체 교체는 확인되지 않았다.

이 시험의 Apple container 1.7.1 명명 볼륨은 호스트 저장소 캐시를 켠 상태로 연결되었다. Apple은 Linux 파일 시스템 문제를 피하기 위해 이 설정을 기본값으로 사용하므로 캐시를 임의로 끄지 않는다. 설치 매체, 작업 매체와 중지된 시험 전용 볼륨 파일의 읽기 전용 매핑에서 `mincore`로 상주 페이지를 조회한 한 시점에는 모두 0바이트였다. 이는 해당 파일들의 그 시점 표본이며 다른 시스템 캐시나 메모리 압박의 원인을 특정하지 못한다.
