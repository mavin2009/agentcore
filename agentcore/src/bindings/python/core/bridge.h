#ifndef AGENTCORE_BINDINGS_PYTHON_BRIDGE_H
#define AGENTCORE_BINDINGS_PYTHON_BRIDGE_H

#include <Python.h>

#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace agentcore::python_binding {

inline constexpr const char* kGraphCapsuleName = "agentcore.python_graph";
inline constexpr const char* kRuntimeCapsuleName = "agentcore.python_runtime";
inline constexpr const char* kInternalEndNodeName = "__agentcore_internal_end__";
inline constexpr const char* kInternalBootstrapNodeName = "__agentcore_internal_bootstrap__";
inline constexpr const char* kPythonRuntimeConfigKey = "__agentcore_runtime__";

class GraphHandle {
public:
    GraphHandle(std::string name, std::size_t worker_count, GraphId graph_id);
    ~GraphHandle();

    GraphHandle(const GraphHandle&) = delete;
    GraphHandle& operator=(const GraphHandle&) = delete;

    bool add_node(
        std::string_view name,
        PyObject* callback,
        NodeKind kind,
        uint32_t policy_flags,
        const std::vector<std::pair<std::string, JoinMergeStrategy>>& merge_rules,
        std::string* error_message
    );
    bool add_subgraph_node(
        std::string_view name,
        GraphHandle* subgraph_handle,
        std::string_view namespace_name,
        const std::vector<std::pair<std::string, std::string>>& input_bindings,
        const std::vector<std::pair<std::string, std::string>>& output_bindings,
        bool propagate_knowledge_graph,
        std::string_view session_mode_name,
        const std::optional<std::string>& session_id_source_name,
        std::string* error_message
    );
    bool add_edge(std::string_view from_name, std::string_view to_name, std::string* error_message);
    bool set_entry_point(std::string_view node_name, std::string* error_message);
    bool finalize(std::string* error_message);
    bool register_python_tool(
        std::string_view name,
        PyObject* callback,
        ToolPolicy policy,
        AdapterMetadata metadata,
        std::string* error_message
    );
    bool register_python_model(
        std::string_view name,
        PyObject* callback,
        ModelPolicy policy,
        AdapterMetadata metadata,
        std::string* error_message
    );
    bool invoke(
        PyObject* input_state,
        PyObject* config,
        PyObject** output_state,
        std::string* error_message
    );
    bool invoke_with_details(
        PyObject* input_state,
        PyObject* config,
        bool include_subgraphs,
        PyObject** output_details,
        std::string* error_message
    );
    bool invoke_until_pause_with_details(
        PyObject* input_state,
        PyObject* config,
        bool include_subgraphs,
        PyObject** output_details,
        std::string* error_message
    );
    bool resume_with_details(
        CheckpointId checkpoint_id,
        bool include_subgraphs,
        PyObject** output_details,
        std::string* error_message
    );
    bool stream(
        PyObject* input_state,
        PyObject* config,
        bool include_subgraphs,
        PyObject** output_events,
        std::string* error_message
    );

    [[nodiscard]] GraphId graph_id() const noexcept { return graph_id_; }
    [[nodiscard]] const GraphDefinition& graph() const noexcept { return graph_; }
    [[nodiscard]] ToolRegistry& tools() noexcept { return engine_.tools(); }
    [[nodiscard]] const ToolRegistry& tools() const noexcept {
        return const_cast<ExecutionEngine&>(engine_).tools();
    }
    [[nodiscard]] ModelRegistry& models() noexcept { return engine_.models(); }
    [[nodiscard]] const ModelRegistry& models() const noexcept {
        return const_cast<ExecutionEngine&>(engine_).models();
    }
    [[nodiscard]] std::string node_name(NodeId node_id) const;
    [[nodiscard]] static std::string fetch_python_error();
    [[nodiscard]] static std::string node_status_name(NodeResult::Status status);
    [[nodiscard]] static std::string execution_status_name(ExecutionStatus status);

private:
    friend NodeResult python_bootstrap_executor(ExecutionContext& context);
    friend NodeResult python_node_executor(ExecutionContext& context);

    struct NodeBinding {
        NodeId node_id{0};
        std::string name;
        PyObject* callback{nullptr};
        NodeKind kind{NodeKind::Compute};
        uint32_t policy_flags{0U};
        std::vector<FieldMergeRule> merge_rules;
        std::optional<SubgraphBinding> subgraph;
        GraphHandle* subgraph_handle{nullptr};
    };

    struct RunArtifacts {
        PyObject* state{nullptr};
        PyObject* trace{nullptr};
        RunResult result{};
        RunProofDigest proof{};
        RunId run_id{0};
    };

