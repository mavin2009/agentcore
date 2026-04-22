#include "agentcore/state/intelligence/ops.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace agentcore {

namespace {

template <typename EntryT>
bool append_with_limit(std::vector<EntryT>& entries, const EntryT& entry, uint32_t limit) {
    if (limit != 0U && entries.size() >= static_cast<std::size_t>(limit)) {
        return false;
    }
    entries.push_back(entry);
    return true;
}

template <typename EntryT>
bool append_and_continue(std::vector<EntryT>& entries, const EntryT& entry, uint32_t limit) {
    if (!append_with_limit(entries, entry, limit)) {
        return false;
    }
    return limit == 0U || entries.size() < static_cast<std::size_t>(limit);
}

bool matches_key_filter(
    InternedStringId entry_key,
    const IntelligenceQuery& query,
    const StringInterner& strings
) {
    if (query.key != 0U && entry_key != query.key) {
        return false;
    }
    if (!query.key_prefix.empty()) {
        const std::string_view resolved = strings.resolve(entry_key);
        if (resolved.size() < query.key_prefix.size() ||
            resolved.substr(0U, query.key_prefix.size()) != query.key_prefix) {
            return false;
        }
    }
    return true;
}

bool claim_graph_filters_active(const IntelligenceQuery& query) noexcept {
    return query.subject_label != 0U || query.relation != 0U || query.object_label != 0U;
}

bool matches_claim_query(
    const IntelligenceClaim& claim,
    const IntelligenceQuery& query,
    const StringInterner& strings
) {
    return matches_key_filter(claim.key, query, strings) &&
        (query.claim_key == 0U || claim.key == query.claim_key) &&
        (query.subject_label == 0U || claim.subject_label == query.subject_label) &&
        (query.relation == 0U || claim.relation == query.relation) &&
        (query.object_label == 0U || claim.object_label == query.object_label) &&
        (!query.claim_status.has_value() || claim.status == *query.claim_status) &&
        claim.confidence >= query.min_confidence;
}

template <typename IdT>
void insert_sorted_unique_id(std::vector<IdT>& ids, IdT id) {
    const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
    if (iterator == ids.end() || *iterator != id) {
        ids.insert(iterator, id);
    }
}

template <typename IdT>
const std::vector<IdT>& empty_ids() {
    static const std::vector<IdT> kEmpty;
    return kEmpty;
}

template <typename IdT>
void append_sorted_unique_ids(std::vector<IdT>& target, const std::vector<IdT>& ids) {
    for (IdT id : ids) {
        insert_sorted_unique_id(target, id);
    }
}

template <typename EntryT>
bool insert_unique_by_key(
    std::vector<EntryT>& target,
    std::unordered_set<InternedStringId>& seen_keys,
    const EntryT& entry,
    uint32_t limit
) {
    if (!seen_keys.insert(entry.key).second) {
        return false;
    }
    if (!append_with_limit(target, entry, limit)) {
        seen_keys.erase(entry.key);
        return false;
    }
    return true;
}

template <typename IdT>
struct CandidateIds {
    const std::vector<IdT>* ids{nullptr};
    bool constrained{false};
    bool impossible{false};
};

template <typename IdT>
void constrain_candidates(
    CandidateIds<IdT>& candidates,
    bool active,
    const std::vector<IdT>& ids
) {
    if (!active || candidates.impossible) {
        return;
    }
    if (ids.empty()) {
        candidates.ids = &ids;
        candidates.constrained = true;
        candidates.impossible = true;
        return;
    }
    if (!candidates.constrained || candidates.ids == nullptr || ids.size() < candidates.ids->size()) {
        candidates.ids = &ids;
        candidates.constrained = true;
    }
}

template <typename IdT>
const std::vector<IdT>& single_candidate(IdT id, std::vector<IdT>& storage) {
    storage[0] = id;
    return storage;
}

template <typename EntryT, typename IdT, typename LookupFn, typename EntriesFn, typename FilterFn, typename VisitorFn>
void for_each_candidate(
    const CandidateIds<IdT>& candidates,
    LookupFn&& lookup,
    EntriesFn&& entries,
    FilterFn&& filter,
    VisitorFn&& visitor
) {
    if (candidates.impossible) {
        return;
    }

    if (candidates.constrained) {
        for (IdT id : *candidates.ids) {
            const EntryT* entry = lookup(id);
            if (entry == nullptr || !filter(*entry)) {
                continue;
            }
            if (!visitor(*entry)) {
                break;
            }
        }
        return;
    }

    for (const EntryT& entry : entries()) {
        if (!filter(entry)) {
            continue;
        }
        if (!visitor(entry)) {
            break;
        }
    }
}

template <typename VisitorFn>
void for_each_matching_task(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query,
    VisitorFn&& visitor
) {
    if (claim_graph_filters_active(query)) {
        return;
    }
    if (query.key != 0U && query.task_key != 0U && query.key != query.task_key) {
        return;
    }

    CandidateIds<IntelligenceTaskId> candidates;
    std::vector<IntelligenceTaskId> exact_id(1U);
    const InternedStringId exact_key = query.key != 0U ? query.key : query.task_key;
    if (exact_key != 0U) {
        const IntelligenceTask* task = store.find_task_by_key(exact_key);
        constrain_candidates(
            candidates,
            true,
            task == nullptr ? empty_ids<IntelligenceTaskId>() : single_candidate(task->id, exact_id)
        );
    }
    constrain_candidates(candidates, query.owner != 0U, store.task_ids_for_owner(query.owner));
    constrain_candidates(
        candidates,
        query.task_status.has_value(),
        query.task_status.has_value()
            ? store.task_ids_for_status(*query.task_status)
            : empty_ids<IntelligenceTaskId>()
    );

    const auto filter = [&](const IntelligenceTask& task) {
        return matches_key_filter(task.key, query, strings) &&
            (query.task_key == 0U || task.key == query.task_key) &&
            (query.owner == 0U || task.owner == query.owner) &&
            (!query.task_status.has_value() || task.status == *query.task_status) &&
            task.confidence >= query.min_confidence;
    };

    for_each_candidate<IntelligenceTask>(
        candidates,
        [&](IntelligenceTaskId id) { return store.find_task(id); },
        [&]() { return store.tasks(); },
        filter,
        std::forward<VisitorFn>(visitor)
    );
}

template <typename VisitorFn>
void for_each_matching_claim(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query,
    VisitorFn&& visitor
) {
    if (query.key != 0U && query.claim_key != 0U && query.key != query.claim_key) {
        return;
    }

    CandidateIds<IntelligenceClaimId> candidates;
    std::vector<IntelligenceClaimId> exact_id(1U);
    const InternedStringId exact_key = query.key != 0U ? query.key : query.claim_key;
    if (exact_key != 0U) {
        const IntelligenceClaim* claim = store.find_claim_by_key(exact_key);
        constrain_candidates(
            candidates,
            true,
            claim == nullptr ? empty_ids<IntelligenceClaimId>() : single_candidate(claim->id, exact_id)
        );
    }
    constrain_candidates(
        candidates,
        query.claim_status.has_value(),
        query.claim_status.has_value()
            ? store.claim_ids_for_status(*query.claim_status)
            : empty_ids<IntelligenceClaimId>()
    );
    constrain_candidates(
        candidates,
        query.subject_label != 0U,
        store.claim_ids_for_subject(query.subject_label)
    );
    constrain_candidates(
        candidates,
        query.relation != 0U,
        store.claim_ids_for_relation(query.relation)
    );
    constrain_candidates(
        candidates,
        query.object_label != 0U,
        store.claim_ids_for_object(query.object_label)
    );

    for_each_candidate<IntelligenceClaim>(
        candidates,
        [&](IntelligenceClaimId id) { return store.find_claim(id); },
        [&]() { return store.claims(); },
        [&](const IntelligenceClaim& claim) { return matches_claim_query(claim, query, strings); },
        std::forward<VisitorFn>(visitor)
    );
}

template <typename VisitorFn>
void for_each_matching_evidence(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query,
    VisitorFn&& visitor
) {
    if (claim_graph_filters_active(query)) {
        return;
    }
    CandidateIds<IntelligenceEvidenceId> candidates;
    std::vector<IntelligenceEvidenceId> exact_id(1U);
    if (query.key != 0U) {
        const IntelligenceEvidence* evidence = store.find_evidence_by_key(query.key);
        constrain_candidates(
            candidates,
            true,
            evidence == nullptr ? empty_ids<IntelligenceEvidenceId>() : single_candidate(evidence->id, exact_id)
        );
    }
    constrain_candidates(candidates, query.task_key != 0U, store.evidence_ids_for_task_key(query.task_key));
    constrain_candidates(candidates, query.claim_key != 0U, store.evidence_ids_for_claim_key(query.claim_key));
    constrain_candidates(candidates, query.source != 0U, store.evidence_ids_for_source(query.source));

    const auto filter = [&](const IntelligenceEvidence& evidence) {
        return matches_key_filter(evidence.key, query, strings) &&
            (query.task_key == 0U || evidence.task_key == query.task_key) &&
            (query.claim_key == 0U || evidence.claim_key == query.claim_key) &&
            (query.source == 0U || evidence.source == query.source) &&
            evidence.confidence >= query.min_confidence;
    };

    for_each_candidate<IntelligenceEvidence>(
        candidates,
        [&](IntelligenceEvidenceId id) { return store.find_evidence(id); },
        [&]() { return store.evidence(); },
        filter,
        std::forward<VisitorFn>(visitor)
    );
}

template <typename VisitorFn>
void for_each_matching_decision(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query,
    VisitorFn&& visitor
) {
    if (claim_graph_filters_active(query)) {
        return;
    }
    CandidateIds<IntelligenceDecisionId> candidates;
    std::vector<IntelligenceDecisionId> exact_id(1U);
    if (query.key != 0U) {
        const IntelligenceDecision* decision = store.find_decision_by_key(query.key);
        constrain_candidates(
            candidates,
            true,
            decision == nullptr ? empty_ids<IntelligenceDecisionId>() : single_candidate(decision->id, exact_id)
        );
    }
    constrain_candidates(candidates, query.task_key != 0U, store.decision_ids_for_task_key(query.task_key));
    constrain_candidates(candidates, query.claim_key != 0U, store.decision_ids_for_claim_key(query.claim_key));
    constrain_candidates(
        candidates,
        query.decision_status.has_value(),
        query.decision_status.has_value()
            ? store.decision_ids_for_status(*query.decision_status)
            : empty_ids<IntelligenceDecisionId>()
    );

    const auto filter = [&](const IntelligenceDecision& decision) {
        return matches_key_filter(decision.key, query, strings) &&
            (query.task_key == 0U || decision.task_key == query.task_key) &&
            (query.claim_key == 0U || decision.claim_key == query.claim_key) &&
            (!query.decision_status.has_value() || decision.status == *query.decision_status) &&
            decision.confidence >= query.min_confidence;
    };

    for_each_candidate<IntelligenceDecision>(
        candidates,
        [&](IntelligenceDecisionId id) { return store.find_decision(id); },
        [&]() { return store.decisions(); },
        filter,
        std::forward<VisitorFn>(visitor)
    );
}

template <typename VisitorFn>
void for_each_matching_memory(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query,
    VisitorFn&& visitor
) {
    if (claim_graph_filters_active(query)) {
        return;
    }
    CandidateIds<IntelligenceMemoryId> candidates;
    std::vector<IntelligenceMemoryId> exact_id(1U);
    if (query.key != 0U) {
        const IntelligenceMemoryEntry* memory = store.find_memory_by_key(query.key);
        constrain_candidates(
            candidates,
            true,
            memory == nullptr ? empty_ids<IntelligenceMemoryId>() : single_candidate(memory->id, exact_id)
        );
    }
    constrain_candidates(candidates, query.task_key != 0U, store.memory_ids_for_task_key(query.task_key));
    constrain_candidates(candidates, query.claim_key != 0U, store.memory_ids_for_claim_key(query.claim_key));
    constrain_candidates(candidates, query.scope != 0U, store.memory_ids_for_scope(query.scope));
    constrain_candidates(
        candidates,
        query.memory_layer.has_value(),
        query.memory_layer.has_value()
            ? store.memory_ids_for_layer(*query.memory_layer)
            : empty_ids<IntelligenceMemoryId>()
    );

    const auto filter = [&](const IntelligenceMemoryEntry& memory) {
        return matches_key_filter(memory.key, query, strings) &&
            (query.task_key == 0U || memory.task_key == query.task_key) &&
            (query.claim_key == 0U || memory.claim_key == query.claim_key) &&
            (query.scope == 0U || memory.scope == query.scope) &&
            (!query.memory_layer.has_value() || memory.layer == *query.memory_layer) &&
            memory.importance >= query.min_importance;
    };

    for_each_candidate<IntelligenceMemoryEntry>(
        candidates,
        [&](IntelligenceMemoryId id) { return store.find_memory(id); },
        [&]() { return store.memories(); },
        filter,
        std::forward<VisitorFn>(visitor)
    );
}

template <typename VisitorFn>
std::size_t count_matching_tasks(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query,
    VisitorFn&& visitor
) {
    std::size_t count = 0U;
    for_each_matching_task(store, strings, query, [&](const IntelligenceTask& task) {
        ++count;
        return visitor(task, count);
    });
    return count;
}

template <typename EntryT>
void collect_related_task_id(
    const IntelligenceStore& store,
    InternedStringId task_key,
    std::vector<IntelligenceTaskId>& related_task_ids
) {
    if (task_key == 0U) {
        return;
    }
    const IntelligenceTask* task = store.find_task_by_key(task_key);
    if (task != nullptr) {
        insert_sorted_unique_id(related_task_ids, task->id);
    }
}

template <typename EntryT>
void collect_related_claim_id(
    const IntelligenceStore& store,
    InternedStringId claim_key,
    std::vector<IntelligenceClaimId>& related_claim_ids
) {
    if (claim_key == 0U) {
        return;
    }
    const IntelligenceClaim* claim = store.find_claim_by_key(claim_key);
    if (claim != nullptr) {
        insert_sorted_unique_id(related_claim_ids, claim->id);
    }
}

template <typename EntryT>
void collect_related_links(
    const IntelligenceStore& store,
    const EntryT& entry,
    std::vector<IntelligenceTaskId>& related_task_ids,
    std::vector<IntelligenceClaimId>& related_claim_ids
) {
    collect_related_task_id<EntryT>(store, entry.task_key, related_task_ids);
    collect_related_claim_id<EntryT>(store, entry.claim_key, related_claim_ids);
}

uint8_t agenda_task_status_rank(IntelligenceTaskStatus status) noexcept {
    switch (status) {
        case IntelligenceTaskStatus::InProgress:
            return 0U;
        case IntelligenceTaskStatus::Open:
            return 1U;
        case IntelligenceTaskStatus::Blocked:
            return 2U;
        case IntelligenceTaskStatus::Completed:
            return 3U;
        case IntelligenceTaskStatus::Cancelled:
            return 4U;
    }
    return 5U;
}

bool agenda_task_less(const IntelligenceTask& left, const IntelligenceTask& right) noexcept {
    const uint8_t left_status_rank = agenda_task_status_rank(left.status);
    const uint8_t right_status_rank = agenda_task_status_rank(right.status);
    if (left_status_rank != right_status_rank) {
        return left_status_rank < right_status_rank;
    }
    if (left.priority != right.priority) {
        return left.priority > right.priority;
    }
    if (left.confidence != right.confidence) {
        return left.confidence > right.confidence;
    }
    return left.id < right.id;
}

bool recall_memory_less(
    const IntelligenceMemoryEntry& left,
    const IntelligenceMemoryEntry& right
) noexcept {
    if (left.importance != right.importance) {
        return left.importance > right.importance;
    }
    return left.id < right.id;
}

uint8_t focus_claim_status_rank(IntelligenceClaimStatus status) noexcept {
    switch (status) {
        case IntelligenceClaimStatus::Confirmed:
            return 0U;
        case IntelligenceClaimStatus::Supported:
            return 1U;
        case IntelligenceClaimStatus::Proposed:
            return 2U;
        case IntelligenceClaimStatus::Disputed:
            return 3U;
        case IntelligenceClaimStatus::Rejected:
            return 4U;
    }
    return 5U;
}

uint8_t focus_decision_status_rank(IntelligenceDecisionStatus status) noexcept {
    switch (status) {
        case IntelligenceDecisionStatus::Selected:
            return 0U;
        case IntelligenceDecisionStatus::Pending:
            return 1U;
        case IntelligenceDecisionStatus::Superseded:
            return 2U;
        case IntelligenceDecisionStatus::Rejected:
            return 3U;
    }
    return 4U;
}

uint32_t resolve_focus_limit(uint32_t limit) noexcept {
    return limit == 0U ? 5U : limit;
}

uint32_t resolve_focus_candidate_cap(uint32_t limit) noexcept {
    return std::max<uint32_t>(resolve_focus_limit(limit) * 8U, 32U);
}

struct FocusRelationScore {
    uint32_t decisive{0U};
    uint32_t support{0U};
    uint32_t links{0U};
};

struct FocusRecordSignal {
    FocusRelationScore total{};
    FocusRelationScore aligned{};
};

uint32_t quantize_focus_float(float value, uint32_t scale) noexcept {
    if (!(value > 0.0F)) {
        return 0U;
    }
    return static_cast<uint32_t>(value * static_cast<float>(scale) + 0.5F);
}

FocusRelationScore supporting_claim_base_score(const IntelligenceClaim& claim) noexcept {
    FocusRelationScore score;
    const uint32_t confidence = quantize_focus_float(claim.confidence, 1000U);
    score.links = 1U;
    switch (claim.status) {
        case IntelligenceClaimStatus::Confirmed:
            score.decisive = 4U + (claim.confidence >= 0.9F ? 1U : 0U);
            score.support = 3600U + confidence;
            break;
        case IntelligenceClaimStatus::Supported:
            score.decisive = 2U + (claim.confidence >= 0.95F ? 1U : 0U);
            score.support = 2200U + confidence;
            break;
        case IntelligenceClaimStatus::Proposed:
            score.decisive = claim.confidence >= 0.92F ? 1U : 0U;
            score.support = 900U + confidence;
            break;
        case IntelligenceClaimStatus::Disputed:
            score.support = 240U + confidence / 2U;
            break;
        case IntelligenceClaimStatus::Rejected:
            score.support = 60U + confidence / 4U;
            break;
    }
    return score;
}

uint32_t focus_memory_layer_weight(IntelligenceMemoryLayer layer) noexcept {
    switch (layer) {
        case IntelligenceMemoryLayer::Procedural:
            return 320U;
        case IntelligenceMemoryLayer::Semantic:
            return 280U;
        case IntelligenceMemoryLayer::Episodic:
            return 180U;
        case IntelligenceMemoryLayer::Working:
            return 120U;
    }
    return 100U;
}

FocusRelationScore focus_score_for_evidence(const IntelligenceEvidence& evidence) noexcept {
    FocusRelationScore score;
    score.links = 1U;
    score.support = 200U + quantize_focus_float(evidence.confidence, 1000U);
    if (evidence.confidence >= 0.9F) {
        score.decisive = 2U;
    } else if (evidence.confidence >= 0.75F) {
        score.decisive = 1U;
    }
    return score;
}

FocusRelationScore focus_score_for_decision(const IntelligenceDecision& decision) noexcept {
    FocusRelationScore score;
    score.links = 1U;
    const uint32_t confidence = quantize_focus_float(decision.confidence, 1000U);
    switch (decision.status) {
        case IntelligenceDecisionStatus::Selected:
            score.decisive = 4U + (decision.confidence >= 0.9F ? 1U : 0U);
            score.support = 4000U + confidence;
            break;
        case IntelligenceDecisionStatus::Pending:
            score.decisive = decision.confidence >= 0.92F ? 1U : 0U;
            score.support = 1000U + confidence;
            break;
        case IntelligenceDecisionStatus::Superseded:
            score.support = 240U + confidence / 2U;
            break;
        case IntelligenceDecisionStatus::Rejected:
            score.support = 40U + confidence / 4U;
            break;
    }
    return score;
}

FocusRelationScore focus_score_for_memory(const IntelligenceMemoryEntry& memory) noexcept {
    FocusRelationScore score;
    score.links = 1U;
    score.support =
        focus_memory_layer_weight(memory.layer) + quantize_focus_float(memory.importance, 1000U);
    const bool stable_memory =
        memory.layer == IntelligenceMemoryLayer::Semantic ||
        memory.layer == IntelligenceMemoryLayer::Procedural;
    if ((stable_memory && memory.importance >= 0.85F) || memory.importance >= 0.97F) {
        score.decisive = 1U;
    }
    return score;
}

void accumulate_focus_relation_score(
    FocusRelationScore& target,
    const FocusRelationScore& delta
) noexcept {
    target.decisive += delta.decisive;
    target.support += delta.support;
    target.links += delta.links;
}

void accumulate_focus_signal(
    std::unordered_map<InternedStringId, FocusRecordSignal>& signals,
    InternedStringId key,
    const FocusRelationScore& delta,
    bool aligned
) {
    if (key == 0U) {
        return;
    }
    FocusRecordSignal& signal = signals[key];
    accumulate_focus_relation_score(signal.total, delta);
    if (aligned) {
        accumulate_focus_relation_score(signal.aligned, delta);
    }
}

const FocusRecordSignal& lookup_focus_signal(
    const std::unordered_map<InternedStringId, FocusRecordSignal>& signals,
    InternedStringId key
) {
    static const FocusRecordSignal kEmpty{};
    const auto iterator = signals.find(key);
    return iterator == signals.end() ? kEmpty : iterator->second;
}

int compare_focus_relation_score(
    const FocusRelationScore& left,
    const FocusRelationScore& right
) noexcept {
    if (left.decisive != right.decisive) {
        return left.decisive > right.decisive ? -1 : 1;
    }
    if (left.support != right.support) {
        return left.support > right.support ? -1 : 1;
    }
    if (left.links != right.links) {
        return left.links > right.links ? -1 : 1;
    }
    return 0;
}

FocusRecordSignal combine_focus_signals(
    const FocusRecordSignal& left,
    const FocusRecordSignal& right
) noexcept {
    FocusRecordSignal combined;
    accumulate_focus_relation_score(combined.total, left.total);
    accumulate_focus_relation_score(combined.total, right.total);
    accumulate_focus_relation_score(combined.aligned, left.aligned);
    accumulate_focus_relation_score(combined.aligned, right.aligned);
    return combined;
}

template <typename IdT>
void append_sorted_unique_ids_limited(
    std::vector<IdT>& target,
    const std::vector<IdT>& ids,
    uint32_t cap
) {
    for (IdT id : ids) {
        if (target.size() >= static_cast<std::size_t>(cap)) {
            break;
        }
        insert_sorted_unique_id(target, id);
    }
}

template <typename IdT>
bool contains_sorted_id(const std::vector<IdT>& ids, IdT id) {
    return std::binary_search(ids.begin(), ids.end(), id);
}

void add_task_candidate_for_key(
    const IntelligenceStore& store,
    InternedStringId task_key,
    std::vector<IntelligenceTaskId>& task_ids,
    std::vector<InternedStringId>& task_keys,
    uint32_t cap
) {
    if (task_key == 0U || task_ids.size() >= static_cast<std::size_t>(cap)) {
        return;
    }
    const IntelligenceTask* task = store.find_task_by_key(task_key);
    if (task == nullptr) {
        return;
    }
    insert_sorted_unique_id(task_ids, task->id);
    insert_sorted_unique_id(task_keys, task->key);
}

void add_claim_candidate_for_key(
    const IntelligenceStore& store,
    InternedStringId claim_key,
    std::vector<IntelligenceClaimId>& claim_ids,
    std::vector<InternedStringId>& claim_keys,
    uint32_t cap
) {
    if (claim_key == 0U || claim_ids.size() >= static_cast<std::size_t>(cap)) {
        return;
    }
    const IntelligenceClaim* claim = store.find_claim_by_key(claim_key);
    if (claim == nullptr) {
        return;
    }
    insert_sorted_unique_id(claim_ids, claim->id);
    insert_sorted_unique_id(claim_keys, claim->key);
}

void append_related_ids_for_task_keys(
    const IntelligenceStore& store,
    const std::vector<InternedStringId>& task_keys,
    std::vector<IntelligenceEvidenceId>& evidence_ids,
    std::vector<IntelligenceDecisionId>& decision_ids,
    std::vector<IntelligenceMemoryId>& memory_ids,
    uint32_t cap
) {
    for (InternedStringId task_key : task_keys) {
        append_sorted_unique_ids_limited(evidence_ids, store.evidence_ids_for_task_key(task_key), cap);
        append_sorted_unique_ids_limited(decision_ids, store.decision_ids_for_task_key(task_key), cap);
        append_sorted_unique_ids_limited(memory_ids, store.memory_ids_for_task_key(task_key), cap);
    }
}

void append_related_ids_for_claim_keys(
    const IntelligenceStore& store,
    const std::vector<InternedStringId>& claim_keys,
    std::vector<IntelligenceEvidenceId>& evidence_ids,
    std::vector<IntelligenceDecisionId>& decision_ids,
    std::vector<IntelligenceMemoryId>& memory_ids,
    uint32_t cap
) {
    for (InternedStringId claim_key : claim_keys) {
        append_sorted_unique_ids_limited(evidence_ids, store.evidence_ids_for_claim_key(claim_key), cap);
        append_sorted_unique_ids_limited(decision_ids, store.decision_ids_for_claim_key(claim_key), cap);
        append_sorted_unique_ids_limited(memory_ids, store.memory_ids_for_claim_key(claim_key), cap);
    }
}

void expand_task_and_claim_keys_from_records(
    const IntelligenceStore& store,
    const std::vector<IntelligenceEvidenceId>& evidence_ids,
    const std::vector<IntelligenceDecisionId>& decision_ids,
    const std::vector<IntelligenceMemoryId>& memory_ids,
    std::vector<IntelligenceTaskId>& task_ids,
    std::vector<InternedStringId>& task_keys,
    std::vector<IntelligenceClaimId>& claim_ids,
    std::vector<InternedStringId>& claim_keys,
    uint32_t cap
) {
    for (IntelligenceEvidenceId evidence_id : evidence_ids) {
        const IntelligenceEvidence* evidence = store.find_evidence(evidence_id);
        if (evidence == nullptr) {
            continue;
        }
        add_task_candidate_for_key(store, evidence->task_key, task_ids, task_keys, cap);
        add_claim_candidate_for_key(store, evidence->claim_key, claim_ids, claim_keys, cap);
    }
    for (IntelligenceDecisionId decision_id : decision_ids) {
        const IntelligenceDecision* decision = store.find_decision(decision_id);
        if (decision == nullptr) {
            continue;
        }
        add_task_candidate_for_key(store, decision->task_key, task_ids, task_keys, cap);
        add_claim_candidate_for_key(store, decision->claim_key, claim_ids, claim_keys, cap);
    }
    for (IntelligenceMemoryId memory_id : memory_ids) {
        const IntelligenceMemoryEntry* memory = store.find_memory(memory_id);
        if (memory == nullptr) {
            continue;
        }
        add_task_candidate_for_key(store, memory->task_key, task_ids, task_keys, cap);
        add_claim_candidate_for_key(store, memory->claim_key, claim_ids, claim_keys, cap);
    }
}

void collect_related_record_ids_for_frontier(
    const IntelligenceStore& store,
    const std::vector<InternedStringId>& frontier_task_keys,
    const std::vector<InternedStringId>& frontier_claim_keys,
    std::vector<IntelligenceEvidenceId>& evidence_ids,
    std::vector<IntelligenceDecisionId>& decision_ids,
    std::vector<IntelligenceMemoryId>& memory_ids
) {
    for (InternedStringId frontier_task_key : frontier_task_keys) {
        append_sorted_unique_ids(evidence_ids, store.evidence_ids_for_task_key(frontier_task_key));
        append_sorted_unique_ids(decision_ids, store.decision_ids_for_task_key(frontier_task_key));
        append_sorted_unique_ids(memory_ids, store.memory_ids_for_task_key(frontier_task_key));
    }
    for (InternedStringId frontier_claim_key : frontier_claim_keys) {
        append_sorted_unique_ids(evidence_ids, store.evidence_ids_for_claim_key(frontier_claim_key));
        append_sorted_unique_ids(decision_ids, store.decision_ids_for_claim_key(frontier_claim_key));
        append_sorted_unique_ids(memory_ids, store.memory_ids_for_claim_key(frontier_claim_key));
    }
}

bool add_related_task_key(
    const IntelligenceStore& store,
    InternedStringId task_key,
    std::unordered_set<InternedStringId>& visited_task_keys,
    std::unordered_set<InternedStringId>& output_task_keys,
    std::vector<InternedStringId>& next_frontier_task_keys,
    IntelligenceSnapshot& result,
    uint32_t limit
) {
    if (task_key == 0U) {
        return false;
    }
    const IntelligenceTask* task = store.find_task_by_key(task_key);
    if (task == nullptr || !visited_task_keys.insert(task->key).second) {
        return false;
    }
    next_frontier_task_keys.push_back(task->key);
    static_cast<void>(insert_unique_by_key(result.tasks, output_task_keys, *task, limit));
    return true;
}

bool add_related_claim_key(
    const IntelligenceStore& store,
    InternedStringId claim_key,
    std::unordered_set<InternedStringId>& visited_claim_keys,
    std::unordered_set<InternedStringId>& output_claim_keys,
    std::vector<InternedStringId>& next_frontier_claim_keys,
    IntelligenceSnapshot& result,
    uint32_t limit
) {
    if (claim_key == 0U) {
        return false;
    }
    const IntelligenceClaim* claim = store.find_claim_by_key(claim_key);
    if (claim == nullptr || !visited_claim_keys.insert(claim->key).second) {
        return false;
    }
    next_frontier_claim_keys.push_back(claim->key);
    static_cast<void>(insert_unique_by_key(result.claims, output_claim_keys, *claim, limit));
    return true;
}

void collect_task_aligned_claim_keys(
    const IntelligenceStore& store,
    InternedStringId task_key,
    std::unordered_set<InternedStringId>& aligned_claim_keys
) {
    if (task_key == 0U) {
        return;
    }

    for (IntelligenceEvidenceId evidence_id : store.evidence_ids_for_task_key(task_key)) {
        const IntelligenceEvidence* evidence = store.find_evidence(evidence_id);
        if (evidence != nullptr && evidence->claim_key != 0U) {
            aligned_claim_keys.insert(evidence->claim_key);
        }
    }
    for (IntelligenceDecisionId decision_id : store.decision_ids_for_task_key(task_key)) {
        const IntelligenceDecision* decision = store.find_decision(decision_id);
        if (decision != nullptr && decision->claim_key != 0U) {
            aligned_claim_keys.insert(decision->claim_key);
        }
    }
    for (IntelligenceMemoryId memory_id : store.memory_ids_for_task_key(task_key)) {
        const IntelligenceMemoryEntry* memory = store.find_memory(memory_id);
        if (memory != nullptr && memory->claim_key != 0U) {
            aligned_claim_keys.insert(memory->claim_key);
        }
    }
}

struct SupportingClaimSignal {
    FocusRecordSignal support{};
    uint8_t status_rank{0U};
    uint32_t confidence{0U};
    bool task_aligned{false};
};

const SupportingClaimSignal& lookup_supporting_claim_signal(
    const std::unordered_map<InternedStringId, SupportingClaimSignal>& signals,
    InternedStringId key
) {
    static const SupportingClaimSignal kEmpty{};
    const auto iterator = signals.find(key);
    return iterator == signals.end() ? kEmpty : iterator->second;
}

} // namespace

