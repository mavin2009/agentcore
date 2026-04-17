#include "agentcore/graph/composition/subgraph.h"

#include <algorithm>

namespace agentcore {

namespace {

std::string namespace_name_for(const NodeDefinition& node) {
    if (!node.subgraph.has_value()) {
        return {};
    }
    if (!node.subgraph->namespace_name.empty()) {
        return node.subgraph->namespace_name;
    }
    return node.name;
}

} // namespace

bool validate_subgraph_binding(const NodeDefinition& node, std::string* error_message) {
    auto fail = [error_message](const std::string& message) {
        if (error_message != nullptr) {
            *error_message = message;
        }
        return false;
    };

    if (node.kind != NodeKind::Subgraph) {
        return fail("subgraph binding validation requires a subgraph node");
    }
    if (!node.subgraph.has_value() || !node.subgraph->valid()) {
        return fail("subgraph node is missing a valid target graph binding: " + node.name);
    }

    const std::string namespace_name = namespace_name_for(node);
    if (namespace_name.empty()) {
        return fail("subgraph node requires a stable namespace name: " + node.name);
    }

    std::vector<StateKey> seen_child_inputs;
    for (const SubgraphStateBinding& binding : node.subgraph->input_bindings) {
        if (std::find(seen_child_inputs.begin(), seen_child_inputs.end(), binding.child_key) != seen_child_inputs.end()) {
            return fail("duplicate subgraph child input key on node " + node.name);
        }
        seen_child_inputs.push_back(binding.child_key);
    }

    std::vector<StateKey> seen_parent_outputs;
    for (const SubgraphStateBinding& binding : node.subgraph->output_bindings) {
        if (std::find(seen_parent_outputs.begin(), seen_parent_outputs.end(), binding.parent_key) != seen_parent_outputs.end()) {
            return fail("duplicate subgraph parent output key on node " + node.name);
        }
        seen_parent_outputs.push_back(binding.parent_key);
    }

    return true;
}

uint32_t infer_subgraph_initial_field_count(const SubgraphBinding& binding) noexcept {
    uint32_t field_count = binding.initial_field_count;
    for (const SubgraphStateBinding& input : binding.input_bindings) {
        field_count = std::max(field_count, input.child_key + 1U);
    }
    for (const SubgraphStateBinding& output : binding.output_bindings) {
        field_count = std::max(field_count, output.child_key + 1U);
    }
    return field_count;
}

std::vector<ExecutionNamespaceRef> prefix_namespace_path(
    const ExecutionNamespaceRef& prefix,
    const std::vector<ExecutionNamespaceRef>& child_path
) {
    std::vector<ExecutionNamespaceRef> result;
    result.reserve(child_path.size() + 1U);
    result.push_back(prefix);
    result.insert(result.end(), child_path.begin(), child_path.end());
    return result;
}

} // namespace agentcore
