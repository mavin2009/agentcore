#include "agentcore/execution/checkpoint.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <istream>
#include <memory>
#include <ostream>
#include <sstream>
#include <stdexcept>

#if defined(AGENTCORE_HAVE_SQLITE3)
#include <sqlite3.h>
#endif

namespace agentcore {

namespace {

constexpr uint64_t kCheckpointMagic = 0x41474350434B5054ULL;
constexpr uint32_t kCheckpointVersion = 10U;

template <typename T>
void write_pod(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!output) {
        throw std::runtime_error("failed to write checkpoint stream");
    }
}

template <typename T>
T read_pod(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!input) {
        throw std::runtime_error("failed to read checkpoint stream");
    }
    return value;
}

void write_string(std::ostream& output, std::string_view value) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(value.size()));
    if (!value.empty()) {
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        if (!output) {
            throw std::runtime_error("failed to write checkpoint string");
        }
    }
}

std::string read_string(std::istream& input) {
    const uint64_t size = read_pod<uint64_t>(input);
    std::string value(size, '\0');
    if (size != 0U) {
        input.read(value.data(), static_cast<std::streamsize>(size));
        if (!input) {
            throw std::runtime_error("failed to read checkpoint string");
        }
    }
    return value;
}

void write_bytes(std::ostream& output, const std::vector<std::byte>& bytes) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(bytes.size()));
    if (!bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
        if (!output) {
            throw std::runtime_error("failed to write checkpoint bytes");
        }
    }
}

std::vector<std::byte> read_bytes(std::istream& input) {
    const uint64_t size = read_pod<uint64_t>(input);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (size != 0U) {
        input.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(size)
        );
        if (!input) {
            throw std::runtime_error("failed to read checkpoint bytes");
        }
    }
    return bytes;
}

void write_u32_vector(std::ostream& output, const std::vector<uint32_t>& values) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(values.size()));
    for (uint32_t value : values) {
        write_pod(output, value);
    }
}

std::vector<uint32_t> read_u32_vector(std::istream& input) {
    const uint64_t count = read_pod<uint64_t>(input);
    std::vector<uint32_t> values;
    values.reserve(static_cast<std::size_t>(count));
    for (uint64_t index = 0; index < count; ++index) {
        values.push_back(read_pod<uint32_t>(input));
    }
    return values;
}

void write_pending_async(std::ostream& output, const std::optional<PendingAsyncOperation>& pending_async) {
    const bool has_value = pending_async.has_value();
    write_pod(output, has_value);
    if (has_value) {
        write_pod(output, static_cast<uint8_t>(pending_async->kind));
        write_pod(output, pending_async->handle_id);
    }
}

std::optional<PendingAsyncOperation> read_pending_async(std::istream& input) {
    if (!read_pod<bool>(input)) {
        return std::nullopt;
    }
    return PendingAsyncOperation{
        static_cast<AsyncOperationKind>(read_pod<uint8_t>(input)),
        read_pod<uint64_t>(input)
    };
}

void write_pending_async_vector(
    std::ostream& output,
    const std::vector<PendingAsyncOperation>& pending_async_group
) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(pending_async_group.size()));
    for (const PendingAsyncOperation& pending_async : pending_async_group) {
        write_pod(output, static_cast<uint8_t>(pending_async.kind));
        write_pod(output, pending_async.handle_id);
    }
}

std::vector<PendingAsyncOperation> read_pending_async_vector(std::istream& input) {
    const uint64_t count = read_pod<uint64_t>(input);
    std::vector<PendingAsyncOperation> pending_async_group;
    pending_async_group.reserve(static_cast<std::size_t>(count));
    for (uint64_t index = 0; index < count; ++index) {
        pending_async_group.push_back(PendingAsyncOperation{
            static_cast<AsyncOperationKind>(read_pod<uint8_t>(input)),
            read_pod<uint64_t>(input)
        });
    }
    return pending_async_group;
}

void write_pending_subgraph(
    std::ostream& output,
    const std::optional<PendingSubgraphExecution>& pending_subgraph
) {
    const bool has_value = pending_subgraph.has_value();
    write_pod(output, has_value);
    if (!has_value) {
        return;
    }

    write_pod(output, pending_subgraph->child_run_id);
    write_bytes(output, pending_subgraph->snapshot_bytes);
    write_string(output, pending_subgraph->session_id);
    write_pod(output, pending_subgraph->session_revision);
}

std::optional<PendingSubgraphExecution> read_pending_subgraph(std::istream& input) {
    if (!read_pod<bool>(input)) {
        return std::nullopt;
    }

    PendingSubgraphExecution pending_subgraph;
    pending_subgraph.child_run_id = read_pod<RunId>(input);
    pending_subgraph.snapshot_bytes = read_bytes(input);
    pending_subgraph.session_id = read_string(input);
    pending_subgraph.session_revision = read_pod<uint64_t>(input);
    return pending_subgraph;
}

