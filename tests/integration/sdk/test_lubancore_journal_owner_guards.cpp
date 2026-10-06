#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>

#include "trajectory/session_lock.hpp"
#include "trajectory/session_recovery_view.hpp"
#include "trajectory/v3/reader.hpp"

namespace lubancore_consumer {
std::string JournalOwnerCase(const std::string&, const std::filesystem::path&);
}
namespace {
namespace fs = std::filesystem;
namespace tr = lubancode::trajectory;
namespace v3 = tr::v3;
struct Directory {
    fs::path root;
    Directory() {
        static std::atomic<unsigned> serial{0}; root = fs::temp_directory_path() / ("sdk-journal-native-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        REQUIRE(fs::create_directory(root));
    }
    ~Directory() { std::error_code ec; fs::remove_all(root, ec); }
};
std::string Bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary); REQUIRE(input.is_open());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
}
TEST_CASE("SDK Journal owner guards: actual installed-host path preserves canonical run chain and old prefix") {
    Directory directory; const auto path = fs::u8path(lubancore_consumer::JournalOwnerCase("close-read", directory.root));
    REQUIRE(fs::is_regular_file(path)); CHECK_FALSE(tr::SessionLock::Inspect(path.parent_path()).has_value());
    const auto ledger = v3::ReadV3Ledger(path); REQUIRE_MESSAGE(ledger.has_value(), (ledger ? "" : ledger.error()));
    const auto state = path.parent_path().parent_path().parent_path().parent_path().parent_path();
    const auto receipt_file = state.parent_path() / "journal-owner-host-receipt.txt";
    std::ifstream input(receipt_file, std::ios::binary); REQUIRE(input.is_open());
    std::map<std::string, std::string> receipt; std::string line;
    while (std::getline(input, line)) { const auto position = line.find('='); REQUIRE(position != std::string::npos);
        REQUIRE(receipt.emplace(line.substr(0, position), line.substr(position + 1)).second); }
    CHECK(receipt.at("session_id") == ledger->session_id); CHECK(receipt.at("model_calls") == "4"); CHECK(receipt.at("tool_calls") == "2");
    const auto bytes = Bytes(path); const auto before = Bytes(state.parent_path() / "journal-owner-before.jsonl");
    const auto count = std::stoull(receipt.at("prefix_bytes"));
    CHECK(before.size() == count); REQUIRE(count < bytes.size()); CHECK(bytes.size() == std::stoull(receipt.at("after_bytes")));
    CHECK(bytes.starts_with(before));
    const auto prefix_lines = tr::RecoveryStreamLines(before, std::nullopt, true); REQUIRE(prefix_lines.has_value());
    const auto prefix = v3::ReadV3LedgerOwned(path, *prefix_lines); REQUIRE(prefix.has_value()); CHECK(prefix->lines < ledger->lines);
    CHECK(prefix->session_id == ledger->session_id); CHECK(prefix->run_id == ledger->run_id);
    unsigned started = 0, ended = 0, prepared = 0; std::uint64_t expected_seq = 1;
    for (const auto& entry : ledger->timeline) {
        CHECK(entry.seq == expected_seq++);
        if (entry.is_message) {
            const auto& message = ledger->messages.at(entry.index); CHECK(message.session_id == prefix->session_id); CHECK(message.run_id == prefix->run_id);
            if (message.seq <= prefix->lines) { const auto* old = prefix->FindMessage(message.message_id); REQUIRE(old); CHECK(old->line_hash == message.line_hash); }
        } else {
            const auto& event = ledger->events.at(entry.index); CHECK(event.session_id == prefix->session_id); CHECK(event.run_id == prefix->run_id);
            if (event.seq <= prefix->lines) { const auto* old = prefix->FindEvent(event.event_id); REQUIRE(old); CHECK(old->line_hash == event.line_hash); }
            started += event.kind == v3::EventKindV3::SessionStarted; ended += event.kind == v3::EventKindV3::SessionEnded;
            if (event.kind == v3::EventKindV3::ModelRequestPrepared) { ++prepared; CHECK(v3::CheckPreparedAgainstChain(*ledger, event.event_id).empty()); }
        }
    }
    CHECK(started == 1); CHECK(ended == 2); CHECK(prepared == 4); CHECK(expected_seq == ledger->lines + 1);
    std::cout << "[sdk-journal-owner-guard] actual-public-source" << std::endl;
}
