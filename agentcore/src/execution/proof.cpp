#include "agentcore/execution/proof.h"

#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace agentcore {

namespace {

class Fnv1a64 {
public:
    void update_bytes(const void* data, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash_ ^= static_cast<uint64_t>(bytes[index]);
            hash_ *= 1099511628211ULL;
        }
    }

    template <typename T>
    void update_pod(const T& value) {
        update_bytes(&value, sizeof(T));
    }

    void update_string(std::string_view value) {
        const uint64_t size = static_cast<uint64_t>(value.size());
        update_pod(size);
        if (!value.empty()) {
            update_bytes(value.data(), value.size());
        }
    }

    [[nodiscard]] uint64_t value() const noexcept {
        return hash_;
    }

private:
    uint64_t hash_{1469598103934665603ULL};
};

void hash_state_store(Fnv1a64& hasher, const StateStore& state_store) {
    std::ostringstream stream(std::ios::binary);
    state_store.serialize(stream);
    const std::string serialized = stream.str();
    hasher.update_string(serialized);
}

void hash_graph(Fnv1a64& hasher, const GraphDefinition& graph) {
    hasher.update_pod(graph.id);
    hasher.update_string(graph.name);
    hasher.update_pod(graph.entry);

    const uint64_t node_count = static_cast<uint64_t>(graph.nodes.size());
    hasher.update_pod(node_count);
    for (const NodeDefinition& node : graph.nodes) {
        hasher.update_pod(node.id);
        hasher.update_pod(static_cast<uint8_t>(node.kind));
        hasher.update_string(node.name);
        hasher.update_pod(node.policy_flags);
        hasher.update_pod(node.timeout_ms);
        hasher.update_pod(node.retry_limit);
        hasher.update_pod(node.outgoing_range.offset);
        hasher.update_pod(node.outgoing_range.count);
        const uint64_t merge_rule_count = static_cast<uint64_t>(node.field_merge_rules.size());
        hasher.update_pod(merge_rule_count);
        for (const FieldMergeRule& rule : node.field_merge_rules) {
            hasher.update_pod(rule.key);
            hasher.update_pod(static_cast<uint8_t>(rule.strategy));
        }
        const uint64_t subscription_count = static_cast<uint64_t>(node.knowledge_subscriptions.size());
        hasher.update_pod(subscription_count);
        for (const KnowledgeSubscription& subscription : node.knowledge_subscriptions) {
            hasher.update_pod(static_cast<uint8_t>(subscription.kind));
            hasher.update_string(subscription.entity_label);
            hasher.update_string(subscription.subject_label);
            hasher.update_string(subscription.relation);
            hasher.update_string(subscription.object_label);
        }
        hasher.update_pod(node.subgraph.has_value());
        if (node.subgraph.has_value()) {
            hasher.update_pod(node.subgraph->graph_id);
            hasher.update_string(node.subgraph->namespace_name);
            const uint64_t input_binding_count = static_cast<uint64_t>(node.subgraph->input_bindings.size());
            hasher.update_pod(input_binding_count);
            for (const SubgraphStateBinding& binding : node.subgraph->input_bindings) {
                hasher.update_pod(binding.parent_key);
                hasher.update_pod(binding.child_key);
            }
            const uint64_t output_binding_count = static_cast<uint64_t>(node.subgraph->output_bindings.size());
            hasher.update_pod(output_binding_count);
            for (const SubgraphStateBinding& binding : node.subgraph->output_bindings) {
                hasher.update_pod(binding.parent_key);
                hasher.update_pod(binding.child_key);
            }
            hasher.update_pod(node.subgraph->propagate_knowledge_graph);
            hasher.update_pod(node.subgraph->initial_field_count);
        }
    }

    const uint64_t edge_count = static_cast<uint64_t>(graph.edges.size());
    hasher.update_pod(edge_count);
    for (const EdgeDefinition& edge : graph.edges) {
        hasher.update_pod(edge.id);
        hasher.update_pod(edge.from);
        hasher.update_pod(edge.to);
        hasher.update_pod(static_cast<uint8_t>(edge.kind));
        hasher.update_pod(edge.priority);
    }

    const uint64_t edge_index_count = static_cast<uint64_t>(graph.edge_index_table.size());
    hasher.update_pod(edge_index_count);
    for (EdgeId edge_id : graph.edge_index_table) {
        hasher.update_pod(edge_id);
    }
}

void hash_pending_async(Fnv1a64& hasher, const std::optional<PendingAsyncOperation>& pending_async) {
    const bool has_value = pending_async.has_value();
    hasher.update_pod(has_value);
    if (!has_value) {
        return;
    }
    hasher.update_pod(static_cast<uint8_t>(pending_async->kind));
    hasher.update_pod(pending_async->handle_id);
}

void hash_pending_async_vector(
    Fnv1a64& hasher,
    const std::vector<PendingAsyncOperation>& pending_async_group
) {
    const uint64_t count = static_cast<uint64_t>(pending_async_group.size());
    hasher.update_pod(count);
    for (const PendingAsyncOperation& pending_async : pending_async_group) {
        hasher.update_pod(static_cast<uint8_t>(pending_async.kind));
        hasher.update_pod(pending_async.handle_id);
    }
}

