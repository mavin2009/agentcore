#include "agentcore/graph/graph_ir.h"
#include "agentcore/graph/composition/subgraph.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace agentcore {

namespace {

constexpr uint8_t kRouteMaskSuccess = 1U << 0;
constexpr uint8_t kRouteMaskSoftFail = 1U << 1;
constexpr uint8_t kRouteMaskHardFail = 1U << 2;

auto fail_validation(std::string* error_message, const std::string& message) -> bool {
    if (error_message != nullptr) {
        *error_message = message;
    }
    return false;
}

struct JoinBarrierSearchResult {
    bool ok{true};
    std::vector<NodeId> first_join_nodes;
    std::string error_message;
};

JoinBarrierSearchResult find_first_join_nodes(
    const GraphDefinition& graph,
    NodeId node_id,
    std::unordered_map<NodeId, JoinBarrierSearchResult>& cache,
    std::unordered_set<NodeId>& active_path
) {
    if (const auto cached = cache.find(node_id); cached != cache.end()) {
        return cached->second;
    }

    const NodeDefinition* node = graph.find_node(node_id);
    if (node == nullptr) {
        return JoinBarrierSearchResult{
            false,
            {},
            "join scope path references missing node " + std::to_string(node_id)
        };
    }

    if (has_node_policy(node->policy_flags, NodePolicyFlag::JoinIncomingBranches)) {
        return JoinBarrierSearchResult{
            true,
            std::vector<NodeId>{node->id},
            {}
        };
    }

    if (!active_path.insert(node_id).second) {
        return JoinBarrierSearchResult{
            false,
            {},
            "join scope path enters a cycle before reaching a join barrier at node " +
                std::to_string(node_id)
        };
    }

    const auto outgoing = graph.outgoing_edges_view(*node);
    if (outgoing.empty() || has_node_policy(node->policy_flags, NodePolicyFlag::StopAfterNode)) {
        active_path.erase(node_id);
        return JoinBarrierSearchResult{
            false,
            {},
            "join scope branch can terminate at node " + std::to_string(node->id) +
                " before reaching a join barrier"
        };
    }

    std::unordered_set<NodeId> join_nodes;
    for (EdgeId edge_id : outgoing) {
        const EdgeDefinition* edge = graph.find_edge(edge_id);
        if (edge == nullptr) {
            active_path.erase(node_id);
            return JoinBarrierSearchResult{
                false,
                {},
                "join scope branch references missing edge " + std::to_string(edge_id)
            };
        }

        const JoinBarrierSearchResult child_result =
            find_first_join_nodes(graph, edge->to, cache, active_path);
        if (!child_result.ok) {
            active_path.erase(node_id);
            return child_result;
        }
        join_nodes.insert(child_result.first_join_nodes.begin(), child_result.first_join_nodes.end());
    }

    active_path.erase(node_id);

    JoinBarrierSearchResult result;
    result.ok = true;
    result.first_join_nodes.assign(join_nodes.begin(), join_nodes.end());
    std::sort(result.first_join_nodes.begin(), result.first_join_nodes.end());
    cache.emplace(node_id, result);
    return result;
}

bool should_use_dense_lookup(uint32_t max_id, std::size_t item_count) {
    if (item_count == 0U) {
        return false;
    }
    return max_id <= static_cast<uint32_t>(item_count * 8U) && max_id <= (1U << 20);
}

template <typename Item, typename IdAccessor>
void build_lookup_tables(
    const std::vector<Item>& items,
    IdAccessor id_accessor,
    std::vector<uint32_t>& dense_lookup,
    std::vector<IdLookupEntry>& sparse_lookup
) {
    dense_lookup.clear();
    sparse_lookup.clear();
    if (items.empty()) {
        return;
    }

    uint32_t max_id = 0U;
    for (const Item& item : items) {
        max_id = std::max(max_id, id_accessor(item));
    }

    if (should_use_dense_lookup(max_id, items.size())) {
        dense_lookup.assign(static_cast<std::size_t>(max_id) + 1U, 0U);
        for (std::size_t index = 0; index < items.size(); ++index) {
            dense_lookup[id_accessor(items[index])] = static_cast<uint32_t>(index + 1U);
        }
        return;
    }

    sparse_lookup.reserve(items.size());
    for (std::size_t index = 0; index < items.size(); ++index) {
        sparse_lookup.push_back(IdLookupEntry{
            id_accessor(items[index]),
            static_cast<uint32_t>(index)
        });
    }

    std::sort(
        sparse_lookup.begin(),
        sparse_lookup.end(),
        [](const IdLookupEntry& left, const IdLookupEntry& right) {
            return left.id < right.id;
        }
    );
}

std::optional<uint32_t> lookup_index(
    uint32_t id,
    const std::vector<uint32_t>& dense_lookup,
    const std::vector<IdLookupEntry>& sparse_lookup
) {
    if (!dense_lookup.empty()) {
        if (id >= dense_lookup.size() || dense_lookup[id] == 0U) {
            return std::nullopt;
        }
        return dense_lookup[id] - 1U;
    }

    if (sparse_lookup.empty()) {
        return std::nullopt;
    }

    const auto iterator = std::lower_bound(
        sparse_lookup.begin(),
        sparse_lookup.end(),
        id,
        [](const IdLookupEntry& entry, uint32_t lookup_id) {
            return entry.id < lookup_id;
        }
    );
    if (iterator == sparse_lookup.end() || iterator->id != id) {
        return std::nullopt;
    }
    return iterator->index;
}

uint8_t route_mask_for_edge_kind(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::Always:
            return static_cast<uint8_t>(kRouteMaskSuccess | kRouteMaskSoftFail | kRouteMaskHardFail);
        case EdgeKind::OnSuccess:
        case EdgeKind::Conditional:
            return kRouteMaskSuccess;
        case EdgeKind::OnSoftFail:
            return kRouteMaskSoftFail;
        case EdgeKind::OnHardFail:
            return kRouteMaskHardFail;
    }

