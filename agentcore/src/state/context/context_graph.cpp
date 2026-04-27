#include "agentcore/state/context/context_graph.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace agentcore {

namespace {

constexpr uint16_t kNoSelectorOrder = std::numeric_limits<uint16_t>::max();
constexpr std::size_t kKindCount = static_cast<std::size_t>(ContextRecordKind::Count);

[[nodiscard]] std::size_t kind_index(ContextRecordKind kind) noexcept {
    return static_cast<std::size_t>(kind);
}

[[nodiscard]] int64_t kind_priority(ContextRecordKind kind) noexcept {
    switch (kind) {
        case ContextRecordKind::Task: return 900;
        case ContextRecordKind::Claim: return 860;
        case ContextRecordKind::Evidence: return 830;
        case ContextRecordKind::Decision: return 790;
        case ContextRecordKind::Memory: return 760;
        case ContextRecordKind::Knowledge: return 730;
        case ContextRecordKind::Count: break;
    }
    return 500;
}

[[nodiscard]] uint16_t selector_order_for(const ContextQueryPlan& plan, ContextRecordKind kind) noexcept {
    const uint16_t order = plan.selector_order[kind_index(kind)];
    return order == kNoSelectorOrder ? 4096U : order;
}

void enable_kind(ContextQueryPlan& plan, ContextRecordKind kind, uint16_t selector_order) noexcept {
    plan.include_mask |= context_record_kind_bit(kind);
    uint16_t& current = plan.selector_order[kind_index(kind)];
    current = std::min(current, selector_order);
}

[[nodiscard]] std::string normalize_selector(std::string_view selector) {
    std::size_t begin = 0U;
    std::size_t end = selector.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(selector[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(selector[end - 1U])) != 0) {
        --end;
    }

    std::string normalized;
    normalized.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(selector[index]))));
    }
    return normalized;
}

struct ContextNode {
    ContextGraphRecordRef ref;
    InternedStringId key{0};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    InternedStringId object_label{0};
    InternedStringId owner{0};
    InternedStringId source{0};
    InternedStringId scope{0};
    float confidence{0.0F};
    float importance{0.0F};
    int32_t priority{0};
    int64_t status_score{0};
    uint16_t selector_order{4096U};
    int64_t motif_score{0};
};

struct Edge {
    uint32_t to{0};
    int64_t weight{0};
};

using NodeBuckets = std::unordered_map<InternedStringId, std::vector<uint32_t>>;

[[nodiscard]] int64_t task_status_score(IntelligenceTaskStatus status) noexcept {
    switch (status) {
        case IntelligenceTaskStatus::Open: return 260;
        case IntelligenceTaskStatus::InProgress: return 280;
        case IntelligenceTaskStatus::Blocked: return 80;
        case IntelligenceTaskStatus::Completed: return 120;
        case IntelligenceTaskStatus::Cancelled: return -180;
    }
    return 0;
}

[[nodiscard]] int64_t claim_status_score(IntelligenceClaimStatus status) noexcept {
    switch (status) {
        case IntelligenceClaimStatus::Proposed: return 80;
        case IntelligenceClaimStatus::Supported: return 260;
        case IntelligenceClaimStatus::Disputed: return -40;
        case IntelligenceClaimStatus::Confirmed: return 300;
        case IntelligenceClaimStatus::Rejected: return -180;
    }
    return 0;
}

[[nodiscard]] int64_t decision_status_score(IntelligenceDecisionStatus status) noexcept {
    switch (status) {
        case IntelligenceDecisionStatus::Pending: return 80;
        case IntelligenceDecisionStatus::Selected: return 300;
        case IntelligenceDecisionStatus::Superseded: return -20;
        case IntelligenceDecisionStatus::Rejected: return -160;
    }
    return 0;
}

[[nodiscard]] bool task_matches(const IntelligenceTask& task, const ContextQueryPlan& plan) noexcept {
    return (plan.task_key == 0U || task.key == plan.task_key) &&
        (plan.owner == 0U || task.owner == plan.owner);
}

