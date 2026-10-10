#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Experimental same-source C++ data contract, not a stable DLL ABI.
namespace lubancore::usage::v1 {

enum class Field : std::uint8_t {
    Input, Output, CacheRead, CacheCreation, OutputReasoning
};
inline constexpr std::size_t kFieldCount = 5;
enum class Presence : std::uint8_t { Missing, Present };
enum class Validity : std::uint8_t {
    Unknown, ValidInteger, InvalidType, OutOfRange, UnavailableOperands, Overflow
};
enum class Origin : std::uint8_t { Unknown, Reported, Normalized, Inferred };
enum class RawKind : std::uint8_t {
    Null, Boolean, SignedInteger, UnsignedInteger, FloatingPoint, String, Object, Array
};
enum class AnomalyCode : std::uint8_t {
    Negative, CacheExceedsTotal, AliasConflict, MissingOperand, ArithmeticOverflow,
    InvalidType, OutOfRange, InconsistentTotal, AliasUnverifiable, ResponseIdentityConflict, ParseIncomplete
};

// Admission checks limits before copying strings/vectors. Provider parsers only
// capture the known accounting fields, never traverse arbitrary response bodies.
inline constexpr std::size_t kMaxRawFields = 64;
inline constexpr std::size_t kMaxOperandsPerField = 8;
inline constexpr std::size_t kMaxExtensions = 16;
inline constexpr std::size_t kMaxAnomalies = 16;
inline constexpr std::size_t kMaxNamespaceBytes = 64;
inline constexpr std::size_t kMaxPathBytes = 256;
inline constexpr std::size_t kMaxSummaryBytes = 96;
inline constexpr std::size_t kMaxResponseIdBytes = 256;
inline constexpr std::size_t kMaxMaterialBytes = 16 * 1024;

struct RawField {
    std::string path;
    RawKind kind = RawKind::Null;
    // A present int64 includes negative and explicit zero. Wrong types and
    // unsigned values above INT64_MAX remain raw evidence, never cast silently.
    std::optional<std::int64_t> integer;
    std::string summary;
    // Optional lowercase SHA-256 hex of a raw scalar. No body-sized serialization.
    std::string fingerprint;
};
struct FieldObservation {
    Presence presence = Presence::Missing;
    Validity validity = Validity::Unknown;
    Origin origin = Origin::Unknown;
    std::array<std::uint16_t, kMaxOperandsPerField> operands{};
    std::uint8_t operand_count = 0;
};
struct Anomaly {
    Field field = Field::Input;
    AnomalyCode code = AnomalyCode::InvalidType;
    std::string detail;
    // Evidence may include a rejected alias of the wrong type. These witnesses
    // are distinct from the integer operands used to produce the selected value.
    std::array<std::uint16_t, kMaxOperandsPerField> raw_fields{};
    std::uint8_t raw_field_count = 0;
    // Empty keeps the legacy single-field scope. Otherwise these fields are
    // affected together, e.g. one response identity conflict invalidates all five.
    std::array<Field, kFieldCount> affected_fields{};
    std::uint8_t affected_field_count = 0;
};
struct Extension {
    std::string namespace_name;
    RawField field;
};
struct Observation {
    std::string provider_namespace;
    std::array<FieldObservation, kFieldCount> fields{};
    std::vector<RawField> raw_fields;
    std::vector<Anomaly> anomalies;
    std::vector<Extension> extensions;
};

// Coverage counts observations submitted to an aggregate, not network sends.
// Validity and presence remain independent: an inferred field can have a value
// while its provider spelling was missing. Arithmetic never wraps a total.
struct FieldCoverage {
    std::uint64_t observed = 0;
    std::uint64_t missing = 0;
    std::uint64_t valid = 0;
    std::uint64_t inferred = 0;
    std::uint64_t anomalous = 0;
    std::uint64_t included = 0;
    std::uint64_t omitted = 0;
    bool arithmetic_overflow = false;
};
struct Coverage {
    std::uint64_t samples = 0;
    std::array<FieldCoverage, kFieldCount> fields{};
    bool counter_overflow = false;
};

}  // namespace lubancore::usage::v1