void write_async_tool_snapshot(std::ostream& output, const std::optional<AsyncToolSnapshot>& snapshot) {
    const bool has_value = snapshot.has_value();
    write_pod(output, has_value);
    if (!has_value) {
        return;
    }

    write_pod(output, snapshot->handle.id);
    write_string(output, snapshot->tool_name);
    write_bytes(output, snapshot->input);
    write_pod(output, snapshot->result_ready);
    write_pod(output, snapshot->ok);
    write_bytes(output, snapshot->output);
    write_pod(output, snapshot->flags);
    write_pod(output, snapshot->attempts);
    write_pod(output, snapshot->latency_ns);
}

std::optional<AsyncToolSnapshot> read_async_tool_snapshot(std::istream& input) {
    if (!read_pod<bool>(input)) {
        return std::nullopt;
    }

    AsyncToolSnapshot snapshot;
    snapshot.handle = AsyncToolHandle{read_pod<uint64_t>(input)};
    snapshot.tool_name = read_string(input);
    snapshot.input = read_bytes(input);
    snapshot.result_ready = read_pod<bool>(input);
    snapshot.ok = read_pod<bool>(input);
    snapshot.output = read_bytes(input);
    snapshot.flags = read_pod<uint32_t>(input);
    snapshot.attempts = read_pod<uint16_t>(input);
    snapshot.latency_ns = read_pod<uint64_t>(input);
    return snapshot;
}

void write_async_tool_snapshot_vector(
    std::ostream& output,
    const std::vector<AsyncToolSnapshot>& snapshots
) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(snapshots.size()));
    for (const AsyncToolSnapshot& snapshot : snapshots) {
        write_async_tool_snapshot(output, snapshot);
    }
}

std::vector<AsyncToolSnapshot> read_async_tool_snapshot_vector(std::istream& input) {
    const uint64_t count = read_pod<uint64_t>(input);
    std::vector<AsyncToolSnapshot> snapshots;
    snapshots.reserve(static_cast<std::size_t>(count));
    for (uint64_t index = 0; index < count; ++index) {
        const auto snapshot = read_async_tool_snapshot(input);
        if (!snapshot.has_value()) {
            throw std::runtime_error("checkpoint encoded missing async tool snapshot entry");
        }
        snapshots.push_back(std::move(*snapshot));
    }
    return snapshots;
}

void write_async_model_snapshot(std::ostream& output, const std::optional<AsyncModelSnapshot>& snapshot) {
    const bool has_value = snapshot.has_value();
    write_pod(output, has_value);
    if (!has_value) {
        return;
    }

    write_pod(output, snapshot->handle.id);
    write_string(output, snapshot->model_name);
    write_bytes(output, snapshot->prompt);
    write_bytes(output, snapshot->schema);
    write_pod(output, snapshot->max_tokens);
    write_pod(output, snapshot->result_ready);
    write_pod(output, snapshot->ok);
    write_bytes(output, snapshot->output);
    write_pod(output, snapshot->confidence);
    write_pod(output, snapshot->token_usage);
    write_pod(output, snapshot->flags);
    write_pod(output, snapshot->attempts);
    write_pod(output, snapshot->latency_ns);
}

std::optional<AsyncModelSnapshot> read_async_model_snapshot(std::istream& input) {
    if (!read_pod<bool>(input)) {
        return std::nullopt;
    }

    AsyncModelSnapshot snapshot;
    snapshot.handle = AsyncModelHandle{read_pod<uint64_t>(input)};
    snapshot.model_name = read_string(input);
    snapshot.prompt = read_bytes(input);
    snapshot.schema = read_bytes(input);
    snapshot.max_tokens = read_pod<uint32_t>(input);
    snapshot.result_ready = read_pod<bool>(input);
    snapshot.ok = read_pod<bool>(input);
    snapshot.output = read_bytes(input);
    snapshot.confidence = read_pod<float>(input);
    snapshot.token_usage = read_pod<uint32_t>(input);
    snapshot.flags = read_pod<uint32_t>(input);
    snapshot.attempts = read_pod<uint16_t>(input);
    snapshot.latency_ns = read_pod<uint64_t>(input);
    return snapshot;
}

void write_async_model_snapshot_vector(
    std::ostream& output,
    const std::vector<AsyncModelSnapshot>& snapshots
) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(snapshots.size()));
    for (const AsyncModelSnapshot& snapshot : snapshots) {
        write_async_model_snapshot(output, snapshot);
    }
}

std::vector<AsyncModelSnapshot> read_async_model_snapshot_vector(std::istream& input) {
    const uint64_t count = read_pod<uint64_t>(input);
    std::vector<AsyncModelSnapshot> snapshots;
    snapshots.reserve(static_cast<std::size_t>(count));
    for (uint64_t index = 0; index < count; ++index) {
        const auto snapshot = read_async_model_snapshot(input);
        if (!snapshot.has_value()) {
            throw std::runtime_error("checkpoint encoded missing async model snapshot entry");
        }
        snapshots.push_back(std::move(*snapshot));
    }
    return snapshots;
}

void write_execution_frame(std::ostream& output, const ExecutionFrame& frame) {
    write_pod(output, frame.graph_id);
    write_pod(output, frame.current_node);
    write_pod(output, frame.step_index);
    write_pod(output, frame.checkpoint_id);
    write_pod(output, frame.active_branch_id);
    write_pod(output, static_cast<uint8_t>(frame.status));
}

