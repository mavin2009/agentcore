#include "bridge.h"

#include "agentcore/runtime/tool_api.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <utility>

namespace agentcore::python_binding {

namespace {

constexpr std::byte kJsonBlobTag{static_cast<std::byte>(0x7BU)};
constexpr std::byte kBytesBlobTag{static_cast<std::byte>(0x42U)};
constexpr std::byte kPickleBlobTag{static_cast<std::byte>(0x50U)};

class GraphHandleRegistry {
public:
    static GraphHandleRegistry& instance() {
        static GraphHandleRegistry registry;
        return registry;
    }

    void add(GraphId graph_id, GraphHandle* handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        handles_[graph_id] = handle;
    }

    void remove(GraphId graph_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        handles_.erase(graph_id);
    }

    [[nodiscard]] GraphHandle* find(GraphId graph_id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handles_.find(graph_id);
        return iterator == handles_.end() ? nullptr : iterator->second;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<GraphId, GraphHandle*> handles_;
};

[[nodiscard]] GraphId next_python_graph_id() {
    static std::atomic<GraphId> next_graph_id{900000U};
    return next_graph_id.fetch_add(1U);
}

struct CallbackRuntimeHandle {
    ExecutionContext* context{nullptr};
};

void destroy_graph_capsule(PyObject* capsule) {
    auto* handle = static_cast<GraphHandle*>(PyCapsule_GetPointer(capsule, kGraphCapsuleName));
    if (handle == nullptr) {
        PyErr_Clear();
        return;
    }
    delete handle;
}

void destroy_runtime_capsule(PyObject* capsule) {
    auto* handle = static_cast<CallbackRuntimeHandle*>(PyCapsule_GetPointer(capsule, kRuntimeCapsuleName));
    if (handle == nullptr) {
        PyErr_Clear();
        return;
    }
    delete handle;
}

[[nodiscard]] bool tagged_blob_has_payload(const std::vector<std::byte>& bytes, std::byte tag) {
    return !bytes.empty() && bytes.front() == tag;
}

[[nodiscard]] BlobRef append_tagged_blob(
    BlobStore& blobs,
    std::byte tag,
    const std::byte* bytes,
    std::size_t size
) {
    std::vector<std::byte> tagged(size + 1U, std::byte{0});
    tagged[0] = tag;
    if (size != 0U) {
        std::memcpy(tagged.data() + 1, bytes, size);
    }
    return blobs.append(tagged.data(), tagged.size());
}

[[nodiscard]] PyObject* unicode_from_utf8(std::string_view value) {
    return PyUnicode_FromStringAndSize(
        value.data(),
        static_cast<Py_ssize_t>(value.size())
    );
}

bool set_dict_item(PyObject* dict, const char* key, PyObject* value) {
    if (value == nullptr) {
        return false;
    }
    const int rc = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    return rc == 0;
}

[[nodiscard]] std::string python_string(PyObject* unicode_object, std::string* error_message) {
    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(unicode_object, &size);
    if (utf8 == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return {};
    }
    return std::string(utf8, static_cast<std::size_t>(size));
}

bool dump_object_as_json(PyObject* object, std::string* output, std::string* error_message) {
    PyObject* json_module = PyImport_ImportModule("json");
    if (json_module == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    PyObject* dumps = PyObject_GetAttrString(json_module, "dumps");
    Py_DECREF(json_module);
    if (dumps == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    PyObject* serialized = PyObject_CallFunctionObjArgs(dumps, object, nullptr);
    Py_DECREF(dumps);
    if (serialized == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    if (!PyUnicode_Check(serialized)) {
        Py_DECREF(serialized);
        *error_message = "json.dumps returned a non-string payload";
        return false;
    }
    *output = python_string(serialized, error_message);
    Py_DECREF(serialized);
    return error_message->empty();
}

bool dump_object_as_pickle_bytes(
    PyObject* object,
    std::vector<std::byte>* output,
    std::string* error_message
) {
    PyObject* pickle_module = PyImport_ImportModule("pickle");
    if (pickle_module == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    PyObject* dumps = PyObject_GetAttrString(pickle_module, "dumps");
    Py_DECREF(pickle_module);
    if (dumps == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }

    PyObject* serialized = PyObject_CallFunctionObjArgs(dumps, object, nullptr);
    Py_DECREF(dumps);
    if (serialized == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    if (!PyBytes_Check(serialized)) {
        Py_DECREF(serialized);
        *error_message = "pickle.dumps returned a non-bytes payload";
        return false;
    }

    char* bytes = nullptr;
    Py_ssize_t size = 0;
    if (PyBytes_AsStringAndSize(serialized, &bytes, &size) != 0) {
        Py_DECREF(serialized);
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }

    output->assign(
        reinterpret_cast<const std::byte*>(bytes),
        reinterpret_cast<const std::byte*>(bytes) + static_cast<std::size_t>(size)
    );
    Py_DECREF(serialized);
    return true;
}

PyObject* load_object_from_json_bytes(
    const std::vector<std::byte>& bytes,
    std::string* error_message
) {
    PyObject* json_module = PyImport_ImportModule("json");
    if (json_module == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }
    PyObject* loads = PyObject_GetAttrString(json_module, "loads");
    Py_DECREF(json_module);
    if (loads == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }
    PyObject* text = PyUnicode_FromStringAndSize(
        reinterpret_cast<const char*>(bytes.data() + 1),
        static_cast<Py_ssize_t>(bytes.size() - 1U)
    );
    if (text == nullptr) {
        Py_DECREF(loads);
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }
    PyObject* decoded = PyObject_CallFunctionObjArgs(loads, text, nullptr);
    Py_DECREF(text);
    Py_DECREF(loads);
    if (decoded == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }
    return decoded;
}

PyObject* load_object_from_pickle_bytes(
    const std::vector<std::byte>& bytes,
    std::string* error_message
) {
    PyObject* pickle_module = PyImport_ImportModule("pickle");
    if (pickle_module == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }
    PyObject* loads = PyObject_GetAttrString(pickle_module, "loads");
    Py_DECREF(pickle_module);
    if (loads == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }

    PyObject* payload = PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<Py_ssize_t>(bytes.size())
    );
    if (payload == nullptr) {
        Py_DECREF(loads);
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }

    PyObject* decoded = PyObject_CallFunctionObjArgs(loads, payload, nullptr);
    Py_DECREF(payload);
    Py_DECREF(loads);
    if (decoded == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }
    return decoded;
}

ExecutionContext* runtime_context_from_capsule(PyObject* capsule, std::string* error_message) {
    auto* handle = static_cast<CallbackRuntimeHandle*>(
        PyCapsule_GetPointer(capsule, kRuntimeCapsuleName)
    );
    if (handle == nullptr) {
        if (PyErr_Occurred() != nullptr) {
            *error_message = GraphHandle::fetch_python_error();
            PyErr_Clear();
        } else {
            *error_message = "invalid runtime context capsule";
        }
        return nullptr;
    }
    if (handle->context == nullptr) {
        *error_message = "runtime context is not available";
        return nullptr;
    }
    return handle->context;
}

bool pickle_blob_payload_matches(
    const BlobStore& blobs,
    BlobRef ref,
    const std::vector<std::byte>& expected_payload
) {
    const std::vector<std::byte> tagged = blobs.read_bytes(ref);
    if (!tagged_blob_has_payload(tagged, kPickleBlobTag)) {
        return false;
    }
    if (tagged.size() != expected_payload.size() + 1U) {
        return false;
    }
    return std::equal(expected_payload.begin(), expected_payload.end(), tagged.begin() + 1);
}

PyObject* load_pickled_blob_value(
    const BlobStore& blobs,
    BlobRef ref,
    std::string* error_message
) {
    const std::vector<std::byte> tagged = blobs.read_bytes(ref);
    if (!tagged_blob_has_payload(tagged, kPickleBlobTag)) {
        *error_message = "recorded effect payload is not pickle-encoded";
        return nullptr;
    }
    return load_object_from_pickle_bytes(
        std::vector<std::byte>(tagged.begin() + 1, tagged.end()),
        error_message
    );
}

PyObject* event_to_python(
    const GraphHandle& handle,
    const StreamEvent& event,
    std::string* error_message
) {
    const GraphHandle* event_handle = &handle;
    if (GraphHandle* resolved_handle = GraphHandleRegistry::instance().find(event.graph_id);
        resolved_handle != nullptr) {
        event_handle = resolved_handle;
    }

    PyObject* event_dict = PyDict_New();
    if (event_dict == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }

    if (!set_dict_item(event_dict, "sequence", PyLong_FromUnsignedLongLong(event.sequence)) ||
        !set_dict_item(event_dict, "run_id", PyLong_FromUnsignedLongLong(event.run_id)) ||
        !set_dict_item(event_dict, "graph_id", PyLong_FromUnsignedLong(event.graph_id)) ||
        !set_dict_item(event_dict, "node_id", PyLong_FromUnsignedLong(event.node_id)) ||
        !set_dict_item(event_dict, "branch_id", PyLong_FromUnsignedLong(event.branch_id)) ||
        !set_dict_item(
            event_dict,
            "checkpoint_id",
            PyLong_FromUnsignedLongLong(event.checkpoint_id)
        ) ||
        !set_dict_item(
            event_dict,
            "status",
            unicode_from_utf8(GraphHandle::node_status_name(event.node_status))
        ) ||
        !set_dict_item(event_dict, "confidence", PyFloat_FromDouble(event.confidence)) ||
        !set_dict_item(event_dict, "patch_count", PyLong_FromUnsignedLong(event.patch_count)) ||
        !set_dict_item(event_dict, "flags", PyLong_FromUnsignedLong(event.flags)) ||
        !set_dict_item(event_dict, "session_id", unicode_from_utf8(event.session_id)) ||
        !set_dict_item(
            event_dict,
            "session_revision",
            PyLong_FromUnsignedLongLong(event.session_revision)
        ) ||
        !set_dict_item(event_dict, "graph_name", unicode_from_utf8(event_handle->graph().name)) ||
        !set_dict_item(event_dict, "node_name", unicode_from_utf8(event_handle->node_name(event.node_id)))) {
        Py_DECREF(event_dict);
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }

    PyObject* namespaces = PyList_New(0);
    if (namespaces == nullptr) {
        Py_DECREF(event_dict);
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }
    for (const StreamNamespaceFrame& frame : event.namespaces) {
        PyObject* frame_dict = PyDict_New();
        if (frame_dict == nullptr) {
            Py_DECREF(namespaces);
            Py_DECREF(event_dict);
            *error_message = GraphHandle::fetch_python_error();
            return nullptr;
        }
        const bool frame_ok =
            set_dict_item(frame_dict, "graph_id", PyLong_FromUnsignedLong(frame.graph_id)) &&
            set_dict_item(frame_dict, "node_id", PyLong_FromUnsignedLong(frame.node_id)) &&
            set_dict_item(frame_dict, "graph_name", unicode_from_utf8(frame.graph_name)) &&
            set_dict_item(frame_dict, "node_name", unicode_from_utf8(frame.node_name)) &&
            set_dict_item(frame_dict, "session_id", unicode_from_utf8(frame.session_id)) &&
            set_dict_item(
                frame_dict,
                "session_revision",
                PyLong_FromUnsignedLongLong(frame.session_revision)
            );
        if (!frame_ok || PyList_Append(namespaces, frame_dict) != 0) {
            Py_DECREF(frame_dict);
            Py_DECREF(namespaces);
            Py_DECREF(event_dict);
            *error_message = GraphHandle::fetch_python_error();
            return nullptr;
        }
        Py_DECREF(frame_dict);
    }
    if (!set_dict_item(event_dict, "namespaces", namespaces)) {
        Py_DECREF(event_dict);
        *error_message = GraphHandle::fetch_python_error();
        return nullptr;
    }

    return event_dict;
}

} // namespace

GraphHandle::GraphHandle(std::string name, std::size_t worker_count, GraphId graph_id)
    : engine_(worker_count),
      name_(std::move(name)),
      graph_id_(graph_id) {
    GraphHandleRegistry::instance().add(graph_id_, this);
    node_bindings_.push_back(NodeBinding{
        next_node_id_++,
        kInternalEndNodeName,
        nullptr,
        NodeKind::Control,
        node_policy_mask(NodePolicyFlag::StopAfterNode),
        {},
        std::nullopt,
        nullptr
    });
    node_ids_by_name_.emplace(kInternalEndNodeName, node_bindings_.back().node_id);
    node_bindings_.push_back(NodeBinding{
        next_node_id_++,
        kInternalBootstrapNodeName,
        nullptr,
        NodeKind::Compute,
        0U,
        {},
        std::nullopt,
        nullptr
    });
    node_ids_by_name_.emplace(kInternalBootstrapNodeName, node_bindings_.back().node_id);
}

GraphHandle::~GraphHandle() {
    GraphHandleRegistry::instance().remove(graph_id_);
    if (Py_IsInitialized() == 0) {
        return;
    }

    const PyGILState_STATE gil_state = PyGILState_Ensure();
    for (NodeBinding& binding : node_bindings_) {
        Py_XDECREF(binding.callback);
        binding.callback = nullptr;
    }
    for (auto& [_, input_state] : pending_initial_inputs_) {
        Py_XDECREF(input_state);
    }
    pending_initial_inputs_.clear();
    for (auto& [_, config] : pending_configs_) {
        Py_XDECREF(config);
    }
    pending_configs_.clear();
    PyGILState_Release(gil_state);
}

bool GraphHandle::add_node(
    std::string_view name,
    PyObject* callback,
    NodeKind kind,
    uint32_t policy_flags,
    const std::vector<std::pair<std::string, JoinMergeStrategy>>& merge_rules,
    std::string* error_message
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) {
        *error_message = "graph has already been finalized";
        return false;
    }
    if (name.empty()) {
        *error_message = "node name must not be empty";
        return false;
    }
    if (name == kInternalEndNodeName) {
        *error_message = "node name is reserved for the internal END sentinel";
        return false;
    }
    if (name == kInternalBootstrapNodeName) {
        *error_message = "node name is reserved for the internal bootstrap sentinel";
        return false;
    }
    if (node_ids_by_name_.find(std::string(name)) != node_ids_by_name_.end()) {
        *error_message = "duplicate node name: " + std::string(name);
        return false;
    }
    if (callback != nullptr && !PyCallable_Check(callback)) {
        *error_message = "node callback must be callable";
        return false;
    }
    if (kind == NodeKind::Subgraph) {
        *error_message = "subgraph nodes must be added with add_subgraph_node";
        return false;
    }

    Py_XINCREF(callback);
    const NodeId node_id = next_node_id_++;
    node_ids_by_name_.emplace(std::string(name), node_id);
    std::vector<FieldMergeRule> resolved_merge_rules;
    resolved_merge_rules.reserve(merge_rules.size());
    for (const auto& [key_name, strategy] : merge_rules) {
        resolved_merge_rules.push_back(FieldMergeRule{
            ensure_state_key_locked(key_name),
            strategy
        });
    }
    node_bindings_.push_back(NodeBinding{
        node_id,
        std::string(name),
        callback,
        kind,
        policy_flags,
        std::move(resolved_merge_rules),
        std::nullopt,
        nullptr
    });
    if (!entry_node_id_.has_value()) {
        entry_node_id_ = node_id;
    }
    return true;
}

bool GraphHandle::add_subgraph_node(
    std::string_view name,
    GraphHandle* subgraph_handle,
    std::string_view namespace_name,
    const std::vector<std::pair<std::string, std::string>>& input_bindings,
    const std::vector<std::pair<std::string, std::string>>& output_bindings,
    bool propagate_knowledge_graph,
    std::string_view session_mode_name,
    const std::optional<std::string>& session_id_source_name,
    std::string* error_message
) {
    if (subgraph_handle == nullptr) {
        *error_message = "subgraph graph handle must not be null";
        return false;
    }
    if (subgraph_handle == this) {
        *error_message = "graph cannot reference itself as a subgraph";
        return false;
    }
    if (!subgraph_handle->ensure_finalized(error_message)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) {
        *error_message = "graph has already been finalized";
        return false;
    }
    if (name.empty()) {
        *error_message = "node name must not be empty";
        return false;
    }
    if (name == kInternalEndNodeName) {
        *error_message = "node name is reserved for the internal END sentinel";
        return false;
    }
    if (name == kInternalBootstrapNodeName) {
        *error_message = "node name is reserved for the internal bootstrap sentinel";
        return false;
    }
    if (node_ids_by_name_.find(std::string(name)) != node_ids_by_name_.end()) {
        *error_message = "duplicate node name: " + std::string(name);
        return false;
    }

    SubgraphSessionMode session_mode = SubgraphSessionMode::Ephemeral;
    if (session_mode_name == "persistent") {
        session_mode = SubgraphSessionMode::Persistent;
    } else if (session_mode_name != "ephemeral") {
        *error_message = "subgraph session mode must be either 'ephemeral' or 'persistent'";
        return false;
    }
    if (session_mode == SubgraphSessionMode::Persistent && !session_id_source_name.has_value()) {
        *error_message = "persistent subgraph nodes require a session id source field";
        return false;
    }
    if (session_mode == SubgraphSessionMode::Ephemeral && session_id_source_name.has_value()) {
        *error_message = "ephemeral subgraph nodes must not declare a session id source field";
        return false;
    }

    const NodeId node_id = next_node_id_++;
    node_ids_by_name_.emplace(std::string(name), node_id);

    std::vector<SubgraphStateBinding> resolved_input_bindings;
    resolved_input_bindings.reserve(input_bindings.size());
    for (const auto& [parent_key_name, child_key_name] : input_bindings) {
        resolved_input_bindings.push_back(SubgraphStateBinding{
            ensure_state_key_locked(parent_key_name),
            subgraph_handle->ensure_state_key(child_key_name)
        });
    }

    std::vector<SubgraphStateBinding> resolved_output_bindings;
    resolved_output_bindings.reserve(output_bindings.size());
    for (const auto& [parent_key_name, child_key_name] : output_bindings) {
        resolved_output_bindings.push_back(SubgraphStateBinding{
            ensure_state_key_locked(parent_key_name),
            subgraph_handle->ensure_state_key(child_key_name)
        });
    }

    uint32_t initial_field_count = 0U;
    {
        std::lock_guard<std::mutex> child_lock(subgraph_handle->mutex_);
        initial_field_count = static_cast<uint32_t>(subgraph_handle->state_names_.size());
    }
    std::optional<StateKey> session_id_source_key;
    if (session_id_source_name.has_value()) {
        session_id_source_key = ensure_state_key_locked(*session_id_source_name);
    }

    node_bindings_.push_back(NodeBinding{
        node_id,
        std::string(name),
        nullptr,
        NodeKind::Subgraph,
        0U,
        {},
        SubgraphBinding{
            subgraph_handle->graph_id(),
            namespace_name.empty() ? std::string(name) : std::string(namespace_name),
            std::move(resolved_input_bindings),
            std::move(resolved_output_bindings),
            propagate_knowledge_graph,
            initial_field_count,
            session_mode,
            session_id_source_key
        },
        subgraph_handle
    });
    if (!entry_node_id_.has_value()) {
        entry_node_id_ = node_id;
    }

    std::unordered_set<GraphId> visited;
    subgraph_handle->register_graphs_recursive(engine_, &visited);
    return true;
}

bool GraphHandle::add_edge(
    std::string_view from_name,
    std::string_view to_name,
    std::string* error_message
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) {
        *error_message = "graph has already been finalized";
        return false;
    }

    const auto from_iterator = node_ids_by_name_.find(std::string(from_name));
    if (from_iterator == node_ids_by_name_.end()) {
        *error_message = "unknown source node: " + std::string(from_name);
        return false;
    }
    const auto to_iterator = node_ids_by_name_.find(std::string(to_name));
    if (to_iterator == node_ids_by_name_.end()) {
        *error_message = "unknown target node: " + std::string(to_name);
        return false;
    }

    edges_.push_back(std::make_pair(from_iterator->second, to_iterator->second));
    return true;
}

bool GraphHandle::set_entry_point(std::string_view node_name, std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) {
        *error_message = "graph has already been finalized";
        return false;
    }

    const auto iterator = node_ids_by_name_.find(std::string(node_name));
    if (iterator == node_ids_by_name_.end() ||
        iterator->second == node_ids_by_name_.at(kInternalEndNodeName) ||
        iterator->second == node_ids_by_name_.at(kInternalBootstrapNodeName)) {
        *error_message = "unknown entry node: " + std::string(node_name);
        return false;
    }
    entry_node_id_ = iterator->second;
    return true;
}

bool GraphHandle::finalize(std::string* error_message) {
    return ensure_finalized(error_message);
}

bool GraphHandle::invoke(
    PyObject* input_state,
    PyObject* config,
    PyObject** output_state,
    std::string* error_message
) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, true, false, &artifacts, error_message)) {
        return false;
    }

