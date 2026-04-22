#include "agentcore/execution/subgraph/inline/runner.h"

#include <array>
#include <chrono>
#include <utility>
#include <vector>

namespace agentcore {

namespace {

constexpr RunId kInlineSubgraphRunId = 1U;
constexpr uint32_t kTraceFlagRecordedEffect = 1U << 25;

uint64_t inline_now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

class SelectedEdges {
public:
    void push(const EdgeDefinition* edge) {
        if (edge == nullptr) {
            return;
        }

        if (overflow_.empty() && inline_count_ < inline_edges_.size()) {
            inline_edges_[inline_count_++] = edge;
            return;
        }

        if (overflow_.empty()) {
            overflow_.reserve(inline_edges_.size() * 2U);
            overflow_.insert(
                overflow_.end(),
                inline_edges_.begin(),
                inline_edges_.begin() + static_cast<std::ptrdiff_t>(inline_count_)
            );
        }
        overflow_.push_back(edge);
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0U;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return overflow_.empty() ? inline_count_ : overflow_.size();
    }

    [[nodiscard]] const EdgeDefinition* front() const noexcept {
        return (*this)[0U];
    }

    [[nodiscard]] const EdgeDefinition* operator[](std::size_t index) const noexcept {
        if (overflow_.empty()) {
            return index < inline_count_ ? inline_edges_[index] : nullptr;
        }
        return index < overflow_.size() ? overflow_[index] : nullptr;
    }

private:
    std::array<const EdgeDefinition*, 4U> inline_edges_{};
    std::size_t inline_count_{0U};
    std::vector<const EdgeDefinition*> overflow_;
};

uint8_t select_mask_for_result(NodeResult::Status result) noexcept {
    switch (result) {
        case NodeResult::Success:
            return 1U << 0U;
        case NodeResult::SoftFail:
            return 1U << 1U;
        case NodeResult::HardFail:
            return 1U << 2U;
        case NodeResult::Waiting:
        case NodeResult::Cancelled:
            return 0U;
    }

    return 0U;
}

SelectedEdges select_edges(
    const GraphDefinition& graph,
    const NodeDefinition& node,
    const WorkflowState& state,
    const NodeResult& result
) {
    SelectedEdges selected;
    const uint8_t result_mask = select_mask_for_result(result.status);
    if (result_mask == 0U) {
        return selected;
    }

    if (const auto compiled_routes = graph.compiled_routes_view(node); !compiled_routes.empty()) {
        for (const CompiledEdgeRoute& route : compiled_routes) {
            if ((route.result_mask & result_mask) == 0U ||
                route.edge_index >= graph.edges.size()) {
                continue;
            }

            const EdgeDefinition& edge = graph.edges[route.edge_index];
            if (route.requires_condition &&
                (edge.condition == nullptr || !edge.condition(state))) {
                continue;
            }
            selected.push(&edge);
        }
        return selected;
    }

    for (EdgeId edge_id : graph.outgoing_edges_view(node)) {
        const EdgeDefinition* edge = graph.find_edge(edge_id);
        if (edge == nullptr) {
            continue;
        }
        if (edge->kind == EdgeKind::Always) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::OnSuccess && result.status == NodeResult::Success) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::OnSoftFail && result.status == NodeResult::SoftFail) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::OnHardFail && result.status == NodeResult::HardFail) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::Conditional &&
            result.status == NodeResult::Success &&
            edge->condition != nullptr &&
            edge->condition(state)) {
            selected.push(edge);
        }
    }

    return selected;
}

bool node_inline_single_branch_eligible(
    const GraphDefinition& graph,
    const NodeDefinition& node
) noexcept {
    if (node.kind == NodeKind::Subgraph || node.kind == NodeKind::Human) {
        return false;
    }
    if (has_node_policy(node.policy_flags, NodePolicyFlag::AllowFanOut) ||
        has_node_policy(node.policy_flags, NodePolicyFlag::JoinIncomingBranches) ||
        has_node_policy(node.policy_flags, NodePolicyFlag::CreateJoinScope) ||
        has_node_policy(node.policy_flags, NodePolicyFlag::ReactToKnowledgeGraph) ||
        has_node_policy(node.policy_flags, NodePolicyFlag::ReactToIntelligence)) {
        return false;
    }
    if (node.memoization.enabled()) {
        return false;
    }
    return graph.outgoing_edges_view(node).size() <= 1U;
}

