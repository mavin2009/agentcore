#include "agentcore/execution/engine.h"

#include "agentcore/execution/subgraph/subgraph_runtime.h"
#include "agentcore/graph/composition/subgraph.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentcore {

namespace {

constexpr uint32_t kTraceFlagJoinBarrier = 1U << 30;
constexpr uint32_t kTraceFlagJoinMerged = 1U << 29;
constexpr uint32_t kTraceFlagKnowledgeReactive = 1U << 28;
constexpr uint32_t kTraceFlagManualInterrupt = 1U << 27;
constexpr uint32_t kTraceFlagManualStateEdit = 1U << 26;
constexpr uint32_t kTraceFlagRecordedEffect = 1U << 25;

struct BranchFieldWrite {
    uint32_t branch_id{0};
    const StateStore* source_state{nullptr};
    Value value;
};

bool contains_branch_id(const std::vector<uint32_t>& branch_ids, uint32_t branch_id) {
    return std::find(branch_ids.begin(), branch_ids.end(), branch_id) != branch_ids.end();
}

AsyncWaitKey async_wait_key_for(const PendingAsyncOperation& pending_async) {
    switch (pending_async.kind) {
        case AsyncOperationKind::Tool:
            return AsyncWaitKey{AsyncWaitKind::Tool, pending_async.handle_id};
        case AsyncOperationKind::Model:
            return AsyncWaitKey{AsyncWaitKind::Model, pending_async.handle_id};
        case AsyncOperationKind::None:
            return {};
    }
    return {};
}

bool pending_async_equal(
    const PendingAsyncOperation& left,
    const PendingAsyncOperation& right
) noexcept {
    return left.kind == right.kind && left.handle_id == right.handle_id;
}

void append_unique_pending_async(
    std::vector<PendingAsyncOperation>& pending_asyncs,
    const PendingAsyncOperation& pending_async
) {
    if (!pending_async.valid()) {
        return;
    }

    const auto existing = std::find_if(
        pending_asyncs.begin(),
        pending_asyncs.end(),
        [&pending_async](const PendingAsyncOperation& candidate) {
            return pending_async_equal(candidate, pending_async);
        }
    );
    if (existing == pending_asyncs.end()) {
        pending_asyncs.push_back(pending_async);
    }
}

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

std::vector<PendingAsyncOperation> collect_pending_async_operations(const BranchSnapshot& branch) {
    std::vector<PendingAsyncOperation> pending_asyncs;
    if (branch.pending_async.has_value()) {
        append_unique_pending_async(pending_asyncs, *branch.pending_async);
    }
    for (const PendingAsyncOperation& pending_async : branch.pending_async_group) {
        append_unique_pending_async(pending_asyncs, pending_async);
    }
    return pending_asyncs;
}

const AsyncToolSnapshot* find_pending_tool_snapshot(const BranchSnapshot& branch, uint64_t handle_id) {
    const auto iterator = std::find_if(
        branch.pending_tool_snapshots.begin(),
        branch.pending_tool_snapshots.end(),
        [handle_id](const AsyncToolSnapshot& snapshot) {
            return snapshot.handle.id == handle_id;
        }
    );
    return iterator == branch.pending_tool_snapshots.end() ? nullptr : std::addressof(*iterator);
}

const AsyncModelSnapshot* find_pending_model_snapshot(const BranchSnapshot& branch, uint64_t handle_id) {
    const auto iterator = std::find_if(
        branch.pending_model_snapshots.begin(),
        branch.pending_model_snapshots.end(),
        [handle_id](const AsyncModelSnapshot& snapshot) {
            return snapshot.handle.id == handle_id;
        }
    );
    return iterator == branch.pending_model_snapshots.end() ? nullptr : std::addressof(*iterator);
}

bool subscription_pattern_matches(std::string_view actual, const std::string& expected) {
    return expected.empty() || actual == expected;
}

bool subscription_matches_entity_write(
    const KnowledgeSubscription& subscription,
    std::string_view entity_label
) {
    return subscription.kind == KnowledgeSubscriptionKind::EntityLabel &&
        subscription.entity_label == entity_label;
}

bool subscription_matches_triple_write(
    const KnowledgeSubscription& subscription,
    std::string_view subject_label,
    std::string_view relation,
    std::string_view object_label
) {
    return subscription.kind == KnowledgeSubscriptionKind::TriplePattern &&
        subscription_pattern_matches(subject_label, subscription.subject_label) &&
        subscription_pattern_matches(relation, subscription.relation) &&
        subscription_pattern_matches(object_label, subscription.object_label);
}

std::vector<uint32_t> collect_reactive_node_indices(
    const GraphDefinition& graph,
    const StringInterner& strings,
    const KnowledgeGraphDeltaSummary& delta,
    NodeId source_node_id
) {
    if (!graph.is_runtime_compiled() || delta.empty()) {
        return {};
    }

    std::vector<bool> matched_nodes(graph.nodes.size(), false);
    auto consider_subscription = [&](uint32_t compiled_subscription_index, auto&& matches_write) {
        const CompiledKnowledgeSubscription* compiled = graph.knowledge_subscription(compiled_subscription_index);
        if (compiled == nullptr || compiled->node_index >= graph.nodes.size()) {
            return;
        }
        const NodeDefinition& node = graph.nodes[compiled->node_index];
        if (node.id == source_node_id ||
            compiled->subscription_index >= node.knowledge_subscriptions.size() ||
            matched_nodes[compiled->node_index]) {
            return;
        }
        const KnowledgeSubscription& subscription = node.knowledge_subscriptions[compiled->subscription_index];
        if (matches_write(subscription)) {
            matched_nodes[compiled->node_index] = true;
        }
    };

    for (const KnowledgeEntityDelta& entity : delta.entities) {
        const std::string_view entity_label = strings.resolve(entity.label);
        for (uint32_t compiled_index : graph.candidate_entity_subscriptions(entity_label)) {
            consider_subscription(compiled_index, [&](const KnowledgeSubscription& subscription) {
                return subscription_matches_entity_write(subscription, entity_label);
            });
        }
    }

    for (const KnowledgeTripleDelta& triple : delta.triples) {
        const std::string_view subject_label = strings.resolve(triple.subject_label);
        const std::string_view relation = strings.resolve(triple.relation);
        const std::string_view object_label = strings.resolve(triple.object_label);
        for (uint32_t compiled_index : graph.candidate_triple_subscriptions(subject_label, relation, object_label)) {
            consider_subscription(compiled_index, [&](const KnowledgeSubscription& subscription) {
                return subscription_matches_triple_write(
                    subscription,
                    subject_label,
                    relation,
                    object_label
                );
            });
        }
    }

    std::vector<uint32_t> node_indices;
    for (std::size_t index = 0; index < matched_nodes.size(); ++index) {
        if (matched_nodes[index]) {
            node_indices.push_back(static_cast<uint32_t>(index));
        }
    }
    return node_indices;
}

BlobRef rebase_blob_ref(const StateStore& source_state, StateStore& destination_state, BlobRef ref) {
    if (ref.empty()) {
        return {};
    }

    const std::vector<std::byte> bytes = source_state.blobs().read_bytes(ref);
    if (bytes.size() != ref.size) {
        throw std::runtime_error("failed to rebase blob reference across branch merge");
    }

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

const FieldMergeRule* find_field_merge_rule(const NodeDefinition& node, StateKey key) {
    const auto iterator = std::find_if(
        node.field_merge_rules.begin(),
        node.field_merge_rules.end(),
        [key](const FieldMergeRule& rule) {
            return rule.key == key;
        }
    );
    if (iterator == node.field_merge_rules.end()) {
        return nullptr;
    }
    return &(*iterator);
}

KnowledgeGraphPatch rebase_knowledge_graph_patch(
    const StateStore& source_state,
    StateStore& destination_state,
    const KnowledgeGraphPatch& patch
) {
    KnowledgeGraphPatch rebased;
    rebased.entities.reserve(patch.entities.size());
    for (const KnowledgeEntityWrite& entity : patch.entities) {
        rebased.entities.push_back(KnowledgeEntityWrite{
            rebase_string_id(source_state, destination_state, entity.label),
            rebase_blob_ref(source_state, destination_state, entity.payload)
        });
    }

    rebased.triples.reserve(patch.triples.size());
    for (const KnowledgeTripleWrite& triple : patch.triples) {
        rebased.triples.push_back(KnowledgeTripleWrite{
            rebase_string_id(source_state, destination_state, triple.subject_label),
            rebase_string_id(source_state, destination_state, triple.relation),
            rebase_string_id(source_state, destination_state, triple.object_label),
            rebase_blob_ref(source_state, destination_state, triple.payload)
        });
    }

    return rebased;
}

void replay_branch_knowledge_graph_delta(
    const StateStore& source_state,
    uint64_t split_patch_log_offset,
    StateStore& destination_state
) {
    const auto& entries = source_state.patch_log().entries();
    if (split_patch_log_offset > entries.size()) {
        throw std::runtime_error("invalid split patch offset during knowledge graph merge");
    }

    for (std::size_t index = static_cast<std::size_t>(split_patch_log_offset); index < entries.size(); ++index) {
        const KnowledgeGraphPatch rebased_kg =
            rebase_knowledge_graph_patch(source_state, destination_state, entries[index].patch.knowledge_graph);
        if (rebased_kg.empty()) {
            continue;
        }

        StatePatch patch;
        patch.knowledge_graph = rebased_kg;
        patch.flags = entries[index].patch.flags;
        static_cast<void>(destination_state.apply(patch));
    }
}

template <typename BranchMap>
std::unordered_map<StateKey, std::vector<BranchFieldWrite>> collect_branch_field_writes(
    const std::vector<uint32_t>& branch_ids,
    const BranchMap& branches,
    uint64_t split_patch_log_offset
) {
    std::unordered_map<StateKey, std::vector<BranchFieldWrite>> writes_by_key;

    for (uint32_t branch_id : branch_ids) {
        const auto branch_iterator = branches.find(branch_id);
        if (branch_iterator == branches.end()) {
            throw std::runtime_error("missing branch while collecting join field writes");
        }

        std::unordered_map<StateKey, Value> last_writes_for_branch;
        const auto& entries = branch_iterator->second.state_store.patch_log().entries();
        if (split_patch_log_offset > entries.size()) {
            throw std::runtime_error("invalid split patch offset while collecting join field writes");
        }

        for (std::size_t index = static_cast<std::size_t>(split_patch_log_offset); index < entries.size(); ++index) {
            for (const FieldUpdate& update : entries[index].patch.updates) {
                last_writes_for_branch[update.key] = update.value;
            }
        }

        for (const auto& [key, value] : last_writes_for_branch) {
            writes_by_key[key].push_back(BranchFieldWrite{
                branch_id,
                &branch_iterator->second.state_store,
                value
            });
        }
    }

    for (auto& [_, writes] : writes_by_key) {
        std::sort(writes.begin(), writes.end(), [](const BranchFieldWrite& left, const BranchFieldWrite& right) {
            return left.branch_id < right.branch_id;
        });
    }

    return writes_by_key;
}

std::optional<Value> merge_join_field_values(
    const NodeDefinition& join_node,
    StateKey key,
    const std::vector<BranchFieldWrite>& writes,
    const StateStore& base_state,
    StateStore& destination_state,
    std::string* error_message
) {
    const FieldMergeRule* rule = find_field_merge_rule(join_node, key);
    if (writes.empty()) {
        return std::nullopt;
    }

    const auto fail_merge = [&](std::string message) -> std::optional<Value> {
        if (error_message != nullptr) {
            *error_message = std::move(message);
        }
        return std::nullopt;
    };

    if (rule == nullptr) {
        const BranchFieldWrite& candidate = writes.front();
        const bool all_equal = std::all_of(
            writes.begin() + 1,
            writes.end(),
            [&](const BranchFieldWrite& write) {
                return logical_value_equal(
                    *candidate.source_state,
                    candidate.value,
                    *write.source_state,
                    write.value
                );
            }
        );
        if (!all_equal && writes.size() > 1U) {
            return fail_merge(
                "conflicting writes for state key " + std::to_string(key) +
                " reached join barrier " + join_node.name + " without an explicit merge rule"
            );
        }
        return rebase_value(*candidate.source_state, destination_state, candidate.value);
    }

    switch (rule->strategy) {
        case JoinMergeStrategy::RequireEqual: {
            const BranchFieldWrite& candidate = writes.front();
            const bool all_equal = std::all_of(
                writes.begin() + 1,
                writes.end(),
                [&](const BranchFieldWrite& write) {
                    return logical_value_equal(
                        *candidate.source_state,
                        candidate.value,
                        *write.source_state,
                        write.value
                    );
                }
            );
            if (!all_equal) {
                return fail_merge(
                    "join barrier " + join_node.name +
                    " requires equal values for state key " + std::to_string(key)
                );
            }
            return rebase_value(*candidate.source_state, destination_state, candidate.value);
        }
        case JoinMergeStrategy::RequireSingleWriter:
            if (writes.size() != 1U) {
                return fail_merge(
                    "join barrier " + join_node.name +
                    " requires a single writer for state key " + std::to_string(key)
                );
            }
            return rebase_value(*writes.front().source_state, destination_state, writes.front().value);
        case JoinMergeStrategy::LastWriterWins:
            return rebase_value(*writes.back().source_state, destination_state, writes.back().value);
        case JoinMergeStrategy::FirstWriterWins:
            return rebase_value(*writes.front().source_state, destination_state, writes.front().value);
        case JoinMergeStrategy::SumInt64: {
            int64_t sum = 0;
            if (const Value* base_value = base_state.find(key); base_value != nullptr && !std::holds_alternative<std::monostate>(*base_value)) {
                if (!std::holds_alternative<int64_t>(*base_value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires int64 baseline for SumInt64 on state key " + std::to_string(key)
                    );
                }
                sum += std::get<int64_t>(*base_value);
            }
            for (const BranchFieldWrite& write : writes) {
                if (!std::holds_alternative<int64_t>(write.value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires int64 branch values for SumInt64 on state key " + std::to_string(key)
                    );
                }
                sum += std::get<int64_t>(write.value);
            }
            return Value{sum};
        }
        case JoinMergeStrategy::MaxInt64:
        case JoinMergeStrategy::MinInt64: {
            bool has_value = false;
            int64_t selected = 0;
            if (const Value* base_value = base_state.find(key); base_value != nullptr && !std::holds_alternative<std::monostate>(*base_value)) {
                if (!std::holds_alternative<int64_t>(*base_value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires int64 baseline for ordered int64 merge on state key " + std::to_string(key)
                    );
                }
                selected = std::get<int64_t>(*base_value);
                has_value = true;
            }
            for (const BranchFieldWrite& write : writes) {
                if (!std::holds_alternative<int64_t>(write.value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires int64 branch values for ordered int64 merge on state key " + std::to_string(key)
                    );
                }
                const int64_t branch_value = std::get<int64_t>(write.value);
                if (!has_value) {
                    selected = branch_value;
                    has_value = true;
                } else if (rule->strategy == JoinMergeStrategy::MaxInt64) {
                    selected = std::max(selected, branch_value);
                } else {
                    selected = std::min(selected, branch_value);
                }
            }
            return has_value ? std::optional<Value>{Value{selected}} : std::nullopt;
        }
        case JoinMergeStrategy::LogicalOr:
        case JoinMergeStrategy::LogicalAnd: {
            bool value = (rule->strategy == JoinMergeStrategy::LogicalAnd);
            if (const Value* base_value = base_state.find(key); base_value != nullptr && !std::holds_alternative<std::monostate>(*base_value)) {
                if (!std::holds_alternative<bool>(*base_value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires bool baseline for logical merge on state key " + std::to_string(key)
                    );
                }
                value = std::get<bool>(*base_value);
            }
            for (const BranchFieldWrite& write : writes) {
                if (!std::holds_alternative<bool>(write.value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires bool branch values for logical merge on state key " + std::to_string(key)
                    );
                }
                if (rule->strategy == JoinMergeStrategy::LogicalOr) {
                    value = value || std::get<bool>(write.value);
                } else {
                    value = value && std::get<bool>(write.value);
                }
            }
            return Value{value};
        }
    }

    return fail_merge(
        "join barrier " + join_node.name + " references an unsupported merge strategy"
    );
}

bool branch_snapshot_is_join_blocked(const RunSnapshot& snapshot, const BranchSnapshot& branch) {
    if (branch.join_stack.empty()) {
        return false;
    }

    const uint32_t split_id = branch.join_stack.back();
    const auto scope_iterator = std::find_if(
        snapshot.join_scopes.begin(),
        snapshot.join_scopes.end(),
        [split_id](const JoinScopeSnapshot& scope) {
            return scope.split_id == split_id;
        }
    );
    if (scope_iterator == snapshot.join_scopes.end()) {
        return false;
    }

    if (scope_iterator->join_node_id != 0U &&
        branch.frame.current_node != scope_iterator->join_node_id) {
        return false;
    }

    return contains_branch_id(scope_iterator->arrived_branch_ids, branch.frame.active_branch_id);
}

constexpr uint8_t kSelectMaskSuccess = 1U << 0;
constexpr uint8_t kSelectMaskSoftFail = 1U << 1;
constexpr uint8_t kSelectMaskHardFail = 1U << 2;

class SelectedEdges {
public:
    void push(const EdgeDefinition* edge) {
        if (edge == nullptr) {
            return;
        }

        if (overflow_.empty() && inline_count_ < inline_edges_.size()) {
            inline_edges_[inline_count_++] = edge;
            return;
        }

        if (overflow_.empty()) {
            overflow_.reserve(inline_edges_.size() * 2U);
            overflow_.insert(
                overflow_.end(),
                inline_edges_.begin(),
                inline_edges_.begin() + static_cast<std::ptrdiff_t>(inline_count_)
            );
        }
        overflow_.push_back(edge);
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0U;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return overflow_.empty() ? inline_count_ : overflow_.size();
    }

    [[nodiscard]] const EdgeDefinition* front() const noexcept {
        return (*this)[0];
    }

    [[nodiscard]] const EdgeDefinition* operator[](std::size_t index) const noexcept {
        if (overflow_.empty()) {
            return index < inline_count_ ? inline_edges_[index] : nullptr;
        }
        return index < overflow_.size() ? overflow_[index] : nullptr;
    }

private:
    std::array<const EdgeDefinition*, 8> inline_edges_{};
    std::size_t inline_count_{0};
    std::vector<const EdgeDefinition*> overflow_;
};

uint8_t select_mask_for_result(NodeResult::Status result) {
    switch (result) {
        case NodeResult::Success:
            return kSelectMaskSuccess;
        case NodeResult::SoftFail:
            return kSelectMaskSoftFail;
        case NodeResult::HardFail:
            return kSelectMaskHardFail;
        case NodeResult::Waiting:
        case NodeResult::Cancelled:
            return 0U;
    }

    return 0U;
}

SelectedEdges select_edges(
    const GraphDefinition& graph,
    const NodeDefinition& node,
    const WorkflowState& state,
    const NodeResult& result
) {
    SelectedEdges selected;
    const uint8_t result_mask = select_mask_for_result(result.status);
    if (result_mask == 0U) {
        return selected;
    }

    if (const auto compiled_routes = graph.compiled_routes_view(node); !compiled_routes.empty()) {
        for (const CompiledEdgeRoute& route : compiled_routes) {
            if ((route.result_mask & result_mask) == 0U ||
                route.edge_index >= graph.edges.size()) {
                continue;
            }

            const EdgeDefinition& edge = graph.edges[route.edge_index];
            if (route.requires_condition &&
                (edge.condition == nullptr || !edge.condition(state))) {
                continue;
            }
            selected.push(&edge);
        }
        return selected;
    }

    for (EdgeId edge_id : graph.outgoing_edges_view(node)) {
        const EdgeDefinition* edge = graph.find_edge(edge_id);
        if (edge == nullptr) {
            continue;
        }
        if (edge->kind == EdgeKind::Always) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::OnSuccess && result.status == NodeResult::Success) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::OnSoftFail && result.status == NodeResult::SoftFail) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::OnHardFail && result.status == NodeResult::HardFail) {
            selected.push(edge);
            continue;
        }
        if (edge->kind == EdgeKind::Conditional &&
            result.status == NodeResult::Success &&
            edge->condition != nullptr &&
            edge->condition(state)) {
            selected.push(edge);
        }
    }

    return selected;
}

