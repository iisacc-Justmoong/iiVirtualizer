#include "../src/Files/FilesWire.h"
#include "../src/Processes/ProcessWire.h"
#include <chrono>
#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace v = ii::virtualizer;

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const std::string uuid = "00000000-0000-4000-8000-000000000001";
        v::FileRequest file;
        file.action = v::FileAction::create;
        file.path = "binary.dat";
        file.data = {std::byte{0}, std::byte{127}, std::byte{255}};
        const auto file_frame = v::files_detail::encode_request(uuid, "file-test", file, std::chrono::seconds(5));
        const auto file_request = v::files_detail::decode_request(file_frame, uuid);
        check(file_request && file_request->request.data == file.data, "binary file request roundtrip");
        check(!v::files_detail::decode_request(file_frame, "00000000-0000-4000-8000-000000000002"), "file request identity");
        auto file_value = v::files_detail::request_payload(file);
        auto file_fields = file_value.get<ii::json::Value::Object>()->get();
        file_fields.insert_or_assign("data_base64", ii::json::Value("AR=="));
        check(!v::files_detail::parse_request(ii::json::Value(file_fields), v::FileAction::create), "noncanonical binary encoding");

        v::ProcessLaunch launch;
        launch.key = "job";
        launch.executable = "@worker";
        v::ProcessRequest request;
        request.action = v::ProcessAction::launch;
        request.launch = launch;
        const auto frame = v::process_detail::encode_request(uuid, "process-test", request);
        const auto decoded = v::process_detail::decode_request(frame, uuid);
        check(decoded && decoded->request.launch && *decoded->request.launch == launch, "process launch roundtrip");
        auto invalid = request;
        invalid.launch->environment["PATH"] = "/tmp";
        check(!v::validate(invalid), "reserved process environment");
        invalid.launch.reset();
        check(!v::validate(invalid), "missing process launch");

        v::ProcessSnapshot snapshot;
        snapshot.id = "process1";
        snapshot.key = launch.key;
        snapshot.executable = launch.executable;
        v::ProcessReply reply;
        reply.process = snapshot;
        const auto response = v::process_detail::encode_reply(uuid, "process-test", request.action, reply);
        check(bool(v::process_detail::decode_reply(response, uuid, "process-test", request)), "process reply roundtrip");
        invalid = request;
        invalid.launch->key = "different";
        check(!v::process_detail::decode_reply(response, uuid, "process-test", invalid), "process launch binding");
        std::cout << "Files and process protocol contracts passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
