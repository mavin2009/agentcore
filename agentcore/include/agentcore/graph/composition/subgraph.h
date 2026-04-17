#ifndef AGENTCORE_GRAPH_COMPOSITION_SUBGRAPH_H
#define AGENTCORE_GRAPH_COMPOSITION_SUBGRAPH_H

#include "agentcore/graph/graph_ir.h"
#include <string>
#include <vector>

namespace agentcore {

[[nodiscard]] bool validate_subgraph_binding(
    const NodeDefinition& node,
    std::string* error_message = nullptr
);

[[nodiscard]] uint32_t infer_subgraph_initial_field_count(const SubgraphBinding& binding) noexcept;

[[nodiscard]] std::vector<ExecutionNamespaceRef> prefix_namespace_path(
    const ExecutionNamespaceRef& prefix,
    const std::vector<ExecutionNamespaceRef>& child_path
);

} // namespace agentcore

#endif // AGENTCORE_GRAPH_COMPOSITION_SUBGRAPH_H
