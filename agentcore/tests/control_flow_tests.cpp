#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/graph/graph_ir.h"

#include <cassert>
#include <iostream>
#include <string>
#include <variant>

namespace agentcore {

namespace {

constexpr int kRouteCount = 16;
constexpr int64_t kMaxIterations = 64;

enum ControlFlowStateKey : StateKey {
    kIteration = 0,
    kRouteKey = 1,
    kAccumulator = 2,
    kShouldStop = 3
};

int64_t read_int_field(const WorkflowState& state, StateKey key) {
    if (state.fields.size() <= key || !std::holds_alternative<int64_t>(state.fields[key])) {
        return 0;
    }
    return std::get<int64_t>(state.fields[key]);
}

bool read_bool_field(const WorkflowState& state, StateKey key) {
    return state.fields.size() > key &&
        std::holds_alternative<bool>(state.fields[key]) &&
        std::get<bool>(state.fields[key]);
}

NodeResult router_node(ExecutionContext& context) {
    const int64_t iteration = read_int_field(context.state, kIteration);

    StatePatch patch;
    if (iteration >= kMaxIterations) {
        patch.updates.push_back(FieldUpdate{kShouldStop, true});
        return NodeResult::success(std::move(patch), 1.0F);
    }

    const int64_t next_iteration = iteration + 1;
    patch.updates.push_back(FieldUpdate{kIteration, next_iteration});
    patch.updates.push_back(FieldUpdate{kRouteKey, next_iteration % kRouteCount});
    patch.updates.push_back(FieldUpdate{kShouldStop, false});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

template <int RouteIndex>
bool route_selected(const WorkflowState& state) {
    return read_int_field(state, kRouteKey) == RouteIndex;
}

bool should_stop(const WorkflowState& state) {
    return read_bool_field(state, kShouldStop);
}

template <int RouteIndex>
NodeResult route_branch_node(ExecutionContext& context) {
    const int64_t accumulator = read_int_field(context.state, kAccumulator);

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kAccumulator, accumulator + static_cast<int64_t>(RouteIndex + 1)});
    return NodeResult::success(std::move(patch), 0.95F);
}

#define AGENTCORE_ROUTE_CASE(N) \
    case N: \
        return route_selected<N>;

ConditionFn route_condition_for_index(int route_index) {
    switch (route_index) {
        AGENTCORE_ROUTE_CASE(0)
        AGENTCORE_ROUTE_CASE(1)
        AGENTCORE_ROUTE_CASE(2)
        AGENTCORE_ROUTE_CASE(3)
        AGENTCORE_ROUTE_CASE(4)
        AGENTCORE_ROUTE_CASE(5)
        AGENTCORE_ROUTE_CASE(6)
        AGENTCORE_ROUTE_CASE(7)
        AGENTCORE_ROUTE_CASE(8)
        AGENTCORE_ROUTE_CASE(9)
        AGENTCORE_ROUTE_CASE(10)
        AGENTCORE_ROUTE_CASE(11)
        AGENTCORE_ROUTE_CASE(12)
        AGENTCORE_ROUTE_CASE(13)
        AGENTCORE_ROUTE_CASE(14)
        AGENTCORE_ROUTE_CASE(15)
        default:
            return nullptr;
    }
}

#undef AGENTCORE_ROUTE_CASE

#define AGENTCORE_BRANCH_CASE(N) \
    case N: \
        return route_branch_node<N>;

NodeExecutorFn route_executor_for_index(int route_index) {
    switch (route_index) {
        AGENTCORE_BRANCH_CASE(0)
        AGENTCORE_BRANCH_CASE(1)
        AGENTCORE_BRANCH_CASE(2)
        AGENTCORE_BRANCH_CASE(3)
        AGENTCORE_BRANCH_CASE(4)
        AGENTCORE_BRANCH_CASE(5)
        AGENTCORE_BRANCH_CASE(6)
        AGENTCORE_BRANCH_CASE(7)
        AGENTCORE_BRANCH_CASE(8)
        AGENTCORE_BRANCH_CASE(9)
        AGENTCORE_BRANCH_CASE(10)
        AGENTCORE_BRANCH_CASE(11)
        AGENTCORE_BRANCH_CASE(12)
        AGENTCORE_BRANCH_CASE(13)
        AGENTCORE_BRANCH_CASE(14)
        AGENTCORE_BRANCH_CASE(15)
        default:
            return nullptr;
    }
}

#undef AGENTCORE_BRANCH_CASE

GraphDefinition make_control_flow_graph() {
    GraphDefinition graph;
    graph.id = 701;
    graph.name = "compiled_control_flow";
    graph.entry = 1;

    graph.nodes.push_back(NodeDefinition{
        1,
        NodeKind::Control,
        "router",
        0U,
        0U,
        0U,
        router_node,
        {}
    });

    for (int route_index = 0; route_index < kRouteCount; ++route_index) {
        graph.nodes.push_back(NodeDefinition{
            static_cast<NodeId>(2 + route_index),
            NodeKind::Compute,
            "route_" + std::to_string(route_index),
            0U,
            0U,
            0U,
            route_executor_for_index(route_index),
            {}
        });
    }

    const NodeId stop_node_id = static_cast<NodeId>(2 + kRouteCount);
    graph.nodes.push_back(NodeDefinition{
        stop_node_id,
        NodeKind::Control,
        "stop",
        node_policy_mask(NodePolicyFlag::StopAfterNode),
        0U,
        0U,
        stop_node,
        {}
    });

    EdgeId next_edge_id = 1;
    std::vector<EdgeId> router_edges;
    router_edges.push_back(next_edge_id);
    graph.edges.push_back(EdgeDefinition{
        next_edge_id++,
        1,
        stop_node_id,
        EdgeKind::Conditional,
        should_stop,
        1000U
    });

    for (int route_index = 0; route_index < kRouteCount; ++route_index) {
        router_edges.push_back(next_edge_id);
        graph.edges.push_back(EdgeDefinition{
            next_edge_id++,
            1,
            static_cast<NodeId>(2 + route_index),
            EdgeKind::Conditional,
            route_condition_for_index(route_index),
            static_cast<uint16_t>(900U - route_index)
        });
    }

    graph.bind_outgoing_edges(1, router_edges);
    for (int route_index = 0; route_index < kRouteCount; ++route_index) {
        graph.edges.push_back(EdgeDefinition{
            next_edge_id,
            static_cast<NodeId>(2 + route_index),
            1,
            EdgeKind::Always,
            nullptr,
            100U
        });
        graph.bind_outgoing_edges(
            static_cast<NodeId>(2 + route_index),
            std::vector<EdgeId>{next_edge_id}
        );
        ++next_edge_id;
    }

    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

int64_t expected_accumulator() {
    int64_t total = 0;
    for (int64_t iteration = 1; iteration <= kMaxIterations; ++iteration) {
        total += (iteration % kRouteCount) + 1;
    }
    return total;
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    GraphDefinition graph = make_control_flow_graph();
    assert(graph.is_runtime_compiled());

    std::string error_message;
    assert(graph.validate(&error_message));

    const NodeDefinition* router = graph.find_node(1);
    assert(router != nullptr);
    assert(graph.compiled_routes_view(*router).size() == static_cast<std::size_t>(kRouteCount + 1));

    ExecutionEngine first_engine(1);
    const RunId first_run_id = first_engine.start(graph, InputEnvelope{4U});
    const RunResult first_result = first_engine.run_to_completion(first_run_id);
    assert(first_result.status == ExecutionStatus::Completed);

    const WorkflowState& first_state = first_engine.state(first_run_id);
    assert(read_int_field(first_state, kIteration) == kMaxIterations);
    assert(read_int_field(first_state, kAccumulator) == expected_accumulator());
    assert(read_bool_field(first_state, kShouldStop));

    const auto first_trace = first_engine.trace().events_for_run(first_run_id);
    const auto first_record = first_engine.checkpoints().get(first_result.last_checkpoint_id);
    assert(first_record.has_value());
    assert(first_record->resumable());
    const RunProofDigest first_digest = compute_run_proof_digest(
        *first_record,
        first_trace
    );
    assert(first_digest.combined_digest != 0U);

    ExecutionEngine second_engine(1);
    const RunId second_run_id = second_engine.start(graph, InputEnvelope{4U});
    const RunResult second_result = second_engine.run_to_completion(second_run_id);
    assert(second_result.status == ExecutionStatus::Completed);

    const WorkflowState& second_state = second_engine.state(second_run_id);
    assert(read_int_field(second_state, kAccumulator) == expected_accumulator());

    const auto second_trace = second_engine.trace().events_for_run(second_run_id);
    const auto second_record = second_engine.checkpoints().get(second_result.last_checkpoint_id);
    assert(second_record.has_value());
    assert(second_record->resumable());
    const RunProofDigest second_digest = compute_run_proof_digest(
        *second_record,
        second_trace
    );
    assert(second_digest.combined_digest == first_digest.combined_digest);

    std::cout << "control flow tests passed" << std::endl;
    return 0;
}