ExecutionFrame read_execution_frame(std::istream& input) {
    return ExecutionFrame{
        read_pod<GraphId>(input),
        read_pod<NodeId>(input),
        read_pod<uint64_t>(input),
        read_pod<CheckpointId>(input),
        read_pod<uint32_t>(input),
        static_cast<ExecutionStatus>(read_pod<uint8_t>(input))
    };
}

void write_scheduled_task(std::ostream& output, const ScheduledTask& task) {
    write_pod(output, task.run_id);
    write_pod(output, task.node_id);
    write_pod(output, task.branch_id);
    write_pod(output, task.ready_at_ns);
}

ScheduledTask read_scheduled_task(std::istream& input) {
    return ScheduledTask{
        read_pod<RunId>(input),
        read_pod<NodeId>(input),
        read_pod<uint32_t>(input),
        read_pod<uint64_t>(input)
    };
}

void write_optional_node_id(std::ostream& output, std::optional<NodeId> node_id) {
    write_pod(output, node_id.has_value());
    if (node_id.has_value()) {
        write_pod(output, *node_id);
    }
}

std::optional<NodeId> read_optional_node_id(std::istream& input) {
    if (!read_pod<bool>(input)) {
        return std::nullopt;
    }
    return read_pod<NodeId>(input);
}

void write_graph(std::ostream& output, const GraphDefinition& graph) {
    write_pod(output, graph.id);
    write_string(output, graph.name);
    write_pod(output, graph.entry);

    write_pod<uint64_t>(output, static_cast<uint64_t>(graph.nodes.size()));
    for (const NodeDefinition& node : graph.nodes) {
        write_pod(output, node.id);
        write_pod(output, static_cast<uint8_t>(node.kind));
        write_string(output, node.name);
        write_pod(output, node.policy_flags);
        write_pod(output, node.timeout_ms);
        write_pod(output, node.retry_limit);
        write_pod(output, node.outgoing_range.offset);
        write_pod(output, node.outgoing_range.count);
        write_pod<uint64_t>(output, static_cast<uint64_t>(node.field_merge_rules.size()));
        for (const FieldMergeRule& rule : node.field_merge_rules) {
            write_pod(output, rule.key);
            write_pod(output, static_cast<uint8_t>(rule.strategy));
        }
        write_pod<uint64_t>(output, static_cast<uint64_t>(node.knowledge_subscriptions.size()));
        for (const KnowledgeSubscription& subscription : node.knowledge_subscriptions) {
            write_pod(output, static_cast<uint8_t>(subscription.kind));
            write_string(output, subscription.entity_label);
            write_string(output, subscription.subject_label);
            write_string(output, subscription.relation);
            write_string(output, subscription.object_label);
        }
        write_pod(output, node.subgraph.has_value());
        if (node.subgraph.has_value()) {
            write_pod(output, node.subgraph->graph_id);
            write_string(output, node.subgraph->namespace_name);
            write_pod<uint64_t>(output, static_cast<uint64_t>(node.subgraph->input_bindings.size()));
            for (const SubgraphStateBinding& binding : node.subgraph->input_bindings) {
                write_pod(output, binding.parent_key);
                write_pod(output, binding.child_key);
            }
            write_pod<uint64_t>(output, static_cast<uint64_t>(node.subgraph->output_bindings.size()));
            for (const SubgraphStateBinding& binding : node.subgraph->output_bindings) {
                write_pod(output, binding.parent_key);
                write_pod(output, binding.child_key);
            }
            write_pod(output, node.subgraph->propagate_knowledge_graph);
            write_pod(output, node.subgraph->initial_field_count);
            write_pod(output, static_cast<uint8_t>(node.subgraph->session_mode));
            write_pod(output, node.subgraph->session_id_source_key.has_value());
            if (node.subgraph->session_id_source_key.has_value()) {
                write_pod(output, *node.subgraph->session_id_source_key);
            }
        }
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(graph.edges.size()));
    for (const EdgeDefinition& edge : graph.edges) {
        write_pod(output, edge.id);
        write_pod(output, edge.from);
        write_pod(output, edge.to);
        write_pod(output, static_cast<uint8_t>(edge.kind));
        write_pod(output, edge.priority);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(graph.edge_index_table.size()));
    for (EdgeId edge_id : graph.edge_index_table) {
        write_pod(output, edge_id);
    }
}