IntelligenceSnapshot query_intelligence_records(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
) {
    IntelligenceSnapshot result;

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Tasks) {
        for_each_matching_task(store, strings, query, [&](const IntelligenceTask& task) {
            return append_and_continue(result.tasks, task, query.limit);
        });
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Claims) {
        for_each_matching_claim(store, strings, query, [&](const IntelligenceClaim& claim) {
            return append_and_continue(result.claims, claim, query.limit);
        });
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Evidence) {
        for_each_matching_evidence(store, strings, query, [&](const IntelligenceEvidence& evidence) {
            return append_and_continue(result.evidence, evidence, query.limit);
        });
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Decisions) {
        for_each_matching_decision(store, strings, query, [&](const IntelligenceDecision& decision) {
            return append_and_continue(result.decisions, decision, query.limit);
        });
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Memories) {
        for_each_matching_memory(store, strings, query, [&](const IntelligenceMemoryEntry& memory) {
            return append_and_continue(result.memories, memory, query.limit);
        });
    }

    return result;
}

IntelligenceSnapshot related_intelligence_records(
    const IntelligenceStore& store,
    InternedStringId task_key,
    InternedStringId claim_key,
    uint32_t limit,
    uint32_t hops
) {
    IntelligenceSnapshot result;
    if ((task_key == 0U && claim_key == 0U) || hops == 0U) {
        return result;
    }

    std::unordered_set<InternedStringId> visited_task_keys;
    std::unordered_set<InternedStringId> visited_claim_keys;
    std::unordered_set<InternedStringId> visited_evidence_keys;
    std::unordered_set<InternedStringId> visited_decision_keys;
    std::unordered_set<InternedStringId> visited_memory_keys;

    std::unordered_set<InternedStringId> output_task_keys;
    std::unordered_set<InternedStringId> output_claim_keys;
    std::unordered_set<InternedStringId> output_evidence_keys;
    std::unordered_set<InternedStringId> output_decision_keys;
    std::unordered_set<InternedStringId> output_memory_keys;

    std::vector<InternedStringId> frontier_task_keys;
    std::vector<InternedStringId> frontier_claim_keys;
    static_cast<void>(add_related_task_key(
        store,
        task_key,
        visited_task_keys,
        output_task_keys,
        frontier_task_keys,
        result,
        limit
    ));
    static_cast<void>(add_related_claim_key(
        store,
        claim_key,
        visited_claim_keys,
        output_claim_keys,
        frontier_claim_keys,
        result,
        limit
    ));

    for (uint32_t hop = 0U; hop < hops; ++hop) {
        if (frontier_task_keys.empty() && frontier_claim_keys.empty()) {
            break;
        }

        std::vector<IntelligenceEvidenceId> related_evidence_ids;
        std::vector<IntelligenceDecisionId> related_decision_ids;
        std::vector<IntelligenceMemoryId> related_memory_ids;
        collect_related_record_ids_for_frontier(
            store,
            frontier_task_keys,
            frontier_claim_keys,
            related_evidence_ids,
            related_decision_ids,
            related_memory_ids
        );

        std::vector<InternedStringId> next_frontier_task_keys;
        std::vector<InternedStringId> next_frontier_claim_keys;

        for (IntelligenceEvidenceId id : related_evidence_ids) {
            const IntelligenceEvidence* evidence = store.find_evidence(id);
            if (evidence == nullptr || !visited_evidence_keys.insert(evidence->key).second) {
                continue;
            }
            static_cast<void>(insert_unique_by_key(result.evidence, output_evidence_keys, *evidence, limit));
            static_cast<void>(add_related_task_key(
                store,
                evidence->task_key,
                visited_task_keys,
                output_task_keys,
                next_frontier_task_keys,
                result,
                limit
            ));
            static_cast<void>(add_related_claim_key(
                store,
                evidence->claim_key,
                visited_claim_keys,
                output_claim_keys,
                next_frontier_claim_keys,
                result,
                limit
            ));
        }

        for (IntelligenceDecisionId id : related_decision_ids) {
            const IntelligenceDecision* decision = store.find_decision(id);
            if (decision == nullptr || !visited_decision_keys.insert(decision->key).second) {
                continue;
            }
            static_cast<void>(insert_unique_by_key(result.decisions, output_decision_keys, *decision, limit));
            static_cast<void>(add_related_task_key(
                store,
                decision->task_key,
                visited_task_keys,
                output_task_keys,
                next_frontier_task_keys,
                result,
                limit
            ));
            static_cast<void>(add_related_claim_key(
                store,
                decision->claim_key,
                visited_claim_keys,
                output_claim_keys,
                next_frontier_claim_keys,
                result,
                limit
            ));
        }

        for (IntelligenceMemoryId id : related_memory_ids) {
            const IntelligenceMemoryEntry* memory = store.find_memory(id);
            if (memory == nullptr || !visited_memory_keys.insert(memory->key).second) {
                continue;
            }
            static_cast<void>(insert_unique_by_key(result.memories, output_memory_keys, *memory, limit));
            static_cast<void>(add_related_task_key(
                store,
                memory->task_key,
                visited_task_keys,
                output_task_keys,
                next_frontier_task_keys,
                result,
                limit
            ));
            static_cast<void>(add_related_claim_key(
                store,
                memory->claim_key,
                visited_claim_keys,
                output_claim_keys,
                next_frontier_claim_keys,
                result,
                limit
            ));
        }

        frontier_task_keys = std::move(next_frontier_task_keys);
        frontier_claim_keys = std::move(next_frontier_claim_keys);
    }

    return result;
}