    return 0U;
}

bool is_empty_pattern_field(const std::string& value) {
    return value.empty();
}

} // namespace

void GraphDefinition::bind_outgoing_edges(NodeId node_id, const std::vector<EdgeId>& edge_ids) {
    invalidate_runtime();

    auto node = std::find_if(nodes.begin(), nodes.end(), [node_id](const NodeDefinition& candidate) {
        return candidate.id == node_id;
    });

    if (node == nodes.end()) {
        return;
    }

    node->outgoing_range.offset = static_cast<uint32_t>(edge_index_table.size());
    node->outgoing_range.count = static_cast<uint32_t>(edge_ids.size());
    edge_index_table.insert(edge_index_table.end(), edge_ids.begin(), edge_ids.end());
}

void GraphDefinition::sort_edges_by_priority() {
    invalidate_runtime();

    for (const NodeDefinition& node : nodes) {
        std::stable_sort(
            edge_index_table.begin() + static_cast<std::ptrdiff_t>(node.outgoing_range.offset),
            edge_index_table.begin() + static_cast<std::ptrdiff_t>(node.outgoing_range.offset + node.outgoing_range.count),
            [this](EdgeId left, EdgeId right) {
                const EdgeDefinition* left_edge = find_edge(left);
                const EdgeDefinition* right_edge = find_edge(right);
                if (left_edge == nullptr || right_edge == nullptr) {
                    return left < right;
                }
                if (left_edge->priority != right_edge->priority) {
                    return left_edge->priority > right_edge->priority;
                }
                return left_edge->id < right_edge->id;
            }
        );
    }
}