[[nodiscard]] bool claim_matches(const IntelligenceClaim& claim, const ContextQueryPlan& plan) noexcept {
    if (plan.claim_key != 0U && claim.key != plan.claim_key) {
        return false;
    }
    if (plan.subject_label != 0U && claim.subject_label != plan.subject_label) {
        return false;
    }
    if (plan.relation != 0U && claim.relation != plan.relation) {
        return false;
    }
    if (plan.object_label != 0U && claim.object_label != plan.object_label) {
        return false;
    }
    if (plan.confirmed_claims_only && claim.status != IntelligenceClaimStatus::Confirmed) {
        return false;
    }
    if (plan.supported_claims_only &&
        claim.status != IntelligenceClaimStatus::Supported &&
        claim.status != IntelligenceClaimStatus::Confirmed) {
        return false;
    }
    return true;
}

[[nodiscard]] bool evidence_matches(const IntelligenceEvidence& evidence, const ContextQueryPlan& plan) noexcept {
    return (plan.task_key == 0U || evidence.task_key == plan.task_key) &&
        (plan.claim_key == 0U || evidence.claim_key == plan.claim_key) &&
        (plan.source == 0U || evidence.source == plan.source);
}

[[nodiscard]] bool decision_matches(const IntelligenceDecision& decision, const ContextQueryPlan& plan) noexcept {
    if (plan.task_key != 0U && decision.task_key != plan.task_key) {
        return false;
    }
    if (plan.claim_key != 0U && decision.claim_key != plan.claim_key) {
        return false;
    }
    if (plan.selected_decisions_only && decision.status != IntelligenceDecisionStatus::Selected) {
        return false;
    }
    return true;
}

[[nodiscard]] bool memory_matches(const IntelligenceMemoryEntry& memory, const ContextQueryPlan& plan) noexcept {
    return (plan.task_key == 0U || memory.task_key == plan.task_key) &&
        (plan.claim_key == 0U || memory.claim_key == plan.claim_key) &&
        (plan.scope == 0U || memory.scope == plan.scope);
}

[[nodiscard]] bool knowledge_matches(
    const KnowledgeGraphStore& knowledge_graph,
    const KnowledgeTriple& triple,
    const ContextQueryPlan& plan
) noexcept {
    const KnowledgeEntity* subject = knowledge_graph.find_entity(triple.subject);
    const KnowledgeEntity* object = knowledge_graph.find_entity(triple.object);
    if (subject == nullptr || object == nullptr) {
        return false;
    }
    if (plan.relation != 0U && triple.relation != plan.relation) {
        return false;
    }
    if (!plan.knowledge_neighborhood) {
        return (plan.subject_label == 0U || subject->label == plan.subject_label) &&
            (plan.object_label == 0U || object->label == plan.object_label);
    }
    if (plan.subject_label == 0U && plan.object_label == 0U) {
        return true;
    }
    return subject->label == plan.subject_label ||
        object->label == plan.subject_label ||
        subject->label == plan.object_label ||
        object->label == plan.object_label;
}

void add_bucket(NodeBuckets& buckets, InternedStringId key, uint32_t node_index) {
    if (key == 0U) {
        return;
    }
    buckets[key].push_back(node_index);
}

void add_edge(std::vector<std::vector<Edge>>& adjacency, uint32_t from, uint32_t to, int64_t weight) {
    if (from == to) {
        return;
    }
    adjacency[from].push_back(Edge{to, weight});
}

void add_undirected_edge(
    std::vector<std::vector<Edge>>& adjacency,
    uint32_t left,
    uint32_t right,
    int64_t weight
) {
    add_edge(adjacency, left, right, weight);
    add_edge(adjacency, right, left, std::max<int64_t>(1, weight - 80));
}

void connect_bucket(
    std::vector<std::vector<Edge>>& adjacency,
    uint32_t source,
    const NodeBuckets& buckets,
    InternedStringId key,
    int64_t weight,
    std::size_t max_bucket_size = 256U
) {
    if (key == 0U) {
        return;
    }
    const auto iterator = buckets.find(key);
    if (iterator == buckets.end() || iterator->second.size() > max_bucket_size) {
        return;
    }
    for (uint32_t target : iterator->second) {
        add_undirected_edge(adjacency, source, target, weight);
    }
}

[[nodiscard]] uint64_t context_pair_key(InternedStringId left, InternedStringId right) noexcept {
    return (static_cast<uint64_t>(left) << 32U) ^ static_cast<uint64_t>(right);
}

