#include "agentcore/state/state_store.h"
#include "agentcore/state/context/context_graph.h"
#include "agentcore/state/intelligence/ops.h"

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
    const InternedStringId task_key = strings.intern("task:benchmark");
    const InternedStringId claim_key = strings.intern("claim:native-runtime");
    const InternedStringId evidence_key = strings.intern("evidence:head-to-head");
    const InternedStringId decision_key = strings.intern("decision:adopt");
    const InternedStringId memory_key = strings.intern("memory:semantic:native-runtime");
    const InternedStringId planner_owner = strings.intern("planner");
    const InternedStringId memory_scope = strings.intern("runtime");

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
    patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        task_key,
        strings.intern("Benchmark runtime"),
        planner_owner,
        blobs.append_string("{\"goal\":\"measure\"}"),
        {},
        IntelligenceTaskStatus::InProgress,
        5,
        0.8F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        claim_key,
        runtime,
        relation,
        reference_runtime,
        blobs.append_string("agentcore outperforms the reference runtime"),
        IntelligenceClaimStatus::Supported,
        0.9F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        evidence_key,
        strings.intern("benchmark"),
        strings.intern("runtime_benchmark"),
        blobs.append_string("{\"speedup\":2.0}"),
        task_key,
        claim_key,
        0.85F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    patch.intelligence.decisions.push_back(IntelligenceDecisionWrite{
        decision_key,
        task_key,
        claim_key,
        blobs.append_string("{\"action\":\"publish\"}"),
        IntelligenceDecisionStatus::Selected,
        0.75F,
        intelligence_fields::kDecisionTaskKey |
            intelligence_fields::kDecisionClaimKey |
            intelligence_fields::kDecisionSummary |
            intelligence_fields::kDecisionStatus |
            intelligence_fields::kDecisionConfidence
    });
    patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        memory_key,
        IntelligenceMemoryLayer::Semantic,
        memory_scope,
        blobs.append_string("{\"fact\":\"agentcore is native\"}"),
        task_key,
        claim_key,
        0.7F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });

    const StateApplyResult apply_result = store.apply_with_summary(patch);
    assert(apply_result.patch_log_offset == 0U);
    assert(apply_result.state_changed);
    assert(apply_result.knowledge_graph_delta.entities.size() == 2U);
    assert(apply_result.knowledge_graph_delta.triples.size() == 1U);
    assert(apply_result.intelligence_delta.tasks.size() == 1U);
    assert(apply_result.intelligence_delta.claims.size() == 1U);
    assert(apply_result.intelligence_delta.evidence.size() == 1U);
    assert(apply_result.intelligence_delta.decisions.size() == 1U);
    assert(apply_result.intelligence_delta.memories.size() == 1U);
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
    assert(store.intelligence().task_count() == 1U);
    assert(store.intelligence().claim_count() == 1U);
    assert(store.intelligence().evidence_count() == 1U);
    assert(store.intelligence().decision_count() == 1U);
    assert(store.intelligence().memory_count() == 1U);
    const IntelligenceTask* task = store.intelligence().find_task_by_key(task_key);
    assert(task != nullptr);
    assert(task->status == IntelligenceTaskStatus::InProgress);
    assert(task->priority == 5);
    assert(task->owner == planner_owner);
    assert(store.blobs().read_string(task->payload) == "{\"goal\":\"measure\"}");

    IntelligenceQuery task_query;
    task_query.kind = IntelligenceRecordKind::Tasks;
    task_query.task_status = IntelligenceTaskStatus::InProgress;
    const IntelligenceSnapshot task_query_result =
        query_intelligence_records(store.intelligence(), store.strings(), task_query);
    assert(task_query_result.tasks.size() == 1U);
    assert(task_query_result.tasks.front().key == task_key);

    IntelligenceQuery memory_query;
    memory_query.kind = IntelligenceRecordKind::Memories;
    memory_query.memory_layer = IntelligenceMemoryLayer::Semantic;
    const IntelligenceSnapshot memory_query_result =
        query_intelligence_records(store.intelligence(), store.strings(), memory_query);
    assert(memory_query_result.memories.size() == 1U);
    assert(memory_query_result.memories.front().key == memory_key);

    const IntelligenceSnapshot related_to_task =
        related_intelligence_records(store.intelligence(), task_key, 0U);
    assert(related_to_task.tasks.size() == 1U);
    assert(related_to_task.claims.size() == 1U);
    assert(related_to_task.evidence.size() == 1U);
    assert(related_to_task.decisions.size() == 1U);
    assert(related_to_task.memories.size() == 1U);
    const IntelligenceSnapshot related_to_claim =
        related_intelligence_records(store.intelligence(), 0U, claim_key);
    assert(related_to_claim.tasks.size() == 1U);
    assert(related_to_claim.claims.size() == 1U);
    assert(related_to_claim.evidence.size() == 1U);
    assert(related_to_claim.decisions.size() == 1U);
    assert(related_to_claim.memories.size() == 1U);

    const IntelligenceOperationalSummary intelligence_summary = summarize_intelligence(store.intelligence());
    assert(intelligence_summary.task_count == 1U);
    assert(intelligence_summary.in_progress_task_count == 1U);
    assert(intelligence_summary.supported_claim_count == 1U);
    assert(intelligence_summary.selected_decision_count == 1U);
    assert(intelligence_summary.semantic_memory_count == 1U);

    IntelligenceQuery supported_claim_query;
    supported_claim_query.kind = IntelligenceRecordKind::Claims;
    supported_claim_query.claim_status = IntelligenceClaimStatus::Supported;
    assert(
        count_intelligence_records(store.intelligence(), store.strings(), supported_claim_query) == 1U
    );

    IntelligenceQuery semantic_claim_query;
    semantic_claim_query.kind = IntelligenceRecordKind::Claims;
    semantic_claim_query.subject_label = runtime;
    semantic_claim_query.relation = relation;
    semantic_claim_query.object_label = reference_runtime;
    const IntelligenceSnapshot semantic_claim_result =
        query_intelligence_records(store.intelligence(), store.strings(), semantic_claim_query);
    assert(semantic_claim_result.claims.size() == 1U);
    assert(semantic_claim_result.claims.front().key == claim_key);
    assert(
        count_intelligence_records(store.intelligence(), store.strings(), semantic_claim_query) == 1U
    );
    const IntelligenceSnapshot semantic_focus =
        focus_intelligence_records(store.intelligence(), store.strings(), semantic_claim_query);
    assert(semantic_focus.tasks.size() == 1U);
    assert(semantic_focus.tasks.front().key == task_key);
    assert(semantic_focus.claims.size() == 1U);
    assert(semantic_focus.claims.front().key == claim_key);
    assert(semantic_focus.evidence.size() == 1U);
    assert(semantic_focus.evidence.front().key == evidence_key);
    assert(semantic_focus.decisions.size() == 1U);
    assert(semantic_focus.decisions.front().key == decision_key);
    assert(semantic_focus.memories.size() == 1U);
    assert(semantic_focus.memories.front().key == memory_key);

    ContextQueryPlan context_plan;
    assert(context_query_plan_add_selector(context_plan, "tasks.agenda"));
    assert(context_query_plan_add_selector(context_plan, "claims.supported"));
    assert(context_query_plan_add_selector(context_plan, "evidence.relevant"));
    assert(context_query_plan_add_selector(context_plan, "decisions.selected"));
    assert(context_query_plan_add_selector(context_plan, "memories.recall"));
    assert(context_query_plan_add_selector(context_plan, "knowledge.neighborhood"));
    context_plan.task_key = task_key;
    context_plan.claim_key = claim_key;
    context_plan.subject_label = runtime;
    context_plan.relation = relation;
    context_plan.object_label = reference_runtime;
    context_plan.scope = memory_scope;
    context_plan.limit = 12U;
    const ContextGraphResult context_rank =
        rank_context_graph(store.intelligence(), store.knowledge_graph(), context_plan);
    const ContextGraphIndex context_index(store.intelligence(), store.knowledge_graph(), context_plan);
    const ContextGraphResult indexed_context_rank = context_index.rank();
    const ContextGraphResult repeated_indexed_context_rank = context_index.rank();
    assert(indexed_context_rank.records.size() == context_rank.records.size());
    assert(repeated_indexed_context_rank.records.size() == indexed_context_rank.records.size());
    for (std::size_t index = 0U; index < indexed_context_rank.records.size(); ++index) {
        assert(indexed_context_rank.records[index].kind == context_rank.records[index].kind);
        assert(indexed_context_rank.records[index].id == context_rank.records[index].id);
        assert(indexed_context_rank.records[index].score == context_rank.records[index].score);
        assert(repeated_indexed_context_rank.records[index].kind == indexed_context_rank.records[index].kind);
        assert(repeated_indexed_context_rank.records[index].id == indexed_context_rank.records[index].id);
        assert(repeated_indexed_context_rank.records[index].score == indexed_context_rank.records[index].score);
    }
    assert(context_rank.records.size() >= 6U);
    bool saw_context_task = false;
    bool saw_context_claim = false;
    bool saw_context_evidence = false;
    bool saw_context_decision = false;
    bool saw_context_memory = false;
    bool saw_context_knowledge = false;
    for (const ContextGraphRecordRef& ref : context_rank.records) {
        assert(ref.score > 0);
        saw_context_task = saw_context_task ||
            (ref.kind == ContextRecordKind::Task && ref.id == task->id);
        saw_context_claim = saw_context_claim ||
            (ref.kind == ContextRecordKind::Claim && ref.id == semantic_claim_result.claims.front().id);
        saw_context_evidence = saw_context_evidence ||
            (ref.kind == ContextRecordKind::Evidence && ref.id == semantic_focus.evidence.front().id);
        saw_context_decision = saw_context_decision ||
            (ref.kind == ContextRecordKind::Decision && ref.id == semantic_focus.decisions.front().id);
        saw_context_memory = saw_context_memory ||
            (ref.kind == ContextRecordKind::Memory && ref.id == semantic_focus.memories.front().id);
        saw_context_knowledge = saw_context_knowledge || ref.kind == ContextRecordKind::Knowledge;
    }
    assert(saw_context_task);
    assert(saw_context_claim);
    assert(saw_context_evidence);
    assert(saw_context_decision);
    assert(saw_context_memory);
    assert(saw_context_knowledge);

    std::vector<IntelligenceRouteRule> route_rules;
    route_rules.push_back(IntelligenceRouteRule{
        supported_claim_query,
        1U,
        std::nullopt,
        "publish"
    });
    const std::optional<std::string> selected_route =
        select_intelligence_route(store.intelligence(), store.strings(), route_rules);
    assert(selected_route.has_value());
    assert(*selected_route == "publish");

    const std::optional<std::string> semantic_route = select_intelligence_route(
        store.intelligence(),
        store.strings(),
        {
            IntelligenceRouteRule{semantic_claim_query, 1U, std::nullopt, "publish-semantic"},
            IntelligenceRouteRule{IntelligenceQuery{}, 1U, std::nullopt, "fallback"}
        }
    );
    assert(semantic_route.has_value());
    assert(*semantic_route == "publish-semantic");

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
    assert(restored.intelligence().task_count() == 1U);
    assert(restored.intelligence().claim_count() == 1U);
    assert(restored.intelligence().evidence_count() == 1U);
    assert(restored.intelligence().decision_count() == 1U);
    assert(restored.intelligence().memory_count() == 1U);
    assert(restored.get_current_state().field_revision(kCounter) == 1U);
    assert(restored.get_current_state().field_revision(kSummary) == 1U);
    const IntelligenceDecision* restored_decision = restored.intelligence().find_decision_by_key(decision_key);
    assert(restored_decision != nullptr);
    assert(restored_decision->status == IntelligenceDecisionStatus::Selected);
    assert(restored.blobs().read_string(restored_decision->summary) == "{\"action\":\"publish\"}");

    const std::vector<const KnowledgeTriple*> matches =
        restored.knowledge_graph().match(runtime, relation, reference_runtime);
    assert(matches.size() == 1U);
    assert(restored.blobs().read_string(matches.front()->payload) == "deterministic");

    KnowledgeGraphStore analytics_graph;
    const InternedStringId hub_label = strings.intern("analytics:hub");
    const InternedStringId sparse_relation = strings.intern("analytics:sparse");
    const InternedStringId dense_relation = strings.intern("analytics:dense");
    const InternedStringId exact_target = strings.intern("analytics:leaf:31");
    for (uint32_t index = 0U; index < 32U; ++index) {
        analytics_graph.upsert_triple(
            hub_label,
            dense_relation,
            strings.intern(std::string("analytics:leaf:") + std::to_string(index)),
            blobs.append_string(std::string("dense-") + std::to_string(index))
        );
    }
    analytics_graph.upsert_triple(
        strings.intern("analytics:other"),
        sparse_relation,
        exact_target,
        blobs.append_string("sparse")
    );
    const std::vector<const KnowledgeTriple*> exact_match =
        analytics_graph.match(hub_label, dense_relation, exact_target);
    assert(exact_match.size() == 1U);
    assert(blobs.read_string(exact_match.front()->payload) == "dense-31");
    const KnowledgeEntity* exact_target_entity = analytics_graph.find_entity_by_label(exact_target);
    assert(exact_target_entity != nullptr);
    const std::vector<KnowledgeEntityId> hub_neighbors = analytics_graph.neighbors(hub_label);
    assert(hub_neighbors.size() == 32U);
    assert(hub_neighbors.back() == exact_target_entity->id);
    const std::vector<const KnowledgeTriple*> object_constrained_match =
        analytics_graph.match(std::nullopt, sparse_relation, exact_target);
    assert(object_constrained_match.size() == 1U);
    assert(blobs.read_string(object_constrained_match.front()->payload) == "sparse");
    const std::vector<const KnowledgeTriple*> missing_constrained_match =
        analytics_graph.match(hub_label, sparse_relation, exact_target);
    assert(missing_constrained_match.empty());
    const InternedStringId late_leaf = strings.intern("analytics:leaf:32");
    analytics_graph.upsert_triple(
        hub_label,
        dense_relation,
        late_leaf,
        blobs.append_string("dense-32")
    );
    const std::vector<KnowledgeEntityId> updated_hub_neighbors = analytics_graph.neighbors(hub_label);
    assert(updated_hub_neighbors.size() == 33U);
    const KnowledgeEntity* late_leaf_entity = analytics_graph.find_entity_by_label(late_leaf);
    assert(late_leaf_entity != nullptr);
    assert(updated_hub_neighbors.back() == late_leaf_entity->id);

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
    duplicate_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        task_key,
        strings.intern("Benchmark runtime"),
        planner_owner,
        blobs.append_string("{\"goal\":\"measure\"}"),
        {},
        IntelligenceTaskStatus::InProgress,
        5,
        0.8F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    const StateApplyResult duplicate_result = store.apply_with_summary(duplicate_patch);
    assert(!duplicate_result.state_changed);
    assert(duplicate_result.knowledge_graph_delta.empty());
    assert(duplicate_result.intelligence_delta.empty());
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

    const InternedStringId reviewer_owner = strings.intern("reviewer");
    const InternedStringId second_task_key = strings.intern("task:benchmark:followup");
    const InternedStringId urgent_task_key = strings.intern("task:benchmark:urgent");
    const InternedStringId low_memory_key = strings.intern("memory:runtime:low");
    const InternedStringId high_memory_key = strings.intern("memory:runtime:high");
    const InternedStringId focus_owner = strings.intern("focus-owner");
    const InternedStringId focus_scope = strings.intern("focus:runtime");
    const InternedStringId weak_task_key = strings.intern("task:focus:weak");
    const InternedStringId decisive_task_key = strings.intern("task:focus:decisive");
    const InternedStringId weak_claim_key = strings.intern("claim:focus:weak");
    const InternedStringId decisive_claim_key = strings.intern("claim:focus:decisive");
    const InternedStringId decisive_evidence_key = strings.intern("evidence:focus:decisive");
    const InternedStringId decisive_decision_key = strings.intern("decision:focus:decisive");
    const InternedStringId decisive_memory_key = strings.intern("memory:focus:decisive");
    const InternedStringId chain_task_seed_key = strings.intern("task:related:seed");
    const InternedStringId chain_task_second_key = strings.intern("task:related:second");
    const InternedStringId chain_claim_key = strings.intern("claim:related:shared");
    const InternedStringId chain_evidence_key = strings.intern("evidence:related:seed");
    const InternedStringId chain_memory_key = strings.intern("memory:related:second");
    const InternedStringId chain_owner = strings.intern("related-owner");
    const InternedStringId chain_scope = strings.intern("scope:related");

    StatePatch indexed_order_patch;
    indexed_order_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        second_task_key,
        strings.intern("Follow up benchmark"),
        reviewer_owner,
        blobs.append_string("{\"goal\":\"follow-up\"}"),
        {},
        IntelligenceTaskStatus::Completed,
        3,
        0.7F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    indexed_order_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        task_key,
        {},
        reviewer_owner,
        {},
        {},
        IntelligenceTaskStatus::Completed,
        0,
        0.0F,
        intelligence_fields::kTaskOwner | intelligence_fields::kTaskStatus
    });
    indexed_order_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        urgent_task_key,
        strings.intern("Urgent benchmark follow-up"),
        reviewer_owner,
        blobs.append_string("{\"goal\":\"urgent\"}"),
        {},
        IntelligenceTaskStatus::Open,
        1,
        0.95F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    indexed_order_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        low_memory_key,
        IntelligenceMemoryLayer::Semantic,
        memory_scope,
        blobs.append_string("{\"fact\":\"low-importance\"}"),
        second_task_key,
        claim_key,
        0.2F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    indexed_order_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        high_memory_key,
        IntelligenceMemoryLayer::Semantic,
        memory_scope,
        blobs.append_string("{\"fact\":\"high-importance\"}"),
        urgent_task_key,
        claim_key,
        0.95F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    const StateApplyResult indexed_order_result = store.apply_with_summary(indexed_order_patch);
    assert(indexed_order_result.state_changed);
    assert(indexed_order_result.intelligence_delta.tasks.size() == 3U);
    assert(indexed_order_result.intelligence_delta.memories.size() == 2U);

    IntelligenceQuery completed_reviewer_query;
    completed_reviewer_query.kind = IntelligenceRecordKind::Tasks;
    completed_reviewer_query.owner = reviewer_owner;
    completed_reviewer_query.task_status = IntelligenceTaskStatus::Completed;
    const IntelligenceSnapshot completed_reviewer_tasks =
        query_intelligence_records(store.intelligence(), store.strings(), completed_reviewer_query);
    assert(completed_reviewer_tasks.tasks.size() == 2U);
    assert(completed_reviewer_tasks.tasks[0].key == task_key);
    assert(completed_reviewer_tasks.tasks[1].key == second_task_key);

    IntelligenceQuery reviewer_agenda_query;
    reviewer_agenda_query.owner = reviewer_owner;
    const IntelligenceSnapshot reviewer_agenda =
        agenda_intelligence_tasks(store.intelligence(), store.strings(), reviewer_agenda_query);
    assert(reviewer_agenda.tasks.size() == 3U);
    assert(reviewer_agenda.tasks[0].key == urgent_task_key);
    assert(reviewer_agenda.tasks[1].key == task_key);
    assert(reviewer_agenda.tasks[2].key == second_task_key);

    IntelligenceQuery runtime_recall_query;
    runtime_recall_query.scope = memory_scope;
    runtime_recall_query.memory_layer = IntelligenceMemoryLayer::Semantic;
    const IntelligenceSnapshot runtime_recall =
        recall_intelligence_memories(store.intelligence(), store.strings(), runtime_recall_query);
    assert(runtime_recall.memories.size() == 3U);
    assert(runtime_recall.memories[0].key == high_memory_key);
    assert(runtime_recall.memories[1].key == memory_key);
    assert(runtime_recall.memories[2].key == low_memory_key);

    IntelligenceQuery reviewer_focus_query;
    reviewer_focus_query.owner = reviewer_owner;
    reviewer_focus_query.scope = memory_scope;
    const IntelligenceSnapshot reviewer_focus =
        focus_intelligence_records(store.intelligence(), store.strings(), reviewer_focus_query);
    assert(reviewer_focus.tasks.size() == 3U);
    assert(reviewer_focus.tasks[0].key == urgent_task_key);
    assert(reviewer_focus.claims.size() == 1U);
    assert(reviewer_focus.claims[0].key == claim_key);
    assert(reviewer_focus.evidence.size() == 1U);
    assert(reviewer_focus.evidence[0].key == evidence_key);
    assert(reviewer_focus.decisions.size() == 1U);
    assert(reviewer_focus.decisions[0].key == decision_key);
    assert(reviewer_focus.memories.size() == 3U);
    assert(reviewer_focus.memories[0].key == high_memory_key);

    StatePatch focus_rank_patch;
    focus_rank_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        weak_task_key,
        strings.intern("Weak task"),
        focus_owner,
        blobs.append_string("{\"class\":\"weak\"}"),
        {},
        IntelligenceTaskStatus::Open,
        4,
        0.8F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_rank_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        decisive_task_key,
        strings.intern("Decisive task"),
        focus_owner,
        blobs.append_string("{\"class\":\"decisive\"}"),
        {},
        IntelligenceTaskStatus::Open,
        4,
        0.8F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_rank_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        weak_claim_key,
        runtime,
        relation,
        reference_runtime,
        blobs.append_string("{\"support\":\"weak\"}"),
        IntelligenceClaimStatus::Supported,
        0.8F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    focus_rank_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        decisive_claim_key,
        runtime,
        relation,
        reference_runtime,
        blobs.append_string("{\"support\":\"decisive\"}"),
        IntelligenceClaimStatus::Supported,
        0.8F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    for (int index = 0; index < 4; ++index) {
        const std::string suffix = std::to_string(index);
        focus_rank_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
            strings.intern("evidence:focus:weak:" + suffix),
            strings.intern("observation"),
            strings.intern("focus-suite"),
            blobs.append_string("{\"weak_evidence\":" + suffix + "}"),
            weak_task_key,
            weak_claim_key,
            0.2F + static_cast<float>(index) * 0.02F,
            intelligence_fields::kEvidenceKind |
                intelligence_fields::kEvidenceSource |
                intelligence_fields::kEvidenceContent |
                intelligence_fields::kEvidenceTaskKey |
                intelligence_fields::kEvidenceClaimKey |
                intelligence_fields::kEvidenceConfidence
        });
        focus_rank_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
            strings.intern("memory:focus:weak:" + suffix),
            IntelligenceMemoryLayer::Working,
            focus_scope,
            blobs.append_string("{\"weak_memory\":" + suffix + "}"),
            weak_task_key,
            weak_claim_key,
            0.1F + static_cast<float>(index) * 0.02F,
            intelligence_fields::kMemoryLayer |
                intelligence_fields::kMemoryScope |
                intelligence_fields::kMemoryContent |
                intelligence_fields::kMemoryTaskKey |
                intelligence_fields::kMemoryClaimKey |
                intelligence_fields::kMemoryImportance
        });
    }
    focus_rank_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        decisive_evidence_key,
        strings.intern("benchmark"),
        strings.intern("focus-suite"),
        blobs.append_string("{\"decisive_evidence\":true}"),
        decisive_task_key,
        decisive_claim_key,
        0.97F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    focus_rank_patch.intelligence.decisions.push_back(IntelligenceDecisionWrite{
        decisive_decision_key,
        decisive_task_key,
        decisive_claim_key,
        blobs.append_string("{\"selected\":true}"),
        IntelligenceDecisionStatus::Selected,
        0.93F,
        intelligence_fields::kDecisionTaskKey |
            intelligence_fields::kDecisionClaimKey |
            intelligence_fields::kDecisionSummary |
            intelligence_fields::kDecisionStatus |
            intelligence_fields::kDecisionConfidence
    });
    focus_rank_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        decisive_memory_key,
        IntelligenceMemoryLayer::Semantic,
        focus_scope,
        blobs.append_string("{\"decisive_memory\":true}"),
        decisive_task_key,
        decisive_claim_key,
        0.88F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    const StateApplyResult focus_rank_result = store.apply_with_summary(focus_rank_patch);
    assert(focus_rank_result.state_changed);
    assert(focus_rank_result.intelligence_delta.tasks.size() == 2U);
    assert(focus_rank_result.intelligence_delta.claims.size() == 2U);
    assert(focus_rank_result.intelligence_delta.evidence.size() == 5U);
    assert(focus_rank_result.intelligence_delta.decisions.size() == 1U);
    assert(focus_rank_result.intelligence_delta.memories.size() == 5U);

    IntelligenceQuery decisive_focus_query;
    decisive_focus_query.owner = focus_owner;
    decisive_focus_query.scope = focus_scope;
    decisive_focus_query.limit = 2U;
    const IntelligenceSnapshot decisive_focus =
        focus_intelligence_records(store.intelligence(), store.strings(), decisive_focus_query);
    assert(decisive_focus.tasks.size() == 2U);
    assert(decisive_focus.tasks[0].key == decisive_task_key);
    assert(decisive_focus.tasks[1].key == weak_task_key);
    assert(decisive_focus.claims.size() == 2U);
    assert(decisive_focus.claims[0].key == decisive_claim_key);
    assert(decisive_focus.claims[1].key == weak_claim_key);
    assert(!decisive_focus.evidence.empty());
    assert(decisive_focus.evidence[0].key == decisive_evidence_key);
    assert(!decisive_focus.decisions.empty());
    assert(decisive_focus.decisions[0].key == decisive_decision_key);
    assert(!decisive_focus.memories.empty());
    assert(decisive_focus.memories[0].key == decisive_memory_key);

    IntelligenceQuery action_query;
    action_query.owner = focus_owner;
    action_query.subject_label = runtime;
    action_query.relation = relation;
    action_query.object_label = reference_runtime;
    action_query.limit = 2U;
    const IntelligenceSnapshot action_candidates =
        action_intelligence_tasks(store.intelligence(), store.strings(), action_query);
    assert(action_candidates.tasks.size() == 2U);
    assert(action_candidates.tasks[0].key == decisive_task_key);
    assert(action_candidates.tasks[1].key == weak_task_key);

    IntelligenceQuery supporting_claims_query;
    supporting_claims_query.kind = IntelligenceRecordKind::Claims;
    supporting_claims_query.task_key = decisive_task_key;
    supporting_claims_query.limit = 2U;
    const IntelligenceSnapshot supporting_claims =
        supporting_intelligence_claims(store.intelligence(), store.strings(), supporting_claims_query);
    assert(supporting_claims.claims.size() == 1U);
    assert(supporting_claims.claims[0].key == decisive_claim_key);

    IntelligenceQuery ranked_supporting_claims_query;
    ranked_supporting_claims_query.kind = IntelligenceRecordKind::Claims;
    ranked_supporting_claims_query.subject_label = runtime;
    ranked_supporting_claims_query.relation = relation;
    ranked_supporting_claims_query.object_label = reference_runtime;
    ranked_supporting_claims_query.limit = 3U;
    const IntelligenceSnapshot ranked_supporting_claims = supporting_intelligence_claims(
        store.intelligence(),
        store.strings(),
        ranked_supporting_claims_query
    );
    assert(ranked_supporting_claims.claims.size() == 3U);
    assert(ranked_supporting_claims.claims[0].key == decisive_claim_key);
    assert(ranked_supporting_claims.claims[1].key == claim_key);
    assert(ranked_supporting_claims.claims[2].key == weak_claim_key);

    StatePatch related_hops_patch;
    related_hops_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        chain_task_seed_key,
        strings.intern("Related seed task"),
        chain_owner,
        blobs.append_string("{\"task\":\"seed\"}"),
        {},
        IntelligenceTaskStatus::InProgress,
        2,
        0.7F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    related_hops_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        chain_task_second_key,
        strings.intern("Related second task"),
        chain_owner,
        blobs.append_string("{\"task\":\"second\"}"),
        {},
        IntelligenceTaskStatus::Open,
        1,
        0.65F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    related_hops_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        chain_claim_key,
        runtime,
        relation,
        reference_runtime,
        blobs.append_string("{\"claim\":\"shared\"}"),
        IntelligenceClaimStatus::Supported,
        0.8F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    related_hops_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        chain_evidence_key,
        strings.intern("bridge"),
        strings.intern("related-test"),
        blobs.append_string("{\"edge\":\"seed\"}"),
        chain_task_seed_key,
        chain_claim_key,
        0.7F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    related_hops_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        chain_memory_key,
        IntelligenceMemoryLayer::Semantic,
        chain_scope,
        blobs.append_string("{\"edge\":\"second\"}"),
        chain_task_second_key,
        chain_claim_key,
        0.6F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    const StateApplyResult related_hops_result = store.apply_with_summary(related_hops_patch);
    assert(related_hops_result.state_changed);
    assert(related_hops_result.intelligence_delta.tasks.size() == 2U);
    assert(related_hops_result.intelligence_delta.claims.size() == 1U);
    assert(related_hops_result.intelligence_delta.evidence.size() == 1U);
    assert(related_hops_result.intelligence_delta.memories.size() == 1U);

    const IntelligenceSnapshot related_seed_hop1 =
        related_intelligence_records(store.intelligence(), chain_task_seed_key, 0U, 0U, 1U);
    assert(related_seed_hop1.tasks.size() == 1U);
    assert(related_seed_hop1.tasks[0].key == chain_task_seed_key);
    assert(related_seed_hop1.claims.size() == 1U);
    assert(related_seed_hop1.claims[0].key == chain_claim_key);
    assert(related_seed_hop1.evidence.size() == 1U);
    assert(related_seed_hop1.evidence[0].key == chain_evidence_key);
    assert(related_seed_hop1.memories.empty());

    const IntelligenceSnapshot related_seed_hop2 =
        related_intelligence_records(store.intelligence(), chain_task_seed_key, 0U, 0U, 2U);
    assert(related_seed_hop2.tasks.size() == 2U);
    assert(related_seed_hop2.tasks[0].key == chain_task_seed_key);
    assert(related_seed_hop2.tasks[1].key == chain_task_second_key);
    assert(related_seed_hop2.claims.size() == 1U);
    assert(related_seed_hop2.claims[0].key == chain_claim_key);
    assert(related_seed_hop2.evidence.size() == 1U);
    assert(related_seed_hop2.evidence[0].key == chain_evidence_key);
    assert(related_seed_hop2.memories.size() == 1U);
    assert(related_seed_hop2.memories[0].key == chain_memory_key);

    IntelligenceQuery stale_owner_query;
    stale_owner_query.kind = IntelligenceRecordKind::Tasks;
    stale_owner_query.owner = planner_owner;
    stale_owner_query.task_status = IntelligenceTaskStatus::InProgress;
    const IntelligenceSnapshot stale_owner_tasks =
        query_intelligence_records(store.intelligence(), store.strings(), stale_owner_query);
    assert(stale_owner_tasks.tasks.empty());

    StatePatch recorded_task_patch;
    recorded_task_patch.task_records.push_back(TaskRecord{
        strings.intern("effect:write-once"),
        blobs.append_string("request::v1"),
        blobs.append_string("outcome::ok"),
        7U
    });
    const StateApplyResult recorded_task_result = store.apply_with_summary(recorded_task_patch);
    assert(recorded_task_result.state_changed);
    assert(store.get_current_state().version == 5U);
    assert(store.patch_log().size() == 5U);
    assert(store.task_journal().size() == 1U);
    const TaskRecord* recorded_task = store.task_journal().find(strings.intern("effect:write-once"));
    assert(recorded_task != nullptr);
    assert(store.blobs().read_string(recorded_task->request) == "request::v1");
    assert(store.blobs().read_string(recorded_task->output) == "outcome::ok");
    assert(recorded_task->flags == 7U);

    const StateApplyResult duplicate_recorded_task_result = store.apply_with_summary(recorded_task_patch);
    assert(!duplicate_recorded_task_result.state_changed);
    assert(duplicate_recorded_task_result.patch_log_offset == 5U);
    assert(store.get_current_state().version == 5U);
    assert(store.patch_log().size() == 5U);

    StateStore fork = store;
    const StateStore::SharedBacking shared_before = store.shared_backing_with(fork);
    assert(shared_before.blobs);
    assert(shared_before.strings);
    assert(shared_before.knowledge_graph);
    assert(shared_before.intelligence);

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
    assert(shared_after_patch.intelligence);

    const BlobRef fork_blob = fork.blobs().append_string("forked-artifact");
    const InternedStringId fork_label = fork.strings().intern("fork_entity");
    fork.knowledge_graph().upsert_entity(fork_label, fork_blob);
    fork.intelligence().upsert_memory(IntelligenceMemoryWrite{
        fork.strings().intern("memory:fork"),
        IntelligenceMemoryLayer::Working,
        fork.strings().intern("fork"),
        fork.blobs().append_string("{\"forked\":true}"),
        {},
        {},
        0.4F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryImportance
    });

    const StateStore::SharedBacking shared_after_detach = store.shared_backing_with(fork);
    assert(!shared_after_detach.blobs);
    assert(!shared_after_detach.strings);
    assert(!shared_after_detach.knowledge_graph);
    assert(!shared_after_detach.intelligence);
    assert(store.blobs().read_string(fork_blob).empty());
    assert(store.strings().resolve(fork_label).empty());
    assert(store.knowledge_graph().find_entity_by_label(fork_label) == nullptr);
    assert(fork.knowledge_graph().find_entity_by_label(fork_label) != nullptr);
    assert(store.knowledge_graph().entity_count() == 2U);
    assert(fork.knowledge_graph().entity_count() == 3U);
    assert(store.intelligence().memory_count() == 9U);
    assert(fork.intelligence().memory_count() == 10U);
    IntelligenceQuery fork_scope_query;
    fork_scope_query.kind = IntelligenceRecordKind::Memories;
    fork_scope_query.scope = fork.strings().intern("fork");
    const IntelligenceSnapshot store_fork_scope =
        query_intelligence_records(store.intelligence(), store.strings(), fork_scope_query);
    const IntelligenceSnapshot fork_fork_scope =
        query_intelligence_records(fork.intelligence(), fork.strings(), fork_scope_query);
    assert(store_fork_scope.memories.empty());
    assert(fork_fork_scope.memories.size() == 1U);
    assert(store.task_journal().size() == 1U);
    assert(fork.task_journal().size() == 1U);

    std::cout << "state module tests passed" << std::endl;
    return 0;
}
