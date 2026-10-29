# 게이트웨이의 VM Files API

0.5.0은 게이트웨이를 통해 실행 중인 VM의 `/data/Society.societycontainer/Files` 파일을 관리한다. 모든 경로는 이 디렉터리를 기준으로 하는 UTF-8 상대 경로이다. 계정 인증과 런타임·VM 권한 확인을 거친 뒤 게스트의 비특권 파일 처리기가 실제 작업을 수행한다. 호스트에서 게스트 RAW 볼륨을 마운트하지 않는다.

## 공개 계약

`Files.h`는 `FileAction`, `FileRequest`, `FileEntry`, `FileReply`를 제공한다. 로컬 제어에는 `Runtime::files(ID, request, timeout)`을 사용하고 원격 제어에는 `Gateway::files(runtimeID, instanceID, request, timeout)`을 사용한다. 기존 `Gateway::execute`와 JSON 호출에서도 같은 작업을 제공한다.

| 게이트웨이 action | FileAction | 동작 |
| --- | --- | --- |
| `file-list` | `list` | 직계 자식의 메타데이터를 이름 순서로 페이지 조회 |
| `file-stat` | `stat` | 항목 메타데이터 조회, 심볼릭 링크는 따라가지 않음 |
| `file-read` | `read` | 일반 파일의 `offset`부터 제한된 길이의 바이너리 내용 읽기 |
| `file-create` | `create` | 새 일반 파일 생성과 초기 내용 기록, 기존 대상이면 거부 |
| `file-write` | `write` | 기존 일반 파일의 지정 offset 수정, `truncate=true`이면 기록 끝으로 크기를 명시적으로 조정 |
| `file-mkdir` | `make_directory` | 한 단계의 새 디렉터리 생성, 부모는 이미 존재해야 함 |
| `file-move` | `move` | 파일·디렉터리·링크를 같은 Files 안에서 이동 또는 이름 변경, 대상이 있으면 거부 |
| `file-remove` | `remove` | 파일·링크 또는 빈 디렉터리 삭제, 링크 대상은 변경하지 않음 |

파일 작업은 요청당 VM 하나를 지정한다. 정지·일시정지한 VM은 `busy`로 반환한다. `Files` 루트의 빈 경로는 list·stat에서만 허용하며 루트 자체의 생성·이동·삭제는 거부한다. 중간 디렉터리와 내용 접근의 마지막 항목에 심볼릭 링크가 있으면 따라가지 않는다. 파일이 아닌 장치·FIFO·소켓에는 내용 읽기·쓰기를 수행하지 않는다. 디렉터리는 비재귀 삭제만 지원한다.

메타데이터에는 `path`, `name`, `kind`, `size`, `modified_ns`가 있다. kind는 `file`, `directory`, `symlink`, `other`이다. 극단적인 과거·미래 시각의 나노초 값은 int64 표현 범위로 제한한다. 목록에는 파일 본문을 넣지 않는다. 디렉터리 목록에서 받은 경로를 다음 조회에 사용하면 파일시스템의 실제 Unicode 이름 표현을 유지할 수 있다.

## C++ 사용

```cpp
#include <iiVirtualizer/Gateway.h>

namespace v = ii::virtualizer;
v::FileRequest request;
request.action = v::FileAction::create;
request.path = "notes/hello.txt"; // notes 디렉터리는 먼저 file-mkdir로 생성한다.
const auto content = ii::core::byte_view("Hello from iiOS\n");
request.data.assign(content.begin(), content.end());
auto created = gateway.files("primary", "account-a", request);

request = {};
request.action = v::FileAction::read;
request.path = "notes/hello.txt";
auto read = gateway.files("primary", "account-a", request);
if (read && read->items.front().result) {
    const auto& bytes = read->items.front().result->file->data;
    // bytes는 UTF-8 강제 변환을 하지 않는 iiSDKCore Bytes이다.
}
```

`GatewayResult` 오류는 요청·인증·권한·SSH·프로토콜 오류이다. 정상 게이트웨이 응답의 `items[0].result`는 VM 상태 또는 실제 파일 작업의 성공 여부이다. 파일 작업이 성공하면 그 안의 `file`에 `FileReply`가 있다. 파일 없음은 `not_found`, 충돌은 `already_exists`, 비어 있지 않은 디렉터리는 `busy`이다. 로컬 `Runtime::files`는 동일한 `core::Result<FileReply>`를 바로 반환한다. 기존 Backend 구현은 선택적인 `Backend::files` 함수를 주입하여 다른 전송으로 교체할 수 있다.

## JSON과 CLI

기존 SSH 클라이언트 구성과 계정 세션 공급 방법을 그대로 사용한다.

```sh
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-file-mkdir.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-file-create.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-file-list.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-file-read.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-file-write.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-file-move.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-file-remove.json
```

각 예제는 `account-a`의 `notes`를 사용한다. 내용은 `data_base64`로 전달하고 읽기 결과도 같은 필드를 사용한다. UTF-8 텍스트와 NUL을 포함하는 임의 바이너리 파일에 같은 형식을 사용한다.

```json
{
  "action": "file-read",
  "instances": ["account-a"],
  "file": {"path": "notes/hello.txt", "offset": 0, "length": 16384}
}
```

게이트웨이의 `file` 객체는 작업별 필드만 허용한다. `file-read`에 쓰기 데이터나 다른 작업을 주입하면 거부한다. 미지정 필드는 list의 `path=""`, `limit=16`, `include_hidden=false`, read의 `offset=0`, `length=16384`, write의 `offset=0`, `truncate=false`를 사용한다. create의 내용은 생략하면 빈 파일이다.

## 권한과 경계