IntelligenceSnapshot agenda_intelligence_tasks(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
) {
    IntelligenceSnapshot result;
    std::vector<IntelligenceTask> ranked_tasks;

    for_each_matching_task(store, strings, query, [&](const IntelligenceTask& task) {
        ranked_tasks.push_back(task);
        return true;
    });

    std::stable_sort(ranked_tasks.begin(), ranked_tasks.end(), agenda_task_less);
    const std::size_t limit = query.limit == 0U
        ? ranked_tasks.size()
        : std::min(ranked_tasks.size(), static_cast<std::size_t>(query.limit));
    result.tasks.assign(ranked_tasks.begin(), ranked_tasks.begin() + static_cast<std::ptrdiff_t>(limit));
    return result;
}

IntelligenceSnapshot recall_intelligence_memories(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
) {
    IntelligenceSnapshot result;
    std::vector<IntelligenceMemoryEntry> ranked_memories;

    for_each_matching_memory(store, strings, query, [&](const IntelligenceMemoryEntry& memory) {
        ranked_memories.push_back(memory);
        return true;
    });

    std::stable_sort(ranked_memories.begin(), ranked_memories.end(), recall_memory_less);
    const std::size_t limit = query.limit == 0U
        ? ranked_memories.size()
        : std::min(ranked_memories.size(), static_cast<std::size_t>(query.limit));
    result.memories.assign(
        ranked_memories.begin(),
        ranked_memories.begin() + static_cast<std::ptrdiff_t>(limit)
    );
    return result;
}

