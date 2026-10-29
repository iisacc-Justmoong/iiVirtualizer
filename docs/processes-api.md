# 앱 런타임의 VM 프로세스 제어와 모니터링

0.6.0은 기존 iisacc 계정·SSH 게이트웨이와 VM별 Files 정책 위에 프로세스 API를 제공한다. 앱 런타임은 `VmClient`를 런타임 ID·VM ID에 바인딩하고 VM 상태·자원·관리 프로세스를 조회하거나 실행·중지·입출력을 수행한다. 관리 프로세스의 소유자는 게스트 PID 1의 `ProcessSupervisor`이다. SSH 연결이나 앱의 C++ 클라이언트 객체가 사라져도 프로세스는 게스트에 남는다.

## 공개 인터페이스

`Processes.h`는 표준 타입과 iiSDKCore Bytes·Result 기반의 요청·스냅샷 계약이다. 로컬은 `Runtime::processes`, 원격은 `Gateway::processes`를 사용한다. `VmClient`는 게이트웨이의 한 VM에 대한 가벼운 참조 객체이며 자체 스레드·주기 타이머·전역 캐시를 만들지 않는다. 호출은 제한 시간이 있는 동기 RPC이므로 앱이 iiSDK executor와 폴링 주기를 소유한다. 같은 Gateway가 VmClient보다 오래 살아 있어야 한다.

| 게이트웨이 action | 동작 |
| --- | --- |
| `process-launch` | 실행 키에 바인딩된 새 관리 프로세스 실행 |
| `process-list` | 관리 프로세스 메타데이터를 핸들 순서로 페이지 조회 |
| `process-stat` | 상태·입출력 위치·자원·종료 코드와 signal 조회 |
| `process-read` | stdout 또는 stderr의 지정 절대 바이트 위치부터 읽기 |
| `process-write` | stdin 큐에 바이너리 청크 접수 |
| `process-close-input` | 이미 접수한 입력을 전달한 뒤 stdin 닫기 |
| `process-stop` | 소유한 프로세스 트리를 SIGKILL로 중지 |
| `process-release` | 완전히 종료·회수한 프로세스 기록과 버퍼 해제 |
| `vm-monitor` | 구조화된 게스트 자원·감독기 상태 조회 |

`VmClient`에는 `launch`, `process`, `list`, `read`, `write`, `close_input`, `stop`, `release`, `monitor`, `files`, `describe`, `control`이 있다. 프로세스 중지는 `stop(handle)`이며 VM 수명주기는 `control(Operation::pause/resume/stop/...)`이다. `VmResult<T>`의 오류는 `variant<GatewayError, core::Error>`로, 인증·전송 오류와 VM이 반환한 작업 오류를 구분한다. 낮은 단계의 GatewayReply는 기존처럼 `items[0].result`에서 작업 결과를 구분한다.

```cpp
#include <iiVirtualizer/VmClient.h>
namespace v = ii::virtualizer;

v::VmClient vm(gateway, "primary", "account-a");
// bin/application은 먼저 Files에 배포한 이 게스트 아키텍처의 정적 ELF이다.
// work는 Files API로 미리 만든 디렉터리이다.
v::ProcessLaunch launch;
launch.key = "job-001";
launch.executable = "bin/application";
launch.working_directory = "work";
launch.arguments = {"--batch"};
launch.memory_mib = 128;
auto started = vm.launch(launch);
if (started) {
    const auto id = started->id;
    auto accepted = vm.write(id, 0, ii::core::byte_view("input\n"));
    auto closing = vm.close_input(id);
    auto output = vm.read(id, v::ProcessStream::output, 0);
    auto status = vm.process(id);
    // 이후 read의 next_offset으로 재개한다. 종료 후에도 EOF까지 조회한다.
}
auto guest = vm.monitor();  // 작은 자원·기능·개수 스냅샷
auto jobs = vm.list();      // 필요한 시점에 프로세스별 상세 메타데이터 조회
auto host = vm.describe(); // 일시정지·정지 VM의 호스트 상태도 조회 가능
```

## 실행과 수명

`ProcessLaunch.key`는 호출자가 정한 최대 64자의 영숫자·`-`·`_` 실행 키이다. 같은 VM 부팅에서 키와 실행 옵션이 같으면 기존 핸들을 반환한다. 같은 키의 다른 옵션은 충돌로 거부한다. 응답을 잃었을 때 같은 요청으로 기존 실행을 찾을 수 있으며, 신규 작업에는 새 키를 사용한다. release 후에는 그 키도 해제된다. VM 재부팅은 새로운 관리 세션이며 이전 핸들과 실행 키 기록을 이어받지 않는다.

프로세스 핸들은 부팅 난수와 순번으로 구성한다. PID는 관찰용 메타데이터이며 제어 대상으로 받지 않는다. PID 1·파일 처리기·기존 부팅 워크로드 등 관리 핸들이 없는 프로세스를 임의로 중지하는 API는 제공하지 않는다. VM별 ID·상태·QMP UUID와 게스트 UUID·요청 ID·작업·결과 핸들을 확인한다. 일시정지·정지 VM의 게스트 작업은 `busy`, 구형 이미지의 미지원은 `unsupported`이다.

