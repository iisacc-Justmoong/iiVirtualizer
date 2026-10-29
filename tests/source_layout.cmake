if(NOT SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()
if(EXISTS "${SOURCE_ROOT}/src/iiVirtualizer")
    message(FATAL_ERROR "The source tree must not repeat the project name")
endif()
foreach(_file Core/Types.h Core/Types.cpp Runtime/Runtime.h Runtime/Runtime.cpp
        Runtime/Qemu.h Runtime/Qemu.cpp Protocol/GuestProtocol.h Protocol/GuestProtocol.cpp
        Files/Files.h Files/Files.cpp Processes/Processes.h Processes/Processes.cpp
        Gateway/Gateway.h Gateway/Gateway.cpp Gateway/VmClient.h Gateway/VmClient.cpp)
    if(NOT EXISTS "${SOURCE_ROOT}/src/${_file}")
        message(FATAL_ERROR "Missing source in its responsibility directory: ${_file}")
    endif()
endforeach()
file(READ "${SOURCE_ROOT}/src/guest/CMakeLists.txt" _guest)
if(NOT _guest MATCHES "iiJson::iiJson" OR _guest MATCHES "src/iiJson/Json.cpp")
    message(FATAL_ERROR "Guest protocol must use the iiJson target, not an internal source path")
endif()