IntelligenceSnapshot supporting_intelligence_claims(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
) {
    IntelligenceSnapshot result;
    std::unordered_set<InternedStringId> aligned_claim_keys;
    if (query.task_key != 0U) {
        collect_task_aligned_claim_keys(store, query.task_key, aligned_claim_keys);
        if (aligned_claim_keys.empty()) {
            return result;
        }
    }

    std::vector<IntelligenceClaim> ranked_claims;
    std::unordered_map<InternedStringId, SupportingClaimSignal> claim_signals;

    const auto consider_claim = [&](const IntelligenceClaim& claim, bool task_aligned) {
        ranked_claims.push_back(claim);

        SupportingClaimSignal& signal = claim_signals[claim.key];
        signal.status_rank = focus_claim_status_rank(claim.status);
        signal.confidence = quantize_focus_float(claim.confidence, 1000U);
        signal.task_aligned = task_aligned;

        const FocusRelationScore base_score = supporting_claim_base_score(claim);
        accumulate_focus_relation_score(signal.support.total, base_score);
        if (task_aligned) {
            accumulate_focus_relation_score(signal.support.aligned, base_score);
        }

        for (IntelligenceEvidenceId evidence_id : store.evidence_ids_for_claim_key(claim.key)) {
            const IntelligenceEvidence* evidence = store.find_evidence(evidence_id);
            if (evidence == nullptr) {
                continue;
            }
            const FocusRelationScore evidence_score = focus_score_for_evidence(*evidence);
            accumulate_focus_relation_score(signal.support.total, evidence_score);
            if (query.task_key == 0U || evidence->task_key == query.task_key) {
                accumulate_focus_relation_score(signal.support.aligned, evidence_score);
            }
        }

        for (IntelligenceDecisionId decision_id : store.decision_ids_for_claim_key(claim.key)) {
            const IntelligenceDecision* decision = store.find_decision(decision_id);
            if (decision == nullptr) {
                continue;
            }
            const FocusRelationScore decision_score = focus_score_for_decision(*decision);
            accumulate_focus_relation_score(signal.support.total, decision_score);
            if (query.task_key == 0U || decision->task_key == query.task_key) {
                accumulate_focus_relation_score(signal.support.aligned, decision_score);
            }
        }

        for (IntelligenceMemoryId memory_id : store.memory_ids_for_claim_key(claim.key)) {
            const IntelligenceMemoryEntry* memory = store.find_memory(memory_id);
            if (memory == nullptr) {
                continue;
            }
            const FocusRelationScore memory_score = focus_score_for_memory(*memory);
            accumulate_focus_relation_score(signal.support.total, memory_score);
            if (query.task_key == 0U || memory->task_key == query.task_key) {
                accumulate_focus_relation_score(signal.support.aligned, memory_score);
            }
        }
    };

    if (query.task_key != 0U) {
        for (InternedStringId aligned_claim_key : aligned_claim_keys) {
            const IntelligenceClaim* claim = store.find_claim_by_key(aligned_claim_key);
            if (claim == nullptr || !matches_claim_query(*claim, query, strings)) {
                continue;
            }
            consider_claim(*claim, true);
        }
    } else {
        for_each_matching_claim(store, strings, query, [&](const IntelligenceClaim& claim) {
            consider_claim(claim, true);
            return true;
        });
    }

    std::stable_sort(
        ranked_claims.begin(),
        ranked_claims.end(),
        [&](const IntelligenceClaim& left, const IntelligenceClaim& right) {
            const SupportingClaimSignal& left_signal =
                lookup_supporting_claim_signal(claim_signals, left.key);
            const SupportingClaimSignal& right_signal =
                lookup_supporting_claim_signal(claim_signals, right.key);
            if (left_signal.task_aligned != right_signal.task_aligned) {
                return left_signal.task_aligned > right_signal.task_aligned;
            }
            const int aligned_compare =
                compare_focus_relation_score(left_signal.support.aligned, right_signal.support.aligned);
            if (aligned_compare != 0) {
                return aligned_compare < 0;
            }
            const int total_compare =
                compare_focus_relation_score(left_signal.support.total, right_signal.support.total);
            if (total_compare != 0) {
                return total_compare < 0;
            }
            if (left_signal.status_rank != right_signal.status_rank) {
                return left_signal.status_rank < right_signal.status_rank;
            }
            if (left_signal.confidence != right_signal.confidence) {
                return left_signal.confidence > right_signal.confidence;
            }
            return left.id < right.id;
        }
    );

    const std::size_t limit = query.limit == 0U
        ? ranked_claims.size()
        : std::min(ranked_claims.size(), static_cast<std::size_t>(query.limit));
    result.claims.assign(
        ranked_claims.begin(),
        ranked_claims.begin() + static_cast<std::ptrdiff_t>(limit)
    );
    return result;
}