    *output_state = artifacts.state;
    Py_XDECREF(artifacts.trace);
    return true;
}

bool GraphHandle::invoke_with_details(
    PyObject* input_state,
    PyObject* config,
    bool include_subgraphs,
    PyObject** output_details,
    std::string* error_message
) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, false, &artifacts, error_message)) {
        return false;
    }

    *output_details = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    return *output_details != nullptr;
}

bool GraphHandle::invoke_until_pause_with_details(
    PyObject* input_state,
    PyObject* config,
    bool include_subgraphs,
    PyObject** output_details,
    std::string* error_message
) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, true, &artifacts, error_message)) {
        return false;
    }

    *output_details = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    return *output_details != nullptr;
}

bool GraphHandle::resume_with_details(
    CheckpointId checkpoint_id,
    bool include_subgraphs,
    PyObject** output_details,
    std::string* error_message
) {
    if (!ensure_finalized(error_message)) {
        return false;
    }

    PyThreadState* released_thread_state = PyEval_SaveThread();
    const ResumeResult resume_result = engine_.resume(checkpoint_id);
    PyEval_RestoreThread(released_thread_state);
    if (!resume_result.resumed) {
        *error_message = resume_result.message.empty()
            ? "failed to resume checkpoint"
            : resume_result.message;
        return false;
    }

    released_thread_state = PyEval_SaveThread();
    const RunResult run_result = engine_.run_to_completion(resume_result.run_id);
    PyEval_RestoreThread(released_thread_state);

    RunArtifacts artifacts;
    if (!populate_run_artifacts(
            resume_result.run_id,
            run_result,
            include_subgraphs,
            &artifacts,
            error_message
        )) {
        return false;
    }

    if (run_result.status == ExecutionStatus::Completed ||
        run_result.status == ExecutionStatus::Failed ||
        run_result.status == ExecutionStatus::Cancelled) {
        release_run_context(resume_result.run_id);
    }

    *output_details = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    return *output_details != nullptr;
}

