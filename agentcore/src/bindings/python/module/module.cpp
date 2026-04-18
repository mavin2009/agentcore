#include "bridge.h"

#include "agentcore/adapters/adapter_factories.h"

#include <Python.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace agentcore::python_binding {

namespace {

GraphHandle* require_graph_handle(PyObject* capsule) {
    GraphHandle* handle = graph_handle_from_capsule(capsule);
    if (handle == nullptr) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, "invalid native graph handle");
        }
    }
    return handle;
}

bool parse_python_string(PyObject* object, std::string* value, std::string* error_message) {
    if (!PyUnicode_Check(object)) {
        *error_message = "expected a string value";
        return false;
    }

    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(object, &size);
    if (utf8 == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }

    *value = std::string(utf8, static_cast<std::size_t>(size));
    return true;
}

bool parse_optional_python_string(
    PyObject* object,
    std::string* value,
    std::string* error_message
) {
    if (object == nullptr || object == Py_None) {
        value->clear();
        return true;
    }
    return parse_python_string(object, value, error_message);
}

bool parse_node_kind(PyObject* object, NodeKind* kind, std::string* error_message) {
    if (object == nullptr || object == Py_None) {
        *kind = NodeKind::Compute;
        return true;
    }

    std::string text;
    if (!parse_python_string(object, &text, error_message)) {
        *error_message = "node kind must be a string";
        return false;
    }

    if (text == "compute") {
        *kind = NodeKind::Compute;
        return true;
    }
    if (text == "control") {
        *kind = NodeKind::Control;
        return true;
    }
    if (text == "tool") {
        *kind = NodeKind::Tool;
        return true;
    }
    if (text == "model") {
        *kind = NodeKind::Model;
        return true;
    }
    if (text == "aggregate") {
        *kind = NodeKind::Aggregate;
        return true;
    }
    if (text == "human") {
        *kind = NodeKind::Human;
        return true;
    }
    if (text == "subgraph") {
        *kind = NodeKind::Subgraph;
        return true;
    }

    *error_message = "unsupported node kind: " + text;
    return false;
}

bool parse_join_merge_strategy(
    const std::string& text,
    JoinMergeStrategy* strategy,
    std::string* error_message
) {
    if (text == "require_equal") {
        *strategy = JoinMergeStrategy::RequireEqual;
        return true;
    }
    if (text == "require_single_writer") {
        *strategy = JoinMergeStrategy::RequireSingleWriter;
        return true;
    }
    if (text == "last_writer_wins") {
        *strategy = JoinMergeStrategy::LastWriterWins;
        return true;
    }
    if (text == "first_writer_wins") {
        *strategy = JoinMergeStrategy::FirstWriterWins;
        return true;
    }
    if (text == "sum_int64") {
        *strategy = JoinMergeStrategy::SumInt64;
        return true;
    }
    if (text == "max_int64") {
        *strategy = JoinMergeStrategy::MaxInt64;
        return true;
    }
    if (text == "min_int64") {
        *strategy = JoinMergeStrategy::MinInt64;
        return true;
    }
    if (text == "logical_or") {
        *strategy = JoinMergeStrategy::LogicalOr;
        return true;
    }
    if (text == "logical_and") {
        *strategy = JoinMergeStrategy::LogicalAnd;
        return true;
    }

    *error_message = "unsupported join merge strategy: " + text;
    return false;
}