void hash_pending_subgraph(
    Fnv1a64& hasher,
    const std::optional<PendingSubgraphExecution>& pending_subgraph
) {
    const bool has_value = pending_subgraph.has_value();
    hasher.update_pod(has_value);
    if (!has_value) {
        return;
    }

    hasher.update_pod(pending_subgraph->child_run_id);
    const uint64_t snapshot_size = static_cast<uint64_t>(pending_subgraph->snapshot_bytes.size());
    hasher.update_pod(snapshot_size);
    if (!pending_subgraph->snapshot_bytes.empty()) {
        hasher.update_bytes(
            pending_subgraph->snapshot_bytes.data(),
            pending_subgraph->snapshot_bytes.size()
        );
    }
}

void hash_tool_snapshot(Fnv1a64& hasher, const std::optional<AsyncToolSnapshot>& snapshot) {
    const bool has_value = snapshot.has_value();
    hasher.update_pod(has_value);
    if (!has_value) {
        return;
    }
    hasher.update_pod(snapshot->handle.id);
    hasher.update_string(snapshot->tool_name);
    hasher.update_pod(snapshot->result_ready);
    hasher.update_pod(snapshot->ok);
    hasher.update_pod(snapshot->flags);
    hasher.update_pod(snapshot->attempts);
    hasher.update_pod(snapshot->latency_ns);
    hasher.update_pod(static_cast<uint64_t>(snapshot->input.size()));
    if (!snapshot->input.empty()) {
        hasher.update_bytes(snapshot->input.data(), snapshot->input.size());
    }
    hasher.update_pod(static_cast<uint64_t>(snapshot->output.size()));
    if (!snapshot->output.empty()) {
        hasher.update_bytes(snapshot->output.data(), snapshot->output.size());
    }
}

void hash_tool_snapshot_vector(Fnv1a64& hasher, const std::vector<AsyncToolSnapshot>& snapshots) {
    const uint64_t count = static_cast<uint64_t>(snapshots.size());
    hasher.update_pod(count);
    for (const AsyncToolSnapshot& snapshot : snapshots) {
        hash_tool_snapshot(hasher, snapshot);
    }
}

void hash_model_snapshot(Fnv1a64& hasher, const std::optional<AsyncModelSnapshot>& snapshot) {
    const bool has_value = snapshot.has_value();
    hasher.update_pod(has_value);
    if (!has_value) {
        return;
    }
    hasher.update_pod(snapshot->handle.id);
    hasher.update_string(snapshot->model_name);
    hasher.update_pod(snapshot->max_tokens);
    hasher.update_pod(snapshot->result_ready);
    hasher.update_pod(snapshot->ok);
    hasher.update_pod(snapshot->confidence);
    hasher.update_pod(snapshot->token_usage);
    hasher.update_pod(snapshot->flags);
    hasher.update_pod(snapshot->attempts);
    hasher.update_pod(snapshot->latency_ns);
    hasher.update_pod(static_cast<uint64_t>(snapshot->prompt.size()));
    if (!snapshot->prompt.empty()) {
        hasher.update_bytes(snapshot->prompt.data(), snapshot->prompt.size());
    }
    hasher.update_pod(static_cast<uint64_t>(snapshot->schema.size()));
    if (!snapshot->schema.empty()) {
        hasher.update_bytes(snapshot->schema.data(), snapshot->schema.size());
    }
    hasher.update_pod(static_cast<uint64_t>(snapshot->output.size()));
    if (!snapshot->output.empty()) {
        hasher.update_bytes(snapshot->output.data(), snapshot->output.size());
    }
}

void hash_model_snapshot_vector(Fnv1a64& hasher, const std::vector<AsyncModelSnapshot>& snapshots) {
    const uint64_t count = static_cast<uint64_t>(snapshots.size());
    hasher.update_pod(count);
    for (const AsyncModelSnapshot& snapshot : snapshots) {
        hash_model_snapshot(hasher, snapshot);
    }
}