bool GraphHandle::stream(
    PyObject* input_state,
    PyObject* config,
    bool include_subgraphs,
    PyObject** output_events,
    std::string* error_message
) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, false, &artifacts, error_message)) {
        return false;
    }

    *output_events = artifacts.trace;
    Py_XDECREF(artifacts.state);
    artifacts.trace = nullptr;
    return true;
}

bool GraphHandle::ensure_finalized(std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) {
        return true;
    }
    return finalize_locked(error_message);
}

bool GraphHandle::finalize_locked(std::string* error_message) {
    if (entry_node_id_.has_value() == false) {
        *error_message = "graph has no entry point";
        return false;
    }

    graph_ = GraphDefinition{};
    graph_.id = graph_id_;
    graph_.name = name_;
    graph_.entry = node_ids_by_name_.at(kInternalBootstrapNodeName);
    graph_.nodes.reserve(node_bindings_.size());
    for (const NodeBinding& binding : node_bindings_) {
        NodeExecutorFn executor = nullptr;
        if (binding.kind == NodeKind::Subgraph) {
            executor = nullptr;
        } else if (binding.callback == nullptr) {
            executor = native_stop_executor;
        } else {
            executor = python_node_executor;
        }
        if (binding.name == kInternalBootstrapNodeName) {
            executor = python_bootstrap_executor;
        }
        graph_.nodes.push_back(NodeDefinition{
            binding.node_id,
            binding.kind,
            binding.name,
            binding.policy_flags,
            0U,
            0U,
            executor,
            {},
            binding.merge_rules,
            {},
            binding.subgraph
        });
    }

    std::unordered_map<NodeId, std::vector<EdgeId>> outgoing_edges;
    {
        const EdgeId bootstrap_edge_id = next_edge_id_++;
        graph_.edges.push_back(EdgeDefinition{
            bootstrap_edge_id,
            node_ids_by_name_.at(kInternalBootstrapNodeName),
            *entry_node_id_,
            EdgeKind::OnSuccess,
            nullptr,
            1000U
        });
        outgoing_edges[node_ids_by_name_.at(kInternalBootstrapNodeName)].push_back(bootstrap_edge_id);
    }
    for (const auto& edge : edges_) {
        const EdgeId edge_id = next_edge_id_++;
        graph_.edges.push_back(EdgeDefinition{
            edge_id,
            edge.first,
            edge.second,
            EdgeKind::OnSuccess,
            nullptr,
            100U
        });
        outgoing_edges[edge.first].push_back(edge_id);
    }
    for (const auto& [node_id, edge_ids] : outgoing_edges) {
        graph_.bind_outgoing_edges(node_id, edge_ids);
    }
    graph_.sort_edges_by_priority();
    graph_.compile_runtime();

    std::string validation_error;
    if (!graph_.validate(&validation_error)) {
        *error_message = validation_error;
        return false;
    }

    finalized_ = true;
    return true;
}

