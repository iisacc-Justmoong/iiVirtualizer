# iisacc 계정 기반 SSH 게이트웨이

`Gateway`는 계정 세션 공급자와 SSH 전송기를 조합하여 다른 머신의 런타임을 제어한다. 수신 측 `GatewayAgent`는 iisacc 계정 서버에 현재 세션을 확인한 뒤 subject·런타임·VM·작업별 권한을 검사한다. 이메일·표시 이름·캐시된 Account 객체만으로 인증하지 않는다.

기본 인증 기관은 `https://iisacc.com/Account/GraphQL`의 읽기 전용 `accountSession` query이다. 현재 신원 쿠키 `iisacc_auth_id` 또는 `iisacc_auth_linked`와 `iisacc_login_session`을 사용한다. refresh token·비밀번호는 전달하지 않는다. 요청마다 서버 확인을 수행하여 취소된 세션이 다음 제어 요청에 재사용되지 않도록 한다. TLS 인증서 검증과 SSH 호스트 키 검증을 모두 유지하며 리디렉션을 따라 인증 정보를 전달하지 않는다.

SSH 호스트 인증은 OpenSSH의 identity 파일 또는 agent를 사용한다. iisacc 세션은 SSH 채널 안에서 게이트웨이의 애플리케이션 권한을 인증한다. 계정 비밀번호에서 SSH 개인키를 만들지 않는다. 전송은 고정된 `iivirtualizer` SSH subsystem의 stdin/stdout이며 PTY·agent forwarding·port forwarding·로컬 명령·사용자 SSH 설정을 비활성화한다. `known_hosts`는 명시적으로 지정한다. 세션은 요청 단위 메모리에서만 다루고 argv·환경 변수·응답·로그·평문 자격 증명 파일에 저장하지 않는다.

계정 주체의 `GatewayGrant`는 런타임 ID, 작업 목록, VM ID 또는 prefix, 인스턴스당 생성 자원 한도를 명시한다. 기본 권한은 거부이다. 여러 VM 중 하나라도 권한 밖이면 실행 전에 요청 전체를 거부한다. 허용된 요청 내부의 VM 실행 오류는 VM별 결과로 반환하며 성공한 다른 VM은 유지한다.

원격 생성은 서버의 부팅 이미지 카탈로그 ID만 받는다. 클라이언트가 커널·initramfs·저장소 root·셸 명령·실행 인수를 지정할 수 없다. 서로 다른 런타임을 명시적으로 등록하여 같은 SSH 연결 대상에서도 여러 VM 저장소를 제어할 수 있다.

iiOS 0.4.0 이미지는 원격으로 생성한 VM에도 동일한 [society-v1 파일시스템 경계](filesystem.md)를 적용한다. 서버 운영자는 이미지 카탈로그의 부팅 복사본을 갱신하고, 이미 생성한 정지 VM은 별도의 안전한 이미지 갱신 절차를 사용한다. 게이트웨이에 경계를 끄는 요청 필드는 없다.

0.5.0의 [VM Files API](files-api.md)는 같은 인증 경로에서 파일 목록·메타데이터·내용 읽기·생성·수정·디렉터리 생성·이동·삭제를 제공한다. `file-*` 권한을 개별 지정하며 요청당 VM 하나의 Files 상대 경로만 받는다.

네이티브 계정 저장소 연동은 `secure_account_session(service, key)`로 명시적으로 고른 iiAccountManager Native SecureStore 항목을 읽는다. 항목 형식은 `version=1`, `issuer`, `subject`, `cookie`를 갖는 JSON이다. 앱이 인증된 현재 세션으로 이 항목을 관리하거나, 메모리상의 세션을 반환하는 공급자를 직접 주입한다. 다른 앱·브라우저의 저장소를 검색하지 않으며 기존 Qt 계정 SDK의 refresh 전용 저장 레코드를 자동으로 변환하지 않는다. Linux 네이티브 보안 저장소 미지원 시 평문 파일로 대체하지 않는다.

