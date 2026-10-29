#include "Files.h"
#include <iiSDKCore/Core/Text/Utf8.h>
#include <algorithm>

namespace ii::virtualizer {
std::string_view name(FileAction action) {
    switch(action){case FileAction::list:return "list";case FileAction::stat:return "stat";case FileAction::read:return "read";
        case FileAction::create:return "create";case FileAction::write:return "write";case FileAction::make_directory:return "mkdir";
        case FileAction::move:return "move";case FileAction::remove:return "remove";}return "unknown";
}
std::string_view name(FileKind kind) {
    switch(kind){case FileKind::regular:return "file";case FileKind::directory:return "directory";case FileKind::symlink:return "symlink";case FileKind::other:return "other";}return "unknown";
}
core::Result<FileAction> file_action(std::string_view text) {
    for(auto action:{FileAction::list,FileAction::stat,FileAction::read,FileAction::create,FileAction::write,FileAction::make_directory,FileAction::move,FileAction::remove})if(name(action)==text)return action;
    return core::fail(core::ErrorCode::invalid_argument,"Unknown Files operation");
}
core::Result<void> validate_file_path(std::string_view path,bool allow_root) {
    auto fail=[](){return core::fail(core::ErrorCode::invalid_argument,"Files paths must be bounded relative UTF-8 paths without dot segments or redirects");};
    if(path.empty())return allow_root?core::Result<void>{}:fail();
    if(path.size()>1024||path.front()=='/'||!core::valid_utf8(core::byte_view(path))||
        std::any_of(path.begin(),path.end(),[](unsigned char c){return c<32||c==127||c=='\\'||c==':';}))return fail();
    while(!path.empty()) {
        const auto separator=path.find('/');const auto part=path.substr(0,separator);
        if(part.empty()||part=="."||part==".."||part.size()>255)return fail();
        if(separator==std::string_view::npos)break;
        path.remove_prefix(separator+1);if(path.empty())return fail();
    }
    return {};
}
core::Result<void> validate(const FileRequest& request) {
    if(name(request.action)=="unknown")return core::fail(core::ErrorCode::invalid_argument,"Unknown Files operation");
    if(auto valid=validate_file_path(request.path,request.action==FileAction::list||request.action==FileAction::stat);!valid)return valid;
    if(request.action==FileAction::move) {
        if(auto valid=validate_file_path(request.destination);!valid)return valid;
        if(request.destination==request.path)return core::fail(core::ErrorCode::invalid_argument,"Move source and destination must differ");
    }else if(!request.destination.empty())return core::fail(core::ErrorCode::invalid_argument,"Destination requires a move operation");
    if(request.action!=FileAction::create&&request.action!=FileAction::write&&(!request.data.empty()||request.truncate))
        return core::fail(core::ErrorCode::invalid_argument,"File data requires a create/write operation");
    if(request.action!=FileAction::write&&request.truncate)return core::fail(core::ErrorCode::invalid_argument,"Truncation requires an explicit write operation");
    if(request.action!=FileAction::read&&request.action!=FileAction::write&&request.offset)
        return core::fail(core::ErrorCode::invalid_argument,"Offset requires a read/write operation");
    if(request.offset>file_offset_limit||request.data.size()>file_chunk_limit||request.offset>file_offset_limit-request.data.size()||!request.length||request.length>file_chunk_limit||!request.limit||request.limit>file_page_limit)
        return core::fail(core::ErrorCode::out_of_range,"Files transfer or page exceeds its bounded limit");
    if(!request.cursor.empty()) {
        if(request.action!=FileAction::list||request.cursor.find('/')!=std::string::npos)return core::fail(core::ErrorCode::invalid_argument,"Invalid directory page cursor");
        if(auto valid=validate_file_path(request.cursor);!valid)return valid;
    }
    return {};
}
} // namespace ii::virtualizer
