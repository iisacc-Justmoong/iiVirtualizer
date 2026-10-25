# 검증 기록

검증일은 2026-10-25이다. 실제 호스트는 Apple Silicon ARM64 macOS이다. QEMU 11.1.2를 프로젝트 `build/dependencies/`에 빌드했고, 게스트는 Linux 6.12.114 ARM64와 정적 링크한 iiSDKCore PID 1이다. 호스트는 기존 설치본 iiSDKCore 0.18.1과 iiJson을 사용한다.

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