IntelligenceSnapshot action_intelligence_tasks(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
) {
    const uint32_t limit = resolve_focus_limit(query.limit);
    const uint32_t candidate_cap = resolve_focus_candidate_cap(limit);
    IntelligenceSnapshot result;

    IntelligenceQuery task_seed_query;
    task_seed_query.kind = IntelligenceRecordKind::Tasks;
    task_seed_query.task_key = query.task_key;
    task_seed_query.owner = query.owner;
    task_seed_query.min_confidence = query.min_confidence;
    task_seed_query.limit = candidate_cap;
    task_seed_query.task_status = query.task_status;

    IntelligenceQuery claim_seed_query;
    claim_seed_query.kind = IntelligenceRecordKind::Claims;
    claim_seed_query.claim_key = query.claim_key;
    claim_seed_query.subject_label = query.subject_label;
    claim_seed_query.relation = query.relation;
    claim_seed_query.object_label = query.object_label;
    claim_seed_query.min_confidence = query.min_confidence;
    claim_seed_query.limit = candidate_cap;
    claim_seed_query.claim_status = query.claim_status;

    IntelligenceQuery evidence_seed_query;
    evidence_seed_query.kind = IntelligenceRecordKind::Evidence;
    evidence_seed_query.task_key = query.task_key;
    evidence_seed_query.claim_key = query.claim_key;
    evidence_seed_query.source = query.source;
    evidence_seed_query.min_confidence = query.min_confidence;
    evidence_seed_query.limit = candidate_cap;

    IntelligenceQuery decision_seed_query;
    decision_seed_query.kind = IntelligenceRecordKind::Decisions;
    decision_seed_query.task_key = query.task_key;
    decision_seed_query.claim_key = query.claim_key;
    decision_seed_query.min_confidence = query.min_confidence;
    decision_seed_query.limit = candidate_cap;

    IntelligenceQuery memory_seed_query;
    memory_seed_query.kind = IntelligenceRecordKind::Memories;
    memory_seed_query.task_key = query.task_key;
    memory_seed_query.claim_key = query.claim_key;
    memory_seed_query.scope = query.scope;
    memory_seed_query.min_importance = query.min_importance;
    memory_seed_query.limit = candidate_cap;
    memory_seed_query.memory_layer = query.memory_layer;

    const bool task_query_anchored =
        query.task_key != 0U ||
        query.owner != 0U ||
        query.task_status.has_value();
    const bool claim_query_anchored =
        query.claim_key != 0U ||
        query.subject_label != 0U ||
        query.relation != 0U ||
        query.object_label != 0U ||
        query.claim_status.has_value();
    const bool evidence_query_anchored =
        query.task_key != 0U ||
        query.claim_key != 0U ||
        query.source != 0U;
    const bool decision_query_anchored =
        query.task_key != 0U ||
        query.claim_key != 0U ||
        query.decision_status.has_value();
    const bool memory_query_anchored =
        query.task_key != 0U ||
        query.claim_key != 0U ||
        query.scope != 0U ||
        query.memory_layer.has_value() ||
        query.min_importance > 0.0F;

    const IntelligenceSnapshot seeded_tasks = task_query_anchored
        ? agenda_intelligence_tasks(store, strings, task_seed_query)
        : IntelligenceSnapshot{};
    const IntelligenceSnapshot seeded_claims = claim_query_anchored
        ? query_intelligence_records(store, strings, claim_seed_query)
        : IntelligenceSnapshot{};
    const IntelligenceSnapshot seeded_evidence = evidence_query_anchored
        ? query_intelligence_records(store, strings, evidence_seed_query)
        : IntelligenceSnapshot{};
    const IntelligenceSnapshot seeded_decisions = decision_query_anchored
        ? query_intelligence_records(store, strings, decision_seed_query)
        : IntelligenceSnapshot{};
    const IntelligenceSnapshot seeded_memories = memory_query_anchored
        ? recall_intelligence_memories(store, strings, memory_seed_query)
        : IntelligenceSnapshot{};

    std::vector<IntelligenceTaskId> candidate_task_ids;
    std::vector<InternedStringId> candidate_task_keys;
    std::vector<InternedStringId> direct_task_keys;
    std::vector<IntelligenceClaimId> candidate_claim_ids;
    std::vector<InternedStringId> candidate_claim_keys;
    std::vector<InternedStringId> direct_claim_keys;
    std::vector<IntelligenceEvidenceId> candidate_evidence_ids;
    std::vector<IntelligenceDecisionId> candidate_decision_ids;
    std::vector<IntelligenceMemoryId> candidate_memory_ids;

    for (const IntelligenceTask& task : seeded_tasks.tasks) {
        insert_sorted_unique_id(candidate_task_ids, task.id);
        insert_sorted_unique_id(candidate_task_keys, task.key);
        if (task_query_anchored) {
            insert_sorted_unique_id(direct_task_keys, task.key);
        }
    }
    for (const IntelligenceClaim& claim : seeded_claims.claims) {
        insert_sorted_unique_id(candidate_claim_ids, claim.id);
        insert_sorted_unique_id(candidate_claim_keys, claim.key);
        if (claim_query_anchored) {
            insert_sorted_unique_id(direct_claim_keys, claim.key);
        }
    }
    for (const IntelligenceEvidence& evidence : seeded_evidence.evidence) {
        insert_sorted_unique_id(candidate_evidence_ids, evidence.id);
        if (evidence_query_anchored) {
            insert_sorted_unique_id(direct_task_keys, evidence.task_key);
            insert_sorted_unique_id(direct_claim_keys, evidence.claim_key);
        }
    }
    for (const IntelligenceDecision& decision : seeded_decisions.decisions) {
        insert_sorted_unique_id(candidate_decision_ids, decision.id);
    }
    for (const IntelligenceMemoryEntry& memory : seeded_memories.memories) {
        insert_sorted_unique_id(candidate_memory_ids, memory.id);
        if (memory_query_anchored) {
            insert_sorted_unique_id(direct_task_keys, memory.task_key);
            insert_sorted_unique_id(direct_claim_keys, memory.claim_key);
        }
    }

    expand_task_and_claim_keys_from_records(
        store,
        candidate_evidence_ids,
        candidate_decision_ids,
        candidate_memory_ids,
        candidate_task_ids,
        candidate_task_keys,
        candidate_claim_ids,
        candidate_claim_keys,
        candidate_cap
    );

    for (uint32_t round = 0U; round < 2U; ++round) {
        append_related_ids_for_task_keys(
            store,
            candidate_task_keys,
            candidate_evidence_ids,
            candidate_decision_ids,
            candidate_memory_ids,
            candidate_cap
        );
        append_related_ids_for_claim_keys(
            store,
            candidate_claim_keys,
            candidate_evidence_ids,
            candidate_decision_ids,
            candidate_memory_ids,
            candidate_cap
        );
        expand_task_and_claim_keys_from_records(
            store,
            candidate_evidence_ids,
            candidate_decision_ids,
            candidate_memory_ids,
            candidate_task_ids,
            candidate_task_keys,
            candidate_claim_ids,
            candidate_claim_keys,
            candidate_cap
        );
    }

    std::vector<IntelligenceTask> ranked_tasks;
    ranked_tasks.reserve(candidate_task_ids.size());
    for (IntelligenceTaskId task_id : candidate_task_ids) {
        const IntelligenceTask* task = store.find_task(task_id);
        if (task == nullptr || task->confidence < query.min_confidence) {
            continue;
        }
        if (query.owner != 0U && task->owner != query.owner) {
            continue;
        }
        if (query.task_status.has_value() && task->status != *query.task_status) {
            continue;
        }
        ranked_tasks.push_back(*task);
    }

    std::vector<IntelligenceEvidence> ranked_evidence;
    ranked_evidence.reserve(candidate_evidence_ids.size());
    for (IntelligenceEvidenceId evidence_id : candidate_evidence_ids) {
        const IntelligenceEvidence* evidence = store.find_evidence(evidence_id);
        if (evidence == nullptr || evidence->confidence < query.min_confidence) {
            continue;
        }
        ranked_evidence.push_back(*evidence);
    }

    std::vector<IntelligenceDecision> ranked_decisions;
    ranked_decisions.reserve(candidate_decision_ids.size());
    for (IntelligenceDecisionId decision_id : candidate_decision_ids) {
        const IntelligenceDecision* decision = store.find_decision(decision_id);
        if (decision == nullptr || decision->confidence < query.min_confidence) {
            continue;
        }
        ranked_decisions.push_back(*decision);
    }

    std::vector<IntelligenceMemoryEntry> ranked_memories;
    ranked_memories.reserve(candidate_memory_ids.size());
    for (IntelligenceMemoryId memory_id : candidate_memory_ids) {
        const IntelligenceMemoryEntry* memory = store.find_memory(memory_id);
        if (memory == nullptr || memory->importance < query.min_importance) {
            continue;
        }
        ranked_memories.push_back(*memory);
    }

    std::unordered_map<InternedStringId, FocusRecordSignal> task_signals;
    task_signals.reserve(candidate_task_keys.size());

    for (const IntelligenceEvidence& evidence : ranked_evidence) {
        const FocusRelationScore score = focus_score_for_evidence(evidence);
        const bool aligned_task = contains_sorted_id(direct_task_keys, evidence.task_key) ||
            contains_sorted_id(direct_claim_keys, evidence.claim_key);
        accumulate_focus_signal(task_signals, evidence.task_key, score, aligned_task);
    }
    for (const IntelligenceDecision& decision : ranked_decisions) {
        const FocusRelationScore score = focus_score_for_decision(decision);
        const bool aligned_task = contains_sorted_id(direct_task_keys, decision.task_key) ||
            contains_sorted_id(direct_claim_keys, decision.claim_key);
        accumulate_focus_signal(task_signals, decision.task_key, score, aligned_task);
    }
    for (const IntelligenceMemoryEntry& memory : ranked_memories) {
        const FocusRelationScore score = focus_score_for_memory(memory);
        const bool aligned_task = contains_sorted_id(direct_task_keys, memory.task_key) ||
            contains_sorted_id(direct_claim_keys, memory.claim_key);
        accumulate_focus_signal(task_signals, memory.task_key, score, aligned_task);
    }

    const auto task_less = [&](const IntelligenceTask& left, const IntelligenceTask& right) {
        const bool left_exact = query.task_key != 0U && left.key == query.task_key;
        const bool right_exact = query.task_key != 0U && right.key == query.task_key;
        if (left_exact != right_exact) {
            return left_exact;
        }
        const bool left_direct = contains_sorted_id(direct_task_keys, left.key);
        const bool right_direct = contains_sorted_id(direct_task_keys, right.key);
        if (left_direct != right_direct) {
            return left_direct;
        }
        const FocusRecordSignal& left_signal = lookup_focus_signal(task_signals, left.key);
        const FocusRecordSignal& right_signal = lookup_focus_signal(task_signals, right.key);
        const int aligned_compare =
            compare_focus_relation_score(left_signal.aligned, right_signal.aligned);
        if (aligned_compare != 0) {
            return aligned_compare < 0;
        }
        const uint8_t left_status_rank = agenda_task_status_rank(left.status);
        const uint8_t right_status_rank = agenda_task_status_rank(right.status);
        if (left_status_rank != right_status_rank) {
            return left_status_rank < right_status_rank;
        }
        const int total_compare =
            compare_focus_relation_score(left_signal.total, right_signal.total);
        if (total_compare != 0) {
            return total_compare < 0;
        }
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        return left.id < right.id;
    };

    std::stable_sort(ranked_tasks.begin(), ranked_tasks.end(), task_less);
    result.tasks.assign(
        ranked_tasks.begin(),
        ranked_tasks.begin() + static_cast<std::ptrdiff_t>(
            std::min(ranked_tasks.size(), static_cast<std::size_t>(limit))
        )
    );
    return result;
}