bool graph_requires_runtime_rebind(const GraphDefinition& graph) {
    return std::any_of(graph.nodes.begin(), graph.nodes.end(), [](const NodeDefinition& node) {
               return node.executor == nullptr && node.kind != NodeKind::Subgraph;
           }) ||
           std::any_of(graph.edges.begin(), graph.edges.end(), [](const EdgeDefinition& edge) {
               return edge.kind == EdgeKind::Conditional && edge.condition == nullptr;
           });
}

std::optional<GraphId> missing_subgraph_graph_id(
    const GraphDefinition& graph,
    const std::unordered_map<GraphId, GraphDefinition>& registry
) {
    for (const NodeDefinition& node : graph.nodes) {
        if (node.kind != NodeKind::Subgraph || !node.subgraph.has_value()) {
            continue;
        }
        if (registry.find(node.subgraph->graph_id) == registry.end()) {
            return node.subgraph->graph_id;
        }
    }
    return std::nullopt;
}

} // namespace

ExecutionEngine::ExecutionEngine(std::size_t worker_count) : scheduler_(worker_count) {
    tool_registry_.set_async_completion_listener([this](AsyncToolHandle handle) {
        scheduler_.signal_async_completion(AsyncWaitKey{AsyncWaitKind::Tool, handle.id});
    });
    model_registry_.set_async_completion_listener([this](AsyncModelHandle handle) {
        scheduler_.signal_async_completion(AsyncWaitKey{AsyncWaitKind::Model, handle.id});
    });
}

NodeResult NodeResult::waiting_on_tool(
    AsyncToolHandle handle,
    StatePatch patch,
    float confidence
) {
    return NodeResult{
        Waiting,
        std::move(patch),
        confidence,
        0U,
        std::nullopt,
        PendingAsyncOperation{AsyncOperationKind::Tool, handle.id}
    };
}

NodeResult NodeResult::waiting_on_model(
    AsyncModelHandle handle,
    StatePatch patch,
    float confidence
) {
    return NodeResult{
        Waiting,
        std::move(patch),
        confidence,
        0U,
        std::nullopt,
        PendingAsyncOperation{AsyncOperationKind::Model, handle.id}
    };
}

ScratchArena::ScratchArena(std::size_t initial_capacity) : storage_(initial_capacity) {}

std::size_t ScratchArena::align_cursor(std::size_t cursor, std::size_t alignment) const noexcept {
    if (alignment == 0U) {
        return cursor;
    }
    const std::size_t remainder = cursor % alignment;
    return remainder == 0U ? cursor : cursor + (alignment - remainder);
}

void* ScratchArena::allocate(std::size_t bytes, std::size_t alignment) {
    std::size_t aligned_cursor = align_cursor(cursor_, alignment);
    if (aligned_cursor + bytes > storage_.size()) {
        storage_.resize(std::max(storage_.size() * 2U, aligned_cursor + bytes));
    }

    void* pointer = storage_.data() + aligned_cursor;
    cursor_ = aligned_cursor + bytes;
    return pointer;
}

void ScratchArena::reset() noexcept {
    cursor_ = 0U;
}

std::size_t ScratchArena::bytes_used() const noexcept {
    return cursor_;
}

uint64_t ExecutionEngine::now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

bool ExecutionEngine::is_terminal_status(ExecutionStatus status) noexcept {
    return status == ExecutionStatus::Completed ||
        status == ExecutionStatus::Cancelled ||
        status == ExecutionStatus::Failed;
}