bool GraphHandle::execute_run(
    PyObject* input_state,
    PyObject* config,
    bool include_subgraphs,
    bool allow_paused,
    RunArtifacts* artifacts,
    std::string* error_message
) {
    if (!ensure_finalized(error_message)) {
        return false;
    }

    InputEnvelope input;
    if (!build_initial_envelope(input_state, config, &input, error_message)) {
        return false;
    }

    const RunId run_id = engine_.start(graph_, input);
    capture_run_context(run_id, input_state, config);
    PyThreadState* released_thread_state = PyEval_SaveThread();
    const RunResult run_result = engine_.run_to_completion(run_id);
    PyEval_RestoreThread(released_thread_state);

    if (run_result.status == ExecutionStatus::Completed ||
        run_result.status == ExecutionStatus::Failed ||
        run_result.status == ExecutionStatus::Cancelled) {
        release_run_context(run_id);
    }

    if (run_result.status != ExecutionStatus::Completed &&
        !(allow_paused && run_result.status == ExecutionStatus::Paused)) {
        if (run_result.status == ExecutionStatus::Paused) {
            release_run_context(run_id);
        }
        *error_message = "graph execution did not reach an allowed terminal status: " +
            execution_status_name(run_result.status);
        return false;
    }

    return populate_run_artifacts(run_id, run_result, include_subgraphs, artifacts, error_message);
}

bool GraphHandle::populate_run_artifacts(
    RunId run_id,
    const RunResult& run_result,
    bool include_subgraphs,
    RunArtifacts* artifacts,
    std::string* error_message
) {
    const StateStore& state_store = engine_.state_store(run_id);
    artifacts->state = state_to_python_dict(
        engine_.state(run_id),
        state_store.blobs(),
        state_store.strings(),
        error_message
    );
    if (artifacts->state == nullptr) {
        return false;
    }

    artifacts->trace = build_trace_list(run_id, include_subgraphs, error_message);
    if (artifacts->trace == nullptr) {
        Py_DECREF(artifacts->state);
        artifacts->state = nullptr;
        return false;
    }

    artifacts->run_id = run_id;
    artifacts->result = run_result;
    if (run_result.last_checkpoint_id != 0U) {
        const auto record = engine_.checkpoints().get(run_result.last_checkpoint_id);
        if (record.has_value() && record->resumable()) {
            artifacts->proof = compute_run_proof_digest(
                *record,
                engine_.trace().events_for_run(run_id)
            );
        }
    }
    return true;
}

void GraphHandle::capture_run_context(RunId run_id, PyObject* input_state, PyObject* config) {
    std::lock_guard<std::mutex> lock(mutex_);
    PyObject* captured_input = input_state == nullptr ? Py_None : input_state;
    Py_INCREF(captured_input);
    pending_initial_inputs_[run_id] = captured_input;
    PyObject* captured_config = config == nullptr ? Py_None : config;
    Py_INCREF(captured_config);
    pending_configs_[run_id] = captured_config;
}