void apply_motif_scores(
    std::vector<ContextNode>& nodes,
    const NodeBuckets& by_claim_key,
    const NodeBuckets& by_semantic_label
) {
    std::unordered_map<uint64_t, std::unordered_set<InternedStringId>> supported_claim_objects;
    supported_claim_objects.reserve(nodes.size());
    for (const ContextNode& node : nodes) {
        if (node.ref.kind != ContextRecordKind::Claim ||
            node.subject_label == 0U ||
            node.relation == 0U ||
            node.object_label == 0U ||
            node.status_score < 260) {
            continue;
        }
        supported_claim_objects[context_pair_key(node.subject_label, node.relation)].insert(node.object_label);
    }

    std::unordered_set<uint64_t> conflicting_claim_groups;
    conflicting_claim_groups.reserve(supported_claim_objects.size());
    for (const auto& entry : supported_claim_objects) {
        if (entry.second.size() > 1U) {
            conflicting_claim_groups.insert(entry.first);
        }
    }

    const auto boost_exact_knowledge = [&](ContextNode& claim) {
        const auto iterator = by_semantic_label.find(claim.subject_label);
        if (iterator == by_semantic_label.end()) {
            return false;
        }
        bool matched = false;
        for (uint32_t linked_index : iterator->second) {
            ContextNode& linked = nodes[linked_index];
            if (linked.ref.kind != ContextRecordKind::Knowledge ||
                linked.subject_label != claim.subject_label ||
                linked.relation != claim.relation ||
                linked.object_label != claim.object_label) {
                continue;
            }
            claim.motif_score += 380;
            linked.motif_score += 320;
            matched = true;
        }
        return matched;
    };

    for (ContextNode& node : nodes) {
        if (node.ref.kind == ContextRecordKind::Evidence && node.task_key != 0U && node.claim_key != 0U) {
            node.motif_score += 260;
        } else if (node.ref.kind == ContextRecordKind::Decision && node.status_score >= 300) {
            node.motif_score += 320;
        } else if (node.ref.kind == ContextRecordKind::Memory && node.task_key != 0U && node.claim_key != 0U) {
            node.motif_score += 180 + static_cast<int64_t>(std::max(0.0F, node.importance) * 140.0F);
        }
    }

    for (ContextNode& node : nodes) {
        if (node.ref.kind != ContextRecordKind::Claim) {
            continue;
        }

        int64_t support = 0;
        const auto linked = by_claim_key.find(node.key);
        if (linked != by_claim_key.end()) {
            for (uint32_t linked_index : linked->second) {
                ContextNode& support_node = nodes[linked_index];
                switch (support_node.ref.kind) {
                    case ContextRecordKind::Evidence:
                        support += 280 + static_cast<int64_t>(std::max(0.0F, support_node.confidence) * 220.0F);
                        support_node.motif_score += 160;
                        break;
                    case ContextRecordKind::Decision:
                        if (support_node.status_score >= 300) {
                            support += 420;
                            support_node.motif_score += 180;
                        }
                        break;
                    case ContextRecordKind::Memory:
                        support += 140 + static_cast<int64_t>(std::max(0.0F, support_node.importance) * 180.0F);
                        support_node.motif_score += 120;
                        break;
                    default:
                        break;
                }
            }
        }
        if (boost_exact_knowledge(node)) {
            support += 300;
        }
        if (conflicting_claim_groups.find(context_pair_key(node.subject_label, node.relation)) !=
            conflicting_claim_groups.end()) {
            support += 140;
        }
        node.motif_score += std::min<int64_t>(support, 1400);
    }
}

[[nodiscard]] bool node_matches_seed(const ContextNode& node, const ContextQueryPlan& plan) noexcept {
    if (plan.task_key != 0U && (node.key == plan.task_key || node.task_key == plan.task_key)) {
        return true;
    }
    if (plan.claim_key != 0U && (node.key == plan.claim_key || node.claim_key == plan.claim_key)) {
        return true;
    }
    if (plan.subject_label != 0U && node.subject_label == plan.subject_label) {
        return true;
    }
    if (plan.relation != 0U && node.relation == plan.relation) {
        return true;
    }
    if (plan.object_label != 0U && node.object_label == plan.object_label) {
        return true;
    }
    if (plan.owner != 0U && node.owner == plan.owner) {
        return true;
    }
    if (plan.scope != 0U && node.scope == plan.scope) {
        return true;
    }
    if (plan.source != 0U && node.source == plan.source) {
        return true;
    }
    return false;
}

