# 동적 라이브러리 빌드

iiVirtualizer는 기본적으로 공유 라이브러리다. 정적 연결은
`BUILD_SHARED_LIBS=OFF`로 명시한다. 공개 CMake 타깃을 유지하고 Windows DLL,
iOS 프레임워크, WebAssembly 사이드 모듈 형식을 지원한다. 대상 플랫폼의
가상화 기능 자체가 존재하는지는 별도로 판별해야 한다.

연결과 프로토콜 테스트는 가상 머신이나 애플리케이션을 시작하지 않는다.
