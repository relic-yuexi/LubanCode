#include "accounting/pricing_table.hpp"

#include <cmath>
#include <string_view>

namespace lubancode::accounting {
namespace {

// 单价 JSON 值 -> 整数 micros。整数当货币单位(3 -> 3'000'000);小数一次
// llround(x * 1e6) 折完(1.25 -> 1'250'000),此后不再碰浮点。
std::optional<std::int64_t> ParsePriceMicros(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        const std::int64_t units = value.get<std::int64_t>();
        if (units < 0) {
            return std::nullopt;
        }
        // 单位上限:防溢出(1e6 倍之后仍在 int64 内)。
        if (units > 9'000'000'000'000LL) {
            return std::nullopt;
        }
        return units * 1'000'000;
    }
    if (value.is_number_float()) {
        const double price = value.get<double>();
        if (!(price >= 0.0) || price > 9.0e12) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(std::llround(price * 1e6));
    }
    return std::nullopt;
}

// "YYYY-MM-DD" 形状 + 日历粗校验。
bool IsValidDate(std::string_view date) {
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') {
        return false;
    }
    for (std::size_t i = 0; i < date.size(); ++i) {
        if (i == 4 || i == 7) {
            continue;
        }
        if (date[i] < '0' || date[i] > '9') {
            return false;
        }
    }
    const int month = (date[5] - '0') * 10 + (date[6] - '0');
    const int day = (date[8] - '0') * 10 + (date[9] - '0');
    return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

std::optional<ModelPrice> ParseModelPrice(const nlohmann::json& json, std::string* error) {
    if (!json.is_object()) {
        *error = "models 条目须是 object";
        return std::nullopt;
    }
    ModelPrice price;
    const struct {
        const char* key;
        std::int64_t* out;
    } fields[] = {
        {"input_per_million", &price.input_per_million_micros},
        {"cache_read_per_million", &price.cache_read_per_million_micros},
        {"cache_creation_per_million", &price.cache_creation_per_million_micros},
        {"output_per_million", &price.output_per_million_micros},
    };
    for (const auto& field : fields) {
        if (!json.contains(field.key)) {
            *error = std::string("单价缺键: ") + field.key;
            return std::nullopt;
        }
        const auto micros = ParsePriceMicros(json.at(field.key));
        if (!micros.has_value()) {
            *error = std::string("单价非数或为负: ") + field.key;
            return std::nullopt;
        }
        *field.out = *micros;
    }
    if (json.size() != 4) {
        *error = "models 条目未知键";
        return std::nullopt;
    }
    return price;
}

}  // namespace

nlohmann::json PricingTable::ToJson() const {
    nlohmann::json json = nlohmann::json::object();
    json["schema"] = kPricingTableSchema;
    json["schema_version"] = kPricingTableSchemaVersion;
    json["id"] = id;
    json["currency"] = currency;
    json["effective_from"] = effective_from;
    json["source"] = source;
    nlohmann::json models = nlohmann::json::object();
    for (const auto& [key, price] : this->models) {
        // micros 折回货币单位只给人看;机器读一律 micros 整数。
        nlohmann::json entry = nlohmann::json::object();
        entry["input_per_million"] = price.input_per_million_micros;
        entry["cache_read_per_million"] = price.cache_read_per_million_micros;
        entry["cache_creation_per_million"] = price.cache_creation_per_million_micros;
        entry["output_per_million"] = price.output_per_million_micros;
        models[key] = std::move(entry);
    }
    json["models"] = std::move(models);
    return json;
}

std::optional<PricingTable> PricingTable::FromJsonStrict(const nlohmann::json& json,
                                                         std::string* error) {
    if (!json.is_object()) {
        *error = "价格表须是 object";
        return std::nullopt;
    }
    PricingTable table;
    const auto read_string = [&](const char* key, std::string* out) {
        return json.contains(key) && json.at(key).is_string() &&
               (*out = json.at(key).get<std::string>(), true);
    };
    std::string schema;
    if (!read_string("schema", &schema) || schema != kPricingTableSchema) {
        *error = "schema 名不是 " + std::string(kPricingTableSchema);
        return std::nullopt;
    }
    if (!json.contains("schema_version") || !json.at("schema_version").is_number_integer() ||
        json.at("schema_version").get<int>() != kPricingTableSchemaVersion) {
        *error = "schema_version 只认 1";
        return std::nullopt;
    }
    if (!read_string("id", &table.id) || table.id.empty()) {
        *error = "id 必填";
        return std::nullopt;
    }
    read_string("currency", &table.currency);
    if (!read_string("effective_from", &table.effective_from) ||
        !IsValidDate(table.effective_from)) {
        *error = "effective_from 须是 YYYY-MM-DD";
        return std::nullopt;
    }
    read_string("source", &table.source);
    if (!json.contains("models") || !json.at("models").is_object()) {
        *error = "models 须是 object";
        return std::nullopt;
    }
    for (auto it = json.at("models").begin(); it != json.at("models").end(); ++it) {
        const auto key = it.key();
        if (key.empty() || key.find('/') == std::string::npos) {
            *error = "models 键须是 provider/model 或 */model: " + key;
            return std::nullopt;
        }
        const auto price = ParseModelPrice(*it, error);
        if (!price.has_value()) {
            *error = key + ": " + *error;
            return std::nullopt;
        }
        table.models[key] = *price;
    }
    for (auto it = json.begin(); it != json.end(); ++it) {
        if (it.key() != "schema" && it.key() != "schema_version" && it.key() != "id" &&
            it.key() != "currency" && it.key() != "effective_from" && it.key() != "source" &&
            it.key() != "models") {
            *error = "价格表未知键: " + it.key();
            return std::nullopt;
        }
    }
    return table;
}

const ModelPrice* PricingTable::Find(std::string_view provider, std::string_view model) const {
    if (model.empty()) {
        return nullptr;
    }
    const std::string exact = std::string(provider) + "/" + std::string(model);
    const auto exact_it = models.find(exact);
    if (exact_it != models.end()) {
        return &exact_it->second;
    }
    const std::string wildcard = "*/" + std::string(model);
    const auto wildcard_it = models.find(wildcard);
    if (wildcard_it != models.end()) {
        return &wildcard_it->second;
    }
    return nullptr;
}

bool PricingTable::EffectiveOn(std::string_view yyyymmdd) const {
    if (!IsValidDate(yyyymmdd) || !IsValidDate(effective_from)) {
        return false;
    }
    return yyyymmdd >= std::string_view(effective_from);
}

}  // namespace lubancode::accounting
