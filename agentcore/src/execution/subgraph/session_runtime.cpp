#include "agentcore/execution/subgraph/session_runtime.h"

#include <algorithm>
#include <stdexcept>

namespace agentcore {

namespace {

const BranchSnapshot* primary_branch_snapshot(const RunSnapshot& snapshot) {
    if (snapshot.branches.empty()) {
        return nullptr;
    }

    const auto explicit_root = std::find_if(
        snapshot.branches.begin(),
        snapshot.branches.end(),
        [](const BranchSnapshot& branch) {
            return branch.frame.active_branch_id == 0U;
        }
    );
    if (explicit_root != snapshot.branches.end()) {
        return std::addressof(*explicit_root);
    }

    return std::addressof(
        *std::min_element(
            snapshot.branches.begin(),
            snapshot.branches.end(),
            [](const BranchSnapshot& left, const BranchSnapshot& right) {
                return left.frame.active_branch_id < right.frame.active_branch_id;
            }
        )
    );
}

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

bool logical_value_equal(
    const StateStore& left_state,
    const Value& left,
    const StateStore& right_state,
    const Value& right
) {
    if (left.index() != right.index()) {
        return false;
    }

    switch (left.index()) {
        case 0:
            return true;
        case 1:
            return std::get<int64_t>(left) == std::get<int64_t>(right);
        case 2:
            return std::get<double>(left) == std::get<double>(right);
        case 3:
            return std::get<bool>(left) == std::get<bool>(right);
        case 4:
            return left_state.blobs().read_bytes(std::get<BlobRef>(left)) ==
                right_state.blobs().read_bytes(std::get<BlobRef>(right));
        case 5:
            return left_state.strings().resolve(std::get<InternedStringId>(left)) ==
                right_state.strings().resolve(std::get<InternedStringId>(right));
        default:
            return false;
    }
}

bool field_updates_equal(
    const StateStore& left_state,
    const std::vector<FieldUpdate>& left,
    const StateStore& right_state,
    const std::vector<FieldUpdate>& right
) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].key != right[index].key ||
            !logical_value_equal(left_state, left[index].value, right_state, right[index].value)) {
            return false;
        }
    }
    return true;
}

bool blob_ref_equal(
    const BlobStore& left_blobs,
    BlobRef left,
    const BlobStore& right_blobs,
    BlobRef right
) {
    return left_blobs.read_bytes(left) == right_blobs.read_bytes(right);
}

bool task_record_equal(
    const StateStore& left_state,
    const TaskRecord& left,
    const StateStore& right_state,
    const TaskRecord& right
) {
    if (left.flags != right.flags) {
        return false;
    }
    if (left_state.strings().resolve(left.key) != right_state.strings().resolve(right.key)) {
        return false;
    }
    return blob_ref_equal(left_state.blobs(), left.request, right_state.blobs(), right.request) &&
        blob_ref_equal(left_state.blobs(), left.output, right_state.blobs(), right.output);
}

bool task_records_equal(
    const StateStore& left_state,
    const std::vector<TaskRecord>& left,
    const StateStore& right_state,
    const std::vector<TaskRecord>& right
) {
    if (left.size() != right.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!task_record_equal(left_state, left[index], right_state, right[index])) {
            return false;
        }
    }
    return true;
}

bool knowledge_graph_patches_equal(
    const StateStore& left_state,
    const KnowledgeGraphPatch& left,
    const StateStore& right_state,
    const KnowledgeGraphPatch& right
) {
    if (left.entities.size() != right.entities.size() || left.triples.size() != right.triples.size()) {
        return false;
    }

    for (std::size_t index = 0; index < left.entities.size(); ++index) {
        if (left_state.strings().resolve(left.entities[index].label) !=
                right_state.strings().resolve(right.entities[index].label) ||
            !blob_ref_equal(
                left_state.blobs(),
                left.entities[index].payload,
                right_state.blobs(),
                right.entities[index].payload
            )) {
            return false;
        }
    }

    for (std::size_t index = 0; index < left.triples.size(); ++index) {
        const KnowledgeTripleWrite& left_write = left.triples[index];
        const KnowledgeTripleWrite& right_write = right.triples[index];
        if (left_state.strings().resolve(left_write.subject_label) !=
                right_state.strings().resolve(right_write.subject_label) ||
            left_state.strings().resolve(left_write.relation) !=
                right_state.strings().resolve(right_write.relation) ||
            left_state.strings().resolve(left_write.object_label) !=
                right_state.strings().resolve(right_write.object_label) ||
            !blob_ref_equal(
                left_state.blobs(),
                left_write.payload,
                right_state.blobs(),
                right_write.payload
            )) {
            return false;
        }
    }

    return true;
}