uint64_t compute_snapshot_digest(const RunSnapshot& snapshot) {
    Fnv1a64 hasher;
    hash_graph(hasher, snapshot.graph);
    hasher.update_pod(static_cast<uint8_t>(snapshot.status));
    const uint64_t runtime_config_payload_size =
        static_cast<uint64_t>(snapshot.runtime_config_payload.size());
    hasher.update_pod(runtime_config_payload_size);
    if (!snapshot.runtime_config_payload.empty()) {
        hasher.update_bytes(
            snapshot.runtime_config_payload.data(),
            snapshot.runtime_config_payload.size()
        );
    }
    hasher.update_pod(snapshot.next_branch_id);
    hasher.update_pod(snapshot.next_split_id);

    const uint64_t branch_count = static_cast<uint64_t>(snapshot.branches.size());
    hasher.update_pod(branch_count);
    for (const BranchSnapshot& branch : snapshot.branches) {
        hasher.update_pod(branch.frame.graph_id);
        hasher.update_pod(branch.frame.current_node);
        hasher.update_pod(branch.frame.step_index);
        hasher.update_pod(branch.frame.checkpoint_id);
        hasher.update_pod(branch.frame.active_branch_id);
        hasher.update_pod(static_cast<uint8_t>(branch.frame.status));
        hasher.update_pod(branch.retry_count);
        hash_pending_async(hasher, branch.pending_async);
        hash_pending_async_vector(hasher, branch.pending_async_group);
        hash_pending_subgraph(hasher, branch.pending_subgraph);
        hash_tool_snapshot_vector(hasher, branch.pending_tool_snapshots);
        hash_model_snapshot_vector(hasher, branch.pending_model_snapshots);
        hasher.update_pod(branch.reactive_root_node_id.has_value());
        if (branch.reactive_root_node_id.has_value()) {
            hasher.update_pod(*branch.reactive_root_node_id);
        }
        const uint64_t join_stack_size = static_cast<uint64_t>(branch.join_stack.size());
        hasher.update_pod(join_stack_size);
        for (uint32_t split_id : branch.join_stack) {
            hasher.update_pod(split_id);
        }
        hash_state_store(hasher, branch.state_store);
    }

    const uint64_t join_scope_count = static_cast<uint64_t>(snapshot.join_scopes.size());
    hasher.update_pod(join_scope_count);
    for (const JoinScopeSnapshot& join_scope : snapshot.join_scopes) {
        hasher.update_pod(join_scope.split_id);
        hasher.update_pod(join_scope.expected_branch_count);
        hasher.update_pod(join_scope.join_node_id);
        hasher.update_pod(join_scope.split_patch_log_offset);
        const uint64_t arrived_count = static_cast<uint64_t>(join_scope.arrived_branch_ids.size());
        hasher.update_pod(arrived_count);
        for (uint32_t branch_id : join_scope.arrived_branch_ids) {
            hasher.update_pod(branch_id);
        }
        hash_state_store(hasher, join_scope.base_state);
    }

    const uint64_t reactive_frontier_count = static_cast<uint64_t>(snapshot.reactive_frontiers.size());
    hasher.update_pod(reactive_frontier_count);
    for (const ReactiveFrontierSnapshot& frontier : snapshot.reactive_frontiers) {
        hasher.update_pod(frontier.node_id);
        hasher.update_pod(frontier.pending_rerun);
        hasher.update_pod(frontier.pending_rerun_seed.has_value());
        if (frontier.pending_rerun_seed.has_value()) {
            hasher.update_pod(frontier.pending_rerun_seed->step_index);
            hash_state_store(hasher, frontier.pending_rerun_seed->state_store);
        }
    }

    const uint64_t task_count = static_cast<uint64_t>(snapshot.pending_tasks.size());
    hasher.update_pod(task_count);
    for (const ScheduledTask& task : snapshot.pending_tasks) {
        hasher.update_pod(task.run_id);
        hasher.update_pod(task.node_id);
        hasher.update_pod(task.branch_id);
        hasher.update_pod(task.ready_at_ns);
    }

    return hasher.value();
}

uint64_t compute_trace_digest(const std::vector<TraceEvent>& trace_events) {
    Fnv1a64 hasher;
    const uint64_t event_count = static_cast<uint64_t>(trace_events.size());
    hasher.update_pod(event_count);
    for (const TraceEvent& event : trace_events) {
        hasher.update_pod(event.run_id);
        hasher.update_pod(event.graph_id);
        hasher.update_pod(event.node_id);
        hasher.update_pod(event.branch_id);
        hasher.update_pod(event.checkpoint_id);
        hasher.update_pod(static_cast<uint8_t>(event.result));
        hasher.update_pod(event.confidence);
        hasher.update_pod(event.patch_count);
        hasher.update_pod(event.flags);
        const uint64_t namespace_count = static_cast<uint64_t>(event.namespace_path.size());
        hasher.update_pod(namespace_count);
        for (const ExecutionNamespaceRef& namespace_ref : event.namespace_path) {
            hasher.update_pod(namespace_ref.graph_id);
            hasher.update_pod(namespace_ref.node_id);
        }
    }
    return hasher.value();
}

} // namespace

RunProofDigest compute_run_proof_digest(
    const RunSnapshot& snapshot,
    const std::vector<TraceEvent>& trace_events
) {
    const uint64_t snapshot_digest = compute_snapshot_digest(snapshot);
    const uint64_t trace_digest = compute_trace_digest(trace_events);

    Fnv1a64 combined;
    combined.update_pod(snapshot_digest);
    combined.update_pod(trace_digest);

    return RunProofDigest{
        snapshot_digest,
        trace_digest,
        combined.value()
    };
}

RunProofDigest compute_run_proof_digest(
    const CheckpointRecord& record,
    const std::vector<TraceEvent>& trace_events
) {
    if (!record.snapshot.has_value()) {
        throw std::invalid_argument("checkpoint record does not carry a resumable snapshot");
    }
    return compute_run_proof_digest(*record.snapshot, trace_events);
}

} // namespace agentcore