void ExecutionEngine::rebuild_reactive_frontier_state(RunRuntime& run) {
    for (auto& [_, frontier] : run.reactive_frontiers) {
        frontier.active_branch_count = 0U;
    }

    for (const auto& [_, branch] : run.branches) {
        if (!branch.reactive_root_node_id.has_value() ||
            is_terminal_status(branch.frame.status)) {
            continue;
        }

        ReactiveFrontierState& frontier = run.reactive_frontiers[*branch.reactive_root_node_id];
        frontier.node_id = *branch.reactive_root_node_id;
        frontier.active_branch_count += 1U;
    }

    for (auto iterator = run.reactive_frontiers.begin(); iterator != run.reactive_frontiers.end();) {
        if (iterator->second.active_branch_count == 0U &&
            !iterator->second.pending_rerun &&
            !iterator->second.pending_rerun_seed.has_value()) {
            iterator = run.reactive_frontiers.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

void ExecutionEngine::spawn_reactive_branch(
    RunId run_id,
    RunRuntime& run,
    NodeId reactive_node_id,
    const StateStore& state_seed,
    const ExecutionFrame& frame_seed,
    std::size_t& enqueued_tasks,
    uint32_t& trace_flags
) {
    if (run.graph.find_node(reactive_node_id) == nullptr) {
        return;
    }

    BranchRuntime reactive_branch;
    reactive_branch.frame = frame_seed;
    reactive_branch.frame.active_branch_id = run.next_branch_id;
    reactive_branch.frame.current_node = reactive_node_id;
    reactive_branch.frame.status = ExecutionStatus::Running;
    reactive_branch.state_store = state_seed;
    reactive_branch.pending_async.reset();
    reactive_branch.pending_async_group.clear();
    reactive_branch.retry_count = 0;
    reactive_branch.join_stack.clear();
    reactive_branch.reactive_root_node_id = reactive_node_id;

    const uint32_t reactive_branch_id = run.next_branch_id++;
    run.branches.emplace(reactive_branch_id, std::move(reactive_branch));
    scheduler_.enqueue_task(ScheduledTask{
        run_id,
        reactive_node_id,
        reactive_branch_id,
        now_ns()
    });
    ReactiveFrontierState& frontier = run.reactive_frontiers[reactive_node_id];
    frontier.node_id = reactive_node_id;
    ++enqueued_tasks;
    trace_flags |= kTraceFlagKnowledgeReactive;
}

void ExecutionEngine::mark_reactive_trigger(
    RunId run_id,
    RunRuntime& run,
    NodeId reactive_node_id,
    const StateStore& state_seed,
    const ExecutionFrame& frame_seed,
    std::size_t& enqueued_tasks,
    uint32_t& trace_flags
) {
    ReactiveFrontierState& frontier = run.reactive_frontiers[reactive_node_id];
    frontier.node_id = reactive_node_id;
    if (frontier.active_branch_count == 0U) {
        frontier.pending_rerun = false;
        frontier.pending_rerun_seed.reset();
        spawn_reactive_branch(
            run_id,
            run,
            reactive_node_id,
            state_seed,
            frame_seed,
            enqueued_tasks,
            trace_flags
        );
        return;
    }

    frontier.pending_rerun = true;
    frontier.pending_rerun_seed = ReactiveRerunSeed{
        state_seed,
        frame_seed.step_index
    };
}

void ExecutionEngine::finalize_reactive_frontier_for_branch(
    RunId run_id,
    RunRuntime& run,
    BranchRuntime& branch,
    std::size_t& enqueued_tasks,
    uint32_t& trace_flags
) {
    if (!branch.reactive_root_node_id.has_value() ||
        branch.frame.status != ExecutionStatus::Completed) {
        return;
    }

    const NodeId reactive_root_node_id = *branch.reactive_root_node_id;
    auto frontier_iterator = run.reactive_frontiers.find(reactive_root_node_id);
    if (frontier_iterator == run.reactive_frontiers.end()) {
        return;
    }

    ReactiveFrontierState& frontier = frontier_iterator->second;
    if (frontier.active_branch_count != 0U || !frontier.pending_rerun) {
        return;
    }

    const ReactiveRerunSeed rerun_seed = frontier.pending_rerun_seed.value_or(ReactiveRerunSeed{
        branch.state_store,
        branch.frame.step_index
    });
    frontier.pending_rerun = false;
    frontier.pending_rerun_seed.reset();

    ExecutionFrame rerun_frame = branch.frame;
    rerun_frame.step_index = rerun_seed.step_index;
    spawn_reactive_branch(
        run_id,
        run,
        reactive_root_node_id,
        rerun_seed.state_store,
        rerun_frame,
        enqueued_tasks,
        trace_flags
    );
    rebuild_reactive_frontier_state(run);
}

void ExecutionEngine::register_graph(const GraphDefinition& graph) {
    GraphDefinition compiled_graph = graph;
    if (!compiled_graph.is_runtime_compiled()) {
        compiled_graph.compile_runtime();
    }
    graph_registry_[compiled_graph.id] = std::move(compiled_graph);
}

void ExecutionEngine::set_checkpoint_policy(CheckpointPolicy policy) noexcept {
    checkpoint_policy_ = policy;
}

const CheckpointPolicy& ExecutionEngine::checkpoint_policy() const noexcept {
    return checkpoint_policy_;
}

void ExecutionEngine::set_checkpointer(std::shared_ptr<CheckpointStorageBackend> storage) {
    checkpoint_manager_.set_storage(std::move(storage));
}

void ExecutionEngine::enable_checkpoint_persistence(std::string path) {
    checkpoint_manager_.enable_persistence(std::move(path));
}

void ExecutionEngine::enable_sqlite_checkpoint_persistence(std::string path) {
    checkpoint_manager_.enable_sqlite_persistence(std::move(path));
}

std::size_t ExecutionEngine::load_persisted_checkpoints() {
    return checkpoint_manager_.load_persisted_records();
}

RunId ExecutionEngine::start(const GraphDefinition& graph, const InputEnvelope& input) {
    GraphDefinition runtime_graph = graph;
    runtime_graph.compile_runtime();

    std::string validation_error;
    if (!runtime_graph.validate(&validation_error)) {
        throw std::invalid_argument(validation_error);
    }
    if (input.entry_override.has_value() && runtime_graph.find_node(*input.entry_override) == nullptr) {
        throw std::invalid_argument("entry override does not reference a valid node");
    }

    register_graph(runtime_graph);
    if (const auto missing_graph_id = missing_subgraph_graph_id(runtime_graph, graph_registry_);
        missing_graph_id.has_value()) {
        throw std::invalid_argument(
            "subgraph target graph is not registered: " + std::to_string(*missing_graph_id)
        );
    }

    const RunId run_id = next_run_id_++;
    RunRuntime runtime;
    runtime.graph = runtime_graph;
    runtime.status = ExecutionStatus::Running;
    runtime.runtime_config_payload = input.runtime_config_payload;
    runtime.next_branch_id = 1;
    runtime.next_split_id = 1;

    BranchRuntime root_branch;
    root_branch.frame.graph_id = runtime_graph.id;
    root_branch.frame.current_node = input.entry_override.value_or(runtime_graph.entry);
    root_branch.frame.active_branch_id = 0;
    root_branch.frame.status = ExecutionStatus::Running;
    root_branch.state_store = StateStore(input.initial_field_count);
    if (!input.initial_patch.empty()) {
        static_cast<void>(root_branch.state_store.apply(input.initial_patch));
    }

    runtime.branches.emplace(0U, std::move(root_branch));
    runs_.emplace(run_id, std::move(runtime));
    scheduler_.enqueue_task(ScheduledTask{
        run_id,
        input.entry_override.value_or(runtime_graph.entry),
        0U,
        now_ns()
    });
    return run_id;
}

StepResult ExecutionEngine::step(RunId run_id) {
    StepResult result;
    result.run_id = run_id;

    auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        result.message = "run not found";
        result.status = ExecutionStatus::Failed;
        return result;
    }

    RunRuntime& run = run_iterator->second;
    static_cast<void>(scheduler_.promote_ready_async_tasks(now_ns()));
    const auto task = scheduler_.dequeue_ready_for_run(run_id, now_ns());
    if (!task.has_value()) {
        update_run_status(run, run_id);
        result.status = run.status;
        result.waiting = (run.status == ExecutionStatus::Paused) || scheduler_.has_async_waiters_for_run(run_id);
        result.message = scheduler_.has_async_waiters_for_run(run_id)
            ? "waiting on async completion"
            : "no ready task";
        return result;
    }

    return commit_task_execution(run_id, run, execute_task(run, *task));
}

RunResult ExecutionEngine::run_to_completion(RunId run_id) {
    RunResult result;
    result.run_id = run_id;

    auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        result.status = ExecutionStatus::Failed;
        return result;
    }

    RunRuntime& run = run_iterator->second;
    while (true) {
        static_cast<void>(scheduler_.promote_ready_async_tasks(now_ns()));
        const std::vector<ScheduledTask> tasks = scheduler_.dequeue_ready_batch_for_run(
            run_id,
            now_ns(),
            scheduler_.parallelism()
        );

        if (tasks.empty()) {
            update_run_status(run, run_id);
            if (scheduler_.has_async_waiters_for_run(run_id)) {
                scheduler_.wait_for_async_activity(std::chrono::milliseconds(50));
                continue;
            }
            result.status = run.status;
            break;
        }

        if (tasks.size() == 1U) {
            const TaskExecutionRecord record = execute_task(run, tasks.front());
            const StepResult step_result = commit_task_execution(run_id, run, record);
            result.status = step_result.status;
            result.last_checkpoint_id = step_result.checkpoint_id;
            if (step_result.progressed) {
                result.steps_executed += 1U;
            }

            update_run_status(run, run_id);
            if (run.status != ExecutionStatus::Running) {
                result.status = run.status;
                break;
            }
            continue;
        }

        std::vector<TaskExecutionRecord> records(tasks.size());
        std::vector<std::function<void()>> jobs;
        jobs.reserve(tasks.size());
        for (std::size_t index = 0; index < tasks.size(); ++index) {
            jobs.push_back([this, &run, &records, task = tasks[index], index]() {
                records[index] = execute_task(run, task);
            });
        }
        scheduler_.run_batch(jobs);

        for (const TaskExecutionRecord& record : records) {
            const StepResult step_result = commit_task_execution(run_id, run, record);
            result.status = step_result.status;
            result.last_checkpoint_id = step_result.checkpoint_id;
            if (step_result.progressed) {
                result.steps_executed += 1U;
            }
        }

        update_run_status(run, run_id);
        if (run.status != ExecutionStatus::Running) {
            result.status = run.status;
            break;
        }
    }

    return result;
}

ResumeResult ExecutionEngine::resume(CheckpointId checkpoint_id) {
    ResumeResult result;
    result.restored_checkpoint_id = checkpoint_id;

    const auto record = checkpoint_manager_.get(checkpoint_id);
    if (!record.has_value()) {
        result.status = ExecutionStatus::Failed;
        result.message = "checkpoint not found";
        return result;
    }

    const RunId run_id = record->checkpoint.run_id;
    result.run_id = run_id;

    if (!record->resumable()) {
        result.status = ExecutionStatus::Failed;
        const auto fallback = checkpoint_manager_.latest_resumable_for_run(run_id, checkpoint_id);
        if (fallback.has_value()) {
            result.message =
                "checkpoint is metadata-only; resume from checkpoint " +
                std::to_string(fallback->checkpoint_id);
        } else {
            result.message = "checkpoint is metadata-only and no resumable snapshot is available";
        }
        return result;
    }

    std::size_t missing_async_handles = 0U;
    if (!restore_run_from_snapshot(run_id, *record->snapshot, &result.message, &missing_async_handles)) {
        result.status = ExecutionStatus::Failed;
        return result;
    }

    result.status = runs_.at(run_id).status;
    result.resumed = true;
    result.message = missing_async_handles == 0U
        ? "checkpoint restored"
        : "checkpoint restored with unresolved async handles";
    return result;
}

ResumeResult ExecutionEngine::resume_run(RunId run_id) {
    ResumeResult result;
    result.run_id = run_id;

    auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        result.status = ExecutionStatus::Failed;
        result.message = "run not found";
        return result;
    }

    RunRuntime& run = run_iterator->second;
    if (run.status != ExecutionStatus::Paused) {
        result.status = run.status;
        result.message = "run is not paused";
        return result;
    }

    scheduler_.remove_run(run_id);
    re_register_run_async_waiters(run_id, run);
    const std::size_t enqueued_tasks = enqueue_resumable_paused_branches(run_id, run);
    update_run_status(run, run_id);

    result.status = run.status;
    result.resumed = enqueued_tasks != 0U || scheduler_.has_async_waiters_for_run(run_id);
    if (result.resumed) {
        result.message = "paused run resumed";
    } else if (run.status == ExecutionStatus::Paused) {
        result.message = "paused run has no resumable branches";
    } else {
        result.message = "run status refreshed";
    }
    return result;
}

