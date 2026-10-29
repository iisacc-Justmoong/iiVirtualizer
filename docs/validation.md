# 검증 기록

검증일은 2026-10-25이다. 아래 첫 표는 초기 0.1.0 검증이며, 0.2.0 다중 VM·0.3.0 게이트웨이·0.4.0 파일시스템 경계 검증은 뒤에 별도로 기록한다. 실제 호스트는 Apple Silicon ARM64 macOS이다. QEMU 11.1.2를 프로젝트 `build/dependencies/`에 빌드했고, 게스트는 Linux 6.12.114 ARM64와 정적 링크한 iiSDKCore PID 1이다. 호스트는 기존 설치본 iiSDKCore 0.18.1과 iiJson을 사용한다.

| 검증 | 결과 | 증거 |
| --- | --- | --- |
| 호스트 Release 계약·CLI·패키징 | 3/3 통과 | `build/host-tests.xml` |
| AddressSanitizer + UndefinedBehaviorSanitizer | 3/3 통과 | `build/asan/build/asan-tests.xml` |
| 별도 설치 패키지 소비자 | 1/1 통과 | `build/consumer/build/installed-tests.xml` |
| ARM64 HVF 하드웨어 가상화 | 실제 부팅·서비스·종료 통과 | `build/verification/s-hvf-f39c4a/result.json` |
| ARM64 TCG CPU 에뮬레이션 | 실제 부팅·서비스·종료 통과 | `build/verification/s-tcg-1136a3/result.json` |

두 실행기에서 각각 2 vCPU·384 MiB 메모리·64 MiB 디스크를 할당하여 다음을 확인했다.

1. Linux가 `/init`을 PID 1로 실행한다.
2. iiSDKCore의 worker 2개가 실제 작업 4개를 완료하고 EventLoop·QueryPort를 통해 게스트 정보에 응답한다.
3. ext4 데이터 볼륨이 `/data`에 마운트된다.
4. 게스트 DHCP·사용자 모드 NAT·호스트 loopback 포트 전달을 통해 실제 HTTP 서비스를 읽는다.
5. 일시정지 상태를 QMP로 조회하고 재개한다.
6. 게스트가 정상 종료한 후 동일 디스크로 다시 부팅한다. 게스트 정보와 HTTP 응답 모두 `boot=1`에서 `boot=2`로 변한다.
7. 강제 종료 후 정지를 확인하고 시험 인스턴스를 삭제한다. 원시 콘솔은 결과 디렉터리에 보존한다.

실제 게스트 응답에는 `os=iiOS runtime=iiSDKCore pid=1 cpus=2 workers=2 tasks=4 volume=mounted boot=2 workload=running`이 포함되었다. HTTP 응답은 `iiOS workload via iiSDKCore; boot=2`였다.

호스트 테스트는 경로 탈출·유효하지 않은 자원 지정·중복 인스턴스 거부, 다른 프로세스 6개의 동시 생성 경쟁, 실패한 시작의 재시도, 영속 UUID, 활성 VM 삭제 거부, 게스트 프로토콜 경계, 분할된 QMP 프레임·비동기 이벤트·응답 ID·UUID 검증, 응답 전 연결이 닫히는 `quit`, QMP를 잃었으나 소유 PID가 존재하는 상태의 삭제 거부를 포함한다. 패키징 테스트는 newc 항목·모드·링크·재현 가능한 압축과 아카이브 경로 경계를 확인한다.

Linux/KVM 호스트 실행은 현재 macOS에서 실증하지 않았다. Windows의 영속 QEMU 어댑터와 x86_64 게스트 부팅도 검증 범위 밖이다. x86_64 QEMU 실행기는 빌드되어 있으나 해당 게스트 이미지 작성기는 제공하지 않는다. 이 모듈은 단일 호스트 제어와 iiSDK 게스트 실행을 제공하며 분산 클라우드 운영, 다중 사용자 권한 격리, 과금, VPC, 라이브 마이그레이션을 구현하지 않는다.

호스트·게스트·외부 QEMU는 각각 별도의 빌드·검증 경계이다. 모의 백엔드 테스트나 게스트 교차 컴파일을 하드웨어 부팅 증거로 사용하지 않는다. 최종 요약과 부팅 이미지 해시는 `build/verification.json`과 `build/images/aarch64/image.json`에서 확인한다.

