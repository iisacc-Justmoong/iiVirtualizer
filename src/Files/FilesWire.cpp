#include "FilesWire.h"
#include "BinaryWire.h"
#include <algorithm>
#include <random>
#include <set>
#include <stdexcept>

namespace ii::virtualizer::files_detail {
using Json=ii::json::Value;
namespace {
const Json& field(const Json& value,std::string_view name){auto item=value.find(name);if(!item)throw std::invalid_argument("Missing Files field");return item->get();}
std::string text(const Json& value,std::string_view name){auto item=field(value,name).get<std::string>();if(!item)throw std::invalid_argument("Invalid Files text");return item->get();}
std::uint64_t integer(const Json& value,std::string_view name){const auto& item=field(value,name);if(auto n=item.get<std::uint64_t>())return n->get();if(auto n=item.get<std::int64_t>();n&&n->get()>=0)return static_cast<std::uint64_t>(n->get());throw std::invalid_argument("Invalid Files integer");}
bool boolean(const Json& value,std::string_view name){auto item=field(value,name).get<bool>();if(!item)throw std::invalid_argument("Invalid Files flag");return item->get();}
void fields(const Json& value,std::initializer_list<std::string_view> allowed) {
    auto object=value.get<Json::Object>();if(!object)throw std::invalid_argument("Expected Files object");
    for(const auto& [name,item]:object->get()){(void)item;if(std::find(allowed.begin(),allowed.end(),name)==allowed.end())throw std::invalid_argument("Unknown Files field");}
}
Json entry_json(const FileEntry& entry) {return Json(Json::Object{{"path",Json(entry.path)},{"name",Json(entry.name)},{"kind",Json(name(entry.kind))},{"size",Json(entry.size)},{"modified_ns",Json(entry.modified_ns)}});}
FileEntry parse_entry(const Json& value) {
    fields(value,{"path","name","kind","size","modified_ns"});FileEntry result;result.path=text(value,"path");result.name=text(value,"name");result.size=integer(value,"size");
    auto modified=field(value,"modified_ns").get<std::int64_t>();if(modified)result.modified_ns=modified->get();else {const auto n=integer(value,"modified_ns");if(n>INT64_MAX)throw std::invalid_argument("Invalid Files timestamp");result.modified_ns=static_cast<std::int64_t>(n);}
    const auto kind=text(value,"kind");if(kind=="file")result.kind=FileKind::regular;else if(kind=="directory")result.kind=FileKind::directory;else if(kind=="symlink")result.kind=FileKind::symlink;else if(kind=="other")result.kind=FileKind::other;else throw std::invalid_argument("Invalid Files kind");
    if(!validate_file_path(result.path,true)||result.name!=result.path.substr(result.path.rfind('/')==std::string::npos?0:result.path.rfind('/')+1))throw std::invalid_argument("Invalid Files entry path");
    return result;
}
bool identifier(std::string_view value){return !value.empty()&&value.size()<=64&&std::all_of(value.begin(),value.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_';});}
bool uuid_valid(std::string_view value){if(value.size()!=36)return false;for(std::size_t i=0;i<value.size();++i)if((i==8||i==13||i==18||i==23)?value[i]!='-':!((value[i]>='0'&&value[i]<='9')||(value[i]>='a'&&value[i]<='f')))return false;return true;}
Json frame_json(std::string_view frame,std::string_view uuid,std::string_view id) {
    if(frame.size()>frame_limit||!uuid_valid(uuid)||!identifier(id))throw std::invalid_argument("Files frame limit or binding");
    const std::string prefix="IIVF1 "+std::string(uuid)+" "+std::string(id)+" ";
    if(!frame.starts_with(prefix))throw std::invalid_argument("Files frame identity mismatch");
    auto parsed=ii::json::parse(frame.substr(prefix.size()),{.input_bytes=frame_limit,.output_bytes=frame_limit,.string_bytes=32768,.items=1024,.depth=12});
    if(!parsed)throw std::invalid_argument("Malformed Files frame");
    return *parsed;
}
std::string serialize(std::string_view uuid,std::string_view id,Json payload) {
    if(!uuid_valid(uuid)||!identifier(id))throw std::invalid_argument("Files frame binding required");
    auto body=ii::json::stringify(payload,{.input_bytes=frame_limit,.output_bytes=frame_limit,.string_bytes=32768,.items=1024,.depth=12});
    if(!body)throw std::invalid_argument("Files frame encoding failed");
    auto output="IIVF1 "+std::string(uuid)+" "+std::string(id)+" "+*body+"\n";
    if(output.size()>frame_limit)throw std::invalid_argument("Files frame exceeds limit");
    return output;
}
}
std::string nonce(){std::random_device random;std::string value;constexpr char hex[]="0123456789abcdef";for(unsigned i=0;i<16;++i){auto byte=random()&255;value+=hex[byte>>4];value+=hex[byte&15];}return value;}
Json request_payload(const FileRequest& request) {
    Json::Object value{{"path",Json(request.path)}};
    if(request.action==FileAction::move)value.emplace("destination",Json(request.destination));
    if(request.action==FileAction::read){value.emplace("offset",Json(request.offset));value.emplace("length",Json(request.length));}
    if(request.action==FileAction::create||request.action==FileAction::write)value.emplace("data_base64",Json(wire_detail::encode_binary(request.data)));
    if(request.action==FileAction::write){value.emplace("offset",Json(request.offset));value.emplace("truncate",Json(request.truncate));}
    if(request.action==FileAction::list){value.emplace("cursor",Json(request.cursor));value.emplace("limit",Json(request.limit));value.emplace("include_hidden",Json(request.include_hidden));}
    return Json(std::move(value));
}
core::Result<FileRequest> parse_request(const Json& json,FileAction action) {
    try {
        switch(action){case FileAction::list:fields(json,{"path","cursor","limit","include_hidden"});break;case FileAction::read:fields(json,{"path","offset","length"});break;
            case FileAction::create:fields(json,{"path","data_base64"});break;case FileAction::write:fields(json,{"path","offset","data_base64","truncate"});break;
            case FileAction::move:fields(json,{"path","destination"});break;default:fields(json,{"path"});break;}
        FileRequest request;request.action=action;if(json.find("path"))request.path=text(json,"path");
        if(json.find("destination"))request.destination=text(json,"destination");
        if(json.find("data_base64"))request.data=wire_detail::decode_binary(text(json,"data_base64"),file_chunk_limit);
        if(json.find("offset"))request.offset=integer(json,"offset");
        if(json.find("length"))request.length=integer(json,"length");
        if(json.find("limit"))request.limit=integer(json,"limit");
        if(json.find("cursor"))request.cursor=text(json,"cursor");
        if(json.find("include_hidden"))request.include_hidden=boolean(json,"include_hidden");
        if(json.find("truncate"))request.truncate=boolean(json,"truncate");
        if(auto valid=validate(request);!valid)return std::unexpected(valid.error());
        return request;
    }catch(...){return core::fail(core::ErrorCode::invalid_argument,"Malformed Files request");}
}
Json reply_payload(const FileReply& reply) {
    Json::Array entries;for(const auto& value:reply.entries)entries.push_back(entry_json(value));
    Json::Object value{{"entries",Json(std::move(entries))},{"data_base64",Json(wire_detail::encode_binary(reply.data))},{"next_cursor",Json(reply.next_cursor)},{"offset",Json(reply.offset)},{"eof",Json(reply.eof)},{"removed",Json(reply.removed)}};
    if(reply.entry)value.emplace("entry",entry_json(*reply.entry));
    return Json(std::move(value));
}
core::Result<FileReply> parse_reply(const Json& value,const FileRequest& request) {
    try {
        fields(value,{"entry","entries","data_base64","next_cursor","offset","eof","removed"});FileReply reply;
        if(value.find("entry"))reply.entry=parse_entry(field(value,"entry"));
        reply.data=wire_detail::decode_binary(text(value,"data_base64"),file_chunk_limit);reply.next_cursor=text(value,"next_cursor");reply.offset=integer(value,"offset");reply.eof=boolean(value,"eof");reply.removed=boolean(value,"removed");
        auto entries=field(value,"entries").get<Json::Array>();if(!entries||entries->get().size()>request.limit)throw std::invalid_argument("Files page size mismatch");
        std::string previous=request.cursor;
        for(const auto& row:entries->get()) {
            auto item=parse_entry(row);const auto expected=request.path.empty()?item.name:request.path+"/"+item.name;
            if(item.path!=expected||item.name<=previous)throw std::invalid_argument("Files page scope/order mismatch");
            previous=item.name;reply.entries.push_back(std::move(item));
        }
        if(request.action==FileAction::list) {
            if(reply.entry||!reply.data.empty()||reply.removed||(!reply.next_cursor.empty()&&(reply.entries.empty()||reply.next_cursor!=reply.entries.back().name)))throw std::invalid_argument("Invalid Files list result");
        }else if(request.action==FileAction::remove) {
            if(!reply.removed||reply.entry||!reply.entries.empty()||!reply.data.empty())throw std::invalid_argument("Invalid Files deletion result");
        }else {
            const auto expected=request.action==FileAction::move?request.destination:request.path;
            if(!reply.entry||reply.entry->path!=expected||reply.removed||!reply.entries.empty()||!reply.next_cursor.empty())throw std::invalid_argument("Files result scope mismatch");
            if(request.action==FileAction::read) {
                if(reply.entry->kind!=FileKind::regular||reply.offset!=request.offset||reply.data.size()>request.length)throw std::invalid_argument("Invalid Files read result");
            }else if(!reply.data.empty())throw std::invalid_argument("Unexpected Files contents");
        }
        return reply;
    }catch(...){return core::fail(core::ErrorCode::failed,"Malformed or mismatched Files result");}
}
std::string encode_request(std::string_view uuid,std::string_view id,const FileRequest& request,std::chrono::milliseconds timeout) {
    if(!validate(request)||timeout.count()<1||timeout>std::chrono::seconds(60))throw std::invalid_argument("Invalid Files request or deadline");
    return serialize(uuid,id,Json(Json::Object{{"operation",Json(name(request.action))},{"timeout_ms",Json(timeout.count())},{"request",request_payload(request)}}));
}
core::Result<Envelope> decode_request(std::string_view frame,std::string_view uuid) {
    try {
        const std::string prefix="IIVF1 "+std::string(uuid)+" ";if(!frame.starts_with(prefix))throw std::invalid_argument("Files UUID mismatch");
        const auto end=frame.find(' ',prefix.size());if(end==std::string_view::npos)throw std::invalid_argument("Files request ID missing");
        const auto id=frame.substr(prefix.size(),end-prefix.size());const auto body=frame_json(frame,uuid,id);fields(body,{"operation","timeout_ms","request"});
        auto action=file_action(text(body,"operation"));if(!action)return std::unexpected(action.error());auto request=parse_request(field(body,"request"),*action);if(!request)return std::unexpected(request.error());
        const auto timeout=integer(body,"timeout_ms");if(!timeout||timeout>60000)throw std::invalid_argument("Invalid Files timeout");
        return Envelope{std::string(uuid),std::string(id),std::move(*request),std::chrono::milliseconds(timeout)};
    }catch(...){return core::fail(core::ErrorCode::invalid_argument,"Malformed or mismatched Files frame");}
}
std::string encode_reply(std::string_view uuid,std::string_view id,FileAction action,const core::Result<FileReply>& result) {
    Json::Object value{{"operation",Json(name(action))},{"ok",Json(bool(result))}};
    if(result)value.emplace("result",reply_payload(*result));else value.emplace("error",Json(Json::Object{{"code",Json(static_cast<int>(result.error().code))},{"message",Json(result.error().message)}}));
    return serialize(uuid,id,Json(std::move(value)));
}
core::Result<FileReply> decode_reply(std::string_view frame,std::string_view uuid,std::string_view id,const FileRequest& request) {
    try {
        const auto body=frame_json(frame,uuid,id);fields(body,{"operation","ok","result","error"});if(text(body,"operation")!=name(request.action))throw std::invalid_argument("Files operation mismatch");
        if(!boolean(body,"ok")){const auto& error=field(body,"error");fields(error,{"code","message"});const auto code=integer(error,"code");if(code>static_cast<unsigned>(core::ErrorCode::wrong_executor))throw std::invalid_argument("Unknown Files error");return core::fail(static_cast<core::ErrorCode>(code),text(error,"message"));}
        return parse_reply(field(body,"result"),request);
    }catch(...){return core::fail(core::ErrorCode::failed,"Malformed or mismatched Files response frame");}
}
}