InterruptResult ExecutionEngine::interrupt(RunId run_id) {
    InterruptResult result;
    result.run_id = run_id;

    auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        result.status = ExecutionStatus::Failed;
        result.message = "run not found";
        return result;
    }

    RunRuntime& run = run_iterator->second;
    if (run.branches.empty()) {
        result.status = ExecutionStatus::Failed;
        result.message = "run has no branches";
        return result;
    }

    if (run.status == ExecutionStatus::Completed ||
        run.status == ExecutionStatus::Cancelled ||
        run.status == ExecutionStatus::Failed) {
        result.status = run.status;
        result.message = "run is already terminal";
        return result;
    }

    scheduler_.remove_run(run_id);
    for (auto& [_, branch] : run.branches) {
        if (branch.frame.status == ExecutionStatus::Running ||
            branch.frame.status == ExecutionStatus::NotStarted) {
            branch.frame.status = ExecutionStatus::Paused;
        }
    }
    update_run_status(run, run_id);

    auto checkpoint_branch_iterator = run.branches.find(0U);
    if (checkpoint_branch_iterator == run.branches.end()) {
        checkpoint_branch_iterator = std::min_element(
            run.branches.begin(),
            run.branches.end(),
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            }
        );
    }

    BranchRuntime& checkpoint_branch = checkpoint_branch_iterator->second;
    const CheckpointId checkpoint_id = static_cast<CheckpointId>(checkpoint_manager_.size() + 1U);
    checkpoint_branch.frame.checkpoint_id = checkpoint_id;
    const uint64_t patch_log_offset = static_cast<uint64_t>(checkpoint_branch.state_store.patch_log().size());
    checkpoint_manager_.append(
        Checkpoint{
            run_id,
            checkpoint_id,
            checkpoint_branch.frame.current_node,
            checkpoint_branch.state_store.get_current_state().version,
            patch_log_offset,
            run.status,
            checkpoint_branch.frame.active_branch_id,
            checkpoint_branch.frame.step_index
        },
        snapshot_run(run_id, run)
    );

    const uint64_t now = now_ns();
    trace_sink_.emit(TraceEvent{
        0U,
        now,
        now,
        run_id,
        run.graph.id,
        checkpoint_branch.frame.current_node,
        checkpoint_branch.frame.active_branch_id,
        checkpoint_id,
        NodeResult::Waiting,
        1.0F,
        0U,
        kTraceFlagManualInterrupt,
        {},
        0U,
        {}
    });

    result.checkpoint_id = checkpoint_id;
    result.status = run.status;
    result.interrupted = (run.status == ExecutionStatus::Paused);
    result.message = result.interrupted ? "run interrupted" : "run status updated";
    return result;
}

StateEditResult ExecutionEngine::apply_state_patch(RunId run_id, const StatePatch& patch, uint32_t branch_id) {
    StateEditResult result;
    result.run_id = run_id;
    result.branch_id = branch_id;

    auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        result.message = "run not found";
        return result;
    }

    RunRuntime& run = run_iterator->second;
    if (run.status != ExecutionStatus::Paused) {
        result.message = "run must be paused before applying a state patch";
        return result;
    }
    if (patch.empty()) {
        result.message = "state patch is empty";
        return result;
    }

    auto branch_iterator = run.branches.find(branch_id);
    if (branch_iterator == run.branches.end()) {
        result.message = "branch not found";
        return result;
    }

    BranchRuntime& branch = branch_iterator->second;
    const StateApplyResult apply_result = branch.state_store.apply_with_summary(patch);
    uint32_t trace_flags = kTraceFlagManualStateEdit;

    if (!apply_result.knowledge_graph_delta.empty()) {
        const std::vector<uint32_t> reactive_node_indices = collect_reactive_node_indices(
            run.graph,
            branch.state_store.strings(),
            apply_result.knowledge_graph_delta,
            branch.frame.current_node
        );
        for (uint32_t reactive_node_index : reactive_node_indices) {
            if (reactive_node_index >= run.graph.nodes.size()) {
                continue;
            }

            ReactiveFrontierState& frontier = run.reactive_frontiers[run.graph.nodes[reactive_node_index].id];
            frontier.node_id = run.graph.nodes[reactive_node_index].id;
            frontier.pending_rerun = true;
            frontier.pending_rerun_seed = ReactiveRerunSeed{
                branch.state_store,
                branch.frame.step_index
            };
            trace_flags |= kTraceFlagKnowledgeReactive;
        }
        rebuild_reactive_frontier_state(run);
    }

    const CheckpointId checkpoint_id = static_cast<CheckpointId>(checkpoint_manager_.size() + 1U);
    branch.frame.checkpoint_id = checkpoint_id;
    checkpoint_manager_.append(
        Checkpoint{
            run_id,
            checkpoint_id,
            branch.frame.current_node,
            branch.state_store.get_current_state().version,
            apply_result.patch_log_offset,
            run.status,
            branch.frame.active_branch_id,
            branch.frame.step_index
        },
        snapshot_run(run_id, run)
    );

    const uint64_t now = now_ns();
    trace_sink_.emit(TraceEvent{
        0U,
        now,
        now,
        run_id,
        run.graph.id,
        branch.frame.current_node,
        branch.frame.active_branch_id,
        checkpoint_id,
        NodeResult::Success,
        1.0F,
        static_cast<uint32_t>(patch.updates.size()),
        trace_flags,
        {},
        0U,
        {}
    });

    result.checkpoint_id = checkpoint_id;
    result.state_version = branch.state_store.get_current_state().version;
    result.applied = true;
    result.message = "state patch applied";
    return result;
}

RunSnapshot ExecutionEngine::inspect(RunId run_id) const {
    const auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        throw std::out_of_range("run not found");
    }
    return snapshot_run(run_id, run_iterator->second);
}

const WorkflowState& ExecutionEngine::state(RunId run_id, uint32_t branch_id) const {
    return state_store(run_id, branch_id).get_current_state();
}

const StateStore& ExecutionEngine::state_store(RunId run_id, uint32_t branch_id) const {
    const auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        throw std::out_of_range("run not found");
    }

    const auto branch_iterator = run_iterator->second.branches.find(branch_id);
    if (branch_iterator == run_iterator->second.branches.end()) {
        throw std::out_of_range("branch not found");
    }

    return branch_iterator->second.state_store;
}

const KnowledgeGraphStore& ExecutionEngine::knowledge_graph(RunId run_id, uint32_t branch_id) const {
    return state_store(run_id, branch_id).knowledge_graph();
}

const GraphDefinition& ExecutionEngine::graph(RunId run_id) const {
    const auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        throw std::out_of_range("run not found");
    }

    return run_iterator->second.graph;
}

ToolRegistry& ExecutionEngine::tools() noexcept {
    return tool_registry_;
}

ModelRegistry& ExecutionEngine::models() noexcept {
    return model_registry_;
}

TraceSink& ExecutionEngine::trace() noexcept {
    return trace_sink_;
}

const TraceSink& ExecutionEngine::trace() const noexcept {
    return trace_sink_;
}

const CheckpointManager& ExecutionEngine::checkpoints() const noexcept {
    return checkpoint_manager_;
}

void ExecutionEngine::borrow_runtime_registries(ToolRegistry& tools, ModelRegistry& models) noexcept {
    borrowed_tool_registry_ = &tools;
    borrowed_model_registry_ = &models;
}

ToolRegistry& ExecutionEngine::runtime_tools() noexcept {
    return borrowed_tool_registry_ != nullptr ? *borrowed_tool_registry_ : tool_registry_;
}

const ToolRegistry& ExecutionEngine::runtime_tools() const noexcept {
    return borrowed_tool_registry_ != nullptr ? *borrowed_tool_registry_ : tool_registry_;
}

ModelRegistry& ExecutionEngine::runtime_models() noexcept {
    return borrowed_model_registry_ != nullptr ? *borrowed_model_registry_ : model_registry_;
}

const ModelRegistry& ExecutionEngine::runtime_models() const noexcept {
    return borrowed_model_registry_ != nullptr ? *borrowed_model_registry_ : model_registry_;
}

std::vector<StreamEvent> ExecutionEngine::stream_events(
    RunId run_id,
    StreamCursor& cursor,
    const StreamReadOptions& options
) const {
    return build_public_stream_events(
        trace_sink_,
        run_id,
        cursor,
        options,
        [this](GraphId graph_id) {
            return registered_graph(graph_id);
        }
    );
}

bool ExecutionEngine::branch_is_join_blocked(const RunRuntime& run, const BranchRuntime& branch) const noexcept {
    if (branch.join_stack.empty()) {
        return false;
    }

    const uint32_t split_id = branch.join_stack.back();
    const auto scope_iterator = run.join_scopes.find(split_id);
    if (scope_iterator == run.join_scopes.end()) {
        return false;
    }

    if (scope_iterator->second.join_node_id != 0U &&
        branch.frame.current_node != scope_iterator->second.join_node_id) {
        return false;
    }

    return contains_branch_id(
        scope_iterator->second.arrived_branch_ids,
        branch.frame.active_branch_id
    );
}

void ExecutionEngine::re_register_run_async_waiters(RunId run_id, const RunRuntime& run) {
    for (const auto& [branch_id, branch] : run.branches) {
        std::vector<PendingAsyncOperation> pending_asyncs;
        if (branch.pending_async.has_value()) {
            append_unique_pending_async(pending_asyncs, *branch.pending_async);
        }
        for (const PendingAsyncOperation& pending_async : branch.pending_async_group) {
            append_unique_pending_async(pending_asyncs, pending_async);
        }
        for (const PendingAsyncOperation& pending_async : pending_asyncs) {
            re_register_async_waiter(run_id, branch_id, pending_async);
        }
    }
}

std::size_t ExecutionEngine::enqueue_resumable_paused_branches(
    RunId run_id,
    RunRuntime& run,
    uint32_t* trace_flags
) {
    std::size_t enqueued_tasks = 0U;
    uint32_t local_trace_flags = 0U;

    rebuild_reactive_frontier_state(run);
    std::vector<NodeId> reactive_root_ids;
    reactive_root_ids.reserve(run.reactive_frontiers.size());
    for (const auto& [node_id, _] : run.reactive_frontiers) {
        reactive_root_ids.push_back(node_id);
    }
    std::sort(reactive_root_ids.begin(), reactive_root_ids.end());

    for (NodeId reactive_root_id : reactive_root_ids) {
        auto frontier_iterator = run.reactive_frontiers.find(reactive_root_id);
        if (frontier_iterator == run.reactive_frontiers.end()) {
            continue;
        }

        ReactiveFrontierState& frontier = frontier_iterator->second;
        if (frontier.active_branch_count != 0U ||
            !frontier.pending_rerun ||
            !frontier.pending_rerun_seed.has_value()) {
            continue;
        }

        const ReactiveRerunSeed rerun_seed = *frontier.pending_rerun_seed;
        frontier.pending_rerun = false;
        frontier.pending_rerun_seed.reset();

        ExecutionFrame rerun_frame{};
        rerun_frame.graph_id = run.graph.id;
        rerun_frame.current_node = reactive_root_id;
        rerun_frame.step_index = rerun_seed.step_index;
        rerun_frame.status = ExecutionStatus::Paused;
        spawn_reactive_branch(
            run_id,
            run,
            reactive_root_id,
            rerun_seed.state_store,
            rerun_frame,
            enqueued_tasks,
            local_trace_flags
        );
    }

    rebuild_reactive_frontier_state(run);
    std::vector<uint32_t> branch_ids;
    branch_ids.reserve(run.branches.size());
    for (const auto& [branch_id, _] : run.branches) {
        branch_ids.push_back(branch_id);
    }
    std::sort(branch_ids.begin(), branch_ids.end());

    for (uint32_t branch_id : branch_ids) {
        auto branch_iterator = run.branches.find(branch_id);
        if (branch_iterator == run.branches.end()) {
            continue;
        }

        BranchRuntime& branch = branch_iterator->second;
        if (branch.frame.status != ExecutionStatus::Paused ||
            branch.pending_async.has_value() ||
            !branch.pending_async_group.empty() ||
            branch_is_join_blocked(run, branch)) {
            continue;
        }

        scheduler_.enqueue_task(ScheduledTask{
            run_id,
            branch.frame.current_node,
            branch_id,
            now_ns()
        });
        branch.frame.status = ExecutionStatus::Running;
        enqueued_tasks += 1U;
    }

    if (trace_flags != nullptr) {
        *trace_flags |= local_trace_flags;
    }
    return enqueued_tasks;
}