InlineSingleBranchSubgraphResult finalize_inline_result(
    InlineSingleBranchSubgraphOutcome outcome,
    StateStore state_store,
    const ExecutionFrame& frame,
    uint16_t retry_count,
    std::optional<PendingAsyncOperation> pending_async,
    TraceSink& trace_sink,
    float confidence,
    std::string error_message = {}
) {
    return InlineSingleBranchSubgraphResult{
        outcome,
        std::move(state_store),
        frame,
        retry_count,
        pending_async,
        trace_sink.take_events_for_run(kInlineSubgraphRunId),
        confidence,
        std::move(error_message)
    };
}

} // namespace

bool inline_single_branch_subgraph_eligible(const GraphDefinition& graph) noexcept {
    for (const NodeDefinition& node : graph.nodes) {
        if (!node_inline_single_branch_eligible(graph, node)) {
            return false;
        }
    }
    return true;
}

InlineSingleBranchSubgraphResult run_inline_single_branch_subgraph(
    const GraphDefinition& graph,
    NodeId entry_node,
    StateStore initial_state,
    const std::vector<std::byte>& runtime_config_payload,
    ToolRegistry& tools,
    ModelRegistry& models,
    bool capture_trace
) {
    TraceSink trace_sink;
    ScratchArena scratch;
    CancellationToken cancel;
    StateStore state_store = std::move(initial_state);
    uint16_t retry_count = 0U;
    float confidence = 1.0F;

    ExecutionFrame frame;
    frame.graph_id = graph.id;
    frame.current_node = entry_node;
    frame.active_branch_id = 0U;
    frame.status = ExecutionStatus::Running;

    while (true) {
        const NodeDefinition* node = graph.find_node(frame.current_node);
        if (node == nullptr || node->executor == nullptr) {
            frame.status = ExecutionStatus::Failed;
            return finalize_inline_result(
                InlineSingleBranchSubgraphOutcome::Failed,
                std::move(state_store),
                frame,
                retry_count,
                std::nullopt,
                trace_sink,
                confidence,
                "inline subgraph node executor not found"
            );
        }

        const uint64_t started_at_ns = inline_now_ns();
        scratch.reset();

        std::vector<TaskRecord> recorded_effects;
        NodeResult node_result;
        std::string error_message;
        try {
            ExecutionContext context{
                state_store.get_current_state(),
                kInlineSubgraphRunId,
                graph.id,
                node->id,
                0U,
                runtime_config_payload,
                scratch,
                state_store.blobs(),
                state_store.strings(),
                state_store.knowledge_graph(),
                state_store.intelligence(),
                state_store.task_journal(),
                tools,
                models,
                trace_sink,
                Deadline(
                    node->timeout_ms == 0U
                        ? 0U
                        : inline_now_ns() + (static_cast<uint64_t>(node->timeout_ms) * 1000000ULL)
                ),
                cancel,
                std::nullopt,
                &recorded_effects
            };
            node_result = node->executor(context);
            if (!recorded_effects.empty()) {
                node_result.patch.task_records.insert(
                    node_result.patch.task_records.end(),
                    std::make_move_iterator(recorded_effects.begin()),
                    std::make_move_iterator(recorded_effects.end())
                );
                node_result.flags |= kTraceFlagRecordedEffect;
            }
        } catch (const std::exception& error) {
            node_result.status = NodeResult::HardFail;
            node_result.flags = kToolFlagHandlerException;
            error_message = error.what();
        } catch (...) {
            node_result.status = NodeResult::HardFail;
            node_result.flags = kToolFlagHandlerException;
            error_message = "unknown inline subgraph execution failure";
        }

        if (cancel.is_cancelled()) {
            node_result.status = NodeResult::Cancelled;
        }

        const uint64_t ended_at_ns = inline_now_ns();
        static_cast<void>(state_store.apply_with_summary(node_result.patch));
        frame.step_index += 1U;
        frame.current_node = node->id;
        frame.checkpoint_id = static_cast<CheckpointId>(frame.checkpoint_id + 1U);
        confidence = node_result.confidence;

        if (capture_trace) {
            trace_sink.emit(TraceEvent{
                0U,
                started_at_ns,
                ended_at_ns,
                kInlineSubgraphRunId,
                graph.id,
                node->id,
                0U,
                frame.checkpoint_id,
                node_result.status,
                confidence,
                static_cast<uint32_t>(node_result.patch.updates.size()),
                node_result.flags,
                {},
                0U,
                {}
            });
        }

        if (node_result.status == NodeResult::Waiting) {
            frame.status = ExecutionStatus::Paused;
            return finalize_inline_result(
                InlineSingleBranchSubgraphOutcome::Waiting,
                std::move(state_store),
                frame,
                retry_count,
                node_result.pending_async,
                trace_sink,
                confidence,
                std::move(error_message)
            );
        }

        if ((node_result.status == NodeResult::SoftFail ||
             node_result.status == NodeResult::HardFail) &&
            retry_count < node->retry_limit) {
            retry_count += 1U;
            frame.status = ExecutionStatus::Running;
            continue;
        }

        retry_count = 0U;
        if (node_result.status == NodeResult::HardFail) {
            frame.status = ExecutionStatus::Failed;
            return finalize_inline_result(
                InlineSingleBranchSubgraphOutcome::Failed,
                std::move(state_store),
                frame,
                retry_count,
                std::nullopt,
                trace_sink,
                confidence,
                std::move(error_message)
            );
        }
        if (node_result.status == NodeResult::Cancelled) {
            frame.status = ExecutionStatus::Cancelled;
            return finalize_inline_result(
                InlineSingleBranchSubgraphOutcome::Cancelled,
                std::move(state_store),
                frame,
                retry_count,
                std::nullopt,
                trace_sink,
                confidence,
                std::move(error_message)
            );
        }
        if (has_node_policy(node->policy_flags, NodePolicyFlag::StopAfterNode)) {
            frame.status = ExecutionStatus::Completed;
            return finalize_inline_result(
                InlineSingleBranchSubgraphOutcome::Completed,
                std::move(state_store),
                frame,
                retry_count,
                std::nullopt,
                trace_sink,
                confidence
            );
        }
        if (node_result.next_override.has_value()) {
            if (graph.find_node(*node_result.next_override) == nullptr) {
                frame.status = ExecutionStatus::Failed;
                return finalize_inline_result(
                    InlineSingleBranchSubgraphOutcome::Failed,
                    std::move(state_store),
                    frame,
                    retry_count,
                    std::nullopt,
                    trace_sink,
                    confidence,
                    "inline subgraph next_override target is invalid"
                );
            }
            frame.status = ExecutionStatus::Running;
            frame.current_node = *node_result.next_override;
            continue;
        }

        const SelectedEdges next_edges = select_edges(
            graph,
            *node,
            state_store.get_current_state(),
            node_result
        );
        if (next_edges.empty()) {
            frame.status = ExecutionStatus::Completed;
            return finalize_inline_result(
                InlineSingleBranchSubgraphOutcome::Completed,
                std::move(state_store),
                frame,
                retry_count,
                std::nullopt,
                trace_sink,
                confidence
            );
        }

        if (next_edges.size() != 1U || next_edges.front() == nullptr) {
            frame.status = ExecutionStatus::Failed;
            return finalize_inline_result(
                InlineSingleBranchSubgraphOutcome::Failed,
                std::move(state_store),
                frame,
                retry_count,
                std::nullopt,
                trace_sink,
                confidence,
                "inline subgraph encountered unsupported branch fanout"
            );
        }

        frame.status = ExecutionStatus::Running;
        frame.current_node = next_edges.front()->to;
    }
}

} // namespace agentcore