## 0.2.0 — 하나의 런타임에서 여러 VM 실행

`Runtime` 객체 하나를 유지하는 C++ 제어 프로세스에서 Linux VM 두 개를 동시에 실행하였다. 두 VM은 각각 1 vCPU·256 MiB와 2 vCPU·384 MiB를 사용하며 디스크는 각각 64 MiB이다. 동일한 게스트 HTTP 포트 8080을 서로 다른 호스트 포트로 전달하였다.

| 검증 | 결과 | 증거 |
| --- | --- | --- |
| Release 계약·다중 VM·CLI·패키징 | 4/4 통과 | `build/multi-host-tests.xml` |
| AddressSanitizer + UndefinedBehaviorSanitizer | 4/4 통과 | `build/asan/build/multi-asan-tests.xml` |
| ThreadSanitizer | 4/4 통과 | `build/tsan/build/multi-tsan-tests.xml` |
| 갱신한 설치 패키지의 별도 C++ 소비자 | 1/1 통과 | `build/consumer/build/multi-installed-tests.xml` |
| ARM64 HVF 동시 VM 2개 | 실제 부팅과 격리 검증 통과 | `build/multi-verification/hvf-a59d57/result.json` |
| ARM64 TCG 동시 VM 2개 | 실제 부팅과 격리 검증 통과 | `build/multi-verification/tcg-90d9cb/result.json` |

HVF 제어 프로세스 PID는 74332, 두 QEMU PID는 74711과 74710이었다. TCG 제어 프로세스 PID는 75458, QEMU PID는 75488과 75487이었다. 각 제어 프로세스에는 VM Runtime 객체 하나와 제어용 worker 4개만 생성하였다. PID 값은 해당 검증 실행의 기록이며 현재 프로세스 상태를 뜻하지 않는다.

두 서비스에 동시 접속한 뒤 A의 일시정지·재개·종료 중에도 B의 HTTP 응답과 QEMU PID가 유지됨을 확인했다. B의 호스트 포트를 재사용하는 C의 기동 실패와 A의 재기동을 같은 일괄 작업에 넣었을 때 C만 실패하였다. A는 같은 UUID·디스크로 `boot=2`가 되었고 B는 `boot=1`로 계속 응답하였다. 이후 시험 VM은 모두 정지·삭제하였고 콘솔과 원시 이벤트를 보존하였다.

다중 VM 계약 테스트는 다른 VM 기동의 실제 중첩, VM별 프로세스 잠금, 작업 중인 VM의 삭제 거부, 오류 행을 유지하는 snapshot, 개별 실패·입력 순서·중복 ID 사전 거부, 잘못된 executor에서의 무부작용 거부, 포화 시 Future 종결, queue 용량 1에서 더 큰 일괄 요청 처리, Runtime 파괴 시 실행 작업의 수명과 대기 Future 종결을 확인한다.

최신 집계는 `build/verification-multi.json`이다. 게스트 커널과 이미지의 프로토콜은 기존 IIV1을 사용하며 이번 변경은 호스트 런타임의 다중 인스턴스 제어에 한정된다. Linux/KVM·Windows에 관한 앞의 실증 범위 제한은 유지된다.

## 0.3.0 — iisacc 계정과 SSH 원격 게이트웨이

`Gateway` 클라이언트와 `GatewayAgent` 수신 객체를 추가하고 iiAccountManager Native·iiNetwork·OpenSSL을 연결하였다. 설치본의 공개 헤더와 CMake 의존성을 통해 별도 C++ 소비자를 빌드하였다. `build/install/bin/iivirtualizer-gateway`와 호스트 CLI는 모두 0.3.0이다.

| 검증 | 결과 | 증거 |
| --- | --- | --- |
| Release 전체 계약·다중 VM·CLI·스토리지·SSH/TLS | 7/7 통과 | `build/gateway-full-tests.xml` |
| AddressSanitizer + UndefinedBehaviorSanitizer 전체 재검사 | 7/7 통과 | `build/asan/build/gateway-asan-recheck.xml` |
| ThreadSanitizer 전체 | 7/7 통과 | `build/tsan/build/gateway-tsan-tests.xml` |
| 설치한 SDK의 별도 C++ 소비자 | 1/1 통과 | `build/consumer/build/gateway-installed-tests.xml` |
| 게이트웨이 제외 빌드와 전체 기존 계약 | 5/5 통과 | `build/off/build/gateway-off-tests.xml` |
| 실제 SSH + TLS 계정 시험 서버 + ARM64 HVF VM 2개 | 27회 호출과 개별 VM 제어 통과 | `build/gr/b41d0b/result.json` |
| 운영 iisacc GraphQL 익명 읽기 | HTTP 200·account null·GraphQL 오류 없음 | `build/gateway-production-anonymous.json` |