`GatewayGrant.actions`에 파일 작업을 개별 지정한다. 읽기 전용 계정은 `file-list`, `file-stat`, `file-read`만 가진다. 같은 계정의 grant에 있는 런타임·VM ID 또는 prefix 범위도 함께 적용한다. 예제 서버 구성에는 전체 파일 작업 권한을 명시하였으며 실제 서비스에서는 계정 역할에 필요한 작업만 선택한다. 요청마다 iisacc 세션을 다시 검증하므로 취소된 세션의 파일 접근도 거부된다.

호스트 런타임은 기존 VM별 잠금을 잡고 실행 상태와 QMP UUID를 확인한다. virtio 제어 채널에서는 `files_api=1`을 확인하고 UUID·작업·요청 ID에 대응하는 `IIVF1` 프레임만 받는다. 구형 이미지는 `unsupported`로 반환하여 정지 후 이미지 갱신이 필요함을 알린다. 파일 경로·내용·명령을 셸 인수로 실행하지 않는다.

게스트 PID 1은 프레임을 검증한 뒤 iiSDK의 제한된 worker에서 `ii-files` 프로세스의 stdin으로 전달한다. 파일 I/O를 기다리는 동안 EventLoop와 제어 채널은 계속 동작한다. 파일 처리기는 PID 1의 자식인지 확인하고 별도 mount namespace·읽기 전용 실행 루트·UID/GID 1000·제거한 capabilities·no_new_privs·seccomp를 적용한 뒤에만 파일을 연다. 0.5.0의 워크로드와 파일 처리기는 **Files 디렉터리만** 데이터 마운트로 연결한다. 컨테이너의 관리 메타데이터와 다른 섹션은 작업 실행 루트에 노출하지 않는다.

파일 경로는 절대 경로, `.`·`..` 성분, 빈 성분, 끝의 `/`, 제어 문자·NUL, 역슬래시·콜론을 거부한다. 최대 1024 UTF-8 바이트, 성분당 최대 255바이트이다. 디렉터리 descriptor를 기준으로 각 성분을 [O_NOFOLLOW와 openat](https://man7.org/linux/man-pages/man2/open.2.html)으로 열고, [renameat2 RENAME_NOREPLACE](https://man7.org/linux/man-pages/man2/rename.2.html)로 이동 충돌을 원자적으로 거부한다. 호스트 계약 검사의 macOS 어댑터는 `renameatx_np(RENAME_EXCL)`을 사용한다. Files 밖과 inode를 공유하는 하드링크는 부팅 준비에서 거부하며 Files 내부 하드링크는 유지한다.

## 예산과 변경 의미

- 내용은 호출당 최대 16 KiB이다. 큰 파일은 offset을 증가시키며 여러 번 읽고 기록한다. offset 최대값은 16 TiB이며 쓰기 시작 위치는 현재 파일 끝을 넘길 수 없다.
- 목록은 호출당 최대 16개이다. `next_cursor`가 있으면 다음 list 요청의 cursor로 그대로 보낸다. `include_hidden=true`로 숨김 항목도 포함한다. 요청당 디렉터리 검사 예산은 10만 항목이며 초과하면 오류를 반환한다.
- 목록은 이름 순서이며 파일시스템의 고정 스냅샷이 아니다. 다른 프로그램이 항목을 변경하면 새 조회로 상태를 확인한다. 경로 규칙에 맞지 않는 기존 이름이 있으면 부분 목록 대신 오류를 반환한다.
- 생성·이동은 기존 대상을 덮어쓰지 않는다. 내용 수정은 지정한 범위에 청크 단위로 반영된다. 같은 파일을 워크로드가 동시에 수정하는 경우 응용 프로그램이 협조하여 충돌을 조정해야 한다.
- 성공한 변경은 파일과 필요한 부모 디렉터리를 동기화한다. I/O 오류·프로세스 중단·전송 제한 시간 초과에는 일부 변경이 이미 반영되었을 수 있다. 자동 재시도하지 않으며 stat·read로 확인한 뒤 재개한다. 생성 중 실패한 파일도 남을 수 있다.
- 게스트 프레임 최대값은 48 KiB, 게이트웨이 요청은 기존 64 KiB이다. 계정 증명과 파일 payload의 합도 게이트웨이 프레임 제한 안에 있어야 한다.

## 실제 검증

새 `ii-files`를 포함한 iiOS 시험 이미지와 Gateway 지원 빌드를 전용 Linux 컨테이너의 볼륨에 준비한다. 기존 정지 VM은 `tools/upgrade_guest.py`로 같은 커널의 initramfs를 갱신한다. 저장소 루트가 아닌 Files 내부만 노출하는 변경도 새 이미지에 포함된다. 다음 VM 시험은 그 컨테이너 안에서만 실행한다.

```sh
python3 -B tests/gateway_ssh.py --gateway build/iivirtualizer-gateway \
  --runtime build/iivirtualizer --root resources/platform-guests/files-tests --vms --files \
  --openssl /usr/bin/openssl --qemu /usr/bin/qemu-system-aarch64 \
  --image resources/platform-guests/iios-arm --accel tcg
```

`iiVirtualizer.Files`는 실제 디렉터리에서 CRUD·바이너리 청크·페이지·충돌·경로·링크 교체 경쟁을 검사한다. `iiVirtualizer.Gateway.Files`는 C++ 게이트웨이·권한·VM 범위·취소·게스트 프레임 대응을 검사한다. 실제 VM 시험은 OpenSSH와 TLS 합성 계정 서버, 독립 VM 두 개를 사용하며 재부팅 영속성·읽기 전용 역할·실행 상태·외부 경로 거부를 확인한다. 운영 계정 세션이나 외부 물리 호스트의 배포 증거와 구분한다. 결과는 [검증 기록](validation.md)에 추가한다.
