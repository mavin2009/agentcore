#ifndef AGENTCORE_ENGINE_H
#define AGENTCORE_ENGINE_H

#include "agentcore/core/types.h"
#include "agentcore/execution/checkpoint.h"
#include "agentcore/execution/streaming/public_stream.h"
#include "agentcore/graph/graph_ir.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/node_runtime.h"
#include "agentcore/runtime/scheduler.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/state/state_store.h"
#include <cstddef>
#include <string>
#include <unordered_map>

namespace agentcore {

struct InputEnvelope {
    std::size_t initial_field_count{0};
    StatePatch initial_patch{};
    std::vector<std::byte> runtime_config_payload;
    std::optional<NodeId> entry_override;
};

struct StepResult {
    RunId run_id{0};
    NodeId node_id{0};
    uint32_t branch_id{0};
    uint64_t step_index{0};
    CheckpointId checkpoint_id{0};
    ExecutionStatus status{ExecutionStatus::NotStarted};
    NodeResult::Status node_status{NodeResult::Success};
    bool progressed{false};
    bool waiting{false};
    std::size_t enqueued_tasks{0};
    std::string message;
};

struct RunResult {
    RunId run_id{0};
    ExecutionStatus status{ExecutionStatus::NotStarted};
    uint64_t steps_executed{0};
    CheckpointId last_checkpoint_id{0};
};

struct ResumeResult {
    RunId run_id{0};
    CheckpointId restored_checkpoint_id{0};
    ExecutionStatus status{ExecutionStatus::NotStarted};
    bool resumed{false};
    std::string message;
};

struct CheckpointPolicy {
    uint64_t snapshot_interval_steps{64U};
    bool snapshot_on_wait{true};
    bool snapshot_on_terminal{true};
    bool snapshot_on_failure{true};
    bool snapshot_on_join_events{true};
};

class ExecutionEngine {
public:
    explicit ExecutionEngine(std::size_t worker_count = 0);

    void register_graph(const GraphDefinition& graph);
    void set_checkpoint_policy(CheckpointPolicy policy) noexcept;
    [[nodiscard]] const CheckpointPolicy& checkpoint_policy() const noexcept;
    void enable_checkpoint_persistence(std::string path);
    [[nodiscard]] std::size_t load_persisted_checkpoints();
    RunId start(const GraphDefinition&, const InputEnvelope&);
    StepResult step(RunId);
    RunResult run_to_completion(RunId);
    ResumeResult resume(CheckpointId);

    [[nodiscard]] const WorkflowState& state(RunId run_id, uint32_t branch_id = 0) const;
    [[nodiscard]] const StateStore& state_store(RunId run_id, uint32_t branch_id = 0) const;
    [[nodiscard]] const KnowledgeGraphStore& knowledge_graph(RunId run_id, uint32_t branch_id = 0) const;
    [[nodiscard]] const GraphDefinition& graph(RunId run_id) const;
    [[nodiscard]] ToolRegistry& tools() noexcept;
    [[nodiscard]] ModelRegistry& models() noexcept;
    [[nodiscard]] TraceSink& trace() noexcept;
    [[nodiscard]] const TraceSink& trace() const noexcept;
    [[nodiscard]] const CheckpointManager& checkpoints() const noexcept;
    [[nodiscard]] std::vector<StreamEvent> stream_events(
        RunId run_id,
        StreamCursor& cursor,
        const StreamReadOptions& options = {}
    ) const;

private:
    struct BranchRuntime {
        ExecutionFrame frame{};
        StateStore state_store{};
        ScratchArena scratch{};
        Deadline deadline{};
        CancellationToken cancel{};
        uint16_t retry_count{0};
        std::optional<PendingAsyncOperation> pending_async;
        std::vector<PendingAsyncOperation> pending_async_group;
        std::optional<PendingSubgraphExecution> pending_subgraph;
        std::vector<uint32_t> join_stack;
        std::optional<NodeId> reactive_root_node_id;
    };

    struct JoinScope {
        uint32_t split_id{0};
        uint32_t expected_branch_count{0};
        NodeId join_node_id{0};
        uint64_t split_patch_log_offset{0};
        StateStore base_state{};
        std::vector<uint32_t> arrived_branch_ids;
    };

    struct ReactiveRerunSeed {
        StateStore state_store{};
        uint64_t step_index{0};
    };

    struct ReactiveFrontierState {
        NodeId node_id{0};
        uint32_t active_branch_count{0};
        bool pending_rerun{false};
        std::optional<ReactiveRerunSeed> pending_rerun_seed;
    };

