#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/graph/graph_ir.h"

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
    assert(state.fields.size() > kResult);
    assert(std::holds_alternative<BlobRef>(state.fields[kResult]));
    assert(
        engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.fields[kResult])) ==
        "execution-module-ok"
    );
    assert(state.fields.size() > kBranchId);
    assert(std::holds_alternative<int64_t>(state.fields[kGraphId]));
    assert(std::holds_alternative<int64_t>(state.fields[kNodeId]));
    assert(std::holds_alternative<int64_t>(state.fields[kBranchId]));
    assert(std::get<int64_t>(state.fields[kGraphId]) == 301);
    assert(std::get<int64_t>(state.fields[kNodeId]) == 1);
    assert(std::get<int64_t>(state.fields[kBranchId]) == 0);

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

    std::cout << "execution module tests passed" << std::endl;
    return 0;
}
