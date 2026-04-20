#include "agentcore/execution/engine.h"
#include "agentcore/graph/graph_ir.h"

#include <iostream>
#include <string_view>
#include <vector>

namespace agentcore {

namespace {

enum PlannerStateKey : StateKey {
    kGoal = 0,
    kPlan = 1,
    kExecution = 2,
    kCompleted = 3
};

NodeResult planner_node(ExecutionContext& context) {
    const BlobRef prompt = context.blobs.append_string("plan a deterministic execution path");
    ModelInvocationContext model_context{context.blobs, context.strings};
    const ModelResponse response = context.models.invoke(
        ModelRequest{context.strings.intern("planner"), prompt, {}, 128},
        model_context
    );

    if (!response.ok) {
        return NodeResult{NodeResult::HardFail};
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPlan, response.output});
    return NodeResult::success(std::move(patch), response.confidence);
}

NodeResult executor_node(ExecutionContext& context) {
    const Value plan_value = context.state.size() > kPlan ? context.state.load(kPlan) : Value{};
    if (!std::holds_alternative<BlobRef>(plan_value)) {
        return NodeResult{NodeResult::HardFail};
    }

    ToolInvocationContext tool_context{context.blobs, context.strings};
    const ToolResponse response = context.tools.invoke(
        ToolRequest{context.strings.intern("executor"), std::get<BlobRef>(plan_value)},
        tool_context
    );

    if (!response.ok) {
        return NodeResult{NodeResult::HardFail};
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kExecution, response.output});
    patch.updates.push_back(FieldUpdate{kCompleted, true});
    return NodeResult::success(std::move(patch));
}

NodeResult finish_node(ExecutionContext&) {
    return NodeResult::success();
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    ExecutionEngine engine;
    engine.models().register_model("planner", [](const ModelRequest& request, ModelInvocationContext& context) {
        const std::string_view prompt = context.blobs.read_string(request.prompt);
        const BlobRef output = context.blobs.append_string(std::string("plan::") + std::string(prompt));
        return ModelResponse{true, output, 0.97F, 42U};
    });
    engine.tools().register_tool("executor", [](const ToolRequest& request, ToolInvocationContext& context) {
        const std::string_view plan = context.blobs.read_string(request.input);
        const BlobRef output = context.blobs.append_string(std::string("executed::") + std::string(plan));
        return ToolResponse{true, output, 0U};
    });

    GraphDefinition graph;
    graph.id = 1;
    graph.name = "planner_executor";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Model, "plan", 0U, 0U, 0U, planner_node, {}},
        NodeDefinition{2, NodeKind::Tool, "execute", 0U, 0U, 0U, executor_node, {}},
        NodeDefinition{3, NodeKind::Control, "finish", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, finish_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 2, 3, EdgeKind::OnSuccess, nullptr, 100U}
    };

    const std::vector<EdgeId> plan_edges{1};
    const std::vector<EdgeId> execute_edges{2};
    graph.bind_outgoing_edges(1, plan_edges);
    graph.bind_outgoing_edges(2, execute_edges);
    graph.sort_edges_by_priority();

    InputEnvelope input;
    input.initial_field_count = 4;

    const RunId run_id = engine.start(graph, input);
    const RunResult result = engine.run_to_completion(run_id);
    const StateStore& state_store = engine.state_store(run_id);
    const WorkflowState& state = state_store.get_current_state();

    const BlobRef plan_ref = std::get<BlobRef>(state.load(kPlan));
    const BlobRef execution_ref = std::get<BlobRef>(state.load(kExecution));

    std::cout << "planner_executor_graph\n";
    std::cout << "status=" << static_cast<int>(result.status) << "\n";
    std::cout << "steps=" << result.steps_executed << "\n";
    std::cout << "plan=" << state_store.blobs().read_string(plan_ref) << "\n";
    std::cout << "execution=" << state_store.blobs().read_string(execution_ref) << "\n";
    std::cout << "trace_events=" << engine.trace().events_for_run(run_id).size() << "\n";
    return 0;
}