void GraphHandle::release_run_context(RunId run_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = pending_initial_inputs_.find(run_id);
    if (iterator != pending_initial_inputs_.end()) {
        Py_DECREF(iterator->second);
        pending_initial_inputs_.erase(iterator);
    }
    const auto config_iterator = pending_configs_.find(run_id);
    if (config_iterator != pending_configs_.end()) {
        Py_DECREF(config_iterator->second);
        pending_configs_.erase(config_iterator);
    }
}

bool GraphHandle::build_initial_envelope(
    PyObject* input_state,
    PyObject* config,
    InputEnvelope* input,
    std::string* error_message
) {
    input->initial_patch = StatePatch{};
    input->runtime_config_payload.clear();
    if (input_state != nullptr && input_state != Py_None && !PyMapping_Check(input_state)) {
        *error_message = "input state must be a mapping";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    input->initial_field_count = state_names_.size();
    const bool requires_runtime_config_payload = std::any_of(
        node_bindings_.begin(),
        node_bindings_.end(),
        [](const NodeBinding& binding) {
            return binding.subgraph_handle != nullptr;
        }
    );
    if (config != nullptr && config != Py_None &&
        !dump_object_as_pickle_bytes(config, &input->runtime_config_payload, error_message)) {
        if (requires_runtime_config_payload) {
            *error_message =
                "graph config must be pickle-serializable when subgraph nodes are present: " +
                *error_message;
            return false;
        }
        input->runtime_config_payload.clear();
    }
    return true;
}

bool GraphHandle::convert_mapping_to_patch(
    PyObject* mapping,
    BlobStore& blobs,
    StringInterner& strings,
    StatePatch* patch,
    std::string* error_message
) {
    PyObject* items = PyMapping_Items(mapping);
    if (items == nullptr) {
        *error_message = fetch_python_error();
        return false;
    }
    PyObject* sequence = PySequence_Fast(items, "mapping items must be a sequence");
    Py_DECREF(items);
    if (sequence == nullptr) {
        *error_message = fetch_python_error();
        return false;
    }

    const Py_ssize_t item_count = PySequence_Fast_GET_SIZE(sequence);
    PyObject** item_values = PySequence_Fast_ITEMS(sequence);
    patch->updates.reserve(
        patch->updates.size() + static_cast<std::size_t>(std::max<Py_ssize_t>(item_count, 0))
    );

    for (Py_ssize_t index = 0; index < item_count; ++index) {
        PyObject* item = item_values[index];
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            Py_DECREF(sequence);
            *error_message = "mapping entries must be key/value pairs";
            return false;
        }
        PyObject* key = PyTuple_GET_ITEM(item, 0);
        PyObject* value = PyTuple_GET_ITEM(item, 1);
        if (!PyUnicode_Check(key)) {
            Py_DECREF(sequence);
            *error_message = "state keys must be strings";
            return false;
        }

        std::string key_name = python_string(key, error_message);
        if (!error_message->empty()) {
            Py_DECREF(sequence);
            return false;
        }

        Value converted_value;
        if (!convert_python_value(value, blobs, strings, &converted_value, error_message)) {
            Py_DECREF(sequence);
            return false;
        }

        const StateKey state_key = [&]() {
            return ensure_state_key(key_name);
        }();

        patch->updates.push_back(FieldUpdate{state_key, std::move(converted_value)});
    }

    Py_DECREF(sequence);
    return true;
}

bool GraphHandle::parse_callback_result(
    PyObject* result,
    BlobStore& blobs,
    StringInterner& strings,
    StatePatch* patch,
    std::optional<NodeId>* next_override,
    bool* should_wait,
    std::string* error_message
) {
    if (should_wait != nullptr) {
        *should_wait = false;
    }

    if (result == Py_None) {
        return true;
    }

    auto parse_goto = [&](PyObject* goto_object) -> bool {
        if (goto_object == Py_None) {
            return true;
        }
        if (!PyUnicode_Check(goto_object)) {
            *error_message = "goto target must be a string";
            return false;
        }

        std::string target_name = python_string(goto_object, error_message);
        if (!error_message->empty()) {
            return false;
        }

        const std::optional<NodeId> node_id = lookup_node_id(target_name);
        if (!node_id.has_value()) {
            *error_message = "unknown goto target: " + target_name;
            return false;
        }
        *next_override = *node_id;
        return true;
    };

    if (PyUnicode_Check(result)) {
        return parse_goto(result);
    }
    if (PyTuple_Check(result) || PyList_Check(result)) {
        PyObject* sequence = PySequence_Fast(result, "tuple/list callback result must be sequence");
        if (sequence == nullptr) {
            *error_message = fetch_python_error();
            return false;
        }
        const Py_ssize_t size = PySequence_Fast_GET_SIZE(sequence);
        if (size != 2 && size != 3) {
            Py_DECREF(sequence);
            *error_message = "tuple/list callback result must contain exactly two or three elements";
            return false;
        }
        PyObject* updates = PySequence_Fast_GET_ITEM(sequence, 0);
        PyObject* goto_target = PySequence_Fast_GET_ITEM(sequence, 1);
        PyObject* wait_object = size == 3 ? PySequence_Fast_GET_ITEM(sequence, 2) : Py_False;
        const bool updates_ok = (updates == Py_None) ||
            convert_mapping_to_patch(updates, blobs, strings, patch, error_message);
        if (!updates_ok) {
            Py_DECREF(sequence);
            return false;
        }
        if (!PyBool_Check(wait_object)) {
            Py_DECREF(sequence);
            *error_message = "optional wait flag must be a bool";
            return false;
        }
        if (wait_object == Py_True && goto_target != Py_None) {
            Py_DECREF(sequence);
            *error_message = "waiting callback results must not specify goto targets";
            return false;
        }
        if (should_wait != nullptr) {
            *should_wait = wait_object == Py_True;
        }
        const bool goto_ok = parse_goto(goto_target);
        Py_DECREF(sequence);
        return goto_ok;
    }
    if (PyMapping_Check(result)) {
        return convert_mapping_to_patch(result, blobs, strings, patch, error_message);
    }

    *error_message = "callback result must be a mapping, string, tuple, list, or None";
    return false;
}

