#include "agentcore/execution/engine.h"

#include "agentcore/execution/subgraph/inline/runner.h"
#include "agentcore/execution/subgraph/subgraph_runtime.h"
#include "agentcore/execution/subgraph/reuse/engine_pool.h"
#include "agentcore/graph/composition/subgraph.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentcore {

namespace {

bool runtime_debug_enabled() {
    static const bool enabled = std::getenv("AGENTCORE_RUNTIME_DEBUG") != nullptr;
    return enabled;
}

void runtime_debug_log(const std::string& message) {
    if (runtime_debug_enabled()) {
        std::cerr << "[agentcore_runtime_debug] " << message << '\n';
    }
}

constexpr uint64_t kImmediateTaskReadyAtNs = 0U;
constexpr uint32_t kTraceFlagJoinBarrier = 1U << 30;
constexpr uint32_t kTraceFlagJoinMerged = 1U << 29;
constexpr uint32_t kTraceFlagKnowledgeReactive = 1U << 28;
constexpr uint32_t kTraceFlagManualInterrupt = 1U << 27;
constexpr uint32_t kTraceFlagManualStateEdit = 1U << 26;
constexpr uint32_t kTraceFlagRecordedEffect = 1U << 25;
constexpr uint32_t kTraceFlagIntelligenceReactive = 1U << 24;
constexpr std::byte kSequenceBlobTag{0x04};
constexpr std::byte kMessageBlobTag{0x05};
constexpr std::size_t kSequenceHeaderSize = 1U + sizeof(uint64_t);
constexpr std::size_t kMessageHeaderSize = 1U + sizeof(uint64_t);

struct BranchFieldWrite {
    uint32_t branch_id{0};
    const StateStore* source_state{nullptr};
    Value value;
};

struct MessageRecordView {
    std::string id;
    const std::byte* payload{nullptr};
    std::size_t payload_size{0};
};

void append_u64(std::vector<std::byte>& output, uint64_t value) {
    for (std::size_t byte_index = 0; byte_index < sizeof(uint64_t); ++byte_index) {
        output.push_back(static_cast<std::byte>((value >> (byte_index * 8U)) & 0xFFU));
    }
}

void write_u64_at(std::vector<std::byte>& output, std::size_t offset, uint64_t value) {
    assert(offset <= output.size());
    assert(output.size() - offset >= sizeof(uint64_t));
    for (std::size_t byte_index = 0; byte_index < sizeof(uint64_t); ++byte_index) {
        output[offset + byte_index] =
            static_cast<std::byte>((value >> (byte_index * 8U)) & 0xFFU);
    }
}

bool read_u64(const std::byte* data, std::size_t size, std::size_t* offset, uint64_t* value) {
    if (*offset > size || size - *offset < sizeof(uint64_t)) {
        return false;
    }
    uint64_t decoded = 0;
    for (std::size_t byte_index = 0; byte_index < sizeof(uint64_t); ++byte_index) {
        decoded |= static_cast<uint64_t>(
            std::to_integer<unsigned char>(data[*offset + byte_index])
        ) << (byte_index * 8U);
    }
    *value = decoded;
    *offset += sizeof(uint64_t);
    return true;
}

bool append_sequence_items_from_blob(
    const BlobStore& source_blobs,
    BlobRef ref,
    std::vector<std::byte>& output,
    uint64_t* item_count,
    std::string* error_message
) {
    const auto buffer = source_blobs.read_buffer(ref);
    const std::byte* data = buffer.first;
    const std::size_t size = buffer.second;
    if (data == nullptr || size < kSequenceHeaderSize || data[0] != kSequenceBlobTag) {
        if (error_message != nullptr) {
            *error_message = "sequence concat merge requires tagged sequence blob values";
        }
        return false;
    }

    std::size_t offset = 1U;
    uint64_t count = 0;
    if (!read_u64(data, size, &offset, &count)) {
        if (error_message != nullptr) {
            *error_message = "sequence concat merge encountered a truncated sequence header";
        }
        return false;
    }

    const std::size_t item_payload_start = offset;
    for (uint64_t index = 0; index < count; ++index) {
        uint64_t item_size = 0;
        if (!read_u64(data, size, &offset, &item_size)) {
            if (error_message != nullptr) {
                *error_message = "sequence concat merge encountered a truncated item header";
            }
            return false;
        }
        if (item_size > static_cast<uint64_t>(size - offset)) {
            if (error_message != nullptr) {
                *error_message = "sequence concat merge encountered a truncated item payload";
            }
            return false;
        }
        offset += static_cast<std::size_t>(item_size);
    }

    if (offset != size) {
        if (error_message != nullptr) {
            *error_message = "sequence concat merge encountered trailing sequence bytes";
        }
        return false;
    }

    output.insert(output.end(), data + item_payload_start, data + size);
    *item_count += count;
    return true;
}

bool append_message_records_from_blob(
    const BlobStore& source_blobs,
    BlobRef ref,
    std::vector<MessageRecordView>& records,
    std::unordered_map<std::string, std::size_t>& index_by_id,
    std::string* error_message
) {
    const auto buffer = source_blobs.read_buffer(ref);
    const std::byte* data = buffer.first;
    const std::size_t size = buffer.second;
    if (data == nullptr || size < kMessageHeaderSize || data[0] != kMessageBlobTag) {
        if (error_message != nullptr) {
            *error_message = "message merge requires tagged message blob values";
        }
        return false;
    }

    std::size_t offset = 1U;
    uint64_t count = 0;
    if (!read_u64(data, size, &offset, &count)) {
        if (error_message != nullptr) {
            *error_message = "message merge encountered a truncated message header";
        }
        return false;
    }

    for (uint64_t index = 0; index < count; ++index) {
        uint64_t id_size = 0;
        if (!read_u64(data, size, &offset, &id_size) ||
            id_size > static_cast<uint64_t>(size - offset)) {
            if (error_message != nullptr) {
                *error_message = "message merge encountered a truncated message id";
            }
            return false;
        }
        std::string id(
            reinterpret_cast<const char*>(data + offset),
            static_cast<std::size_t>(id_size)
        );
        offset += static_cast<std::size_t>(id_size);

        uint64_t payload_size = 0;
        if (!read_u64(data, size, &offset, &payload_size) ||
            payload_size > static_cast<uint64_t>(size - offset)) {
            if (error_message != nullptr) {
                *error_message = "message merge encountered a truncated message payload";
            }
            return false;
        }

        MessageRecordView record{
            std::move(id),
            data + offset,
            static_cast<std::size_t>(payload_size)
        };
        offset += static_cast<std::size_t>(payload_size);

        if (record.id.empty()) {
            records.push_back(std::move(record));
            continue;
        }

        auto existing = index_by_id.find(record.id);
        if (existing == index_by_id.end()) {
            const std::size_t record_index = records.size();
            index_by_id.emplace(record.id, record_index);
            records.push_back(std::move(record));
        } else {
            records[existing->second] = std::move(record);
        }
    }

    if (offset != size) {
        if (error_message != nullptr) {
            *error_message = "message merge encountered trailing message bytes";
        }
        return false;
    }

    return true;
}

BlobRef append_merged_message_blob(
    BlobStore& destination_blobs,
    const std::vector<MessageRecordView>& records
) {
    std::size_t byte_count = kMessageHeaderSize;
    for (const MessageRecordView& record : records) {
        byte_count += sizeof(uint64_t) + record.id.size() +
            sizeof(uint64_t) + record.payload_size;
    }

    std::vector<std::byte> merged;
    merged.reserve(byte_count);
    merged.push_back(kMessageBlobTag);
    append_u64(merged, static_cast<uint64_t>(records.size()));
    for (const MessageRecordView& record : records) {
        append_u64(merged, static_cast<uint64_t>(record.id.size()));
        const auto* id_bytes = reinterpret_cast<const std::byte*>(record.id.data());
        merged.insert(merged.end(), id_bytes, id_bytes + record.id.size());
        append_u64(merged, static_cast<uint64_t>(record.payload_size));
        merged.insert(merged.end(), record.payload, record.payload + record.payload_size);
    }
    return destination_blobs.append(merged.data(), merged.size());
}

CheckpointPolicy checkpoint_policy_for_profile(ExecutionProfile profile) {
    switch (profile) {
        case ExecutionProfile::Strict:
            return CheckpointPolicy{64U, true, true, true, true};
        case ExecutionProfile::Balanced:
            return CheckpointPolicy{256U, true, true, true, true};
        case ExecutionProfile::Fast:
            return CheckpointPolicy{0U, true, true, true, false};
    }
    return CheckpointPolicy{};
}

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

constexpr RunId kInlineSubgraphResumableRunId = 1U;

std::pair<std::vector<AsyncToolSnapshot>, std::vector<AsyncModelSnapshot>>
export_pending_async_snapshots(
    const std::vector<PendingAsyncOperation>& pending_asyncs,
    const ToolRegistry& tools,
    const ModelRegistry& models
) {
    std::vector<AsyncToolSnapshot> pending_tool_snapshots;
    std::vector<AsyncModelSnapshot> pending_model_snapshots;
    pending_tool_snapshots.reserve(pending_asyncs.size());
    pending_model_snapshots.reserve(pending_asyncs.size());

    for (const PendingAsyncOperation& pending_async : pending_asyncs) {
        switch (pending_async.kind) {
            case AsyncOperationKind::Tool:
                if (const auto pending_tool_snapshot =
                        tools.export_async_operation(AsyncToolHandle{pending_async.handle_id});
                    pending_tool_snapshot.has_value()) {
                    pending_tool_snapshots.push_back(std::move(*pending_tool_snapshot));
                }
                break;
            case AsyncOperationKind::Model:
                if (const auto pending_model_snapshot =
                        models.export_async_operation(AsyncModelHandle{pending_async.handle_id});
                    pending_model_snapshot.has_value()) {
                    pending_model_snapshots.push_back(std::move(*pending_model_snapshot));
                }
                break;
            case AsyncOperationKind::None:
                break;
        }
    }

    return {std::move(pending_tool_snapshots), std::move(pending_model_snapshots)};
}

RunSnapshot build_inline_single_branch_subgraph_snapshot(
    const GraphDefinition& graph,
    const std::vector<std::byte>& runtime_config_payload,
    const StateStore& state_store,
    const ExecutionFrame& frame,
    uint16_t retry_count,
    std::optional<PendingAsyncOperation> pending_async,
    const ToolRegistry& tools,
    const ModelRegistry& models
) {
    RunSnapshot snapshot;
    snapshot.graph = graph;
    snapshot.status = frame.status;
    snapshot.runtime_config_payload = runtime_config_payload;
    snapshot.next_branch_id = 1U;
    snapshot.next_split_id = 1U;

    BranchSnapshot branch_snapshot;
    branch_snapshot.frame = frame;
    branch_snapshot.state_store = state_store;
    branch_snapshot.retry_count = retry_count;
    branch_snapshot.pending_async = pending_async;

    const std::vector<PendingAsyncOperation> pending_asyncs =
        collect_pending_async_operations(branch_snapshot);
    auto [pending_tool_snapshots, pending_model_snapshots] =
        export_pending_async_snapshots(pending_asyncs, tools, models);
    branch_snapshot.pending_tool_snapshots = std::move(pending_tool_snapshots);
    branch_snapshot.pending_model_snapshots = std::move(pending_model_snapshots);

    snapshot.branches.push_back(std::move(branch_snapshot));
    return snapshot;
}

bool subscription_pattern_matches(std::string_view actual, const std::string& expected) {
    return expected.empty() || actual == expected;
}

bool subscription_prefix_matches(std::string_view actual, const std::string& expected_prefix) {
    return expected_prefix.empty() ||
        (actual.size() >= expected_prefix.size() &&
         actual.substr(0U, expected_prefix.size()) == expected_prefix);
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

bool subscription_has_claim_graph_filters(const IntelligenceSubscription& subscription) noexcept {
    return !subscription.subject_label.empty() ||
        !subscription.relation.empty() ||
        !subscription.object_label.empty();
}

bool subscription_matches_task_write(
    const IntelligenceSubscription& subscription,
    const IntelligenceTask& task,
    const StringInterner& strings
) {
    const std::string_view key = strings.resolve(task.key);
    return !subscription_has_claim_graph_filters(subscription) &&
        subscription.kind != IntelligenceSubscriptionKind::Claims &&
        subscription.kind != IntelligenceSubscriptionKind::Evidence &&
        subscription.kind != IntelligenceSubscriptionKind::Decisions &&
        subscription.kind != IntelligenceSubscriptionKind::Memories &&
        subscription_pattern_matches(key, subscription.key) &&
        subscription_prefix_matches(key, subscription.key_prefix) &&
        subscription_pattern_matches(key, subscription.task_key) &&
        subscription_pattern_matches(strings.resolve(task.owner), subscription.owner) &&
        (!subscription.task_status.has_value() || task.status == *subscription.task_status) &&
        task.confidence >= subscription.min_confidence;
}

bool subscription_matches_claim_write(
    const IntelligenceSubscription& subscription,
    const IntelligenceClaim& claim,
    const StringInterner& strings
) {
    const std::string_view key = strings.resolve(claim.key);
    return subscription.kind != IntelligenceSubscriptionKind::Tasks &&
        subscription.kind != IntelligenceSubscriptionKind::Evidence &&
        subscription.kind != IntelligenceSubscriptionKind::Decisions &&
        subscription.kind != IntelligenceSubscriptionKind::Memories &&
        subscription_pattern_matches(key, subscription.key) &&
        subscription_prefix_matches(key, subscription.key_prefix) &&
        subscription_pattern_matches(key, subscription.claim_key) &&
        subscription_pattern_matches(strings.resolve(claim.subject_label), subscription.subject_label) &&
        subscription_pattern_matches(strings.resolve(claim.relation), subscription.relation) &&
        subscription_pattern_matches(strings.resolve(claim.object_label), subscription.object_label) &&
        (!subscription.claim_status.has_value() || claim.status == *subscription.claim_status) &&
        claim.confidence >= subscription.min_confidence;
}

bool subscription_matches_evidence_write(
    const IntelligenceSubscription& subscription,
    const IntelligenceEvidence& evidence,
    const StringInterner& strings
) {
    const std::string_view key = strings.resolve(evidence.key);
    return !subscription_has_claim_graph_filters(subscription) &&
        (subscription.kind == IntelligenceSubscriptionKind::All ||
            subscription.kind == IntelligenceSubscriptionKind::Evidence) &&
        subscription_pattern_matches(key, subscription.key) &&
        subscription_prefix_matches(key, subscription.key_prefix) &&
        subscription_pattern_matches(strings.resolve(evidence.task_key), subscription.task_key) &&
        subscription_pattern_matches(strings.resolve(evidence.claim_key), subscription.claim_key) &&
        subscription_pattern_matches(strings.resolve(evidence.source), subscription.source) &&
        evidence.confidence >= subscription.min_confidence;
}

bool subscription_matches_decision_write(
    const IntelligenceSubscription& subscription,
    const IntelligenceDecision& decision,
    const StringInterner& strings
) {
    const std::string_view key = strings.resolve(decision.key);
    return !subscription_has_claim_graph_filters(subscription) &&
        (subscription.kind == IntelligenceSubscriptionKind::All ||
            subscription.kind == IntelligenceSubscriptionKind::Decisions) &&
        subscription_pattern_matches(key, subscription.key) &&
        subscription_prefix_matches(key, subscription.key_prefix) &&
        subscription_pattern_matches(strings.resolve(decision.task_key), subscription.task_key) &&
        subscription_pattern_matches(strings.resolve(decision.claim_key), subscription.claim_key) &&
        (!subscription.decision_status.has_value() ||
         decision.status == *subscription.decision_status) &&
        decision.confidence >= subscription.min_confidence;
}

bool subscription_matches_memory_write(
    const IntelligenceSubscription& subscription,
    const IntelligenceMemoryEntry& memory,
    const StringInterner& strings
) {
    const std::string_view key = strings.resolve(memory.key);
    return !subscription_has_claim_graph_filters(subscription) &&
        (subscription.kind == IntelligenceSubscriptionKind::All ||
            subscription.kind == IntelligenceSubscriptionKind::Memories) &&
        subscription_pattern_matches(key, subscription.key) &&
        subscription_prefix_matches(key, subscription.key_prefix) &&
        subscription_pattern_matches(strings.resolve(memory.task_key), subscription.task_key) &&
        subscription_pattern_matches(strings.resolve(memory.claim_key), subscription.claim_key) &&
        subscription_pattern_matches(strings.resolve(memory.scope), subscription.scope) &&
        (!subscription.memory_layer.has_value() || memory.layer == *subscription.memory_layer) &&
        memory.importance >= subscription.min_importance;
}

template <typename MatchesSubscriptionFn>
void consider_intelligence_subscription_candidates(
    const GraphDefinition& graph,
    IntelligenceSubscriptionKind kind,
    NodeId source_node_id,
    std::vector<bool>& matched_nodes,
    MatchesSubscriptionFn&& matches_subscription
) {
    auto consider_compiled_index = [&](uint32_t compiled_subscription_index) {
        const CompiledIntelligenceSubscription* compiled =
            graph.intelligence_subscription(compiled_subscription_index);
        if (compiled == nullptr || compiled->node_index >= graph.nodes.size()) {
            return;
        }
        const NodeDefinition& node = graph.nodes[compiled->node_index];
        if (node.id == source_node_id ||
            compiled->subscription_index >= node.intelligence_subscriptions.size() ||
            matched_nodes[compiled->node_index]) {
            return;
        }
        const IntelligenceSubscription& subscription =
            node.intelligence_subscriptions[compiled->subscription_index];
        if (matches_subscription(subscription)) {
            matched_nodes[compiled->node_index] = true;
        }
    };

    for (uint32_t compiled_index : graph.candidate_intelligence_subscriptions(kind)) {
        consider_compiled_index(compiled_index);
    }
    if (kind != IntelligenceSubscriptionKind::All) {
        for (uint32_t compiled_index : graph.candidate_intelligence_subscriptions(
                 IntelligenceSubscriptionKind::All)) {
            consider_compiled_index(compiled_index);
        }
    }
}

std::vector<uint32_t> collect_knowledge_reactive_node_indices(
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

std::vector<uint32_t> collect_intelligence_reactive_node_indices(
    const GraphDefinition& graph,
    const StringInterner& strings,
    const IntelligenceStore& intelligence,
    const IntelligenceDeltaSummary& delta,
    NodeId source_node_id
) {
    if (!graph.is_runtime_compiled() || delta.empty()) {
        return {};
    }

    std::vector<bool> matched_nodes(graph.nodes.size(), false);

    for (const IntelligenceDelta& task_delta : delta.tasks) {
        const IntelligenceTask* task = intelligence.find_task(static_cast<IntelligenceTaskId>(task_delta.id));
        if (task == nullptr) {
            continue;
        }
        consider_intelligence_subscription_candidates(
            graph,
            IntelligenceSubscriptionKind::Tasks,
            source_node_id,
            matched_nodes,
            [&](const IntelligenceSubscription& subscription) {
                return subscription_matches_task_write(subscription, *task, strings);
            }
        );
    }

    for (const IntelligenceDelta& claim_delta : delta.claims) {
        const IntelligenceClaim* claim =
            intelligence.find_claim(static_cast<IntelligenceClaimId>(claim_delta.id));
        if (claim == nullptr) {
            continue;
        }
        consider_intelligence_subscription_candidates(
            graph,
            IntelligenceSubscriptionKind::Claims,
            source_node_id,
            matched_nodes,
            [&](const IntelligenceSubscription& subscription) {
                return subscription_matches_claim_write(subscription, *claim, strings);
            }
        );
    }

    for (const IntelligenceDelta& evidence_delta : delta.evidence) {
        const IntelligenceEvidence* evidence =
            intelligence.find_evidence(static_cast<IntelligenceEvidenceId>(evidence_delta.id));
        if (evidence == nullptr) {
            continue;
        }
        consider_intelligence_subscription_candidates(
            graph,
            IntelligenceSubscriptionKind::Evidence,
            source_node_id,
            matched_nodes,
            [&](const IntelligenceSubscription& subscription) {
                return subscription_matches_evidence_write(subscription, *evidence, strings);
            }
        );
    }

    for (const IntelligenceDelta& decision_delta : delta.decisions) {
        const IntelligenceDecision* decision =
            intelligence.find_decision(static_cast<IntelligenceDecisionId>(decision_delta.id));
        if (decision == nullptr) {
            continue;
        }
        consider_intelligence_subscription_candidates(
            graph,
            IntelligenceSubscriptionKind::Decisions,
            source_node_id,
            matched_nodes,
            [&](const IntelligenceSubscription& subscription) {
                return subscription_matches_decision_write(subscription, *decision, strings);
            }
        );
    }

    for (const IntelligenceDelta& memory_delta : delta.memories) {
        const IntelligenceMemoryEntry* memory =
            intelligence.find_memory(static_cast<IntelligenceMemoryId>(memory_delta.id));
        if (memory == nullptr) {
            continue;
        }
        consider_intelligence_subscription_candidates(
            graph,
            IntelligenceSubscriptionKind::Memories,
            source_node_id,
            matched_nodes,
            [&](const IntelligenceSubscription& subscription) {
                return subscription_matches_memory_write(subscription, *memory, strings);
            }
        );
    }

    std::vector<uint32_t> node_indices;
    for (std::size_t index = 0; index < matched_nodes.size(); ++index) {
        if (matched_nodes[index]) {
            node_indices.push_back(static_cast<uint32_t>(index));
        }
    }
    return node_indices;
}

void append_unique_node_indices(
    std::vector<uint32_t>& target,
    const std::vector<uint32_t>& source
) {
    for (uint32_t node_index : source) {
        if (std::find(target.begin(), target.end(), node_index) == target.end()) {
            target.push_back(node_index);
        }
    }
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
    if (split_patch_log_offset > source_state.patch_log().size()) {
        throw std::runtime_error("invalid split patch offset during knowledge graph merge");
    }
    const std::vector<PatchLogEntry> entries = source_state.patch_log().entries_from(split_patch_log_offset);

    for (const PatchLogEntry& entry : entries) {
        const KnowledgeGraphPatch rebased_kg =
            rebase_knowledge_graph_patch(source_state, destination_state, entry.patch.knowledge_graph);
        if (rebased_kg.empty()) {
            continue;
        }

        StatePatch patch;
        patch.knowledge_graph = rebased_kg;
        patch.flags = entry.patch.flags;
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
        if (split_patch_log_offset > branch_iterator->second->state_store.patch_log().size()) {
            throw std::runtime_error("invalid split patch offset while collecting join field writes");
        }
        const std::vector<PatchLogEntry> entries =
            branch_iterator->second->state_store.patch_log().entries_from(split_patch_log_offset);

        for (const PatchLogEntry& entry : entries) {
            for (const FieldUpdate& update : entry.patch.updates) {
                last_writes_for_branch[update.key] = update.value;
            }
        }

        for (const auto& [key, value] : last_writes_for_branch) {
            writes_by_key[key].push_back(BranchFieldWrite{
                branch_id,
                &branch_iterator->second->state_store,
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
        case JoinMergeStrategy::ConcatSequence: {
            std::vector<std::byte> merged;
            merged.reserve(kSequenceHeaderSize);
            merged.push_back(kSequenceBlobTag);
            append_u64(merged, 0U);

            uint64_t item_count = 0;
            if (const Value* base_value = base_state.find(key); base_value != nullptr && !std::holds_alternative<std::monostate>(*base_value)) {
                if (!std::holds_alternative<BlobRef>(*base_value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires sequence baseline for ConcatSequence on state key " + std::to_string(key)
                    );
                }
                if (!append_sequence_items_from_blob(
                        base_state.blobs(),
                        std::get<BlobRef>(*base_value),
                        merged,
                        &item_count,
                        error_message
                    )) {
                    return std::nullopt;
                }
            }

            for (const BranchFieldWrite& write : writes) {
                if (!std::holds_alternative<BlobRef>(write.value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires sequence branch values for ConcatSequence on state key " + std::to_string(key)
                    );
                }
                if (!append_sequence_items_from_blob(
                        write.source_state->blobs(),
                        std::get<BlobRef>(write.value),
                        merged,
                        &item_count,
                        error_message
                    )) {
                    return std::nullopt;
                }
            }

            write_u64_at(merged, 1U, item_count);
            return Value{destination_state.blobs().append(merged.data(), merged.size())};
        }
        case JoinMergeStrategy::MergeMessages: {
            std::vector<MessageRecordView> records;
            std::unordered_map<std::string, std::size_t> index_by_id;

            if (const Value* base_value = base_state.find(key); base_value != nullptr && !std::holds_alternative<std::monostate>(*base_value)) {
                if (!std::holds_alternative<BlobRef>(*base_value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires message baseline for MergeMessages on state key " + std::to_string(key)
                    );
                }
                if (!append_message_records_from_blob(
                        base_state.blobs(),
                        std::get<BlobRef>(*base_value),
                        records,
                        index_by_id,
                        error_message
                    )) {
                    return std::nullopt;
                }
            }

            for (const BranchFieldWrite& write : writes) {
                if (!std::holds_alternative<BlobRef>(write.value)) {
                    return fail_merge(
                        "join barrier " + join_node.name +
                        " requires message branch values for MergeMessages on state key " + std::to_string(key)
                    );
                }
                if (!append_message_records_from_blob(
                        write.source_state->blobs(),
                        std::get<BlobRef>(write.value),
                        records,
                        index_by_id,
                        error_message
                    )) {
                    return std::nullopt;
                }
            }

            return Value{append_merged_message_blob(destination_state.blobs(), records)};
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

} // namespace

ExecutionEngine::BranchRuntime::BranchRuntime(BranchRuntime&& other) noexcept
    : frame(std::move(other.frame)),
      state_store(std::move(other.state_store)),
      scratch(std::move(other.scratch)),
      deadline(std::move(other.deadline)),
      cancel(std::move(other.cancel)),
      retry_count(other.retry_count),
      pending_async(std::move(other.pending_async)),
      pending_async_group(std::move(other.pending_async_group)),
      pending_subgraph(std::move(other.pending_subgraph)),
      join_stack(std::move(other.join_stack)),
      reactive_root_node_id(std::move(other.reactive_root_node_id)),
      memo_cache(std::move(other.memo_cache)),
      last_subgraph_session_id(std::move(other.last_subgraph_session_id)),
      last_subgraph_session_revision(other.last_subgraph_session_revision) {}

ExecutionEngine::BranchRuntime& ExecutionEngine::BranchRuntime::operator=(BranchRuntime&& other) noexcept {
    if (this != &other) {
        frame = std::move(other.frame);
        state_store = std::move(other.state_store);
        scratch = std::move(other.scratch);
        deadline = std::move(other.deadline);
        cancel = std::move(other.cancel);
        retry_count = other.retry_count;
        pending_async = std::move(other.pending_async);
        pending_async_group = std::move(other.pending_async_group);
        pending_subgraph = std::move(other.pending_subgraph);
        join_stack = std::move(other.join_stack);
        reactive_root_node_id = std::move(other.reactive_root_node_id);
        memo_cache = std::move(other.memo_cache);
        last_subgraph_session_id = std::move(other.last_subgraph_session_id);
        last_subgraph_session_revision = other.last_subgraph_session_revision;
    }
    return *this;
}

ExecutionEngine::RunRuntime::RunRuntime() : mutex(std::make_unique<std::mutex>()) {}

ExecutionEngine::RunRuntime::RunRuntime(RunRuntime&& other) noexcept
    : graph(std::move(other.graph)),
      status(other.status),
      runtime_config_payload(std::move(other.runtime_config_payload)),
      mutex(std::move(other.mutex)),
      branches(std::move(other.branches)),
      next_branch_id(other.next_branch_id),
      join_scopes(std::move(other.join_scopes)),
      reactive_frontiers(std::move(other.reactive_frontiers)),
      committed_subgraph_sessions(std::move(other.committed_subgraph_sessions)),
      active_subgraph_session_leases(std::move(other.active_subgraph_session_leases)),
      subgraph_session_mutex(std::move(other.subgraph_session_mutex)),
      next_split_id(other.next_split_id),
      in_flight_tasks(other.in_flight_tasks),
      capture_options(other.capture_options),
      deferred_ready_task(std::move(other.deferred_ready_task)) {
    if (!mutex) {
        mutex = std::make_unique<std::mutex>();
    }
}

ExecutionEngine::RunRuntime& ExecutionEngine::RunRuntime::operator=(RunRuntime&& other) noexcept {
    if (this != &other) {
        graph = std::move(other.graph);
        status = other.status;
        runtime_config_payload = std::move(other.runtime_config_payload);
        mutex = std::move(other.mutex);
        branches = std::move(other.branches);
        next_branch_id = other.next_branch_id;
        join_scopes = std::move(other.join_scopes);
        reactive_frontiers = std::move(other.reactive_frontiers);
        committed_subgraph_sessions = std::move(other.committed_subgraph_sessions);
        active_subgraph_session_leases = std::move(other.active_subgraph_session_leases);
        subgraph_session_mutex = std::move(other.subgraph_session_mutex);
        next_split_id = other.next_split_id;
        in_flight_tasks = other.in_flight_tasks;
        capture_options = other.capture_options;
        deferred_ready_task = std::move(other.deferred_ready_task);
        if (!mutex) {
            mutex = std::make_unique<std::mutex>();
        }
    }
    return *this;
}

ExecutionEngine::ExecutionEngine(const ExecutionEngineOptions& options)
 : scheduler_(options.worker_count, options.inline_scheduler),
   options_(options),
   checkpoint_policy_(checkpoint_policy_for_profile(options.profile)) {
    subgraph_engine_pool_ = std::make_unique<SubgraphChildEnginePool>(
        SubgraphChildEnginePoolOptions{options.profile}
    );
    checkpoint_manager_.configure(options.profile);
    trace_sink_.configure(options.profile);
    tool_registry_.set_async_completion_listener([this](AsyncToolHandle handle) {
        scheduler_.signal_async_completion(AsyncWaitKey{AsyncWaitKind::Tool, handle.id});
    });
    model_registry_.set_async_completion_listener([this](AsyncModelHandle handle) {
        scheduler_.signal_async_completion(AsyncWaitKey{AsyncWaitKind::Model, handle.id});
    });
}

ExecutionEngine::ExecutionEngine(std::size_t worker_count, bool inline_scheduler)
 : ExecutionEngine(ExecutionEngineOptions{worker_count, inline_scheduler, ExecutionProfile::Balanced}) {}

ExecutionEngine::~ExecutionEngine() = default;

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
        if (!branch->reactive_root_node_id.has_value() ||
            is_terminal_status(branch->frame.status)) {
            continue;
        }

        ReactiveFrontierState& frontier = run.reactive_frontiers[*branch->reactive_root_node_id];
        frontier.node_id = *branch->reactive_root_node_id;
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
    std::size_t& enqueued_tasks
) {
    if (run.graph.find_node(reactive_node_id) == nullptr) {
        return;
    }

    auto reactive_branch = std::make_shared<BranchRuntime>();
    reactive_branch->frame = frame_seed;
    reactive_branch->frame.active_branch_id = run.next_branch_id;
    reactive_branch->frame.current_node = reactive_node_id;
    reactive_branch->frame.status = ExecutionStatus::Running;
    reactive_branch->state_store = state_seed;
    reactive_branch->pending_async.reset();
    reactive_branch->pending_async_group.clear();
    reactive_branch->retry_count = 0;
    reactive_branch->join_stack.clear();
    reactive_branch->reactive_root_node_id = reactive_node_id;

    const uint32_t reactive_branch_id = run.next_branch_id++;
    run.branches.emplace(reactive_branch_id, std::move(reactive_branch));
    scheduler_.enqueue_task(ScheduledTask{
        run_id,
        reactive_node_id,
        reactive_branch_id,
        kImmediateTaskReadyAtNs
    });
    ReactiveFrontierState& frontier = run.reactive_frontiers[reactive_node_id];
    frontier.node_id = reactive_node_id;
    ++enqueued_tasks;
}

void ExecutionEngine::mark_reactive_trigger(
    RunId run_id,
    RunRuntime& run,
    NodeId reactive_node_id,
    const StateStore& state_seed,
    const ExecutionFrame& frame_seed,
    std::size_t& enqueued_tasks
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
            enqueued_tasks
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
        enqueued_tasks
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
    return start(graph, input, RunCaptureOptions{});
}

RunId ExecutionEngine::start(
    const GraphDefinition& graph,
    const InputEnvelope& input,
    const RunCaptureOptions& capture_options
) {
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
    for (const NodeDefinition& node : runtime_graph.nodes) {
        if (node.kind != NodeKind::Subgraph || !node.subgraph.has_value()) {
            continue;
        }
        if (registered_graph(node.subgraph->graph_id) == nullptr) {
            throw std::invalid_argument(
                "subgraph target graph is not registered: " + std::to_string(node.subgraph->graph_id)
            );
        }
    }

    const RunId run_id = next_run_id_++;
    RunRuntime runtime;
    runtime.graph = runtime_graph;
    runtime.status = ExecutionStatus::Running;
    runtime.runtime_config_payload = input.runtime_config_payload;
    runtime.next_branch_id = 1;
    runtime.next_split_id = 1;
    runtime.capture_options = capture_options;
    runtime.deferred_ready_task = ScheduledTask{
        run_id,
        input.entry_override.value_or(runtime_graph.entry),
        0U,
        kImmediateTaskReadyAtNs
    };

    auto root_branch = std::make_shared<BranchRuntime>();
    root_branch->frame.graph_id = runtime_graph.id;
    root_branch->frame.current_node = input.entry_override.value_or(runtime_graph.entry);
    root_branch->frame.active_branch_id = 0;
    root_branch->frame.status = ExecutionStatus::Running;
    root_branch->state_store = StateStore(input.initial_field_count);
    root_branch->state_store.blobs() = input.initial_blobs;
    root_branch->state_store.strings() = input.initial_strings;
    if (!input.initial_patch.empty()) {
        static_cast<void>(root_branch->state_store.apply(input.initial_patch));
    }

    runtime.branches.emplace(0U, std::move(root_branch));
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        runs_.emplace(run_id, std::move(runtime));
    }
    return run_id;
}

StepResult ExecutionEngine::step(RunId run_id) {
    StepResult result;
    result.run_id = run_id;

    RunRuntime* run_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        auto run_iterator = runs_.find(run_id);
        if (run_iterator == runs_.end()) {
            result.message = "run not found";
            result.status = ExecutionStatus::Failed;
            return result;
        }
        run_ptr = &run_iterator->second;
    }
    RunRuntime& run = *run_ptr;

    static_cast<void>(scheduler_.promote_ready_async_tasks(now_ns()));
    std::optional<ScheduledTask> task;
    {
        std::lock_guard<std::mutex> run_lock(*run.mutex);
        task = take_deferred_ready_task_locked(run);
    }
    if (!task.has_value()) {
        task = scheduler_.dequeue_ready_for_run(run_id, now_ns());
    }
    if (!task.has_value()) {
        update_run_status(run, run_id);
        {
            std::lock_guard<std::mutex> run_lock(*run.mutex);
            result.status = run.status;
        }
        result.waiting = (result.status == ExecutionStatus::Paused) || scheduler_.has_async_waiters_for_run(run_id);
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

    RunRuntime* run_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        auto run_iterator = runs_.find(run_id);
        if (run_iterator == runs_.end()) {
            result.status = ExecutionStatus::Failed;
            return result;
        }
        run_ptr = &run_iterator->second;
    }
    RunRuntime& run = *run_ptr;
    const bool single_worker_inline_lane = scheduler_.parallelism() == 1U;
    std::optional<ScheduledTask> inline_followup_task;

    auto dispatch_ready_tasks = [this, run_id, &run]() -> std::vector<ScheduledTask> {
        std::optional<ScheduledTask> deferred_task;
        {
            std::lock_guard<std::mutex> run_lock(*run.mutex);
            deferred_task = take_deferred_ready_task_locked(run);
        }
        if (deferred_task.has_value()) {
            {
                std::lock_guard<std::mutex> run_lock(*run.mutex);
                run.in_flight_tasks += 1U;
            }
            return std::vector<ScheduledTask>{*deferred_task};
        }
        std::vector<ScheduledTask> tasks = scheduler_.dequeue_ready_batch_for_run(
            run_id,
            now_ns(),
            scheduler_.parallelism()
        );
        for (const ScheduledTask& task : tasks) {
            {
                std::lock_guard<std::mutex> run_lock(*run.mutex);
                run.in_flight_tasks += 1U;
            }
        }
        return tasks;
    };

    auto execute_task_safely = [this, &run](const ScheduledTask& task) {
        TaskExecutionRecord record;
        try {
            record = execute_task(run, task);
        } catch (const std::exception& error) {
            record.task = task;
            record.started_at_ns = now_ns();
            record.ended_at_ns = record.started_at_ns;
            record.node_result.status = NodeResult::HardFail;
            record.node_result.flags = kToolFlagHandlerException;
            record.error_message = error.what();
        } catch (...) {
            record.task = task;
            record.started_at_ns = now_ns();
            record.ended_at_ns = record.started_at_ns;
            record.node_result.status = NodeResult::HardFail;
            record.node_result.flags = kToolFlagHandlerException;
            record.error_message = "unexpected task execution failure";
        }
        return record;
    };

    auto execute_dispatched_tasks = [this, &run, execute_task_safely](const std::vector<ScheduledTask>& tasks) {
        std::vector<TaskExecutionRecord> ordered_records(tasks.size());
        std::vector<std::function<void()>> jobs;
        jobs.reserve(tasks.size());

        for (std::size_t slot = 0; slot < tasks.size(); ++slot) {
            const ScheduledTask task = tasks[slot];
            jobs.push_back([&run, &ordered_records, execute_task_safely, task, slot]() {
                TaskExecutionRecord record = execute_task_safely(task);
                {
                    std::lock_guard<std::mutex> run_lock(*run.mutex);
                    if (run.in_flight_tasks > 0U) {
                        run.in_flight_tasks -= 1U;
                    }
                }
                ordered_records[slot] = std::move(record);
            });
        }

        scheduler_.run_batch(jobs);
        return ordered_records;
    };

    auto read_run_status = [&run]() {
        std::lock_guard<std::mutex> run_lock(*run.mutex);
        return std::pair{run.status, run.in_flight_tasks};
    };

    auto wait_for_run_activity = [this, run_id]() {
        const uint64_t observed_epoch = scheduler_.activity_epoch();
        const uint64_t current_now = now_ns();
        if (scheduler_.has_ready_for_run(run_id, current_now)) {
            return;
        }

        std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        const std::optional<uint64_t> next_ready_time = scheduler_.next_task_ready_time_for_run(run_id);
        if (next_ready_time.has_value()) {
            if (*next_ready_time <= current_now) {
                return;
            }
            deadline = std::min(
                deadline,
                std::chrono::steady_clock::now() +
                    std::chrono::nanoseconds(*next_ready_time - current_now)
            );
        }

        scheduler_.wait_for_activity(observed_epoch, deadline);
    };

    while (true) {
        static_cast<void>(scheduler_.promote_ready_async_tasks(now_ns()));
        bool progressed = false;
        if (single_worker_inline_lane) {
            std::optional<ScheduledTask> task = std::move(inline_followup_task);
            inline_followup_task.reset();
            if (!task.has_value()) {
                {
                    std::lock_guard<std::mutex> run_lock(*run.mutex);
                    task = take_deferred_ready_task_locked(run);
                }
            }
            if (!task.has_value()) {
                task = scheduler_.dequeue_ready_for_run(run_id, now_ns());
            }
            if (task.has_value()) {
                const StepResult step_result =
                    commit_task_execution(run_id, run, execute_task_safely(*task), &inline_followup_task);
                result.status = step_result.status;
                result.last_checkpoint_id = step_result.checkpoint_id;
                if (step_result.progressed) {
                    result.steps_executed += 1U;
                }
                progressed = step_result.progressed;
                if (step_result.status == ExecutionStatus::Running) {
                    continue;
                }
            }
        } else {
            const std::vector<ScheduledTask> dispatched_tasks = dispatch_ready_tasks();

            if (!dispatched_tasks.empty()) {
                std::vector<TaskExecutionRecord> ordered_records =
                    execute_dispatched_tasks(dispatched_tasks);
                for (TaskExecutionRecord& record : ordered_records) {
                    const StepResult step_result = commit_task_execution(run_id, run, std::move(record));
                    result.status = step_result.status;
                    result.last_checkpoint_id = step_result.checkpoint_id;
                    if (step_result.progressed) {
                        result.steps_executed += 1U;
                    }
                    progressed = progressed || step_result.progressed;
                }
            }
        }

        if (progressed && single_worker_inline_lane) {
            continue;
        }

        static_cast<void>(scheduler_.promote_ready_async_tasks(now_ns()));
        update_run_status(run, run_id);
        auto [current_status, current_in_flight] = read_run_status();
        if (current_status == ExecutionStatus::Running) {
            const uint64_t current_now = now_ns();
            if (!scheduler_.has_ready_for_run(run_id, current_now)) {
                const bool has_pending_tasks = scheduler_.has_tasks_for_run(run_id);
                const bool has_async_waiters = scheduler_.has_async_waiters_for_run(run_id);
                if (has_pending_tasks || has_async_waiters || current_in_flight != 0U) {
                    wait_for_run_activity();
                }
            }
            continue;
        }

        if (scheduler_.promote_ready_async_tasks(now_ns()) != 0U) {
            continue;
        }

        update_run_status(run, run_id);
        std::tie(current_status, current_in_flight) = read_run_status();
        if (current_status == ExecutionStatus::Running) {
            continue;
        }

        result.status = current_status;
        break;
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

    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        result.status = runs_.at(run_id).status;
    }
    result.resumed = true;
    result.message = missing_async_handles == 0U
        ? "checkpoint restored"
        : "checkpoint restored with unresolved async handles";
    return result;
}

ResumeResult ExecutionEngine::resume_run(RunId run_id) {
    ResumeResult result;
    result.run_id = run_id;

    RunRuntime* run_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        auto run_iterator = runs_.find(run_id);
        if (run_iterator == runs_.end()) {
            result.status = ExecutionStatus::Failed;
            result.message = "run not found";
            return result;
        }
        run_ptr = &run_iterator->second;
    }
    RunRuntime& run = *run_ptr;

    std::lock_guard<std::mutex> run_lock(*run.mutex);
    if (run.status != ExecutionStatus::Paused) {
        result.status = run.status;
        result.message = "run is not paused";
        return result;
    }

    scheduler_.remove_run(run_id);
    re_register_run_async_waiters(run_id, run);
    const std::size_t enqueued_tasks = enqueue_resumable_paused_branches(run_id, run);
    update_run_status_locked(run, run_id);

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

    RunRuntime* run_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        auto run_iterator = runs_.find(run_id);
        if (run_iterator == runs_.end()) {
            result.status = ExecutionStatus::Failed;
            result.message = "run not found";
            return result;
        }
        run_ptr = &run_iterator->second;
    }
    RunRuntime& run = *run_ptr;
    std::lock_guard<std::mutex> run_lock(*run.mutex);

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
    run.deferred_ready_task.reset();
    for (auto& [_, branch] : run.branches) {
        if (branch->frame.status == ExecutionStatus::Running ||
            branch->frame.status == ExecutionStatus::NotStarted) {
            branch->frame.status = ExecutionStatus::Paused;
        }
    }
    update_run_status_locked(run, run_id);

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

    BranchRuntime& checkpoint_branch = *checkpoint_branch_iterator->second;
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

    RunRuntime* run_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        auto run_iterator = runs_.find(run_id);
        if (run_iterator == runs_.end()) {
            result.message = "run not found";
            return result;
        }
        run_ptr = &run_iterator->second;
    }
    RunRuntime& run = *run_ptr;
    std::lock_guard<std::mutex> run_lock(*run.mutex);

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

    BranchRuntime& branch = *branch_iterator->second;
    const StateApplyResult apply_result = branch.state_store.apply_with_summary(patch);
    if (apply_result.knowledge_graph_delta.empty() && apply_result.intelligence_delta.empty()) {
        branch.memo_cache.invalidate(apply_result.changed_keys);
    } else {
        branch.memo_cache.clear();
    }
    uint32_t trace_flags = kTraceFlagManualStateEdit;

    std::vector<uint32_t> reactive_node_indices;
    if (!apply_result.knowledge_graph_delta.empty()) {
        const std::vector<uint32_t> knowledge_reactive_node_indices =
            collect_knowledge_reactive_node_indices(
                run.graph,
                branch.state_store.strings(),
                apply_result.knowledge_graph_delta,
                branch.frame.current_node
            );
        append_unique_node_indices(reactive_node_indices, knowledge_reactive_node_indices);
        if (!knowledge_reactive_node_indices.empty()) {
            trace_flags |= kTraceFlagKnowledgeReactive;
        }
    }
    if (!apply_result.intelligence_delta.empty()) {
        const std::vector<uint32_t> intelligence_reactive_node_indices =
            collect_intelligence_reactive_node_indices(
                run.graph,
                branch.state_store.strings(),
                branch.state_store.intelligence(),
                apply_result.intelligence_delta,
                branch.frame.current_node
            );
        append_unique_node_indices(reactive_node_indices, intelligence_reactive_node_indices);
        if (!intelligence_reactive_node_indices.empty()) {
            trace_flags |= kTraceFlagIntelligenceReactive;
        }
    }
    if (!reactive_node_indices.empty()) {
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
    std::lock_guard<std::mutex> lock(runs_mutex_);
    const auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        throw std::out_of_range("run not found");
    }
    const RunRuntime& run = run_iterator->second;
    std::lock_guard<std::mutex> run_lock(*run.mutex);
    return snapshot_run(run_id, run);
}

const WorkflowState& ExecutionEngine::state(RunId run_id, uint32_t branch_id) const {
    return state_store(run_id, branch_id).get_current_state();
}

const StateStore& ExecutionEngine::state_store(RunId run_id, uint32_t branch_id) const {
    std::lock_guard<std::mutex> lock(runs_mutex_);
    const auto run_iterator = runs_.find(run_id);
    if (run_iterator == runs_.end()) {
        throw std::out_of_range("run not found");
    }

    const RunRuntime& run = run_iterator->second;
    std::lock_guard<std::mutex> run_lock(*run.mutex);
    const auto branch_iterator = run.branches.find(branch_id);
    if (branch_iterator == run.branches.end()) {
        throw std::out_of_range("branch not found");
    }

    return branch_iterator->second->state_store;
}

const KnowledgeGraphStore& ExecutionEngine::knowledge_graph(RunId run_id, uint32_t branch_id) const {
    return state_store(run_id, branch_id).knowledge_graph();
}

const GraphDefinition& ExecutionEngine::graph(RunId run_id) const {
    std::lock_guard<std::mutex> lock(runs_mutex_);
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

void ExecutionEngine::borrow_graph_registry(const ExecutionEngine& provider) noexcept {
    borrowed_graph_provider_ = &provider;
}

void ExecutionEngine::reset_reusable_subgraph_child_state() {
    scheduler_.clear();
    checkpoint_manager_.clear();
    trace_sink_.clear();
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        runs_.clear();
    }
    graph_registry_.clear();
    borrowed_graph_provider_ = nullptr;
    borrowed_tool_registry_ = nullptr;
    borrowed_model_registry_ = nullptr;
    next_run_id_ = 1U;
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
        if (branch->pending_async.has_value()) {
            re_register_async_waiter(run_id, branch_id, *branch->pending_async);
        }

        std::vector<AsyncWaitKey> group_wait_keys;
        for (const PendingAsyncOperation& pending_async : branch->pending_async_group) {
            const AsyncWaitKey wait_key = async_wait_key_for(pending_async);
            if (!wait_key.valid() ||
                std::find(group_wait_keys.begin(), group_wait_keys.end(), wait_key) != group_wait_keys.end()) {
                continue;
            }
            group_wait_keys.push_back(wait_key);
        }
        if (!group_wait_keys.empty()) {
            scheduler_.register_async_wait_group(
                group_wait_keys,
                ScheduledTask{
                    run_id,
                    branch->frame.current_node,
                    branch_id,
                    0U
                }
            );
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
            enqueued_tasks
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

        BranchRuntime& branch = *branch_iterator->second;
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
            kImmediateTaskReadyAtNs
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
    if (run.deferred_ready_task.has_value()) {
        snapshot.pending_tasks.insert(snapshot.pending_tasks.begin(), *run.deferred_ready_task);
    }
    snapshot.next_branch_id = run.next_branch_id;
    snapshot.next_split_id = run.next_split_id;

    std::vector<uint32_t> branch_ids;
    branch_ids.reserve(run.branches.size());
    for (const auto& [branch_id, _] : run.branches) {
        branch_ids.push_back(branch_id);
    }
    std::sort(branch_ids.begin(), branch_ids.end());

    for (uint32_t branch_id : branch_ids) {
        const BranchRuntime& branch = *run.branches.at(branch_id);
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
    for (const NodeDefinition& node : runtime_graph.nodes) {
        if (node.kind != NodeKind::Subgraph || !node.subgraph.has_value()) {
            continue;
        }
        if (registered_graph(node.subgraph->graph_id) == nullptr) {
            if (error_message != nullptr) {
                *error_message =
                    "subgraph target graph is not registered: " + std::to_string(node.subgraph->graph_id);
            }
            return false;
        }
    }

    RunRuntime runtime;
    runtime.graph = std::move(runtime_graph);
    runtime.status = snapshot.status;
    runtime.runtime_config_payload = snapshot.runtime_config_payload;
    runtime.next_branch_id = snapshot.next_branch_id;
    runtime.next_split_id = snapshot.next_split_id;
    restore_subgraph_session_table(runtime.committed_subgraph_sessions, snapshot.committed_subgraph_sessions);

    for (const BranchSnapshot& branch_snapshot : snapshot.branches) {
        auto branch = std::make_shared<BranchRuntime>();
        branch->frame = branch_snapshot.frame;
        branch->state_store = branch_snapshot.state_store;
        branch->retry_count = branch_snapshot.retry_count;
        branch->pending_async = branch_snapshot.pending_async;
        branch->pending_async_group = branch_snapshot.pending_async_group;
        branch->pending_subgraph = branch_snapshot.pending_subgraph;
        branch->join_stack = branch_snapshot.join_stack;
        branch->reactive_root_node_id = branch_snapshot.reactive_root_node_id;
        if (branch->pending_subgraph.has_value()) {
            branch->last_subgraph_session_id = branch->pending_subgraph->session_id;
            branch->last_subgraph_session_revision = branch->pending_subgraph->session_revision;
        }
        runtime.branches.emplace(branch->frame.active_branch_id, std::move(branch));
    }

    for (const auto& [branch_id, branch] : runtime.branches) {
        if (!branch->pending_subgraph.has_value() || branch->pending_subgraph->session_id.empty()) {
            continue;
        }
        std::string lease_error;
        if (!acquire_subgraph_session_lease(
                runtime.active_subgraph_session_leases,
                branch->frame.current_node,
                branch->pending_subgraph->session_id,
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

    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        runs_[run_id] = std::move(runtime);
    }
    next_run_id_ = std::max(next_run_id_, run_id + 1U);
    scheduler_.remove_run(run_id);
    for (ScheduledTask task : snapshot.pending_tasks) {
        task.run_id = run_id;
        scheduler_.enqueue_task(task);
    }

    std::size_t local_missing_async_handles = 0U;
    if (restore_async_waiters) {
        for (const BranchSnapshot& branch_snapshot : snapshot.branches) {
            if (branch_snapshot.pending_async.has_value()) {
                const PendingAsyncOperation pending_async = *branch_snapshot.pending_async;
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

            std::vector<AsyncWaitKey> group_wait_keys;
            bool missing_group_handle = false;
            for (const PendingAsyncOperation& pending_async : branch_snapshot.pending_async_group) {
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

                if (!restored_async) {
                    local_missing_async_handles += 1U;
                    missing_group_handle = true;
                    continue;
                }

                const AsyncWaitKey wait_key = async_wait_key_for(pending_async);
                if (!wait_key.valid() ||
                    std::find(group_wait_keys.begin(), group_wait_keys.end(), wait_key) != group_wait_keys.end()) {
                    continue;
                }
                group_wait_keys.push_back(wait_key);
            }

            if (!missing_group_handle && !group_wait_keys.empty()) {
                scheduler_.register_async_wait_group(
                    group_wait_keys,
                    ScheduledTask{
                        run_id,
                        branch_snapshot.frame.current_node,
                        branch_snapshot.frame.active_branch_id,
                        0U
                    }
                );
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
                    kImmediateTaskReadyAtNs
                });
            }
        }
    }

    RunRuntime* final_run_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        final_run_ptr = &runs_.at(run_id);
    }
    update_run_status(*final_run_ptr, run_id);
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
    std::unique_lock<std::mutex> lock(*run.mutex);
    TaskExecutionRecord record;
    record.task = task;
    record.started_at_ns = now_ns();

    auto branch_iterator = run.branches.find(task.branch_id);
    if (branch_iterator == run.branches.end()) {
        record.node_result.status = NodeResult::HardFail;
        record.node_result.flags = kToolFlagValidationError;
        record.error_message = "branch not found";
        record.ended_at_ns = record.started_at_ns;
        return record;
    }

    const BranchPtr branch_ptr = branch_iterator->second;
    BranchRuntime& branch = *branch_ptr;
    const NodeDefinition* node = run.graph.find_node(task.node_id);
    if (node == nullptr || (node->executor == nullptr && node->kind != NodeKind::Subgraph)) {
        branch.frame.status = ExecutionStatus::Failed;
        record.node_result.status = NodeResult::HardFail;
        record.node_result.flags = kToolFlagValidationError;
        record.error_message = "node executor not found";
        record.ended_at_ns = record.started_at_ns;
        return record;
    }

    branch.frame.current_node = task.node_id;
    branch.frame.status = ExecutionStatus::Running;
    branch.scratch.reset();
    if (branch.pending_async.has_value() || !branch.pending_async_group.empty()) {
        scheduler_.remove_async_waiters_for_task(task);
    }

    if (has_node_policy(node->policy_flags, NodePolicyFlag::JoinIncomingBranches)) {
        if (!branch.join_stack.empty()) {
            const uint32_t split_id = branch.join_stack.back();
            if (run.join_scopes.find(split_id) == run.join_scopes.end()) {
                branch.frame.status = ExecutionStatus::Failed;
                record.node_result.status = NodeResult::HardFail;
                record.node_result.flags = kToolFlagValidationError;
                record.error_message = "join scope not found for branch";
                record.ended_at_ns = record.started_at_ns;
                return record;
            }

            record.ended_at_ns = now_ns();
            record.node_result = NodeResult::waiting({}, 1.0F);
            record.node_result.flags = kTraceFlagJoinBarrier;
            record.progressed = true;
            record.blocked_on_join = true;
            record.blocked_join_scope_id = split_id;
            return record;
        }
    }

    // Capture necessary state before releasing the lock
    const std::vector<std::byte> runtime_config_payload = run.runtime_config_payload;
    const GraphId graph_id = branch.frame.graph_id;
    TraceSink task_trace_sink;
    if (node->kind != NodeKind::Subgraph &&
        !branch.pending_async.has_value() &&
        branch.pending_async_group.empty()) {
        if (const std::optional<NodeResult> cached_result =
                branch.memo_cache.lookup(*node, branch.state_store.get_current_state(), runtime_config_payload);
            cached_result.has_value()) {
            record.node_result = *cached_result;
            record.ended_at_ns = now_ns();
            record.progressed = true;
            return record;
        }
    }

    lock.unlock();

    try {
        if (node->kind == NodeKind::Subgraph) {
            record.node_result = execute_subgraph_node(
                record.task.run_id,
                run,
                *node,
                branch_ptr,
                run.capture_options.capture_trace ? &record.deferred_trace_events : nullptr
            );
        } else {
            std::vector<TaskRecord> recorded_effects;
            ExecutionContext context{
                branch.state_store.get_current_state(),
                record.task.run_id,
                graph_id,
                task.node_id,
                task.branch_id,
                runtime_config_payload,
                branch.scratch,
                branch.state_store.blobs(),
                branch.state_store.strings(),
                branch.state_store.knowledge_graph(),
                branch.state_store.intelligence(),
                branch.state_store.task_journal(),
                runtime_tools(),
                runtime_models(),
                task_trace_sink,
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

    if (node->kind != NodeKind::Subgraph && run.capture_options.capture_trace) {
        record.deferred_trace_events = task_trace_sink.events();
    }

    lock.lock();
    record.ended_at_ns = now_ns();

    if (branch.cancel.is_cancelled()) {
        record.node_result.status = NodeResult::Cancelled;
    }

    if (node->kind != NodeKind::Subgraph &&
        !branch.cancel.is_cancelled()) {
        branch.memo_cache.store(
            *node,
            branch.state_store.get_current_state(),
            runtime_config_payload,
            record.node_result
        );
    }

    record.progressed = true;
    return record;
}

NodeResult ExecutionEngine::execute_subgraph_node(
    RunId run_id,
    RunRuntime& run,
    const NodeDefinition& node,
    const BranchPtr& branch_ptr,
    std::vector<TraceEvent>* deferred_trace_events
) {
    BranchRuntime& branch = *branch_ptr;
    if (runtime_debug_enabled()) {
        runtime_debug_log(
            "execute_subgraph_node:start parent_run=" + std::to_string(run_id) +
            " node=" + std::to_string(node.id) +
            " branch=" + std::to_string(branch.frame.active_branch_id) +
            " pending_async=" + std::to_string(branch.pending_async.has_value() ? branch.pending_async->handle_id : 0U) +
            " pending_group=" + std::to_string(branch.pending_async_group.size()) +
            " has_pending_subgraph=" + std::to_string(branch.pending_subgraph.has_value() ? 1 : 0)
        );
    }
    if (!node.subgraph.has_value() || !node.subgraph->valid()) {
        throw std::runtime_error("subgraph node is missing a valid binding");
    }

    const GraphDefinition* target_graph = registered_graph(node.subgraph->graph_id);
    if (target_graph == nullptr) {
        throw std::runtime_error("subgraph target graph is not registered");
    }

    const bool persistent_session = is_persistent_subgraph_binding(*node.subgraph);
    std::string active_session_id;
    uint64_t active_session_revision = 0U;
    std::shared_ptr<RunSnapshot> committed_session_snapshot;
    std::shared_ptr<StateStore> committed_session_projection;
    std::vector<std::byte> committed_session_snapshot_bytes;
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
            const SubgraphSessionRecord* committed_session = lookup_committed_subgraph_session(
                run.committed_subgraph_sessions,
                node.id,
                active_session_id
            );
            active_session_revision = committed_session == nullptr
                ? 1U
                : committed_session->session_revision + 1U;
            if (committed_session != nullptr) {
                committed_session_snapshot = committed_session->snapshot;
                committed_session_projection = committed_session->projected_state;
                if (!committed_session_snapshot &&
                    !committed_session_projection &&
                    !committed_session->snapshot_bytes.empty()) {
                    committed_session_snapshot_bytes = committed_session->snapshot_bytes;
                }
            }
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

    SubgraphChildEnginePool::Lease child_engine_lease;
    ExecutionEngine* child_engine_ptr = nullptr;
    auto ensure_child_engine = [&]() -> ExecutionEngine& {
        if (child_engine_ptr == nullptr) {
            child_engine_lease = subgraph_engine_pool_->acquire(*this);
            child_engine_ptr = &*child_engine_lease;
        }
        return *child_engine_ptr;
    };

    auto import_child_trace = [&](std::vector<TraceEvent> child_trace, float default_confidence = 1.0F) -> float {
        if (child_trace.empty()) {
            return default_confidence;
        }

        const float confidence = child_trace.back().confidence;
        if (deferred_trace_events == nullptr) {
            return confidence;
        }

        const ExecutionNamespaceRef prefix{
            run.graph.id,
            node.id,
            active_session_id,
            active_session_revision
        };
        for (TraceEvent& child_event : child_trace) {
            child_event.run_id = run_id;
            if (child_event.session_id.empty()) {
                child_event.session_id = active_session_id;
                child_event.session_revision = active_session_revision;
            }
            child_event.namespace_path = prefix_namespace_path(prefix, child_event.namespace_path);
        }
        deferred_trace_events->insert(
            deferred_trace_events->end(),
            std::make_move_iterator(child_trace.begin()),
            std::make_move_iterator(child_trace.end())
        );
        return confidence;
    };

    try {
        auto materialize_committed_session_projection = [&]() -> bool {
            if (committed_session_projection != nullptr ||
                (committed_session_snapshot == nullptr && committed_session_snapshot_bytes.empty())) {
                return true;
            }

            const RunSnapshot committed_snapshot = committed_session_snapshot != nullptr
                ? *committed_session_snapshot
                : deserialize_run_snapshot_bytes(committed_session_snapshot_bytes);
            std::string projection_error;
            const std::optional<StateStore> committed_projection =
                project_subgraph_session_state(committed_snapshot, &projection_error);
            if (!committed_projection.has_value()) {
                release_active_session_lease();
                return false;
            }
            committed_session_projection = std::make_shared<StateStore>(*committed_projection);
            return true;
        };

        if (!branch.pending_subgraph.has_value() &&
            inline_single_branch_subgraph_eligible(*target_graph)) {
            if (runtime_debug_enabled()) {
                runtime_debug_log(
                    "execute_subgraph_node:inline child run parent_run=" + std::to_string(run_id) +
                    " child_graph=" + std::to_string(target_graph->id)
                );
            }

            StateStore inline_child_state(infer_subgraph_initial_field_count(*node.subgraph));
            bool seed_knowledge_graph = node.subgraph->propagate_knowledge_graph;
            if (committed_session_snapshot != nullptr ||
                committed_session_projection != nullptr ||
                !committed_session_snapshot_bytes.empty()) {
                if (!materialize_committed_session_projection()) {
                    NodeResult result;
                    result.status = NodeResult::HardFail;
                    result.flags = kToolFlagValidationError;
                    return result;
                }
                inline_child_state = *committed_session_projection;
                seed_knowledge_graph = false;
            }

            const StatePatch inline_input_patch = build_subgraph_input_patch(
                branch.state_store,
                *node.subgraph,
                inline_child_state,
                seed_knowledge_graph
            );
            if (!inline_input_patch.empty()) {
                static_cast<void>(inline_child_state.apply(inline_input_patch));
            }

            InlineSingleBranchSubgraphResult inline_result = run_inline_single_branch_subgraph(
                *target_graph,
                target_graph->entry,
                std::move(inline_child_state),
                run.runtime_config_payload,
                runtime_tools(),
                runtime_models(),
                run.capture_options.capture_trace
            );
            const float confidence =
                import_child_trace(std::move(inline_result.trace_events), inline_result.confidence);

            if (inline_result.outcome == InlineSingleBranchSubgraphOutcome::Waiting) {
                const RunSnapshot child_snapshot = build_inline_single_branch_subgraph_snapshot(
                    *target_graph,
                    run.runtime_config_payload,
                    inline_result.state_store,
                    inline_result.frame,
                    inline_result.retry_count,
                    inline_result.pending_async,
                    runtime_tools(),
                    runtime_models()
                );
                branch.pending_subgraph = PendingSubgraphExecution{
                    kInlineSubgraphResumableRunId,
                    serialize_run_snapshot_bytes(child_snapshot),
                    active_session_id,
                    active_session_revision
                };
                branch.pending_async_group.clear();

                NodeResult result = NodeResult::waiting({}, confidence);
                if (inline_result.pending_async.has_value()) {
                    result.pending_async = inline_result.pending_async;
                }
                if (runtime_debug_enabled()) {
                    runtime_debug_log(
                        "execute_subgraph_node:inline waiting parent_run=" + std::to_string(run_id) +
                        " child_run=" + std::to_string(kInlineSubgraphResumableRunId) +
                        " bubbled_waits=" + std::to_string(result.pending_async.has_value() ? 1U : 0U)
                    );
                }
                return result;
            }

            branch.pending_subgraph.reset();
            branch.pending_async_group.clear();

            if (inline_result.outcome == InlineSingleBranchSubgraphOutcome::Failed) {
                release_active_session_lease();
                NodeResult result;
                result.status = NodeResult::HardFail;
                result.confidence = confidence;
                result.flags = kToolFlagValidationError;
                return result;
            }
            if (inline_result.outcome == InlineSingleBranchSubgraphOutcome::Cancelled) {
                release_active_session_lease();
                NodeResult result;
                result.status = NodeResult::Cancelled;
                result.confidence = confidence;
                return result;
            }
            if (inline_result.outcome != InlineSingleBranchSubgraphOutcome::Completed) {
                throw std::runtime_error("inline subgraph execution returned an unsupported outcome");
            }

            if (persistent_session) {
                const RunSnapshot child_snapshot = build_inline_single_branch_subgraph_snapshot(
                    *target_graph,
                    run.runtime_config_payload,
                    inline_result.state_store,
                    inline_result.frame,
                    inline_result.retry_count,
                    std::nullopt,
                    runtime_tools(),
                    runtime_models()
                );
                std::lock_guard<std::mutex> session_guard(*run.subgraph_session_mutex);
                store_committed_subgraph_session(
                    run.committed_subgraph_sessions,
                    node.id,
                    active_session_id,
                    active_session_revision,
                    {},
                    std::make_shared<RunSnapshot>(child_snapshot),
                    std::make_shared<StateStore>(inline_result.state_store)
                );
                release_subgraph_session_lease(
                    run.active_subgraph_session_leases,
                    node.id,
                    active_session_id,
                    branch.frame.active_branch_id
                );
            }

            StatePatch patch = build_subgraph_output_patch(
                inline_result.state_store,
                *node.subgraph,
                branch.state_store
            );
            return NodeResult::success(std::move(patch), confidence);
        }

        RunId child_run_id = 0U;
        if (branch.pending_subgraph.has_value()) {
            if (runtime_debug_enabled()) {
                runtime_debug_log(
                    "execute_subgraph_node:restore child snapshot parent_run=" + std::to_string(run_id)
                );
            }
            if (!branch.pending_subgraph->valid()) {
                throw std::runtime_error("pending subgraph snapshot is invalid");
            }

            ExecutionEngine& child_engine = ensure_child_engine();
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
            if (runtime_debug_enabled()) {
                runtime_debug_log(
                    "execute_subgraph_node:start child run parent_run=" + std::to_string(run_id) +
                    " child_graph=" + std::to_string(target_graph->id)
                );
            }

            ExecutionEngine& child_engine = ensure_child_engine();
            InputEnvelope child_input;
            child_input.initial_field_count = infer_subgraph_initial_field_count(*node.subgraph);
            child_input.runtime_config_payload = run.runtime_config_payload;

            child_run_id = child_engine.start(*target_graph, child_input, run.capture_options);
            if (runtime_debug_enabled()) {
                runtime_debug_log(
                    "execute_subgraph_node:child run started parent_run=" + std::to_string(run_id) +
                    " child_run=" + std::to_string(child_run_id)
                );
            }
            BranchRuntime& child_root_branch = *child_engine.runs_.at(child_run_id).branches.at(0U);
            bool seed_knowledge_graph = node.subgraph->propagate_knowledge_graph;
            if (committed_session_snapshot != nullptr ||
                committed_session_projection != nullptr ||
                !committed_session_snapshot_bytes.empty()) {
                if (!materialize_committed_session_projection()) {
                    NodeResult result;
                    result.status = NodeResult::HardFail;
                    result.flags = kToolFlagValidationError;
                    return result;
                }
                child_root_branch.state_store = *committed_session_projection;
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

        if (runtime_debug_enabled()) {
            runtime_debug_log(
                "execute_subgraph_node:enter child loop parent_run=" + std::to_string(run_id) +
                " child_run=" + std::to_string(child_run_id)
            );
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
            ExecutionEngine& child_engine = ensure_child_engine();
            RunRuntime& child_runtime = child_engine.runs_.at(child_run_id);
            bool resumed_branch = false;
            for (auto& [child_branch_id, child_branch] : child_runtime.branches) {
                if (!child_branch->pending_async.has_value()) {
                    continue;
                }

                const bool matched_ready_async = std::any_of(
                    ready_asyncs.begin(),
                    ready_asyncs.end(),
                    [&child_branch](const PendingAsyncOperation& ready_async) {
                        return pending_async_equal(*child_branch->pending_async, ready_async);
                    }
                );
                if (!matched_ready_async) {
                    continue;
                }

                child_engine.scheduler_.enqueue_task(ScheduledTask{
                    child_run_id,
                    child_branch->frame.current_node,
                    child_branch_id,
                    kImmediateTaskReadyAtNs
                });
                child_branch->frame.status = ExecutionStatus::Running;
                resumed_branch = true;
            }
            return resumed_branch;
        };

        auto snapshot_waiting_child = [&]() -> NodeResult {
            ExecutionEngine& child_engine = ensure_child_engine();
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
            const float confidence =
                import_child_trace(child_engine.trace().take_events_for_run(child_run_id));

            branch.pending_async_group.clear();
            std::vector<PendingAsyncOperation> bubbled_waits;
            for (const BranchSnapshot& child_branch_snapshot : child_snapshot.branches) {
                for (const PendingAsyncOperation& pending_async :
                     collect_pending_async_operations(child_branch_snapshot)) {
                    append_unique_pending_async(bubbled_waits, pending_async);
                }
            }

            NodeResult result = NodeResult::waiting({}, confidence);
            if (bubbled_waits.size() == 1U) {
                result.pending_async = bubbled_waits.front();
            } else if (!bubbled_waits.empty()) {
                branch.pending_async_group = std::move(bubbled_waits);
            }
            if (runtime_debug_enabled()) {
                runtime_debug_log(
                    "execute_subgraph_node:waiting parent_run=" + std::to_string(run_id) +
                    " child_run=" + std::to_string(child_run_id) +
                    " child_status=" + std::to_string(static_cast<uint32_t>(child_runtime.status)) +
                    " bubbled_waits=" + std::to_string(
                        result.pending_async.has_value() ? 1U : branch.pending_async_group.size()
                    )
                );
            }
            return result;
        };

        if (branch.pending_async.has_value()) {
            if (!pending_async_ready(*branch.pending_async)) {
                NodeResult result = NodeResult::waiting({}, 1.0F);
                result.pending_async = branch.pending_async;
                return result;
            }

            ExecutionEngine& child_engine = ensure_child_engine();
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

            if (ready_asyncs.size() != branch.pending_async_group.size()) {
                return NodeResult::waiting({}, 1.0F);
            }

            ExecutionEngine& child_engine = ensure_child_engine();
            if (!resume_child_branches(ready_asyncs) &&
                !child_engine.scheduler_.has_tasks_for_run(child_run_id)) {
                throw std::runtime_error(
                    "nested subgraph multi-wait resume found no runnable continuation branch"
                );
            }
            branch.pending_async_group.clear();
        }

        ExecutionEngine& child_engine = ensure_child_engine();
        while (true) {
            const StepResult child_step = child_engine.step(child_run_id);
            if (runtime_debug_enabled()) {
                runtime_debug_log(
                    "execute_subgraph_node:child_step parent_run=" + std::to_string(run_id) +
                    " child_run=" + std::to_string(child_run_id) +
                    " progressed=" + std::to_string(child_step.progressed ? 1 : 0) +
                    " waiting=" + std::to_string(child_step.waiting ? 1 : 0) +
                    " status=" + std::to_string(static_cast<uint32_t>(child_step.status)) +
                    " node_status=" + std::to_string(static_cast<uint32_t>(child_step.node_status)) +
                    " checkpoint=" + std::to_string(child_step.checkpoint_id)
                );
            }
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
        const float confidence =
            import_child_trace(child_engine.trace().take_events_for_run(child_run_id));
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
                {},
                std::make_shared<RunSnapshot>(child_snapshot),
                std::make_shared<StateStore>(*projected_child_state)
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

StepResult ExecutionEngine::commit_task_execution(
    RunId run_id,
    RunRuntime& run,
    TaskExecutionRecord record,
    std::optional<ScheduledTask>* inline_followup_task
) {
    std::lock_guard<std::mutex> lock(*run.mutex);
    const NodeDefinition* node_ptr = run.graph.find_node(record.task.node_id);
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

    BranchRuntime& branch = *branch_iterator->second;
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
    auto flush_inline_followup_task = [this, inline_followup_task]() {
        if (inline_followup_task != nullptr && inline_followup_task->has_value()) {
            scheduler_.enqueue_task(**inline_followup_task);
            inline_followup_task->reset();
        }
    };
    auto schedule_task = [this, run_id, inline_followup_task, &enqueued_tasks, &flush_inline_followup_task](
                             ScheduledTask task
                         ) {
        const bool can_capture_inline =
            inline_followup_task != nullptr &&
            task.run_id == run_id &&
            task.ready_at_ns == kImmediateTaskReadyAtNs &&
            !inline_followup_task->has_value() &&
            enqueued_tasks == 0U &&
            !scheduler_.has_tasks_for_run(run_id);
        if (can_capture_inline) {
            *inline_followup_task = task;
            ++enqueued_tasks;
            return;
        }
        flush_inline_followup_task();
        scheduler_.enqueue_task(task);
        ++enqueued_tasks;
    };

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

                if (arrived_branch_iterator->second->reactive_root_node_id.has_value()) {
                    if (!merged_reactive_root_node_id.has_value()) {
                        merged_reactive_root_node_id = arrived_branch_iterator->second->reactive_root_node_id;
                    } else if (*merged_reactive_root_node_id !=
                               *arrived_branch_iterator->second->reactive_root_node_id) {
                        branch.frame.status = ExecutionStatus::Failed;
                        run.status = ExecutionStatus::Failed;
                        result.status = ExecutionStatus::Failed;
                        result.message = "join scope attempted to merge multiple reactive roots";
                        return result;
                    }
                }

                replay_branch_knowledge_graph_delta(
                    arrived_branch_iterator->second->state_store,
                    join_scope.split_patch_log_offset,
                    merged_state
                );
                merged_step_index = std::max(
                    merged_step_index,
                    arrived_branch_iterator->second->frame.step_index
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

            BranchRuntime& survivor_branch = *run.branches.at(survivor_branch_id);
            survivor_branch.state_store = std::move(merged_state);
            survivor_branch.pending_async.reset();
            survivor_branch.pending_async_group.clear();
            survivor_branch.pending_subgraph.reset();
            survivor_branch.memo_cache.clear();
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
            schedule_task(ScheduledTask{
                run_id,
                record.task.node_id,
                survivor_branch_id,
                kImmediateTaskReadyAtNs
            });
            checkpoint_branch_id = survivor_branch_id;
            checkpoint_branch = run.branches.at(survivor_branch_id).get();
            trace_flags |= kTraceFlagJoinMerged;
        } else {
            checkpoint_patch_offset = static_cast<uint64_t>(checkpoint_branch->state_store.patch_log().size());
        }
    } else {
        const StateApplyResult apply_result = branch.state_store.apply_with_summary(record.node_result.patch);
        if (apply_result.knowledge_graph_delta.empty() && apply_result.intelligence_delta.empty()) {
            branch.memo_cache.invalidate(apply_result.changed_keys);
        } else {
            branch.memo_cache.clear();
        }
        checkpoint_patch_offset = apply_result.patch_log_offset;
        std::vector<uint32_t> reactive_node_indices;
        if (!apply_result.knowledge_graph_delta.empty()) {
            const std::vector<uint32_t> knowledge_reactive_node_indices =
                collect_knowledge_reactive_node_indices(
                    run.graph,
                    branch.state_store.strings(),
                    apply_result.knowledge_graph_delta,
                    record.task.node_id
                );
            append_unique_node_indices(reactive_node_indices, knowledge_reactive_node_indices);
            if (!knowledge_reactive_node_indices.empty()) {
                trace_flags |= kTraceFlagKnowledgeReactive;
            }
        }
        if (!apply_result.intelligence_delta.empty()) {
            const std::vector<uint32_t> intelligence_reactive_node_indices =
                collect_intelligence_reactive_node_indices(
                    run.graph,
                    branch.state_store.strings(),
                    branch.state_store.intelligence(),
                    apply_result.intelligence_delta,
                    record.task.node_id
                );
            append_unique_node_indices(reactive_node_indices, intelligence_reactive_node_indices);
            if (!intelligence_reactive_node_indices.empty()) {
                trace_flags |= kTraceFlagIntelligenceReactive;
            }
        }

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
                re_register_async_waiter_locked(
                    run_id,
                    run,
                    record.task.branch_id,
                    *branch.pending_async
                );
            }
            if (!branch.pending_async_group.empty()) {
                std::vector<AsyncWaitKey> wait_group_keys;
                wait_group_keys.reserve(branch.pending_async_group.size());
                for (const PendingAsyncOperation& pending_async : branch.pending_async_group) {
                    const AsyncWaitKey wait_key = async_wait_key_for(pending_async);
                    if (!wait_key.valid() ||
                        std::find(wait_group_keys.begin(), wait_group_keys.end(), wait_key) !=
                            wait_group_keys.end()) {
                        continue;
                    }
                    wait_group_keys.push_back(wait_key);
                }
                if (!wait_group_keys.empty()) {
                    scheduler_.register_async_wait_group(
                        wait_group_keys,
                        ScheduledTask{
                            run_id,
                            branch.frame.current_node,
                            record.task.branch_id,
                            0U
                        }
                    );
                }
            }
        } else if ((record.node_result.status == NodeResult::SoftFail ||
                    record.node_result.status == NodeResult::HardFail) &&
                   branch.retry_count < node->retry_limit) {
            branch.pending_async.reset();
            branch.pending_async_group.clear();
            branch.pending_subgraph.reset();
            branch.retry_count += 1U;
            branch.frame.status = ExecutionStatus::Running;
            schedule_task(ScheduledTask{
                run_id,
                record.task.node_id,
                record.task.branch_id,
                kImmediateTaskReadyAtNs
            });
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
                    schedule_task(ScheduledTask{
                        run_id,
                        *record.node_result.next_override,
                        record.task.branch_id,
                        kImmediateTaskReadyAtNs
                    });
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
                    schedule_task(ScheduledTask{
                        run_id,
                        primary_edge->to,
                        record.task.branch_id,
                        kImmediateTaskReadyAtNs
                    });

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
                            auto cloned_branch = std::make_shared<BranchRuntime>();
                            cloned_branch->frame = branch_frame_template;
                            cloned_branch->frame.active_branch_id = run.next_branch_id;
                            cloned_branch->state_store = branch_state_template;
                            cloned_branch->retry_count = branch_retry_template;
                            cloned_branch->join_stack = branch_join_stack_template;
                            cloned_branch->reactive_root_node_id = branch_reactive_root_template;
                            cloned_branch->memo_cache = branch.memo_cache;
                            cloned_branch->last_subgraph_session_id = branch_subgraph_session_id_template;
                            cloned_branch->last_subgraph_session_revision =
                                branch_subgraph_session_revision_template;
                            run.branches.emplace(run.next_branch_id, std::move(cloned_branch));
                            schedule_task(ScheduledTask{
                                run_id,
                                next_edges[index]->to,
                                run.next_branch_id,
                                kImmediateTaskReadyAtNs
                            });
                            ++run.next_branch_id;
                        }
                    }
                }
            }
        }

        if (run.status != ExecutionStatus::Failed && !reactive_node_indices.empty()) {
            flush_inline_followup_task();
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
                    enqueued_tasks
                );
            }
        }

        rebuild_reactive_frontier_state(run);
        flush_inline_followup_task();
        finalize_reactive_frontier_for_branch(run_id, run, branch, enqueued_tasks, trace_flags);
    }

    rebuild_reactive_frontier_state(run);

    checkpoint_branch = run.branches.at(checkpoint_branch_id).get();

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

    update_run_status_locked(run, run_id);

    CheckpointId checkpoint_id = 0U;
    if (run.capture_options.capture_checkpoints) {
        checkpoint_id = static_cast<CheckpointId>(checkpoint_manager_.size() + 1U);
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
    } else {
        checkpoint_branch->frame.checkpoint_id = 0U;
    }

    if (run.capture_options.capture_trace) {
        if (!record.deferred_trace_events.empty()) {
            trace_sink_.emit_batch(std::move(record.deferred_trace_events));
        }

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
    }

    result.checkpoint_id = checkpoint_id;
    result.enqueued_tasks = enqueued_tasks;
    result.waiting = (record.node_result.status == NodeResult::Waiting) ||
        (run.status == ExecutionStatus::Paused) ||
        scheduler_.has_async_waiters_for_run(run_id);
    result.status = run.status;
    return result;
}

void ExecutionEngine::discard_run(RunId run_id) {
    scheduler_.remove_run(run_id);
    static_cast<void>(trace_sink_.take_events_for_run(run_id));
    std::lock_guard<std::mutex> lock(runs_mutex_);
    runs_.erase(run_id);
}

bool ExecutionEngine::has_deferred_ready_task_locked(const RunRuntime& run) const noexcept {
    return run.deferred_ready_task.has_value();
}

std::optional<ScheduledTask> ExecutionEngine::take_deferred_ready_task_locked(RunRuntime& run) {
    std::optional<ScheduledTask> task = std::move(run.deferred_ready_task);
    run.deferred_ready_task.reset();
    return task;
}

void ExecutionEngine::update_run_status(RunRuntime& run, RunId run_id) {
    std::lock_guard<std::mutex> run_lock(*run.mutex);
    update_run_status_locked(run, run_id);
}

void ExecutionEngine::update_run_status_locked(RunRuntime& run, RunId run_id) {
    if (is_terminal_status(run.status)) {
        return;
    }

    const bool has_pending_tasks = has_deferred_ready_task_locked(run) || scheduler_.has_tasks_for_run(run_id);
    const bool has_async_waiters = scheduler_.has_async_waiters_for_run(run_id);
    if (run.branches.size() == 1U) {
        const ExecutionStatus branch_status = run.branches.begin()->second->frame.status;
        if (branch_status == ExecutionStatus::Failed && !has_pending_tasks) {
            run.status = ExecutionStatus::Failed;
            return;
        }
        if (has_pending_tasks ||
            branch_status == ExecutionStatus::Running ||
            branch_status == ExecutionStatus::NotStarted ||
            has_async_waiters ||
            run.in_flight_tasks > 0U) {
            run.status = ExecutionStatus::Running;
            return;
        }
        run.status = branch_status;
        return;
    }

    bool has_failed_branch = false;
    bool has_paused_branch = false;
    bool has_completed_branch = false;
    bool has_cancelled_branch = false;
    bool has_active_branch = false;

    for (const auto& [_, branch] : run.branches) {
        switch (branch->frame.status) {
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

    if (has_failed_branch && !has_pending_tasks) {
        run.status = ExecutionStatus::Failed;
    } else if (has_pending_tasks || has_active_branch || has_async_waiters || run.in_flight_tasks > 0U) {
        run.status = ExecutionStatus::Running;
    } else if (has_paused_branch) {
        run.status = ExecutionStatus::Paused;
    } else if (has_completed_branch) {
        run.status = ExecutionStatus::Completed;
    } else if (has_cancelled_branch) {
        run.status = ExecutionStatus::Cancelled;
    } else {
        run.status = ExecutionStatus::NotStarted;
    }
}

GraphDefinition ExecutionEngine::resolve_runtime_graph(const RunSnapshot& snapshot) const {
    if (const GraphDefinition* runtime_graph = registered_graph(snapshot.graph.id);
        runtime_graph != nullptr) {
        return *runtime_graph;
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

    if (borrowed_graph_provider_ != nullptr) {
        if (const GraphDefinition* borrowed = borrowed_graph_provider_->registered_graph(graph_id);
            borrowed != nullptr) {
            return borrowed;
        }
    }

    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        for (const auto& [_, run] : runs_) {
            if (run.graph.id == graph_id) {
                return &run.graph;
            }
        }
    }

    return nullptr;
}

void ExecutionEngine::re_register_async_waiter(
    RunId run_id,
    uint32_t branch_id,
    const PendingAsyncOperation& pending_async
) {
    RunRuntime* run_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(runs_mutex_);
        auto run_iterator = runs_.find(run_id);
        if (run_iterator == runs_.end()) {
            return;
        }
        run_ptr = &run_iterator->second;
    }
    std::lock_guard<std::mutex> run_lock(*run_ptr->mutex);
    re_register_async_waiter_locked(run_id, *run_ptr, branch_id, pending_async);
}

void ExecutionEngine::re_register_async_waiter_locked(
    RunId run_id,
    const RunRuntime& run,
    uint32_t branch_id,
    const PendingAsyncOperation& pending_async
) {
    const auto branch_iterator = run.branches.find(branch_id);
    if (branch_iterator == run.branches.end()) {
        return;
    }

    const ScheduledTask task{
        run_id,
        branch_iterator->second->frame.current_node,
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
