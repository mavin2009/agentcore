#ifndef AGENTCORE_STATE_CONTEXT_CONTEXT_GRAPH_H
#define AGENTCORE_STATE_CONTEXT_CONTEXT_GRAPH_H

#include "agentcore/core/types.h"
#include "agentcore/state/intelligence/model.h"
#include "agentcore/state/knowledge_graph.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace agentcore {

enum class ContextRecordKind : uint8_t {
    Task = 0,
    Claim = 1,
    Evidence = 2,
    Decision = 3,
    Memory = 4,
    Knowledge = 5,
    Count = 6
};

struct ContextQueryPlan {
    ContextQueryPlan();

    uint32_t include_mask{0U};
    std::array<uint16_t, static_cast<std::size_t>(ContextRecordKind::Count)> selector_order{};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    InternedStringId object_label{0};
    InternedStringId owner{0};
    InternedStringId source{0};
    InternedStringId scope{0};
    uint32_t limit{32U};
    uint32_t limit_per_source{5U};
    uint16_t selector_count{0U};
    bool supported_claims_only{false};
    bool confirmed_claims_only{false};
    bool selected_decisions_only{false};
    bool knowledge_neighborhood{false};

    [[nodiscard]] bool includes(ContextRecordKind kind) const noexcept;
};

struct ContextGraphRecordRef {
    ContextRecordKind kind{ContextRecordKind::Task};
    uint32_t id{0};
    int64_t score{0};
};

struct ContextGraphResult {
    std::vector<ContextGraphRecordRef> records;
};

class ContextGraphIndex {
public:
    ContextGraphIndex(
        const IntelligenceStore& intelligence,
        const KnowledgeGraphStore& knowledge_graph,
        const ContextQueryPlan& plan
    );
    ~ContextGraphIndex();

    ContextGraphIndex(ContextGraphIndex&&) noexcept;
    auto operator=(ContextGraphIndex&&) noexcept -> ContextGraphIndex&;

    ContextGraphIndex(const ContextGraphIndex&) = delete;
    auto operator=(const ContextGraphIndex&) -> ContextGraphIndex& = delete;

    [[nodiscard]] ContextGraphResult rank() const;
    [[nodiscard]] std::size_t node_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] const char* context_record_kind_name(ContextRecordKind kind) noexcept;
[[nodiscard]] uint32_t context_record_kind_bit(ContextRecordKind kind) noexcept;

bool context_query_plan_add_selector(ContextQueryPlan& plan, std::string_view selector);

[[nodiscard]] ContextGraphResult rank_context_graph(
    const IntelligenceStore& intelligence,
    const KnowledgeGraphStore& knowledge_graph,
    const ContextQueryPlan& plan
);

} // namespace agentcore

#endif // AGENTCORE_STATE_CONTEXT_CONTEXT_GRAPH_H