bool GraphHandle::convert_python_value(
    PyObject* value,
    BlobStore& blobs,
    StringInterner& strings,
    Value* output,
    std::string* error_message
) {
    if (value == Py_None) {
        *output = std::monostate{};
        return true;
    }
    if (PyBool_Check(value)) {
        *output = (value == Py_True);
        return true;
    }
    if (PyLong_Check(value)) {
        int overflow = 0;
        const long long converted = PyLong_AsLongLongAndOverflow(value, &overflow);
        if (overflow != 0 || (converted == -1 && PyErr_Occurred() != nullptr)) {
            *error_message = "integer state value is outside the int64 range";
            PyErr_Clear();
            return false;
        }
        *output = static_cast<int64_t>(converted);
        return true;
    }
    if (PyFloat_Check(value)) {
        const double converted = PyFloat_AsDouble(value);
        if (converted == -1.0 && PyErr_Occurred() != nullptr) {
            *error_message = fetch_python_error();
            return false;
        }
        *output = converted;
        return true;
    }
    if (PyUnicode_Check(value)) {
        std::string text = python_string(value, error_message);
        if (!error_message->empty()) {
            return false;
        }
        *output = strings.intern(text);
        return true;
    }
    if (PyBytes_Check(value)) {
        char* bytes = nullptr;
        Py_ssize_t size = 0;
        if (PyBytes_AsStringAndSize(value, &bytes, &size) != 0) {
            *error_message = fetch_python_error();
            return false;
        }
        *output = append_tagged_blob(
            blobs,
            kBytesBlobTag,
            reinterpret_cast<const std::byte*>(bytes),
            static_cast<std::size_t>(size)
        );
        return true;
    }
    if (PyByteArray_Check(value)) {
        const auto* bytes = reinterpret_cast<const std::byte*>(PyByteArray_AsString(value));
        const std::size_t size = static_cast<std::size_t>(PyByteArray_Size(value));
        *output = append_tagged_blob(blobs, kBytesBlobTag, bytes, size);
        return true;
    }

    std::string serialized_json;
    if (!dump_object_as_json(value, &serialized_json, error_message)) {
        return false;
    }
    *output = append_tagged_blob(
        blobs,
        kJsonBlobTag,
        reinterpret_cast<const std::byte*>(serialized_json.data()),
        serialized_json.size()
    );
    return true;
}

PyObject* GraphHandle::state_to_python_dict(
    const WorkflowState& state,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) const {
    std::vector<std::string> state_names;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_names = state_names_;
    }

    PyObject* state_dict = PyDict_New();
    if (state_dict == nullptr) {
        *error_message = fetch_python_error();
        return nullptr;
    }

    const std::size_t field_count = std::min(state.fields.size(), state_names.size());
    for (std::size_t index = 0; index < field_count; ++index) {
        const Value& value = state.fields[index];
        if (std::holds_alternative<std::monostate>(value)) {
            continue;
        }

        PyObject* python_value = convert_value_to_python(value, blobs, strings, error_message);
        if (python_value == nullptr) {
            Py_DECREF(state_dict);
            return nullptr;
        }
        if (PyDict_SetItemString(state_dict, state_names[index].c_str(), python_value) != 0) {
            Py_DECREF(python_value);
            Py_DECREF(state_dict);
            *error_message = fetch_python_error();
            return nullptr;
        }
        Py_DECREF(python_value);
    }

    return state_dict;
}

PyObject* GraphHandle::convert_value_to_python(
    const Value& value,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) const {
    if (std::holds_alternative<int64_t>(value)) {
        return PyLong_FromLongLong(std::get<int64_t>(value));
    }
    if (std::holds_alternative<double>(value)) {
        return PyFloat_FromDouble(std::get<double>(value));
    }
    if (std::holds_alternative<bool>(value)) {
        return PyBool_FromLong(std::get<bool>(value) ? 1 : 0);
    }
    if (std::holds_alternative<InternedStringId>(value)) {
        return unicode_from_utf8(strings.resolve(std::get<InternedStringId>(value)));
    }
    if (std::holds_alternative<BlobRef>(value)) {
        const std::vector<std::byte> bytes = blobs.read_bytes(std::get<BlobRef>(value));
        if (tagged_blob_has_payload(bytes, kJsonBlobTag)) {
            return load_object_from_json_bytes(bytes, error_message);
        }
        if (tagged_blob_has_payload(bytes, kBytesBlobTag)) {
            return PyBytes_FromStringAndSize(
                reinterpret_cast<const char*>(bytes.data() + 1),
                static_cast<Py_ssize_t>(bytes.size() - 1U)
            );
        }
        return PyBytes_FromStringAndSize(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<Py_ssize_t>(bytes.size())
        );
    }
    Py_RETURN_NONE;
}

PyObject* GraphHandle::build_trace_list(
    RunId run_id,
    bool include_subgraphs,
    std::string* error_message
) const {
    StreamCursor cursor;
    const std::vector<StreamEvent> events = engine_.stream_events(
        run_id,
        cursor,
        StreamReadOptions{include_subgraphs}
    );

    PyObject* trace_list = PyList_New(0);
    if (trace_list == nullptr) {
        *error_message = fetch_python_error();
        return nullptr;
    }

    for (const StreamEvent& event : events) {
        if (is_internal_node(event.node_id)) {
            continue;
        }
        PyObject* python_event = event_to_python(*this, event, error_message);
        if (python_event == nullptr) {
            Py_DECREF(trace_list);
            return nullptr;
        }
        if (PyList_Append(trace_list, python_event) != 0) {
            Py_DECREF(python_event);
            Py_DECREF(trace_list);
            *error_message = fetch_python_error();
            return nullptr;
        }
        Py_DECREF(python_event);
    }

    return trace_list;
}

PyObject* GraphHandle::build_details_dict(
    const RunArtifacts& artifacts,
    std::string* error_message
) const {
    PyObject* summary = PyDict_New();
    PyObject* proof = PyDict_New();
    PyObject* details = PyDict_New();
    if (summary == nullptr || proof == nullptr || details == nullptr) {
        Py_XDECREF(summary);
        Py_XDECREF(proof);
        Py_XDECREF(details);
        *error_message = fetch_python_error();
        return nullptr;
    }

    const bool summary_ok =
        set_dict_item(summary, "run_id", PyLong_FromUnsignedLongLong(artifacts.run_id)) &&
        set_dict_item(
            summary,
            "status",
            unicode_from_utf8(execution_status_name(artifacts.result.status))
        ) &&
        set_dict_item(
            summary,
            "steps_executed",
            PyLong_FromUnsignedLongLong(artifacts.result.steps_executed)
        ) &&
        set_dict_item(
            summary,
            "checkpoint_id",
            PyLong_FromUnsignedLongLong(artifacts.result.last_checkpoint_id)
        ) &&
        set_dict_item(summary, "graph_id", PyLong_FromUnsignedLong(graph_id_)) &&
        set_dict_item(summary, "graph_name", unicode_from_utf8(name_));

    const bool proof_ok =
        set_dict_item(
            proof,
            "snapshot_digest",
            PyLong_FromUnsignedLongLong(artifacts.proof.snapshot_digest)
        ) &&
        set_dict_item(
            proof,
            "trace_digest",
            PyLong_FromUnsignedLongLong(artifacts.proof.trace_digest)
        ) &&
        set_dict_item(
            proof,
            "combined_digest",
            PyLong_FromUnsignedLongLong(artifacts.proof.combined_digest)
        );

    const bool details_ok =
        summary_ok &&
        proof_ok &&
        PyDict_SetItemString(details, "state", artifacts.state) == 0 &&
        PyDict_SetItemString(details, "trace", artifacts.trace) == 0 &&
        PyDict_SetItemString(details, "summary", summary) == 0 &&
        PyDict_SetItemString(details, "proof", proof) == 0;

    Py_DECREF(summary);
    Py_DECREF(proof);

    if (!details_ok) {
        Py_DECREF(details);
        *error_message = fetch_python_error();
        return nullptr;
    }

    return details;
}