RunSnapshot ExecutionEngine::snapshot_run(RunId run_id, const RunRuntime& run) const {
    RunSnapshot snapshot;
    snapshot.graph = run.graph;
    snapshot.status = run.status;
    snapshot.runtime_config_payload = run.runtime_config_payload;
    snapshot.pending_tasks = scheduler_.tasks_for_run(run_id);
    snapshot.next_branch_id = run.next_branch_id;
    snapshot.next_split_id = run.next_split_id;

    std::vector<uint32_t> branch_ids;
    branch_ids.reserve(run.branches.size());
    for (const auto& [branch_id, _] : run.branches) {
        branch_ids.push_back(branch_id);
    }
    std::sort(branch_ids.begin(), branch_ids.end());

    for (uint32_t branch_id : branch_ids) {
        const BranchRuntime& branch = run.branches.at(branch_id);
        std::vector<PendingAsyncOperation> pending_asyncs;
        if (branch.pending_async.has_value()) {
            append_unique_pending_async(pending_asyncs, *branch.pending_async);
        }
        for (const PendingAsyncOperation& pending_async : branch.pending_async_group) {
            append_unique_pending_async(pending_asyncs, pending_async);
        }

        std::vector<AsyncToolSnapshot> pending_tool_snapshots;
        std::vector<AsyncModelSnapshot> pending_model_snapshots;
        pending_tool_snapshots.reserve(pending_asyncs.size());
        pending_model_snapshots.reserve(pending_asyncs.size());
        for (const PendingAsyncOperation& pending_async : pending_asyncs) {
            switch (pending_async.kind) {
                case AsyncOperationKind::Tool:
                    if (const auto pending_tool_snapshot =
                            runtime_tools().export_async_operation(AsyncToolHandle{pending_async.handle_id});
                        pending_tool_snapshot.has_value()) {
                        pending_tool_snapshots.push_back(std::move(*pending_tool_snapshot));
                    }
                    break;
                case AsyncOperationKind::Model:
                    if (const auto pending_model_snapshot =
                            runtime_models().export_async_operation(AsyncModelHandle{pending_async.handle_id});
                        pending_model_snapshot.has_value()) {
                        pending_model_snapshots.push_back(std::move(*pending_model_snapshot));
                    }
                    break;
                case AsyncOperationKind::None:
                    break;
            }
        }
        snapshot.branches.push_back(BranchSnapshot{
            branch.frame,
            branch.state_store,
            branch.retry_count,
            branch.pending_async,
            branch.pending_async_group,
            branch.pending_subgraph,
            std::move(pending_tool_snapshots),
            std::move(pending_model_snapshots),
            branch.join_stack,
            branch.reactive_root_node_id
        });
    }

    std::vector<uint32_t> split_ids;
    split_ids.reserve(run.join_scopes.size());
    for (const auto& [split_id, _] : run.join_scopes) {
        split_ids.push_back(split_id);
    }
    std::sort(split_ids.begin(), split_ids.end());

    for (uint32_t split_id : split_ids) {
        const JoinScope& join_scope = run.join_scopes.at(split_id);
        snapshot.join_scopes.push_back(JoinScopeSnapshot{
            join_scope.split_id,
            join_scope.expected_branch_count,
            join_scope.join_node_id,
            join_scope.split_patch_log_offset,
            join_scope.base_state,
            join_scope.arrived_branch_ids
        });
    }

    std::vector<NodeId> reactive_root_ids;
    reactive_root_ids.reserve(run.reactive_frontiers.size());
    for (const auto& [node_id, _] : run.reactive_frontiers) {
        reactive_root_ids.push_back(node_id);
    }
    std::sort(reactive_root_ids.begin(), reactive_root_ids.end());

    for (NodeId node_id : reactive_root_ids) {
        const ReactiveFrontierState& frontier = run.reactive_frontiers.at(node_id);
        std::optional<ReactiveRerunSeedSnapshot> pending_rerun_seed;
        if (frontier.pending_rerun_seed.has_value()) {
            pending_rerun_seed = ReactiveRerunSeedSnapshot{
                frontier.pending_rerun_seed->state_store,
                frontier.pending_rerun_seed->step_index
            };
        }
        snapshot.reactive_frontiers.push_back(ReactiveFrontierSnapshot{
            frontier.node_id,
            frontier.pending_rerun,
            std::move(pending_rerun_seed)
        });
    }

    snapshot.committed_subgraph_sessions = flatten_subgraph_session_table(run.committed_subgraph_sessions);

    return snapshot;
}

bool ExecutionEngine::restore_run_from_snapshot(
    RunId run_id,
    const RunSnapshot& snapshot,
    std::string* error_message,
    std::size_t* missing_async_handles,
    bool restore_async_waiters,
    bool enqueue_paused_branches
) {
    GraphDefinition runtime_graph = resolve_runtime_graph(snapshot);
    if (graph_requires_runtime_rebind(runtime_graph)) {
        if (error_message != nullptr) {
            *error_message = "graph definition not registered for checkpoint restore";
        }
        return false;
    }
    if (const auto missing_graph_id = missing_subgraph_graph_id(runtime_graph, graph_registry_);
        missing_graph_id.has_value()) {
        if (error_message != nullptr) {
            *error_message = "subgraph target graph is not registered: " + std::to_string(*missing_graph_id);
        }
        return false;
    }

    RunRuntime runtime;
    runtime.graph = std::move(runtime_graph);
    runtime.status = snapshot.status;
    runtime.runtime_config_payload = snapshot.runtime_config_payload;
    runtime.next_branch_id = snapshot.next_branch_id;
    runtime.next_split_id = snapshot.next_split_id;
    restore_subgraph_session_table(runtime.committed_subgraph_sessions, snapshot.committed_subgraph_sessions);

    for (const BranchSnapshot& branch_snapshot : snapshot.branches) {
        BranchRuntime branch;
        branch.frame = branch_snapshot.frame;
        branch.state_store = branch_snapshot.state_store;
        branch.retry_count = branch_snapshot.retry_count;
        branch.pending_async = branch_snapshot.pending_async;
        branch.pending_async_group = branch_snapshot.pending_async_group;
        branch.pending_subgraph = branch_snapshot.pending_subgraph;
        branch.join_stack = branch_snapshot.join_stack;
        branch.reactive_root_node_id = branch_snapshot.reactive_root_node_id;
        if (branch.pending_subgraph.has_value()) {
            branch.last_subgraph_session_id = branch.pending_subgraph->session_id;
            branch.last_subgraph_session_revision = branch.pending_subgraph->session_revision;
        }
        runtime.branches.emplace(branch.frame.active_branch_id, std::move(branch));
    }

    for (const auto& [branch_id, branch] : runtime.branches) {
        if (!branch.pending_subgraph.has_value() || branch.pending_subgraph->session_id.empty()) {
            continue;
        }
        std::string lease_error;
        if (!acquire_subgraph_session_lease(
                runtime.active_subgraph_session_leases,
                branch.frame.current_node,
                branch.pending_subgraph->session_id,
                branch_id,
                &lease_error
            )) {
            if (error_message != nullptr) {
                *error_message = lease_error.empty()
                    ? "failed to restore persistent subgraph session lease"
                    : lease_error;
            }
            return false;
        }
    }

    for (const JoinScopeSnapshot& join_scope_snapshot : snapshot.join_scopes) {
        runtime.join_scopes.emplace(join_scope_snapshot.split_id, JoinScope{
            join_scope_snapshot.split_id,
            join_scope_snapshot.expected_branch_count,
            join_scope_snapshot.join_node_id,
            join_scope_snapshot.split_patch_log_offset,
            join_scope_snapshot.base_state,
            join_scope_snapshot.arrived_branch_ids
        });
    }

    for (const ReactiveFrontierSnapshot& frontier_snapshot : snapshot.reactive_frontiers) {
        ReactiveFrontierState frontier;
        frontier.node_id = frontier_snapshot.node_id;
        frontier.pending_rerun = frontier_snapshot.pending_rerun;
        if (frontier_snapshot.pending_rerun_seed.has_value()) {
            frontier.pending_rerun_seed = ReactiveRerunSeed{
                frontier_snapshot.pending_rerun_seed->state_store,
                frontier_snapshot.pending_rerun_seed->step_index
            };
        }
        runtime.reactive_frontiers.emplace(frontier.node_id, std::move(frontier));
    }
    rebuild_reactive_frontier_state(runtime);

    runs_[run_id] = std::move(runtime);
    next_run_id_ = std::max(next_run_id_, run_id + 1U);
    scheduler_.remove_run(run_id);
    for (ScheduledTask task : snapshot.pending_tasks) {
        task.run_id = run_id;
        scheduler_.enqueue_task(task);
    }

    std::size_t local_missing_async_handles = 0U;
    if (restore_async_waiters) {
        for (const BranchSnapshot& branch_snapshot : snapshot.branches) {
            for (const PendingAsyncOperation& pending_async : collect_pending_async_operations(branch_snapshot)) {
                bool restored_async = false;
                switch (pending_async.kind) {
                    case AsyncOperationKind::Tool:
                        if (runtime_tools().has_async_handle(AsyncToolHandle{pending_async.handle_id})) {
                            restored_async = true;
                        } else if (const AsyncToolSnapshot* pending_tool_snapshot =
                                       find_pending_tool_snapshot(branch_snapshot, pending_async.handle_id);
                                   pending_tool_snapshot != nullptr) {
                            const AsyncToolHandle restored_handle =
                                runtime_tools().restore_async_operation(*pending_tool_snapshot);
                            restored_async = (restored_handle.id == pending_async.handle_id);
                        }
                        break;
                    case AsyncOperationKind::Model:
                        if (runtime_models().has_async_handle(AsyncModelHandle{pending_async.handle_id})) {
                            restored_async = true;
                        } else if (const AsyncModelSnapshot* pending_model_snapshot =
                                       find_pending_model_snapshot(branch_snapshot, pending_async.handle_id);
                                   pending_model_snapshot != nullptr) {
                            const AsyncModelHandle restored_handle =
                                runtime_models().restore_async_operation(*pending_model_snapshot);
                            restored_async = (restored_handle.id == pending_async.handle_id);
                        }
                        break;
                    case AsyncOperationKind::None:
                        break;
                }

                if (restored_async) {
                    re_register_async_waiter(run_id, branch_snapshot.frame.active_branch_id, pending_async);
                } else {
                    local_missing_async_handles += 1U;
                }
            }
        }
    }

    if (enqueue_paused_branches && snapshot.pending_tasks.empty() && snapshot.status == ExecutionStatus::Paused) {
        for (const BranchSnapshot& branch_snapshot : snapshot.branches) {
            if (branch_snapshot.frame.status == ExecutionStatus::Paused &&
                !branch_snapshot.pending_async.has_value() &&
                branch_snapshot.pending_async_group.empty() &&
                !branch_snapshot_is_join_blocked(snapshot, branch_snapshot)) {
                scheduler_.enqueue_task(ScheduledTask{
                    run_id,
                    branch_snapshot.frame.current_node,
                    branch_snapshot.frame.active_branch_id,
                    now_ns()
                });
            }
        }
    }

    update_run_status(runs_.at(run_id), run_id);
    if (missing_async_handles != nullptr) {
        *missing_async_handles = local_missing_async_handles;
    }
    return true;
}

bool ExecutionEngine::should_capture_checkpoint_snapshot(
    const RunRuntime& run,
    const BranchRuntime& checkpoint_branch,
    const TaskExecutionRecord& record,
    uint32_t trace_flags,
    std::size_t enqueued_tasks
) const noexcept {
    if (checkpoint_policy_.snapshot_on_wait &&
        record.node_result.status == NodeResult::Waiting) {
        return true;
    }

    if (checkpoint_policy_.snapshot_on_terminal &&
        (run.status == ExecutionStatus::Completed ||
         run.status == ExecutionStatus::Cancelled)) {
        return true;
    }

    if (checkpoint_policy_.snapshot_on_failure &&
        (run.status == ExecutionStatus::Failed ||
         record.node_result.status == NodeResult::HardFail)) {
        return true;
    }

    if (checkpoint_policy_.snapshot_on_join_events &&
        (trace_flags & (kTraceFlagJoinBarrier | kTraceFlagJoinMerged)) != 0U) {
        return true;
    }

    if (enqueued_tasks > 1U) {
        return true;
    }

    if (checkpoint_policy_.snapshot_interval_steps != 0U &&
        checkpoint_branch.frame.step_index != 0U &&
        (checkpoint_branch.frame.step_index % checkpoint_policy_.snapshot_interval_steps) == 0U) {
        return true;
    }

    return false;
}

