#include "ProcessBootstrap.h"
#include <iiJson/Json.h>

namespace ii::virtualizer::guest {
core::Result<ii::process::Launch> process_bootstrap_launch(const ProcessLaunch& request) {
    if(auto valid=validate(request);!valid)return std::unexpected(valid.error());
    ii::json::Value::Object environment;
    for(const auto& [key,value]:request.environment)environment.emplace(key,ii::json::Value(value));
    auto encoded=ii::json::stringify(ii::json::Value(std::move(environment)),{.input_bytes=65536,.output_bytes=65536,.string_bytes=32768,.items=128,.depth=3});
    if(!encoded)return core::fail(core::ErrorCode::invalid_argument,"Cannot encode the bounded application environment");
    ii::process::Launch launch;launch.arguments={"/sbin/ii-process",request.executable,request.working_directory};
    launch.arguments.insert(launch.arguments.end(),request.arguments.begin(),request.arguments.end());launch.working_directory="/";
    launch.inherit_environment=false;launch.environment.emplace(process_environment_key,std::move(*encoded));return launch;
}
core::Result<ii::process::Environment> process_bootstrap_environment(std::string_view encoded) {
    if(encoded.size()>65536)return core::fail(core::ErrorCode::invalid_argument,"Application environment exceeds its bootstrap budget");
    auto value=ii::json::parse(encoded,{.input_bytes=65536,.output_bytes=65536,.string_bytes=32768,.items=128,.depth=3});
    if(!value)return core::fail(core::ErrorCode::invalid_argument,"Malformed application environment");
    auto object=value->get<ii::json::Value::Object>();if(!object||object->get().size()>32)return core::fail(core::ErrorCode::invalid_argument,"Application environment requires a bounded object");
    ProcessLaunch check;check.key="bootstrap";check.executable="@service";
    for(const auto& [key,item]:object->get()){auto text=item.get<std::string>();if(!text)return core::fail(core::ErrorCode::invalid_argument,"Application environment values must be text");check.environment.emplace(key,text->get());}
    if(auto valid=validate(check);!valid)return std::unexpected(valid.error());return std::move(check.environment);
}
}
