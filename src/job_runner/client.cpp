#include "job_runner/client.hpp"
#include "job_runner/common.hpp"
#include <chrono>
#include <thread>
#include "platform/paths.hpp"

namespace lubancode::job_runner {
Json Request(const fs::path& root, const std::string& expected_id, const Json& body, int timeout_ms) {
    try {
        if (!HexId(expected_id) || timeout_ms < 1 || timeout_ms > 30000 || body.dump().size() > kMaxRequestBytes / 2) {
            return Failure("runner.invalid_request");
        }
        VerifyPrivateDirectory(root);
        const auto identity = ReadJson(root / "identity.json");
        if (identity.value("runner_id", "") != expected_id) return Failure("runner.identity_mismatch");
        const auto endpoint = ReadJson(root / "endpoint.json");
        if (endpoint.value("runner_id", "") != expected_id || !HexId(endpoint.value("boot_id", ""))) {
            return Failure("runner.identity_mismatch");
        }
        const std::string request_id = RandomHex();
        const std::string filename = request_id + ".json";
        const auto response_path = root / "responses" / filename;
        Json envelope{{"schemaVersion", 1}, {"runner_id", expected_id}, {"boot_id", endpoint["boot_id"]},
                      {"auth_token", identity["auth_token"]}, {"request_id", request_id},
                      {"expires_at_ms", NowMs() + timeout_ms}, {"body", body}};
        PublishJson(root / "requests", filename, envelope);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            std::error_code ec;
            if (fs::exists(platform::FileIoPath(response_path), ec)) {
                auto response = ReadJson(response_path);
                fs::remove(platform::FileIoPath(response_path), ec);
                if (response.value("runner_id", "") != expected_id || response.value("request_id", "") != request_id) {
                    return Failure("runner.reply_identity_mismatch");
                }
                response.erase("request_id");
                return response;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::error_code ignored;
        fs::remove(platform::FileIoPath(root / "requests" / filename), ignored);
        // Delivery may already have happened. Retry start only with the same key.
        return Failure("runner.request_timeout");
    } catch (const Error& error) {
        return Failure(error.what());
    } catch (...) {
        return Failure("runner.unavailable");
    }
}
}  // namespace lubancode::job_runner
