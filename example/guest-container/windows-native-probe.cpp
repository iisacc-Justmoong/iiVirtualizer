#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cwctype>

#if defined(_M_ARM64) || defined(__aarch64__)
constexpr WORD expected_machine = PROCESSOR_ARCHITECTURE_ARM64;
constexpr const char* binary_architecture = "aarch64";
#elif defined(_M_X64) || defined(__x86_64__)
constexpr WORD expected_machine = PROCESSOR_ARCHITECTURE_AMD64;
constexpr const char* binary_architecture = "x86_64";
#else
#error Unsupported Windows probe architecture
#endif

int main(int argc, char** argv) {
    const bool winpe = argc == 3 && std::strcmp(argv[2], "--winpe") == 0;
    if ((argc != 2 && !winpe) || std::strlen(argv[1]) != 32) return 2;
    for (const char* p = argv[1]; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 2;

    SYSTEM_INFO machine{};
    GetNativeSystemInfo(&machine);
    if (machine.wProcessorArchitecture != expected_machine) return 3;
    auto ntdll = GetModuleHandleW(L"ntdll.dll");
    using VersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto version_function = reinterpret_cast<VersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (!version_function || version_function(&version) != 0 || version.dwMajorVersion < 10) return 4;
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length < 3 || length >= MAX_PATH || system_directory[1] != L':' || system_directory[2] != L'\\') return 5;
    const auto drive = static_cast<char>(std::towupper(system_directory[0]));
    if (drive < 'A' || drive > 'Z' || (drive == 'X') != winpe) return 6;

    char report[512]{};
    const int count = std::snprintf(report, sizeof(report),
        "IIV_WINDOWS_PROBE_BEGIN\nos=Windows\nnative_arch=%s\nbinary_arch=%s\n"
        "major=%lu\nminor=%lu\nbuild=%lu\nsystem_drive=%c\nnonce=%s\nIIV_WINDOWS_PROBE_END\n",
        binary_architecture, binary_architecture, static_cast<unsigned long>(version.dwMajorVersion),
        static_cast<unsigned long>(version.dwMinorVersion), static_cast<unsigned long>(version.dwBuildNumber),
        drive, argv[1]);
    if (count < 0 || static_cast<std::size_t>(count) >= sizeof(report)) return 7;
    HANDLE serial = CreateFileW(L"\\\\.\\COM1", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (serial == INVALID_HANDLE_VALUE) return 8;
    DCB settings{};
    settings.DCBlength = sizeof(settings);
    if (GetCommState(serial, &settings)) {
        settings.BaudRate = CBR_115200;
        settings.ByteSize = 8;
        settings.Parity = NOPARITY;
        settings.StopBits = ONESTOPBIT;
        settings.fOutxCtsFlow = FALSE;
        settings.fOutxDsrFlow = FALSE;
        settings.fOutX = FALSE;
        settings.fInX = FALSE;
        SetCommState(serial, &settings);
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(serial, report, static_cast<DWORD>(count), &written, nullptr);
    CloseHandle(serial);
    if (!ok || written != static_cast<DWORD>(count)) return 9;
    std::fwrite(report, 1, static_cast<std::size_t>(count), stdout);
    return 0;
}
