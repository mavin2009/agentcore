#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/graph/graph_ir.h"

#include <atomic>
#include <cassert>
#include <iostream>
#include <string>
#include <cstdio>
#include <variant>

namespace agentcore {

namespace {

enum ExecutionTestKey : StateKey {
    kResult = 0,
    kGraphId = 1,
    kNodeId = 2,
    kBranchId = 3
};

enum MemoizationLoopKey : StateKey {
    kMemoLoopInput = 0,
    kMemoLoopOutput = 1,
    kMemoLoopVisits = 2
};

enum MemoizationInvalidationKey : StateKey {
    kMemoInvalidationInput = 0,
    kMemoInvalidationOutput = 1,
    kMemoInvalidationStep = 2
};

std::atomic<int> g_memo_loop_invocations{0};
std::atomic<int> g_memo_invalidation_invocations{0};

int64_t read_int_field(const WorkflowState& state, StateKey key) {
    if (state.size() <= key) {
        return 0;
    }
    const Value value = state.load(key);
    if (!std::holds_alternative<int64_t>(value)) {
        return 0;
    }
    return std::get<int64_t>(value);
}

NodeResult write_result_node(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kResult,
        context.blobs.append_string("execution-module-ok")
    });
    patch.updates.push_back(FieldUpdate{kGraphId, static_cast<int64_t>(context.graph_id)});
    patch.updates.push_back(FieldUpdate{kNodeId, static_cast<int64_t>(context.node_id)});
    patch.updates.push_back(FieldUpdate{kBranchId, static_cast<int64_t>(context.branch_id)});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult memoized_loop_node(ExecutionContext& context) {
    g_memo_loop_invocations.fetch_add(1);
    const int64_t input_value = read_int_field(context.state, kMemoLoopInput);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kMemoLoopOutput, input_value * 2});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult memoized_loop_router_node(ExecutionContext& context) {
    const int64_t visits = read_int_field(context.state, kMemoLoopVisits);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kMemoLoopVisits, visits + 1});
    NodeResult result = NodeResult::success(std::move(patch), 1.0F);
    result.next_override = visits == 0 ? NodeId{1} : NodeId{3};
    return result;
}

NodeResult memoized_invalidation_node(ExecutionContext& context) {
    g_memo_invalidation_invocations.fetch_add(1);
    const int64_t input_value = read_int_field(context.state, kMemoInvalidationInput);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kMemoInvalidationOutput, input_value * 3});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult memoized_invalidation_router_node(ExecutionContext& context) {
    const int64_t step = read_int_field(context.state, kMemoInvalidationStep);
    StatePatch patch;
    NodeResult result = NodeResult::success({}, 1.0F);
    if (step == 0) {
        patch.updates.push_back(FieldUpdate{kMemoInvalidationInput, read_int_field(context.state, kMemoInvalidationInput) + 1});
        patch.updates.push_back(FieldUpdate{kMemoInvalidationStep, int64_t{1}});
        result.patch = std::move(patch);
        result.next_override = NodeId{1};
        return result;
    }

    result.next_override = NodeId{3};
    return result;
}