bool patch_entries_equal(
    const StateStore& left_state,
    const PatchLogEntry& left,
    const StateStore& right_state,
    const PatchLogEntry& right
) {
    return left.offset == right.offset &&
        left.state_version == right.state_version &&
        left.patch.flags == right.patch.flags &&
        field_updates_equal(left_state, left.patch.updates, right_state, right.patch.updates) &&
        task_records_equal(left_state, left.patch.task_records, right_state, right.patch.task_records) &&
        knowledge_graph_patches_equal(
            left_state,
            left.patch.knowledge_graph,
            right_state,
            right.patch.knowledge_graph
        );
}

bool patch_log_has_prefix(
    const StateStore& prefix_state,
    const StateStore& candidate_state
) {
    const auto& prefix_entries = prefix_state.patch_log().entries();
    const auto& candidate_entries = candidate_state.patch_log().entries();
    if (prefix_entries.size() > candidate_entries.size()) {
        return false;
    }

    for (std::size_t index = 0; index < prefix_entries.size(); ++index) {
        if (!patch_entries_equal(prefix_state, prefix_entries[index], candidate_state, candidate_entries[index])) {
            return false;
        }
    }
    return true;
}

bool merge_task_journal(
    const StateStore& source_state,
    StateStore& destination_state,
    std::string* error_message
) {
    std::vector<TaskRecord> additions;
    for (const TaskRecord& source_record : source_state.task_journal().records()) {
        const TaskRecord rebased{
            rebase_string_id(source_state, destination_state, source_record.key),
            rebase_blob_ref(source_state, destination_state, source_record.request),
            rebase_blob_ref(source_state, destination_state, source_record.output),
            source_record.flags
        };

        const TaskRecord* existing = destination_state.task_journal().find(rebased.key);
        if (existing == nullptr) {
            additions.push_back(rebased);
            continue;
        }

        if (!task_record_equal(destination_state, *existing, destination_state, rebased)) {
            if (error_message != nullptr) {
                *error_message =
                    "completed subgraph session produced conflicting task journal records across branches";
            }
            return false;
        }
    }

    if (!additions.empty()) {
        static_cast<void>(destination_state.task_journal().apply(additions));
    }
    return true;
}

bool merge_knowledge_graph(
    const StateStore& source_state,
    StateStore& destination_state,
    std::string* error_message
) {
    StatePatch patch;

    for (KnowledgeEntityId entity_id = 1U; entity_id <= source_state.knowledge_graph().entity_count(); ++entity_id) {
        const KnowledgeEntity* entity = source_state.knowledge_graph().find_entity(entity_id);
        if (entity == nullptr) {
            continue;
        }

        const InternedStringId rebased_label =
            rebase_string_id(source_state, destination_state, entity->label);
        const BlobRef rebased_payload =
            rebase_blob_ref(source_state, destination_state, entity->payload);
        if (const KnowledgeEntity* existing =
                destination_state.knowledge_graph().find_entity_by_label(rebased_label);
            existing != nullptr &&
            !blob_ref_equal(destination_state.blobs(), existing->payload, destination_state.blobs(), rebased_payload)) {
            if (error_message != nullptr) {
                *error_message =
                    "completed subgraph session produced conflicting knowledge graph entity payloads";
            }
            return false;
        }

        patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{rebased_label, rebased_payload});
    }

    const std::vector<const KnowledgeTriple*> triples =
        source_state.knowledge_graph().match(std::nullopt, std::nullopt, std::nullopt);
    for (const KnowledgeTriple* triple : triples) {
        if (triple == nullptr) {
            continue;
        }

        const KnowledgeEntity* subject = source_state.knowledge_graph().find_entity(triple->subject);
        const KnowledgeEntity* object = source_state.knowledge_graph().find_entity(triple->object);
        if (subject == nullptr || object == nullptr) {
            continue;
        }

        const InternedStringId rebased_subject =
            rebase_string_id(source_state, destination_state, subject->label);
        const InternedStringId rebased_relation =
            rebase_string_id(source_state, destination_state, triple->relation);
        const InternedStringId rebased_object =
            rebase_string_id(source_state, destination_state, object->label);
        const BlobRef rebased_payload =
            rebase_blob_ref(source_state, destination_state, triple->payload);

        if (const KnowledgeTriple* existing =
                destination_state.knowledge_graph().find_triple_by_labels(
                    rebased_subject,
                    rebased_relation,
                    rebased_object
                );
            existing != nullptr &&
            !blob_ref_equal(destination_state.blobs(), existing->payload, destination_state.blobs(), rebased_payload)) {
            if (error_message != nullptr) {
                *error_message =
                    "completed subgraph session produced conflicting knowledge graph triple payloads";
            }
            return false;
        }

        patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
            rebased_subject,
            rebased_relation,
            rebased_object,
            rebased_payload
        });
    }

    if (!patch.empty()) {
        static_cast<void>(destination_state.apply(patch));
    }
    return true;
}