실제 OpenSSH 서버는 같은 물리 호스트의 별도 loopback 포트에서 전용 시험 키와 forced command로 실행하였다. 수신 객체는 TLS로 보호한 합성 계정 서버에 요청마다 세션을 검증하였다. 계정 subject 위조, 다른 런타임·VM 접근, 생성 자원 한도 초과, 세션 취소, 데이터와 함께 반환된 GraphQL 오류, TLS 인증서 불일치, SSH 호스트 키 불일치를 거부하였다. 잘못된 호스트 키에서는 계정 proof가 전송되지 않았다. 다른 계정에 허용된 두 번째 런타임의 생성·목록·삭제도 확인하였다.

실제 VM은 각각 1 vCPU·256 MiB·64 MiB와 2 vCPU·384 MiB·64 MiB를 사용하였다. SSH 요청으로 생성·동시 기동한 뒤 두 게스트의 iiSDK PID 1과 HTTP 서비스에 응답을 받았다. A의 일시정지·정상 종료 중 B가 계속 응답하였고, A의 재기동 후 A는 `boot=2`, B는 `boot=1`이었다. 계정 세션을 취소한 상태의 종료 요청은 거부되었으며 이미 실행 중인 두 VM의 서비스는 유지되었다. 실행 중인 VM의 삭제도 거부하였다. 시험 VM은 마지막에 정지·삭제하고 원시 콘솔을 남겼다.

첫 ASan 실행과 CLI 단독 재검사에는 잘못된 경로를 거부하는 프로세스의 15초 제한 시간 초과가 있었다. 동일 입력의 별도 실행은 올바른 오류로 종료하였고, 제한 시간과 소스를 바꾸지 않은 전체 재검사는 7/7 통과하였다. 초기 결과 `gateway-asan-tests.xml`·`gateway-asan-cli-recheck.xml`과 별도 실행 기록은 보존하였다. 게이트웨이 제외 첫 시험의 긴 root는 macOS AF_UNIX 길이 제한에 걸렸으며 `build/off/build`의 짧은 root에서 5/5 통과하였다.

계측 대상은 iiVirtualizer의 소스와 실행 파일이다. 연결한 기존 iiSDK 동적 라이브러리 전체를 이번 검사에서 재계측하지 않았다. 게스트 부팅 이미지는 앞서 검증한 Linux·iiSDK 이미지를 재사용하였다. 운영 사용자 세션의 인증 성공, 외부 물리 머신 배포, Linux/KVM 원격 호스트는 이번 실증 범위 밖이다. 운영 API 검사는 익명 조회만 수행하고 계정 데이터를 생성·수정하지 않았다.

시험의 SSH·TLS 개인키는 제거하였으며 기존 `demo` 인스턴스의 UUID·자원·정지 상태를 보존하였다. 시스템 SSH 구성·사용자의 SSH 키·계정 Keychain을 변경하지 않았다. 최신 집계는 `build/verification-gateway.json`이며, 0.1.0과 0.2.0 결과 파일도 유지한다.

## 0.4.0 — Society 컨테이너 내부의 작업 파일 제한

읽기 전용 실행 루트·전용 mount namespace·UID/GID 1000·권한 제거·필수 seccomp를 구성하는 `ii-workload`를 추가하였다. `ii-init`은 워크로드를 직접 실행하지 않는다. iiSocietyContainer의 Qt 독립 DirectoryStorage로 `/data/Society.societycontainer`를 준비하고 작업·HOME·임시 파일을 그 안의 Files로 제한한다. 구현 계약은 [파일시스템 문서](filesystem.md)에 있다.

