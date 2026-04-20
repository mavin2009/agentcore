#ifndef AGENTCORE_EXECUTION_REACTIVE_MEMOIZATION_H
#define AGENTCORE_EXECUTION_REACTIVE_MEMOIZATION_H

#include "agentcore/graph/graph_ir.h"
#include "agentcore/runtime/node_runtime.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace agentcore {

struct NodeMemoEntry {
    uint64_t runtime_config_digest{0U};
    std::vector<StateKey> read_keys;
    std::vector<uint64_t> read_revisions;
    NodeResult result{};
};

class DeterministicNodeMemoCache {
public:
    [[nodiscard]] std::optional<NodeResult> lookup(
        const NodeDefinition& node,
        const WorkflowState& state,
        const std::vector<std::byte>& runtime_config_payload
    ) const;

    void store(
        const NodeDefinition& node,
        const WorkflowState& state,
        const std::vector<std::byte>& runtime_config_payload,
        const NodeResult& result
    );

    void invalidate(const std::vector<StateKey>& changed_keys);
    void clear() noexcept;
    [[nodiscard]] std::size_t size_for_node(NodeId node_id) const noexcept;

private:
    [[nodiscard]] static uint64_t digest_runtime_config(const std::vector<std::byte>& runtime_config_payload) noexcept;
    [[nodiscard]] static std::vector<uint64_t> capture_revisions(
        const WorkflowState& state,
        const std::vector<StateKey>& read_keys
    );

    std::unordered_map<NodeId, std::vector<NodeMemoEntry>> entries_;
};

[[nodiscard]] bool node_supports_deterministic_memoization(const NodeDefinition& node) noexcept;
[[nodiscard]] bool node_result_is_memoizable(const NodeResult& result) noexcept;

} // namespace agentcore

#endif // AGENTCORE_EXECUTION_REACTIVE_MEMOIZATION_H