void GraphDefinition::compile_runtime() {
    if (runtime_compiled) {
        return;
    }

    build_lookup_tables(
        nodes,
        [](const NodeDefinition& node) { return node.id; },
        dense_node_lookup,
        sparse_node_lookup
    );
    build_lookup_tables(
        edges,
        [](const EdgeDefinition& edge) { return edge.id; },
        dense_edge_lookup,
        sparse_edge_lookup
    );

    compiled_routes.clear();
    compiled_node_routing.clear();
    compiled_knowledge_subscriptions.clear();
    compiled_intelligence_subscriptions.clear();
    compiled_entity_subscription_index.clear();
    compiled_subject_subscription_index.clear();
    compiled_relation_subscription_index.clear();
    compiled_object_subscription_index.clear();
    for (auto& bucket : compiled_intelligence_subscription_index) {
        bucket.clear();
    }
    compiled_node_routing.resize(nodes.size());

    for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
        const NodeDefinition& node = nodes[node_index];
        CompiledNodeRouting routing;
        routing.route_offset = static_cast<uint32_t>(compiled_routes.size());
        for (EdgeId edge_id : outgoing_edges_view(node)) {
            const auto edge_index = lookup_index(edge_id, dense_edge_lookup, sparse_edge_lookup);
            if (!edge_index.has_value()) {
                continue;
            }

            const EdgeDefinition& edge = edges[*edge_index];
            compiled_routes.push_back(CompiledEdgeRoute{
                *edge_index,
                route_mask_for_edge_kind(edge.kind),
                edge.kind == EdgeKind::Conditional
            });
        }
        routing.route_count = static_cast<uint32_t>(compiled_routes.size()) - routing.route_offset;
        compiled_node_routing[node_index] = routing;

        for (std::size_t subscription_index = 0; subscription_index < node.knowledge_subscriptions.size(); ++subscription_index) {
            const KnowledgeSubscription& subscription = node.knowledge_subscriptions[subscription_index];
            const uint32_t compiled_index = static_cast<uint32_t>(compiled_knowledge_subscriptions.size());
            compiled_knowledge_subscriptions.push_back(CompiledKnowledgeSubscription{
                static_cast<uint32_t>(node_index),
                static_cast<uint32_t>(subscription_index),
                subscription.kind
            });

            if (subscription.kind == KnowledgeSubscriptionKind::EntityLabel) {
                compiled_entity_subscription_index[subscription.entity_label].push_back(compiled_index);
            } else if (!subscription.relation.empty()) {
                compiled_relation_subscription_index[subscription.relation].push_back(compiled_index);
            } else if (!subscription.subject_label.empty()) {
                compiled_subject_subscription_index[subscription.subject_label].push_back(compiled_index);
            } else {
                compiled_object_subscription_index[subscription.object_label].push_back(compiled_index);
            }
        }

        for (std::size_t subscription_index = 0;
             subscription_index < node.intelligence_subscriptions.size();
             ++subscription_index) {
            const IntelligenceSubscription& subscription = node.intelligence_subscriptions[subscription_index];
            const uint32_t compiled_index =
                static_cast<uint32_t>(compiled_intelligence_subscriptions.size());
            compiled_intelligence_subscriptions.push_back(CompiledIntelligenceSubscription{
                static_cast<uint32_t>(node_index),
                static_cast<uint32_t>(subscription_index),
                subscription.kind
            });
            compiled_intelligence_subscription_index[static_cast<std::size_t>(subscription.kind)]
                .push_back(compiled_index);
        }
    }

    runtime_compiled = true;
}

void GraphDefinition::invalidate_runtime() noexcept {
    runtime_compiled = false;
    dense_node_lookup.clear();
    dense_edge_lookup.clear();
    sparse_node_lookup.clear();
    sparse_edge_lookup.clear();
    compiled_routes.clear();
    compiled_node_routing.clear();
    compiled_knowledge_subscriptions.clear();
    compiled_intelligence_subscriptions.clear();
    compiled_entity_subscription_index.clear();
    compiled_subject_subscription_index.clear();
    compiled_relation_subscription_index.clear();
    compiled_object_subscription_index.clear();
    for (auto& bucket : compiled_intelligence_subscription_index) {
        bucket.clear();
    }
}

const NodeDefinition* GraphDefinition::find_node(NodeId node_id) const {
    if (runtime_compiled) {
        const auto node_index = lookup_index(node_id, dense_node_lookup, sparse_node_lookup);
        if (!node_index.has_value()) {
            return nullptr;
        }
        return &nodes[*node_index];
    }

    auto iterator = std::find_if(nodes.begin(), nodes.end(), [node_id](const NodeDefinition& candidate) {
        return candidate.id == node_id;
    });

    if (iterator == nodes.end()) {
        return nullptr;
    }

    return &(*iterator);
}

