#ifndef AGENTCORE_CHECKPOINT_H
#define AGENTCORE_CHECKPOINT_H

#include "agentcore/core/types.h"
#include "agentcore/graph/graph_ir.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/node_runtime.h"
#include "agentcore/runtime/scheduler.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/state/state_store.h"
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agentcore {

struct Checkpoint {
    RunId run_id{0};
    CheckpointId checkpoint_id{0};
    NodeId node_id{0};
    uint64_t state_version{0};
    uint64_t patch_log_offset{0};
    ExecutionStatus status{ExecutionStatus::NotStarted};
    uint32_t branch_id{0};
    uint64_t step_index{0};
};

struct TraceEvent {
    uint64_t sequence{0};
    uint64_t ts_start_ns{0};
    uint64_t ts_end_ns{0};
    RunId run_id{0};
    GraphId graph_id{0};
    NodeId node_id{0};
    uint32_t branch_id{0};
    CheckpointId checkpoint_id{0};
    NodeResult::Status result{NodeResult::Success};
    float confidence{0.0F};
    uint32_t patch_count{0};
    uint32_t flags{0};
    std::vector<ExecutionNamespaceRef> namespace_path;
};

struct PendingSubgraphExecution {
    RunId child_run_id{0};
    std::vector<std::byte> snapshot_bytes;

    [[nodiscard]] bool valid() const noexcept {
        return child_run_id != 0U && !snapshot_bytes.empty();
    }
};

struct BranchSnapshot {
    ExecutionFrame frame{};
    StateStore state_store{};
    uint16_t retry_count{0};
    std::optional<PendingAsyncOperation> pending_async;
    std::vector<PendingAsyncOperation> pending_async_group;
    std::optional<PendingSubgraphExecution> pending_subgraph;
    std::vector<AsyncToolSnapshot> pending_tool_snapshots;
    std::vector<AsyncModelSnapshot> pending_model_snapshots;
    std::vector<uint32_t> join_stack;
    std::optional<NodeId> reactive_root_node_id;
};

struct JoinScopeSnapshot {
    uint32_t split_id{0};
    uint32_t expected_branch_count{0};
    NodeId join_node_id{0};
    uint64_t split_patch_log_offset{0};
    StateStore base_state{};
    std::vector<uint32_t> arrived_branch_ids;
};

struct ReactiveRerunSeedSnapshot {
    StateStore state_store{};
    uint64_t step_index{0};
};

struct ReactiveFrontierSnapshot {
    NodeId node_id{0};
    bool pending_rerun{false};
    std::optional<ReactiveRerunSeedSnapshot> pending_rerun_seed;
};

struct RunSnapshot {
    GraphDefinition graph{};
    ExecutionStatus status{ExecutionStatus::NotStarted};
    std::vector<std::byte> runtime_config_payload;
    std::vector<BranchSnapshot> branches;
    std::vector<JoinScopeSnapshot> join_scopes;
    std::vector<ReactiveFrontierSnapshot> reactive_frontiers;
    std::vector<ScheduledTask> pending_tasks;
    uint32_t next_branch_id{1};
    uint32_t next_split_id{1};
};

enum class CheckpointPayloadKind : uint8_t {
    MetadataOnly = 0,
    FullSnapshot = 1
};

struct CheckpointRecord {
    Checkpoint checkpoint{};
    CheckpointPayloadKind payload_kind{CheckpointPayloadKind::MetadataOnly};
    std::optional<RunSnapshot> snapshot;

    [[nodiscard]] bool resumable() const noexcept {
        return payload_kind == CheckpointPayloadKind::FullSnapshot && snapshot.has_value();
    }
};

class CheckpointManager {
public:
    CheckpointId append(const Checkpoint& checkpoint, std::optional<RunSnapshot> snapshot);
    [[nodiscard]] std::optional<CheckpointRecord> get(CheckpointId checkpoint_id) const;
    [[nodiscard]] const std::vector<CheckpointRecord>& records() const noexcept;
    [[nodiscard]] std::vector<Checkpoint> checkpoints_for_run(RunId run_id) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t resumable_count() const noexcept;
    [[nodiscard]] std::optional<Checkpoint> latest_resumable_for_run(
        RunId run_id,
        CheckpointId at_or_before
    ) const;
    void enable_persistence(std::string path);
    [[nodiscard]] bool persistence_enabled() const noexcept;
    [[nodiscard]] const std::string& persistence_path() const noexcept;
    [[nodiscard]] std::size_t load_persisted_records();

private:
    void persist_locked() const;

    mutable std::mutex mutex_;
    std::vector<CheckpointRecord> records_;
    std::string persistence_path_;
};

[[nodiscard]] std::vector<std::byte> serialize_run_snapshot_bytes(const RunSnapshot& snapshot);
[[nodiscard]] RunSnapshot deserialize_run_snapshot_bytes(const std::vector<std::byte>& bytes);

class TraceSink {
public:
    void emit(const TraceEvent& event);
    [[nodiscard]] const std::vector<TraceEvent>& events() const noexcept;
    [[nodiscard]] std::vector<TraceEvent> events_for_run(RunId run_id) const;
    [[nodiscard]] std::vector<TraceEvent> events_for_run_since_sequence(
        RunId run_id,
        uint64_t next_sequence
    ) const;
    [[nodiscard]] uint64_t next_sequence() const;
    void clear();

private:
    mutable std::mutex mutex_;
    std::vector<TraceEvent> events_;
    uint64_t next_sequence_{1};
};

} // namespace agentcore

#endif // AGENTCORE_CHECKPOINT_H