요청과 응답은 버전·request ID·런타임 ID·계정 subject를 확인한다. 자동 전송 재시도는 하지 않는다. 전송이 종료되거나 제한 시간을 넘기면 원격 작업이 일부 완료되었을 수 있으므로 상태를 조회한 뒤 판단한다. SSH 권한은 서버에서 subsystem 또는 제한된 forced command로 연결해야 하며 일반 셸 접근 정책은 OpenSSH 설정이 담당한다.

## 구성과 실행

게이트웨이는 기본 활성화된다. iiNetwork·iiAccountManagerNative·OpenSSL 3 이상이 필요하다. 필요하지 않은 소비자는 `-DIIVIRTUALIZER_BUILD_GATEWAY=OFF`로 호스트 VM 런타임만 빌드할 수 있다. macOS의 keg-only OpenSSL 설치는 `OPENSSL_ROOT_DIR` 또는 CMake prefix로 지정한다. `install-native.sh`는 설치된 Homebrew OpenSSL 경로도 확인한다.

1. 서버에서 `example/gateway-server.json`의 런타임 root·실행기·부팅 이미지 경로와 `YOUR_IISACC_SUBJECT`를 실제 값으로 바꾼다. subject는 iisacc 계정 서버가 반환한 `sub`이며 이메일·표시 이름을 대신 넣지 않는다.
2. `example/gateway-sshd.conf`를 참고하여 전용 SSH OS 사용자를 고정 게이트웨이 프로그램에 연결한다. 이 저장소의 빌드는 시스템 SSH 설정이나 `~/.ssh/authorized_keys`를 자동으로 수정하지 않는다.
3. 클라이언트의 `example/gateway-client.json`에 호스트·SSH 사용자·검증한 known_hosts·identity 또는 선택한 키를 가진 SSH agent·런타임 ID·계정 subject·보안 저장소 항목을 지정한다. 저장소·이미지·인증서·known_hosts·identity의 상대 경로는 해당 구성 파일의 디렉터리를 기준으로 해석한다. QEMU·SSH 실행기는 절대 경로나 PATH의 프로그램 이름으로 지정한다.
4. 앱의 활성 세션을 공급자 또는 Native SecureStore를 통해 전달한다. `call`은 기본적으로 선택한 보안 저장소 항목을 읽는다. 시험·프로세스 연결용 `--session-stdin`은 쿠키 헤더를 비공개 파이프로 받고 파일·argv·환경 변수에 저장하지 않는다.

```sh
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-list.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-create.json
build/iivirtualizer-gateway call example/gateway-client.json example/gateway-start.json
```

예제는 실제 계정·호스트 설정을 포함하지 않는 템플릿이다. account-b도 생성한 뒤 시작 예제를 실행한다. macOS 대상은 이미지의 accelerator를 `hvf`로 지정한다. 수신 프로그램의 직접 실행 형식은 `iivirtualizer-gateway agent SERVER_CONFIG`이며 SSH subsystem이 이 프로그램을 실행한다. 공개 API는 호출 완료까지 대기하므로 `GatewayAgent::handle`은 iiSDK worker/HostLoop 밖의 수신 스레드에서 호출한다. 클라이언트 `Gateway::execute`는 필요하면 앱의 iiSDK Concurrent 작업으로 제출할 수 있다.

## C++ 객체 조합

소비자는 `Gateway.h`와 `GatewayAgent.h`를 포함하고 `iiVirtualizer::iiVirtualizer`에 링크한다. 기존 계정 앱은 현재 세션을 반환하는 `GatewayCredentials` 함수를 주입할 수 있다. 다음은 명시적으로 선택한 Native SecureStore 항목을 사용하는 클라이언트이다.

```cpp
namespace v = ii::virtualizer;
v::SshGatewayEndpoint endpoint;
endpoint.host = "vm-host.example.com";
endpoint.user = "virtualizer";
endpoint.known_hosts = trusted_known_hosts_absolute_path;
endpoint.identity_file = selected_ssh_identity_absolute_path;
v::Gateway gateway(
    v::secure_account_session("com.iisacc.virtualizer.account-session", "primary-session"),
    v::ssh_gateway_transport(endpoint));

v::GatewayRequest request;
request.runtime = "primary";
request.action = v::GatewayAction::start;
request.instances = {"account-a", "account-b"};
auto reply = gateway.execute(request);
// reply 오류는 인증·권한·전송·프로토콜 오류이고,
// 성공 응답의 각 item.result는 해당 VM 작업의 성공 여부이다.
```

