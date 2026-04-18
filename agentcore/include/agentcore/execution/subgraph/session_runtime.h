#ifndef AGENTCORE_EXECUTION_SUBGRAPH_SESSION_RUNTIME_H
#define AGENTCORE_EXECUTION_SUBGRAPH_SESSION_RUNTIME_H

#include "agentcore/execution/checkpoint.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentcore {

struct SubgraphSessionRecord {
    std::string session_id;
    uint64_t session_revision{0};
    std::vector<std::byte> snapshot_bytes;
};

using SubgraphSessionShard = std::unordered_map<std::string, SubgraphSessionRecord>;
using SubgraphSessionTable = std::unordered_map<NodeId, SubgraphSessionShard>;
using SubgraphSessionLeaseShard = std::unordered_map<std::string, uint32_t>;
using SubgraphSessionLeaseTable = std::unordered_map<NodeId, SubgraphSessionLeaseShard>;

[[nodiscard]] bool is_persistent_subgraph_binding(const SubgraphBinding& binding) noexcept;

[[nodiscard]] std::optional<std::string> resolve_subgraph_session_id(
    const StateStore& parent_state,
    const SubgraphBinding& binding,
    std::string* error_message
);

[[nodiscard]] const SubgraphSessionRecord* lookup_committed_subgraph_session(
    const SubgraphSessionTable& session_table,
    NodeId parent_node_id,
    std::string_view session_id
);

void store_committed_subgraph_session(
    SubgraphSessionTable& session_table,
    NodeId parent_node_id,
    std::string session_id,
    uint64_t session_revision,
    std::vector<std::byte> snapshot_bytes
);

bool acquire_subgraph_session_lease(
    SubgraphSessionLeaseTable& lease_table,
    NodeId parent_node_id,
    std::string_view session_id,
    uint32_t branch_id,
    std::string* error_message
);

void release_subgraph_session_lease(
    SubgraphSessionLeaseTable& lease_table,
    NodeId parent_node_id,
    std::string_view session_id,
    uint32_t branch_id
);

[[nodiscard]] std::vector<CommittedSubgraphSessionSnapshot> flatten_subgraph_session_table(
    const SubgraphSessionTable& session_table
);

void restore_subgraph_session_table(
    SubgraphSessionTable& session_table,
    const std::vector<CommittedSubgraphSessionSnapshot>& snapshots
);

[[nodiscard]] std::optional<StateStore> project_subgraph_session_state(
    const RunSnapshot& snapshot,
    std::string* error_message
);

} // namespace agentcore

#endif // AGENTCORE_EXECUTION_SUBGRAPH_SESSION_RUNTIME_H