StateKey GraphHandle::ensure_state_key(std::string_view state_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ensure_state_key_locked(state_name);
}

StateKey GraphHandle::ensure_state_key_locked(std::string_view state_name) {
    const std::string key_name(state_name);
    const auto iterator = state_keys_.find(key_name);
    if (iterator != state_keys_.end()) {
        return iterator->second;
    }

    const StateKey next_key = static_cast<StateKey>(state_names_.size());
    state_names_.push_back(key_name);
    state_keys_.emplace(state_names_.back(), next_key);
    return next_key;
}

void GraphHandle::register_graphs_recursive(
    ExecutionEngine& engine,
    std::unordered_set<GraphId>* visited
) const {
    std::unordered_set<GraphId> local_visited;
    std::unordered_set<GraphId>& seen = visited == nullptr ? local_visited : *visited;

    std::vector<const GraphHandle*> child_handles;
    GraphDefinition graph_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        graph_copy = graph_;
        for (const NodeBinding& binding : node_bindings_) {
            if (binding.subgraph_handle != nullptr) {
                child_handles.push_back(binding.subgraph_handle);
            }
        }
    }

    if (!seen.insert(graph_copy.id).second) {
        return;
    }

    engine.register_graph(graph_copy);
    for (const GraphHandle* child_handle : child_handles) {
        child_handle->register_graphs_recursive(engine, &seen);
    }
}

std::optional<NodeId> GraphHandle::lookup_node_id(std::string_view node_name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = node_ids_by_name_.find(std::string(node_name));
    if (iterator == node_ids_by_name_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

const GraphHandle::NodeBinding* GraphHandle::lookup_node_binding(NodeId node_id) const {
    const auto iterator = std::find_if(
        node_bindings_.begin(),
        node_bindings_.end(),
        [node_id](const NodeBinding& binding) { return binding.node_id == node_id; }
    );
    return iterator == node_bindings_.end() ? nullptr : &(*iterator);
}

bool GraphHandle::is_internal_node(NodeId node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const NodeBinding* binding = lookup_node_binding(node_id); binding != nullptr) {
        return binding->name == kInternalEndNodeName ||
            binding->name == kInternalBootstrapNodeName;
    }
    return false;
}

std::string GraphHandle::node_name(NodeId node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const NodeBinding* binding = lookup_node_binding(node_id); binding != nullptr) {
        if (binding->name == kInternalBootstrapNodeName) {
            return "START";
        }
        if (binding->name == kInternalEndNodeName) {
            return "END";
        }
        return binding->name;
    }
    return {};
}

std::string GraphHandle::fetch_python_error() {
    if (PyErr_Occurred() == nullptr) {
        return {};
    }

    PyObject* exception_type = nullptr;
    PyObject* exception_value = nullptr;
    PyObject* traceback = nullptr;
    PyErr_Fetch(&exception_type, &exception_value, &traceback);
    PyErr_NormalizeException(&exception_type, &exception_value, &traceback);

    std::string message = "python execution failed";
    if (exception_value != nullptr) {
        PyObject* rendered = PyObject_Str(exception_value);
        if (rendered != nullptr) {
            message = python_string(rendered, &message);
            Py_DECREF(rendered);
        }
    }

    Py_XDECREF(exception_type);
    Py_XDECREF(exception_value);
    Py_XDECREF(traceback);
    return message;
}

