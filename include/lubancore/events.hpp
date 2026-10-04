#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <lubancore/api.hpp>
#include <lubancore/subagents.hpp>

namespace lubancore {

enum class ApprovalMode { Confirm, AcceptEdits, DontAsk, Yolo };
enum class ApprovalDecision { Accept, AcceptForSession, Decline, Cancel };
struct Approval {
    std::string request_id; // opaque; scoped to this session and durable turn
    std::string operation_id;
    std::string tool_call_id;
    std::string tool_name;
    std::string input_json;
    std::string cwd;
    std::string reason;
    // Present only for a real main-foreground child ticket. Session acceptance
    // then grants this child only; the ordinary parent allowed account is separate.
    std::optional<subagents::v1::ApprovalScope> child;
};
struct Event {
    // Runtime event names plus approval_requested and operation_completed.
    std::string kind;
    std::string session_id;
    std::string operation_id;
    std::string turn_id;
    std::string text;
    std::string payload_json;
    std::optional<Approval> approval;
};

} // namespace lubancore

namespace lubancore::events::v1 {

// Trusted host queue. Push is bounded and nonblocking; only Next may wait.
// Close must stop admission and wake Next, and may overlap admitted Push/Next.
// All methods must support concurrent callers. Queue acceptance is transient,
// not a durable cursor or authority to execute. Ownership stays with one stream.
class EventQueue {
public:
    virtual ~EventQueue() = default;
    virtual Result<void> Push(Event value) = 0;
    virtual Result<std::optional<Event>> Next(std::chrono::milliseconds timeout) = 0;
    virtual Result<void> Close() = 0;
};

// One host-created, Session-owned factory. Each call returns an independent,
// unique queue honoring capacity, FIFO and the default 16 MiB event-byte limit.
// CreateQueue may run concurrently for independent subscriptions. No delivery
// thread is created. Never destroy owning Runtime/Session handles in callbacks.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual Result<std::unique_ptr<EventQueue>> CreateQueue(std::size_t capacity) = 0;
};

} // namespace lubancore::events::v1