GraphDefinition read_graph(std::istream& input) {
    GraphDefinition graph;
    graph.id = read_pod<GraphId>(input);
    graph.name = read_string(input);
    graph.entry = read_pod<NodeId>(input);

    const uint64_t node_count = read_pod<uint64_t>(input);
    graph.nodes.reserve(static_cast<std::size_t>(node_count));
    for (uint64_t index = 0; index < node_count; ++index) {
        graph.nodes.push_back(NodeDefinition{
            read_pod<NodeId>(input),
            static_cast<NodeKind>(read_pod<uint8_t>(input)),
            read_string(input),
            read_pod<uint32_t>(input),
            read_pod<uint32_t>(input),
            read_pod<uint16_t>(input),
            nullptr,
            EdgeRange{
                read_pod<uint32_t>(input),
                read_pod<uint32_t>(input)
            },
            {}
        });
        const uint64_t rule_count = read_pod<uint64_t>(input);
        graph.nodes.back().field_merge_rules.reserve(static_cast<std::size_t>(rule_count));
        for (uint64_t rule_index = 0; rule_index < rule_count; ++rule_index) {
            graph.nodes.back().field_merge_rules.push_back(FieldMergeRule{
                read_pod<StateKey>(input),
                static_cast<JoinMergeStrategy>(read_pod<uint8_t>(input))
            });
        }
        const uint64_t subscription_count = read_pod<uint64_t>(input);
        graph.nodes.back().knowledge_subscriptions.reserve(static_cast<std::size_t>(subscription_count));
        for (uint64_t subscription_index = 0; subscription_index < subscription_count; ++subscription_index) {
            graph.nodes.back().knowledge_subscriptions.push_back(KnowledgeSubscription{
                static_cast<KnowledgeSubscriptionKind>(read_pod<uint8_t>(input)),
                read_string(input),
                read_string(input),
                read_string(input),
                read_string(input)
            });
        }
        if (read_pod<bool>(input)) {
            SubgraphBinding subgraph;
            subgraph.graph_id = read_pod<GraphId>(input);
            subgraph.namespace_name = read_string(input);
            const uint64_t input_binding_count = read_pod<uint64_t>(input);
            subgraph.input_bindings.reserve(static_cast<std::size_t>(input_binding_count));
            for (uint64_t binding_index = 0; binding_index < input_binding_count; ++binding_index) {
                subgraph.input_bindings.push_back(SubgraphStateBinding{
                    read_pod<StateKey>(input),
                    read_pod<StateKey>(input)
                });
            }
            const uint64_t output_binding_count = read_pod<uint64_t>(input);
            subgraph.output_bindings.reserve(static_cast<std::size_t>(output_binding_count));
            for (uint64_t binding_index = 0; binding_index < output_binding_count; ++binding_index) {
                subgraph.output_bindings.push_back(SubgraphStateBinding{
                    read_pod<StateKey>(input),
                    read_pod<StateKey>(input)
                });
            }
            subgraph.propagate_knowledge_graph = read_pod<bool>(input);
            subgraph.initial_field_count = read_pod<uint32_t>(input);
            subgraph.session_mode = static_cast<SubgraphSessionMode>(read_pod<uint8_t>(input));
            if (read_pod<bool>(input)) {
                subgraph.session_id_source_key = read_pod<StateKey>(input);
            }
            graph.nodes.back().subgraph = std::move(subgraph);
        }
    }

    const uint64_t edge_count = read_pod<uint64_t>(input);
    graph.edges.reserve(static_cast<std::size_t>(edge_count));
    for (uint64_t index = 0; index < edge_count; ++index) {
        graph.edges.push_back(EdgeDefinition{
            read_pod<EdgeId>(input),
            read_pod<NodeId>(input),
            read_pod<NodeId>(input),
            static_cast<EdgeKind>(read_pod<uint8_t>(input)),
            nullptr,
            read_pod<uint16_t>(input)
        });
    }

    const uint64_t edge_index_count = read_pod<uint64_t>(input);
    graph.edge_index_table.reserve(static_cast<std::size_t>(edge_index_count));
    for (uint64_t index = 0; index < edge_index_count; ++index) {
        graph.edge_index_table.push_back(read_pod<EdgeId>(input));
    }

    graph.compile_runtime();
    return graph;
}

void write_checkpoint(std::ostream& output, const Checkpoint& checkpoint) {
    write_pod(output, checkpoint.run_id);
    write_pod(output, checkpoint.checkpoint_id);
    write_pod(output, checkpoint.node_id);
    write_pod(output, checkpoint.state_version);
    write_pod(output, checkpoint.patch_log_offset);
    write_pod(output, static_cast<uint8_t>(checkpoint.status));
    write_pod(output, checkpoint.branch_id);
    write_pod(output, checkpoint.step_index);
}

Checkpoint read_checkpoint(std::istream& input) {
    return Checkpoint{
        read_pod<RunId>(input),
        read_pod<CheckpointId>(input),
        read_pod<NodeId>(input),
        read_pod<uint64_t>(input),
        read_pod<uint64_t>(input),
        static_cast<ExecutionStatus>(read_pod<uint8_t>(input)),
        read_pod<uint32_t>(input),
        read_pod<uint64_t>(input)
    };
}

void write_branch_snapshot(std::ostream& output, const BranchSnapshot& branch) {
    write_execution_frame(output, branch.frame);
    branch.state_store.serialize(output);
    write_pod(output, branch.retry_count);
    write_pending_async(output, branch.pending_async);
    write_pending_async_vector(output, branch.pending_async_group);
    write_pending_subgraph(output, branch.pending_subgraph);
    write_async_tool_snapshot_vector(output, branch.pending_tool_snapshots);
    write_async_model_snapshot_vector(output, branch.pending_model_snapshots);
    write_u32_vector(output, branch.join_stack);
    write_optional_node_id(output, branch.reactive_root_node_id);
}

