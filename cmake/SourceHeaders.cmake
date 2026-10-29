function(iivirtualizer_forward_source_headers output_root source_root)
    foreach(_header Core/Types Runtime/Runtime Runtime/Qemu Protocol/GuestProtocol
            Files/Files Processes/Processes Gateway/Gateway Gateway/GatewayAgent Gateway/VmClient)
        get_filename_component(_name "${_header}" NAME)
        file(GENERATE OUTPUT "${output_root}/iiVirtualizer/${_name}.h"
            CONTENT "#pragma once\n#include \"${source_root}/${_header}.h\"\n")
    endforeach()
endfunction()