수신 앱은 기존 `Runtime`들을 `GatewayRuntime`에 등록하고, `iisacc_account_verifier()` 및 계정별 `GatewayGrant`를 `GatewayAgent`에 주입한다. 이미지 카탈로그의 `InstanceSpec`은 서버가 소유하는 부팅 경로를 지정한다. 등록한 Runtime의 수명은 agent보다 길어야 한다. 별도 머신마다 같은 수신 객체를 설치하고 클라이언트의 endpoint를 바꾸면 같은 호출 계약으로 제어할 수 있다.

프로토콜은 요청 최대 64 KiB·응답 최대 2 MiB·일괄 VM 최대 128개이다. VM 종료 대기 시간은 요청의 `timeout`으로 최대 60초이며, SSH 전체 통신 시간은 endpoint의 `timeout`으로 별도 지정한다. 생성 자원 한도는 VM별 한도이다. 전체 계정의 누적 자원 할당량과 분산 스케줄링은 상위 운영 계층에서 관리한다. 요청 ID는 응답 대응을 확인하는 값이며 재실행 방지 저장소로 사용하지 않는다.

## 검증

`iiVirtualizer.Gateway`는 계정 바인딩·권한·자원 한도·이미지 카탈로그·다중 VM·응답 상관관계·SSH 인수 경계를 검사한다. `iiVirtualizer.Gateway.Ssh`는 임시 키·별도 포트의 OpenSSH 서버와 TLS 계정 시험 서버를 실제로 연결한다. 시스템의 SSH 서비스·기존 키·실제 계정 저장소를 변경하지 않는다. 시험용 개인키는 종료 시 삭제한다.

실제 Linux VM을 포함하는 시험은 Gateway 지원 빌드와 iiOS 시험 이미지를 전용 Linux 컨테이너의 볼륨에 준비한 뒤 그 컨테이너 안에서 실행한다. 다음 경로는 컨테이너 내부의 예이다.

```sh
python3 -B tests/gateway_ssh.py --gateway build/iivirtualizer-gateway \
  --runtime build/iivirtualizer --root resources/platform-guests/gateway-tests --vms \
  --qemu /usr/bin/qemu-system-aarch64 \
  --image resources/platform-guests/iios-arm --accel tcg
```

이 검증의 계정 서버는 TLS로 보호한 합성 세션 fixture이다. 운영 iisacc 사용자 로그인이나 외부 물리 머신 배포의 증거로 대체하지 않는다. 운영 API에는 익명 GraphQL 요청을 보내 HTTP 200과 `account: null` 및 GraphQL 오류 부재를 별도로 확인했다.

0.3.0의 실제 결과와 각 실행의 원시 증거 경로는 [검증 기록](validation.md)의 게이트웨이 항목과 `build/verification-gateway.json`에 기록한다. 릴리스·ASan/UBSan·TSan 빌드에 연결된 기존 iiSDK 동적 라이브러리는 설치본을 사용하며 이번 검사에서 해당 의존 라이브러리 전체를 재계측한 것은 아니다.

전송 구성은 OpenSSH의 [SSH subsystem](https://man.openbsd.org/ssh.1), [호스트 키 검증](https://man.openbsd.org/ssh_config.5), [ForceCommand와 DisableForwarding](https://man.openbsd.org/sshd_config.5) 계약을 따른다.

## 앱 런타임의 프로세스와 모니터링

0.6.0의 `Gateway::processes`와 `VmClient`는 실행·중지·표준 입출력·종료 상태·자원 감시를 제공한다. `process-*`와 `vm-monitor`를 기존 grant에 개별 지정하며 세션·런타임·VM 범위를 동일하게 적용한다. 작업 목록·stdin 접수 위치·출력 cursor·Files 실행 파일·cgroup 트리 회수와 한도는 [프로세스 API](processes-api.md)에 있다. JSON 예제는 `example/gateway-process-*.json`과 `example/gateway-monitor.json`이다.
