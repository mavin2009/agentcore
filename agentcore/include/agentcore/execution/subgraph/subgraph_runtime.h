#ifndef AGENTCORE_EXECUTION_SUBGRAPH_RUNTIME_H
#define AGENTCORE_EXECUTION_SUBGRAPH_RUNTIME_H

#include "agentcore/graph/graph_ir.h"
#include "agentcore/state/state_store.h"

namespace agentcore {

[[nodiscard]] StatePatch build_subgraph_input_patch(
    const StateStore& parent_state,
    const SubgraphBinding& binding,
    StateStore& child_state,
    bool seed_knowledge_graph
);

[[nodiscard]] StatePatch build_subgraph_output_patch(
    const StateStore& child_state,
    const SubgraphBinding& binding,
    StateStore& parent_state
);

} // namespace agentcore

#endif // AGENTCORE_EXECUTION_SUBGRAPH_RUNTIME_H
