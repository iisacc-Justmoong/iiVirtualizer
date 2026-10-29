# 파일 생성 경계

호스트의 저장소·스테이징·잠금 파일·커널과 initramfs 복사·RAW 디스크·manifest 공개는 기본적으로 iiFileProvider Native API를 거친다. `--clone-base`를 명시한 macOS 생성만 예외로, 인스턴스 전용 스테이징 디렉터리에 APFS 등의 copy-on-write 복제를 먼저 시도한다. 실패하거나 다른 OS이면 기존 제공자 복사로 돌아간다. 게스트가 복제본에 쓴 내용은 원본 이미지에 반영되지 않지만, 파일시스템은 원본의 ctime을 갱신할 수 있다. 기존 비차단 flock, O_NOFOLLOW, 디렉터리 권한, manifest fsync 및 완성된 디렉터리의 이름 교체 계약은 유지한다.

Linux 게스트도 마운트 지점과 boot-count를 동일한 제공자로 생성한다. 게스트 CMake는 Native 제공자를 정적으로 포함하며, 대상용 SQLite 소스를 함께 빌드한다. 교차 컴파일 도구 체인은 C와 C++ 컴파일러·대상·sysroot를 모두 지정해야 한다. 런타임에 Qt나 별도 인터프리터를 추가하지 않는다.

0.4.0에서는 작업 폴더·HOME·임시 파일과 부팅 메타데이터를 `.societycontainer` 내부에 생성한다. 제공자의 API를 사용하지 않는 직접 파일 호출도 [게스트 파일시스템 경계](filesystem.md)의 읽기 전용 루트·권한·seccomp 정책을 따른다.

0.5.0의 파일 처리기는 iiFileProvider Native의 descriptor 기반 열기·디렉터리 생성 API를 사용한다. POSIX의 `fstatat`, `pread`·`pwrite`, `renameat2`와 `unlinkat`는 같은 descriptor 경계 안에서 메타데이터·청크·원자적 충돌 거부·링크 자체 삭제를 구현한다. 실제 내용 I/O 전에 Files만 데이터 마운트로 연결하고 권한과 syscall을 제한한다. 공개 요청·한도·부분 기록의 의미는 [Files API](files-api.md)에 정의한다.

`iiVirtualizer.ProviderStorage`는 한국어 상위 경로, 원본 보존 복사, RAW 볼륨 크기, 중복 생성 거부와 파일로 막힌 상위 경로를 검사한다. 게스트 부팅 검사는 빌드 검사와 별도로 수행한다.
