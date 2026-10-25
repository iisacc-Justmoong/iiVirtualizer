#include "GuestProtocol.h"

namespace ii::virtualizer {
Result<GuestCommand> guest_request(std::string_view line,std::string_view uuid) {
    if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
    const std::string prefix="IIV1 "+std::string(uuid)+" ";
    if(line.size()>4096||!line.starts_with(prefix))return core::fail(core::ErrorCode::invalid_argument,"Guest request protocol or UUID mismatch");
    line.remove_prefix(prefix.size());
    if(line=="info")return GuestCommand::info;
    if(line=="ping")return GuestCommand::ping;
    if(line=="shutdown")return GuestCommand::shutdown;
    return core::fail(core::ErrorCode::unsupported,"Unsupported guest command");
}
std::string guest_message(std::string_view uuid,GuestCommand command) {
    auto verb=command==GuestCommand::info?"info":command==GuestCommand::ping?"ping":"shutdown";
    return "IIV1 "+std::string(uuid)+" "+verb+"\n";
}
} // namespace ii::virtualizer
