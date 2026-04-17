#ifndef AGENTCORE_NODE_RUNTIME_H
#define AGENTCORE_NODE_RUNTIME_H

#include "agentcore/core/types.h"
#include "agentcore/state/state_store.h"
#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace agentcore {

class ToolRegistry;
class ModelRegistry;
class TraceSink;
struct AsyncToolHandle;
struct AsyncModelHandle;

enum class AsyncOperationKind : uint8_t {
    None,
    Tool,
    Model
};

struct PendingAsyncOperation {
    AsyncOperationKind kind{AsyncOperationKind::None};
    uint64_t handle_id{0};

    [[nodiscard]] bool valid() const noexcept {
        return kind != AsyncOperationKind::None && handle_id != 0U;
    }
};

class ScratchArena {
public:
    explicit ScratchArena(std::size_t initial_capacity = 4096);

    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));
    void reset() noexcept;
    [[nodiscard]] std::size_t bytes_used() const noexcept;

private:
    std::size_t align_cursor(std::size_t cursor, std::size_t alignment) const noexcept;

    std::vector<std::byte> storage_;
    std::size_t cursor_{0};
};

class Deadline {
public:
    explicit Deadline(uint64_t deadline_ns = 0) : deadline_ns_(deadline_ns) {}

    [[nodiscard]] uint64_t deadline_ns() const noexcept { return deadline_ns_; }
    [[nodiscard]] bool expired(uint64_t now_ns) const noexcept {
        return deadline_ns_ != 0U && now_ns >= deadline_ns_;
    }

private:
    uint64_t deadline_ns_{0};
};

class CancellationToken {
public:
    void request_cancel() noexcept { cancelled_ = true; }
    [[nodiscard]] bool is_cancelled() const noexcept { return cancelled_; }

private:
    bool cancelled_{false};
};

struct NodeResult {
    enum Status : uint8_t {
        Success,
        SoftFail,
        HardFail,
        Waiting,
        Cancelled
    } status{Success};

    StatePatch patch;
    float confidence{1.0F};
    uint32_t flags{0};
    std::optional<NodeId> next_override;
    std::optional<PendingAsyncOperation> pending_async;

    static NodeResult success(StatePatch patch = {}, float confidence = 1.0F) {
        return NodeResult{Success, std::move(patch), confidence, 0, std::nullopt, std::nullopt};
    }

    static NodeResult waiting(StatePatch patch = {}, float confidence = 1.0F) {
        return NodeResult{Waiting, std::move(patch), confidence, 0, std::nullopt, std::nullopt};
    }

    static NodeResult waiting_on_tool(
        AsyncToolHandle handle,
        StatePatch patch = {},
        float confidence = 1.0F
    );

    static NodeResult waiting_on_model(
        AsyncModelHandle handle,
        StatePatch patch = {},
        float confidence = 1.0F
    );

    [[nodiscard]] bool is_waiting_on_async() const noexcept {
        return status == Waiting && pending_async.has_value() && pending_async->valid();
    }
};

struct ExecutionContext {
    const WorkflowState& state;
    RunId run_id;
    GraphId graph_id;
    NodeId node_id;
    uint32_t branch_id;
    const std::vector<std::byte>& runtime_config_payload;
    ScratchArena& scratch;
    BlobStore& blobs;
    StringInterner& strings;
    const KnowledgeGraphStore& knowledge_graph;
    ToolRegistry& tools;
    ModelRegistry& models;
    TraceSink& trace;
    Deadline deadline;
    CancellationToken& cancel;
    std::optional<PendingAsyncOperation> pending_async;
};

} // namespace agentcore

#endif // AGENTCORE_NODE_RUNTIME_H