std::string value_to_session_id(
    const StateStore& state_store,
    const Value& value,
    std::string* error_message
) {
    if (std::holds_alternative<InternedStringId>(value)) {
        return std::string(state_store.strings().resolve(std::get<InternedStringId>(value)));
    }
    if (std::holds_alternative<int64_t>(value)) {
        return std::to_string(std::get<int64_t>(value));
    }
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    }

    if (error_message != nullptr) {
        *error_message = "persistent subgraph session ids must resolve to string, int64, or bool fields";
    }
    return {};
}

} // namespace

bool is_persistent_subgraph_binding(const SubgraphBinding& binding) noexcept {
    return binding.session_mode == SubgraphSessionMode::Persistent;
}

std::optional<std::string> resolve_subgraph_session_id(
    const StateStore& parent_state,
    const SubgraphBinding& binding,
    std::string* error_message
) {
    if (!is_persistent_subgraph_binding(binding)) {
        return std::nullopt;
    }
    if (!binding.session_id_source_key.has_value()) {
        if (error_message != nullptr) {
            *error_message = "persistent subgraph binding is missing a session id source key";
        }
        return std::nullopt;
    }

    const Value* value = parent_state.find(*binding.session_id_source_key);
    if (value == nullptr || std::holds_alternative<std::monostate>(*value)) {
        if (error_message != nullptr) {
            *error_message = "persistent subgraph session id source field is missing from parent state";
        }
        return std::nullopt;
    }

    const std::string session_id = value_to_session_id(parent_state, *value, error_message);
    if (session_id.empty()) {
        return std::nullopt;
    }
    return session_id;
}

const SubgraphSessionRecord* lookup_committed_subgraph_session(
    const SubgraphSessionTable& session_table,
    NodeId parent_node_id,
    std::string_view session_id
) {
    const auto node_iterator = session_table.find(parent_node_id);
    if (node_iterator == session_table.end()) {
        return nullptr;
    }
    const auto session_iterator = node_iterator->second.find(std::string(session_id));
    if (session_iterator == node_iterator->second.end()) {
        return nullptr;
    }
    return std::addressof(session_iterator->second);
}

void store_committed_subgraph_session(
    SubgraphSessionTable& session_table,
    NodeId parent_node_id,
    std::string session_id,
    uint64_t session_revision,
    std::vector<std::byte> snapshot_bytes,
    std::shared_ptr<RunSnapshot> snapshot,
    std::shared_ptr<StateStore> projected_state
) {
    const std::string session_key = session_id;
    session_table[parent_node_id][session_key] = SubgraphSessionRecord{
        std::move(session_id),
        session_revision,
        std::move(snapshot_bytes),
        std::move(snapshot),
        std::move(projected_state)
    };
}

bool acquire_subgraph_session_lease(
    SubgraphSessionLeaseTable& lease_table,
    NodeId parent_node_id,
    std::string_view session_id,
    uint32_t branch_id,
    std::string* error_message
) {
    auto& shard = lease_table[parent_node_id];
    const auto existing = shard.find(std::string(session_id));
    if (existing != shard.end() && existing->second != branch_id) {
        if (error_message != nullptr) {
            *error_message =
                "persistent subgraph session is already active on another branch for this node";
        }
        return false;
    }
    shard[std::string(session_id)] = branch_id;
    return true;
}

void release_subgraph_session_lease(
    SubgraphSessionLeaseTable& lease_table,
    NodeId parent_node_id,
    std::string_view session_id,
    uint32_t branch_id
) {
    const auto node_iterator = lease_table.find(parent_node_id);
    if (node_iterator == lease_table.end()) {
        return;
    }
    const auto session_iterator = node_iterator->second.find(std::string(session_id));
    if (session_iterator == node_iterator->second.end() || session_iterator->second != branch_id) {
        return;
    }
    node_iterator->second.erase(session_iterator);
    if (node_iterator->second.empty()) {
        lease_table.erase(node_iterator);
    }
}