[[nodiscard]] int64_t base_score(const ContextNode& node, const ContextQueryPlan& plan) noexcept {
    int64_t score = kind_priority(node.ref.kind);
    score += std::max<int64_t>(0, 100 - static_cast<int64_t>(node.selector_order) * 8);
    score += node.status_score;
    score += static_cast<int64_t>(std::max(0.0F, node.confidence) * 180.0F);
    score += static_cast<int64_t>(std::max(0.0F, node.importance) * 160.0F);
    score += static_cast<int64_t>(std::max<int32_t>(0, node.priority) * 12);
    score += node.motif_score;
    if (node_matches_seed(node, plan)) {
        score += 2500;
    }
    return score;
}

void append_kind_balanced(
    std::vector<ContextGraphRecordRef>& output,
    const std::vector<ContextNode>& nodes,
    const std::vector<uint32_t>& ranked_indices,
    uint32_t limit
) {
    if (limit == 0U || ranked_indices.empty()) {
        return;
    }
    const std::size_t bounded_limit = std::min<std::size_t>(limit, ranked_indices.size());
    std::array<uint32_t, kKindCount> kind_presence{};
    uint32_t distinct_kinds = 0U;
    for (uint32_t index : ranked_indices) {
        const std::size_t bucket = kind_index(nodes[index].ref.kind);
        if (kind_presence[bucket] == 0U) {
            kind_presence[bucket] = 1U;
            ++distinct_kinds;
        }
    }
    if (distinct_kinds <= 1U) {
        for (std::size_t index = 0U; index < bounded_limit; ++index) {
            output.push_back(nodes[ranked_indices[index]].ref);
        }
        return;
    }

    const uint32_t per_kind_cap = std::max<uint32_t>(2U, (limit / std::max<uint32_t>(1U, distinct_kinds)) + 2U);
    std::array<uint32_t, kKindCount> selected_counts{};
    std::unordered_set<uint32_t> selected_indices;
    selected_indices.reserve(bounded_limit);

    for (uint32_t index : ranked_indices) {
        if (output.size() >= bounded_limit) {
            break;
        }
        const std::size_t bucket = kind_index(nodes[index].ref.kind);
        if (selected_counts[bucket] >= per_kind_cap) {
            continue;
        }
        selected_counts[bucket] += 1U;
        selected_indices.insert(index);
        output.push_back(nodes[index].ref);
    }

    for (uint32_t index : ranked_indices) {
        if (output.size() >= bounded_limit) {
            break;
        }
        if (selected_indices.find(index) != selected_indices.end()) {
            continue;
        }
        output.push_back(nodes[index].ref);
    }
}

} // namespace

ContextQueryPlan::ContextQueryPlan() {
    selector_order.fill(kNoSelectorOrder);
}

bool ContextQueryPlan::includes(ContextRecordKind kind) const noexcept {
    return (include_mask & context_record_kind_bit(kind)) != 0U;
}

const char* context_record_kind_name(ContextRecordKind kind) noexcept {
    switch (kind) {
        case ContextRecordKind::Task: return "task";
        case ContextRecordKind::Claim: return "claim";
        case ContextRecordKind::Evidence: return "evidence";
        case ContextRecordKind::Decision: return "decision";
        case ContextRecordKind::Memory: return "memory";
        case ContextRecordKind::Knowledge: return "knowledge";
        case ContextRecordKind::Count: break;
    }
    return "unknown";
}

uint32_t context_record_kind_bit(ContextRecordKind kind) noexcept {
    if (kind == ContextRecordKind::Count) {
        return 0U;
    }
    return 1U << static_cast<uint32_t>(kind);
}