ExecutionEngine::TaskExecutionRecord ExecutionEngine::execute_task(RunRuntime& run, const ScheduledTask& task) {
    TaskExecutionRecord record;
    record.task = task;

    auto branch_iterator = run.branches.find(task.branch_id);
    if (branch_iterator == run.branches.end()) {
        record.node_result.status = NodeResult::HardFail;
        record.node_result.flags = kToolFlagValidationError;
        record.error_message = "branch not found";
        return record;
    }

    BranchRuntime& branch = branch_iterator->second;
    const NodeDefinition* node = run.graph.find_node(task.node_id);
    if (node == nullptr || (node->executor == nullptr && node->kind != NodeKind::Subgraph)) {
        branch.frame.status = ExecutionStatus::Failed;
        record.node_result.status = NodeResult::HardFail;
        record.node_result.flags = kToolFlagValidationError;
        record.error_message = "node executor not found";
        return record;
    }

    branch.frame.current_node = task.node_id;
    branch.frame.status = ExecutionStatus::Running;
    branch.scratch.reset();
    if (branch.pending_async.has_value() || !branch.pending_async_group.empty()) {
        scheduler_.remove_async_waiters_for_task(task);
    }

    if (has_node_policy(node->policy_flags, NodePolicyFlag::JoinIncomingBranches) &&
        !branch.join_stack.empty()) {
        const uint32_t split_id = branch.join_stack.back();
        if (run.join_scopes.find(split_id) == run.join_scopes.end()) {
            branch.frame.status = ExecutionStatus::Failed;
            record.node_result.status = NodeResult::HardFail;
            record.node_result.flags = kToolFlagValidationError;
            record.error_message = "join scope not found for branch";
            return record;
        }

        record.started_at_ns = now_ns();
        record.ended_at_ns = record.started_at_ns;
        record.node_result = NodeResult::waiting({}, 1.0F);
        record.node_result.flags = kTraceFlagJoinBarrier;
        record.progressed = true;
        record.blocked_on_join = true;
        record.blocked_join_scope_id = split_id;
        return record;
    }

    record.started_at_ns = now_ns();
    try {
        if (node->kind == NodeKind::Subgraph) {
            record.node_result = execute_subgraph_node(record.task.run_id, run, *node, branch);
        } else {
            std::vector<TaskRecord> recorded_effects;
            ExecutionContext context{
                branch.state_store.get_current_state(),
                record.task.run_id,
                branch.frame.graph_id,
                task.node_id,
                task.branch_id,
                run.runtime_config_payload,
                branch.scratch,
                branch.state_store.blobs(),
                branch.state_store.strings(),
                branch.state_store.knowledge_graph(),
                branch.state_store.task_journal(),
                runtime_tools(),
                runtime_models(),
                trace_sink_,
                Deadline(
                    node->timeout_ms == 0U
                        ? 0U
                        : now_ns() + (static_cast<uint64_t>(node->timeout_ms) * 1000000ULL)
                ),
                branch.cancel,
                branch.pending_async,
                &recorded_effects
            };
            record.node_result = node->executor(context);
            if (!recorded_effects.empty()) {
                record.node_result.patch.task_records.insert(
                    record.node_result.patch.task_records.end(),
                    std::make_move_iterator(recorded_effects.begin()),
                    std::make_move_iterator(recorded_effects.end())
                );
                record.node_result.flags |= kTraceFlagRecordedEffect;
            }
        }
    } catch (const std::exception& error) {
        record.node_result.status = NodeResult::HardFail;
        record.node_result.flags = kToolFlagHandlerException;
        record.error_message = error.what();
    } catch (...) {
        record.node_result.status = NodeResult::HardFail;
        record.node_result.flags = kToolFlagHandlerException;
        record.error_message = "unknown execution failure";
    }
    record.ended_at_ns = now_ns();

    if (branch.cancel.is_cancelled()) {
        record.node_result.status = NodeResult::Cancelled;
    }

    record.progressed = true;
    return record;
}

NodeResult ExecutionEngine::execute_subgraph_node(
    RunId run_id,
    RunRuntime& run,
    const NodeDefinition& node,
    BranchRuntime& branch
) {
    if (!node.subgraph.has_value() || !node.subgraph->valid()) {
        throw std::runtime_error("subgraph node is missing a valid binding");
    }

    const auto target_graph_iterator = graph_registry_.find(node.subgraph->graph_id);
    if (target_graph_iterator == graph_registry_.end()) {
        throw std::runtime_error("subgraph target graph is not registered");
    }

    ExecutionEngine child_engine(scheduler_.parallelism());
    child_engine.set_checkpoint_policy(checkpoint_policy_);
    child_engine.borrow_runtime_registries(runtime_tools(), runtime_models());
    for (const auto& [_, graph] : graph_registry_) {
        child_engine.register_graph(graph);
    }

    const bool persistent_session = is_persistent_subgraph_binding(*node.subgraph);
    std::string active_session_id;
    uint64_t active_session_revision = 0U;
    const SubgraphSessionRecord* committed_session = nullptr;
    if (branch.pending_subgraph.has_value()) {
        active_session_id = branch.pending_subgraph->session_id;
        active_session_revision = branch.pending_subgraph->session_revision;
    } else if (persistent_session) {
        std::string session_error;
        const std::optional<std::string> resolved_session_id =
            resolve_subgraph_session_id(branch.state_store, *node.subgraph, &session_error);
        if (!resolved_session_id.has_value()) {
            NodeResult result;
            result.status = NodeResult::HardFail;
            result.flags = kToolFlagValidationError;
            return result;
        }
        active_session_id = *resolved_session_id;
        {
            std::lock_guard<std::mutex> session_guard(*run.subgraph_session_mutex);
            if (!acquire_subgraph_session_lease(
                    run.active_subgraph_session_leases,
                    node.id,
                    active_session_id,
                    branch.frame.active_branch_id,
                    &session_error
                )) {
                NodeResult result;
                result.status = NodeResult::HardFail;
                result.flags = kToolFlagValidationError;
                return result;
            }
            committed_session = lookup_committed_subgraph_session(
                run.committed_subgraph_sessions,
                node.id,
                active_session_id
            );
            active_session_revision = committed_session == nullptr
                ? 1U
                : committed_session->session_revision + 1U;
        }
    }
    branch.last_subgraph_session_id = active_session_id;
    branch.last_subgraph_session_revision = active_session_revision;

    auto release_active_session_lease = [&]() {
        if (!persistent_session || active_session_id.empty()) {
            return;
        }
        std::lock_guard<std::mutex> session_guard(*run.subgraph_session_mutex);
        release_subgraph_session_lease(
            run.active_subgraph_session_leases,
            node.id,
            active_session_id,
            branch.frame.active_branch_id
        );
    };

    try {
    RunId child_run_id = 0U;
    if (branch.pending_subgraph.has_value()) {
        if (!branch.pending_subgraph->valid()) {
            throw std::runtime_error("pending subgraph snapshot is invalid");
        }

        const RunSnapshot child_snapshot =
            deserialize_run_snapshot_bytes(branch.pending_subgraph->snapshot_bytes);
        std::string restore_error;
        std::size_t missing_async_handles = 0U;
        child_run_id = branch.pending_subgraph->child_run_id;
        if (!child_engine.restore_run_from_snapshot(
                child_run_id,
                child_snapshot,
                &restore_error,
                &missing_async_handles,
                false,
                !branch.pending_async.has_value() && branch.pending_async_group.empty()
            )) {
            throw std::runtime_error(
                restore_error.empty()
                    ? "failed to restore nested subgraph snapshot"
                    : restore_error
            );
        }
        if (missing_async_handles != 0U) {
            throw std::runtime_error("nested subgraph restore lost async handles");
        }
    } else {
        InputEnvelope child_input;
        child_input.initial_field_count = infer_subgraph_initial_field_count(*node.subgraph);
        child_input.runtime_config_payload = run.runtime_config_payload;

        child_run_id = child_engine.start(target_graph_iterator->second, child_input);
        BranchRuntime& child_root_branch = child_engine.runs_.at(child_run_id).branches.at(0U);
        bool seed_knowledge_graph = node.subgraph->propagate_knowledge_graph;
        if (committed_session != nullptr) {
            const RunSnapshot committed_snapshot =
                deserialize_run_snapshot_bytes(committed_session->snapshot_bytes);
            std::string projection_error;
            const std::optional<StateStore> committed_projection =
                project_subgraph_session_state(committed_snapshot, &projection_error);
            if (!committed_projection.has_value()) {
                release_active_session_lease();
                NodeResult result;
                result.status = NodeResult::HardFail;
                result.flags = kToolFlagValidationError;
                return result;
            }
            child_root_branch.state_store = *committed_projection;
            seed_knowledge_graph = false;
        }
        const StatePatch child_input_patch =
            build_subgraph_input_patch(
                branch.state_store,
                *node.subgraph,
                child_root_branch.state_store,
                seed_knowledge_graph
            );
        if (!child_input_patch.empty()) {
            static_cast<void>(child_root_branch.state_store.apply(child_input_patch));
        }
    }

    auto pending_async_ready = [this](const PendingAsyncOperation& pending_async) {
        switch (pending_async.kind) {
            case AsyncOperationKind::Tool:
                return runtime_tools().is_async_ready(AsyncToolHandle{pending_async.handle_id});
            case AsyncOperationKind::Model:
                return runtime_models().is_async_ready(AsyncModelHandle{pending_async.handle_id});
            case AsyncOperationKind::None:
                return false;
        }
        return false;
    };

    auto resume_child_branches = [&](const std::vector<PendingAsyncOperation>& ready_asyncs) {
        RunRuntime& child_runtime = child_engine.runs_.at(child_run_id);
        bool resumed_branch = false;
        for (auto& [child_branch_id, child_branch] : child_runtime.branches) {
            if (!child_branch.pending_async.has_value()) {
                continue;
            }

            const bool matched_ready_async = std::any_of(
                ready_asyncs.begin(),
                ready_asyncs.end(),
                [&child_branch](const PendingAsyncOperation& ready_async) {
                    return pending_async_equal(*child_branch.pending_async, ready_async);
                }
            );
            if (!matched_ready_async) {
                continue;
            }

            child_engine.scheduler_.enqueue_task(ScheduledTask{
                child_run_id,
                child_branch.frame.current_node,
                child_branch_id,
                now_ns()
            });
            child_branch.frame.status = ExecutionStatus::Running;
            resumed_branch = true;
        }
        return resumed_branch;
    };

    auto import_child_trace = [&]() -> std::vector<TraceEvent> {
        const ExecutionNamespaceRef prefix{
            run.graph.id,
            node.id,
            active_session_id,
            active_session_revision
        };
        std::vector<TraceEvent> child_trace = child_engine.trace().events_for_run(child_run_id);
        for (TraceEvent child_event : child_trace) {
            child_event.run_id = run_id;
            if (child_event.session_id.empty()) {
                child_event.session_id = active_session_id;
                child_event.session_revision = active_session_revision;
            }
            child_event.namespace_path = prefix_namespace_path(prefix, child_event.namespace_path);
            trace_sink_.emit(child_event);
        }
        return child_trace;
    };

    auto snapshot_waiting_child = [&]() -> NodeResult {
        RunRuntime& child_runtime = child_engine.runs_.at(child_run_id);
        child_engine.scheduler_.remove_run(child_run_id);
        child_engine.update_run_status(child_runtime, child_run_id);
        const RunSnapshot child_snapshot = child_engine.snapshot_run(child_run_id, child_runtime);
        branch.pending_subgraph = PendingSubgraphExecution{
            child_run_id,
            serialize_run_snapshot_bytes(child_snapshot),
            active_session_id,
            active_session_revision
        };
        const std::vector<TraceEvent> child_trace = import_child_trace();
        const float confidence = child_trace.empty() ? 1.0F : child_trace.back().confidence;

        branch.pending_async_group.clear();
        std::vector<PendingAsyncOperation> bubbled_waits;
        for (const BranchSnapshot& child_branch_snapshot : child_snapshot.branches) {
            for (const PendingAsyncOperation& pending_async : collect_pending_async_operations(child_branch_snapshot)) {
                append_unique_pending_async(bubbled_waits, pending_async);
            }
        }

        NodeResult result = NodeResult::waiting({}, confidence);
        if (bubbled_waits.size() == 1U) {
            result.pending_async = bubbled_waits.front();
        } else if (!bubbled_waits.empty()) {
            branch.pending_async_group = std::move(bubbled_waits);
        }
        return result;
    };

    if (branch.pending_async.has_value()) {
        if (!pending_async_ready(*branch.pending_async)) {
            NodeResult result = NodeResult::waiting({}, 1.0F);
            result.pending_async = branch.pending_async;
            return result;
        }

        if (!resume_child_branches(std::vector<PendingAsyncOperation>{*branch.pending_async}) &&
            !child_engine.scheduler_.has_tasks_for_run(child_run_id)) {
            throw std::runtime_error("nested subgraph resume could not find an async continuation branch");
        }
        branch.pending_async.reset();
        branch.pending_async_group.clear();
    } else if (!branch.pending_async_group.empty()) {
        std::vector<PendingAsyncOperation> ready_asyncs;
        for (const PendingAsyncOperation& pending_async : branch.pending_async_group) {
            if (pending_async_ready(pending_async)) {
                append_unique_pending_async(ready_asyncs, pending_async);
            }
        }

        if (ready_asyncs.empty()) {
            return NodeResult::waiting({}, 1.0F);
        }

        if (!resume_child_branches(ready_asyncs) &&
            !child_engine.scheduler_.has_tasks_for_run(child_run_id)) {
            throw std::runtime_error("nested subgraph multi-wait resume found no runnable continuation branch");
        }
        branch.pending_async_group.clear();
    }

    while (true) {
        const StepResult child_step = child_engine.step(child_run_id);
        if (!child_step.progressed) {
            if (child_step.waiting) {
                return snapshot_waiting_child();
            }
            if (child_step.status == ExecutionStatus::Running) {
                throw std::runtime_error("subgraph runtime stalled without progress");
            }
            break;
        }

        if (child_step.status == ExecutionStatus::Completed ||
            child_step.status == ExecutionStatus::Failed ||
            child_step.status == ExecutionStatus::Cancelled) {
            break;
        }

        if (child_step.waiting &&
            child_engine.scheduler_.has_ready_for_run(child_run_id, now_ns())) {
            continue;
        }
        if (child_step.waiting) {
            return snapshot_waiting_child();
        }
    }

    const RunRuntime& child_run = child_engine.runs_.at(child_run_id);
    const RunSnapshot child_snapshot = child_engine.snapshot_run(child_run_id, child_run);
    const std::vector<TraceEvent> child_trace = import_child_trace();
    const float confidence = child_trace.empty() ? 1.0F : child_trace.back().confidence;
    branch.pending_subgraph.reset();
    branch.pending_async_group.clear();

    if (child_run.status == ExecutionStatus::Failed) {
        release_active_session_lease();
        NodeResult result;
        result.status = NodeResult::HardFail;
        result.confidence = confidence;
        result.flags = kToolFlagValidationError;
        return result;
    }
    if (child_run.status == ExecutionStatus::Cancelled) {
        release_active_session_lease();
        NodeResult result;
        result.status = NodeResult::Cancelled;
        result.confidence = confidence;
        return result;
    }
    if (child_run.status != ExecutionStatus::Completed) {
        throw std::runtime_error("subgraph execution did not reach a terminal state");
    }

    std::string projection_error;
    const std::optional<StateStore> projected_child_state =
        project_subgraph_session_state(child_snapshot, &projection_error);
    if (!projected_child_state.has_value()) {
        release_active_session_lease();
        NodeResult result;
        result.status = NodeResult::HardFail;
        result.confidence = confidence;
        result.flags = kToolFlagValidationError;
        return result;
    }

    if (persistent_session) {
        std::lock_guard<std::mutex> session_guard(*run.subgraph_session_mutex);
        store_committed_subgraph_session(
            run.committed_subgraph_sessions,
            node.id,
            active_session_id,
            active_session_revision,
            serialize_run_snapshot_bytes(child_snapshot)
        );
        release_subgraph_session_lease(
            run.active_subgraph_session_leases,
            node.id,
            active_session_id,
            branch.frame.active_branch_id
        );
    }

    StatePatch patch = build_subgraph_output_patch(
        *projected_child_state,
        *node.subgraph,
        branch.state_store
    );
    return NodeResult::success(std::move(patch), confidence);
    } catch (...) {
        release_active_session_lease();
        throw;
    }
}