const EdgeDefinition* GraphDefinition::find_edge(EdgeId edge_id) const {
    if (runtime_compiled) {
        const auto edge_index = lookup_index(edge_id, dense_edge_lookup, sparse_edge_lookup);
        if (!edge_index.has_value()) {
            return nullptr;
        }
        return &edges[*edge_index];
    }

    auto iterator = std::find_if(edges.begin(), edges.end(), [edge_id](const EdgeDefinition& candidate) {
        return candidate.id == edge_id;
    });

    if (iterator == edges.end()) {
        return nullptr;
    }

    return &(*iterator);
}

ArrayView<EdgeId> GraphDefinition::outgoing_edges_view(const NodeDefinition& node) const {
    const std::size_t offset = node.outgoing_range.offset;
    const std::size_t count = node.outgoing_range.count;

    if (offset + count > edge_index_table.size()) {
        return {};
    }

    return ArrayView<EdgeId>(edge_index_table.data() + offset, count);
}

std::vector<EdgeId> GraphDefinition::outgoing_edges(const NodeDefinition& node) const {
    const auto edge_view = outgoing_edges_view(node);
    return std::vector<EdgeId>(edge_view.begin(), edge_view.end());
}

ArrayView<CompiledEdgeRoute> GraphDefinition::compiled_routes_view(const NodeDefinition& node) const {
    const CompiledNodeRouting* compiled_node = routing(node);
    if (compiled_node == nullptr) {
        return {};
    }

    const std::size_t offset = compiled_node->route_offset;
    const std::size_t count = compiled_node->route_count;
    if (offset + count > compiled_routes.size()) {
        return {};
    }

    return ArrayView<CompiledEdgeRoute>(compiled_routes.data() + offset, count);
}

const CompiledNodeRouting* GraphDefinition::routing(const NodeDefinition& node) const {
    if (!runtime_compiled) {
        return nullptr;
    }

    const auto node_index = lookup_index(node.id, dense_node_lookup, sparse_node_lookup);
    if (!node_index.has_value() || *node_index >= compiled_node_routing.size()) {
        return nullptr;
    }

    return &compiled_node_routing[*node_index];
}

const CompiledKnowledgeSubscription* GraphDefinition::knowledge_subscription(uint32_t compiled_index) const {
    if (!runtime_compiled || compiled_index >= compiled_knowledge_subscriptions.size()) {
        return nullptr;
    }
    return &compiled_knowledge_subscriptions[compiled_index];
}

const CompiledIntelligenceSubscription* GraphDefinition::intelligence_subscription(
    uint32_t compiled_index
) const {
    if (!runtime_compiled || compiled_index >= compiled_intelligence_subscriptions.size()) {
        return nullptr;
    }
    return &compiled_intelligence_subscriptions[compiled_index];
}

ArrayView<uint32_t> GraphDefinition::candidate_entity_subscriptions(std::string_view entity_label) const {
    if (!runtime_compiled) {
        return {};
    }
    const auto iterator = compiled_entity_subscription_index.find(std::string(entity_label));
    if (iterator == compiled_entity_subscription_index.end()) {
        return {};
    }
    return ArrayView<uint32_t>(iterator->second.data(), iterator->second.size());
}

ArrayView<uint32_t> GraphDefinition::candidate_triple_subscriptions(
    std::string_view subject_label,
    std::string_view relation,
    std::string_view object_label
) const {
    if (!runtime_compiled) {
        return {};
    }

    if (!relation.empty()) {
        const auto iterator = compiled_relation_subscription_index.find(std::string(relation));
        if (iterator != compiled_relation_subscription_index.end()) {
            return ArrayView<uint32_t>(iterator->second.data(), iterator->second.size());
        }
    }
    if (!subject_label.empty()) {
        const auto iterator = compiled_subject_subscription_index.find(std::string(subject_label));
        if (iterator != compiled_subject_subscription_index.end()) {
            return ArrayView<uint32_t>(iterator->second.data(), iterator->second.size());
        }
    }
    if (!object_label.empty()) {
        const auto iterator = compiled_object_subscription_index.find(std::string(object_label));
        if (iterator != compiled_object_subscription_index.end()) {
            return ArrayView<uint32_t>(iterator->second.data(), iterator->second.size());
        }
    }

    return {};
}

ArrayView<uint32_t> GraphDefinition::candidate_intelligence_subscriptions(
    IntelligenceSubscriptionKind kind
) const {
    if (!runtime_compiled) {
        return {};
    }
    const std::vector<uint32_t>& bucket =
        compiled_intelligence_subscription_index[static_cast<std::size_t>(kind)];
    return ArrayView<uint32_t>(bucket.data(), bucket.size());
}