실행 응답은 감독기의 실행 접수와 보조 프로세스 생성을 뜻한다. Files 실행 파일 읽기·격리·실제 exec 단계가 실패하면 프로세스는 코드 125로 종료하고 원인은 stderr에 남는다. 실제 앱의 준비 상태는 앱 프로토콜과 표준 출력·상태로 확인한다. 일반 종료는 `exited`와 code·signal, API 중지는 `stopped`, 지정한 수명 만료는 `timed-out`으로 구분한다. 종료 시 경과 시간과 최종 상태는 고정하고 출력은 보존한다.

## 실행 파일과 Files 경계

`@service`는 이미지의 기존 `/app/service`, `@이름`은 이미지 작성 시 `--program 이름=정적ELF`로 등록한 `/app/programs/이름`을 선택한다. 카탈로그 이름은 경로 구분자가 없는 검증된 키이며 카탈로그는 최대 16개이다. 그 외 executable과 working_directory는 `.societycontainer/Files` 기준 상대 경로이다. 셸 명령 문자열이나 PATH 탐색으로 실행하지 않고 argv를 그대로 전달한다.

등록 프로그램은 신뢰하는 이미지의 일반 파일만 읽기 전용 실행 루트에 연결한다. Files 실행 파일은 먼저 기존 mount namespace·읽기 전용 루트·Files 전용 데이터 마운트·UID/GID 1000·capabilities 제거·no_new_privs·필수 seccomp를 적용한 뒤 descriptor 기반 Files API로 읽는다. 최대 32 MiB의 현재 아키텍처 ELF64·정적 프로그램만 허용하고 동적 인터프리터·스크립트·링크 경로를 거부한다. 읽은 바이트는 [memfd](https://man7.org/linux/man-pages/man2/memfd_create.2.html)에 담고 쓰기·크기 변경을 seal로 막은 뒤 [execveat](https://man7.org/linux/man-pages/man2/execveat.2.html)로 실행한다. 실행 권한을 부여하려고 원본 파일을 chmod하거나 OS 경로에 복사하지 않는다. 동시에 원본을 기록하면 유효하지 않은 실행 이미지로 실패할 수 있으므로 앱 배포는 새 파일과 no-replace 이동으로 조정한다.

작업 디렉터리는 Files root descriptor에서 각 성분을 O_NOFOLLOW로 열어 들어간다. 앱 환경의 키·값은 보조 프로그램의 환경변수로 직접 넘기지 않고 해석 기능이 없는 전용 키의 bounded JSON payload로 전달한다. 권한 있는 보조 프로그램의 libc 시작 단계에 앱의 loader·allocator·locale 설정이 적용되지 않도록 하기 위함이다. 보조 프로그램은 허용한 내부 키만 받고 payload를 검증하며, Files 격리·권한 제거가 끝난 뒤 환경을 비우고 앱의 명시적인 키·값을 적용한다. HOME·TMPDIR·PATH·IIV_*·LD_*·DYLD_*는 앱 요청에서 덮어쓰지 못한다. HOME·TMPDIR은 Files의 `.home`·`.tmp`이다. 이 디렉터리가 삭제되어 있어도 프로세스 제어기가 외부 임시 경로로 우회하지 않는다. 앱에 필요한 디렉터리는 Files API로 준비한다. 기존 파일 API와 파일시스템 거부 조건은 유지한다.

## 프로세스 트리와 자원

게스트는 CPU·memory·pids controller가 있는 cgroup v2와 `cgroup.kill`을 필수로 요구한다. 실행 보조 프로그램은 앱 코드를 실행하기 전에 PID 1이 준비한 전용 cgroup에 스스로 들어간다. cgroup 경로와 제어 descriptor는 앱 실행 루트에 노출하지 않는다. [cgroup의 자식 상속과 tree kill](https://docs.kernel.org/admin-guide/cgroup-v2.html)에 따라 새 세션·프로세스 그룹으로 분리된 자식도 같은 트리에 남으며, 중지·루트 종료·수명 만료·VM 종료에서 정리한다. iiSDK Process 소유자도 직접 자식을 회수한다. PID 1의 일반 orphan 회수는 이 소유권을 침범하지 않는다.

기본 메모리 상한은 64 MiB, task 상한은 64개이다. memory_mib는 16 이상이고 VM 메모리를 넘지 않아야 하며 pids는 1~256이다. 자식과 스레드도 cgroup 예산을 공유한다. memory.oom.group을 설정하여 해당 트리의 OOM 종료를 함께 처리한다. lifetime_ms는 기본 0으로 시간 제한이 없고, 지정하면 최대 24시간이다. 이 제한은 실행 접수부터 계산하며 느린 준비 단계도 포함한다. cgroup을 준비할 수 없으면 프로세스 실행을 거부한다.

`ProcessSnapshot`에는 CPU 누적 사용량 `cpu_usec`, cgroup에 청구된 현재·최대 메모리, 현재 task 개수, stdin 접수·기록·폐기 바이트, 출력 보존 범위와 EOF, 생성·경과 시간, 종료 코드와 signal이 있다. `started_ms`는 해당 부팅의 감독기 생성 시점 기준 단조 증가 밀리초이며 Unix 시각이 아니다. `elapsed_ms`는 실행 접수 후 경과 시간이다. cgroup 메모리는 단일 PID의 RSS와 의미가 다르며 자식과 파일 캐시를 포함할 수 있다. 종료 후 보존한 값은 마지막 관찰 값이다.

`VmMonitor`에는 부팅 ID·uptime·vCPU 수·전체/가용 메모리·CPU 누적 total/idle ticks·clock ticks·감독기 sequence·실행/보존 개수·iiSDK worker 수와 pending 수·파일/프로세스 기능 가용성이 있다. 출력 본문은 포함하지 않는다. 앱은 두 스냅샷의 증가분으로 사용률을 계산하며, 부팅 ID가 바뀌거나 증가분이 0이면 이전 구간과 연결하지 않는다. 기존 부팅 워크로드는 관리 프로세스 개수에 포함하지 않으며 `guest-info`로 실행 상태를 확인한다.

## 입출력과 예산

- 한 요청은 VM 하나를 대상으로 한다. VM당 동시 실행 16개, 종료 기록을 포함한 보존 32개이며 완료 기록은 명시적으로 release한다.
- stdin·stdout·stderr의 바이너리 전송 청크는 최대 16 KiB, 목록은 최대 16개이다. JSON은 canonical Base64를 사용한다. 게스트 프레임은 최대 48 KiB, 게이트웨이 요청은 기존 64 KiB이다.
- stdout와 stderr는 각각 최근 64 KiB를 독립적으로 보존하고 지속해서 drain한다. 오래된 커서를 요청하면 `truncated=true`와 새 offset을 반환한다. `next_offset`으로 재개하며, EOF는 해당 스트림의 마지막 보존 바이트까지 조회한 경우에만 반환한다. 출력이 많은 앱 때문에 무제한 메모리를 사용하거나 전체 제어 채널이 출력 대기로 멈추지 않는다.
- stdin 큐는 64 KiB이다. write의 offset은 현재 `input_accepted`와 정확히 같아야 하며 큐 부족은 부분 접수 없이 `busy`이다. 접수는 자식의 소비 완료를 뜻하지 않는다. `input_written`은 파이프 기록량이고, 조기 종료로 큐에서 폐기한 양은 `input_discarded`에 남긴다. close-input은 접수한 큐 뒤에서 실행하는 장벽이다.
- argv는 최대 64개, 환경은 최대 32개이며 인수·환경의 합은 최대 8 KiB이다. NUL·유효하지 않은 UTF-8·경로 탈출·예약 환경 재정의는 거부한다.
- 프로세스별 스레드를 만들지 않는다. 게스트 EventLoop의 비차단 Channel 펌프는 tick당 전체 64 KiB 예산과 순환 순서로 동작한다. 파일 작업은 기존 제한된 iiSDK worker를 사용한다.
- 전송 실패나 제한 시간 초과가 발생하면 작업이 접수되었을 수 있다. 실행은 같은 key, 입력은 stat의 input_accepted, 출력은 절대 커서로 상태를 확인한다. 변경을 무조건 반복하지 않는다. stdin의 이전 offset 재전송은 중복 접수로 거부한다.

## JSON과 빌드

```json
{"action":"process-launch","instances":["account-a"],"process":{"key":"job-001","executable":"bin/application","working_directory":"work","arguments":["--batch"],"memory_mib":128}}
```

```json
{"action":"process-read","instances":["account-a"],"process":{"id":"RETURNED_HANDLE","stream":"stderr","offset":0,"length":16384}}
```

```json
{"action":"vm-monitor","instances":["account-a"]}
```

`GatewayGrant.actions`에 필요한 action을 각각 부여한다. 모니터링·메타데이터 조회·로그 읽기·입력·중지·실행 권한은 별개이다. 매 요청마다 기존 iisacc 세션 검증과 런타임·VM 범위가 적용된다. 읽기 전용 계정이 payload의 작업을 바꾸어 쓰기나 중지를 요청하면 거부한다.

```sh
cmake --build build/guest/build --target ii-init ii-workload ii-files ii-process ii-guest-service --parallel 4
python3 -B tools/prepare_guest.py --init build/guest/build/ii-init --workload build/guest/build/ii-guest-service
# 필요한 자체 앱을 카탈로그에 넣을 때 --program app=경로/정적ELF를 추가한다.
python3 -B tools/upgrade_guest.py VM_ID
```

등록 시험 프로그램 `ii-guest-process-probe`는 opt-in 검증 이미지에서만 `--program probe=...`로 넣는다. 소스·native 계약·설치 소비자·실제 Linux/SSH 결과는 [검증 기록](validation.md)에 구분한다. 운영 계정 세션과 외부 물리 호스트의 검증 범위도 별도로 기록한다.