StepResult ExecutionEngine::commit_task_execution(RunId run_id, RunRuntime& run, const TaskExecutionRecord& record) {
    StepResult result;
    result.run_id = run_id;
    result.node_id = record.task.node_id;
    result.node_status = record.node_result.status;
    result.progressed = record.progressed;
    result.message = record.error_message;

    auto branch_iterator = run.branches.find(record.task.branch_id);
    if (branch_iterator == run.branches.end()) {
        run.status = ExecutionStatus::Failed;
        result.status = ExecutionStatus::Failed;
        result.message = "branch not found during commit";
        return result;
    }

    BranchRuntime& branch = branch_iterator->second;
    const NodeDefinition* node = run.graph.find_node(record.task.node_id);
    if (node == nullptr) {
        branch.frame.status = ExecutionStatus::Failed;
        run.status = ExecutionStatus::Failed;
        result.status = ExecutionStatus::Failed;
        result.message = "node not found during commit";
        return result;
    }

    branch.frame.step_index += 1U;
    std::size_t enqueued_tasks = 0;
    uint32_t checkpoint_branch_id = record.task.branch_id;
    BranchRuntime* checkpoint_branch = &branch;
    uint64_t checkpoint_patch_offset = 0U;
    uint32_t trace_flags = record.node_result.flags;

    if (record.blocked_on_join) {
        branch.pending_async.reset();
        branch.pending_async_group.clear();
        branch.pending_subgraph.reset();
        branch.retry_count = 0;

        auto join_scope_iterator = run.join_scopes.find(record.blocked_join_scope_id);
        if (join_scope_iterator == run.join_scopes.end()) {
            branch.frame.status = ExecutionStatus::Failed;
            run.status = ExecutionStatus::Failed;
            result.status = ExecutionStatus::Failed;
            result.message = "join scope missing during barrier commit";
            return result;
        }

        JoinScope& join_scope = join_scope_iterator->second;
        if (branch.join_stack.empty() || branch.join_stack.back() != join_scope.split_id) {
            branch.frame.status = ExecutionStatus::Failed;
            run.status = ExecutionStatus::Failed;
            result.status = ExecutionStatus::Failed;
            result.message = "branch join stack is not aligned with join scope";
            return result;
        }

        if (join_scope.join_node_id == 0U) {
            join_scope.join_node_id = record.task.node_id;
        } else if (join_scope.join_node_id != record.task.node_id) {
            branch.frame.status = ExecutionStatus::Failed;
            run.status = ExecutionStatus::Failed;
            result.status = ExecutionStatus::Failed;
            result.message = "join scope converged on multiple join nodes";
            return result;
        }

        if (contains_branch_id(join_scope.arrived_branch_ids, record.task.branch_id)) {
            branch.frame.status = ExecutionStatus::Failed;
            run.status = ExecutionStatus::Failed;
            result.status = ExecutionStatus::Failed;
            result.message = "branch arrived at join barrier more than once";
            return result;
        }

        join_scope.arrived_branch_ids.push_back(record.task.branch_id);
        std::sort(join_scope.arrived_branch_ids.begin(), join_scope.arrived_branch_ids.end());
        branch.frame.status = ExecutionStatus::Paused;

        if (join_scope.arrived_branch_ids.size() > join_scope.expected_branch_count) {
            branch.frame.status = ExecutionStatus::Failed;
            run.status = ExecutionStatus::Failed;
            result.status = ExecutionStatus::Failed;
            result.message = "join scope accepted more branches than expected";
            return result;
        }

        if (join_scope.arrived_branch_ids.size() == join_scope.expected_branch_count) {
            const std::vector<uint32_t> arrived_branch_ids = join_scope.arrived_branch_ids;
            const uint32_t survivor_branch_id = arrived_branch_ids.front();
            StateStore merged_state = join_scope.base_state;
            uint64_t merged_step_index = 0U;
            std::string merge_error_message;
            std::optional<NodeId> merged_reactive_root_node_id;

            for (uint32_t arrived_branch_id : arrived_branch_ids) {
                const auto arrived_branch_iterator = run.branches.find(arrived_branch_id);
                if (arrived_branch_iterator == run.branches.end()) {
                    branch.frame.status = ExecutionStatus::Failed;
                    run.status = ExecutionStatus::Failed;
                    result.status = ExecutionStatus::Failed;
                    result.message = "join scope lost a participating branch";
                    return result;
                }

                if (arrived_branch_iterator->second.reactive_root_node_id.has_value()) {
                    if (!merged_reactive_root_node_id.has_value()) {
                        merged_reactive_root_node_id = arrived_branch_iterator->second.reactive_root_node_id;
                    } else if (*merged_reactive_root_node_id !=
                               *arrived_branch_iterator->second.reactive_root_node_id) {
                        branch.frame.status = ExecutionStatus::Failed;
                        run.status = ExecutionStatus::Failed;
                        result.status = ExecutionStatus::Failed;
                        result.message = "join scope attempted to merge multiple reactive roots";
                        return result;
                    }
                }

                replay_branch_knowledge_graph_delta(
                    arrived_branch_iterator->second.state_store,
                    join_scope.split_patch_log_offset,
                    merged_state
                );
                merged_step_index = std::max(
                    merged_step_index,
                    arrived_branch_iterator->second.frame.step_index
                );
            }

            const auto writes_by_key = collect_branch_field_writes(
                arrived_branch_ids,
                run.branches,
                join_scope.split_patch_log_offset
            );
            std::vector<StateKey> merge_keys;
            merge_keys.reserve(writes_by_key.size());
            for (const auto& [key, _] : writes_by_key) {
                merge_keys.push_back(key);
            }
            std::sort(merge_keys.begin(), merge_keys.end());

            StatePatch merged_field_patch;
            for (StateKey key : merge_keys) {
                const auto merged_value = merge_join_field_values(
                    *node,
                    key,
                    writes_by_key.at(key),
                    join_scope.base_state,
                    merged_state,
                    &merge_error_message
                );
                if (!merge_error_message.empty()) {
                    branch.frame.status = ExecutionStatus::Failed;
                    run.status = ExecutionStatus::Failed;
                    result.status = ExecutionStatus::Failed;
                    result.message = std::move(merge_error_message);
                    return result;
                }
                if (merged_value.has_value()) {
                    merged_field_patch.updates.push_back(FieldUpdate{key, *merged_value});
                }
            }

            if (!merged_field_patch.empty()) {
                checkpoint_patch_offset = merged_state.apply(merged_field_patch);
            } else {
                checkpoint_patch_offset = static_cast<uint64_t>(merged_state.patch_log().size());
            }

            BranchRuntime& survivor_branch = run.branches.at(survivor_branch_id);
            survivor_branch.state_store = std::move(merged_state);
            survivor_branch.pending_async.reset();
            survivor_branch.pending_async_group.clear();
            survivor_branch.pending_subgraph.reset();
            survivor_branch.retry_count = 0;
            survivor_branch.frame.current_node = record.task.node_id;
            survivor_branch.frame.step_index = merged_step_index;
            survivor_branch.frame.status = ExecutionStatus::Running;
            survivor_branch.reactive_root_node_id = merged_reactive_root_node_id;

            if (survivor_branch.join_stack.empty() ||
                survivor_branch.join_stack.back() != join_scope.split_id) {
                branch.frame.status = ExecutionStatus::Failed;
                run.status = ExecutionStatus::Failed;
                result.status = ExecutionStatus::Failed;
                result.message = "survivor branch lost its join scope stack";
                return result;
            }
            survivor_branch.join_stack.pop_back();

            for (uint32_t arrived_branch_id : arrived_branch_ids) {
                if (arrived_branch_id != survivor_branch_id) {
                    run.branches.erase(arrived_branch_id);
                }
            }

            run.join_scopes.erase(join_scope_iterator);
            scheduler_.enqueue_task(ScheduledTask{
                run_id,
                record.task.node_id,
                survivor_branch_id,
                now_ns()
            });
            enqueued_tasks = 1U;
            checkpoint_branch_id = survivor_branch_id;
            checkpoint_branch = &run.branches.at(survivor_branch_id);
            trace_flags |= kTraceFlagJoinMerged;
        } else {
            checkpoint_patch_offset = static_cast<uint64_t>(checkpoint_branch->state_store.patch_log().size());
        }
    } else {
        const StateApplyResult apply_result = branch.state_store.apply_with_summary(record.node_result.patch);
        checkpoint_patch_offset = apply_result.patch_log_offset;
        const std::vector<uint32_t> reactive_node_indices = collect_reactive_node_indices(
            run.graph,
            branch.state_store.strings(),
            apply_result.knowledge_graph_delta,
            record.task.node_id
        );

        if (record.node_result.status == NodeResult::Waiting) {
            branch.frame.status = ExecutionStatus::Paused;
            branch.pending_async = record.node_result.pending_async;
            if (node->kind != NodeKind::Subgraph) {
                branch.pending_async_group.clear();
                branch.pending_subgraph.reset();
            } else if (branch.pending_async.has_value()) {
                branch.pending_async_group.clear();
            }
            if (branch.pending_async.has_value()) {
                re_register_async_waiter(run_id, record.task.branch_id, *branch.pending_async);
            }
            for (const PendingAsyncOperation& pending_async : branch.pending_async_group) {
                re_register_async_waiter(run_id, record.task.branch_id, pending_async);
            }
        } else if ((record.node_result.status == NodeResult::SoftFail ||
                    record.node_result.status == NodeResult::HardFail) &&
                   branch.retry_count < node->retry_limit) {
            branch.pending_async.reset();
            branch.pending_async_group.clear();
            branch.pending_subgraph.reset();
            branch.retry_count += 1U;
            branch.frame.status = ExecutionStatus::Running;
            scheduler_.enqueue_task(ScheduledTask{run_id, record.task.node_id, record.task.branch_id, now_ns()});
            enqueued_tasks = 1U;
        } else {
            branch.pending_async.reset();
            branch.pending_async_group.clear();
            branch.pending_subgraph.reset();
            branch.retry_count = 0;

            if (record.node_result.status == NodeResult::HardFail) {
                branch.frame.status = ExecutionStatus::Failed;
            } else if (record.node_result.status == NodeResult::Cancelled) {
                branch.frame.status = ExecutionStatus::Cancelled;
            } else if (has_node_policy(node->policy_flags, NodePolicyFlag::StopAfterNode)) {
                branch.frame.status = ExecutionStatus::Completed;
            } else if (record.node_result.next_override.has_value()) {
                if (run.graph.find_node(*record.node_result.next_override) == nullptr) {
                    branch.frame.status = ExecutionStatus::Failed;
                    run.status = ExecutionStatus::Failed;
                } else {
                    branch.frame.status = ExecutionStatus::Running;
                    scheduler_.enqueue_task(ScheduledTask{
                        run_id,
                        *record.node_result.next_override,
                        record.task.branch_id,
                        now_ns()
                    });
                    enqueued_tasks = 1U;
                }
            } else {
                const SelectedEdges next_edges = select_edges(
                    run.graph,
                    *node,
                    branch.state_store.get_current_state(),
                    record.node_result
                );

                if (next_edges.empty()) {
                    if (record.node_result.status == NodeResult::HardFail) {
                        branch.frame.status = ExecutionStatus::Failed;
                    } else if (record.node_result.status == NodeResult::Cancelled) {
                        branch.frame.status = ExecutionStatus::Cancelled;
                    } else {
                        branch.frame.status = ExecutionStatus::Completed;
                    }
                } else {
                    const bool create_join_scope =
                        has_node_policy(node->policy_flags, NodePolicyFlag::AllowFanOut) &&
                        has_node_policy(node->policy_flags, NodePolicyFlag::CreateJoinScope) &&
                        next_edges.size() > 1U;

                    if (create_join_scope) {
                        const uint32_t split_id = run.next_split_id++;
                        run.join_scopes.emplace(split_id, JoinScope{
                            split_id,
                            static_cast<uint32_t>(next_edges.size()),
                            0U,
                            static_cast<uint64_t>(branch.state_store.patch_log().size()),
                            branch.state_store,
                            {}
                        });
                        branch.join_stack.push_back(split_id);
                    }

                    const EdgeDefinition* primary_edge = next_edges.front();
                    branch.frame.status = ExecutionStatus::Running;
                    scheduler_.enqueue_task(ScheduledTask{
                        run_id,
                        primary_edge->to,
                        record.task.branch_id,
                        now_ns()
                    });
                    enqueued_tasks = 1U;

                    if (has_node_policy(node->policy_flags, NodePolicyFlag::AllowFanOut)) {
                        const ExecutionFrame branch_frame_template = branch.frame;
                        const StateStore branch_state_template = branch.state_store;
                        const uint16_t branch_retry_template = branch.retry_count;
                        const std::vector<uint32_t> branch_join_stack_template = branch.join_stack;
                        const std::optional<NodeId> branch_reactive_root_template = branch.reactive_root_node_id;
                        const std::string branch_subgraph_session_id_template =
                            branch.last_subgraph_session_id;
                        const uint64_t branch_subgraph_session_revision_template =
                            branch.last_subgraph_session_revision;
                        for (std::size_t index = 1; index < next_edges.size(); ++index) {
                            BranchRuntime cloned_branch;
                            cloned_branch.frame = branch_frame_template;
                            cloned_branch.frame.active_branch_id = run.next_branch_id;
                            cloned_branch.state_store = branch_state_template;
                            cloned_branch.retry_count = branch_retry_template;
                            cloned_branch.join_stack = branch_join_stack_template;
                            cloned_branch.reactive_root_node_id = branch_reactive_root_template;
                            cloned_branch.last_subgraph_session_id = branch_subgraph_session_id_template;
                            cloned_branch.last_subgraph_session_revision =
                                branch_subgraph_session_revision_template;
                            run.branches.emplace(run.next_branch_id, std::move(cloned_branch));
                            scheduler_.enqueue_task(ScheduledTask{
                                run_id,
                                next_edges[index]->to,
                                run.next_branch_id,
                                now_ns()
                            });
                            ++run.next_branch_id;
                            ++enqueued_tasks;
                        }
                    }
                }
            }
        }

        if (run.status != ExecutionStatus::Failed && !reactive_node_indices.empty()) {
            const ExecutionFrame reactive_frame_seed = branch.frame;
            const StateStore reactive_state_seed = branch.state_store;
            for (uint32_t reactive_node_index : reactive_node_indices) {
                if (reactive_node_index >= run.graph.nodes.size()) {
                    continue;
                }

                mark_reactive_trigger(
                    run_id,
                    run,
                    run.graph.nodes[reactive_node_index].id,
                    reactive_state_seed,
                    reactive_frame_seed,
                    enqueued_tasks,
                    trace_flags
                );
            }
        }

        rebuild_reactive_frontier_state(run);
        finalize_reactive_frontier_for_branch(run_id, run, branch, enqueued_tasks, trace_flags);
    }

    rebuild_reactive_frontier_state(run);

    checkpoint_branch = &run.branches.at(checkpoint_branch_id);

    if (!checkpoint_branch->join_stack.empty() &&
        (checkpoint_branch->frame.status == ExecutionStatus::Completed ||
         checkpoint_branch->frame.status == ExecutionStatus::Cancelled)) {
        checkpoint_branch->frame.status = ExecutionStatus::Failed;
        run.status = ExecutionStatus::Failed;
        if (result.message.empty()) {
            result.message = "branch terminated before satisfying join scope";
        }
    }

    result.branch_id = checkpoint_branch_id;
    result.step_index = checkpoint_branch->frame.step_index;

    update_run_status(run, run_id);

    const CheckpointId checkpoint_id = static_cast<CheckpointId>(checkpoint_manager_.size() + 1U);
    checkpoint_branch->frame.checkpoint_id = checkpoint_id;
    Checkpoint checkpoint{
        run_id,
        checkpoint_id,
        record.task.node_id,
        checkpoint_branch->state_store.get_current_state().version,
        checkpoint_patch_offset,
        run.status,
        checkpoint_branch_id,
        checkpoint_branch->frame.step_index
    };
    const bool capture_snapshot = should_capture_checkpoint_snapshot(
        run,
        *checkpoint_branch,
        record,
        trace_flags,
        enqueued_tasks
    );
    checkpoint_manager_.append(
        checkpoint,
        capture_snapshot ? std::optional<RunSnapshot>{snapshot_run(run_id, run)} : std::nullopt
    );

    trace_sink_.emit(TraceEvent{
        0U,
        record.started_at_ns,
        record.ended_at_ns,
        run_id,
        run.graph.id,
        record.task.node_id,
        checkpoint_branch_id,
        checkpoint_id,
        record.node_result.status,
        record.node_result.confidence,
        static_cast<uint32_t>(record.node_result.patch.updates.size()),
        trace_flags,
        node->kind == NodeKind::Subgraph ? checkpoint_branch->last_subgraph_session_id : std::string{},
        node->kind == NodeKind::Subgraph ? checkpoint_branch->last_subgraph_session_revision : 0U,
        {}
    });

    result.checkpoint_id = checkpoint_id;
    result.enqueued_tasks = enqueued_tasks;
    result.waiting = (record.node_result.status == NodeResult::Waiting) ||
        (run.status == ExecutionStatus::Paused) ||
        scheduler_.has_async_waiters_for_run(run_id);
    result.status = run.status;
    return result;
}