BranchSnapshot read_branch_snapshot(std::istream& input) {
    BranchSnapshot branch;
    branch.frame = read_execution_frame(input);
    branch.state_store = StateStore::deserialize(input);
    branch.retry_count = read_pod<uint16_t>(input);
    branch.pending_async = read_pending_async(input);
    branch.pending_async_group = read_pending_async_vector(input);
    branch.pending_subgraph = read_pending_subgraph(input);
    branch.pending_tool_snapshots = read_async_tool_snapshot_vector(input);
    branch.pending_model_snapshots = read_async_model_snapshot_vector(input);
    branch.join_stack = read_u32_vector(input);
    branch.reactive_root_node_id = read_optional_node_id(input);
    return branch;
}

void write_join_scope_snapshot(std::ostream& output, const JoinScopeSnapshot& scope) {
    write_pod(output, scope.split_id);
    write_pod(output, scope.expected_branch_count);
    write_pod(output, scope.join_node_id);
    write_pod(output, scope.split_patch_log_offset);
    scope.base_state.serialize(output);
    write_u32_vector(output, scope.arrived_branch_ids);
}

JoinScopeSnapshot read_join_scope_snapshot(std::istream& input) {
    JoinScopeSnapshot scope;
    scope.split_id = read_pod<uint32_t>(input);
    scope.expected_branch_count = read_pod<uint32_t>(input);
    scope.join_node_id = read_pod<NodeId>(input);
    scope.split_patch_log_offset = read_pod<uint64_t>(input);
    scope.base_state = StateStore::deserialize(input);
    scope.arrived_branch_ids = read_u32_vector(input);
    return scope;
}

void write_reactive_rerun_seed_snapshot(std::ostream& output, const ReactiveRerunSeedSnapshot& seed) {
    seed.state_store.serialize(output);
    write_pod(output, seed.step_index);
}

ReactiveRerunSeedSnapshot read_reactive_rerun_seed_snapshot(std::istream& input) {
    ReactiveRerunSeedSnapshot seed;
    seed.state_store = StateStore::deserialize(input);
    seed.step_index = read_pod<uint64_t>(input);
    return seed;
}

void write_reactive_frontier_snapshot(std::ostream& output, const ReactiveFrontierSnapshot& frontier) {
    write_pod(output, frontier.node_id);
    write_pod(output, frontier.pending_rerun);
    write_pod(output, frontier.pending_rerun_seed.has_value());
    if (frontier.pending_rerun_seed.has_value()) {
        write_reactive_rerun_seed_snapshot(output, *frontier.pending_rerun_seed);
    }
}

ReactiveFrontierSnapshot read_reactive_frontier_snapshot(std::istream& input) {
    ReactiveFrontierSnapshot frontier;
    frontier.node_id = read_pod<NodeId>(input);
    frontier.pending_rerun = read_pod<bool>(input);
    if (read_pod<bool>(input)) {
        frontier.pending_rerun_seed = read_reactive_rerun_seed_snapshot(input);
    }
    return frontier;
}

void write_committed_subgraph_session_snapshot(
    std::ostream& output,
    const CommittedSubgraphSessionSnapshot& session
) {
    write_pod(output, session.parent_node_id);
    write_string(output, session.session_id);
    write_pod(output, session.session_revision);
    write_bytes(output, session.snapshot_bytes);
}

CommittedSubgraphSessionSnapshot read_committed_subgraph_session_snapshot(std::istream& input) {
    CommittedSubgraphSessionSnapshot session;
    session.parent_node_id = read_pod<NodeId>(input);
    session.session_id = read_string(input);
    session.session_revision = read_pod<uint64_t>(input);
    session.snapshot_bytes = read_bytes(input);
    return session;
}

void write_run_snapshot(std::ostream& output, const RunSnapshot& snapshot) {
    write_graph(output, snapshot.graph);
    write_pod(output, static_cast<uint8_t>(snapshot.status));
    write_bytes(output, snapshot.runtime_config_payload);

    write_pod<uint64_t>(output, static_cast<uint64_t>(snapshot.branches.size()));
    for (const BranchSnapshot& branch : snapshot.branches) {
        write_branch_snapshot(output, branch);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(snapshot.join_scopes.size()));
    for (const JoinScopeSnapshot& scope : snapshot.join_scopes) {
        write_join_scope_snapshot(output, scope);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(snapshot.reactive_frontiers.size()));
    for (const ReactiveFrontierSnapshot& frontier : snapshot.reactive_frontiers) {
        write_reactive_frontier_snapshot(output, frontier);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(snapshot.committed_subgraph_sessions.size()));
    for (const CommittedSubgraphSessionSnapshot& session : snapshot.committed_subgraph_sessions) {
        write_committed_subgraph_session_snapshot(output, session);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(snapshot.pending_tasks.size()));
    for (const ScheduledTask& task : snapshot.pending_tasks) {
        write_scheduled_task(output, task);
    }

    write_pod(output, snapshot.next_branch_id);
    write_pod(output, snapshot.next_split_id);
}