std::vector<CommittedSubgraphSessionSnapshot> flatten_subgraph_session_table(
    const SubgraphSessionTable& session_table
) {
    std::vector<CommittedSubgraphSessionSnapshot> snapshots;
    for (const auto& [node_id, sessions] : session_table) {
        for (const auto& [session_id, record] : sessions) {
            snapshots.push_back(CommittedSubgraphSessionSnapshot{
                node_id,
                session_id,
                record.session_revision,
                record.snapshot_bytes.empty() && record.snapshot != nullptr
                    ? serialize_run_snapshot_bytes(*record.snapshot)
                    : record.snapshot_bytes
            });
        }
    }
    std::sort(
        snapshots.begin(),
        snapshots.end(),
        [](const CommittedSubgraphSessionSnapshot& left, const CommittedSubgraphSessionSnapshot& right) {
            if (left.parent_node_id != right.parent_node_id) {
                return left.parent_node_id < right.parent_node_id;
            }
            return left.session_id < right.session_id;
        }
    );
    return snapshots;
}

void restore_subgraph_session_table(
    SubgraphSessionTable& session_table,
    const std::vector<CommittedSubgraphSessionSnapshot>& snapshots
) {
    session_table.clear();
    for (const CommittedSubgraphSessionSnapshot& snapshot : snapshots) {
        store_committed_subgraph_session(
            session_table,
            snapshot.parent_node_id,
            snapshot.session_id,
            snapshot.session_revision,
            snapshot.snapshot_bytes,
            {},
            {}
        );
    }
}

std::optional<StateStore> project_subgraph_session_state(
    const RunSnapshot& snapshot,
    std::string* error_message
) {
    const BranchSnapshot* primary = primary_branch_snapshot(snapshot);
    if (primary == nullptr) {
        if (error_message != nullptr) {
            *error_message = "subgraph snapshot is missing branches";
        }
        return std::nullopt;
    }

    if (snapshot.branches.size() == 1U) {
        return primary->state_store;
    }

    StateStore projection = primary->state_store;
    std::unordered_map<StateKey, uint32_t> descendant_field_writers;
    for (const BranchSnapshot& branch : snapshot.branches) {
        if (&branch == primary) {
            continue;
        }

        const bool descendant_of_primary = patch_log_has_prefix(primary->state_store, branch.state_store);
        if (descendant_of_primary) {
            const auto& branch_entries = branch.state_store.patch_log().entries();
            const std::size_t delta_start = primary->state_store.patch_log().size();
            for (std::size_t index = delta_start; index < branch_entries.size(); ++index) {
                for (const FieldUpdate& update : branch_entries[index].patch.updates) {
                    const Value rebased = rebase_value(branch.state_store, projection, update.value);
                    const Value* existing = projection.find(update.key);
                    const auto writer = descendant_field_writers.find(update.key);
                    if (writer != descendant_field_writers.end() &&
                        writer->second != branch.frame.active_branch_id &&
                        existing != nullptr &&
                        !logical_value_equal(projection, *existing, projection, rebased)) {
                        if (error_message != nullptr) {
                            *error_message =
                                "completed subgraph session produced conflicting descendant field writes for key " +
                                std::to_string(update.key);
                        }
                        return std::nullopt;
                    }

                    StatePatch field_patch;
                    field_patch.updates.push_back(FieldUpdate{update.key, rebased});
                    static_cast<void>(projection.apply(field_patch));
                    descendant_field_writers[update.key] = branch.frame.active_branch_id;
                }
            }
        } else {
            const WorkflowState& source_state = branch.state_store.get_current_state();
            projection.ensure_field_capacity(source_state.size());
            StatePatch field_patch;

            for (StateKey key = 0; key < source_state.size(); ++key) {
                const Value candidate = source_state.load(key);
                if (std::holds_alternative<std::monostate>(candidate)) {
                    continue;
                }

                const Value* existing = projection.find(key);
                if (existing == nullptr || std::holds_alternative<std::monostate>(*existing)) {
                    field_patch.updates.push_back(FieldUpdate{
                        key,
                        rebase_value(branch.state_store, projection, candidate)
                    });
                    continue;
                }

                if (!logical_value_equal(projection, *existing, branch.state_store, candidate)) {
                    if (error_message != nullptr) {
                        *error_message =
                            "completed subgraph session produced conflicting field values across branches for key " +
                            std::to_string(key);
                    }
                    return std::nullopt;
                }
            }

            if (!field_patch.empty()) {
                static_cast<void>(projection.apply(field_patch));
            }
        }

        if (!merge_task_journal(branch.state_store, projection, error_message)) {
            return std::nullopt;
        }
        if (!merge_knowledge_graph(branch.state_store, projection, error_message)) {
            return std::nullopt;
        }
    }

    return projection;
}

} // namespace agentcore