void ExecutionEngine::update_run_status(RunRuntime& run, RunId run_id) {
    bool has_failed_branch = false;
    bool has_paused_branch = false;
    bool has_completed_branch = false;
    bool has_cancelled_branch = false;
    bool has_active_branch = false;

    for (const auto& [_, branch] : run.branches) {
        switch (branch.frame.status) {
            case ExecutionStatus::Failed:
                has_failed_branch = true;
                break;
            case ExecutionStatus::Paused:
                has_paused_branch = true;
                break;
            case ExecutionStatus::Completed:
                has_completed_branch = true;
                break;
            case ExecutionStatus::Cancelled:
                has_cancelled_branch = true;
                break;
            case ExecutionStatus::Running:
            case ExecutionStatus::NotStarted:
                has_active_branch = true;
                break;
        }
    }

    const bool has_pending_tasks = scheduler_.has_tasks_for_run(run_id);
    const bool has_async_waiters = scheduler_.has_async_waiters_for_run(run_id);
    if (has_failed_branch && !has_pending_tasks) {
        run.status = ExecutionStatus::Failed;
        return;
    }
    if (has_pending_tasks || has_active_branch || has_async_waiters) {
        run.status = ExecutionStatus::Running;
        return;
    }
    if (has_paused_branch) {
        run.status = ExecutionStatus::Paused;
        return;
    }
    if (has_completed_branch) {
        run.status = ExecutionStatus::Completed;
        return;
    }
    if (has_cancelled_branch) {
        run.status = ExecutionStatus::Cancelled;
        return;
    }

    run.status = ExecutionStatus::NotStarted;
}

GraphDefinition ExecutionEngine::resolve_runtime_graph(const RunSnapshot& snapshot) const {
    const auto iterator = graph_registry_.find(snapshot.graph.id);
    if (iterator != graph_registry_.end()) {
        return iterator->second;
    }
    GraphDefinition runtime_graph = snapshot.graph;
    runtime_graph.compile_runtime();
    return runtime_graph;
}

const GraphDefinition* ExecutionEngine::registered_graph(GraphId graph_id) const noexcept {
    const auto iterator = graph_registry_.find(graph_id);
    if (iterator != graph_registry_.end()) {
        return &iterator->second;
    }

    for (const auto& [_, run] : runs_) {
        if (run.graph.id == graph_id) {
            return &run.graph;
        }
    }

    return nullptr;
}

void ExecutionEngine::re_register_async_waiter(
    RunId run_id,
    uint32_t branch_id,
    const PendingAsyncOperation& pending_async
) {
    auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        return;
    }
    const auto branch_iterator = run_iterator->second.branches.find(branch_id);
    if (branch_iterator == run_iterator->second.branches.end()) {
        return;
    }

    const ScheduledTask task{
        run_id,
        branch_iterator->second.frame.current_node,
        branch_id,
        0U
    };

    switch (pending_async.kind) {
        case AsyncOperationKind::Tool: {
            const AsyncToolHandle handle{pending_async.handle_id};
            if (!runtime_tools().has_async_handle(handle)) {
                return;
            }
            scheduler_.register_async_waiter(async_wait_key_for(pending_async), task);
            break;
        }
        case AsyncOperationKind::Model: {
            const AsyncModelHandle handle{pending_async.handle_id};
            if (!runtime_models().has_async_handle(handle)) {
                return;
            }
            scheduler_.register_async_waiter(async_wait_key_for(pending_async), task);
            break;
        }
        case AsyncOperationKind::None:
            break;
    }
}

} // namespace agentcore