RunSnapshot read_run_snapshot(std::istream& input) {
    RunSnapshot snapshot;
    snapshot.graph = read_graph(input);
    snapshot.status = static_cast<ExecutionStatus>(read_pod<uint8_t>(input));
    snapshot.runtime_config_payload = read_bytes(input);

    const uint64_t branch_count = read_pod<uint64_t>(input);
    snapshot.branches.reserve(static_cast<std::size_t>(branch_count));
    for (uint64_t index = 0; index < branch_count; ++index) {
        snapshot.branches.push_back(read_branch_snapshot(input));
    }

    const uint64_t join_scope_count = read_pod<uint64_t>(input);
    snapshot.join_scopes.reserve(static_cast<std::size_t>(join_scope_count));
    for (uint64_t index = 0; index < join_scope_count; ++index) {
        snapshot.join_scopes.push_back(read_join_scope_snapshot(input));
    }

    const uint64_t frontier_count = read_pod<uint64_t>(input);
    snapshot.reactive_frontiers.reserve(static_cast<std::size_t>(frontier_count));
    for (uint64_t index = 0; index < frontier_count; ++index) {
        snapshot.reactive_frontiers.push_back(read_reactive_frontier_snapshot(input));
    }

    const uint64_t session_count = read_pod<uint64_t>(input);
    snapshot.committed_subgraph_sessions.reserve(static_cast<std::size_t>(session_count));
    for (uint64_t index = 0; index < session_count; ++index) {
        snapshot.committed_subgraph_sessions.push_back(read_committed_subgraph_session_snapshot(input));
    }

    const uint64_t task_count = read_pod<uint64_t>(input);
    snapshot.pending_tasks.reserve(static_cast<std::size_t>(task_count));
    for (uint64_t index = 0; index < task_count; ++index) {
        snapshot.pending_tasks.push_back(read_scheduled_task(input));
    }

    snapshot.next_branch_id = read_pod<uint32_t>(input);
    snapshot.next_split_id = read_pod<uint32_t>(input);
    return snapshot;
}

void write_checkpoint_record(std::ostream& output, const CheckpointRecord& record) {
    write_checkpoint(output, record.checkpoint);
    write_pod(output, static_cast<uint8_t>(record.payload_kind));
    if (record.payload_kind == CheckpointPayloadKind::FullSnapshot) {
        if (!record.snapshot.has_value()) {
            throw std::runtime_error("full snapshot checkpoint missing run snapshot payload");
        }
        write_run_snapshot(output, *record.snapshot);
    }
}

CheckpointRecord read_checkpoint_record(std::istream& input) {
    CheckpointRecord record;
    record.checkpoint = read_checkpoint(input);
    record.payload_kind = static_cast<CheckpointPayloadKind>(read_pod<uint8_t>(input));
    if (record.payload_kind == CheckpointPayloadKind::FullSnapshot) {
        record.snapshot = read_run_snapshot(input);
    } else {
        record.payload_kind = CheckpointPayloadKind::MetadataOnly;
        record.snapshot.reset();
    }
    return record;
}

std::vector<std::byte> serialize_checkpoint_records_bytes(const std::vector<CheckpointRecord>& records) {
    std::ostringstream output;
    write_pod<uint64_t>(output, kCheckpointMagic);
    write_pod<uint32_t>(output, kCheckpointVersion);
    write_pod<uint64_t>(output, static_cast<uint64_t>(records.size()));
    for (const CheckpointRecord& record : records) {
        write_checkpoint_record(output, record);
    }

    const std::string serialized = output.str();
    std::vector<std::byte> bytes(serialized.size());
    if (!serialized.empty()) {
        std::memcpy(bytes.data(), serialized.data(), serialized.size());
    }
    return bytes;
}

std::vector<CheckpointRecord> deserialize_checkpoint_records_bytes(const std::vector<std::byte>& bytes) {
    std::string serialized(bytes.size(), '\0');
    if (!bytes.empty()) {
        std::memcpy(serialized.data(), bytes.data(), bytes.size());
    }

    std::istringstream input(serialized);
    const uint64_t magic = read_pod<uint64_t>(input);
    const uint32_t version = read_pod<uint32_t>(input);
    if (magic != kCheckpointMagic || version != kCheckpointVersion) {
        throw std::runtime_error("unsupported checkpoint persistence format");
    }

    const uint64_t record_count = read_pod<uint64_t>(input);
    std::vector<CheckpointRecord> records;
    records.reserve(static_cast<std::size_t>(record_count));
    for (uint64_t index = 0; index < record_count; ++index) {
        records.push_back(read_checkpoint_record(input));
    }
    return records;
}

class FileCheckpointStorageBackend final : public CheckpointStorageBackend {
public:
    explicit FileCheckpointStorageBackend(std::string path) : path_(std::move(path)) {}

    void replace_all(const std::vector<CheckpointRecord>& records) override {
        if (path_.empty()) {
            return;
        }

        const std::vector<std::byte> bytes = serialize_checkpoint_records_bytes(records);
        const std::string temp_path = path_ + ".tmp";
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to open checkpoint temp file");
        }

        if (!bytes.empty()) {
            output.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("failed to flush checkpoint temp file");
        }
        output.close();

