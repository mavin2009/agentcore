#ifndef AGENTCORE_BINDINGS_PYTHON_BRIDGE_H
#define AGENTCORE_BINDINGS_PYTHON_BRIDGE_H

#include <Python.h>

#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/adapters/adapter_factories.h"
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace agentcore::python_binding {

class GraphHandle;
struct StreamIterator;

constexpr const char* kGraphCapsuleName = "agentcore._native.GraphHandle";
constexpr const char* kRuntimeCapsuleName = "agentcore._native.RuntimeContext";
constexpr const char* kPythonRuntimeConfigKey = "_runtime_config";
constexpr const char* kInternalBootstrapNodeName = "__BOOTSTRAP__";
constexpr const char* kInternalEndNodeName = "__END__";

struct NodeBinding {
    NodeId node_id{0};
    std::string name;
    PyObject* callback{nullptr};
    NodeKind kind{NodeKind::Compute};
    uint32_t policy_flags{0U};
    std::vector<FieldMergeRule> merge_rules;
    NodeMemoizationPolicy memoization;
    std::vector<IntelligenceSubscription> intelligence_subscriptions;
    std::optional<SubgraphBinding> subgraph;
    GraphHandle* subgraph_handle{nullptr};
    PyObject* patch_key_lookup{nullptr};
};

struct RunArtifacts {
    PyObject* state{nullptr};
    PyObject* trace{nullptr};
    PyObject* intelligence{nullptr};
    RunResult result{};
    RunProofDigest proof{};
    RunId run_id{0};
};

class GraphHandle {
public:
    explicit GraphHandle(std::unique_ptr<ExecutionEngine> engine, std::string name = "");
    ~GraphHandle();

    GraphHandle(const GraphHandle&) = delete;
    auto operator=(const GraphHandle&) -> GraphHandle& = delete;

    [[nodiscard]] GraphId id() const noexcept { return graph_id_; }

    bool add_node(
        std::string_view name,
        PyObject* callback,
        NodeKind kind,
        uint32_t policy_flags,
        const NodeMemoizationPolicy& memoization,
        const std::vector<IntelligenceSubscription>& intelligence_subscriptions,
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
    bool set_entry_point(std::string_view name, std::string* error_message);
    bool add_edge(std::string_view source, std::string_view target, std::string* error_message);
    bool finalize(std::string* error_message);

    bool invoke(PyObject* input_state, PyObject* config, PyObject** result, std::string* error_message);
    bool stream(
        PyObject* input_state,
        PyObject* config,
        bool include_subgraphs,
        PyObject* owner_ref,
        PyObject** result,
        std::string* error_message
    );

    bool invoke_with_details(PyObject* input_state, PyObject* config, bool include_subgraphs, PyObject** result, std::string* error_message);
    bool invoke_until_pause_with_details(PyObject* input_state, PyObject* config, bool include_subgraphs, PyObject** result, std::string* error_message);
    bool resume_with_details(CheckpointId checkpoint_id, bool include_subgraphs, PyObject** result, std::string* error_message);

    bool register_python_tool(std::string_view name, PyObject* callback, ToolPolicy policy, AdapterMetadata metadata, std::string* error_message);
    bool register_python_model(std::string_view name, PyObject* callback, ModelPolicy policy, AdapterMetadata metadata, std::string* error_message);

    [[nodiscard]] ExecutionEngine& engine() noexcept { return *engine_; }
    [[nodiscard]] ToolRegistry& tools() noexcept { return engine_->tools(); }
    [[nodiscard]] ModelRegistry& models() noexcept { return engine_->models(); }

    static std::string fetch_python_error();

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

    bool convert_python_value(
        PyObject* value,
        BlobStore& blobs,
        StringInterner& strings,
        Value* output,
        std::string* error_message
    );

    bool ensure_finalized(std::string* error_message);

    StateKey ensure_state_key(std::string_view state_name);

    bool build_trace_event_dict(
        const TraceEvent& event,
        bool include_subgraphs,
        PyObject** result,
        std::string* error_message
    ) const;

private:
    friend NodeResult python_bootstrap_executor(ExecutionContext& context);
    friend NodeResult python_node_executor(ExecutionContext& context);

    bool finalize_locked(std::string* error_message);
    bool execute_run_core(
        PyObject* input_state,
        PyObject* config,
        bool until_pause,
        const RunCaptureOptions& capture_options,
        RunId* run_id,
        RunResult* result,
        std::string* error_message
    );
    bool execute_run(PyObject* input_state, PyObject* config, bool include_subgraphs, bool until_pause, RunArtifacts* artifacts, std::string* error_message);
    bool populate_run_artifacts(RunId run_id, const RunResult& run_result, bool include_subgraphs, RunArtifacts* artifacts, std::string* error_message);
    PyObject* build_trace_list(RunId run_id, bool include_subgraphs, std::string* error_message) const;
    PyObject* build_details_dict(const RunArtifacts& artifacts, std::string* error_message) const;
    bool register_graph_hierarchy(ExecutionEngine& target_engine, std::string* error_message);

    bool build_initial_envelope(PyObject* input_state, PyObject* config, InputEnvelope* envelope, std::string* error_message);
    bool convert_mapping_to_patch(
        PyObject* mapping,
        BlobStore& blobs,
        StringInterner& strings,
        StatePatch* patch,
        std::string* error_message,
        PyObject* fast_state_key_lookup = nullptr
    );

    StateKey ensure_state_key_locked(std::string_view state_name);

    std::string node_name(NodeId node_id) const;

    std::unique_ptr<ExecutionEngine> engine_;
    GraphId graph_id_{0};
    GraphDefinition graph_{};
    bool finalized_{false};
    std::string name_;

    std::unordered_map<std::string, NodeId> node_ids_by_name_;
    std::vector<NodeBinding> node_bindings_;
    std::unordered_map<NodeId, std::size_t> node_binding_indices_;
    std::vector<std::pair<NodeId, NodeId>> edges_;
    std::optional<NodeId> entry_node_id_;
    NodeId next_node_id_{1};
    EdgeId next_edge_id_{1};

    mutable std::mutex mutex_;
    std::shared_ptr<std::vector<std::string>> state_names_;
    std::shared_ptr<std::unordered_map<std::string, StateKey>> state_keys_by_name_;
    PyObject* state_key_lookup_{nullptr};
    std::unordered_map<RunId, PyObject*> active_configs_;
};

GraphHandle* create_graph_handle(const char* name, std::size_t worker_count);
PyObject* create_graph_capsule(GraphHandle* handle);
GraphHandle* graph_handle_from_capsule(PyObject* capsule);

bool parse_python_bytes_like(PyObject* object, std::vector<std::byte>* value, std::string* error_message);

NodeResult python_bootstrap_executor(ExecutionContext& context);
NodeResult python_node_executor(ExecutionContext& context);
NodeResult native_stop_executor(ExecutionContext& context);

PyObject* list_registered_tools(const ToolRegistry& registry, std::string* error_message);
PyObject* list_registered_models(const ModelRegistry& registry, std::string* error_message);

PyObject* describe_registered_tool(const ToolRegistry& registry, std::string_view name, std::string* error_message);
PyObject* invoke_tool_registry(ToolRegistry& registry, std::string_view name, const std::vector<std::byte>& input, std::string* error_message);
PyObject* runtime_invoke_tool(PyObject* capsule, std::string_view name, const std::vector<std::byte>& input, std::string* error_message);
PyObject* describe_registered_model(const ModelRegistry& registry, std::string_view name, std::string* error_message);
PyObject* invoke_model_registry(ModelRegistry& registry, std::string_view name, const std::vector<std::byte>& prompt, const std::vector<std::byte>& schema, uint32_t max_tokens, std::string* error_message);
PyObject* runtime_invoke_model(PyObject* capsule, std::string_view name, const std::vector<std::byte>& prompt, const std::vector<std::byte>& schema, uint32_t max_tokens, std::string* error_message);

PyObject* runtime_record_once(PyObject* capsule, std::string_view key, PyObject* request, PyObject* producer, std::string* error_message);
bool runtime_stage_task_write(PyObject* capsule, PyObject* spec, std::string* error_message);
bool runtime_stage_claim_write(PyObject* capsule, PyObject* spec, std::string* error_message);
bool runtime_stage_evidence_write(PyObject* capsule, PyObject* spec, std::string* error_message);
bool runtime_stage_decision_write(PyObject* capsule, PyObject* spec, std::string* error_message);
bool runtime_stage_memory_write(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_snapshot_intelligence(PyObject* capsule, std::string* error_message);
PyObject* runtime_query_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_related_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_agenda_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_supporting_claims_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_action_candidates_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_recall_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_focus_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_intelligence_summary(PyObject* capsule, std::string* error_message);
PyObject* runtime_count_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);
PyObject* runtime_route_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message);

} // namespace agentcore::python_binding

#endif // AGENTCORE_BINDINGS_PYTHON_BRIDGE_H
