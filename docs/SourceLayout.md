# 소스 책임 분류

호스트 코드는 `src/Core`, `src/Runtime`, `src/Protocol`, `src/Files`, `src/Processes`, `src/Gateway`로 나누며 헤더와 구현을 함께 둔다. Linux 게스트 프로그램은 기존대로 `src/guest/`에 있다.

빌드 forwarding 헤더와 설치 패키지는 `<iiVirtualizer/...>` 공개 경로를 유지한다. 게스트 프로토콜은 형제 iiJson 소스의 CMake 타깃 `iiJson::iiJson`에 의존하며 내부 구현 파일 경로를 직접 열거하지 않는다. `tests/source_layout.cmake`는 소스 배치와 이 의존성 계약을 검사한다.

테스트 시 실제 VM 시작은 프로젝트의 컨테이너 규칙을 따른다. 호스트 단위·모의 테스트와 실제 게스트 빌드 및 부팅 결과는 구분한다.