        std::remove(path_.c_str());
        if (std::rename(temp_path.c_str(), path_.c_str()) != 0) {
            throw std::runtime_error("failed to install checkpoint persistence file");
        }
    }

    [[nodiscard]] std::vector<CheckpointRecord> load_all() const override {
        if (path_.empty()) {
            return {};
        }

        std::ifstream input(path_, std::ios::binary);
        if (!input) {
            return {};
        }

        const std::string serialized{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        };
        std::vector<std::byte> bytes(serialized.size());
        if (!serialized.empty()) {
            std::memcpy(bytes.data(), serialized.data(), serialized.size());
        }
        return deserialize_checkpoint_records_bytes(bytes);
    }

    [[nodiscard]] std::string kind() const override {
        return "file";
    }

    [[nodiscard]] const std::string& location() const noexcept override {
        return path_;
    }

private:
    std::string path_;
};

#if defined(AGENTCORE_HAVE_SQLITE3)
class SqliteStatement final {
public:
    SqliteStatement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }
    }

    ~SqliteStatement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    SqliteStatement(const SqliteStatement&) = delete;
    auto operator=(const SqliteStatement&) -> SqliteStatement& = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_{nullptr};
};

class SqliteDatabase final {
public:
    explicit SqliteDatabase(const std::string& path) {
        if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
            std::string message = db_ == nullptr ? "failed to open sqlite database" : sqlite3_errmsg(db_);
            if (db_ != nullptr) {
                sqlite3_close(db_);
                db_ = nullptr;
            }
            throw std::runtime_error(message);
        }
        execute("PRAGMA journal_mode=WAL;");
        execute("PRAGMA synchronous=FULL;");
        execute(
            "CREATE TABLE IF NOT EXISTS checkpoint_records ("
            " checkpoint_id INTEGER PRIMARY KEY,"
            " run_id INTEGER NOT NULL,"
            " payload_kind INTEGER NOT NULL,"
            " record_blob BLOB NOT NULL"
            ");"
        );
        execute(
            "CREATE INDEX IF NOT EXISTS idx_checkpoint_records_run_id"
            " ON checkpoint_records(run_id, checkpoint_id);"
        );
    }

    ~SqliteDatabase() {
        if (db_ != nullptr) {
            sqlite3_close(db_);
        }
    }

    SqliteDatabase(const SqliteDatabase&) = delete;
    auto operator=(const SqliteDatabase&) -> SqliteDatabase& = delete;

    [[nodiscard]] sqlite3* get() const noexcept { return db_; }

    void execute(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error == nullptr ? sqlite3_errmsg(db_) : error;
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

private:
    sqlite3* db_{nullptr};
};

class SqliteCheckpointStorageBackend final : public CheckpointStorageBackend {
public:
    explicit SqliteCheckpointStorageBackend(std::string path) : path_(std::move(path)) {}

    void replace_all(const std::vector<CheckpointRecord>& records) override {
        if (path_.empty()) {
            return;
        }

        SqliteDatabase database(path_);
        database.execute("BEGIN IMMEDIATE TRANSACTION;");
        try {
            database.execute("DELETE FROM checkpoint_records;");
            SqliteStatement insert(
                database.get(),
                "INSERT INTO checkpoint_records"
                " (checkpoint_id, run_id, payload_kind, record_blob)"
                " VALUES (?1, ?2, ?3, ?4);"
            );

            for (const CheckpointRecord& record : records) {
                const std::vector<std::byte> record_bytes =
                    serialize_checkpoint_records_bytes(std::vector<CheckpointRecord>{record});
                sqlite3_reset(insert.get());
                sqlite3_clear_bindings(insert.get());
                if (sqlite3_bind_int64(insert.get(), 1, static_cast<sqlite3_int64>(record.checkpoint.checkpoint_id)) != SQLITE_OK ||
                    sqlite3_bind_int64(insert.get(), 2, static_cast<sqlite3_int64>(record.checkpoint.run_id)) != SQLITE_OK ||
                    sqlite3_bind_int(insert.get(), 3, static_cast<int>(record.payload_kind)) != SQLITE_OK ||
                    sqlite3_bind_blob(
                        insert.get(),
                        4,
                        record_bytes.data(),
                        static_cast<int>(record_bytes.size()),
                        SQLITE_TRANSIENT
                    ) != SQLITE_OK) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
                if (sqlite3_step(insert.get()) != SQLITE_DONE) {
                    throw std::runtime_error(sqlite3_errmsg(database.get()));
                }
            }
            database.execute("COMMIT;");
        } catch (...) {
            try {
                database.execute("ROLLBACK;");
            } catch (...) {
            }
            throw;
        }
    }

    [[nodiscard]] std::vector<CheckpointRecord> load_all() const override {
        if (path_.empty()) {
            return {};
        }

        SqliteDatabase database(path_);
        SqliteStatement query(
            database.get(),
            "SELECT record_blob FROM checkpoint_records ORDER BY checkpoint_id ASC;"
        );

        std::vector<CheckpointRecord> records;
        while (true) {
            const int step = sqlite3_step(query.get());
            if (step == SQLITE_DONE) {
                break;
            }
            if (step != SQLITE_ROW) {
                throw std::runtime_error(sqlite3_errmsg(database.get()));
            }

            const void* blob = sqlite3_column_blob(query.get(), 0);
            const int size = sqlite3_column_bytes(query.get(), 0);
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            if (size > 0 && blob != nullptr) {
                std::memcpy(bytes.data(), blob, static_cast<std::size_t>(size));
            }
            std::vector<CheckpointRecord> decoded = deserialize_checkpoint_records_bytes(bytes);
            if (decoded.size() != 1U) {
                throw std::runtime_error("sqlite checkpoint row did not decode to exactly one record");
            }
            records.push_back(std::move(decoded.front()));
        }
        return records;
    }