bool parse_string_mapping(
    PyObject* object,
    std::vector<std::pair<std::string, std::string>>* entries,
    std::string* error_message
) {
    entries->clear();
    if (object == nullptr || object == Py_None) {
        return true;
    }
    if (!PyMapping_Check(object)) {
        *error_message = "expected a mapping of string keys to string values";
        return false;
    }

    PyObject* items = PyMapping_Items(object);
    if (items == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    PyObject* sequence = PySequence_Fast(items, "mapping items must be a sequence");
    Py_DECREF(items);
    if (sequence == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }

    const Py_ssize_t item_count = PySequence_Fast_GET_SIZE(sequence);
    PyObject** item_values = PySequence_Fast_ITEMS(sequence);
    entries->reserve(static_cast<std::size_t>(item_count));

    for (Py_ssize_t index = 0; index < item_count; ++index) {
        PyObject* item = item_values[index];
        if (!PyTuple_Check(item) || PyTuple_GET_SIZE(item) != 2) {
            Py_DECREF(sequence);
            *error_message = "mapping entries must be key/value pairs";
            return false;
        }

        std::string key;
        std::string value;
        if (!parse_python_string(PyTuple_GET_ITEM(item, 0), &key, error_message) ||
            !parse_python_string(PyTuple_GET_ITEM(item, 1), &value, error_message)) {
            Py_DECREF(sequence);
            return false;
        }

        entries->push_back(std::make_pair(std::move(key), std::move(value)));
    }

    Py_DECREF(sequence);
    return true;
}

bool parse_merge_rules(
    PyObject* object,
    std::vector<std::pair<std::string, JoinMergeStrategy>>* merge_rules,
    std::string* error_message
) {
    merge_rules->clear();

    std::vector<std::pair<std::string, std::string>> raw_entries;
    if (!parse_string_mapping(object, &raw_entries, error_message)) {
        return false;
    }

    merge_rules->reserve(raw_entries.size());
    for (const auto& [field_name, strategy_name] : raw_entries) {
        JoinMergeStrategy strategy = JoinMergeStrategy::RequireEqual;
        if (!parse_join_merge_strategy(strategy_name, &strategy, error_message)) {
            return false;
        }
        merge_rules->push_back(std::make_pair(field_name, strategy));
    }
    return true;
}

bool mapping_contains_key(PyObject* mapping, const char* key, std::string* error_message) {
    const int contains = PyMapping_HasKeyString(mapping, key);
    if (contains < 0) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    return contains != 0;
}

bool get_mapping_item(
    PyObject* mapping,
    const char* key,
    PyObject** value,
    std::string* error_message
) {
    *value = nullptr;
    if (mapping == nullptr || mapping == Py_None) {
        return true;
    }
    if (!PyMapping_Check(mapping)) {
        *error_message = "expected a mapping";
        return false;
    }
    const int contains = PyMapping_HasKeyString(mapping, key);
    if (contains < 0) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    if (contains == 0) {
        return true;
    }
    *value = PyMapping_GetItemString(mapping, key);
    if (*value == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    return true;
}

bool parse_bool_from_mapping(
    PyObject* mapping,
    const char* key,
    bool default_value,
    bool* value,
    std::string* error_message
) {
    *value = default_value;
    PyObject* item = nullptr;
    if (!get_mapping_item(mapping, key, &item, error_message)) {
        return false;
    }
    if (item == nullptr || item == Py_None) {
        Py_XDECREF(item);
        return true;
    }
    const int truth = PyObject_IsTrue(item);
    Py_DECREF(item);
    if (truth < 0) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    *value = truth != 0;
    return true;
}

bool parse_uint32_from_mapping(
    PyObject* mapping,
    const char* key,
    uint32_t default_value,
    uint32_t* value,
    std::string* error_message
) {
    *value = default_value;
    PyObject* item = nullptr;
    if (!get_mapping_item(mapping, key, &item, error_message)) {
        return false;
    }
    if (item == nullptr || item == Py_None) {
        Py_XDECREF(item);
        return true;
    }
    const unsigned long long parsed = PyLong_AsUnsignedLongLong(item);
    Py_DECREF(item);
    if (PyErr_Occurred() != nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        PyErr_Clear();
        return false;
    }
    *value = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_uint16_from_mapping(
    PyObject* mapping,
    const char* key,
    uint16_t default_value,
    uint16_t* value,
    std::string* error_message
) {
    uint32_t parsed = default_value;
    if (!parse_uint32_from_mapping(mapping, key, default_value, &parsed, error_message)) {
        return false;
    }
    *value = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_size_t_from_mapping(
    PyObject* mapping,
    const char* key,
    std::size_t default_value,
    std::size_t* value,
    std::string* error_message
) {
    *value = default_value;
    PyObject* item = nullptr;
    if (!get_mapping_item(mapping, key, &item, error_message)) {
        return false;
    }
    if (item == nullptr || item == Py_None) {
        Py_XDECREF(item);
        return true;
    }
    const unsigned long long parsed = PyLong_AsUnsignedLongLong(item);
    Py_DECREF(item);
    if (PyErr_Occurred() != nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        PyErr_Clear();
        return false;
    }
    *value = static_cast<std::size_t>(parsed);
    return true;
}

bool parse_string_from_mapping(
    PyObject* mapping,
    const char* key,
    std::string* value,
    std::string* error_message
) {
    PyObject* item = nullptr;
    if (!get_mapping_item(mapping, key, &item, error_message)) {
        return false;
    }
    if (item == nullptr || item == Py_None) {
        Py_XDECREF(item);
        return true;
    }
    const bool ok = parse_python_string(item, value, error_message);
    Py_DECREF(item);
    return ok;
}

bool parse_headers_from_mapping(
    PyObject* mapping,
    const char* key,
    std::vector<HttpHeader>* headers,
    std::string* error_message
) {
    headers->clear();
    PyObject* item = nullptr;
    if (!get_mapping_item(mapping, key, &item, error_message)) {
        return false;
    }
    if (item == nullptr || item == Py_None) {
        Py_XDECREF(item);
        return true;
    }

    std::vector<std::pair<std::string, std::string>> entries;
    const bool ok = parse_string_mapping(item, &entries, error_message);
    Py_DECREF(item);
    if (!ok) {
        return false;
    }
    headers->reserve(entries.size());
    for (auto& [name, value] : entries) {
        headers->push_back(HttpHeader{std::move(name), std::move(value)});
    }
    return true;
}

bool parse_tool_policy(
    PyObject* mapping,
    ToolPolicy* policy,
    std::string* error_message
) {
    if (mapping == nullptr || mapping == Py_None) {
        return true;
    }
    if (!PyMapping_Check(mapping)) {
        *error_message = "tool policy must be a mapping or None";
        return false;
    }
    return parse_uint16_from_mapping(mapping, "retry_limit", policy->retry_limit, &policy->retry_limit, error_message) &&
        parse_uint32_from_mapping(mapping, "timeout_ms", policy->timeout_ms, &policy->timeout_ms, error_message) &&
        parse_size_t_from_mapping(mapping, "max_input_bytes", policy->max_input_bytes, &policy->max_input_bytes, error_message) &&
        parse_size_t_from_mapping(mapping, "max_output_bytes", policy->max_output_bytes, &policy->max_output_bytes, error_message);
}

bool parse_model_policy(
    PyObject* mapping,
    ModelPolicy* policy,
    std::string* error_message
) {
    if (mapping == nullptr || mapping == Py_None) {
        return true;
    }
    if (!PyMapping_Check(mapping)) {
        *error_message = "model policy must be a mapping or None";
        return false;
    }
    return parse_uint16_from_mapping(mapping, "retry_limit", policy->retry_limit, &policy->retry_limit, error_message) &&
        parse_uint32_from_mapping(mapping, "timeout_ms", policy->timeout_ms, &policy->timeout_ms, error_message) &&
        parse_size_t_from_mapping(mapping, "max_prompt_bytes", policy->max_prompt_bytes, &policy->max_prompt_bytes, error_message) &&
        parse_size_t_from_mapping(mapping, "max_output_bytes", policy->max_output_bytes, &policy->max_output_bytes, error_message);
}

bool parse_http_transport_options(
    PyObject* mapping,
    HttpTransportOptions* options,
    std::string* error_message
) {
    if (mapping == nullptr || mapping == Py_None) {
        return true;
    }
    if (!PyMapping_Check(mapping)) {
        *error_message = "transport options must be a mapping or None";
        return false;
    }
    return parse_string_from_mapping(mapping, "base_url", &options->base_url, error_message) &&
        parse_headers_from_mapping(mapping, "default_headers", &options->default_headers, error_message) &&
        parse_string_from_mapping(mapping, "bearer_token", &options->bearer_token, error_message) &&
        parse_string_from_mapping(mapping, "bearer_token_env_var", &options->bearer_token_env_var, error_message) &&
        parse_string_from_mapping(mapping, "api_key", &options->api_key, error_message) &&
        parse_string_from_mapping(mapping, "api_key_env_var", &options->api_key_env_var, error_message) &&
        parse_string_from_mapping(mapping, "api_key_header", &options->api_key_header, error_message);
}

bool parse_transport_kind(
    const std::string& value,
    AdapterTransportKind* transport,
    std::string* error_message
) {
    const std::string normalized = value;
    if (normalized == "unknown") {
        *transport = AdapterTransportKind::Unknown;
        return true;
    }
    if (normalized == "in_process") {
        *transport = AdapterTransportKind::InProcess;
        return true;
    }
    if (normalized == "http") {
        *transport = AdapterTransportKind::Http;
        return true;
    }
    if (normalized == "database") {
        *transport = AdapterTransportKind::Database;
        return true;
    }
    if (normalized == "filesystem") {
        *transport = AdapterTransportKind::FileSystem;
        return true;
    }
    *error_message = "unsupported adapter transport: " + normalized;
    return false;
}

bool parse_auth_kind(
    const std::string& value,
    AdapterAuthKind* auth,
    std::string* error_message
) {
    const std::string normalized = value;
    if (normalized == "unknown") {
        *auth = AdapterAuthKind::Unknown;
        return true;
    }
    if (normalized == "none") {
        *auth = AdapterAuthKind::None;
        return true;
    }
    if (normalized == "api_key") {
        *auth = AdapterAuthKind::ApiKey;
        return true;
    }
    if (normalized == "bearer_token") {
        *auth = AdapterAuthKind::BearerToken;
        return true;
    }
    if (normalized == "basic") {
        *auth = AdapterAuthKind::Basic;
        return true;
    }
    if (normalized == "connection_string") {
        *auth = AdapterAuthKind::ConnectionString;
        return true;
    }
    if (normalized == "file_path") {
        *auth = AdapterAuthKind::FilePath;
        return true;
    }
    *error_message = "unsupported adapter auth kind: " + normalized;
    return false;
}

bool parse_capability_name(
    const std::string& value,
    uint64_t* capability,
    std::string* error_message
) {
    if (value == "sync") {
        *capability = kAdapterCapabilitySync;
        return true;
    }
    if (value == "async") {
        *capability = kAdapterCapabilityAsync;
        return true;
    }
    if (value == "streaming") {
        *capability = kAdapterCapabilityStreaming;
        return true;
    }
    if (value == "structured_request") {
        *capability = kAdapterCapabilityStructuredRequest;
        return true;
    }
    if (value == "structured_response") {
        *capability = kAdapterCapabilityStructuredResponse;
        return true;
    }
    if (value == "checkpoint_safe") {
        *capability = kAdapterCapabilityCheckpointSafe;
        return true;
    }
    if (value == "external_network") {
        *capability = kAdapterCapabilityExternalNetwork;
        return true;
    }
    if (value == "local_filesystem") {
        *capability = kAdapterCapabilityLocalFilesystem;
        return true;
    }
    if (value == "json_schema") {
        *capability = kAdapterCapabilityJsonSchema;
        return true;
    }
    if (value == "tool_calling") {
        *capability = kAdapterCapabilityToolCalling;
        return true;
    }
    if (value == "sql") {
        *capability = kAdapterCapabilitySql;
        return true;
    }
    if (value == "chat_messages") {
        *capability = kAdapterCapabilityChatMessages;
        return true;
    }
    *error_message = "unsupported adapter capability: " + value;
    return false;
}

bool parse_adapter_capabilities(
    PyObject* mapping,
    const char* key,
    uint64_t* capabilities,
    std::string* error_message
) {
    PyObject* item = nullptr;
    if (!get_mapping_item(mapping, key, &item, error_message)) {
        return false;
    }
    if (item == nullptr || item == Py_None) {
        Py_XDECREF(item);
        return true;
    }

    if (PyLong_Check(item)) {
        const unsigned long long parsed = PyLong_AsUnsignedLongLong(item);
        Py_DECREF(item);
        if (PyErr_Occurred() != nullptr) {
            *error_message = GraphHandle::fetch_python_error();
            PyErr_Clear();
            return false;
        }
        *capabilities = parsed;
        return true;
    }

    PyObject* sequence = PySequence_Fast(item, "capabilities must be a sequence of strings");
    Py_DECREF(item);
    if (sequence == nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    uint64_t parsed_capabilities = 0U;
    const Py_ssize_t count = PySequence_Fast_GET_SIZE(sequence);
    PyObject** values = PySequence_Fast_ITEMS(sequence);
    for (Py_ssize_t index = 0; index < count; ++index) {
        std::string capability_name;
        uint64_t capability = 0U;
        if (!parse_python_string(values[index], &capability_name, error_message) ||
            !parse_capability_name(capability_name, &capability, error_message)) {
            Py_DECREF(sequence);
            return false;
        }
        parsed_capabilities |= capability;
    }
    Py_DECREF(sequence);
    *capabilities = parsed_capabilities;
    return true;
}

bool parse_adapter_metadata(
    PyObject* mapping,
    AdapterMetadata* metadata,
    std::string* error_message
) {
    if (mapping == nullptr || mapping == Py_None) {
        return true;
    }
    if (!PyMapping_Check(mapping)) {
        *error_message = "adapter metadata must be a mapping or None";
        return false;
    }

    std::string transport_name;
    std::string auth_name;
    if (!parse_string_from_mapping(mapping, "provider", &metadata->provider, error_message) ||
        !parse_string_from_mapping(mapping, "implementation", &metadata->implementation, error_message) ||
        !parse_string_from_mapping(mapping, "display_name", &metadata->display_name, error_message) ||
        !parse_string_from_mapping(mapping, "transport", &transport_name, error_message) ||
        !parse_string_from_mapping(mapping, "auth", &auth_name, error_message) ||
        !parse_adapter_capabilities(mapping, "capabilities", &metadata->capabilities, error_message) ||
        !parse_string_from_mapping(mapping, "request_format", &metadata->request_format, error_message) ||
        !parse_string_from_mapping(mapping, "response_format", &metadata->response_format, error_message)) {
        return false;
    }

    if (!transport_name.empty() &&
        !parse_transport_kind(transport_name, &metadata->transport, error_message)) {
        return false;
    }
    if (!auth_name.empty() && !parse_auth_kind(auth_name, &metadata->auth, error_message)) {
        return false;
    }
    return true;
}

AdapterMetadata default_python_tool_metadata(std::string_view name) {
    AdapterMetadata metadata;
    metadata.provider = "python";
    metadata.implementation = "python_callable_tool";
    metadata.display_name = std::string(name);
    metadata.transport = AdapterTransportKind::InProcess;
    metadata.auth = AdapterAuthKind::None;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe);
    metadata.request_format = "blob";
    metadata.response_format = "blob";
    return metadata;
}

AdapterMetadata default_python_model_metadata(std::string_view name) {
    AdapterMetadata metadata;
    metadata.provider = "python";
    metadata.implementation = "python_callable_model";
    metadata.display_name = std::string(name);
    metadata.transport = AdapterTransportKind::InProcess;
    metadata.auth = AdapterAuthKind::None;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe);
    metadata.request_format = "blob";
    metadata.response_format = "blob";
    return metadata;
}

bool parse_python_bytes_like(
    PyObject* object,
    std::vector<std::byte>* value,
    std::string* error_message
) {
    value->clear();
    if (object == nullptr || object == Py_None) {
        return true;
    }

    Py_buffer view{};
    if (PyObject_GetBuffer(object, &view, PyBUF_SIMPLE) != 0) {
        *error_message = "expected a bytes-like object";
        PyErr_Clear();
        return false;
    }
    const auto* begin = static_cast<const std::byte*>(view.buf);
    value->assign(begin, begin + static_cast<std::size_t>(view.len));
    PyBuffer_Release(&view);
    return true;
}

PyObject* py_create_graph(PyObject*, PyObject* args, PyObject* kwargs) {
    const char* name = nullptr;
    Py_ssize_t worker_count = 1;
    static const char* keywords[] = {"name", "worker_count", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "s|n",
            const_cast<char**>(keywords),
            &name,
            &worker_count
        )) {
        return nullptr;
    }
    if (worker_count < 1) {
        PyErr_SetString(PyExc_ValueError, "worker_count must be at least 1");
        return nullptr;
    }

    return create_graph_capsule(
        create_graph_handle(name, static_cast<std::size_t>(worker_count))
    );
}

PyObject* py_add_node(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    PyObject* callback = Py_None;
    PyObject* kind_object = Py_None;
    int stop_after = 0;
    int allow_fan_out = 0;
    int create_join_scope = 0;
    int join_incoming_branches = 0;
    PyObject* merge_rules_object = Py_None;
    static const char* keywords[] = {
        "graph",
        "name",
        "callback",
        "kind",
        "stop_after",
        "allow_fan_out",
        "create_join_scope",
        "join_incoming_branches",
        "merge_rules",
        nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "Os|OOppppO",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &callback,
            &kind_object,
            &stop_after,
            &allow_fan_out,
            &create_join_scope,
            &join_incoming_branches,
            &merge_rules_object
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    NodeKind kind = NodeKind::Compute;
    std::string error_message;
    if (!parse_node_kind(kind_object, &kind, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }

    std::vector<std::pair<std::string, JoinMergeStrategy>> merge_rules;
    if (!parse_merge_rules(merge_rules_object, &merge_rules, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }

    uint32_t policy_flags = 0U;
    if (stop_after != 0) {
        policy_flags |= node_policy_mask(NodePolicyFlag::StopAfterNode);
    }
    if (allow_fan_out != 0) {
        policy_flags |= node_policy_mask(NodePolicyFlag::AllowFanOut);
    }
    if (create_join_scope != 0) {
        policy_flags |= node_policy_mask(NodePolicyFlag::CreateJoinScope);
    }
    if (join_incoming_branches != 0) {
        policy_flags |= node_policy_mask(NodePolicyFlag::JoinIncomingBranches);
    }

    if (!handle->add_node(
            name,
            callback == Py_None ? nullptr : callback,
            kind,
            policy_flags,
            merge_rules,
            &error_message
        )) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* py_add_subgraph_node(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    PyObject* subgraph_capsule = nullptr;
    PyObject* namespace_object = Py_None;
    PyObject* input_bindings_object = Py_None;
    PyObject* output_bindings_object = Py_None;
    int propagate_knowledge_graph = 0;
    PyObject* session_mode_object = Py_None;
    PyObject* session_id_from_object = Py_None;
    static const char* keywords[] = {
        "graph",
        "name",
        "subgraph",
        "namespace_name",
        "input_bindings",
        "output_bindings",
        "propagate_knowledge_graph",
        "session_mode",
        "session_id_from",
        nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO|OOOpOO",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &subgraph_capsule,
            &namespace_object,
            &input_bindings_object,
            &output_bindings_object,
            &propagate_knowledge_graph,
            &session_mode_object,
            &session_id_from_object
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }
    GraphHandle* subgraph_handle = require_graph_handle(subgraph_capsule);
    if (subgraph_handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    std::string namespace_name;
    if (!parse_optional_python_string(namespace_object, &namespace_name, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }

    std::vector<std::pair<std::string, std::string>> input_bindings;
    if (!parse_string_mapping(input_bindings_object, &input_bindings, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }

    std::vector<std::pair<std::string, std::string>> output_bindings;
    if (!parse_string_mapping(output_bindings_object, &output_bindings, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }

    std::string session_mode = "ephemeral";
    if (!parse_optional_python_string(session_mode_object, &session_mode, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    if (session_mode.empty()) {
        session_mode = "ephemeral";
    }

    std::string session_id_from;
    if (!parse_optional_python_string(session_id_from_object, &session_id_from, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    const std::optional<std::string> session_id_source_name =
        session_id_from.empty() ? std::nullopt : std::optional<std::string>{session_id_from};

    if (!handle->add_subgraph_node(
            name,
            subgraph_handle,
            namespace_name,
            input_bindings,
            output_bindings,
            propagate_knowledge_graph != 0,
            session_mode,
            session_id_source_name,
            &error_message
        )) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* py_add_edge(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* from_name = nullptr;
    const char* to_name = nullptr;
    static const char* keywords[] = {"graph", "from_name", "to_name", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "Oss",
            const_cast<char**>(keywords),
            &capsule,
            &from_name,
            &to_name
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    if (!handle->add_edge(from_name, to_name, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* py_set_entry_point(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* node_name = nullptr;
    static const char* keywords[] = {"graph", "node_name", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "Os",
            const_cast<char**>(keywords),
            &capsule,
            &node_name
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    if (!handle->set_entry_point(node_name, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* py_finalize(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    static const char* keywords[] = {"graph", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O",
            const_cast<char**>(keywords),
            &capsule
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    if (!handle->finalize(&error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* py_invoke(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    PyObject* input_state = Py_None;
    PyObject* config = Py_None;
    static const char* keywords[] = {"graph", "input_state", "config", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|OO",
            const_cast<char**>(keywords),
            &capsule,
            &input_state,
            &config
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    PyObject* output_state = nullptr;
    std::string error_message;
    if (!handle->invoke(input_state, config, &output_state, &error_message)) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return output_state;
}

PyObject* py_invoke_with_details(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    PyObject* input_state = Py_None;
    PyObject* config = Py_None;
    int include_subgraphs = 1;
    static const char* keywords[] = {"graph", "input_state", "config", "include_subgraphs", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|OOp",
            const_cast<char**>(keywords),
            &capsule,
            &input_state,
            &config,
            &include_subgraphs
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    PyObject* output_details = nullptr;
    std::string error_message;
    if (!handle->invoke_with_details(
            input_state,
            config,
            include_subgraphs != 0,
            &output_details,
            &error_message
        )) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return output_details;
}

PyObject* py_invoke_until_pause_with_details(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    PyObject* input_state = Py_None;
    PyObject* config = Py_None;
    int include_subgraphs = 1;
    static const char* keywords[] = {"graph", "input_state", "config", "include_subgraphs", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|OOp",
            const_cast<char**>(keywords),
            &capsule,
            &input_state,
            &config,
            &include_subgraphs
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    PyObject* output_details = nullptr;
    std::string error_message;
    if (!handle->invoke_until_pause_with_details(
            input_state,
            config,
            include_subgraphs != 0,
            &output_details,
            &error_message
        )) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return output_details;
}

PyObject* py_resume_with_details(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    unsigned long long checkpoint_id = 0U;
    int include_subgraphs = 1;
    static const char* keywords[] = {"graph", "checkpoint_id", "include_subgraphs", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OK|p",
            const_cast<char**>(keywords),
            &capsule,
            &checkpoint_id,
            &include_subgraphs
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    PyObject* output_details = nullptr;
    std::string error_message;
    if (!handle->resume_with_details(
            static_cast<CheckpointId>(checkpoint_id),
            include_subgraphs != 0,
            &output_details,
            &error_message
        )) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return output_details;
}

PyObject* py_stream(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    PyObject* input_state = Py_None;
    PyObject* config = Py_None;
    int include_subgraphs = 1;
    static const char* keywords[] = {"graph", "input_state", "config", "include_subgraphs", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|OOp",
            const_cast<char**>(keywords),
            &capsule,
            &input_state,
            &config,
            &include_subgraphs
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    PyObject* output_events = nullptr;
    std::string error_message;
    if (!handle->stream(input_state, config, include_subgraphs != 0, &output_events, &error_message)) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return output_events;
}

PyObject* py_runtime_record_once(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* runtime = nullptr;
    const char* key = nullptr;
    PyObject* request = Py_None;
    PyObject* producer = nullptr;
    static const char* keywords[] = {"runtime", "key", "request", "producer", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsOO",
            const_cast<char**>(keywords),
            &runtime,
            &key,
            &request,
            &producer
        )) {
        return nullptr;
    }

    std::string error_message;
    PyObject* result = runtime_record_once(runtime, key, request, producer, &error_message);
    if (result == nullptr) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        }
        return nullptr;
    }
    return result;
}

PyObject* py_list_tools(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    static const char* keywords[] = {"graph", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O",
            const_cast<char**>(keywords),
            &capsule
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    PyObject* result = list_registered_tools(handle->tools(), &error_message);
    if (result == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return result;
}

PyObject* py_describe_tool(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    static const char* keywords[] = {"graph", "name", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "Os",
            const_cast<char**>(keywords),
            &capsule,
            &name
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    PyObject* result = describe_registered_tool(handle->tools(), name, &error_message);
    if (result == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return result;
}

PyObject* py_register_http_tool_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "http_tool";
    PyObject* policy_object = Py_None;
    int enable_mock_scheme = 1;
    int enable_file_scheme = 1;
    static const char* keywords[] = {
        "graph",
        "name",
        "policy",
        "enable_mock_scheme",
        "enable_file_scheme",
        nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sOpp",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object,
            &enable_mock_scheme,
            &enable_file_scheme
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    HttpToolAdapterOptions options;
    std::string error_message;
    if (!parse_tool_policy(policy_object, &options.policy, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    options.enable_mock_scheme = enable_mock_scheme != 0;
    options.enable_file_scheme = enable_file_scheme != 0;
    register_http_tool_adapter(handle->tools(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_sqlite_tool_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "sqlite_tool";
    PyObject* policy_object = Py_None;
    static const char* keywords[] = {"graph", "name", "policy", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sO",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    SqliteToolAdapterOptions options;
    std::string error_message;
    if (!parse_tool_policy(policy_object, &options.policy, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    register_sqlite_tool_adapter(handle->tools(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_http_json_tool_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "http_json_tool";
    PyObject* policy_object = Py_None;
    PyObject* transport_object = Py_None;
    const char* default_method = "POST";
    static const char* keywords[] = {"graph", "name", "policy", "transport", "default_method", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sOOs",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object,
            &transport_object,
            &default_method
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    JsonHttpToolAdapterOptions options;
    options.default_method = default_method;
    std::string error_message;
    if (!parse_tool_policy(policy_object, &options.policy, &error_message) ||
        !parse_http_transport_options(transport_object, &options.transport, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    register_http_json_tool_adapter(handle->tools(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_python_tool_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    PyObject* handler = nullptr;
    PyObject* policy_object = Py_None;
    PyObject* metadata_object = Py_None;
    static const char* keywords[] = {"graph", "name", "handler", "policy", "metadata", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO|OO",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &handler,
            &policy_object,
            &metadata_object
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    ToolPolicy policy;
    AdapterMetadata metadata = default_python_tool_metadata(name);
    std::string error_message;
    if (!parse_tool_policy(policy_object, &policy, &error_message) ||
        !parse_adapter_metadata(metadata_object, &metadata, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    if (!handle->register_python_tool(name, handler, policy, std::move(metadata), &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* py_invoke_tool_with_details(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    PyObject* input_bytes = Py_None;
    static const char* keywords[] = {"graph", "name", "input_bytes", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &input_bytes
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::vector<std::byte> encoded_input;
    std::string error_message;
    if (!parse_python_bytes_like(input_bytes, &encoded_input, &error_message)) {
        PyErr_SetString(PyExc_TypeError, error_message.c_str());
        return nullptr;
    }
    PyObject* result = invoke_tool_registry(handle->tools(), name, encoded_input, &error_message);
    if (result == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return result;
}

PyObject* py_runtime_invoke_tool_with_details(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* runtime = nullptr;
    const char* name = nullptr;
    PyObject* input_bytes = Py_None;
    static const char* keywords[] = {"runtime", "name", "input_bytes", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO",
            const_cast<char**>(keywords),
            &runtime,
            &name,
            &input_bytes
        )) {
        return nullptr;
    }

    std::vector<std::byte> encoded_input;
    std::string error_message;
    if (!parse_python_bytes_like(input_bytes, &encoded_input, &error_message)) {
        PyErr_SetString(PyExc_TypeError, error_message.c_str());
        return nullptr;
    }
    PyObject* result = runtime_invoke_tool(runtime, name, encoded_input, &error_message);
    if (result == nullptr) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        }
        return nullptr;
    }
    return result;
}

PyObject* py_list_models(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    static const char* keywords[] = {"graph", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O",
            const_cast<char**>(keywords),
            &capsule
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    PyObject* result = list_registered_models(handle->models(), &error_message);
    if (result == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return result;
}

PyObject* py_describe_model(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    static const char* keywords[] = {"graph", "name", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "Os",
            const_cast<char**>(keywords),
            &capsule,
            &name
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::string error_message;
    PyObject* result = describe_registered_model(handle->models(), name, &error_message);
    if (result == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return result;
}

PyObject* py_register_local_model_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "local_model";
    PyObject* policy_object = Py_None;
    unsigned long default_max_tokens = 256U;
    static const char* keywords[] = {"graph", "name", "policy", "default_max_tokens", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sOk",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object,
            &default_max_tokens
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    LocalModelAdapterOptions options;
    options.default_max_tokens = static_cast<uint32_t>(default_max_tokens);
    std::string error_message;
    if (!parse_model_policy(policy_object, &options.policy, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    register_local_model_adapter(handle->models(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_llm_http_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "llm_http";
    PyObject* policy_object = Py_None;
    int enable_mock_transport = 1;
    static const char* keywords[] = {"graph", "name", "policy", "enable_mock_transport", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sOp",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object,
            &enable_mock_transport
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    LlmHttpAdapterOptions options;
    options.enable_mock_transport = enable_mock_transport != 0;
    std::string error_message;
    if (!parse_model_policy(policy_object, &options.policy, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    register_llm_http_adapter(handle->models(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_openai_chat_model_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "openai_chat";
    PyObject* policy_object = Py_None;
    PyObject* transport_object = Py_None;
    const char* provider_model_name = "";
    const char* endpoint_path = "/chat/completions";
    const char* system_prompt = "";
    int include_json_schema = 1;
    static const char* keywords[] = {
        "graph",
        "name",
        "policy",
        "transport",
        "provider_model_name",
        "endpoint_path",
        "system_prompt",
        "include_json_schema",
        nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sOOsssp",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object,
            &transport_object,
            &provider_model_name,
            &endpoint_path,
            &system_prompt,
            &include_json_schema
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    OpenAiChatModelAdapterOptions options;
    options.provider_model_name = provider_model_name;
    options.endpoint_path = endpoint_path;
    options.system_prompt = system_prompt;
    options.include_json_schema = include_json_schema != 0;
    std::string error_message;
    if (!parse_model_policy(policy_object, &options.policy, &error_message) ||
        !parse_http_transport_options(transport_object, &options.transport, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    register_openai_chat_model_adapter(handle->models(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_grok_chat_model_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "grok_chat";
    PyObject* policy_object = Py_None;
    PyObject* transport_object = Py_None;
    const char* provider_model_name = "";
    const char* endpoint_path = "/chat/completions";
    const char* system_prompt = "";
    int include_json_schema = 1;
    static const char* keywords[] = {
        "graph",
        "name",
        "policy",
        "transport",
        "provider_model_name",
        "endpoint_path",
        "system_prompt",
        "include_json_schema",
        nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sOOsssp",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object,
            &transport_object,
            &provider_model_name,
            &endpoint_path,
            &system_prompt,
            &include_json_schema
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    GrokChatModelAdapterOptions options;
    options.provider_model_name = provider_model_name;
    options.endpoint_path = endpoint_path;
    options.system_prompt = system_prompt;
    options.include_json_schema = include_json_schema != 0;
    std::string error_message;
    if (!parse_model_policy(policy_object, &options.policy, &error_message) ||
        !parse_http_transport_options(transport_object, &options.transport, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    register_grok_chat_model_adapter(handle->models(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_gemini_generate_content_model_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = "gemini";
    PyObject* policy_object = Py_None;
    PyObject* transport_object = Py_None;
    const char* provider_model_name = "";
    const char* endpoint_path = "";
    const char* system_prompt = "";
    static const char* keywords[] = {
        "graph",
        "name",
        "policy",
        "transport",
        "provider_model_name",
        "endpoint_path",
        "system_prompt",
        nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "O|sOOsss",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &policy_object,
            &transport_object,
            &provider_model_name,
            &endpoint_path,
            &system_prompt
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    GeminiGenerateContentModelAdapterOptions options;
    options.provider_model_name = provider_model_name;
    options.endpoint_path = endpoint_path;
    options.system_prompt = system_prompt;
    std::string error_message;
    if (!parse_model_policy(policy_object, &options.policy, &error_message) ||
        !parse_http_transport_options(transport_object, &options.transport, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    register_gemini_generate_content_model_adapter(handle->models(), name, options);
    Py_RETURN_NONE;
}

PyObject* py_register_python_model_adapter(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    PyObject* handler = nullptr;
    PyObject* policy_object = Py_None;
    PyObject* metadata_object = Py_None;
    static const char* keywords[] = {"graph", "name", "handler", "policy", "metadata", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO|OO",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &handler,
            &policy_object,
            &metadata_object
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    ModelPolicy policy;
    AdapterMetadata metadata = default_python_model_metadata(name);
    std::string error_message;
    if (!parse_model_policy(policy_object, &policy, &error_message) ||
        !parse_adapter_metadata(metadata_object, &metadata, &error_message)) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    if (!handle->register_python_model(
            name,
            handler,
            policy,
            std::move(metadata),
            &error_message
        )) {
        PyErr_SetString(PyExc_ValueError, error_message.c_str());
        return nullptr;
    }
    Py_RETURN_NONE;
}

PyObject* py_invoke_model_with_details(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* capsule = nullptr;
    const char* name = nullptr;
    PyObject* prompt_bytes = Py_None;
    PyObject* schema_bytes = Py_None;
    unsigned long max_tokens = 0U;
    static const char* keywords[] = {"graph", "name", "prompt_bytes", "schema_bytes", "max_tokens", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO|Ok",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &prompt_bytes,
            &schema_bytes,
            &max_tokens
        )) {
        return nullptr;
    }

    GraphHandle* handle = require_graph_handle(capsule);
    if (handle == nullptr) {
        return nullptr;
    }

    std::vector<std::byte> encoded_prompt;
    std::vector<std::byte> encoded_schema;
    std::string error_message;
    if (!parse_python_bytes_like(prompt_bytes, &encoded_prompt, &error_message) ||
        !parse_python_bytes_like(schema_bytes, &encoded_schema, &error_message)) {
        PyErr_SetString(PyExc_TypeError, error_message.c_str());
        return nullptr;
    }
    PyObject* result = invoke_model_registry(
        handle->models(),
        name,
        encoded_prompt,
        encoded_schema,
        static_cast<uint32_t>(max_tokens),
        &error_message
    );
    if (result == nullptr) {
        PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        return nullptr;
    }
    return result;
}

PyObject* py_runtime_invoke_model_with_details(PyObject*, PyObject* args, PyObject* kwargs) {
    PyObject* runtime = nullptr;
    const char* name = nullptr;
    PyObject* prompt_bytes = Py_None;
    PyObject* schema_bytes = Py_None;
    unsigned long max_tokens = 0U;
    static const char* keywords[] = {"runtime", "name", "prompt_bytes", "schema_bytes", "max_tokens", nullptr};
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO|Ok",
            const_cast<char**>(keywords),
            &runtime,
            &name,
            &prompt_bytes,
            &schema_bytes,
            &max_tokens
        )) {
        return nullptr;
    }

    std::vector<std::byte> encoded_prompt;
    std::vector<std::byte> encoded_schema;
    std::string error_message;
    if (!parse_python_bytes_like(prompt_bytes, &encoded_prompt, &error_message) ||
        !parse_python_bytes_like(schema_bytes, &encoded_schema, &error_message)) {
        PyErr_SetString(PyExc_TypeError, error_message.c_str());
        return nullptr;
    }
    PyObject* result = runtime_invoke_model(
        runtime,
        name,
        encoded_prompt,
        encoded_schema,
        static_cast<uint32_t>(max_tokens),
        &error_message
    );
    if (result == nullptr) {
        if (PyErr_Occurred() == nullptr) {
            PyErr_SetString(PyExc_RuntimeError, error_message.c_str());
        }
        return nullptr;
    }
    return result;
}

PyMethodDef kModuleMethods[] = {
    {
        "_create_graph",
        reinterpret_cast<PyCFunction>(py_create_graph),
        METH_VARARGS | METH_KEYWORDS,
        "Create a native graph builder capsule."
    },
    {
        "_add_node",
        reinterpret_cast<PyCFunction>(py_add_node),
        METH_VARARGS | METH_KEYWORDS,
        "Register a node callback with the native graph builder."
    },
    {
        "_add_subgraph_node",
        reinterpret_cast<PyCFunction>(py_add_subgraph_node),
        METH_VARARGS | METH_KEYWORDS,
        "Register a native subgraph node with the graph builder."
    },
    {
        "_add_edge",
        reinterpret_cast<PyCFunction>(py_add_edge),
        METH_VARARGS | METH_KEYWORDS,
        "Register a directed edge with the native graph builder."
    },
    {
        "_set_entry_point",
        reinterpret_cast<PyCFunction>(py_set_entry_point),
        METH_VARARGS | METH_KEYWORDS,
        "Set the graph entry point."
    },
    {
        "_finalize",
        reinterpret_cast<PyCFunction>(py_finalize),
        METH_VARARGS | METH_KEYWORDS,
        "Finalize and validate the native graph."
    },
    {
        "_invoke",
        reinterpret_cast<PyCFunction>(py_invoke),
        METH_VARARGS | METH_KEYWORDS,
        "Run the native graph to completion and return the final state."
    },
    {
        "_invoke_with_details",
        reinterpret_cast<PyCFunction>(py_invoke_with_details),
        METH_VARARGS | METH_KEYWORDS,
        "Run the native graph and return validation metadata."
    },
    {
        "_invoke_until_pause_with_details",
        reinterpret_cast<PyCFunction>(py_invoke_until_pause_with_details),
        METH_VARARGS | METH_KEYWORDS,
        "Run the native graph until completion or pause and return validation metadata."
    },
    {
        "_resume_with_details",
        reinterpret_cast<PyCFunction>(py_resume_with_details),
        METH_VARARGS | METH_KEYWORDS,
        "Resume a paused native graph checkpoint and return validation metadata."
    },
    {
        "_stream",
        reinterpret_cast<PyCFunction>(py_stream),
        METH_VARARGS | METH_KEYWORDS,
        "Run the native graph and return public stream events."
    },
    {
        "_runtime_record_once",
        reinterpret_cast<PyCFunction>(py_runtime_record_once),
        METH_VARARGS | METH_KEYWORDS,
        "Record a once-only synchronous effect for the active Python node callback."
    },
    {
        "_list_tools",
        reinterpret_cast<PyCFunction>(py_list_tools),
        METH_VARARGS | METH_KEYWORDS,
        "List registered native tools for a compiled graph."
    },
    {
        "_describe_tool",
        reinterpret_cast<PyCFunction>(py_describe_tool),
        METH_VARARGS | METH_KEYWORDS,
        "Describe one registered native tool."
    },
    {
        "_register_http_tool_adapter",
        reinterpret_cast<PyCFunction>(py_register_http_tool_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in HTTP tool adapter on a compiled graph."
    },
    {
        "_register_sqlite_tool_adapter",
        reinterpret_cast<PyCFunction>(py_register_sqlite_tool_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in SQLite-like tool adapter on a compiled graph."
    },
    {
        "_register_http_json_tool_adapter",
        reinterpret_cast<PyCFunction>(py_register_http_json_tool_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in HTTP JSON tool adapter on a compiled graph."
    },
    {
        "_register_python_tool_adapter",
        reinterpret_cast<PyCFunction>(py_register_python_tool_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register a Python-backed tool handler on a compiled graph."
    },
    {
        "_invoke_tool_with_details",
        reinterpret_cast<PyCFunction>(py_invoke_tool_with_details),
        METH_VARARGS | METH_KEYWORDS,
        "Invoke a registered native tool from Python."
    },
    {
        "_runtime_invoke_tool_with_details",
        reinterpret_cast<PyCFunction>(py_runtime_invoke_tool_with_details),
        METH_VARARGS | METH_KEYWORDS,
        "Invoke a registered native tool from an active Python runtime context."
    },
    {
        "_list_models",
        reinterpret_cast<PyCFunction>(py_list_models),
        METH_VARARGS | METH_KEYWORDS,
        "List registered native models for a compiled graph."
    },
    {
        "_describe_model",
        reinterpret_cast<PyCFunction>(py_describe_model),
        METH_VARARGS | METH_KEYWORDS,
        "Describe one registered native model."
    },
    {
        "_register_local_model_adapter",
        reinterpret_cast<PyCFunction>(py_register_local_model_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in local model adapter on a compiled graph."
    },
    {
        "_register_llm_http_adapter",
        reinterpret_cast<PyCFunction>(py_register_llm_http_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in HTTP LLM adapter on a compiled graph."
    },
    {
        "_register_openai_chat_model_adapter",
        reinterpret_cast<PyCFunction>(py_register_openai_chat_model_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in OpenAI-compatible chat model adapter on a compiled graph."
    },
    {
        "_register_grok_chat_model_adapter",
        reinterpret_cast<PyCFunction>(py_register_grok_chat_model_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in xAI Grok chat model adapter on a compiled graph."
    },
    {
        "_register_gemini_generate_content_model_adapter",
        reinterpret_cast<PyCFunction>(py_register_gemini_generate_content_model_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register the built-in Gemini generateContent model adapter on a compiled graph."
    },
    {
        "_register_python_model_adapter",
        reinterpret_cast<PyCFunction>(py_register_python_model_adapter),
        METH_VARARGS | METH_KEYWORDS,
        "Register a Python-backed model handler on a compiled graph."
    },
    {
        "_invoke_model_with_details",
        reinterpret_cast<PyCFunction>(py_invoke_model_with_details),
        METH_VARARGS | METH_KEYWORDS,
        "Invoke a registered native model from Python."
    },
    {
        "_runtime_invoke_model_with_details",
        reinterpret_cast<PyCFunction>(py_runtime_invoke_model_with_details),
        METH_VARARGS | METH_KEYWORDS,
        "Invoke a registered native model from an active Python runtime context."
    },
    {nullptr, nullptr, 0, nullptr}
};

PyModuleDef kModuleDefinition = {
    PyModuleDef_HEAD_INIT,
    "_agentcore_native",
    "CPython bridge for the agentcore state graph runtime.",
    -1,
    kModuleMethods
};

} // namespace

} // namespace agentcore::python_binding

PyMODINIT_FUNC PyInit__agentcore_native(void) {
    using namespace agentcore::python_binding;

    PyObject* module = PyModule_Create(&kModuleDefinition);
    if (module == nullptr) {
        return nullptr;
    }

    if (PyModule_AddStringConstant(module, "_INTERNAL_END_NODE_NAME", kInternalEndNodeName) != 0) {
        Py_DECREF(module);
        return nullptr;
    }
    if (PyModule_AddStringConstant(module, "_INTERNAL_RUNTIME_CONFIG_KEY", kPythonRuntimeConfigKey) != 0) {
        Py_DECREF(module);
        return nullptr;
    }

    return module;
}
