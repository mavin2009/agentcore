#ifndef AGENTCORE_GRAPH_IR_H
#define AGENTCORE_GRAPH_IR_H

#include "agentcore/core/types.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentcore {

enum class NodePolicyFlag : uint32_t {
    None = 0,
    AllowFanOut = 1U << 0,
    StopAfterNode = 1U << 1,
    JoinIncomingBranches = 1U << 2,
    CreateJoinScope = 1U << 3,
    ReactToKnowledgeGraph = 1U << 4
};

constexpr uint32_t node_policy_mask(NodePolicyFlag flag) {
    return static_cast<uint32_t>(flag);
}

constexpr bool has_node_policy(uint32_t flags, NodePolicyFlag flag) {
    return (flags & node_policy_mask(flag)) != 0U;
}

template <typename T>
class ArrayView {
public:
    using value_type = T;

    constexpr ArrayView() = default;
    constexpr ArrayView(const T* data, std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] const T* begin() const noexcept { return data_; }
    [[nodiscard]] const T* end() const noexcept { return data_ + size_; }
    [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return data_[index]; }
    [[nodiscard]] const T* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }

private:
    const T* data_{nullptr};
    std::size_t size_{0};
};

struct EdgeRange {
    uint32_t offset{0};
    uint32_t count{0};
};

enum class NodeKind : uint8_t {
    Compute,
    Control,
    Tool,
    Model,
    Aggregate,
    Human,
    Subgraph
};

enum class JoinMergeStrategy : uint8_t {
    RequireEqual,
    RequireSingleWriter,
    LastWriterWins,
    FirstWriterWins,
    SumInt64,
    MaxInt64,
    MinInt64,
    LogicalOr,
    LogicalAnd
};

struct FieldMergeRule {
    StateKey key{0};
    JoinMergeStrategy strategy{JoinMergeStrategy::RequireEqual};
};

enum class KnowledgeSubscriptionKind : uint8_t {
    EntityLabel,
    TriplePattern
};

struct KnowledgeSubscription {
    KnowledgeSubscriptionKind kind{KnowledgeSubscriptionKind::TriplePattern};
    std::string entity_label;
    std::string subject_label;
    std::string relation;
    std::string object_label;
};

struct SubgraphStateBinding {
    StateKey parent_key{0};
    StateKey child_key{0};
};

enum class SubgraphSessionMode : uint8_t {
    Ephemeral,
    Persistent
};

struct SubgraphBinding {
    GraphId graph_id{0};
    std::string namespace_name;
    std::vector<SubgraphStateBinding> input_bindings;
    std::vector<SubgraphStateBinding> output_bindings;
    bool propagate_knowledge_graph{false};
    uint32_t initial_field_count{0};
    SubgraphSessionMode session_mode{SubgraphSessionMode::Ephemeral};
    std::optional<StateKey> session_id_source_key;

    [[nodiscard]] bool valid() const noexcept {
        if (graph_id == 0U) {
            return false;
        }
        if (session_mode == SubgraphSessionMode::Persistent) {
            return session_id_source_key.has_value();
        }
        return !session_id_source_key.has_value();
    }
};

struct NodeDefinition {
    NodeId id{0};
    NodeKind kind{NodeKind::Compute};
    std::string name;
    uint32_t policy_flags{0};
    uint32_t timeout_ms{0};
    uint16_t retry_limit{0};
    NodeExecutorFn executor{nullptr};
    EdgeRange outgoing_range{};
    std::vector<FieldMergeRule> field_merge_rules;
    std::vector<KnowledgeSubscription> knowledge_subscriptions;
    std::optional<SubgraphBinding> subgraph;
};

enum class EdgeKind : uint8_t {
    Always,
    OnSuccess,
    OnSoftFail,
    OnHardFail,
    Conditional
};

struct EdgeDefinition {
    EdgeId id{0};
    NodeId from{0};
    NodeId to{0};
    EdgeKind kind{EdgeKind::Always};
    ConditionFn condition{nullptr};
    uint16_t priority{0};
};

struct IdLookupEntry {
    uint32_t id{0};
    uint32_t index{0};
};

struct CompiledEdgeRoute {
    uint32_t edge_index{0};
    uint8_t result_mask{0};
    bool requires_condition{false};
};

struct CompiledNodeRouting {
    uint32_t route_offset{0};
    uint32_t route_count{0};
};

struct CompiledKnowledgeSubscription {
    uint32_t node_index{0};
    uint32_t subscription_index{0};
    KnowledgeSubscriptionKind kind{KnowledgeSubscriptionKind::TriplePattern};
};

struct GraphDefinition {
    GraphId id{0};
    std::string name;
    std::vector<NodeDefinition> nodes;
    std::vector<EdgeDefinition> edges;
    std::vector<EdgeId> edge_index_table;
    NodeId entry{0};
    std::vector<uint32_t> dense_node_lookup;
    std::vector<uint32_t> dense_edge_lookup;
    std::vector<IdLookupEntry> sparse_node_lookup;
    std::vector<IdLookupEntry> sparse_edge_lookup;
    std::vector<CompiledEdgeRoute> compiled_routes;
    std::vector<CompiledNodeRouting> compiled_node_routing;
    std::vector<CompiledKnowledgeSubscription> compiled_knowledge_subscriptions;
    std::unordered_map<std::string, std::vector<uint32_t>> compiled_entity_subscription_index;
    std::unordered_map<std::string, std::vector<uint32_t>> compiled_subject_subscription_index;
    std::unordered_map<std::string, std::vector<uint32_t>> compiled_relation_subscription_index;
    std::unordered_map<std::string, std::vector<uint32_t>> compiled_object_subscription_index;
    bool runtime_compiled{false};

    void bind_outgoing_edges(NodeId node_id, const std::vector<EdgeId>& edge_ids);
    void sort_edges_by_priority();
    void compile_runtime();
    void invalidate_runtime() noexcept;

    [[nodiscard]] const NodeDefinition* find_node(NodeId node_id) const;
    [[nodiscard]] const EdgeDefinition* find_edge(EdgeId edge_id) const;
    [[nodiscard]] ArrayView<EdgeId> outgoing_edges_view(const NodeDefinition& node) const;
    [[nodiscard]] std::vector<EdgeId> outgoing_edges(const NodeDefinition& node) const;
    [[nodiscard]] ArrayView<CompiledEdgeRoute> compiled_routes_view(const NodeDefinition& node) const;
    [[nodiscard]] const CompiledNodeRouting* routing(const NodeDefinition& node) const;
    [[nodiscard]] const CompiledKnowledgeSubscription* knowledge_subscription(uint32_t compiled_index) const;
    [[nodiscard]] ArrayView<uint32_t> candidate_entity_subscriptions(std::string_view entity_label) const;
    [[nodiscard]] ArrayView<uint32_t> candidate_triple_subscriptions(
        std::string_view subject_label,
        std::string_view relation,
        std::string_view object_label
    ) const;
    [[nodiscard]] bool is_runtime_compiled() const noexcept;
    [[nodiscard]] bool validate(std::string* error_message = nullptr) const;
};

} // namespace agentcore

#endif // AGENTCORE_GRAPH_IR_H
