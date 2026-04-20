#include "agentcore/state/state_store.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>

namespace agentcore {

namespace {

enum StateTestKey : StateKey {
    kCounter = 0,
    kSummary = 1
};

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    StateStore store(2);
    BlobStore& blobs = store.blobs();
    StringInterner& strings = store.strings();

    const InternedStringId runtime = strings.intern("agentcore");
    const InternedStringId reference_runtime = strings.intern("reference_runtime");
    const InternedStringId relation = strings.intern("faster_than");

    const BlobRef summary_blob = blobs.append_string("native runtime");
    const BlobRef runtime_payload = blobs.append_string("c++");
    const BlobRef edge_payload = blobs.append_string("deterministic");

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kCounter, int64_t{42}});
    patch.updates.push_back(FieldUpdate{kSummary, summary_blob});
    patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{runtime, runtime_payload});
    patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
        reference_runtime,
        blobs.append_string("python")
    });
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        runtime,
        relation,
        reference_runtime,
        edge_payload
    });

    const StateApplyResult apply_result = store.apply_with_summary(patch);
    assert(apply_result.patch_log_offset == 0U);
    assert(apply_result.state_changed);
    assert(apply_result.knowledge_graph_delta.entities.size() == 2U);
    assert(apply_result.knowledge_graph_delta.triples.size() == 1U);
    assert(apply_result.changed_keys.size() == 2U);
    assert(store.get_current_state().version == 1U);
    assert(store.patch_log().size() == 1U);
    assert(store.get_current_state().field_revision(kCounter) == 1U);
    assert(store.get_current_state().field_revision(kSummary) == 1U);

    const WorkflowState& current = store.get_current_state();
    assert(std::holds_alternative<int64_t>(current.load(kCounter)));
    assert(std::get<int64_t>(current.load(kCounter)) == 42);
    assert(std::holds_alternative<BlobRef>(current.load(kSummary)));
    assert(store.blobs().read_string(std::get<BlobRef>(current.load(kSummary))) == "native runtime");
    assert(store.knowledge_graph().entity_count() == 2U);
    assert(store.knowledge_graph().triple_count() == 1U);

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    store.serialize(stream);
    stream.seekg(0);
    StateStore restored = StateStore::deserialize(stream);

    const WorkflowState& restored_state = restored.get_current_state();
    assert(restored_state.version == 1U);
    assert(std::get<int64_t>(restored_state.load(kCounter)) == 42);
    assert(
        restored.blobs().read_string(std::get<BlobRef>(restored_state.load(kSummary))) ==
        "native runtime"
    );
    assert(restored.patch_log().size() == 1U);
    assert(restored.knowledge_graph().entity_count() == 2U);
    assert(restored.knowledge_graph().triple_count() == 1U);
    assert(restored.get_current_state().field_revision(kCounter) == 1U);
    assert(restored.get_current_state().field_revision(kSummary) == 1U);

    const std::vector<const KnowledgeTriple*> matches =
        restored.knowledge_graph().match(runtime, relation, reference_runtime);
    assert(matches.size() == 1U);
    assert(restored.blobs().read_string(matches.front()->payload) == "deterministic");

    StatePatch duplicate_patch;
    duplicate_patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
        runtime,
        blobs.append_string("c++")
    });
    duplicate_patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        runtime,
        relation,
        reference_runtime,
        blobs.append_string("deterministic")
    });
    const StateApplyResult duplicate_result = store.apply_with_summary(duplicate_patch);
    assert(!duplicate_result.state_changed);
    assert(duplicate_result.knowledge_graph_delta.empty());
    assert(duplicate_result.patch_log_offset == 1U);
    assert(store.get_current_state().version == 1U);
    assert(store.patch_log().size() == 1U);

    StatePatch duplicate_field_patch;
    duplicate_field_patch.updates.push_back(FieldUpdate{kCounter, int64_t{42}});
    const StateApplyResult duplicate_field_result = store.apply_with_summary(duplicate_field_patch);
    assert(!duplicate_field_result.state_changed);
    assert(duplicate_field_result.changed_keys.empty());
    assert(duplicate_field_result.patch_log_offset == 1U);
    assert(store.get_current_state().version == 1U);
    assert(store.patch_log().size() == 1U);
    assert(store.get_current_state().field_revision(kCounter) == 1U);

    StatePatch recorded_task_patch;
    recorded_task_patch.task_records.push_back(TaskRecord{
        strings.intern("effect:write-once"),
        blobs.append_string("request::v1"),
        blobs.append_string("outcome::ok"),
        7U
    });
    const StateApplyResult recorded_task_result = store.apply_with_summary(recorded_task_patch);
    assert(recorded_task_result.state_changed);
    assert(store.get_current_state().version == 2U);
    assert(store.patch_log().size() == 2U);
    assert(store.task_journal().size() == 1U);
    const TaskRecord* recorded_task = store.task_journal().find(strings.intern("effect:write-once"));
    assert(recorded_task != nullptr);
    assert(store.blobs().read_string(recorded_task->request) == "request::v1");
    assert(store.blobs().read_string(recorded_task->output) == "outcome::ok");
    assert(recorded_task->flags == 7U);

    const StateApplyResult duplicate_recorded_task_result = store.apply_with_summary(recorded_task_patch);
    assert(!duplicate_recorded_task_result.state_changed);
    assert(duplicate_recorded_task_result.patch_log_offset == 2U);
    assert(store.get_current_state().version == 2U);
    assert(store.patch_log().size() == 2U);

    StateStore fork = store;
    const StateStore::SharedBacking shared_before = store.shared_backing_with(fork);
    assert(shared_before.blobs);
    assert(shared_before.strings);
    assert(shared_before.knowledge_graph);

    StatePatch fork_patch;
    fork_patch.updates.push_back(FieldUpdate{kCounter, int64_t{7}});
    static_cast<void>(fork.apply(fork_patch));
    assert(std::get<int64_t>(store.get_current_state().load(kCounter)) == 42);
    assert(store.get_current_state().field_revision(kCounter) == 1U);
    assert(fork.get_current_state().field_revision(kCounter) == 2U);
    const StateStore::SharedBacking shared_after_patch = store.shared_backing_with(fork);
    assert(shared_after_patch.blobs);
    assert(shared_after_patch.strings);
    assert(shared_after_patch.knowledge_graph);

    const BlobRef fork_blob = fork.blobs().append_string("forked-artifact");
    const InternedStringId fork_label = fork.strings().intern("fork_entity");
    fork.knowledge_graph().upsert_entity(fork_label, fork_blob);

    const StateStore::SharedBacking shared_after_detach = store.shared_backing_with(fork);
    assert(!shared_after_detach.blobs);
    assert(!shared_after_detach.strings);
    assert(!shared_after_detach.knowledge_graph);
    assert(store.blobs().read_string(fork_blob).empty());
    assert(store.strings().resolve(fork_label).empty());
    assert(store.knowledge_graph().find_entity_by_label(fork_label) == nullptr);
    assert(fork.knowledge_graph().find_entity_by_label(fork_label) != nullptr);
    assert(store.knowledge_graph().entity_count() == 2U);
    assert(fork.knowledge_graph().entity_count() == 3U);
    assert(store.task_journal().size() == 1U);
    assert(fork.task_journal().size() == 1U);

    std::cout << "state module tests passed" << std::endl;
    return 0;
}