std::string GraphHandle::node_status_name(NodeResult::Status status) {
    switch (status) {
        case NodeResult::Success:
            return "success";
        case NodeResult::SoftFail:
            return "soft_fail";
        case NodeResult::HardFail:
            return "hard_fail";
        case NodeResult::Waiting:
            return "waiting";
        case NodeResult::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

std::string GraphHandle::execution_status_name(ExecutionStatus status) {
    switch (status) {
        case ExecutionStatus::NotStarted:
            return "not_started";
        case ExecutionStatus::Running:
            return "running";
        case ExecutionStatus::Paused:
            return "paused";
        case ExecutionStatus::Completed:
            return "completed";
        case ExecutionStatus::Failed:
            return "failed";
        case ExecutionStatus::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

std::unique_ptr<GraphHandle> create_graph_handle(std::string name, std::size_t worker_count) {
    return std::make_unique<GraphHandle>(std::move(name), worker_count, next_python_graph_id());
}

PyObject* create_graph_capsule(std::unique_ptr<GraphHandle> handle) {
    return PyCapsule_New(handle.release(), kGraphCapsuleName, destroy_graph_capsule);
}

GraphHandle* graph_handle_from_capsule(PyObject* capsule) {
    return static_cast<GraphHandle*>(PyCapsule_GetPointer(capsule, kGraphCapsuleName));
}

PyObject* create_runtime_capsule(ExecutionContext& context) {
    auto* handle = new CallbackRuntimeHandle{&context};
    return PyCapsule_New(handle, kRuntimeCapsuleName, destroy_runtime_capsule);
}

PyObject* runtime_record_once(
    PyObject* runtime_capsule,
    std::string_view key,
    PyObject* request,
    PyObject* producer,
    std::string* error_message
) {
    ExecutionContext* context = runtime_context_from_capsule(runtime_capsule, error_message);
    if (context == nullptr) {
        return nullptr;
    }
    if (request == nullptr) {
        request = Py_None;
    }
    if (producer == nullptr || !PyCallable_Check(producer)) {
        *error_message = "producer must be callable";
        return nullptr;
    }

    std::vector<std::byte> request_bytes;
    if (!dump_object_as_pickle_bytes(request, &request_bytes, error_message)) {
        return nullptr;
    }

    if (const auto existing = context->find_recorded_effect(key); existing.has_value()) {
        if (!pickle_blob_payload_matches(context->blobs, existing->request, request_bytes)) {
            *error_message = "recorded effect request mismatch for key";
            return nullptr;
        }

        PyObject* value = load_pickled_blob_value(context->blobs, existing->output, error_message);
        if (value == nullptr) {
            return nullptr;
        }

        PyObject* result = PyDict_New();
        if (result == nullptr) {
            Py_DECREF(value);
            *error_message = GraphHandle::fetch_python_error();
            return nullptr;
        }
        const bool ok =
            set_dict_item(result, "value", value) &&
            set_dict_item(result, "replayed", PyBool_FromLong(1)) &&
            set_dict_item(result, "flags", PyLong_FromUnsignedLong(existing->flags));
        if (!ok) {
            Py_DECREF(result);
            *error_message = GraphHandle::fetch_python_error();
            return nullptr;
        }
        return result;
    }

    const BlobRef request_ref = append_tagged_blob(
        context->blobs,
        kPickleBlobTag,
        request_bytes.data(),
        request_bytes.size()
    );

    struct PythonProducerRaised final {};

    try {
        const RecordedEffectResult effect = context->record_blob_effect_once(
            key,
            request_ref,
            [&]() -> BlobRef {
                PyObject* produced = PyObject_CallFunctionObjArgs(producer, nullptr);
                if (produced == nullptr) {
                    throw PythonProducerRaised{};
                }

                std::vector<std::byte> output_bytes;
                std::string producer_error;
                const bool serialized = dump_object_as_pickle_bytes(
                    produced,
                    &output_bytes,
                    &producer_error
                );
                Py_DECREF(produced);
                if (!serialized) {
                    throw std::runtime_error(producer_error);
                }

                return append_tagged_blob(
                    context->blobs,
                    kPickleBlobTag,
                    output_bytes.data(),
                    output_bytes.size()
                );
            }
        );

        PyObject* value = load_pickled_blob_value(context->blobs, effect.output, error_message);
        if (value == nullptr) {
            return nullptr;
        }

        PyObject* result = PyDict_New();
        if (result == nullptr) {
            Py_DECREF(value);
            *error_message = GraphHandle::fetch_python_error();
            return nullptr;
        }
        const bool ok =
            set_dict_item(result, "value", value) &&
            set_dict_item(result, "replayed", PyBool_FromLong(effect.replayed ? 1 : 0)) &&
            set_dict_item(result, "flags", PyLong_FromUnsignedLong(effect.flags));
        if (!ok) {
            Py_DECREF(result);
            *error_message = GraphHandle::fetch_python_error();
            return nullptr;
        }
        return result;
    } catch (const PythonProducerRaised&) {
        return nullptr;
    } catch (const std::exception& error) {
        *error_message = error.what();
        return nullptr;
    }
}

NodeResult python_bootstrap_executor(ExecutionContext& context) {
    GraphHandle* handle = GraphHandleRegistry::instance().find(context.graph_id);
    if (handle == nullptr) {
        NodeResult result;
        result.status = NodeResult::HardFail;
        result.flags = kToolFlagValidationError;
        return result;
    }

    const PyGILState_STATE gil_state = PyGILState_Ensure();
    PyObject* input_state = Py_None;
    {
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const auto iterator = handle->pending_initial_inputs_.find(context.run_id);
        if (iterator != handle->pending_initial_inputs_.end()) {
            input_state = iterator->second;
            Py_INCREF(input_state);
        } else {
            Py_INCREF(Py_None);
        }
    }

    NodeResult result = NodeResult::success();
    if (input_state != Py_None) {
        StatePatch patch;
        std::string error_message;
        if (!handle->convert_mapping_to_patch(
                input_state,
                context.blobs,
                context.strings,
                &patch,
                &error_message
            )) {
            Py_DECREF(input_state);
            PyGILState_Release(gil_state);
            result.status = NodeResult::HardFail;
            result.flags = kToolFlagValidationError;
            return result;
        }
        result.patch = std::move(patch);
    }

    Py_DECREF(input_state);
    PyGILState_Release(gil_state);
    return result;
}

NodeResult python_node_executor(ExecutionContext& context) {
    GraphHandle* handle = GraphHandleRegistry::instance().find(context.graph_id);
    if (handle == nullptr) {
        NodeResult result;
        result.status = NodeResult::HardFail;
        result.flags = kToolFlagValidationError;
        return result;
    }

    const PyGILState_STATE gil_state = PyGILState_Ensure();

    PyObject* callback = nullptr;
    PyObject* config = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mutex_);
        if (const GraphHandle::NodeBinding* binding = handle->lookup_node_binding(context.node_id);
            binding != nullptr) {
            callback = binding->callback;
            Py_XINCREF(callback);
        }
        const auto config_iterator = handle->pending_configs_.find(context.run_id);
        if (config_iterator != handle->pending_configs_.end()) {
            config = config_iterator->second;
            Py_INCREF(config);
        }
    }

    std::string error_message;
    if (config == nullptr) {
        if (!context.runtime_config_payload.empty()) {
            config = load_object_from_pickle_bytes(context.runtime_config_payload, &error_message);
            if (config == nullptr) {
                Py_XDECREF(callback);
                PyGILState_Release(gil_state);
                NodeResult result;
                result.status = NodeResult::HardFail;
                result.flags = kToolFlagValidationError;
                return result;
            }
        } else {
            config = PyDict_New();
            if (config == nullptr) {
                Py_XDECREF(callback);
                PyGILState_Release(gil_state);
                NodeResult result;
                result.status = NodeResult::HardFail;
                result.flags = kToolFlagHandlerException;
                return result;
            }
        }
    }

    if (callback == nullptr) {
        Py_DECREF(config);
        PyGILState_Release(gil_state);
        return NodeResult::success();
    }

    PyObject* state_dict = handle->state_to_python_dict(
        context.state,
        context.blobs,
        context.strings,
        &error_message
    );
    if (state_dict == nullptr) {
        Py_DECREF(callback);
        Py_DECREF(config);
        PyGILState_Release(gil_state);
        NodeResult result;
        result.status = NodeResult::HardFail;
        result.flags = kToolFlagHandlerException;
        return result;
    }

    PyObject* callback_config = config;
    Py_INCREF(callback_config);
    if (PyDict_Check(config)) {
        PyObject* runtime_capsule = create_runtime_capsule(context);
        if (runtime_capsule == nullptr) {
            Py_DECREF(state_dict);
            Py_DECREF(callback);
            Py_DECREF(callback_config);
            Py_DECREF(config);
            PyGILState_Release(gil_state);
            NodeResult result;
            result.status = NodeResult::HardFail;
            result.flags = kToolFlagHandlerException;
            return result;
        }

        PyObject* config_with_runtime = PyDict_Copy(config);
        if (config_with_runtime == nullptr ||
            PyDict_SetItemString(config_with_runtime, kPythonRuntimeConfigKey, runtime_capsule) != 0) {
            Py_XDECREF(config_with_runtime);
            Py_DECREF(runtime_capsule);
            Py_DECREF(state_dict);
            Py_DECREF(callback);
            Py_DECREF(callback_config);
            Py_DECREF(config);
            PyGILState_Release(gil_state);
            NodeResult result;
            result.status = NodeResult::HardFail;
            result.flags = kToolFlagHandlerException;
            return result;
        }

        Py_DECREF(callback_config);
        callback_config = config_with_runtime;
        Py_DECREF(runtime_capsule);
    }

    PyObject* callback_result = PyObject_CallFunctionObjArgs(callback, state_dict, callback_config, nullptr);
    Py_DECREF(state_dict);
    Py_DECREF(callback);
    Py_DECREF(callback_config);
    Py_DECREF(config);
    if (callback_result == nullptr) {
        error_message = GraphHandle::fetch_python_error();
        PyGILState_Release(gil_state);
        NodeResult result;
        result.status = NodeResult::HardFail;
        result.flags = kToolFlagHandlerException;
        return result;
    }

    StatePatch patch;
    std::optional<NodeId> next_override;
    bool should_wait = false;
    const bool parsed = handle->parse_callback_result(
        callback_result,
        context.blobs,
        context.strings,
        &patch,
        &next_override,
        &should_wait,
        &error_message
    );
    Py_DECREF(callback_result);
    PyGILState_Release(gil_state);

    if (!parsed) {
        NodeResult result;
        result.status = NodeResult::HardFail;
        result.flags = kToolFlagValidationError;
        return result;
    }

    NodeResult result = should_wait
        ? NodeResult::waiting(std::move(patch), 1.0F)
        : NodeResult::success(std::move(patch), 1.0F);
    result.next_override = next_override;
    return result;
}

NodeResult native_stop_executor(ExecutionContext&) {
    return NodeResult::success();
}

} // namespace agentcore::python_binding