    bool ensure_finalized(std::string* error_message);
    bool finalize_locked(std::string* error_message);
    bool execute_run(
        PyObject* input_state,
        PyObject* config,
        bool include_subgraphs,
        bool allow_paused,
        RunArtifacts* artifacts,
        std::string* error_message
    );
    bool populate_run_artifacts(
        RunId run_id,
        const RunResult& run_result,
        bool include_subgraphs,
        RunArtifacts* artifacts,
        std::string* error_message
    );
    bool build_initial_envelope(
        PyObject* input_state,
        PyObject* config,
        InputEnvelope* input,
        std::string* error_message
    );
    bool convert_mapping_to_patch(
        PyObject* mapping,
        BlobStore& blobs,
        StringInterner& strings,
        StatePatch* patch,
        std::string* error_message
    );
    bool parse_callback_result(
        PyObject* result,
        BlobStore& blobs,
        StringInterner& strings,
        StatePatch* patch,
        std::optional<NodeId>* next_override,
        bool* should_wait,
        std::string* error_message
    );
    bool convert_python_value(
        PyObject* value,
        BlobStore& blobs,
        StringInterner& strings,
        Value* output,
        std::string* error_message
    );
    PyObject* state_to_python_dict(
        const WorkflowState& state,
        const BlobStore& blobs,
        const StringInterner& strings,
        std::string* error_message
    ) const;
    PyObject* convert_value_to_python(
        const Value& value,
        const BlobStore& blobs,
        const StringInterner& strings,
        std::string* error_message
    ) const;
    PyObject* build_trace_list(
        RunId run_id,
        bool include_subgraphs,
        std::string* error_message
    ) const;
    PyObject* build_details_dict(const RunArtifacts& artifacts, std::string* error_message) const;
    [[nodiscard]] std::optional<NodeId> lookup_node_id(std::string_view node_name) const;
    [[nodiscard]] const NodeBinding* lookup_node_binding(NodeId node_id) const;
    [[nodiscard]] bool is_internal_node(NodeId node_id) const;
    [[nodiscard]] StateKey ensure_state_key(std::string_view state_name);
    [[nodiscard]] StateKey ensure_state_key_locked(std::string_view state_name);
    void capture_run_context(RunId run_id, PyObject* input_state, PyObject* config);
    void release_run_context(RunId run_id);
    void register_graphs_recursive(
        ExecutionEngine& engine,
        std::unordered_set<GraphId>* visited = nullptr
    ) const;

    mutable std::mutex mutex_;
    GraphDefinition graph_{};
    ExecutionEngine engine_;
    std::string name_;
    GraphId graph_id_{0};
    NodeId next_node_id_{1};
    EdgeId next_edge_id_{1};
    std::optional<NodeId> entry_node_id_;
    std::vector<NodeBinding> node_bindings_;
    std::unordered_map<std::string, NodeId> node_ids_by_name_;
    std::vector<std::pair<NodeId, NodeId>> edges_;
    std::vector<std::string> state_names_;
    std::unordered_map<std::string, StateKey> state_keys_;
    std::unordered_map<RunId, PyObject*> pending_initial_inputs_;
    std::unordered_map<RunId, PyObject*> pending_configs_;
    std::unordered_map<std::string, std::shared_ptr<PyObject>> python_tool_callbacks_;
    std::unordered_map<std::string, std::shared_ptr<PyObject>> python_model_callbacks_;
    bool finalized_{false};
};

[[nodiscard]] std::unique_ptr<GraphHandle> create_graph_handle(
    std::string name,
    std::size_t worker_count
);
[[nodiscard]] PyObject* create_graph_capsule(std::unique_ptr<GraphHandle> handle);
[[nodiscard]] GraphHandle* graph_handle_from_capsule(PyObject* capsule);
[[nodiscard]] PyObject* create_runtime_capsule(ExecutionContext& context);
[[nodiscard]] PyObject* runtime_record_once(
    PyObject* runtime_capsule,
    std::string_view key,
    PyObject* request,
    PyObject* producer,
    std::string* error_message
);
[[nodiscard]] PyObject* list_registered_tools(
    const ToolRegistry& registry,
    std::string* error_message
);
[[nodiscard]] PyObject* describe_registered_tool(
    const ToolRegistry& registry,
    std::string_view name,
    std::string* error_message
);
[[nodiscard]] PyObject* invoke_tool_registry(
    ToolRegistry& registry,
    std::string_view name,
    const std::vector<std::byte>& input_bytes,
    std::string* error_message
);
[[nodiscard]] PyObject* list_registered_models(
    const ModelRegistry& registry,
    std::string* error_message
);
[[nodiscard]] PyObject* describe_registered_model(
    const ModelRegistry& registry,
    std::string_view name,
    std::string* error_message
);
[[nodiscard]] PyObject* invoke_model_registry(
    ModelRegistry& registry,
    std::string_view name,
    const std::vector<std::byte>& prompt_bytes,
    const std::vector<std::byte>& schema_bytes,
    uint32_t max_tokens,
    std::string* error_message
);
[[nodiscard]] PyObject* runtime_invoke_tool(
    PyObject* runtime_capsule,
    std::string_view name,
    const std::vector<std::byte>& input_bytes,
    std::string* error_message
);
[[nodiscard]] PyObject* runtime_invoke_model(
    PyObject* runtime_capsule,
    std::string_view name,
    const std::vector<std::byte>& prompt_bytes,
    const std::vector<std::byte>& schema_bytes,
    uint32_t max_tokens,
    std::string* error_message
);

NodeResult python_bootstrap_executor(ExecutionContext& context);
NodeResult python_node_executor(ExecutionContext& context);
NodeResult native_stop_executor(ExecutionContext& context);

} // namespace agentcore::python_binding

#endif // AGENTCORE_BINDINGS_PYTHON_BRIDGE_H
