#include "agentcore/execution/subgraph/subgraph_runtime.h"

namespace agentcore {

namespace {

BlobRef rebase_blob_ref(const StateStore& source_state, StateStore& destination_state, BlobRef ref) {
    if (ref.empty()) {
        return {};
    }
    const std::vector<std::byte> bytes = source_state.blobs().read_bytes(ref);
    return destination_state.blobs().append(bytes.data(), bytes.size());
}

InternedStringId rebase_string_id(
    const StateStore& source_state,
    StateStore& destination_state,
    InternedStringId string_id
) {
    if (string_id == 0U) {
        return 0U;
    }
    return destination_state.strings().intern(source_state.strings().resolve(string_id));
}

Value rebase_value(const StateStore& source_state, StateStore& destination_state, const Value& value) {
    switch (value.index()) {
        case 4:
            return rebase_blob_ref(source_state, destination_state, std::get<BlobRef>(value));
        case 5:
            return rebase_string_id(source_state, destination_state, std::get<InternedStringId>(value));
        default:
            return value;
    }
}

} // namespace

StatePatch build_subgraph_input_patch(
    const StateStore& parent_state,
    const SubgraphBinding& binding,
    StateStore& child_state
) {
    StatePatch patch;
    const WorkflowState& parent = parent_state.get_current_state();
    patch.updates.reserve(binding.input_bindings.size());
    for (const SubgraphStateBinding& mapping : binding.input_bindings) {
        if (mapping.parent_key >= parent.fields.size()) {
            continue;
        }
        patch.updates.push_back(FieldUpdate{
            mapping.child_key,
            rebase_value(parent_state, child_state, parent.fields[mapping.parent_key])
        });
    }
    return patch;
}

StatePatch build_subgraph_output_patch(
    const StateStore& child_state,
    const SubgraphBinding& binding,
    StateStore& parent_state
) {
    StatePatch patch;
    const WorkflowState& child = child_state.get_current_state();
    patch.updates.reserve(binding.output_bindings.size());
    for (const SubgraphStateBinding& mapping : binding.output_bindings) {
        if (mapping.child_key >= child.fields.size()) {
            continue;
        }
        patch.updates.push_back(FieldUpdate{
            mapping.parent_key,
            rebase_value(child_state, parent_state, child.fields[mapping.child_key])
        });
    }

    if (!binding.propagate_knowledge_graph) {
        return patch;
    }

    for (KnowledgeEntityId entity_id = 1U; entity_id <= child_state.knowledge_graph().entity_count(); ++entity_id) {
        const KnowledgeEntity* entity = child_state.knowledge_graph().find_entity(entity_id);
        if (entity == nullptr) {
            continue;
        }
        patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
            rebase_string_id(child_state, parent_state, entity->label),
            rebase_blob_ref(child_state, parent_state, entity->payload)
        });
    }

    const std::vector<const KnowledgeTriple*> triples =
        child_state.knowledge_graph().match(std::nullopt, std::nullopt, std::nullopt);
    patch.knowledge_graph.triples.reserve(triples.size());
    for (const KnowledgeTriple* triple : triples) {
        if (triple == nullptr) {
            continue;
        }
        const KnowledgeEntity* subject = child_state.knowledge_graph().find_entity(triple->subject);
        const KnowledgeEntity* object = child_state.knowledge_graph().find_entity(triple->object);
        if (subject == nullptr || object == nullptr) {
            continue;
        }
        patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
            rebase_string_id(child_state, parent_state, subject->label),
            rebase_string_id(child_state, parent_state, triple->relation),
            rebase_string_id(child_state, parent_state, object->label),
            rebase_blob_ref(child_state, parent_state, triple->payload)
        });
    }

    return patch;
}

} // namespace agentcore