bool context_query_plan_add_selector(ContextQueryPlan& plan, std::string_view selector) {
    const uint16_t selector_order = plan.selector_count++;
    const std::string normalized = normalize_selector(selector);
    if (normalized == "tasks" || normalized == "tasks.agenda" || normalized == "actions.candidates") {
        enable_kind(plan, ContextRecordKind::Task, selector_order);
        return true;
    }
    if (normalized == "claims" || normalized == "claims.all") {
        enable_kind(plan, ContextRecordKind::Claim, selector_order);
        return true;
    }
    if (normalized == "claims.supported" || normalized == "claims.confirmed") {
        enable_kind(plan, ContextRecordKind::Claim, selector_order);
        plan.supported_claims_only = true;
        plan.confirmed_claims_only = normalized == "claims.confirmed";
        return true;
    }
    if (normalized == "evidence" || normalized == "evidence.all" || normalized == "evidence.relevant") {
        enable_kind(plan, ContextRecordKind::Evidence, selector_order);
        return true;
    }
    if (normalized == "decisions" || normalized == "decisions.all" || normalized == "decisions.selected") {
        enable_kind(plan, ContextRecordKind::Decision, selector_order);
        plan.selected_decisions_only = normalized == "decisions.selected";
        return true;
    }
    if (normalized == "memories" || normalized == "memories.recall" || normalized.rfind("memories.", 0U) == 0U) {
        enable_kind(plan, ContextRecordKind::Memory, selector_order);
        return true;
    }
    if (normalized == "knowledge" || normalized == "knowledge.neighborhood") {
        enable_kind(plan, ContextRecordKind::Knowledge, selector_order);
        plan.knowledge_neighborhood = normalized == "knowledge.neighborhood";
        return true;
    }
    if (normalized == "intelligence.focus") {
        enable_kind(plan, ContextRecordKind::Task, selector_order);
        enable_kind(plan, ContextRecordKind::Claim, selector_order);
        enable_kind(plan, ContextRecordKind::Evidence, selector_order);
        enable_kind(plan, ContextRecordKind::Decision, selector_order);
        enable_kind(plan, ContextRecordKind::Memory, selector_order);
        return true;
    }
    return false;
}

struct ContextGraphIndex::Impl {
    ContextGraphResult result;
    std::size_t node_count{0};
};

ContextGraphIndex::ContextGraphIndex(
    const IntelligenceStore& intelligence,
    const KnowledgeGraphStore& knowledge_graph,
    const ContextQueryPlan& plan
) : impl_(std::make_unique<Impl>()) {
    impl_->node_count =
        intelligence.task_count() +
        intelligence.claim_count() +
        intelligence.evidence_count() +
        intelligence.decision_count() +
        intelligence.memory_count() +
        knowledge_graph.triple_count();
    impl_->result = rank_context_graph(intelligence, knowledge_graph, plan);
}

ContextGraphIndex::~ContextGraphIndex() = default;

ContextGraphIndex::ContextGraphIndex(ContextGraphIndex&&) noexcept = default;

auto ContextGraphIndex::operator=(ContextGraphIndex&&) noexcept -> ContextGraphIndex& = default;

ContextGraphResult ContextGraphIndex::rank() const {
    return impl_ == nullptr ? ContextGraphResult{} : impl_->result;
}

std::size_t ContextGraphIndex::node_count() const noexcept {
    return impl_ == nullptr ? 0U : impl_->node_count;
}