    struct RunRuntime {
        GraphDefinition graph{};
        ExecutionStatus status{ExecutionStatus::NotStarted};
        std::vector<std::byte> runtime_config_payload;
        std::unordered_map<uint32_t, BranchRuntime> branches;
        uint32_t next_branch_id{1};
        std::unordered_map<uint32_t, JoinScope> join_scopes;
        std::unordered_map<NodeId, ReactiveFrontierState> reactive_frontiers;
        uint32_t next_split_id{1};
    };

    struct TaskExecutionRecord {
        ScheduledTask task{};
        NodeResult node_result{};
        uint64_t started_at_ns{0};
        uint64_t ended_at_ns{0};
        std::string error_message;
        bool progressed{false};
        bool blocked_on_join{false};
        uint32_t blocked_join_scope_id{0};
    };

    [[nodiscard]] static uint64_t now_ns();
    [[nodiscard]] TaskExecutionRecord execute_task(RunRuntime& run, const ScheduledTask& task);
    [[nodiscard]] NodeResult execute_subgraph_node(
        RunId run_id,
        const RunRuntime& run,
        const NodeDefinition& node,
        BranchRuntime& branch
    );
    StepResult commit_task_execution(RunId run_id, RunRuntime& run, const TaskExecutionRecord& record);
    [[nodiscard]] RunSnapshot snapshot_run(RunId run_id, const RunRuntime& run) const;
    [[nodiscard]] bool should_capture_checkpoint_snapshot(
        const RunRuntime& run,
        const BranchRuntime& checkpoint_branch,
        const TaskExecutionRecord& record,
        uint32_t trace_flags,
        std::size_t enqueued_tasks
    ) const noexcept;
    [[nodiscard]] bool restore_run_from_snapshot(
        RunId run_id,
        const RunSnapshot& snapshot,
        std::string* error_message = nullptr,
        std::size_t* missing_async_handles = nullptr,
        bool restore_async_waiters = true,
        bool enqueue_paused_branches = true
    );
    void update_run_status(RunRuntime& run, RunId run_id);
    [[nodiscard]] GraphDefinition resolve_runtime_graph(const RunSnapshot& snapshot) const;
    void re_register_async_waiter(RunId run_id, uint32_t branch_id, const PendingAsyncOperation& pending_async);
    void borrow_runtime_registries(ToolRegistry& tools, ModelRegistry& models) noexcept;
    [[nodiscard]] ToolRegistry& runtime_tools() noexcept;
    [[nodiscard]] const ToolRegistry& runtime_tools() const noexcept;
    [[nodiscard]] ModelRegistry& runtime_models() noexcept;
    [[nodiscard]] const ModelRegistry& runtime_models() const noexcept;
    [[nodiscard]] static bool is_terminal_status(ExecutionStatus status) noexcept;
    void rebuild_reactive_frontier_state(RunRuntime& run);
    void spawn_reactive_branch(
        RunId run_id,
        RunRuntime& run,
        NodeId reactive_node_id,
        const StateStore& state_seed,
        const ExecutionFrame& frame_seed,
        std::size_t& enqueued_tasks,
        uint32_t& trace_flags
    );
    void mark_reactive_trigger(
        RunId run_id,
        RunRuntime& run,
        NodeId reactive_node_id,
        const StateStore& state_seed,
        const ExecutionFrame& frame_seed,
        std::size_t& enqueued_tasks,
        uint32_t& trace_flags
    );
    void finalize_reactive_frontier_for_branch(
        RunId run_id,
        RunRuntime& run,
        BranchRuntime& branch,
        std::size_t& enqueued_tasks,
        uint32_t& trace_flags
    );
    [[nodiscard]] const GraphDefinition* registered_graph(GraphId graph_id) const noexcept;

    std::unordered_map<RunId, RunRuntime> runs_;
    std::unordered_map<GraphId, GraphDefinition> graph_registry_;
    Scheduler scheduler_;
    CheckpointManager checkpoint_manager_;
    TraceSink trace_sink_;
    ToolRegistry tool_registry_;
    ModelRegistry model_registry_;
    ToolRegistry* borrowed_tool_registry_{nullptr};
    ModelRegistry* borrowed_model_registry_{nullptr};
    CheckpointPolicy checkpoint_policy_{};
    RunId next_run_id_{1};
};

} // namespace agentcore

#endif // AGENTCORE_ENGINE_H