IntelligenceSnapshot focus_intelligence_records(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
) {
    const uint32_t limit = resolve_focus_limit(query.limit);
    const uint32_t candidate_cap = resolve_focus_candidate_cap(limit);

    IntelligenceQuery task_seed_query;
    task_seed_query.kind = IntelligenceRecordKind::Tasks;
    task_seed_query.key = query.key;
    task_seed_query.key_prefix = query.key_prefix;
    task_seed_query.task_key = query.task_key;
    task_seed_query.owner = query.owner;
    task_seed_query.min_confidence = query.min_confidence;
    task_seed_query.limit = limit;
    task_seed_query.task_status = query.task_status;

    IntelligenceQuery claim_seed_query;
    claim_seed_query.kind = IntelligenceRecordKind::Claims;
    claim_seed_query.key = query.key;
    claim_seed_query.key_prefix = query.key_prefix;
    claim_seed_query.claim_key = query.claim_key;
    claim_seed_query.subject_label = query.subject_label;
    claim_seed_query.relation = query.relation;
    claim_seed_query.object_label = query.object_label;
    claim_seed_query.min_confidence = query.min_confidence;
    claim_seed_query.limit = limit;
    claim_seed_query.claim_status = query.claim_status;

    IntelligenceQuery evidence_seed_query;
    evidence_seed_query.kind = IntelligenceRecordKind::Evidence;
    evidence_seed_query.key = query.key;
    evidence_seed_query.key_prefix = query.key_prefix;
    evidence_seed_query.task_key = query.task_key;
    evidence_seed_query.claim_key = query.claim_key;
    evidence_seed_query.source = query.source;
    evidence_seed_query.min_confidence = query.min_confidence;
    evidence_seed_query.limit = limit;

    IntelligenceQuery decision_seed_query;
    decision_seed_query.kind = IntelligenceRecordKind::Decisions;
    decision_seed_query.key = query.key;
    decision_seed_query.key_prefix = query.key_prefix;
    decision_seed_query.task_key = query.task_key;
    decision_seed_query.claim_key = query.claim_key;
    decision_seed_query.min_confidence = query.min_confidence;
    decision_seed_query.limit = limit;
    decision_seed_query.decision_status = query.decision_status;

    IntelligenceQuery memory_seed_query;
    memory_seed_query.kind = IntelligenceRecordKind::Memories;
    memory_seed_query.key = query.key;
    memory_seed_query.key_prefix = query.key_prefix;
    memory_seed_query.task_key = query.task_key;
    memory_seed_query.claim_key = query.claim_key;
    memory_seed_query.scope = query.scope;
    memory_seed_query.min_importance = query.min_importance;
    memory_seed_query.limit = limit;
    memory_seed_query.memory_layer = query.memory_layer;

    const IntelligenceSnapshot seeded_tasks = agenda_intelligence_tasks(store, strings, task_seed_query);
    const IntelligenceSnapshot seeded_claims = query_intelligence_records(store, strings, claim_seed_query);
    const IntelligenceSnapshot seeded_evidence = query_intelligence_records(store, strings, evidence_seed_query);
    const IntelligenceSnapshot seeded_decisions = query_intelligence_records(store, strings, decision_seed_query);
    const IntelligenceSnapshot seeded_memories = recall_intelligence_memories(store, strings, memory_seed_query);
    const bool task_query_anchored =
        query.key != 0U ||
        !query.key_prefix.empty() ||
        query.task_key != 0U ||
        query.owner != 0U ||
        query.task_status.has_value();
    const bool claim_query_anchored =
        query.key != 0U ||
        !query.key_prefix.empty() ||
        query.claim_key != 0U ||
        query.subject_label != 0U ||
        query.relation != 0U ||
        query.object_label != 0U ||
        query.claim_status.has_value();

    std::vector<IntelligenceTaskId> candidate_task_ids;
    std::vector<InternedStringId> candidate_task_keys;
    std::vector<InternedStringId> direct_task_keys;
    std::vector<IntelligenceClaimId> candidate_claim_ids;
    std::vector<InternedStringId> candidate_claim_keys;
    std::vector<InternedStringId> direct_claim_keys;
    std::vector<IntelligenceEvidenceId> candidate_evidence_ids;
    std::vector<IntelligenceDecisionId> candidate_decision_ids;
    std::vector<IntelligenceMemoryId> candidate_memory_ids;

    for (const IntelligenceTask& task : seeded_tasks.tasks) {
        insert_sorted_unique_id(candidate_task_ids, task.id);
        insert_sorted_unique_id(candidate_task_keys, task.key);
        if (task_query_anchored) {
            insert_sorted_unique_id(direct_task_keys, task.key);
        }
    }
    for (const IntelligenceClaim& claim : seeded_claims.claims) {
        insert_sorted_unique_id(candidate_claim_ids, claim.id);
        insert_sorted_unique_id(candidate_claim_keys, claim.key);
        if (claim_query_anchored) {
            insert_sorted_unique_id(direct_claim_keys, claim.key);
        }
    }
    for (const IntelligenceEvidence& evidence : seeded_evidence.evidence) {
        insert_sorted_unique_id(candidate_evidence_ids, evidence.id);
    }
    for (const IntelligenceDecision& decision : seeded_decisions.decisions) {
        insert_sorted_unique_id(candidate_decision_ids, decision.id);
    }
    for (const IntelligenceMemoryEntry& memory : seeded_memories.memories) {
        insert_sorted_unique_id(candidate_memory_ids, memory.id);
    }

    expand_task_and_claim_keys_from_records(
        store,
        candidate_evidence_ids,
        candidate_decision_ids,
        candidate_memory_ids,
        candidate_task_ids,
        candidate_task_keys,
        candidate_claim_ids,
        candidate_claim_keys,
        candidate_cap
    );

    for (uint32_t round = 0U; round < 2U; ++round) {
        append_related_ids_for_task_keys(
            store,
            candidate_task_keys,
            candidate_evidence_ids,
            candidate_decision_ids,
            candidate_memory_ids,
            candidate_cap
        );
        append_related_ids_for_claim_keys(
            store,
            candidate_claim_keys,
            candidate_evidence_ids,
            candidate_decision_ids,
            candidate_memory_ids,
            candidate_cap
        );
        expand_task_and_claim_keys_from_records(
            store,
            candidate_evidence_ids,
            candidate_decision_ids,
            candidate_memory_ids,
            candidate_task_ids,
            candidate_task_keys,
            candidate_claim_ids,
            candidate_claim_keys,
            candidate_cap
        );
    }

    std::vector<IntelligenceTask> ranked_tasks;
    ranked_tasks.reserve(candidate_task_ids.size());
    for (IntelligenceTaskId task_id : candidate_task_ids) {
        const IntelligenceTask* task = store.find_task(task_id);
        if (task == nullptr || task->confidence < query.min_confidence) {
            continue;
        }
        ranked_tasks.push_back(*task);
    }

    std::vector<IntelligenceClaim> ranked_claims;
    ranked_claims.reserve(candidate_claim_ids.size());
    for (IntelligenceClaimId claim_id : candidate_claim_ids) {
        const IntelligenceClaim* claim = store.find_claim(claim_id);
        if (claim == nullptr || claim->confidence < query.min_confidence) {
            continue;
        }
        ranked_claims.push_back(*claim);
    }

    std::vector<IntelligenceEvidence> ranked_evidence;
    ranked_evidence.reserve(candidate_evidence_ids.size());
    for (IntelligenceEvidenceId evidence_id : candidate_evidence_ids) {
        const IntelligenceEvidence* evidence = store.find_evidence(evidence_id);
        if (evidence == nullptr || evidence->confidence < query.min_confidence) {
            continue;
        }
        ranked_evidence.push_back(*evidence);
    }

    std::vector<IntelligenceDecision> ranked_decisions;
    ranked_decisions.reserve(candidate_decision_ids.size());
    for (IntelligenceDecisionId decision_id : candidate_decision_ids) {
        const IntelligenceDecision* decision = store.find_decision(decision_id);
        if (decision == nullptr || decision->confidence < query.min_confidence) {
            continue;
        }
        ranked_decisions.push_back(*decision);
    }

    std::vector<IntelligenceMemoryEntry> ranked_memories;
    ranked_memories.reserve(candidate_memory_ids.size());
    for (IntelligenceMemoryId memory_id : candidate_memory_ids) {
        const IntelligenceMemoryEntry* memory = store.find_memory(memory_id);
        if (memory == nullptr || memory->importance < query.min_importance) {
            continue;
        }
        ranked_memories.push_back(*memory);
    }

    std::unordered_map<InternedStringId, FocusRecordSignal> task_signals;
    std::unordered_map<InternedStringId, FocusRecordSignal> claim_signals;
    task_signals.reserve(candidate_task_keys.size());
    claim_signals.reserve(candidate_claim_keys.size());

    for (const IntelligenceEvidence& evidence : ranked_evidence) {
        const FocusRelationScore score = focus_score_for_evidence(evidence);
        const bool aligned_task =
            evidence.claim_key != 0U && contains_sorted_id(direct_claim_keys, evidence.claim_key);
        const bool aligned_claim =
            evidence.task_key != 0U && contains_sorted_id(direct_task_keys, evidence.task_key);
        accumulate_focus_signal(task_signals, evidence.task_key, score, aligned_task);
        accumulate_focus_signal(claim_signals, evidence.claim_key, score, aligned_claim);
    }
    for (const IntelligenceDecision& decision : ranked_decisions) {
        const FocusRelationScore score = focus_score_for_decision(decision);
        const bool aligned_task =
            decision.claim_key != 0U && contains_sorted_id(direct_claim_keys, decision.claim_key);
        const bool aligned_claim =
            decision.task_key != 0U && contains_sorted_id(direct_task_keys, decision.task_key);
        accumulate_focus_signal(task_signals, decision.task_key, score, aligned_task);
        accumulate_focus_signal(claim_signals, decision.claim_key, score, aligned_claim);
    }
    for (const IntelligenceMemoryEntry& memory : ranked_memories) {
        const FocusRelationScore score = focus_score_for_memory(memory);
        const bool aligned_task =
            memory.claim_key != 0U && contains_sorted_id(direct_claim_keys, memory.claim_key);
        const bool aligned_claim =
            memory.task_key != 0U && contains_sorted_id(direct_task_keys, memory.task_key);
        accumulate_focus_signal(task_signals, memory.task_key, score, aligned_task);
        accumulate_focus_signal(claim_signals, memory.claim_key, score, aligned_claim);
    }

    const auto task_less = [&](const IntelligenceTask& left, const IntelligenceTask& right) {
        const bool left_exact = query.task_key != 0U && left.key == query.task_key;
        const bool right_exact = query.task_key != 0U && right.key == query.task_key;
        if (left_exact != right_exact) {
            return left_exact;
        }
        const bool left_direct = contains_sorted_id(direct_task_keys, left.key);
        const bool right_direct = contains_sorted_id(direct_task_keys, right.key);
        if (left_direct != right_direct) {
            return left_direct;
        }
        const FocusRecordSignal& left_signal = lookup_focus_signal(task_signals, left.key);
        const FocusRecordSignal& right_signal = lookup_focus_signal(task_signals, right.key);
        const int aligned_compare =
            compare_focus_relation_score(left_signal.aligned, right_signal.aligned);
        if (aligned_compare != 0) {
            return aligned_compare < 0;
        }
        const uint8_t left_status_rank = agenda_task_status_rank(left.status);
        const uint8_t right_status_rank = agenda_task_status_rank(right.status);
        if (left_status_rank != right_status_rank) {
            return left_status_rank < right_status_rank;
        }
        if (left.priority != right.priority) {
            return left.priority > right.priority;
        }
        const int total_compare =
            compare_focus_relation_score(left_signal.total, right_signal.total);
        if (total_compare != 0) {
            return total_compare < 0;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        return left.id < right.id;
    };

    const auto claim_less = [&](const IntelligenceClaim& left, const IntelligenceClaim& right) {
        const bool left_exact = query.claim_key != 0U && left.key == query.claim_key;
        const bool right_exact = query.claim_key != 0U && right.key == query.claim_key;
        if (left_exact != right_exact) {
            return left_exact;
        }
        const bool left_direct = contains_sorted_id(direct_claim_keys, left.key);
        const bool right_direct = contains_sorted_id(direct_claim_keys, right.key);
        if (left_direct != right_direct) {
            return left_direct;
        }
        const FocusRecordSignal& left_signal = lookup_focus_signal(claim_signals, left.key);
        const FocusRecordSignal& right_signal = lookup_focus_signal(claim_signals, right.key);
        const int aligned_compare =
            compare_focus_relation_score(left_signal.aligned, right_signal.aligned);
        if (aligned_compare != 0) {
            return aligned_compare < 0;
        }
        const uint8_t left_status_rank = focus_claim_status_rank(left.status);
        const uint8_t right_status_rank = focus_claim_status_rank(right.status);
        if (left_status_rank != right_status_rank) {
            return left_status_rank < right_status_rank;
        }
        const int total_compare =
            compare_focus_relation_score(left_signal.total, right_signal.total);
        if (total_compare != 0) {
            return total_compare < 0;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        return left.id < right.id;
    };

    const auto evidence_less = [&](const IntelligenceEvidence& left, const IntelligenceEvidence& right) {
        const bool left_exact = (query.task_key != 0U && left.task_key == query.task_key) ||
            (query.claim_key != 0U && left.claim_key == query.claim_key);
        const bool right_exact = (query.task_key != 0U && right.task_key == query.task_key) ||
            (query.claim_key != 0U && right.claim_key == query.claim_key);
        if (left_exact != right_exact) {
            return left_exact;
        }
        const bool left_direct = contains_sorted_id(direct_task_keys, left.task_key) ||
            contains_sorted_id(direct_claim_keys, left.claim_key);
        const bool right_direct = contains_sorted_id(direct_task_keys, right.task_key) ||
            contains_sorted_id(direct_claim_keys, right.claim_key);
        if (left_direct != right_direct) {
            return left_direct;
        }
        const FocusRecordSignal left_signal = combine_focus_signals(
            lookup_focus_signal(task_signals, left.task_key),
            lookup_focus_signal(claim_signals, left.claim_key)
        );
        const FocusRecordSignal right_signal = combine_focus_signals(
            lookup_focus_signal(task_signals, right.task_key),
            lookup_focus_signal(claim_signals, right.claim_key)
        );
        const bool left_source_match = query.source != 0U && left.source == query.source;
        const bool right_source_match = query.source != 0U && right.source == query.source;
        if (left_source_match != right_source_match) {
            return left_source_match;
        }
        const int aligned_compare =
            compare_focus_relation_score(left_signal.aligned, right_signal.aligned);
        if (aligned_compare != 0) {
            return aligned_compare < 0;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        const int total_compare =
            compare_focus_relation_score(left_signal.total, right_signal.total);
        if (total_compare != 0) {
            return total_compare < 0;
        }
        return left.id < right.id;
    };

    const auto decision_less = [&](const IntelligenceDecision& left, const IntelligenceDecision& right) {
        const bool left_exact = (query.task_key != 0U && left.task_key == query.task_key) ||
            (query.claim_key != 0U && left.claim_key == query.claim_key);
        const bool right_exact = (query.task_key != 0U && right.task_key == query.task_key) ||
            (query.claim_key != 0U && right.claim_key == query.claim_key);
        if (left_exact != right_exact) {
            return left_exact;
        }
        const bool left_direct = contains_sorted_id(direct_task_keys, left.task_key) ||
            contains_sorted_id(direct_claim_keys, left.claim_key);
        const bool right_direct = contains_sorted_id(direct_task_keys, right.task_key) ||
            contains_sorted_id(direct_claim_keys, right.claim_key);
        if (left_direct != right_direct) {
            return left_direct;
        }
        const FocusRecordSignal left_signal = combine_focus_signals(
            lookup_focus_signal(task_signals, left.task_key),
            lookup_focus_signal(claim_signals, left.claim_key)
        );
        const FocusRecordSignal right_signal = combine_focus_signals(
            lookup_focus_signal(task_signals, right.task_key),
            lookup_focus_signal(claim_signals, right.claim_key)
        );
        const int aligned_compare =
            compare_focus_relation_score(left_signal.aligned, right_signal.aligned);
        if (aligned_compare != 0) {
            return aligned_compare < 0;
        }
        const uint8_t left_status_rank = focus_decision_status_rank(left.status);
        const uint8_t right_status_rank = focus_decision_status_rank(right.status);
        if (left_status_rank != right_status_rank) {
            return left_status_rank < right_status_rank;
        }
        if (left.confidence != right.confidence) {
            return left.confidence > right.confidence;
        }
        const int total_compare =
            compare_focus_relation_score(left_signal.total, right_signal.total);
        if (total_compare != 0) {
            return total_compare < 0;
        }
        return left.id < right.id;
    };

    const auto memory_less = [&](const IntelligenceMemoryEntry& left, const IntelligenceMemoryEntry& right) {
        const bool left_exact = (query.task_key != 0U && left.task_key == query.task_key) ||
            (query.claim_key != 0U && left.claim_key == query.claim_key);
        const bool right_exact = (query.task_key != 0U && right.task_key == query.task_key) ||
            (query.claim_key != 0U && right.claim_key == query.claim_key);
        if (left_exact != right_exact) {
            return left_exact;
        }
        const bool left_direct = contains_sorted_id(direct_task_keys, left.task_key) ||
            contains_sorted_id(direct_claim_keys, left.claim_key);
        const bool right_direct = contains_sorted_id(direct_task_keys, right.task_key) ||
            contains_sorted_id(direct_claim_keys, right.claim_key);
        if (left_direct != right_direct) {
            return left_direct;
        }
        const FocusRecordSignal left_signal = combine_focus_signals(
            lookup_focus_signal(task_signals, left.task_key),
            lookup_focus_signal(claim_signals, left.claim_key)
        );
        const FocusRecordSignal right_signal = combine_focus_signals(
            lookup_focus_signal(task_signals, right.task_key),
            lookup_focus_signal(claim_signals, right.claim_key)
        );
        const bool left_scope_match = query.scope != 0U && left.scope == query.scope;
        const bool right_scope_match = query.scope != 0U && right.scope == query.scope;
        if (left_scope_match != right_scope_match) {
            return left_scope_match;
        }
        if (recall_memory_less(left, right)) {
            return true;
        }
        if (recall_memory_less(right, left)) {
            return false;
        }
        const int aligned_compare =
            compare_focus_relation_score(left_signal.aligned, right_signal.aligned);
        if (aligned_compare != 0) {
            return aligned_compare < 0;
        }
        const int total_compare =
            compare_focus_relation_score(left_signal.total, right_signal.total);
        if (total_compare != 0) {
            return total_compare < 0;
        }
        return left.id < right.id;
    };

    std::stable_sort(ranked_tasks.begin(), ranked_tasks.end(), task_less);
    std::stable_sort(ranked_claims.begin(), ranked_claims.end(), claim_less);
    std::stable_sort(ranked_evidence.begin(), ranked_evidence.end(), evidence_less);
    std::stable_sort(ranked_decisions.begin(), ranked_decisions.end(), decision_less);
    std::stable_sort(ranked_memories.begin(), ranked_memories.end(), memory_less);

    IntelligenceSnapshot result;
    result.tasks.assign(
        ranked_tasks.begin(),
        ranked_tasks.begin() + static_cast<std::ptrdiff_t>(std::min(ranked_tasks.size(), static_cast<std::size_t>(limit)))
    );
    result.claims.assign(
        ranked_claims.begin(),
        ranked_claims.begin() + static_cast<std::ptrdiff_t>(std::min(ranked_claims.size(), static_cast<std::size_t>(limit)))
    );
    result.evidence.assign(
        ranked_evidence.begin(),
        ranked_evidence.begin() + static_cast<std::ptrdiff_t>(std::min(ranked_evidence.size(), static_cast<std::size_t>(limit)))
    );
    result.decisions.assign(
        ranked_decisions.begin(),
        ranked_decisions.begin() + static_cast<std::ptrdiff_t>(std::min(ranked_decisions.size(), static_cast<std::size_t>(limit)))
    );
    result.memories.assign(
        ranked_memories.begin(),
        ranked_memories.begin() + static_cast<std::ptrdiff_t>(std::min(ranked_memories.size(), static_cast<std::size_t>(limit)))
    );
    return result;
}

IntelligenceOperationalSummary summarize_intelligence(const IntelligenceStore& store) {
    IntelligenceOperationalSummary summary;
    summary.task_count = store.task_count();
    summary.claim_count = store.claim_count();
    summary.evidence_count = store.evidence_count();
    summary.decision_count = store.decision_count();
    summary.memory_count = store.memory_count();

    summary.open_task_count = store.task_ids_for_status(IntelligenceTaskStatus::Open).size();
    summary.in_progress_task_count = store.task_ids_for_status(IntelligenceTaskStatus::InProgress).size();
    summary.blocked_task_count = store.task_ids_for_status(IntelligenceTaskStatus::Blocked).size();
    summary.completed_task_count = store.task_ids_for_status(IntelligenceTaskStatus::Completed).size();
    summary.cancelled_task_count = store.task_ids_for_status(IntelligenceTaskStatus::Cancelled).size();

    summary.proposed_claim_count = store.claim_ids_for_status(IntelligenceClaimStatus::Proposed).size();
    summary.supported_claim_count = store.claim_ids_for_status(IntelligenceClaimStatus::Supported).size();
    summary.disputed_claim_count = store.claim_ids_for_status(IntelligenceClaimStatus::Disputed).size();
    summary.confirmed_claim_count = store.claim_ids_for_status(IntelligenceClaimStatus::Confirmed).size();
    summary.rejected_claim_count = store.claim_ids_for_status(IntelligenceClaimStatus::Rejected).size();

    summary.pending_decision_count = store.decision_ids_for_status(IntelligenceDecisionStatus::Pending).size();
    summary.selected_decision_count = store.decision_ids_for_status(IntelligenceDecisionStatus::Selected).size();
    summary.superseded_decision_count =
        store.decision_ids_for_status(IntelligenceDecisionStatus::Superseded).size();
    summary.rejected_decision_count = store.decision_ids_for_status(IntelligenceDecisionStatus::Rejected).size();

    summary.working_memory_count = store.memory_ids_for_layer(IntelligenceMemoryLayer::Working).size();
    summary.episodic_memory_count = store.memory_ids_for_layer(IntelligenceMemoryLayer::Episodic).size();
    summary.semantic_memory_count = store.memory_ids_for_layer(IntelligenceMemoryLayer::Semantic).size();
    summary.procedural_memory_count = store.memory_ids_for_layer(IntelligenceMemoryLayer::Procedural).size();

    return summary;
}

std::size_t count_intelligence_records(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
) {
    std::size_t count = 0U;

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Tasks) {
        for_each_matching_task(store, strings, query, [&](const IntelligenceTask&) {
            ++count;
            return query.limit == 0U || count < static_cast<std::size_t>(query.limit);
        });
        if (query.kind != IntelligenceRecordKind::All) {
            return count;
        }
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Claims) {
        std::size_t claim_count = 0U;
        for_each_matching_claim(store, strings, query, [&](const IntelligenceClaim&) {
            ++claim_count;
            return query.limit == 0U || claim_count < static_cast<std::size_t>(query.limit);
        });
        if (query.kind != IntelligenceRecordKind::All) {
            return claim_count;
        }
        count += claim_count;
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Evidence) {
        std::size_t evidence_count = 0U;
        for_each_matching_evidence(store, strings, query, [&](const IntelligenceEvidence&) {
            ++evidence_count;
            return query.limit == 0U || evidence_count < static_cast<std::size_t>(query.limit);
        });
        if (query.kind != IntelligenceRecordKind::All) {
            return evidence_count;
        }
        count += evidence_count;
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Decisions) {
        std::size_t decision_count = 0U;
        for_each_matching_decision(store, strings, query, [&](const IntelligenceDecision&) {
            ++decision_count;
            return query.limit == 0U || decision_count < static_cast<std::size_t>(query.limit);
        });
        if (query.kind != IntelligenceRecordKind::All) {
            return decision_count;
        }
        count += decision_count;
    }

    if (query.kind == IntelligenceRecordKind::All || query.kind == IntelligenceRecordKind::Memories) {
        std::size_t memory_count = 0U;
        for_each_matching_memory(store, strings, query, [&](const IntelligenceMemoryEntry&) {
            ++memory_count;
            return query.limit == 0U || memory_count < static_cast<std::size_t>(query.limit);
        });
        if (query.kind != IntelligenceRecordKind::All) {
            return memory_count;
        }
        count += memory_count;
    }

    return count;
}

std::optional<std::string> select_intelligence_route(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const std::vector<IntelligenceRouteRule>& rules
) {
    for (const IntelligenceRouteRule& rule : rules) {
        const std::size_t count = count_intelligence_records(store, strings, rule.query);
        if (count < static_cast<std::size_t>(rule.min_count)) {
            continue;
        }
        if (rule.max_count.has_value() &&
            count > static_cast<std::size_t>(*rule.max_count)) {
            continue;
        }
        return rule.target;
    }
    return std::nullopt;
}

} // namespace agentcore
