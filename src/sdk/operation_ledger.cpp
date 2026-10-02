#include "sdk/operation_ledger.hpp"

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>

#include "tools/path_utils.hpp"

namespace lubancore::detail {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
Error Failure(std::string code, std::string message = {}) { return {std::move(code), std::move(message)}; }
bool ValidId(const std::string& value) {
    return !value.empty() && value.size() <= 200 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
}
} // namespace

Result<void> ValidateOperationLedger(const fs::path& session_dir) {
    // The shared live-query reader deliberately skips incomplete lines. Recovery
    // cannot do that: losing an accepted row also loses its dedupe key and the
    // operation counter, permitting both repeated effects and reused IDs.
    const auto path = session_dir / "operations.jsonl";
    const auto invalid = [](std::string reason) -> Result<void> {
        return std::unexpected(Failure("sdk.resume.operation_ledger_invalid", std::move(reason)));
    };
    enum class Stage { Accepted, Dispatched, Final };
    std::map<std::string, Stage> stages;
    const auto check_result_coverage = [&]() -> Result<void> {
        std::error_code result_error;
        const auto results = session_dir / "sdk-results";
        const bool exists = fs::exists(results, result_error);
        if (result_error) return invalid("cannot inspect SDK result directory");
        if (!exists) return {};
        fs::directory_iterator entries(results, result_error);
        if (result_error) return invalid("cannot read SDK result directory");
        for (; entries != fs::directory_iterator{}; entries.increment(result_error)) {
            if (result_error) return invalid("cannot read SDK result directory");
            if (entries->path().extension() != ".json") continue;
            const auto id = lubancode::tools::PathToUtf8(entries->path().stem());
            const auto fact = stages.find(id);
            // Result bytes are written before operation.final, so Dispatched is
            // valid during crash recovery. Missing acceptance/dispatch is not.
            if (fact == stages.end() || fact->second == Stage::Accepted) {
                return invalid("SDK result has no preceding operation dispatch");
            }
        }
        if (result_error) return invalid("cannot read SDK result directory");
        return {};
    };
    std::error_code ec;
    const bool exists = fs::exists(path, ec);
    if (ec) return invalid("cannot inspect operation ledger");
    if (!exists) return check_result_coverage(); // new/empty sessions have a lazy ledger
    if (!fs::is_regular_file(path, ec) || ec) return invalid("operation ledger is not a regular file");
    std::ifstream input(path, std::ios::binary);
    if (!input) return invalid("cannot read operation ledger");
    std::set<std::string> keys;
    std::set<std::string> bound_turns;
    const auto is_string = [](const Json& row, const char* key) {
        return row.contains(key) && row.at(key).is_string();
    };
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || input.eof()) return invalid("incomplete operation ledger line");
        const auto row = Json::parse(line, nullptr, false);
        if (!row.is_object() || !row.contains("schemaVersion") || !row.at("schemaVersion").is_number_integer() ||
            (row.at("schemaVersion") != 1 && row.at("schemaVersion") != 2) ||
            !is_string(row, "kind") || !is_string(row, "operationId")) {
            return invalid("invalid operation fact shape");
        }
        const auto id = row.at("operationId").get<std::string>();
        const auto kind = row.at("kind").get<std::string>();
        if (!ValidId(id)) return invalid("invalid operation ID");
        const auto found = stages.find(id);
        if (kind == "operation.accepted") {
            if (found != stages.end() || !is_string(row, "inputId") || row.at("inputId") == "" ||
                !is_string(row, "clientOperationId") || !is_string(row, "payloadHash")) {
                return invalid("invalid or duplicate operation acceptance");
            }
            const auto key = row.at("clientOperationId").get<std::string>();
            const auto hash = row.at("payloadHash").get<std::string>();
            if ((!key.empty() && !keys.insert(key).second) || hash.size() != 64 ||
                hash.find_first_not_of("0123456789abcdef") != std::string::npos) {
                return invalid("conflicting operation acceptance");
            }
            stages.emplace(id, Stage::Accepted);
        } else if (kind == "operation.dispatched") {
            if (found == stages.end() || found->second != Stage::Accepted) {
                return invalid("dispatch without one preceding acceptance");
            }
            found->second = Stage::Dispatched;
        } else if (kind == "operation.final") {
            if (found == stages.end() || found->second != Stage::Dispatched ||
                !is_string(row, "turnId") || !is_string(row, "executionStatus") ||
                row.at("executionStatus") == "" || !row.contains("finalMessageRefs") ||
                !row.at("finalMessageRefs").is_array() || !row.contains("usageReported") ||
                !row.at("usageReported").is_boolean()) {
                return invalid("invalid final or missing preceding dispatch");
            }
            for (const auto& ref : row.at("finalMessageRefs")) {
                if (!ref.is_string()) return invalid("invalid final message reference");
            }
            const auto turn = row.at("turnId").get<std::string>();
            if (!turn.empty() && (!ValidId(turn) || !bound_turns.insert(turn).second))
                return invalid("invalid turn identity or turn bound to multiple operations");
            found->second = Stage::Final;
        } else {
            return invalid("unknown operation fact kind");
        }
    }
    if (input.bad()) return invalid("failed while reading operation ledger");
    return check_result_coverage();
}
} // namespace lubancore::detail