GraphDefinition make_execution_graph() {
    GraphDefinition graph;
    graph.id = 301;
    graph.name = "execution_module_graph";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "write_result", 0U, 0U, 0U, write_result_node, {}},
        NodeDefinition{
            2,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_memoization_loop_graph() {
    GraphDefinition graph;
    graph.id = 302;
    graph.name = "execution_module_memoization_loop_graph";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Compute,
            "memoized_loop",
            0U,
            0U,
            0U,
            memoized_loop_node,
            {},
            {},
            {},
            std::nullopt,
            NodeMemoizationPolicy{true, std::vector<StateKey>{kMemoLoopInput}, 8U}
        },
        NodeDefinition{2, NodeKind::Control, "memoized_loop_router", 0U, 0U, 0U, memoized_loop_router_node, {}},
        NodeDefinition{
            3,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_memoization_invalidation_graph() {
    GraphDefinition graph;
    graph.id = 303;
    graph.name = "execution_module_memoization_invalidation_graph";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Compute,
            "memoized_invalidation",
            0U,
            0U,
            0U,
            memoized_invalidation_node,
            {},
            {},
            {},
            std::nullopt,
            NodeMemoizationPolicy{true, std::vector<StateKey>{kMemoInvalidationInput}, 8U}
        },
        NodeDefinition{2, NodeKind::Control, "memoized_invalidation_router", 0U, 0U, 0U, memoized_invalidation_router_node, {}},
        NodeDefinition{
            3,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.sort_edges_by_priority();
    return graph;
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    GraphDefinition graph = make_execution_graph();
    std::string error_message;
    assert(graph.validate(&error_message));

    const std::string persistence_path = "agentcore-execution-module-checkpoints.bin";
    std::remove(persistence_path.c_str());

    ExecutionEngine engine(2);
    engine.enable_checkpoint_persistence(persistence_path);
    const RunId run_id = engine.start(graph, InputEnvelope{4U});
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);
    assert(engine.checkpoints().size() >= 2U);

    const WorkflowState& state = engine.state(run_id);
    assert(state.size() > kResult);
    assert(std::holds_alternative<BlobRef>(state.load(kResult)));
    assert(
        engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.load(kResult))) ==
        "execution-module-ok"
    );
    assert(state.size() > kBranchId);
    assert(std::holds_alternative<int64_t>(state.load(kGraphId)));
    assert(std::holds_alternative<int64_t>(state.load(kNodeId)));
    assert(std::holds_alternative<int64_t>(state.load(kBranchId)));
    assert(std::get<int64_t>(state.load(kGraphId)) == 301);
    assert(std::get<int64_t>(state.load(kNodeId)) == 1);
    assert(std::get<int64_t>(state.load(kBranchId)) == 0);

    const auto trace_events = engine.trace().events_for_run(run_id);
    const auto& records = engine.checkpoints().records();
    const auto final_record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(final_record.has_value());
    assert(final_record->resumable());
    const RunProofDigest proof = compute_run_proof_digest(*final_record, trace_events);
    assert(proof.combined_digest != 0U);
    assert(engine.checkpoints().resumable_count() >= 1U);

    ExecutionEngine restored_engine(1);
    restored_engine.register_graph(graph);
    restored_engine.enable_checkpoint_persistence(persistence_path);
    const std::size_t loaded_records = restored_engine.load_persisted_checkpoints();
    assert(loaded_records == records.size());

    const auto restored_record = restored_engine.checkpoints().get(result.last_checkpoint_id);
    assert(restored_record.has_value());
    assert(restored_record->resumable());
    const RunProofDigest restored_proof = compute_run_proof_digest(*restored_record, trace_events);
    assert(restored_proof.combined_digest == proof.combined_digest);

    std::remove(persistence_path.c_str());

    GraphDefinition memoization_loop_graph = make_memoization_loop_graph();
    error_message.clear();
    assert(memoization_loop_graph.validate(&error_message));
    ExecutionEngine memoization_loop_engine(1);
    InputEnvelope memoization_loop_input{3U};
    memoization_loop_input.initial_patch.updates.push_back(FieldUpdate{kMemoLoopInput, int64_t{5}});
    g_memo_loop_invocations.store(0);
    const RunId memoization_loop_run_id = memoization_loop_engine.start(
        memoization_loop_graph,
        memoization_loop_input
    );
    const RunResult memoization_loop_result = memoization_loop_engine.run_to_completion(memoization_loop_run_id);
    assert(memoization_loop_result.status == ExecutionStatus::Completed);
    assert(g_memo_loop_invocations.load() == 1);
    const WorkflowState& memoization_loop_state = memoization_loop_engine.state(memoization_loop_run_id);
    assert(std::get<int64_t>(memoization_loop_state.load(kMemoLoopOutput)) == 10);
    assert(std::get<int64_t>(memoization_loop_state.load(kMemoLoopVisits)) == 2);

    GraphDefinition memoization_invalidation_graph = make_memoization_invalidation_graph();
    error_message.clear();
    assert(memoization_invalidation_graph.validate(&error_message));
    ExecutionEngine memoization_invalidation_engine(1);
    InputEnvelope memoization_invalidation_input{3U};
    memoization_invalidation_input.initial_patch.updates.push_back(FieldUpdate{kMemoInvalidationInput, int64_t{3}});
    g_memo_invalidation_invocations.store(0);
    const RunId memoization_invalidation_run_id = memoization_invalidation_engine.start(
        memoization_invalidation_graph,
        memoization_invalidation_input
    );
    const RunResult memoization_invalidation_result = memoization_invalidation_engine.run_to_completion(
        memoization_invalidation_run_id
    );
    assert(memoization_invalidation_result.status == ExecutionStatus::Completed);
    assert(g_memo_invalidation_invocations.load() == 2);
    const WorkflowState& memoization_invalidation_state =
        memoization_invalidation_engine.state(memoization_invalidation_run_id);
    assert(std::get<int64_t>(memoization_invalidation_state.load(kMemoInvalidationInput)) == 4);
    assert(std::get<int64_t>(memoization_invalidation_state.load(kMemoInvalidationOutput)) == 12);

#if defined(AGENTCORE_HAVE_SQLITE3)
    const std::string sqlite_persistence_path = "agentcore-execution-module-checkpoints.sqlite";
    std::remove(sqlite_persistence_path.c_str());

    ExecutionEngine sqlite_engine(2);
    sqlite_engine.enable_sqlite_checkpoint_persistence(sqlite_persistence_path);
    const RunId sqlite_run_id = sqlite_engine.start(graph, InputEnvelope{4U});
    const RunResult sqlite_result = sqlite_engine.run_to_completion(sqlite_run_id);
    assert(sqlite_result.status == ExecutionStatus::Completed);
    assert(sqlite_engine.checkpoints().storage_kind() == "sqlite");

    ExecutionEngine restored_sqlite_engine(1);
    restored_sqlite_engine.register_graph(graph);
    restored_sqlite_engine.enable_sqlite_checkpoint_persistence(sqlite_persistence_path);
    const std::size_t sqlite_loaded_records = restored_sqlite_engine.load_persisted_checkpoints();
    assert(sqlite_loaded_records == sqlite_engine.checkpoints().size());

    const auto restored_sqlite_record = restored_sqlite_engine.checkpoints().get(sqlite_result.last_checkpoint_id);
    assert(restored_sqlite_record.has_value());
    assert(restored_sqlite_record->resumable());

    std::remove(sqlite_persistence_path.c_str());
#endif

    std::cout << "execution module tests passed" << std::endl;
    return 0;
}
