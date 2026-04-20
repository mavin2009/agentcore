#include "agentcore/execution/reactive/memoization.h"

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace agentcore {

namespace {

bool contains_changed_key(
    const std::vector<StateKey>& read_keys,
    const std::vector<StateKey>& changed_keys
) {
    for (StateKey read_key : read_keys) {
        if (std::find(changed_keys.begin(), changed_keys.end(), read_key) != changed_keys.end()) {
            return true;
        }
    }
    return false;
}

bool same_dependency_signature(
    const NodeMemoEntry& entry,
    uint64_t runtime_config_digest,
    const std::vector<StateKey>& read_keys,
    const std::vector<uint64_t>& read_revisions
) {
    return entry.runtime_config_digest == runtime_config_digest &&
        entry.read_keys == read_keys &&
        entry.read_revisions == read_revisions;
}

} // namespace

bool node_supports_deterministic_memoization(const NodeDefinition& node) noexcept {
    if (!node.memoization.enabled()) {
        return false;
    }

    switch (node.kind) {
        case NodeKind::Compute:
        case NodeKind::Control:
        case NodeKind::Aggregate:
            return !has_node_policy(node.policy_flags, NodePolicyFlag::ReactToKnowledgeGraph);
        case NodeKind::Tool:
        case NodeKind::Model:
        case NodeKind::Human:
        case NodeKind::Subgraph:
            return false;
    }

    return false;
}

bool node_result_is_memoizable(const NodeResult& result) noexcept {
    return result.status == NodeResult::Success &&
        !result.pending_async.has_value();
}

uint64_t DeterministicNodeMemoCache::digest_runtime_config(
    const std::vector<std::byte>& runtime_config_payload
) noexcept {
    uint64_t hash = 1469598103934665603ULL;
    for (const std::byte byte : runtime_config_payload) {
        hash ^= static_cast<uint64_t>(std::to_integer<unsigned char>(byte));
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<uint64_t> DeterministicNodeMemoCache::capture_revisions(
    const WorkflowState& state,
    const std::vector<StateKey>& read_keys
) {
    std::vector<uint64_t> read_revisions;
    read_revisions.reserve(read_keys.size());
    for (StateKey key : read_keys) {
        read_revisions.push_back(state.field_revision(key));
    }
    return read_revisions;
}

std::optional<NodeResult> DeterministicNodeMemoCache::lookup(
    const NodeDefinition& node,
    const WorkflowState& state,
    const std::vector<std::byte>& runtime_config_payload
) const {
    if (!node_supports_deterministic_memoization(node)) {
        return std::nullopt;
    }

    const auto iterator = entries_.find(node.id);
    if (iterator == entries_.end()) {
        return std::nullopt;
    }

    const uint64_t runtime_config_digest = digest_runtime_config(runtime_config_payload);
    const std::vector<uint64_t> read_revisions =
        capture_revisions(state, node.memoization.read_keys);
    for (const NodeMemoEntry& entry : iterator->second) {
        if (same_dependency_signature(
                entry,
                runtime_config_digest,
                node.memoization.read_keys,
                read_revisions)) {
            return entry.result;
        }
    }
    return std::nullopt;
}

void DeterministicNodeMemoCache::store(
    const NodeDefinition& node,
    const WorkflowState& state,
    const std::vector<std::byte>& runtime_config_payload,
    const NodeResult& result
) {
    if (!node_supports_deterministic_memoization(node) ||
        !node_result_is_memoizable(result)) {
        return;
    }

    const uint64_t runtime_config_digest = digest_runtime_config(runtime_config_payload);
    const std::vector<uint64_t> read_revisions =
        capture_revisions(state, node.memoization.read_keys);

    std::vector<NodeMemoEntry>& node_entries = entries_[node.id];
    auto existing = std::find_if(
        node_entries.begin(),
        node_entries.end(),
        [&](const NodeMemoEntry& entry) {
            return same_dependency_signature(
                entry,
                runtime_config_digest,
                node.memoization.read_keys,
                read_revisions
            );
        }
    );
    if (existing != node_entries.end()) {
        existing->result = result;
        return;
    }

    node_entries.push_back(NodeMemoEntry{
        runtime_config_digest,
        node.memoization.read_keys,
        std::move(read_revisions),
        result
    });

    const std::size_t max_entries = std::max<std::size_t>(1U, node.memoization.max_entries);
    if (node_entries.size() > max_entries) {
        node_entries.erase(node_entries.begin());
    }
}

void DeterministicNodeMemoCache::invalidate(const std::vector<StateKey>& changed_keys) {
    if (changed_keys.empty()) {
        return;
    }

    for (auto iterator = entries_.begin(); iterator != entries_.end();) {
        std::vector<NodeMemoEntry>& node_entries = iterator->second;
        node_entries.erase(
            std::remove_if(
                node_entries.begin(),
                node_entries.end(),
                [&](const NodeMemoEntry& entry) {
                    return contains_changed_key(entry.read_keys, changed_keys);
                }
            ),
            node_entries.end()
        );
        if (node_entries.empty()) {
            iterator = entries_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

void DeterministicNodeMemoCache::clear() noexcept {
    entries_.clear();
}

std::size_t DeterministicNodeMemoCache::size_for_node(NodeId node_id) const noexcept {
    const auto iterator = entries_.find(node_id);
    return iterator == entries_.end() ? 0U : iterator->second.size();
}

} // namespace agentcore
