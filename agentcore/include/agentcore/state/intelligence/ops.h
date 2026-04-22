#ifndef AGENTCORE_STATE_INTELLIGENCE_OPS_H
#define AGENTCORE_STATE_INTELLIGENCE_OPS_H

#include "agentcore/state/intelligence/model.h"
#include "agentcore/state/state_store.h"
#include <optional>
#include <string>
#include <vector>

namespace agentcore {

enum class IntelligenceRecordKind : uint8_t {
    All = 0,
    Tasks = 1,
    Claims = 2,
    Evidence = 3,
    Decisions = 4,
    Memories = 5
};

struct IntelligenceQuery {
    IntelligenceRecordKind kind{IntelligenceRecordKind::All};
    InternedStringId key{0};
    std::string key_prefix;
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    InternedStringId object_label{0};
    InternedStringId owner{0};
    InternedStringId source{0};
    InternedStringId scope{0};
    float min_confidence{0.0F};
    float min_importance{0.0F};
    uint32_t limit{0};
    std::optional<IntelligenceTaskStatus> task_status;
    std::optional<IntelligenceClaimStatus> claim_status;
    std::optional<IntelligenceDecisionStatus> decision_status;
    std::optional<IntelligenceMemoryLayer> memory_layer;
};

struct IntelligenceOperationalSummary {
    std::size_t task_count{0};
    std::size_t claim_count{0};
    std::size_t evidence_count{0};
    std::size_t decision_count{0};
    std::size_t memory_count{0};

    std::size_t open_task_count{0};
    std::size_t in_progress_task_count{0};
    std::size_t blocked_task_count{0};
    std::size_t completed_task_count{0};
    std::size_t cancelled_task_count{0};

    std::size_t proposed_claim_count{0};
    std::size_t supported_claim_count{0};
    std::size_t disputed_claim_count{0};
    std::size_t confirmed_claim_count{0};
    std::size_t rejected_claim_count{0};

    std::size_t pending_decision_count{0};
    std::size_t selected_decision_count{0};
    std::size_t superseded_decision_count{0};
    std::size_t rejected_decision_count{0};

    std::size_t working_memory_count{0};
    std::size_t episodic_memory_count{0};
    std::size_t semantic_memory_count{0};
    std::size_t procedural_memory_count{0};
};

struct IntelligenceRouteRule {
    IntelligenceQuery query;
    uint32_t min_count{1};
    std::optional<uint32_t> max_count;
    std::string target;
};

[[nodiscard]] IntelligenceSnapshot query_intelligence_records(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
);

[[nodiscard]] IntelligenceSnapshot related_intelligence_records(
    const IntelligenceStore& store,
    InternedStringId task_key,
    InternedStringId claim_key,
    uint32_t limit = 0U,
    uint32_t hops = 1U
);

[[nodiscard]] IntelligenceSnapshot agenda_intelligence_tasks(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
);

[[nodiscard]] IntelligenceSnapshot recall_intelligence_memories(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
);

[[nodiscard]] IntelligenceSnapshot supporting_intelligence_claims(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
);

[[nodiscard]] IntelligenceSnapshot action_intelligence_tasks(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
);

[[nodiscard]] IntelligenceSnapshot focus_intelligence_records(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
);

[[nodiscard]] IntelligenceOperationalSummary summarize_intelligence(
    const IntelligenceStore& store
);

[[nodiscard]] std::size_t count_intelligence_records(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const IntelligenceQuery& query
);

[[nodiscard]] std::optional<std::string> select_intelligence_route(
    const IntelligenceStore& store,
    const StringInterner& strings,
    const std::vector<IntelligenceRouteRule>& rules
);

} // namespace agentcore

#endif // AGENTCORE_STATE_INTELLIGENCE_OPS_H
