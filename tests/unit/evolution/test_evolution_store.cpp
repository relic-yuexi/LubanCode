// 观察账 store 的单测:只追加、幂等去重、rejected 压制、半截 JSONL 可恢复。

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include "evolution/observation_store.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    TempDir() {
        dir_ = fs::temp_directory_path() /
               ("lubancode_evolution_store_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    const fs::path& Get() const { return dir_; }

private:
    fs::path dir_;
};

lubancode::evolution::EvolutionObservation MakeObservation(const std::string& id,
                                                           const std::string& fingerprint) {
    lubancode::evolution::EvolutionObservation observation;
    observation.id = id;
    observation.source = lubancode::evolution::ObservationSource::Run;
    observation.source_id = "src-of-" + id;
    observation.summary = "测试观察";
    observation.fingerprint = fingerprint;
    observation.details["k"] = 1;
    return observation;
}

}  // namespace

TEST_CASE("store:追加与幂等(同 id 重采不翻倍)") {
    TempDir temp;
    lubancode::evolution::ObservationStore store(temp.Get());
    CHECK(store.Load().empty());

    const auto first = store.Append(MakeObservation("obs-1", "fp-a"));
    REQUIRE(first.has_value());
    CHECK(*first == lubancode::evolution::ObservationStore::AppendStatus::Appended);
    CHECK(store.Load().size() == 1);

    // 同 id 再来:DulicateId,账不涨。
    const auto again = store.Append(MakeObservation("obs-1", "fp-a"));
    REQUIRE(again.has_value());
    CHECK(*again == lubancode::evolution::ObservationStore::AppendStatus::DuplicateId);
    CHECK(store.Load().size() == 1);

    // 不同 id、同指纹:照常追加(同类聚合材料,不压)。
    const auto same_kind = store.Append(MakeObservation("obs-2", "fp-a"));
    REQUIRE(same_kind.has_value());
    CHECK(*same_kind == lubancode::evolution::ObservationStore::AppendStatus::Appended);
    CHECK(store.Load().size() == 2);
    CHECK(store.Find("obs-1").has_value());
    CHECK(store.Find("obs-nope").has_value() == false);
    CHECK(fs::exists(store.observations_file()));
}

TEST_CASE("store:rejected 指纹压制——被拒同类不再进观察账") {
    TempDir temp;
    lubancode::evolution::ObservationStore store(temp.Get());
    REQUIRE(store.Append(MakeObservation("obs-1", "fp-a")).has_value());

    // 记一笔拒绝(阶段 2 的 /evolve reject 落这里;阶段 1 由 store 把门)。
    REQUIRE(store.MarkRejected("fp-b", "不要这类建议").has_value());
    CHECK(store.IsRejected("fp-b"));
    CHECK_FALSE(store.IsRejected("fp-a"));

    // 被拒 fingerprint 的新观察:压下,不进账。
    const auto suppressed = store.Append(MakeObservation("obs-9", "fp-b"));
    REQUIRE(suppressed.has_value());
    CHECK(*suppressed == lubancode::evolution::ObservationStore::AppendStatus::SuppressedRejected);
    CHECK(store.Load().size() == 1);

    // 拒绝账只追加、可重读;空 fingerprint 拒记。
    const auto rejected = store.LoadRejected();
    REQUIRE(rejected.size() == 1);
    CHECK(rejected[0].fingerprint == "fp-b");
    CHECK(rejected[0].reason == "不要这类建议");
    CHECK_FALSE(rejected[0].rejected_at.empty());
    CHECK_FALSE(store.MarkRejected("", "空指纹").has_value());
}

TEST_CASE("store:只追加——旧行不改,新行续尾") {
    TempDir temp;
    lubancode::evolution::ObservationStore store(temp.Get());
    REQUIRE(store.Append(MakeObservation("obs-1", "fp-a")).has_value());
    std::string first_line;
    {
        std::ifstream in(store.observations_file(), std::ios::binary);
        std::getline(in, first_line);
    }
    // 同 id 跳过、新 id 追加之后,首行原样(只追加,不改写)。
    REQUIRE(store.Append(MakeObservation("obs-1", "fp-a")).has_value());
    REQUIRE(store.Append(MakeObservation("obs-2", "fp-a")).has_value());
    std::string all;
    {
        std::ifstream in(store.observations_file(), std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            all += line + "\n";
        }
    }
    CHECK(all.find(first_line) == 0);
    CHECK(std::count(all.begin(), all.end(), '\n') == 2);
}

TEST_CASE("store:半截 JSONL 可恢复——坏行跳过不废整账") {
    TempDir temp;
    lubancode::evolution::ObservationStore store(temp.Get());
    REQUIRE(store.Append(MakeObservation("obs-1", "fp-a")).has_value());
    REQUIRE(store.Append(MakeObservation("obs-2", "fp-b")).has_value());
    // 模拟崩溃截断:尾行劈一半,再塞一行纯垃圾。
    {
        std::ofstream out(store.observations_file(), std::ios::binary | std::ios::app);
        out << "{\"schema\":1,\"id\":\"obs-3\",\"source\":\"run\",\"source_id\":\"src-of-obs-3\",\"sum";
        out << "\n";
        out << "garbage not json";
        out << "\n";
    }
    const auto ledger = store.Load();
    CHECK(ledger.size() == 2);  // 两条完整的还在,半截与垃圾跳过
    // 恢复后照常续账:新观察落在坏行之后,账还能用。
    REQUIRE(store.Append(MakeObservation("obs-3", "fp-c")).has_value());
    CHECK(store.Load().size() == 3);
    CHECK(store.Find("obs-3").has_value());
}

TEST_CASE("store:空 id 拒绝;坏目录如实报错") {
    TempDir temp;
    lubancode::evolution::ObservationStore store(temp.Get());
    CHECK_FALSE(store.Append(MakeObservation("", "fp-a")).has_value());
    // 拿一个文件占住目录名:建目录失败,Append 报错不崩。
    const fs::path blocked = temp.Get() / "blocked";
    std::ofstream(blocked, std::ios::binary) << "x";
    lubancode::evolution::ObservationStore bad_store(blocked);
    const auto result = bad_store.Append(MakeObservation("obs-1", "fp-a"));
    CHECK_FALSE(result.has_value());
    CHECK(bad_store.Load().empty());
}
