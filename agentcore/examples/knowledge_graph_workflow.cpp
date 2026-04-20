#include "agentcore/execution/engine.h"
#include "agentcore/graph/graph_ir.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace agentcore {

namespace {

enum KnowledgeGraphStateKey : StateKey {
    kGraphSummary = 0
};

NodeResult ingest_knowledge_node(ExecutionContext& context) {
    const InternedStringId agentcore = context.strings.intern("agentcore");
    const InternedStringId reference_runtime = context.strings.intern("reference_runtime");
    const InternedStringId knowledge_graphs = context.strings.intern("knowledge_graphs");
    const InternedStringId faster_than = context.strings.intern("faster_than");
    const InternedStringId supports = context.strings.intern("supports");

    StatePatch patch;
    patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
        agentcore,
        context.blobs.append_string("native runtime")
    });
    patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
        reference_runtime,
        context.blobs.append_string("python orchestration runtime")
    });
    patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
        knowledge_graphs,
        context.blobs.append_string("indexed graph memory")
    });
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        agentcore,
        faster_than,
        reference_runtime,
        context.blobs.append_string("native step loop")
    });
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        agentcore,
        supports,
        knowledge_graphs,
        context.blobs.append_string("first-class state patch")
    });

    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult summarize_knowledge_node(ExecutionContext& context) {
    const InternedStringId agentcore = context.strings.intern("agentcore");
    const InternedStringId supports = context.strings.intern("supports");
    const InternedStringId faster_than = context.strings.intern("faster_than");

    const std::vector<const KnowledgeTriple*> support_edges =
        context.knowledge_graph.match(agentcore, supports, std::nullopt);
    const std::vector<const KnowledgeTriple*> speed_edges =
        context.knowledge_graph.match(agentcore, faster_than, std::nullopt);

    std::string summary = "agentcore";
    if (!support_edges.empty()) {
        const KnowledgeEntity* object = context.knowledge_graph.find_entity(support_edges.front()->object);
        if (object != nullptr) {
            summary += " supports ";
            summary += std::string(context.strings.resolve(object->label));
        }
    }
    if (!speed_edges.empty()) {
        const KnowledgeEntity* object = context.knowledge_graph.find_entity(speed_edges.front()->object);
        if (object != nullptr) {
            summary += " and is faster than ";
            summary += std::string(context.strings.resolve(object->label));
        }
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kGraphSummary, context.blobs.append_string(summary)});
    return NodeResult::success(std::move(patch), 0.93F);
}

NodeResult finish_node(ExecutionContext&) {
    return NodeResult::success();
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    GraphDefinition graph;
    graph.id = 3;
    graph.name = "knowledge_graph_workflow";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "ingest_knowledge", 0U, 0U, 0U, ingest_knowledge_node, {}},
        NodeDefinition{2, NodeKind::Aggregate, "summarize_knowledge", 0U, 0U, 0U, summarize_knowledge_node, {}},
        NodeDefinition{3, NodeKind::Control, "finish", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, finish_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 2, 3, EdgeKind::OnSuccess, nullptr, 100U}
    };

    const std::vector<EdgeId> ingest_edges{1};
    const std::vector<EdgeId> summarize_edges{2};
    graph.bind_outgoing_edges(1, ingest_edges);
    graph.bind_outgoing_edges(2, summarize_edges);
    graph.sort_edges_by_priority();

    ExecutionEngine engine;
    InputEnvelope input;
    input.initial_field_count = 1;

    const RunId run_id = engine.start(graph, input);
    const RunResult result = engine.run_to_completion(run_id);
    const StateStore& state_store = engine.state_store(run_id);
    const BlobRef summary_ref = std::get<BlobRef>(state_store.get_current_state().load(kGraphSummary));

    std::cout << "knowledge_graph_workflow\n";
    std::cout << "status=" << static_cast<int>(result.status) << "\n";
    std::cout << "steps=" << result.steps_executed << "\n";
    std::cout << "entities=" << engine.knowledge_graph(run_id).entity_count() << "\n";
    std::cout << "triples=" << engine.knowledge_graph(run_id).triple_count() << "\n";
    std::cout << "summary=" << state_store.blobs().read_string(summary_ref) << "\n";
    return 0;
}