bool GraphDefinition::is_runtime_compiled() const noexcept {
    return runtime_compiled;
}

bool GraphDefinition::validate(std::string* error_message) const {
    if (nodes.empty()) {
        return fail_validation(error_message, "graph has no nodes");
    }

    if (find_node(entry) == nullptr) {
        return fail_validation(error_message, "entry node does not exist");
    }

    std::unordered_set<StateKey> state_reducer_keys;
    for (const FieldMergeRule& rule : state_reducer_rules) {
        if (!state_reducer_keys.insert(rule.key).second) {
            return fail_validation(
                error_message,
                "duplicate graph state reducer for state key " + std::to_string(rule.key)
            );
        }
    }

    std::unordered_set<NodeId> node_ids;
    for (const NodeDefinition& node : nodes) {
        if (!node_ids.insert(node.id).second) {
            return fail_validation(error_message, "duplicate node id: " + std::to_string(node.id));
        }
        if (node.kind == NodeKind::Subgraph) {
            std::string subgraph_error;
            if (!validate_subgraph_binding(node, &subgraph_error)) {
                return fail_validation(error_message, subgraph_error);
            }
        } else if (node.subgraph.has_value()) {
            return fail_validation(error_message, "subgraph binding can only be declared on subgraph nodes");
        }
        if (node.executor == nullptr && node.kind != NodeKind::Subgraph) {
            return fail_validation(error_message, "node executor is null for node: " + node.name);
        }
        if (!has_node_policy(node.policy_flags, NodePolicyFlag::JoinIncomingBranches) &&
            !node.field_merge_rules.empty()) {
            return fail_validation(
                error_message,
                "field merge rules can only be declared on join barrier nodes: " + node.name
            );
        }
        if (has_node_policy(node.policy_flags, NodePolicyFlag::CreateJoinScope) &&
            !has_node_policy(node.policy_flags, NodePolicyFlag::AllowFanOut)) {
            return fail_validation(error_message, "join scope creation requires fanout policy");
        }
        if (has_node_policy(node.policy_flags, NodePolicyFlag::ReactToKnowledgeGraph) &&
            node.knowledge_subscriptions.empty()) {
            return fail_validation(
                error_message,
                "knowledge-reactive node is missing subscriptions: " + node.name
            );
        }
        if (!has_node_policy(node.policy_flags, NodePolicyFlag::ReactToKnowledgeGraph) &&
            !node.knowledge_subscriptions.empty()) {
            return fail_validation(
                error_message,
                "knowledge subscriptions require ReactToKnowledgeGraph policy: " + node.name
            );
        }
        if (has_node_policy(node.policy_flags, NodePolicyFlag::ReactToIntelligence) &&
            node.intelligence_subscriptions.empty()) {
            return fail_validation(
                error_message,
                "intelligence-reactive node is missing subscriptions: " + node.name
            );
        }
        if (!has_node_policy(node.policy_flags, NodePolicyFlag::ReactToIntelligence) &&
            !node.intelligence_subscriptions.empty()) {
            return fail_validation(
                error_message,
                "intelligence subscriptions require ReactToIntelligence policy: " + node.name
            );
        }
        if (node.memoization.enabled()) {
            if (node.kind != NodeKind::Compute &&
                node.kind != NodeKind::Control &&
                node.kind != NodeKind::Aggregate) {
                return fail_validation(
                    error_message,
                    "deterministic memoization is currently supported only for compute, control, and aggregate nodes: " +
                        node.name
                );
            }
            if (has_node_policy(node.policy_flags, NodePolicyFlag::ReactToKnowledgeGraph) ||
                has_node_policy(node.policy_flags, NodePolicyFlag::ReactToIntelligence)) {
                return fail_validation(
                    error_message,
                    "reactive nodes cannot enable deterministic memoization yet: " + node.name
                );
            }
            std::unordered_set<StateKey> memoization_keys;
            for (StateKey key : node.memoization.read_keys) {
                if (!memoization_keys.insert(key).second) {
                    return fail_validation(
                        error_message,
                        "duplicate memoization read dependency for state key " +
                            std::to_string(key) + " on node " + node.name
                    );
                }
            }
        } else if (!node.memoization.read_keys.empty()) {
            return fail_validation(
                error_message,
                "memoization read dependencies require deterministic memoization on node: " + node.name
            );
        }
        std::unordered_set<StateKey> merge_rule_keys;
        for (const FieldMergeRule& rule : node.field_merge_rules) {
            if (!merge_rule_keys.insert(rule.key).second) {
                return fail_validation(
                    error_message,
                    "duplicate merge rule for state key " + std::to_string(rule.key) +
                        " on node " + node.name
                );
            }
        }
        for (const KnowledgeSubscription& subscription : node.knowledge_subscriptions) {
            if (subscription.kind == KnowledgeSubscriptionKind::EntityLabel) {
                if (subscription.entity_label.empty()) {
                    return fail_validation(
                        error_message,
                        "entity knowledge subscription requires an entity label on node " + node.name
                    );
                }
                if (!is_empty_pattern_field(subscription.subject_label) ||
                    !is_empty_pattern_field(subscription.relation) ||
                    !is_empty_pattern_field(subscription.object_label)) {
                    return fail_validation(
                        error_message,
                        "entity knowledge subscription cannot declare triple pattern fields on node " + node.name
                    );
                }
            } else {
                if (subscription.subject_label.empty() &&
                    subscription.relation.empty() &&
                    subscription.object_label.empty()) {
                    return fail_validation(
                        error_message,
                        "triple knowledge subscription must constrain subject, relation, or object on node " + node.name
                    );
                }
                if (!subscription.entity_label.empty()) {
                    return fail_validation(
                        error_message,
                        "triple knowledge subscription cannot declare entity label on node " + node.name
                    );
                }
            }
        }
        for (const IntelligenceSubscription& subscription : node.intelligence_subscriptions) {
            const bool has_filter =
                !subscription.key.empty() ||
                !subscription.key_prefix.empty() ||
                !subscription.task_key.empty() ||
                !subscription.claim_key.empty() ||
                !subscription.subject_label.empty() ||
                !subscription.relation.empty() ||
                !subscription.object_label.empty() ||
                !subscription.owner.empty() ||
                !subscription.source.empty() ||
                !subscription.scope.empty() ||
                subscription.min_confidence > 0.0F ||
                subscription.min_importance > 0.0F ||
                subscription.task_status.has_value() ||
                subscription.claim_status.has_value() ||
                subscription.decision_status.has_value() ||
                subscription.memory_layer.has_value();
            if (subscription.kind == IntelligenceSubscriptionKind::All && !has_filter) {
                return fail_validation(
                    error_message,
                    "intelligence subscription must constrain kind or fields on node " + node.name
                );
            }
            if ((subscription.task_status.has_value() ||
                 !subscription.owner.empty()) &&
                subscription.kind != IntelligenceSubscriptionKind::Tasks) {
                return fail_validation(
                    error_message,
                    "task intelligence filters require kind=tasks on node " + node.name
                );
            }
            if (subscription.claim_status.has_value() &&
                subscription.kind != IntelligenceSubscriptionKind::Claims) {
                return fail_validation(
                    error_message,
                    "claim intelligence filters require kind=claims on node " + node.name
                );
            }
            if ((!subscription.subject_label.empty() ||
                 !subscription.relation.empty() ||
                 !subscription.object_label.empty()) &&
                subscription.kind != IntelligenceSubscriptionKind::All &&
                subscription.kind != IntelligenceSubscriptionKind::Claims) {
                return fail_validation(
                    error_message,
                    "claim graph intelligence filters require kind=claims or kind=all on node " + node.name
                );
            }
            if (!subscription.source.empty() &&
                subscription.kind != IntelligenceSubscriptionKind::Evidence) {
                return fail_validation(
                    error_message,
                    "evidence intelligence filters require kind=evidence on node " + node.name
                );
            }
            if (subscription.decision_status.has_value() &&
                subscription.kind != IntelligenceSubscriptionKind::Decisions) {
                return fail_validation(
                    error_message,
                    "decision intelligence filters require kind=decisions on node " + node.name
                );
            }
            if ((subscription.memory_layer.has_value() ||
                 !subscription.scope.empty() ||
                 subscription.min_importance > 0.0F) &&
                subscription.kind != IntelligenceSubscriptionKind::Memories) {
                return fail_validation(
                    error_message,
                    "memory intelligence filters require kind=memories on node " + node.name
                );
            }
        }
        if (node.outgoing_range.offset + node.outgoing_range.count > edge_index_table.size()) {
            return fail_validation(error_message, "outgoing edge range points outside the edge index table");
        }
    }

    std::unordered_set<EdgeId> edge_ids;
    std::unordered_map<NodeId, std::size_t> incoming_edge_counts;
    for (const EdgeDefinition& edge : edges) {
        if (!edge_ids.insert(edge.id).second) {
            return fail_validation(error_message, "duplicate edge id: " + std::to_string(edge.id));
        }
        if (find_node(edge.from) == nullptr || find_node(edge.to) == nullptr) {
            return fail_validation(error_message, "edge references a missing node");
        }
        if (edge.kind == EdgeKind::Conditional && edge.condition == nullptr) {
            return fail_validation(error_message, "conditional edge is missing its condition function");
        }
        incoming_edge_counts[edge.to] += 1U;
    }

    for (const NodeDefinition& node : nodes) {
        for (EdgeId edge_id : outgoing_edges_view(node)) {
            const EdgeDefinition* edge = find_edge(edge_id);
            if (edge == nullptr) {
                return fail_validation(error_message, "node outgoing edges reference a missing edge");
            }
            if (edge->from != node.id) {
                return fail_validation(error_message, "edge source does not match the node that owns it");
            }
        }
    }

    std::unordered_set<NodeId> validated_join_targets;
    for (const NodeDefinition& node : nodes) {
        if (has_node_policy(node.policy_flags, NodePolicyFlag::JoinIncomingBranches) &&
            incoming_edge_counts[node.id] < 2U) {
            return fail_validation(
                error_message,
                "join barrier node must have at least two incoming edges: " + node.name
            );
        }

        if (!has_node_policy(node.policy_flags, NodePolicyFlag::CreateJoinScope)) {
            continue;
        }

        const auto outgoing = outgoing_edges_view(node);
        if (outgoing.size() < 2U) {
            return fail_validation(
                error_message,
                "join scope creation requires at least two outgoing edges: " + node.name
            );
        }

        std::optional<NodeId> common_join_target;
        std::unordered_map<NodeId, JoinBarrierSearchResult> cache;
        for (EdgeId edge_id : outgoing) {
            const EdgeDefinition* edge = find_edge(edge_id);
            if (edge == nullptr) {
                return fail_validation(
                    error_message,
                    "join scope references missing edge " + std::to_string(edge_id)
                );
            }

            std::unordered_set<NodeId> active_path;
            const JoinBarrierSearchResult search_result =
                find_first_join_nodes(*this, edge->to, cache, active_path);
            if (!search_result.ok) {
                return fail_validation(
                    error_message,
                    "join scope rooted at node " + node.name + " is invalid: " +
                        search_result.error_message
                );
            }
            if (search_result.first_join_nodes.size() != 1U) {
                return fail_validation(
                    error_message,
                    "join scope rooted at node " + node.name +
                        " can reach multiple join barriers before convergence"
                );
            }

            const NodeId branch_join_target = search_result.first_join_nodes.front();
            if (!common_join_target.has_value()) {
                common_join_target = branch_join_target;
            } else if (*common_join_target != branch_join_target) {
                return fail_validation(
                    error_message,
                    "join scope rooted at node " + node.name +
                        " does not converge on a single join barrier"
                );
            }
        }

        if (!common_join_target.has_value()) {
            return fail_validation(
                error_message,
                "join scope rooted at node " + node.name + " does not resolve a join barrier"
            );
        }
        validated_join_targets.insert(*common_join_target);
    }

    for (const NodeDefinition& node : nodes) {
        if (has_node_policy(node.policy_flags, NodePolicyFlag::JoinIncomingBranches) &&
            validated_join_targets.find(node.id) == validated_join_targets.end()) {
            return fail_validation(
                error_message,
                "join barrier node is not the validated convergence target of a join scope: " +
                    node.name
            );
        }
    }

    return true;
}

} // namespace agentcore
