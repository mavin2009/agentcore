#include "agentcore/execution/engine.h"
#include "agentcore/graph/graph_ir.h"

#include <cassert>
#include <iostream>
#include <variant>

namespace agentcore {

namespace {

enum ConsumerStateKey : StateKey {
    kMessage = 0
};

NodeResult write_message_node(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kMessage,
        context.blobs.append_string("consumer-smoke-ok")
    });
    return NodeResult::success(std::move(patch), 1.0F);
}

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    GraphDefinition graph;
    graph.id = 501;
    graph.name = "consumer_smoke_graph";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "write_message", 0U, 0U, 0U, write_message_node, {}},
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

    std::string error_message;
    assert(graph.validate(&error_message));

    ExecutionEngine engine(1);
    const RunId run_id = engine.start(graph, InputEnvelope{1U});
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const WorkflowState& state = engine.state(run_id);
    assert(state.fields.size() > kMessage);
    assert(std::holds_alternative<BlobRef>(state.fields[kMessage]));
    std::cout
        << engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.fields[kMessage]))
        << std::endl;
    return 0;
}
