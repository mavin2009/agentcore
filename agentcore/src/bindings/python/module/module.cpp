#include "bridge.h"

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
    static const char* keywords[] = {
        "graph",
        "name",
        "subgraph",
        "namespace_name",
        "input_bindings",
        "output_bindings",
        "propagate_knowledge_graph",
        nullptr
    };
    if (!PyArg_ParseTupleAndKeywords(
            args,
            kwargs,
            "OsO|OOOp",
            const_cast<char**>(keywords),
            &capsule,
            &name,
            &subgraph_capsule,
            &namespace_object,
            &input_bindings_object,
            &output_bindings_object,
            &propagate_knowledge_graph
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

    if (!handle->add_subgraph_node(
            name,
            subgraph_handle,
            namespace_name,
            input_bindings,
            output_bindings,
            propagate_knowledge_graph != 0,
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
        "_stream",
        reinterpret_cast<PyCFunction>(py_stream),
        METH_VARARGS | METH_KEYWORDS,
        "Run the native graph and return public stream events."
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

    return module;
}
