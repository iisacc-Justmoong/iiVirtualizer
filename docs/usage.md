# 빌드와 운영

## 호스트 빌드

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/.local/SDK/iiSDKCore;$HOME/.local/SDK/iiJson"
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 tools/bootstrap_qemu.py
```

QEMU는 별도 실행 프로세스이며 iiVirtualizer에 링크하지 않는다. 부트스트랩은 SHA-256으로 고정한 QEMU·libslirp 소스를 `build/dependencies/`에 내려받아 빌드한다. macOS에서는 Homebrew의 glib·pixman·pkg-config와 Xcode 도구를 사용할 수 있다. 설치본이 있으면 아래 명령의 `--qemu-aarch64`에 해당 실행 파일을 지정한다.

## Linux 게스트 빌드

반복 이미지 작성 시 검증한 원본 커널과 초기 드라이버 아카이브를 해시로 확인하여 재사용한다. 패키징 순서와 gzip 시각은 고정되어 같은 입력에서 같은 initramfs를 얻는다.

Linux 호스트에서는 네이티브 컴파일러를 사용한다. macOS에서는 C++23과 정적 glibc·libstdc++를 지원하는 Linux 교차 도구 체인이 필요하다. 예를 들어 Workspace 내 iiSDKCore의 기존 ARM64 도구 체인을 재사용할 수 있다.

```sh
cmake -S src/guest -B build/guest/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=/Volumes/Storage/Workspace/SDK/iiSDKCore/build/linux-probe/toolchain.cmake
cmake --build build/guest/build --target ii-init ii-guest-service --parallel 4
python3 tools/prepare_guest.py --init build/guest/build/ii-init \
  --workload build/guest/build/ii-guest-service
```

이미지 작성기는 고정된 Alpine Linux 3.22.7의 Linux 6.12.114 커널, 초기 드라이버, musl·BusyBox·파일시스템 도구를 바탕으로 자체 `/init`을 패키징한다. EFI zboot를 풀 때 `zstd` 실행기가 필요할 수 있다. `image.json`에 커널·PID 1·initramfs 해시와 배포물 출처가 기록된다. 패키지 SHA-256은 최초 HTTPS 취득 결과로 고정한 값이며 APK 서명 검증을 대신한다고 주장하지 않는다.

## 인스턴스 수명주기

### 하나의 런타임에서 여러 VM 제어

실제 동시 부팅 검증은 `python3 tests/multi_boot.py --accel hvf --qemu build/dependencies/qemu-11.1.2/build/qemu-system-aarch64`로 수행한다. C++ 제어 프로세스 하나가 Runtime 객체 하나를 유지한 상태에서 Linux VM 두 개를 병렬 기동한다. 각 서비스에 접속하고, 한 VM의 일시정지·종료 중에도 다른 VM의 응답을 확인한다. 포트 충돌로 실패하는 세 번째 VM과 첫 VM의 재기동을 같은 일괄 작업에 넣어 실패 분리와 서로 다른 부팅 횟수를 확인한다. 결과는 `build/multi-verification/`에 보관한다.

`create`로 서로 다른 ID를 생성한 다음 `start-many vm-a vm-b --workers 4`로 같은 런타임에서 병렬 기동한다. 각 VM은 개별 QEMU 프로세스로 실행되며 CPU·메모리·디스크·UUID·제어 소켓·콘솔을 공유하지 않는다. 두 VM의 게스트 포트가 같아도 호스트 전달 포트는 다르게 지정해야 한다.

`describe-many`, `pause-many`, `resume-many`, `stop-many`도 여러 ID를 받는다. `stop-many --force`는 각 VM을 명시적으로 강제 종료한다. JSON 배열은 입력 순서를 유지하며 각 행에 `id`, `ok`와 성공 시 `instance`, 실패 시 `error`가 포함된다. 일부만 실패해도 성공한 VM의 상태는 유지되며 종료 코드는 1이다. `list`는 정상 VM의 기존 JSON 형식을 유지하고, 조회가 실패한 VM은 `state=unavailable`과 오류를 함께 출력한다.

공개 C++ API의 `submit(Operation, ID)`는 iiSDK `Future<Instance>`를 반환한다. `apply(Operation, IDs)`는 실행기 용량 내에서 병렬 처리한 VM별 결과를 반환한다. `snapshot()`은 VM별 오류를 보존하는 목록이며, 기존 `list()`는 오류 발생 시 전체 오류를 반환한다. 블로킹 일괄 작업은 iiSDK worker나 HostLoop 스레드에서 제출 전에 `wrong_executor`로 거절된다. 해당 스레드에서는 `submit`을 사용한다.

같은 VM에 작업이 겹치면 스레드·프로세스 사이에서 `busy`를 반환한다. 다른 VM의 긴 기동·종료가 저장소 전체를 잠그지 않는다. 실행기 worker 수는 제어 작업의 병렬도이며 실행 중인 VM 개수의 상한이 아니다. 주입 백엔드는 서로 다른 VM의 동시 호출을 지원해야 한다.

프로젝트 루트에서 다음 명령을 실행한다. QEMU 경로를 매번 쓰지 않으려면 `build/dependencies/qemu-11.1.2/build`를 현재 셸의 PATH에 추가할 수 있다. 상태 저장소의 기본값은 `build/instances`이며 `--root`로 변경한다. AF_UNIX 경로 제한 때문에 상태 저장소 경로는 짧게 유지한다.

```sh
export PATH="$PWD/build/dependencies/qemu-11.1.2/build:$PATH"
build/iivirtualizer doctor
build/iivirtualizer create demo \
  --kernel build/images/aarch64/Image \
  --initramfs build/images/aarch64/initramfs.cpio.gz \
  --arch aarch64 --accel hvf --cpus 2 --memory 512 --disk 128
