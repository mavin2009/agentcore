#include "agentcore/graph/graph_ir.h"
#include "agentcore/runtime/node_runtime.h"

#include <cassert>
#include <iostream>
#include <string_view>

namespace agentcore {

namespace {

NodeResult success_node(ExecutionContext&) {
    return NodeResult::success();
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

GraphDefinition make_valid_graph() {
    GraphDefinition graph;
    graph.id = 201;
    graph.name = "graph_module_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            success_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "branch_a", 0U, 0U, 0U, success_node, {}},
        NodeDefinition{3, NodeKind::Compute, "branch_b", 0U, 0U, 0U, success_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "join",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            success_node,
            {},
            std::vector<FieldMergeRule>{
                FieldMergeRule{7, JoinMergeStrategy::SumInt64}
            }
        },
        NodeDefinition{
            5,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            success_node,
            {}
        },
        NodeDefinition{
            6,
            NodeKind::Aggregate,
            "kg_reactive",
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph),
            0U,
            0U,
            success_node,
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    "writes",
                    "artifact"
                }
            }
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 10U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{3, 2, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{4, 3, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{5, 4, 5, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{6, 6, 5, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{3});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{4});
    graph.bind_outgoing_edges(4, std::vector<EdgeId>{5});
    graph.bind_outgoing_edges(6, std::vector<EdgeId>{6});
    graph.sort_edges_by_priority();
    return graph;
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    GraphDefinition graph = make_valid_graph();
    std::string error_message;
    assert(graph.validate(&error_message));
    graph.compile_runtime();

    const NodeDefinition* fanout = graph.find_node(1);
    assert(fanout != nullptr);
    const std::vector<EdgeId> outgoing = graph.outgoing_edges(*fanout);
    assert(outgoing.size() == 2U);
    assert(outgoing[0] == 2U);
    assert(outgoing[1] == 1U);
    const auto kg_candidates = graph.candidate_triple_subscriptions("", "writes", "artifact");
    assert(kg_candidates.size() == 1U);
    const CompiledKnowledgeSubscription* kg_subscription = graph.knowledge_subscription(kg_candidates[0]);
    assert(kg_subscription != nullptr);
    assert(graph.nodes[kg_subscription->node_index].id == 6U);

    GraphDefinition duplicate_rule_graph = graph;
    duplicate_rule_graph.nodes[3].field_merge_rules.push_back(
        FieldMergeRule{7, JoinMergeStrategy::MaxInt64}
    );
    error_message.clear();
    assert(!duplicate_rule_graph.validate(&error_message));
    assert(contains(error_message, "duplicate merge rule"));

    GraphDefinition invalid_policy_graph = graph;
    invalid_policy_graph.nodes[1].field_merge_rules.push_back(
        FieldMergeRule{9, JoinMergeStrategy::RequireEqual}
    );
    error_message.clear();
    assert(!invalid_policy_graph.validate(&error_message));
    assert(contains(error_message, "join barrier"));

    GraphDefinition missing_kg_policy_graph = graph;
    missing_kg_policy_graph.nodes[5].policy_flags = 0U;
    error_message.clear();
    assert(!missing_kg_policy_graph.validate(&error_message));
    assert(contains(error_message, "ReactToKnowledgeGraph"));

    GraphDefinition wildcard_kg_graph = graph;
    wildcard_kg_graph.nodes[5].knowledge_subscriptions = std::vector<KnowledgeSubscription>{
        KnowledgeSubscription{KnowledgeSubscriptionKind::TriplePattern, {}, {}, {}, {}}
    };
    error_message.clear();
    assert(!wildcard_kg_graph.validate(&error_message));
    assert(contains(error_message, "must constrain"));

    GraphDefinition invalid_subgraph_graph = graph;
    invalid_subgraph_graph.nodes[1].kind = NodeKind::Subgraph;
    invalid_subgraph_graph.nodes[1].executor = nullptr;
    invalid_subgraph_graph.nodes[1].subgraph = SubgraphBinding{};
    invalid_subgraph_graph.nodes[1].knowledge_subscriptions.clear();
    error_message.clear();
    assert(!invalid_subgraph_graph.validate(&error_message));
    assert(contains(error_message, "valid target graph binding"));

    GraphDefinition valid_subgraph_graph = graph;
    valid_subgraph_graph.nodes[1].kind = NodeKind::Subgraph;
    valid_subgraph_graph.nodes[1].executor = nullptr;
    valid_subgraph_graph.nodes[1].subgraph = SubgraphBinding{
        999U,
        "stable_namespace",
        std::vector<SubgraphStateBinding>{SubgraphStateBinding{1U, 2U}},
        std::vector<SubgraphStateBinding>{SubgraphStateBinding{3U, 4U}},
        false,
        5U
    };
    valid_subgraph_graph.nodes[1].knowledge_subscriptions.clear();
    error_message.clear();
    assert(valid_subgraph_graph.validate(&error_message));

    std::cout << "graph module tests passed" << std::endl;
    return 0;
}
