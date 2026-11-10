# 빌드와 가상 머신 리소스

기본 `build/`는 iiVirtualizer 모듈, CLI·게이트웨이·게스트 빌드 에이전트와
활성화된 테스트의 컴파일 산출물에 사용한다. 예제는
`IIVIRTUALIZER_BUILD_EXAMPLE=ON`으로 명시할 때만 빌드한다.
기본 설치 경로는 `~/.local/SDK/iiVirtualizer`이며 CMake 또는 설치 스크립트에
다른 prefix를 명시할 수 있다. 설치 파일은 기본적으로 `build/`에 두지 않는다.

게스트 이미지와 다운로드는 `resources/images/`, VM 상태 기본값은
`resources/vm/`, 게스트 빌드 VM 상태는 `resources/gb/`, 기존 OS 부팅 검증의
VM 상태는 `resources/ob/`, QEMU 및 다른 외부 도구는 `resources/dependencies/`에 둔다. 짧은 VM 루트는 macOS의 Unix 소켓 경로 제한을 고려한다. 이전 상태와 이미지,
게스트 빌드 결과는 삭제하지 않고 이 리소스 트리로 이전한다. 예전 절대 경로를
참조하는 설치본과 메타데이터를 위해 기존 경로에 리소스 링크를 둔다. macOS와
Linux의 최상위 `build/` 구성은 필요한 기존 링크를 다시 만들고, 다른 파일이
같은 이름을 차지하면 덮어쓰지 않고 중지한다. Windows에서는 기존 링크를 자동
생성하지 않으며 새 프로그램의 기본 경로는 직접 `resources/`를 사용한다.
과거 `build/os-boot`와 `build/build-layer` 경로도 이 호환 링크로 이어진다.
이 두 기록 영역의 최상위에 있던 재생성 가능한 `.log`와 pip 캐시는 정리했으며,
과거 검증 문서의 로그 경로가 현재 남아 있다는 뜻은 아니다. VM 디스크·이미지와
기존 작업 소스는 보존한다.

테스트는 iiFileProvider Python 어댑터로 `resources/.t/`에 각자 고유한
작업 공간을 만들고 성공과 실패 시 정리한다. 짧은 경로는 macOS의 Unix 소켓
경로 길이 제한을 지키기 위한 것이다. VM 부팅 검증은 기존 정책대로 전용
Linux 컨테이너의 TCG에서만 선택적으로 수행한다. 경로 검사와 모의 계약 테스트의
성공은 실제 VM 실행 증거가 아니다.