build/iivirtualizer start demo
build/iivirtualizer wait-ready demo --timeout 30
build/iivirtualizer guest-info demo
build/iivirtualizer pause demo
build/iivirtualizer resume demo
build/iivirtualizer stop demo
build/iivirtualizer describe demo
```

Linux 호스트에서는 `--accel kvm`을 사용하며 `/dev/kvm` 접근 권한이 필요하다. CPU 에뮬레이션이 필요한 경우 `--accel tcg`를 명시한다. `auto`는 호스트의 기본 가속기를 고르며 하드웨어 가상화 실패를 TCG로 숨기지 않는다. `doctor`는 실행기 버전과 프로세스 기능을 조회하며 실제 하드웨어 부팅 성공을 증명하지는 않는다.

게스트가 준비되면 `os=iiOS runtime=iiSDKCore pid=1 workers=2 tasks=4` 등의 값을 반환한다. `/data`는 인스턴스의 ext4 데이터 볼륨이다. 새 볼륨은 초기 4096바이트가 전부 0이고 생성 정책이 허용한 경우에만 포맷한다. `--base-disk`로 가져온 raw 볼륨에는 자동 포맷을 허용하지 않는다. 정상 종료 시 동기화·마운트 해제·전원 종료를 수행한다. 재시작해도 `/data/boot-count`는 보존된다.

게스트 루트는 initramfs이므로 재부팅 시 다시 생성된다. 영속 데이터는 `/data`에 둔다. 선택적으로 `prepare_guest.py --workload /path/to/static-linux-executable`로 `/app/service`를 넣으면 PID 1이 iiSDKCore Process로 실행·감시하고 종료 시 회수한다. 워크로드의 stdout·stderr는 VM 콘솔에 기록된다. 워크로드는 게스트 관리자 권한으로 실행되며 자동 재시작 정책은 제공하지 않는다.

네트워크는 기본 비활성이다. `create`에 `--network`를 지정하면 사용자 모드 NAT와 DHCP를 사용한다. `--forward 8080:8080`과 같이 호스트 loopback 포트를 게스트 TCP 서비스로 전달한다. 외부 공개 주소 바인딩은 제공하지 않는다.

`console ID`는 최근 최대 1 MiB의 콘솔을 출력하고, `plan ID`는 실행 인수 배열을 JSON으로 출력한다. 모든 경로는 인수 단위로 전달하며 셸 문자열을 실행하지 않는다. 정지한 VM만 `remove ID`로 삭제할 수 있으며 해당 볼륨도 함께 삭제된다. 응답 없는 게스트의 종료는 `stop ID --force`로 명시한다.

## C++ 소비자와 설치

실제 부팅 검증은 `tests/boot.py --accel hvf --qemu build/dependencies/qemu-11.1.2/build/qemu-system-aarch64 --network`로 실행한다. Linux에서는 `--accel kvm`, CPU 에뮬레이션에서는 `--accel tcg`로 바꾼다. `--network` 검증은 `ii-guest-service`를 포함한 이미지가 필요하다. 이 테스트는 인스턴스 두 번 부팅, `/data` 보존, 일시정지·재개, 정상·강제 종료와 선택적 HTTP 전달을 확인하고 `build/verification/`에 원시 콘솔과 결과를 기록한다.

`find_package(iiVirtualizer CONFIG REQUIRED)`와 `iiVirtualizer::iiVirtualizer`를 사용한다. `Runtime`은 값으로 전달한 `Backend`의 함수들로 실행기를 구성한다. 샘플은 `example/lifecycle.cpp`이다. `./install-native.sh`는 빌드와 테스트 후 `build/install`에 설치하며, 첫 인수로 설치 위치를 지정할 수 있다. 외부 의존 패키지는 소비자 측 `CMAKE_PREFIX_PATH`에서도 접근 가능해야 한다.
