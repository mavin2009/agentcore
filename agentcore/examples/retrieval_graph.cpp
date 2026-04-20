#include "agentcore/execution/engine.h"
#include "agentcore/graph/graph_ir.h"

#include <iostream>
#include <string_view>
#include <vector>

namespace agentcore {

namespace {

enum RetrievalStateKey : StateKey {
    kQuery = 0,
    kDocuments = 1,
    kAnswer = 2
};

NodeResult retrieve_node(ExecutionContext& context) {
    const BlobRef query = context.blobs.append_string("native agent graph runtime in C++");
    ToolInvocationContext tool_context{context.blobs, context.strings};
    const ToolResponse response = context.tools.invoke(
        ToolRequest{context.strings.intern("retriever"), query},
        tool_context
    );

    if (!response.ok) {
        return NodeResult{NodeResult::HardFail};
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kQuery, query});
    patch.updates.push_back(FieldUpdate{kDocuments, response.output});
    return NodeResult::success(std::move(patch), 0.9F);
}

NodeResult answer_node(ExecutionContext& context) {
    const Value documents_value = context.state.size() > kDocuments ? context.state.load(kDocuments) : Value{};
    if (!std::holds_alternative<BlobRef>(documents_value)) {
        return NodeResult{NodeResult::HardFail};
    }

    ModelInvocationContext model_context{context.blobs, context.strings};
    const ModelResponse response = context.models.invoke(
        ModelRequest{context.strings.intern("answerer"), std::get<BlobRef>(documents_value), {}, 256U},
        model_context
    );

    if (!response.ok) {
        return NodeResult{NodeResult::HardFail};
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kAnswer, response.output});
    return NodeResult::success(std::move(patch), response.confidence);
}

NodeResult done_node(ExecutionContext&) {
    return NodeResult::success();
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    ExecutionEngine engine;
    engine.tools().register_tool("retriever", [](const ToolRequest& request, ToolInvocationContext& context) {
        const std::string_view query = context.blobs.read_string(request.input);
        const BlobRef output = context.blobs.append_string(
            std::string("doc::native agent graph runtime for ") + std::string(query)
        );
        return ToolResponse{true, output, 0U};
    });
    engine.models().register_model("answerer", [](const ModelRequest& request, ModelInvocationContext& context) {
        const std::string_view documents = context.blobs.read_string(request.prompt);
        const BlobRef output = context.blobs.append_string(
            std::string("answer::") + std::string(documents)
        );
        return ModelResponse{true, output, 0.95F, 84U};
    });

    GraphDefinition graph;
    graph.id = 2;
    graph.name = "retrieval";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Tool, "retrieve", 0U, 0U, 0U, retrieve_node, {}},
        NodeDefinition{2, NodeKind::Model, "answer", 0U, 0U, 0U, answer_node, {}},
        NodeDefinition{3, NodeKind::Control, "done", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, done_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 2, 3, EdgeKind::OnSuccess, nullptr, 100U}
    };

    const std::vector<EdgeId> retrieve_edges{1};
    const std::vector<EdgeId> answer_edges{2};
    graph.bind_outgoing_edges(1, retrieve_edges);
    graph.bind_outgoing_edges(2, answer_edges);
    graph.sort_edges_by_priority();

    InputEnvelope input;
    input.initial_field_count = 3;

    const RunId run_id = engine.start(graph, input);
    const RunResult result = engine.run_to_completion(run_id);
    const StateStore& state_store = engine.state_store(run_id);
    const WorkflowState& state = state_store.get_current_state();
    const BlobRef answer_ref = std::get<BlobRef>(state.load(kAnswer));

    std::cout << "retrieval_graph\n";
    std::cout << "status=" << static_cast<int>(result.status) << "\n";
    std::cout << "steps=" << result.steps_executed << "\n";
    std::cout << "answer=" << state_store.blobs().read_string(answer_ref) << "\n";
    std::cout << "checkpoints=" << engine.checkpoints().checkpoints_for_run(run_id).size() << "\n";
    return 0;
}