| 검증 | 결과 | 증거 |
| --- | --- | --- |
| 호스트 Release 전체 | 9/9 통과 | `build/workspace-final-host-tests.xml` |
| 호스트 ASan + UBSan 전체 | 9/9 통과 | `build/asan/build/workspace-asan-tests.xml` |
| 호스트 TSan 전체 | 9/9 통과 | `build/tsan/build/workspace-tsan-tests.xml` |
| 0.4.0 설치 패키지 소비자 | 1/1 통과 | `build/consumer/build/workspace-installed-tests.xml` |
| ARM64 HVF 실제 파일시스템 제한 | 부팅마다 56개, 2회 부팅 통과·실패 구성 3종 거부 | `build/fs-verification/hvf-cf6005/result.json` |
| ARM64 TCG 실제 파일시스템 제한 | 부팅마다 56개, 2회 부팅 통과·실패 구성 3종 거부 | `build/fs-verification/tcg-817a70/result.json` |
| 제한된 일반 HTTP 워크로드의 HVF 수명주기 | 서비스·재부팅 영속성·일시정지·종료 통과 | `build/verification/s-hvf-9274bc/result.json` |
| 새 이미지로 SSH·TLS 경유 VM 2개 | 27회 호출·독립 제어·재부팅 통과 | `build/gr/e13c43/result.json` |
| 기존 demo 갱신·부팅·정지 | 동일 UUID·자원·서비스 유지 | `build/workspace-demo-validation.json` |

실제 Linux 검증은 컨테이너 내부의 파일 생성·읽기·이동·삭제·하드링크·임시 파일과 iiSDK worker·Process를 확인하였다. OS 파일 변경·삭제·권한 변경, 상대 경로와 심볼릭 링크 탈출, 외부 하드링크, `/dev/vda`·`/dev/mem`·`/proc/1/root` 접근, 마운트와 새 마운트 API, chroot·user namespace·clone 우회, 장치 생성·파일 핸들·외부 descriptor·ptrace·콘솔 ioctl·root 복구를 거부하였다. 읽기 전용 루트, `nosuid,nodev` 작업 마운트, 상속 descriptor 정리와 자식의 보호 상속도 실제 커널에서 검사하였다.

두 번째 부팅에서는 Files에 남긴 값이 보존되었다. 이어 HOME을 일부러 외부 우회 링크로 바꾼 뒤 재부팅하자 워크로드가 실행되지 않았다. launcher를 제거한 이미지와 잘못된 파일시스템을 가진 데이터 볼륨에서도 실행을 거부하였고 제어 채널로 VM을 종료하였다. 각 부팅의 원시 콘솔을 별도 보존하였다.

호스트 패키지 검사는 구현 전 실패한 `build/workspace-red.xml`에서 시작하여 정상 패키지·기존 파일 보존·위장 경로·외부 하드링크·내부 하드링크·신뢰 메타데이터 경계를 검사하였다. 이미지 갱신 검사는 동일 커널·정지·잠금·백업·원자적 교체·반복 실행·UUID/볼륨 해시 보존과 실행 중 VM·링크·불일치 커널의 거부를 검사하였다. 이미지 작성기는 정적 ELF·CPU 아키텍처·동적 인터프리터·잘린 실행 파일을 검사한다.

기존 demo는 이전 initramfs와 콘솔을 `build/guest-upgrades/20261025T093453-demo-fbb71a/`에 보존한 뒤 갱신하였다. 이미지 교체 직후 `data.raw` SHA-256은 `94d2f42e35188b7ef3bdae83ce69d3682b7cb78c0bec0c4ab509471501965865`로 동일하였다. 새 정책으로 부팅한 후 기존 횟수를 이어 `boot=2`와 HTTP 응답을 확인하고 정지하였다. 새 컨테이너와 부팅 메타데이터 생성에 따른 디스크 기록은 이 실제 부팅의 정상 동작이다. UUID·자원·manifest는 유지하였다.

초기 HVF 검사 한 번은 콘솔 교체를 이어쓰기처럼 읽는 시험 스크립트 문제로 중단되어 부팅별 수집을 수정하였다. 이후 HVF·TCG 동시 실행 중 한 HVF 종료가 기본 10초 제한을 넘겼으나, 같은 이미지·같은 제한의 HVF 단독 전체 재검사는 통과하였다. 두 초기 결과도 `build/fs-verification/hvf-f4645e/`·`hvf-5e73cf/`에 유지하였다. 보호를 해제하거나 거부 조건을 완화하지 않았다.