ContextGraphResult rank_context_graph(
    const IntelligenceStore& intelligence,
    const KnowledgeGraphStore& knowledge_graph,
    const ContextQueryPlan& plan
) {
    ContextGraphResult result;
    if (plan.include_mask == 0U || plan.limit == 0U) {
        return result;
    }

    std::vector<ContextNode> nodes;
    nodes.reserve(
        intelligence.task_count() +
        intelligence.claim_count() +
        intelligence.evidence_count() +
        intelligence.decision_count() +
        intelligence.memory_count() +
        knowledge_graph.triple_count()
    );

    const auto append_node = [&](ContextNode node) {
        node.selector_order = selector_order_for(plan, node.ref.kind);
        nodes.push_back(node);
    };

    if (plan.includes(ContextRecordKind::Task)) {
        for (const IntelligenceTask& task : intelligence.tasks()) {
            if (!task_matches(task, plan)) {
                continue;
            }
            append_node(ContextNode{
                ContextGraphRecordRef{ContextRecordKind::Task, task.id, 0},
                task.key,
                0U,
                0U,
                0U,
                0U,
                0U,
                task.owner,
                0U,
                0U,
                task.confidence,
                0.0F,
                task.priority,
                task_status_score(task.status),
                4096U
            });
        }
    }

    if (plan.includes(ContextRecordKind::Claim)) {
        for (const IntelligenceClaim& claim : intelligence.claims()) {
            if (!claim_matches(claim, plan)) {
                continue;
            }
            append_node(ContextNode{
                ContextGraphRecordRef{ContextRecordKind::Claim, claim.id, 0},
                claim.key,
                0U,
                0U,
                claim.subject_label,
                claim.relation,
                claim.object_label,
                0U,
                0U,
                0U,
                claim.confidence,
                0.0F,
                0,
                claim_status_score(claim.status),
                4096U
            });
        }
    }

    if (plan.includes(ContextRecordKind::Evidence)) {
        for (const IntelligenceEvidence& evidence : intelligence.evidence()) {
            if (!evidence_matches(evidence, plan)) {
                continue;
            }
            append_node(ContextNode{
                ContextGraphRecordRef{ContextRecordKind::Evidence, evidence.id, 0},
                evidence.key,
                evidence.task_key,
                evidence.claim_key,
                0U,
                0U,
                0U,
                0U,
                evidence.source,
                0U,
                evidence.confidence,
                0.0F,
                0,
                0,
                4096U
            });
        }
    }

    if (plan.includes(ContextRecordKind::Decision)) {
        for (const IntelligenceDecision& decision : intelligence.decisions()) {
            if (!decision_matches(decision, plan)) {
                continue;
            }
            append_node(ContextNode{
                ContextGraphRecordRef{ContextRecordKind::Decision, decision.id, 0},
                decision.key,
                decision.task_key,
                decision.claim_key,
                0U,
                0U,
                0U,
                0U,
                0U,
                0U,
                decision.confidence,
                0.0F,
                0,
                decision_status_score(decision.status),
                4096U
            });
        }
    }

    if (plan.includes(ContextRecordKind::Memory)) {
        for (const IntelligenceMemoryEntry& memory : intelligence.memories()) {
            if (!memory_matches(memory, plan)) {
                continue;
            }
            append_node(ContextNode{
                ContextGraphRecordRef{ContextRecordKind::Memory, memory.id, 0},
                memory.key,
                memory.task_key,
                memory.claim_key,
                0U,
                0U,
                0U,
                0U,
                0U,
                memory.scope,
                0.0F,
                memory.importance,
                0,
                0,
                4096U
            });
        }
    }

    if (plan.includes(ContextRecordKind::Knowledge)) {
        const std::optional<InternedStringId> subject =
            plan.knowledge_neighborhood ? std::nullopt :
            (plan.subject_label == 0U ? std::nullopt : std::optional<InternedStringId>{plan.subject_label});
        const std::optional<InternedStringId> relation =
            plan.relation == 0U ? std::nullopt : std::optional<InternedStringId>{plan.relation};
        const std::optional<InternedStringId> object =
            plan.knowledge_neighborhood ? std::nullopt :
            (plan.object_label == 0U ? std::nullopt : std::optional<InternedStringId>{plan.object_label});

        std::unordered_set<KnowledgeTripleId> seen_triples;
        for (const KnowledgeTriple* triple : knowledge_graph.match(subject, relation, object)) {
            if (triple == nullptr || !seen_triples.insert(triple->id).second) {
                continue;
            }
            if (!knowledge_matches(knowledge_graph, *triple, plan)) {
                continue;
            }
            const KnowledgeEntity* subject_entity = knowledge_graph.find_entity(triple->subject);
            const KnowledgeEntity* object_entity = knowledge_graph.find_entity(triple->object);
            if (subject_entity == nullptr || object_entity == nullptr) {
                continue;
            }
            append_node(ContextNode{
                ContextGraphRecordRef{ContextRecordKind::Knowledge, triple->id, 0},
                triple->id,
                0U,
                0U,
                subject_entity->label,
                triple->relation,
                object_entity->label,
                0U,
                0U,
                0U,
                0.0F,
                0.0F,
                0,
                0,
                4096U
            });
        }
    }

    if (nodes.empty()) {
        return result;
    }

    NodeBuckets by_key;
    NodeBuckets by_task_key;
    NodeBuckets by_claim_key;
    NodeBuckets by_semantic_label;
    by_key.reserve(nodes.size());
    by_task_key.reserve(nodes.size());
    by_claim_key.reserve(nodes.size());
    by_semantic_label.reserve(nodes.size() * 2U);

    for (uint32_t index = 0U; index < nodes.size(); ++index) {
        const ContextNode& node = nodes[index];
        add_bucket(by_key, node.key, index);
        add_bucket(by_task_key, node.task_key, index);
        add_bucket(by_claim_key, node.claim_key, index);
        add_bucket(by_semantic_label, node.subject_label, index);
        add_bucket(by_semantic_label, node.object_label, index);
        add_bucket(by_semantic_label, node.relation, index);
    }
    apply_motif_scores(nodes, by_claim_key, by_semantic_label);

    std::vector<std::vector<Edge>> adjacency(nodes.size());
    for (uint32_t index = 0U; index < nodes.size(); ++index) {
        const ContextNode& node = nodes[index];
        if (node.ref.kind == ContextRecordKind::Task) {
            connect_bucket(adjacency, index, by_task_key, node.key, 520);
        }
        if (node.ref.kind == ContextRecordKind::Claim) {
            connect_bucket(adjacency, index, by_claim_key, node.key, 560);
            connect_bucket(adjacency, index, by_semantic_label, node.subject_label, 210, 128U);
            connect_bucket(adjacency, index, by_semantic_label, node.object_label, 210, 128U);
            connect_bucket(adjacency, index, by_semantic_label, node.relation, 110, 64U);
        }
        if (node.ref.kind == ContextRecordKind::Knowledge) {
            connect_bucket(adjacency, index, by_semantic_label, node.subject_label, 160, 128U);
            connect_bucket(adjacency, index, by_semantic_label, node.object_label, 160, 128U);
            connect_bucket(adjacency, index, by_semantic_label, node.relation, 90, 64U);
        }
        connect_bucket(adjacency, index, by_key, node.task_key, 440);
        connect_bucket(adjacency, index, by_key, node.claim_key, 500);
    }

    std::vector<int64_t> scores(nodes.size(), 0);
    std::vector<int64_t> frontier(nodes.size(), 0);
    for (uint32_t index = 0U; index < nodes.size(); ++index) {
        scores[index] = base_score(nodes[index], plan);
        frontier[index] = scores[index];
    }

    for (uint32_t depth = 1U; depth <= 2U; ++depth) {
        const int64_t decay = depth == 1U ? 4 : 7;
        std::vector<int64_t> propagated(nodes.size(), 0);
        for (uint32_t index = 0U; index < nodes.size(); ++index) {
            if (frontier[index] <= 0) {
                continue;
            }
            for (const Edge& edge : adjacency[index]) {
                propagated[edge.to] = std::max(propagated[edge.to], (frontier[index] / decay) + edge.weight);
            }
        }
        for (uint32_t index = 0U; index < nodes.size(); ++index) {
            scores[index] = std::max(scores[index], propagated[index]);
        }
        frontier = std::move(propagated);
    }

    std::vector<uint32_t> ranked_indices(nodes.size());
    for (uint32_t index = 0U; index < nodes.size(); ++index) {
        nodes[index].ref.score = scores[index];
        ranked_indices[index] = index;
    }

    std::sort(ranked_indices.begin(), ranked_indices.end(), [&](uint32_t left_index, uint32_t right_index) {
        const ContextNode& left = nodes[left_index];
        const ContextNode& right = nodes[right_index];
        if (left.ref.score != right.ref.score) {
            return left.ref.score > right.ref.score;
        }
        if (left.selector_order != right.selector_order) {
            return left.selector_order < right.selector_order;
        }
        if (kind_priority(left.ref.kind) != kind_priority(right.ref.kind)) {
            return kind_priority(left.ref.kind) > kind_priority(right.ref.kind);
        }
        if (left.ref.kind != right.ref.kind) {
            return kind_index(left.ref.kind) < kind_index(right.ref.kind);
        }
        if (left.key != right.key) {
            return left.key < right.key;
        }
        return left.ref.id < right.ref.id;
    });

    const uint32_t ranked_limit = std::min<uint32_t>(
        static_cast<uint32_t>(std::min<std::size_t>(nodes.size(), std::numeric_limits<uint32_t>::max())),
        std::max<uint32_t>(plan.limit, plan.limit_per_source)
    );
    result.records.reserve(std::min<std::size_t>(ranked_limit, ranked_indices.size()));
    append_kind_balanced(result.records, nodes, ranked_indices, ranked_limit);
    return result;
}

} // namespace agentcore
