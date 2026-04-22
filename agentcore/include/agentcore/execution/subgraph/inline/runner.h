#ifndef AGENTCORE_EXECUTION_SUBGRAPH_INLINE_RUNNER_H
#define AGENTCORE_EXECUTION_SUBGRAPH_INLINE_RUNNER_H

#include "agentcore/execution/checkpoint.h"
#include "agentcore/graph/graph_ir.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/node_runtime.h"
#include "agentcore/runtime/tool_api.h"

#include <optional>
#include <string>
#include <vector>

namespace agentcore {

enum class InlineSingleBranchSubgraphOutcome : uint8_t {
    Completed = 0,
    Waiting = 1,
    Failed = 2,
    Cancelled = 3
};

struct InlineSingleBranchSubgraphResult {
    InlineSingleBranchSubgraphOutcome outcome{InlineSingleBranchSubgraphOutcome::Failed};
    StateStore state_store{};
    ExecutionFrame frame{};
    uint16_t retry_count{0};
    std::optional<PendingAsyncOperation> pending_async;
    std::vector<TraceEvent> trace_events;
    float confidence{1.0F};
    std::string error_message;
};

[[nodiscard]] bool inline_single_branch_subgraph_eligible(const GraphDefinition& graph) noexcept;

[[nodiscard]] InlineSingleBranchSubgraphResult run_inline_single_branch_subgraph(
    const GraphDefinition& graph,
    NodeId entry_node,
    StateStore initial_state,
    const std::vector<std::byte>& runtime_config_payload,
    ToolRegistry& tools,
    ModelRegistry& models,
    bool capture_trace = true
);

} // namespace agentcore

#endif // AGENTCORE_EXECUTION_SUBGRAPH_INLINE_RUNNER_H
