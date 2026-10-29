# 아키텍처와 실행 계약

설계 근거는 Linux의 [initramfs와 `/init` 부팅 계약](https://www.kernel.org/doc/html/latest/filesystems/ramfs-rootfs-initramfs.html), QEMU의 [QMP 명세](https://www.qemu.org/docs/master/interop/qmp-spec.html)와 [ARM virt 머신](https://www.qemu.org/docs/master/system/arm/virt.html)이다.

게스트는 부팅 시 virtio 디스크·네트워크, 패킷 소켓, CRC32C·ext4 드라이버를 초기화하며 커널의 모듈 요청을 위한 `/sbin/modprobe`를 제공한다. QMP `quit`가 출력 큐 전송 전에 연결을 닫아도 요청 전 UUID 검증과 요청 후 실제 종료 확인을 모두 거친다.

QEMU 초기화 제한은 기본 30초, 제어 요청 제한은 기본 5초이며 `QemuOptions`에서 독립적으로 지정한다. 두 값은 양수여야 한다. 초기화 실패에는 종료 사유·종료 코드·시그널과 표준 오류를 함께 반환한다.

iiVirtualizer는 단일 호스트에서 Linux 커널과 iiSDK 사용자 공간을 실행하는 C++23 VM 런타임이다. EC2의 인스턴스 생성·자원 지정·시작·중지·조회라는 사용 모델을 로컬 호스트에 적용한다.

`Runtime`은 영속 인스턴스와 디스크를 소유하고, `Backend`의 함수 계약에 VM 실행과 제어를 위임한다. QEMU 어댑터는 iiSDKCore의 `Process`로 셸 없이 실행하고 QMP로 제어한다. macOS의 HVF와 Linux의 KVM은 동일 아키텍처의 하드웨어 가상화이며, TCG는 명시적으로 선택하는 CPU 에뮬레이션이다.

Linux 커널은 iiSDK를 커널 안에 링크하지 않는다. 커널은 메모리·스케줄링·드라이버를 담당하고, 정적 링크한 `ii-init`이 PID 1로 iiSDKCore의 EventLoop·Runtime을 실행한다. 사용자 프로그램은 이 게스트 사용자 공간에서 실행된다. 커널과 외부 QEMU의 라이선스 및 배포 책임은 각각 분리한다.

애플리케이션의 파일 변경 영역은 `/data/Society.societycontainer/Files`이다. PID 1은 부팅 관리 권한을 유지하지만 외부 워크로드는 별도 mount namespace·읽기 전용 실행 루트·UID/GID 1000·권한 제거·seccomp 필터를 구성한 뒤 실행한다. 이 경계를 준비하지 못하면 실행을 거부한다. 프로세스·디렉터리 배치와 기존 이미지 갱신은 [파일시스템 계약](filesystem.md)에 정의한다.

인스턴스별 잠금은 같은 VM의 중복 생성·시작·삭제를 프로세스 사이에서도 배제한다. 잠금 파일은 VM 디렉터리 밖의 `.vm-ID.lock`에 보존하여 삭제·재생성 시 잠금 소유권이 분리되는 문제를 막는다. 다른 VM은 서로 다른 잠금을 사용한다. 이미 작업 중인 VM에 겹치는 요청은 대기하는 대신 `busy`로 반환하여 공용 실행기를 점유하지 않는다. 인스턴스 식별자는 소문자·숫자·하이픈으로 제한한다. 실행 상태는 QMP로 확인하며 `query-uuid`와 응답 ID를 대조한다.

하나의 `Runtime`은 iiSDKCore `Runtime`·`Concurrent`로 구성한 작업 실행기 하나를 소유한다. 일괄 작업은 VM 수와 관계없이 worker·queue 용량 안의 작업 레인만 제출하므로 용량이 1이어도 더 큰 목록을 순서대로 처리할 수 있다. VM마다 vCPU·메모리·QEMU 프로세스·디스크·콘솔·제어 소켓·UUID를 분리한다. 제어 작업의 worker 수가 실행 가능한 VM 수를 제한하지는 않는다. 이미 기동한 VM은 각자의 실행 프로세스에서 계속 작동한다.

`submit(Operation, ID)`는 `Future<Instance>`를 반환한다. `apply`는 여러 ID를 실행기 용량 안에서 나누어 병렬 제출하고 입력 순서대로 VM별 결과를 반환한다. 한 VM이 실패하여도 성공한 다른 VM은 유지된다. 중복·유효하지 않은 ID는 어떤 작업도 제출하기 전에 일괄 요청을 거부한다. `snapshot`은 작업 중이거나 응답하지 않는 VM도 오류 행으로 보존하여 정상 VM의 조회 결과를 함께 제공한다. 기존 `list`는 오류 발생 시 전체 오류를 반환하는 계약을 유지한다.

주입하는 백엔드는 서로 다른 VM의 호출을 동시에 처리할 수 있어야 한다. 런타임 파괴와 다른 호출의 동시 수행은 허용하지 않는다. 파괴 시 진행 중인 제어 작업의 수명을 정리하고 대기 작업의 Future를 종결하지만, 이미 실행 중인 VM을 종료하지 않는다. Future 취소는 VM 전원 종료 명령이 아니며 시작한 부작용을 되돌리지 않는다. 종료에는 명시적인 `stop`이 필요하다.

QMP가 사라져도 pidfile의 프로세스가 존재하면 정지로 간주하지 않는다. 프로세스 번호는 살아 있는 소유자가 있을 가능성을 확인하여 변경을 거부하는 용도로만 사용한다. PID 재사용으로 소유 여부가 불명확하면 운영자가 상태를 확인해야 하며, 런타임은 그 PID에 종료 시그널을 보내지 않는다.

각 `Runtime`의 VM 실행 범위는 해당 호스트이다. `Gateway`와 `GatewayAgent`는 교체 가능한 계정 세션 공급자·검증기·전송 계약으로 이 런타임들을 원격 제어한다. 기본 전송은 OpenSSH subsystem이며, 수신 측은 iisacc의 현재 세션을 HTTPS로 확인한 뒤 계정 subject에 부여된 런타임·작업·VM 권한을 적용한다. 여러 런타임을 등록할 수 있고 생성은 서버의 이미지 카탈로그를 사용한다. 자세한 공개 API와 전송 경계는 [게이트웨이 계약](gateway.md)에 있다.

0.5.0의 `Gateway::files`는 같은 인증·권한 경계에서 VM별 `Files`를 다룬다. `Runtime::files`가 인스턴스 잠금·실행 상태를 확인하고, QEMU 어댑터가 QMP UUID·게스트 기능·요청 ID를 확인한 뒤 `IIVF1` 프레임을 전달한다. PID 1은 iiSDK worker에서 별도 `ii-files` 프로세스를 실행한다. 이 처리기는 Files만 데이터 마운트로 연결한 뒤 비특권 신원과 필수 syscall 필터를 적용하고 실제 파일을 연다. PID 1의 orphan 회수는 파일 작업의 Process 소유자가 종료 상태를 회수할 때까지 유예한다. 파일 저장·프레임·전송·계정 정책은 각각 교체 가능한 별도 책임이며 공개 요청과 오류 의미는 [Files API 계약](files-api.md)에 정의한다.

게이트웨이 권한은 VM 제어 API의 접근 정책이다. 호스트 OS 사용자와 SSH 접근 정책은 서버 운영자가 구성한다. 분산 스케줄링, 과금, VPC, 호스트별 격리와 라이브 마이그레이션은 별도 상위 계층의 책임이다. 중첩 가상화가 없는 iOS·브라우저에서는 호스트 실행 기능을 제공하지 않는다.

0.6.0의 `ProcessSupervisor`는 PID 1 EventLoop에서 iiSDK Process·비차단 Channel로 프로세스와 입출력을 소유한다. `ProcessPlatform`은 Linux cgroup·자원 관찰·신뢰한 보조 프로그램의 실행 구성을 주입한다. `ii-process`는 cgroup에 들어가고 기존 Files 전용 격리를 적용한 뒤 앱을 실행한다. 호스트의 typed 요청·프레임·백엔드 계약과 `VmClient`는 이 Linux 구현에 의존하지 않는다. 실행 키·보존 예산·커서·종료 회수 계약은 [프로세스 문서](processes-api.md)에 정의한다.