호스트 sanitizer 결과를 게스트 전체의 계측 결과로 간주하지 않는다. 게스트 제한은 별도 실제 Linux 커널 검사이며, 이번 실제 실행 아키텍처는 aarch64이다. SSH 계정 서버는 앞 절과 동일한 TLS 합성 세션 fixture이다. 최신 집계와 이미지·소스 해시는 `build/verification-filesystem.json`에 기록한다.

## 0.5.0 — 게이트웨이의 VM Files API

`Gateway::files`·`Runtime::files`와 여덟 개의 파일 action을 추가하였다. 공개 계약은 [Files API](files-api.md)에 있다. 파일 내용은 비특권 `ii-files`에서만 처리하며, 워크로드와 파일 처리기의 데이터 마운트는 Files 자체로 좁혔다. 컨테이너의 관리 메타데이터를 연결하는 하드링크는 부팅 준비에서 거부한다.

| 검증 | 결과 | 증거 |
| --- | --- | --- |
| Release 전체 계약·스토리지·파일·게이트웨이·SSH/TLS | 11/11 통과 | `build/files-host-tests.xml` |
| ASan + UBSan 전체 | 11/11 통과 | `build/asan/build/files-asan-final-tests.xml` |
| 같은 옵션으로 빌드한 iiJson 의존성 | 2/2 통과 | `build/asan/json/build/json-asan-tests.xml` |
| TSan 전체 | 11/11 통과 | `build/tsan/build/files-tsan-tests.xml` |
| 0.5.0 설치 패키지의 별도 C++ 소비자 | 1/1 통과 | `build/consumer/build/files-installed-tests.xml` |
| 게이트웨이 제외 빌드 | 8/8 통과 | `build/off/build/files-off-tests.xml` |
| 실제 SSH·TLS + ARM64 HVF VM 2개 | 93회 호출 통과 | `build/gf/1f7212/result.json` |
| 실제 SSH·TLS + ARM64 TCG VM 2개 | 92회 호출 통과 | `build/gf/55ce03/result.json` |
| HVF 커널 파일시스템 경계 | 부팅마다 56개·2회 부팅·실패 구성 3종 통과 | `build/fs-verification/hvf-2acec8/result.json` |
| TCG 커널 파일시스템 경계 | 부팅마다 56개·2회 부팅·실패 구성 3종 통과 | `build/fs-verification/tcg-f4b6a4/result.json` |
| 기존 demo 갱신·실제 부팅·정지 | 같은 UUID·자원·boot=3·Files 기능·HTTP 확인 | `build/files-demo-validation.json` |

실제 SSH 시험은 새 디렉터리·한국어 이름·NUL을 포함하는 바이너리 파일의 생성·청크 읽기·추가 기록·부분 수정·명시적 truncate·조회·이동·삭제를 수행하였다. 16,902바이트 왕복 내용의 SHA-256은 두 가속기에서 모두 `a841d88121288a42108025659e2d120c0face53b330cd5babd973de51f7d0baa`였다. 생성·이동 충돌에서 기존 내용 보존, 정렬한 제한 페이지, 비어 있지 않은 디렉터리의 삭제 거부, sparse gap과 초과 청크 거부도 확인하였다.

읽기 전용 계정의 쓰기 거부, 다른 계정·VM 범위의 거부, 요청 action과 payload의 불일치 거부, 세션 취소와 경로 탈출 거부를 확인하였다. 두 VM의 같은 상대 경로에 서로 다른 내용을 기록한 뒤 A의 일시정지·정지 중에도 B의 파일과 HTTP가 유지되었고, A 재부팅 후 각 내용이 보존되었다. 시험 요청 수는 guest 준비 조회 횟수를 포함한 해당 실행의 기록이다.

호스트 파일 계약은 실제 디렉터리에서 링크 메타데이터와 링크 자체 삭제, 중간 링크·내용 링크의 거부, 디렉터리 교체 경쟁을 검사한다. 게이트웨이 계약은 작업별 권한과 VM 범위, 바이너리 인코딩, UUID·요청 ID·작업·응답 경로 대응을 검사한다. 초기 실패 `build/files-red.xml`과 페이지 반복 검사를 통해 구현과 디렉터리 stream offset 공유 문제를 수정하였다.