    [[nodiscard]] std::string kind() const override {
        return "sqlite";
    }

    [[nodiscard]] const std::string& location() const noexcept override {
        return path_;
    }

private:
    std::string path_;
};
#endif

} // namespace

std::vector<std::byte> serialize_run_snapshot_bytes(const RunSnapshot& snapshot) {
    std::ostringstream output;
    write_run_snapshot(output, snapshot);
    const std::string serialized = output.str();
    std::vector<std::byte> bytes(serialized.size());
    if (!serialized.empty()) {
        std::memcpy(bytes.data(), serialized.data(), serialized.size());
    }
    return bytes;
}

RunSnapshot deserialize_run_snapshot_bytes(const std::vector<std::byte>& bytes) {
    std::string serialized(bytes.size(), '\0');
    if (!bytes.empty()) {
        std::memcpy(serialized.data(), bytes.data(), bytes.size());
    }
    std::istringstream input(serialized);
    return read_run_snapshot(input);
}

std::shared_ptr<CheckpointStorageBackend> make_file_checkpoint_storage(std::string path) {
    return std::make_shared<FileCheckpointStorageBackend>(std::move(path));
}

std::shared_ptr<CheckpointStorageBackend> make_sqlite_checkpoint_storage(std::string path) {
#if defined(AGENTCORE_HAVE_SQLITE3)
    return std::make_shared<SqliteCheckpointStorageBackend>(std::move(path));
#else
    (void)path;
    throw std::runtime_error("sqlite checkpoint storage is not available in this build");
#endif
}

CheckpointId CheckpointManager::append(const Checkpoint& checkpoint, std::optional<RunSnapshot> snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(CheckpointRecord{
        checkpoint,
        snapshot.has_value() ? CheckpointPayloadKind::FullSnapshot : CheckpointPayloadKind::MetadataOnly,
        std::move(snapshot)
    });
    persist_locked();
    return checkpoint.checkpoint_id;
}

std::optional<CheckpointRecord> CheckpointManager::get(CheckpointId checkpoint_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const CheckpointRecord& record : records_) {
        if (record.checkpoint.checkpoint_id == checkpoint_id) {
            return record;
        }
    }
    return std::nullopt;
}

const std::vector<CheckpointRecord>& CheckpointManager::records() const noexcept {
    return records_;
}

std::vector<Checkpoint> CheckpointManager::checkpoints_for_run(RunId run_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Checkpoint> checkpoints;
    for (const CheckpointRecord& record : records_) {
        if (record.checkpoint.run_id == run_id) {
            checkpoints.push_back(record.checkpoint);
        }
    }
    return checkpoints;
}

std::size_t CheckpointManager::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.size();
}

std::size_t CheckpointManager::resumable_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<std::size_t>(std::count_if(
        records_.begin(),
        records_.end(),
        [](const CheckpointRecord& record) {
            return record.resumable();
        }
    ));
}

std::optional<Checkpoint> CheckpointManager::latest_resumable_for_run(
    RunId run_id,
    CheckpointId at_or_before
) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto iterator = records_.rbegin(); iterator != records_.rend(); ++iterator) {
        if (iterator->checkpoint.run_id != run_id ||
            iterator->checkpoint.checkpoint_id > at_or_before ||
            !iterator->resumable()) {
            continue;
        }
        return iterator->checkpoint;
    }
    return std::nullopt;
}

void CheckpointManager::set_storage(std::shared_ptr<CheckpointStorageBackend> storage) {
    std::lock_guard<std::mutex> lock(mutex_);
    storage_ = std::move(storage);
    persistence_path_ = storage_ == nullptr ? std::string{} : storage_->location();
    if (storage_ != nullptr && !records_.empty()) {
        persist_locked();
    }
}

void CheckpointManager::enable_persistence(std::string path) {
    set_storage(make_file_checkpoint_storage(std::move(path)));
}

void CheckpointManager::enable_sqlite_persistence(std::string path) {
    set_storage(make_sqlite_checkpoint_storage(std::move(path)));
}

bool CheckpointManager::persistence_enabled() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_ != nullptr && !storage_->location().empty();
}

const std::string& CheckpointManager::persistence_path() const noexcept {
    return persistence_path_;
}

std::string CheckpointManager::storage_kind() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return storage_ == nullptr ? std::string{} : storage_->kind();
}

std::size_t CheckpointManager::load_persisted_records() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_ == nullptr) {
        return 0U;
    }

    records_ = storage_->load_all();
    return records_.size();
}

void CheckpointManager::persist_locked() const {
    if (storage_ == nullptr) {
        return;
    }
    storage_->replace_all(records_);
}

} // namespace agentcore