초기 TCG 시험 `build/gf/0a1c99/result.json`은 게스트 제어 채널 준비 직후 별도 HTTP 워크로드의 listen이 완료되기 전 접속하여 실패하였다. 최초 부팅·재부팅의 읽기 전용 HTTP 조회만 제한 시간 안에서 기다리도록 시험을 수정하였다. 이어 `build/gf/bcbfcd/result.json`에서 파일 오류 응답이 일반 작업자 실패로 바뀌는 경합을 확인하였다. PID 1의 orphan 회수가 다른 iiSDK worker의 Process 종료 상태를 먼저 회수하던 문제였으며, 파일 Process 소유자의 회수가 완료될 때까지 orphan 회수를 유예하였다. 최종 HVF·TCG 시험은 이 수정이 포함된 같은 PID 1 이미지로 수행하였다. 파일 변경 요청의 재시도나 권한 완화는 추가하지 않았다.

최초 ASan 전체 검사 `build/asan/build/files-asan-tests.xml`은 10/11이었다. SSH 수신 실행 파일을 빈 입력으로 별도 실행해도 기존 비계측 iiJson과 계측된 STL의 조합에서 container-overflow가 재현되었다. [Sanitizers의 부분 계측 설명](https://github.com/google/sanitizers/wiki/AddressSanitizerContainerOverflow)에 따라 iiJson 소스를 `build/asan/json/`에서 같은 ASan·UBSan 옵션으로 빌드·검사·설치하고 iiVirtualizer를 그 패키지에 다시 연결하였다. 이후 전체 11/11이 통과하였다. 컨테이너 검사·누수 검사·오류 탐지를 끄지 않았으며, 다른 SDK 설치본은 덮어쓰지 않았다. 기존 iiSDK 동적 의존성 전체와 Linux 게스트 전체를 재계측한 결과로 확대 해석하지 않는다.

demo의 이전 initramfs·메타데이터·콘솔은 `build/guest-upgrades/20261025T110816-demo-06f8de/`에 보존하였다. 교체 전후 데이터 디스크 SHA-256은 `ac2f4b84c31d04d674fca0e6dfc43296f47e6be5f0db19f22c817db9f88f1560`로 동일하였으며 UUID와 자원 설정도 보존하였다. 이후 실제 부팅의 정상 메타데이터 기록을 거쳐 `boot=3`, `files_api=1`, 기존 HTTP 서비스를 확인하고 정지하였다.

SSH·TLS 시험은 같은 물리 호스트의 전용 loopback 서버와 합성 계정 세션을 사용하였다. 두 원격 시험 런타임의 권한을 확인하였으며 운영 계정 데이터는 생성·수정하지 않았다. 외부 물리 머신 배포와 Linux/KVM 호스트의 실증은 범위 밖이다. 시험 VM은 모두 정지·삭제하고 시험 개인키는 제거하였으며 demo는 보존하였다. 최신 집계·소스·실행 파일·이미지 해시는 `build/verification-files-api.json`에 기록한다. 앞 버전의 검증 기록도 유지한다.

## 0.6.0 — 앱 런타임의 프로세스 제어와 상세 감시

`Processes.h`, `Runtime::processes`, `Gateway::processes`, `VmClient`를 추가하였다. 게스트는 iiSDK Process·비차단 Channel을 단일 EventLoop에서 소유하고, Linux adapter가 전용 cgroup·자원 감시·격리 launcher를 연결한다. 실제 실행과 입출력·종료·커서·자원 계약은 [프로세스 API](processes-api.md)에 정의한다.

| 검증 | 결과 | 증거 |
| --- | --- | --- |
| Release 전체 | 13/13 통과 | `build/process-host-bootstrap-tests.xml` |
| ASan + UBSan 전체 | 13/13 통과 | `build/asan/build/process-asan-bootstrap-tests.xml` |
| TSan 전체 | 13/13 통과 | `build/tsan/build/process-tsan-bootstrap-tests.xml` |
| 0.6.0 설치 패키지의 별도 소비자 | 1/1 통과 | `build/consumer/build/process-installed-bootstrap-tests.xml` |
| 게이트웨이 제외 빌드 | 9/9 통과 | `build/off/build/process-off-bootstrap-tests.xml` |
| 실제 SSH·TLS + ARM64 HVF VM 2개 | 200회 호출 통과 | `build/gp/2f4424/result.json` |
| 실제 SSH·TLS + ARM64 TCG VM 2개 | 200회 호출 통과 | `build/gp/159f15/result.json` |
| HVF 기존 파일시스템 경계 | 부팅마다 56개·2회 부팅·실패 구성 3종 통과 | `build/fs-verification/hvf-d78bf1/result.json` |
| TCG 기존 파일시스템 경계 | 부팅마다 56개·2회 부팅·실패 구성 3종 통과 | `build/fs-verification/tcg-5667e4/result.json` |
| demo 이미지 교체·부팅·정지 | 같은 UUID·자원·boot=5·파일/프로세스 기능·HTTP 확인 | `build/process-demo-bootstrap-validation.json` |

구현 전 `build/process-red.xml`은 프로세스 감독기의 미구현 오류로 실패하였다. 네이티브 테스트는 바이너리 양방향 I/O·독립 stderr·EOF·종료 코드·종료 시간 고정·중복 실행 키·실패한 spawn의 자원/등록 회수·실행 시간 제한·동시 실행 16개와 보존 32개·페이지·출력 초과 구간·해제·프레임 대응을 검사한다. 별도 게이트웨이 테스트는 VmClient의 typed 반환·오류 계층·작업별 권한·VM 범위·세션 취소·일시정지·다른 VM의 응답·기존 파일 API를 확인한다.

실제 SSH 시험은 각각 독립 Linux VM 두 개에서 프로세스를 실행하고, 별도 SSH 호출 사이에 같은 핸들·입출력 상태가 유지됨을 확인하였다. stdin 16,397바이트를 여러 청크로 접수하고 EOF를 보낸 뒤 stdout와 stderr에서 동일한 바이너리와 코드 7을 받았다. SHA-256은 두 가속기에서 `ef58c1a090a091c667d75df21ef426c8debfa5cc967d695ab7a226e35da1cf8b`였다. 256 KiB씩 출력하는 앱은 두 스트림 각각 최근 64 KiB와 명시적 gap을 반환하였다. 실행 키의 재호출·충돌, 이전 stdin 위치의 재접수 거부, 수명 만료, 실행 중 해제 거부도 확인하였다.

등록 앱과 Files에서 읽어 seal한 앱이 각각 비특권 코드로 실행되었다. UID·no_new_privs·seccomp·OS 파일·원시 장치·cgroup 제어 경로·권한 복구·링크 탈출·Files 내부 기록 등 12개 검사를 통과하였다. 잘못된 ELF와 링크 작업 디렉터리는 코드 125로 종료하였다. 별도 세션으로 분리한 자식은 process-stop에서 정지하였고, 루트가 먼저 종료한 경우에도 자식의 파일 기록이 더 진행되지 않음을 확인하였다. CPU 사용량·메모리·task 수와 VM 단위의 구조화된 자원 정보도 실제 커널에서 조회하였다.

A의 일시정지·정지·재부팅과 세션 취소 중에도 B의 프로세스 핸들과 서비스는 유지되었다. A 재부팅에서는 새 boot_id가 생기고 이전 핸들은 not_found, 보존 개수는 0이었다. 읽기 계정의 중지·다른 VM 접근, 절대/탈출 실행 경로·예약 환경·NUL 인수를 거부하였다. 기존 Files CRUD·바이너리·권한·재부팅 영속성 시험도 같은 실행에 포함하였다. 최초 HVF 실행 `build/gp/bf02d6/result.json`의 200회 호출도 보존하였으며, 최종 실행은 등록 메모리를 spawn 전에 준비하고 실패 시 회수하는 보강을 포함한다.

최종 경계 검토에서는 앱의 환경변수를 권한 있는 보조 프로그램에 직접 전달하던 경로를 분리하였다. `ProcessBootstrap`이 앱 환경을 내부 키의 bounded JSON payload로 전달하고, 보조 프로그램은 Files 격리·UID 전환 후에만 앱 환경으로 적용한다. 네이티브 테스트는 GLIBC_TUNABLES 같은 앱 설정 키가 보조 프로그램의 실제 환경에 없음을 검사한다. 최종 실제 VM 시험은 APP_VALUE·GLIBC_TUNABLES·LANG을 포함한 앱 환경과 새 이미지를 사용하였다. 보강 전의 201회·202회 검증과 첫 demo 부팅 기록도 별도로 유지하였다.

첫 ASan·TSan 전체 검사는 각각 12/13이었다. 상속 환경을 비운 네이티브 시험 프로그램의 sanitizer runtime이 PATH 부재에 따른 symbolizer 경고 159바이트를 stderr 앞에 출력하여 정확한 바이너리 비교가 실패하였다. 별도 실행에서 stdout 16,384바이트는 동일하고 stderr만 16,543바이트임을 확인하였다. 시험 launcher에 `/usr/bin:/bin`을 명시하자 두 스트림이 모두 정확히 일치하였으며 전체 재검사는 각각 13/13 통과하였다. `build/process-sanitizer-fixture-diagnostics.json`과 최초 XML을 보존한다. 경고를 필터링하거나 sanitizer·컨테이너·누수 검사를 끄지 않았고, Linux 앱에 호스트 PATH를 전달하지 않았다. ASan은 앞 절의 동일 계측 iiJson 패키지를 사용하며 다른 기존 iiSDK 동적 라이브러리 전체와 게스트 전체를 계측한 결과는 아니다.

일반 이미지 `build/images/aarch64`는 시험 카탈로그가 없는 0.6.0이다. 프로세스 시험 프로그램은 별도 `build/images/processes`, 공격성 파일시스템 시험은 `build/images/sandbox`에 분리하였다. demo의 이전 이미지·메타데이터·콘솔은 `build/guest-upgrades/20261025T141132-demo-7eeea6/`에 보존하였다. 이미지 교체 직전·직후 데이터 디스크 SHA-256은 `6c8bf2596ab5957f5a9b15414e4640f92811c582d35cd509a69408fd5c4a7d69`로 동일하였다. 이후 정상 부팅 메타데이터 기록을 거쳐 boot=5와 기존 HTTP를 확인하고 정지하였다.

계정 서버는 TLS 합성 세션 fixture이고 SSH 서버는 같은 물리 호스트의 전용 loopback 포트이다. 운영 계정 인증 성공·외부 물리 호스트 배포·Linux/KVM 또는 Windows 호스트의 실행 증거로 확대하지 않는다. 시험 VM·개인키는 정리하고 demo·기존 SDK 설치본은 보존하였다. 최신 집계와 소스·공개 헤더·실행 파일·이미지 해시는 `build/verification-processes.json`이다.

## 2026-10-28 — 격리된 Linux 컴파일러 및 프로토콜 회귀 검사

전용 컨테이너의 Debian ARM64, GCC 14와 C++23으로 iiSDKCore, iiJson, iiFileProvider Native 및 iiVirtualizer를 빌드하였다. GCC의 경고를 오류로 취급하는 설정에서 Files 및 Processes 프레임 처리의 조건문과 초기화를 명확하게 고쳤다. 새 `iiVirtualizer.WireContracts` 시험은 이진 파일 요청과 UUID 바인딩, 비정규 base64 거부, 프로세스 실행 요청·응답 바인딩 및 예약 환경변수 거부를 검사한다. 컨테이너 내부 CTest는 16/16 통과했으며 기록은 `build/container-linux-ctest-complete.log`이다. 이 구성은 게이트웨이와 게스트 빌더 에이전트를 끄고 빌드한 것이므로 그 구성요소나 Windows의 실행 검증으로 확대하지 않는다. 실제 컨테이너 격리와 게스트 실행의 범위는 [컨테이너 시험](container-testing.md)에 따로 기록한다.

Windows x86 기동 증거 판별과 시험 프로세스 중단 처리 계약을 추가한 뒤 같은 컨테이너에서 다시 빌드하고 CTest 17/17이 통과했다. 기록은 `build/container-linux-ctest-x86-update.log`이다. 별도의 작은 컨테이너에서는 `--init`이 고아 프로세스를 실제로 회수하는 것을 확인했고, `build/container-init-smoke.log`와 `build/container-init-smoke-inspect.json`에 결과를 남긴 다음 해당 시험 컨테이너를 정리했다. 이 검사들은 Windows 게스트 부팅 성공을 뜻하지 않는다.

같은 변경을 macOS 호스트에서도 `build/`에 다시 빌드한 후 SSH 실행 시험을 제외한 CTest 21개 중 20개가 통과했다. TPM 실프로세스 시험 한 개는 격리된 Linux 컨테이너에서만 실행하도록 의도적으로 건너뛰었다. 결과는 `build/container-task-host-ctest-latest.log`이며, 이 호스트 검사에는 실제 게스트 부팅이 포함되지 않는다.
