#include "bridge.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/state/state_store.h"
#include "agentcore/execution/proof.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <utility>

namespace agentcore::python_binding {

namespace {

constexpr std::byte kPickleBlobTag{0x01};
constexpr std::byte kJsonBlobTag{0x02};
constexpr std::byte kBytesBlobTag{0x03};

[[nodiscard]] PyObject* unicode_from_utf8(std::string_view value) {
    return PyUnicode_FromStringAndSize(value.data(), static_cast<Py_ssize_t>(value.size()));
}

[[nodiscard]] std::string python_string(PyObject* obj, std::string* err) {
    Py_ssize_t size = 0;
    const char* utf8 = PyUnicode_AsUTF8AndSize(obj, &size);
    if (!utf8) {
        if (err) {
            PyObject *ptype, *pvalue, *ptraceback;
            PyErr_Fetch(&ptype, &pvalue, &ptraceback);
            if (pvalue) {
                PyObject* s = PyObject_Str(pvalue);
                *err = PyUnicode_AsUTF8(s);
                Py_DECREF(s);
            } else {
                *err = "unknown python error";
            }
            PyErr_Restore(ptype, pvalue, ptraceback);
        }
        return {};
    }
    return std::string(utf8, static_cast<std::size_t>(size));
}

std::string fetch_python_error() {
    PyObject *t, *v, *tb;
    PyErr_Fetch(&t, &v, &tb);
    if (!v) return "unknown python error";
    PyObject* s = PyObject_Str(v);
    const char* u = PyUnicode_AsUTF8(s);
    std::string msg = u ? u : "error converting exception to string";
    Py_XDECREF(s); Py_XDECREF(t); Py_XDECREF(v); Py_XDECREF(tb);
    return msg;
}

[[nodiscard]] bool tagged_blob_has_payload(const std::vector<std::byte>& bytes, std::byte tag) {
    return !bytes.empty() && bytes.front() == tag;
}

[[nodiscard]] bool tagged_buffer_has_payload(const std::byte* bytes, std::size_t size, std::byte tag) {
    return bytes != nullptr && size != 0U && bytes[0] == tag;
}

bool lookup_state_key_in_python_dict(
    PyObject* lookup,
    PyObject* key,
    StateKey* output,
    bool* found,
    std::string* error_message
) {
    *found = false;
    if (lookup == nullptr) {
        return true;
    }

    PyObject* lookup_value = PyDict_GetItemWithError(lookup, key);
    if (lookup_value == nullptr) {
        if (PyErr_Occurred() != nullptr) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return false;
        }
        return true;
    }

    const unsigned long raw_key = PyLong_AsUnsignedLong(lookup_value);
    if (PyErr_Occurred() != nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return false;
    }

    *output = static_cast<StateKey>(raw_key);
    *found = true;
    return true;
}

bool cache_state_key_in_python_dict(
    PyObject* lookup,
    PyObject* key,
    StateKey state_key,
    std::string* error_message
) {
    if (lookup == nullptr) {
        return true;
    }

    PyObject* py_state_key = PyLong_FromUnsignedLong(static_cast<unsigned long>(state_key));
    if (py_state_key == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return false;
    }

    const int rc = PyDict_SetItem(lookup, key, py_state_key);
    Py_DECREF(py_state_key);
    if (rc != 0) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return false;
    }
    return true;
}

[[nodiscard]] BlobRef append_tagged_blob(BlobStore& blobs, std::byte tag, const std::byte* bytes, std::size_t size) {
    std::vector<std::byte> tagged(size + 1U, std::byte{0});
    tagged[0] = tag;
    if (size != 0U) {
        std::memcpy(tagged.data() + 1, bytes, size);
    }
    return blobs.append(tagged.data(), tagged.size());
}

PyObject* get_pickle_module() {
    static PyObject* p = PyImport_ImportModule("pickle");
    return p;
}

PyObject* get_json_module() {
    static PyObject* j = PyImport_ImportModule("json");
    return j;
}

PyObject* load_object_from_pickle_buffer(const std::byte* bytes, std::size_t size, std::string* err);
PyObject* load_object_from_json_buffer(const std::byte* bytes, std::size_t size, std::string* err);

bool dump_object_as_pickle_bytes(PyObject* obj, std::vector<std::byte>* out, std::string* err) {
    PyObject* p = get_pickle_module();
    if (!p) { *err = fetch_python_error(); return false; }
    static PyObject* dumps = PyObject_GetAttrString(p, "dumps");
    PyObject* res = PyObject_CallFunctionObjArgs(dumps, obj, nullptr);
    if (!res) { *err = fetch_python_error(); return false; }
    char* b; Py_ssize_t s;
    if (PyBytes_AsStringAndSize(res, &b, &s) != 0) {
        *err = fetch_python_error();
        Py_DECREF(res);
        return false;
    }
    out->assign(reinterpret_cast<const std::byte*>(b), reinterpret_cast<const std::byte*>(b) + s);
    Py_DECREF(res);
    return true;
}

PyObject* load_object_from_pickle_bytes(const std::vector<std::byte>& bytes, std::string* err) {
    if (bytes.empty()) {
        if (err != nullptr) {
            *err = "pickle payload is empty";
        }
        return nullptr;
    }
    return load_object_from_pickle_buffer(bytes.data(), bytes.size(), err);
}

PyObject* load_object_from_pickle_buffer(const std::byte* bytes, std::size_t size, std::string* err) {
    PyObject* p = get_pickle_module();
    if (!p) { *err = fetch_python_error(); return nullptr; }
    static PyObject* loads = PyObject_GetAttrString(p, "loads");
    if (bytes == nullptr || size <= 1U) {
        if (err != nullptr) {
            *err = "pickle payload is empty";
        }
        return nullptr;
    }

    PyObject* b = PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(bytes + 1),
        static_cast<Py_ssize_t>(size - 1U)
    );
    if (b == nullptr) {
        if (err != nullptr) {
            *err = fetch_python_error();
        }
        return nullptr;
    }

    PyObject* res = PyObject_CallFunctionObjArgs(loads, b, nullptr);
    Py_DECREF(b);
    if (!res && err != nullptr) *err = fetch_python_error();
    return res;
}

PyObject* load_object_from_json_bytes(const std::vector<std::byte>& bytes, std::string* err) {
    if (bytes.empty()) {
        if (err != nullptr) {
            *err = "json payload is empty";
        }
        return nullptr;
    }
    return load_object_from_json_buffer(bytes.data(), bytes.size(), err);
}

PyObject* load_object_from_json_buffer(const std::byte* bytes, std::size_t size, std::string* err) {
    PyObject* j = get_json_module();
    if (!j) { *err = fetch_python_error(); return nullptr; }
    static PyObject* loads = PyObject_GetAttrString(j, "loads");
    if (bytes == nullptr || size <= 1U) {
        if (err != nullptr) {
            *err = "json payload is empty";
        }
        return nullptr;
    }

    PyObject* b = PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(bytes + 1),
        static_cast<Py_ssize_t>(size - 1U)
    );
    if (!b) {
        if (err != nullptr) {
            *err = fetch_python_error();
        }
        return nullptr;
    }

    PyObject* res = PyObject_CallFunctionObjArgs(loads, b, nullptr);
    Py_DECREF(b);
    if (!res && err != nullptr) *err = fetch_python_error();
    return res;
}

struct RuntimeViewProxy {
    PyObject_HEAD
    ExecutionContext* runtime{nullptr};
    const WorkflowState* state{nullptr};
    const BlobStore* blobs{nullptr};
    const StringInterner* strings{nullptr};
    std::atomic<bool> active;
};

struct OwnedStateSnapshot {
    WorkflowState state;
    BlobStore blobs;
    StringInterner strings;

    OwnedStateSnapshot(
        const WorkflowState& source_state,
        const BlobStore& source_blobs,
        const StringInterner& source_strings
    )
        : state(source_state),
          blobs(source_blobs),
          strings(source_strings) {}
};

void RuntimeViewProxy_dealloc(RuntimeViewProxy* self) {
    self->active.~atomic();
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

static PyTypeObject RuntimeViewProxyType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "agentcore.RuntimeViewProxy",         /* tp_name */
    sizeof(RuntimeViewProxy),             /* tp_basicsize */
    0,                                    /* tp_itemsize */
    (destructor)RuntimeViewProxy_dealloc, /* tp_dealloc */
    0,                                    /* tp_print */
    0,                                    /* tp_getattr */
    0,                                    /* tp_setattr */
    0,                                    /* tp_reserved */
    0,                                    /* tp_repr */
    0,                                    /* tp_as_number */
    0,                                    /* tp_as_sequence */
    0,                                    /* tp_as_mapping */
    0,                                    /* tp_hash */
    0,                                    /* tp_call */
    0,                                    /* tp_str */
    0,                                    /* tp_getattro */
    0,                                    /* tp_setattro */
    0,                                    /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                   /* tp_flags */
    "Runtime View Proxy",                 /* tp_doc */
    0,                                    /* tp_traverse */
    0,                                    /* tp_clear */
    0,                                    /* tp_richcompare */
    0,                                    /* tp_weaklistoffset */
    0,                                    /* tp_iter */
    0,                                    /* tp_iternext */
    0,                                    /* tp_methods */
    0,                                    /* tp_members */
    0,                                    /* tp_getset */
    0,                                    /* tp_base */
    0,                                    /* tp_dict */
    0,                                    /* tp_descr_get */
    0,                                    /* tp_descr_set */
    0,                                    /* tp_dictoffset */
    0,                                    /* tp_init */
    0,                                    /* tp_alloc */
    0,                                    /* tp_new */
    0,                                    /* tp_free */
};

PyObject* create_runtime_view_proxy(ExecutionContext& context) {
    if (PyType_Ready(&RuntimeViewProxyType) < 0) {
        return nullptr;
    }
    RuntimeViewProxy* self = PyObject_New(RuntimeViewProxy, &RuntimeViewProxyType);
    if (self != nullptr) {
        self->runtime = &context;
        self->state = &context.state;
        self->blobs = &context.blobs;
        self->strings = &context.strings;
        new (&self->active) std::atomic<bool>(true);
    }
    return reinterpret_cast<PyObject*>(self);
}

RuntimeViewProxy* runtime_view_from_object(PyObject* object, std::string* error_message) {
    if (object == nullptr || !PyObject_TypeCheck(object, &RuntimeViewProxyType)) {
        if (error_message != nullptr) {
            *error_message = "invalid runtime capsule";
        }
        return nullptr;
    }
    auto* view = reinterpret_cast<RuntimeViewProxy*>(object);
    if (!view->active.load(std::memory_order_acquire)) {
        if (error_message != nullptr) {
            *error_message = "runtime context is no longer active";
        }
        return nullptr;
    }
    return view;
}

ExecutionContext* runtime_context_from_capsule(PyObject* capsule, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    return view == nullptr ? nullptr : view->runtime;
}

struct StateProxy {
    PyObject_HEAD
    const WorkflowState* state;
    const BlobStore* blobs;
    const StringInterner* strings;
    std::shared_ptr<OwnedStateSnapshot> owned_snapshot;
    std::shared_ptr<std::vector<std::string>> state_names;
    std::shared_ptr<std::unordered_map<std::string, StateKey>> state_keys;
    PyObject* state_key_lookup;
    PyObject* value_cache;
    RuntimeViewProxy* borrowed_view;
    PyObject* runtime_capsule_owner;
    const GraphHandle* handle;
};

void StateProxy_dealloc(StateProxy* self) {
    self->owned_snapshot.~shared_ptr();
    self->state_names.~shared_ptr();
    self->state_keys.~shared_ptr();
    Py_XDECREF(self->state_key_lookup);
    Py_XDECREF(self->value_cache);
    Py_XDECREF(self->runtime_capsule_owner);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

bool StateProxy_resolve(
    StateProxy* proxy,
    const WorkflowState** state,
    const BlobStore** blobs,
    const StringInterner** strings
) {
    if (proxy->borrowed_view != nullptr &&
        !proxy->borrowed_view->active.load(std::memory_order_acquire)) {
        PyErr_SetString(
            PyExc_RuntimeError,
            "state view is no longer active; call state.copy() during callback to retain a snapshot"
        );
        return false;
    }

    *state = proxy->state;
    *blobs = proxy->blobs;
    *strings = proxy->strings;
    return true;
}

PyObject* StateProxy_cached_value(
    StateProxy* proxy,
    StateKey state_key,
    const Value& value,
    const BlobStore& blobs,
    const StringInterner& strings,
    bool ensure_cache
) {
    if (proxy->value_cache == nullptr && ensure_cache) {
        proxy->value_cache = PyList_New(static_cast<Py_ssize_t>(proxy->state_names->size()));
        if (proxy->value_cache == nullptr) {
            return nullptr;
        }
        for (Py_ssize_t index = 0; index < PyList_GET_SIZE(proxy->value_cache); ++index) {
            Py_INCREF(Py_None);
            PyList_SET_ITEM(proxy->value_cache, index, Py_None);
        }
    }

    if (proxy->value_cache != nullptr &&
        static_cast<Py_ssize_t>(state_key) < PyList_GET_SIZE(proxy->value_cache)) {
        PyObject* cached = PyList_GET_ITEM(proxy->value_cache, static_cast<Py_ssize_t>(state_key));
        if (cached != Py_None) {
            Py_INCREF(cached);
            return cached;
        }
    }

    std::string err;
    PyObject* python_value = proxy->handle->convert_value_to_python(value, blobs, strings, &err);
    if (python_value == nullptr) {
        return nullptr;
    }

    if (proxy->value_cache != nullptr &&
        static_cast<Py_ssize_t>(state_key) < PyList_GET_SIZE(proxy->value_cache)) {
        Py_INCREF(python_value);
        if (PyList_SetItem(proxy->value_cache, static_cast<Py_ssize_t>(state_key), python_value) != 0) {
            Py_DECREF(python_value);
            return nullptr;
        }
    }

    return python_value;
}

bool StateProxy_resolve_key(StateProxy* proxy, PyObject* key, StateKey* state_key, bool* found) {
    *found = false;
    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "keys must be strings");
        return false;
    }

    if (proxy->state_key_lookup != nullptr) {
        PyObject* lookup_value = PyDict_GetItemWithError(proxy->state_key_lookup, key);
        if (lookup_value != nullptr) {
            const unsigned long raw_key = PyLong_AsUnsignedLong(lookup_value);
            if (PyErr_Occurred() != nullptr) {
                return false;
            }
            *state_key = static_cast<StateKey>(raw_key);
            *found = true;
            return true;
        }
        if (PyErr_Occurred() != nullptr) {
            return false;
        }
    }

    std::string err;
    const std::string key_name = python_string(key, &err);
    if (!err.empty()) {
        PyErr_SetString(PyExc_KeyError, err.c_str());
        return false;
    }
    const auto it = proxy->state_keys->find(key_name);
    if (it == proxy->state_keys->end()) {
        return true;
    }

    *state_key = it->second;
    *found = true;
    return true;
}

Py_ssize_t StateProxy_length(PyObject* self) {
    auto* proxy = reinterpret_cast<StateProxy*>(self);
    const WorkflowState* state = nullptr;
    const BlobStore* blobs = nullptr;
    const StringInterner* strings = nullptr;
    if (!StateProxy_resolve(proxy, &state, &blobs, &strings)) {
        return -1;
    }
    return static_cast<Py_ssize_t>(state->size());
}

PyObject* StateProxy_subscript(PyObject* self, PyObject* key) {
    auto* proxy = reinterpret_cast<StateProxy*>(self);
    StateKey state_key = 0;
    bool found = false;
    if (!StateProxy_resolve_key(proxy, key, &state_key, &found)) {
        return nullptr;
    }
    if (!found) {
        std::string err;
        const std::string key_name = python_string(key, &err);
        if (!err.empty()) {
            PyErr_SetString(PyExc_KeyError, err.c_str());
            return nullptr;
        }
        PyErr_Format(PyExc_KeyError, "'%s'", key_name.c_str());
        return nullptr;
    }

    const WorkflowState* state = nullptr;
    const BlobStore* blobs = nullptr;
    const StringInterner* strings = nullptr;
    if (!StateProxy_resolve(proxy, &state, &blobs, &strings)) {
        return nullptr;
    }

    Value v = state->load(state_key);
    if (std::holds_alternative<std::monostate>(v)) {
        Py_RETURN_NONE;
    }
    return StateProxy_cached_value(proxy, state_key, v, *blobs, *strings, false);
}

static PyMappingMethods StateProxy_as_mapping = {
    StateProxy_length,
    StateProxy_subscript,
    nullptr
};

PyObject* StateProxy_get(PyObject* self, PyObject* args) {
    PyObject *k, *d = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &k, &d)) return nullptr;

    auto* proxy = reinterpret_cast<StateProxy*>(self);
    StateKey state_key = 0;
    bool found = false;
    if (!StateProxy_resolve_key(proxy, k, &state_key, &found)) {
        return nullptr;
    }
    if (!found) {
        Py_INCREF(d);
        return d;
    }

    const WorkflowState* state = nullptr;
    const BlobStore* blobs = nullptr;
    const StringInterner* strings = nullptr;
    if (!StateProxy_resolve(proxy, &state, &blobs, &strings)) {
        return nullptr;
    }

    Value value = state->load(state_key);
    if (std::holds_alternative<std::monostate>(value)) {
        Py_RETURN_NONE;
    }
    return StateProxy_cached_value(proxy, state_key, value, *blobs, *strings, false);
}

PyObject* StateProxy_keys(PyObject* self, PyObject*) {
    auto* proxy = reinterpret_cast<StateProxy*>(self);
    const WorkflowState* state = nullptr;
    const BlobStore* blobs = nullptr;
    const StringInterner* strings = nullptr;
    if (!StateProxy_resolve(proxy, &state, &blobs, &strings)) {
        return nullptr;
    }
    PyObject* list = PyList_New(0);
    const auto& names = *proxy->state_names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i < state->size()) {
            Value v = state->load(static_cast<StateKey>(i));
            if (!std::holds_alternative<std::monostate>(v)) {
                PyObject* k = unicode_from_utf8(names[i]);
                PyList_Append(list, k);
                Py_DECREF(k);
            }
        }
    }
    return list;
}

PyObject* StateProxy_items(PyObject* self, PyObject*) {
    auto* proxy = reinterpret_cast<StateProxy*>(self);
    const WorkflowState* state = nullptr;
    const BlobStore* blobs = nullptr;
    const StringInterner* strings = nullptr;
    if (!StateProxy_resolve(proxy, &state, &blobs, &strings)) {
        return nullptr;
    }
    PyObject* list = PyList_New(0);
    const auto& names = *proxy->state_names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i < state->size()) {
            Value v = state->load(static_cast<StateKey>(i));
            if (!std::holds_alternative<std::monostate>(v)) {
                PyObject* k = unicode_from_utf8(names[i]);
                PyObject* val = StateProxy_cached_value(
                    proxy,
                    static_cast<StateKey>(i),
                    v,
                    *blobs,
                    *strings,
                    true
                );
                if (val) {
                    PyObject* item = PyTuple_Pack(2, k, val);
                    PyList_Append(list, item);
                    Py_DECREF(item);
                    Py_DECREF(val);
                }
                Py_DECREF(k);
            }
        }
    }
    return list;
}

PyObject* StateProxy_to_dict(StateProxy* proxy);

PyObject* StateProxy_copy(PyObject* self, PyObject*) {
    return StateProxy_to_dict(reinterpret_cast<StateProxy*>(self));
}

PyObject* StateProxy_to_dict(StateProxy* proxy) {
    const WorkflowState* state = nullptr;
    const BlobStore* blobs = nullptr;
    const StringInterner* strings = nullptr;
    if (!StateProxy_resolve(proxy, &state, &blobs, &strings)) {
        return nullptr;
    }

    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        return nullptr;
    }

    const auto& names = *proxy->state_names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i >= state->size()) {
            continue;
        }
        Value value = state->load(static_cast<StateKey>(i));
        if (std::holds_alternative<std::monostate>(value)) {
            continue;
        }

        PyObject* key = unicode_from_utf8(names[i]);
        PyObject* python_value = StateProxy_cached_value(
            proxy,
            static_cast<StateKey>(i),
            value,
            *blobs,
            *strings,
            true
        );
        if (key == nullptr || python_value == nullptr) {
            Py_XDECREF(key);
            Py_XDECREF(python_value);
            Py_DECREF(dict);
            return nullptr;
        }
        if (PyDict_SetItem(dict, key, python_value) != 0) {
            Py_DECREF(key);
            Py_DECREF(python_value);
            Py_DECREF(dict);
            return nullptr;
        }
        Py_DECREF(key);
        Py_DECREF(python_value);
    }

    return dict;
}

PyObject* StateProxy_richcompare(PyObject* self, PyObject* other, int op) {
    if (op != Py_EQ && op != Py_NE) {
        Py_RETURN_NOTIMPLEMENTED;
    }

    PyObject* left_dict = StateProxy_to_dict(reinterpret_cast<StateProxy*>(self));
    if (left_dict == nullptr) {
        return nullptr;
    }

    PyObject* right_object = other;
    PyObject* owned_right_dict = nullptr;
    if (PyObject_TypeCheck(other, Py_TYPE(self))) {
        owned_right_dict = StateProxy_to_dict(reinterpret_cast<StateProxy*>(other));
        if (owned_right_dict == nullptr) {
            Py_DECREF(left_dict);
            return nullptr;
        }
        right_object = owned_right_dict;
    }

    const int equals = PyObject_RichCompareBool(left_dict, right_object, Py_EQ);
    Py_DECREF(left_dict);
    Py_XDECREF(owned_right_dict);
    if (equals < 0) {
        return nullptr;
    }
    if ((op == Py_EQ && equals != 0) || (op == Py_NE && equals == 0)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

static PyMethodDef StateProxy_methods[] = {
    {"get", reinterpret_cast<PyCFunction>(StateProxy_get), METH_VARARGS, "Get value"},
    {"keys", reinterpret_cast<PyCFunction>(StateProxy_keys), METH_NOARGS, "Get keys"},
    {"items", reinterpret_cast<PyCFunction>(StateProxy_items), METH_NOARGS, "Get items"},
    {"copy", reinterpret_cast<PyCFunction>(StateProxy_copy), METH_NOARGS, "Copy to a dict"},
    {nullptr, nullptr, 0, nullptr}
};

static PyTypeObject StateProxyType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "agentcore.StateProxy",           /* tp_name */
    sizeof(StateProxy),               /* tp_basicsize */
    0,                                /* tp_itemsize */
    (destructor)StateProxy_dealloc,   /* tp_dealloc */
    0,                                /* tp_print */
    0,                                /* tp_getattr */
    0,                                /* tp_setattr */
    0,                                /* tp_reserved */
    0,                                /* tp_repr */
    0,                                /* tp_as_number */
    0,                                /* tp_as_sequence */
    &StateProxy_as_mapping,           /* tp_as_mapping */
    0,                                /* tp_hash */
    0,                                /* tp_call */
    0,                                /* tp_str */
    0,                                /* tp_getattro */
    0,                                /* tp_setattro */
    0,                                /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,               /* tp_flags */
    "State Proxy",                    /* tp_doc */
    0,                                /* tp_traverse */
    0,                                /* tp_clear */
    StateProxy_richcompare,           /* tp_richcompare */
    0,                                /* tp_weaklistoffset */
    0,                                /* tp_iter */
    0,                                /* tp_iternext */
    StateProxy_methods,               /* tp_methods */
    0,                                /* tp_members */
    0,                                /* tp_getset */
    0,                                /* tp_base */
    0,                                /* tp_dict */
    0,                                /* tp_descr_get */
    0,                                /* tp_descr_set */
    0,                                /* tp_dictoffset */
    0,                                /* tp_init */
    0,                                /* tp_alloc */
    0,                                /* tp_new */
    0,                                /* tp_free */
};

PyObject* create_state_proxy(
    const GraphHandle* handle,
    const WorkflowState& state,
    const BlobStore& blobs,
    const StringInterner& strings,
    const std::shared_ptr<std::vector<std::string>>& state_names,
    const std::shared_ptr<std::unordered_map<std::string, StateKey>>& state_keys,
    PyObject* state_key_lookup
) {
    if (PyType_Ready(&StateProxyType) < 0) return nullptr;
    StateProxy* self = PyObject_New(StateProxy, &StateProxyType);
    if (self) {
        new (&self->owned_snapshot) std::shared_ptr<OwnedStateSnapshot>(
            std::make_shared<OwnedStateSnapshot>(state, blobs, strings)
        );
        new (&self->state_names) std::shared_ptr<std::vector<std::string>>(state_names);
        new (&self->state_keys) std::shared_ptr<std::unordered_map<std::string, StateKey>>(state_keys);
        self->state_key_lookup = state_key_lookup;
        Py_XINCREF(self->state_key_lookup);
        self->value_cache = nullptr;
        self->state = &self->owned_snapshot->state;
        self->blobs = &self->owned_snapshot->blobs;
        self->strings = &self->owned_snapshot->strings;
        self->borrowed_view = nullptr;
        self->runtime_capsule_owner = nullptr;
        self->handle = handle;
    }
    return (PyObject*)self;
}

PyObject* create_borrowed_state_proxy(
    const GraphHandle* handle,
    const WorkflowState& state,
    const BlobStore& blobs,
    const StringInterner& strings,
    RuntimeViewProxy* borrowed_view,
    PyObject* runtime_capsule_owner,
    const std::shared_ptr<std::vector<std::string>>& state_names,
    const std::shared_ptr<std::unordered_map<std::string, StateKey>>& state_keys,
    PyObject* state_key_lookup
) {
    if (PyType_Ready(&StateProxyType) < 0) return nullptr;
    StateProxy* self = PyObject_New(StateProxy, &StateProxyType);
    if (self) {
        new (&self->owned_snapshot) std::shared_ptr<OwnedStateSnapshot>();
        new (&self->state_names) std::shared_ptr<std::vector<std::string>>(state_names);
        new (&self->state_keys) std::shared_ptr<std::unordered_map<std::string, StateKey>>(state_keys);
        self->state_key_lookup = state_key_lookup;
        Py_XINCREF(self->state_key_lookup);
        self->value_cache = nullptr;
        self->state = &state;
        self->blobs = &blobs;
        self->strings = &strings;
        self->borrowed_view = borrowed_view;
        self->runtime_capsule_owner = runtime_capsule_owner;
        Py_XINCREF(self->runtime_capsule_owner);
        self->handle = handle;
    }
    return reinterpret_cast<PyObject*>(self);
}

struct ConfigProxy {
    PyObject_HEAD
    PyObject* base_config;
    PyObject* runtime_capsule;
};

void ConfigProxy_dealloc(ConfigProxy* self) {
    Py_XDECREF(self->base_config);
    Py_XDECREF(self->runtime_capsule);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

Py_ssize_t ConfigProxy_length(PyObject* self) {
    auto* proxy = reinterpret_cast<ConfigProxy*>(self);
    return PyMapping_Size(proxy->base_config);
}

PyObject* ConfigProxy_subscript(PyObject* self, PyObject* key) {
    auto* proxy = reinterpret_cast<ConfigProxy*>(self);
    return PyObject_GetItem(proxy->base_config, key);
}

static PyMappingMethods ConfigProxy_as_mapping = {
    ConfigProxy_length,
    ConfigProxy_subscript,
    nullptr
};

PyObject* ConfigProxy_get(PyObject* self, PyObject* args) {
    PyObject *key, *default_value = Py_None;
    if (!PyArg_ParseTuple(args, "O|O", &key, &default_value)) {
        return nullptr;
    }
    PyObject* result = ConfigProxy_subscript(self, key);
    if (result == nullptr) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            Py_INCREF(default_value);
            return default_value;
        }
        return nullptr;
    }
    return result;
}

PyObject* ConfigProxy_keys(PyObject* self, PyObject*) {
    auto* proxy = reinterpret_cast<ConfigProxy*>(self);
    return PyMapping_Keys(proxy->base_config);
}

PyObject* ConfigProxy_items(PyObject* self, PyObject*) {
    auto* proxy = reinterpret_cast<ConfigProxy*>(self);
    return PyMapping_Items(proxy->base_config);
}

PyObject* ConfigProxy_copy(PyObject* self, PyObject*) {
    auto* proxy = reinterpret_cast<ConfigProxy*>(self);
    if (PyDict_Check(proxy->base_config)) {
        return PyDict_Copy(proxy->base_config);
    }
    PyObject* items = PyMapping_Items(proxy->base_config);
    if (items == nullptr) {
        return nullptr;
    }
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        Py_DECREF(items);
        return nullptr;
    }
    if (PyDict_MergeFromSeq2(dict, items, 1) != 0) {
        Py_DECREF(items);
        Py_DECREF(dict);
        return nullptr;
    }
    Py_DECREF(items);
    return dict;
}

PyObject* ConfigProxy_runtime_capsule(PyObject* self, PyObject*) {
    auto* proxy = reinterpret_cast<ConfigProxy*>(self);
    Py_INCREF(proxy->runtime_capsule);
    return proxy->runtime_capsule;
}

static PyMethodDef ConfigProxy_methods[] = {
    {"get", reinterpret_cast<PyCFunction>(ConfigProxy_get), METH_VARARGS, "Get value"},
    {"keys", reinterpret_cast<PyCFunction>(ConfigProxy_keys), METH_NOARGS, "Get keys"},
    {"items", reinterpret_cast<PyCFunction>(ConfigProxy_items), METH_NOARGS, "Get items"},
    {"copy", reinterpret_cast<PyCFunction>(ConfigProxy_copy), METH_NOARGS, "Copy to a dict"},
    {"_agentcore_runtime_capsule", reinterpret_cast<PyCFunction>(ConfigProxy_runtime_capsule), METH_NOARGS, "Return the active runtime capsule"},
    {nullptr, nullptr, 0, nullptr}
};

static PyTypeObject ConfigProxyType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "agentcore.ConfigProxy",          /* tp_name */
    sizeof(ConfigProxy),              /* tp_basicsize */
    0,                                /* tp_itemsize */
    (destructor)ConfigProxy_dealloc,  /* tp_dealloc */
    0,                                /* tp_print */
    0,                                /* tp_getattr */
    0,                                /* tp_setattr */
    0,                                /* tp_reserved */
    0,                                /* tp_repr */
    0,                                /* tp_as_number */
    0,                                /* tp_as_sequence */
    &ConfigProxy_as_mapping,          /* tp_as_mapping */
    0,                                /* tp_hash */
    0,                                /* tp_call */
    0,                                /* tp_str */
    0,                                /* tp_getattro */
    0,                                /* tp_setattro */
    0,                                /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,               /* tp_flags */
    "Config Proxy",                   /* tp_doc */
    0,                                /* tp_traverse */
    0,                                /* tp_clear */
    0,                                /* tp_richcompare */
    0,                                /* tp_weaklistoffset */
    0,                                /* tp_iter */
    0,                                /* tp_iternext */
    ConfigProxy_methods,              /* tp_methods */
    0,                                /* tp_members */
    0,                                /* tp_getset */
    0,                                /* tp_base */
    0,                                /* tp_dict */
    0,                                /* tp_descr_get */
    0,                                /* tp_descr_set */
    0,                                /* tp_dictoffset */
    0,                                /* tp_init */
    0,                                /* tp_alloc */
    0,                                /* tp_new */
    0,                                /* tp_free */
};

PyObject* create_config_proxy(PyObject* base_config, PyObject* runtime_capsule) {
    if (PyType_Ready(&ConfigProxyType) < 0) {
        return nullptr;
    }
    ConfigProxy* self = PyObject_New(ConfigProxy, &ConfigProxyType);
    if (self == nullptr) {
        return nullptr;
    }
    self->base_config = base_config;
    self->runtime_capsule = runtime_capsule;
    Py_XINCREF(self->base_config);
    Py_XINCREF(self->runtime_capsule);
    return reinterpret_cast<PyObject*>(self);
}

class GraphHandleRegistry {
public:
    static GraphHandleRegistry& instance() {
        static GraphHandleRegistry reg;
        return reg;
    }

    void add(GraphId id, GraphHandle* handle) {
        std::lock_guard<std::mutex> lock(mutex_);
        map_[id] = handle;
    }

    void remove(GraphId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        map_.erase(id);
    }

    GraphHandle* find(GraphId id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(id);
        return it == map_.end() ? nullptr : it->second;
    }

private:
    std::mutex mutex_;
    std::unordered_map<GraphId, GraphHandle*> map_;
};

[[nodiscard]] GraphId next_python_graph_id() {
    static std::atomic<GraphId> next_id{1000U};
    return next_id.fetch_add(1U);
}

} // namespace

std::string GraphHandle::fetch_python_error() {
    return fetch_python_error();
}

bool set_python_dict_item(PyObject* dict, const char* key, PyObject* value, std::string* error_message);

GraphHandle::GraphHandle(std::unique_ptr<ExecutionEngine> engine, std::string name)
    : engine_(std::move(engine)), name_(std::move(name)),
      state_names_(std::make_shared<std::vector<std::string>>()),
      state_keys_by_name_(std::make_shared<std::unordered_map<std::string, StateKey>>()) {
    graph_id_ = next_python_graph_id();
    GraphHandleRegistry::instance().add(graph_id_, this);

    if (Py_IsInitialized()) {
        const PyGILState_STATE gil = PyGILState_Ensure();
        state_key_lookup_ = PyDict_New();
        if (state_key_lookup_ == nullptr && PyErr_Occurred() != nullptr) {
            PyErr_Clear();
        }
        PyGILState_Release(gil);
    }

    // Reserved nodes
    node_ids_by_name_[kInternalBootstrapNodeName] = next_node_id_++;
    node_bindings_.push_back({node_ids_by_name_[kInternalBootstrapNodeName], kInternalBootstrapNodeName, nullptr, NodeKind::Compute});
    node_binding_indices_[node_ids_by_name_[kInternalBootstrapNodeName]] = node_bindings_.size() - 1U;

    node_ids_by_name_[kInternalEndNodeName] = next_node_id_++;
    node_bindings_.push_back({node_ids_by_name_[kInternalEndNodeName], kInternalEndNodeName, nullptr, NodeKind::Control, node_policy_mask(NodePolicyFlag::StopAfterNode)});
    node_binding_indices_[node_ids_by_name_[kInternalEndNodeName]] = node_bindings_.size() - 1U;
}

GraphHandle::~GraphHandle() {
    GraphHandleRegistry::instance().remove(graph_id_);
    if (Py_IsInitialized()) {
        const PyGILState_STATE gil = PyGILState_Ensure();
        for (auto& binding : node_bindings_) {
            Py_XDECREF(binding.callback);
            Py_XDECREF(binding.patch_key_lookup);
        }
        for (auto& entry : active_configs_) {
            Py_XDECREF(entry.second);
        }
        Py_XDECREF(state_key_lookup_);
        PyGILState_Release(gil);
    }
}

PyObject* GraphHandle::state_to_python_dict(const WorkflowState& state, const BlobStore& blobs, const StringInterner& strings, std::string* error_message) const {
    std::shared_ptr<std::vector<std::string>> names;
    std::shared_ptr<std::unordered_map<std::string, StateKey>> keys;
    PyObject* state_key_lookup = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        names = state_names_;
        keys = state_keys_by_name_;
        state_key_lookup = state_key_lookup_;
    }
    return create_state_proxy(this, state, blobs, strings, names, keys, state_key_lookup);
}

PyObject* GraphHandle::convert_value_to_python(const Value& value, const BlobStore& blobs, const StringInterner& strings, std::string* error_message) const {
    if (std::holds_alternative<std::monostate>(value)) {
        Py_RETURN_NONE;
    }
    if (std::holds_alternative<int64_t>(value)) {
        return PyLong_FromLongLong(std::get<int64_t>(value));
    }
    if (std::holds_alternative<double>(value)) {
        return PyFloat_FromDouble(std::get<double>(value));
    }
    if (std::holds_alternative<bool>(value)) {
        return PyBool_FromLong(std::get<bool>(value));
    }
    if (std::holds_alternative<InternedStringId>(value)) {
        return unicode_from_utf8(strings.resolve(std::get<InternedStringId>(value)));
    }
    if (std::holds_alternative<BlobRef>(value)) {
        BlobRef ref = std::get<BlobRef>(value);
        auto buffer = blobs.read_buffer(ref);
        if (!buffer.first) {
            Py_RETURN_NONE;
        }
        if (tagged_buffer_has_payload(buffer.first, buffer.second, kPickleBlobTag)) {
            return load_object_from_pickle_buffer(buffer.first, buffer.second, error_message);
        }
        if (tagged_buffer_has_payload(buffer.first, buffer.second, kJsonBlobTag)) {
            return load_object_from_json_buffer(buffer.first, buffer.second, error_message);
        }
        bool is_bytes = tagged_buffer_has_payload(buffer.first, buffer.second, kBytesBlobTag);
        return PyMemoryView_FromMemory(
            const_cast<char*>(reinterpret_cast<const char*>(is_bytes ? buffer.first + 1 : buffer.first)),
            is_bytes ? buffer.second - 1 : buffer.second,
            PyBUF_READ
        );
    }
    Py_RETURN_NONE;
}

bool GraphHandle::convert_python_value(PyObject* value, BlobStore& blobs, StringInterner& strings, Value* output, std::string* error_message) {
    if (value == Py_None) {
        *output = std::monostate{};
        return true;
    }
    if (PyBool_Check(value)) {
        *output = (value == Py_True);
        return true;
    }
    if (PyLong_Check(value)) {
        *output = (int64_t)PyLong_AsLongLong(value);
        return true;
    }
    if (PyFloat_Check(value)) {
        *output = PyFloat_AsDouble(value);
        return true;
    }
    if (PyUnicode_Check(value)) {
        *output = strings.intern(PyUnicode_AsUTF8(value));
        return true;
    }
    if (PyBytes_Check(value)) {
        char* buffer; Py_ssize_t size;
        PyBytes_AsStringAndSize(value, &buffer, &size);
        *output = append_tagged_blob(blobs, kBytesBlobTag, reinterpret_cast<const std::byte*>(buffer), static_cast<std::size_t>(size));
        return true;
    }

    // Default to pickle
    std::vector<std::byte> pickle_bytes;
    if (!dump_object_as_pickle_bytes(value, &pickle_bytes, error_message)) {
        return false;
    }
    *output = append_tagged_blob(blobs, kPickleBlobTag, pickle_bytes.data(), pickle_bytes.size());
    return true;
}

StateKey GraphHandle::ensure_state_key(std::string_view state_name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return ensure_state_key_locked(state_name);
}

StateKey GraphHandle::ensure_state_key_locked(std::string_view state_name) {
    auto it = state_keys_by_name_->find(std::string(state_name));
    if (it != state_keys_by_name_->end()) {
        return it->second;
    }
    StateKey key = static_cast<StateKey>(state_names_->size());
    state_names_->push_back(std::string(state_name));
    (*state_keys_by_name_)[std::string(state_name)] = key;

    if (state_key_lookup_ != nullptr && Py_IsInitialized()) {
        const bool release_gil = !PyGILState_Check();
        PyGILState_STATE gil_state{};
        if (release_gil) {
            gil_state = PyGILState_Ensure();
        }
        PyObject* py_name = unicode_from_utf8(state_name);
        PyObject* py_key = PyLong_FromUnsignedLong(static_cast<unsigned long>(key));
        if (py_name != nullptr && py_key != nullptr) {
            if (PyDict_SetItem(state_key_lookup_, py_name, py_key) != 0 &&
                PyErr_Occurred() != nullptr) {
                PyErr_Clear();
            }
        } else if (PyErr_Occurred() != nullptr) {
            PyErr_Clear();
        }
        Py_XDECREF(py_name);
        Py_XDECREF(py_key);
        if (release_gil) {
            PyGILState_Release(gil_state);
        }
    }
    return key;
}

bool GraphHandle::add_node(std::string_view name, PyObject* callback, NodeKind kind, uint32_t policy_flags, const NodeMemoizationPolicy& memoization, const std::vector<std::pair<std::string, JoinMergeStrategy>>& merge_rules, std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node_ids_by_name_.count(std::string(name))) {
        *error_message = "Node with name '" + std::string(name) + "' already exists";
        return false;
    }

    Py_XINCREF(callback);
    NodeId node_id = next_node_id_++;
    node_ids_by_name_[std::string(name)] = node_id;

    std::vector<FieldMergeRule> rules;
    for (const auto& [field, strategy] : merge_rules) {
        rules.push_back({ensure_state_key_locked(field), strategy});
    }

    node_bindings_.push_back({
        node_id,
        std::string(name),
        callback,
        kind,
        policy_flags,
        std::move(rules),
        memoization
    });
    node_binding_indices_[node_id] = node_bindings_.size() - 1U;

    if (!entry_node_id_) {
        entry_node_id_ = node_id;
    }

    return true;
}

bool GraphHandle::add_subgraph_node(std::string_view name, GraphHandle* subgraph_handle, std::string_view namespace_name, const std::vector<std::pair<std::string, std::string>>& input_bindings, const std::vector<std::pair<std::string, std::string>>& output_bindings, bool propagate_knowledge_graph, std::string_view session_mode_name, const std::optional<std::string>& session_id_source_name, std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (node_ids_by_name_.count(std::string(name))) {
        *error_message = "Node with name '" + std::string(name) + "' already exists";
        return false;
    }

    NodeId node_id = next_node_id_++;
    node_ids_by_name_[std::string(name)] = node_id;

    std::vector<SubgraphStateBinding> in, out;
    for (const auto& [p, c] : input_bindings) {
        in.push_back({ensure_state_key_locked(p), subgraph_handle->ensure_state_key(c)});
    }
    for (const auto& [p, c] : output_bindings) {
        out.push_back({ensure_state_key_locked(p), subgraph_handle->ensure_state_key(c)});
    }

    SubgraphBinding sb{subgraph_handle->id(), std::string(namespace_name), std::move(in), std::move(out), propagate_knowledge_graph};
    sb.session_mode = (session_mode_name == "persistent") ? SubgraphSessionMode::Persistent : SubgraphSessionMode::Ephemeral;
    if (session_id_source_name) {
        sb.session_id_source_key = ensure_state_key_locked(*session_id_source_name);
    }

    node_bindings_.push_back({
        node_id,
        std::string(name),
        nullptr,
        NodeKind::Subgraph,
        0U,
        {},
        {},
        sb,
        subgraph_handle
    });
    node_binding_indices_[node_id] = node_bindings_.size() - 1U;

    return true;
}

bool GraphHandle::set_entry_point(std::string_view name, std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = node_ids_by_name_.find(std::string(name));
    if (it == node_ids_by_name_.end()) {
        *error_message = "Node '" + std::string(name) + "' not found";
        return false;
    }
    entry_node_id_ = it->second;
    return true;
}

bool GraphHandle::add_edge(std::string_view source, std::string_view target, std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto s_it = node_ids_by_name_.find(std::string(source));
    auto t_it = node_ids_by_name_.find(std::string(target));
    if (s_it == node_ids_by_name_.end()) {
        *error_message = "Source node '" + std::string(source) + "' not found";
        return false;
    }
    if (t_it == node_ids_by_name_.end()) {
        *error_message = "Target node '" + std::string(target) + "' not found";
        return false;
    }
    edges_.push_back({s_it->second, t_it->second});
    return true;
}

bool GraphHandle::finalize(std::string* error_message) {
    return ensure_finalized(error_message);
}

bool GraphHandle::ensure_finalized(std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (finalized_) return true;
    return finalize_locked(error_message);
}

bool GraphHandle::finalize_locked(std::string* error_message) {
    if (!entry_node_id_) {
        *error_message = "Graph has no entry point";
        return false;
    }

    graph_ = GraphDefinition{};
    graph_.id = graph_id_;
    graph_.name = name_;
    graph_.entry = node_ids_by_name_[kInternalBootstrapNodeName];

    for (const auto& binding : node_bindings_) {
        NodeExecutorFn executor = nullptr;
        if (binding.name == kInternalBootstrapNodeName) {
            executor = python_bootstrap_executor;
        } else if (binding.kind != NodeKind::Subgraph) {
            executor = python_node_executor;
        }

        graph_.nodes.push_back({
            binding.node_id,
            binding.kind,
            binding.name,
            binding.policy_flags,
            0, 0, executor, {},
            binding.merge_rules,
            {},
            binding.subgraph,
            binding.memoization
        });
    }

    std::unordered_map<NodeId, std::vector<EdgeId>> adj;
    auto add_edge_internal = [&](NodeId from, NodeId to, EdgeKind kind) {
        EdgeId eid = next_edge_id_++;
        graph_.edges.push_back({eid, from, to, kind, nullptr, 100});
        adj[from].push_back(eid);
    };

    add_edge_internal(node_ids_by_name_[kInternalBootstrapNodeName], *entry_node_id_, EdgeKind::OnSuccess);

    for (const auto& edge : edges_) {
        add_edge_internal(edge.first, edge.second, EdgeKind::Always);
    }

    for (auto& node : graph_.nodes) {
        graph_.bind_outgoing_edges(node.id, adj[node.id]);
    }

    graph_.compile_runtime();
    finalized_ = true;
    return true;
}

bool GraphHandle::register_graph_hierarchy(ExecutionEngine& target_engine, std::string* error_message) {
    std::vector<GraphHandle*> subgraph_handles;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subgraph_handles.reserve(node_bindings_.size());
        for (const NodeBinding& binding : node_bindings_) {
            if (binding.subgraph_handle != nullptr) {
                subgraph_handles.push_back(binding.subgraph_handle);
            }
        }
    }

    for (GraphHandle* subgraph_handle : subgraph_handles) {
        if (!subgraph_handle->ensure_finalized(error_message)) {
            return false;
        }
        if (!subgraph_handle->register_graph_hierarchy(target_engine, error_message)) {
            return false;
        }
        target_engine.register_graph(subgraph_handle->graph_);
    }

    return true;
}

bool GraphHandle::execute_run(PyObject* input_state, PyObject* config, bool include_subgraphs, bool until_pause, RunArtifacts* artifacts, std::string* error_message) {
    if (!ensure_finalized(error_message)) return false;
    if (!register_graph_hierarchy(*engine_, error_message)) return false;

    InputEnvelope envelope;
    if (!build_initial_envelope(input_state, config, &envelope, error_message)) return false;

    RunId run_id = engine_->start(graph_, envelope);

    if (config && config != Py_None) {
        std::lock_guard<std::mutex> lock(mutex_);
        Py_INCREF(config);
        active_configs_[run_id] = config;
    }

    RunResult result;
    if (until_pause) {
        Py_BEGIN_ALLOW_THREADS
        while (true) {
            const StepResult step_result = engine_->step(run_id);
            result.run_id = run_id;
            result.status = step_result.status;
            result.last_checkpoint_id = step_result.checkpoint_id;
            if (step_result.progressed) {
                result.steps_executed += 1U;
            }

            if (step_result.waiting ||
                step_result.status == ExecutionStatus::Paused ||
                step_result.status == ExecutionStatus::Completed ||
                step_result.status == ExecutionStatus::Failed ||
                step_result.status == ExecutionStatus::Cancelled ||
                !step_result.progressed) {
                break;
            }
        }
        Py_END_ALLOW_THREADS
    } else {
        Py_BEGIN_ALLOW_THREADS
        result = engine_->run_to_completion(run_id);
        Py_END_ALLOW_THREADS
    }

    bool ok = populate_run_artifacts(run_id, result, include_subgraphs, artifacts, error_message);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_configs_.find(run_id);
        if (it != active_configs_.end()) {
            Py_XDECREF(it->second);
            active_configs_.erase(it);
        }
    }

    return ok;
}

bool GraphHandle::invoke(PyObject* input_state, PyObject* config, PyObject** result, std::string* error_message) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, true, false, &artifacts, error_message)) return false;
    *result = artifacts.state;
    Py_XDECREF(artifacts.trace);
    return true;
}

bool GraphHandle::stream(PyObject* input_state, PyObject* config, bool include_subgraphs, PyObject** result, std::string* error_message) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, false, &artifacts, error_message)) return false;
    *result = artifacts.trace;
    Py_XDECREF(artifacts.state);
    return true;
}

bool GraphHandle::invoke_with_details(PyObject* input_state, PyObject* config, bool include_subgraphs, PyObject** result, std::string* error_message) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, false, &artifacts, error_message)) return false;
    *result = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    return *result != nullptr;
}

bool GraphHandle::invoke_until_pause_with_details(PyObject* input_state, PyObject* config, bool include_subgraphs, PyObject** result, std::string* error_message) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, true, &artifacts, error_message)) return false;
    *result = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    return *result != nullptr;
}

bool GraphHandle::resume_with_details(CheckpointId checkpoint_id, bool include_subgraphs, PyObject** result, std::string* error_message) {
    const ResumeResult resume_result = engine_->resume(checkpoint_id);
    if (!resume_result.resumed) {
        *error_message = resume_result.message.empty()
            ? "failed to resume from checkpoint"
            : resume_result.message;
        return false;
    }

    RunResult run_result;
    run_result.run_id = resume_result.run_id;
    run_result.status = resume_result.status;
    run_result.last_checkpoint_id = checkpoint_id;

    if (resume_result.status != ExecutionStatus::Completed &&
        resume_result.status != ExecutionStatus::Failed &&
        resume_result.status != ExecutionStatus::Cancelled) {
        Py_BEGIN_ALLOW_THREADS
        run_result = engine_->run_to_completion(resume_result.run_id);
        Py_END_ALLOW_THREADS
    }

    RunArtifacts artifacts;
    if (!populate_run_artifacts(resume_result.run_id, run_result, include_subgraphs, &artifacts, error_message)) {
        return false;
    }
    *result = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    return *result != nullptr;
}

bool GraphHandle::populate_run_artifacts(RunId run_id, const RunResult& run_result, bool include_subgraphs, RunArtifacts* artifacts, std::string* error_message) {
    const auto& store = engine_->state_store(run_id);
    artifacts->state = state_to_python_dict(engine_->state(run_id), store.blobs(), store.strings(), error_message);
    artifacts->trace = build_trace_list(run_id, include_subgraphs, error_message);
    artifacts->run_id = run_id;
    artifacts->result = run_result;
    // Compute proof digest if possible
    try {
        artifacts->proof = compute_run_proof_digest(engine_->inspect(run_id), engine_->trace().events_for_run(run_id));
    } catch (...) {
        artifacts->proof = {};
    }
    return artifacts->state && artifacts->trace;
}

PyObject* GraphHandle::build_trace_list(RunId run_id, bool include_subgraphs, std::string* error_message) const {
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    StreamCursor cursor;
    const std::vector<StreamEvent> events = engine_->stream_events(
        run_id,
        cursor,
        StreamReadOptions{include_subgraphs}
    );

    for (const StreamEvent& event : events) {
        const GraphHandle* event_handle = GraphHandleRegistry::instance().find(event.graph_id);
        const std::string name = event_handle == nullptr
            ? node_name(event.node_id)
            : event_handle->node_name(event.node_id);
        const std::string graph_name = event_handle == nullptr
            ? graph_.name
            : event_handle->graph_.name;
        if (name == kInternalBootstrapNodeName || name == kInternalEndNodeName) {
            continue;
        }

        PyObject* dict = PyDict_New();
        if (dict == nullptr) {
            Py_DECREF(list);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }

        PyObject* namespaces = PyList_New(0);
        if (namespaces == nullptr) {
            Py_DECREF(dict);
            Py_DECREF(list);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }

        for (const StreamNamespaceFrame& frame : event.namespaces) {
            PyObject* frame_dict = PyDict_New();
            if (frame_dict == nullptr ||
                !set_python_dict_item(
                    frame_dict,
                    "graph_name",
                    unicode_from_utf8(frame.graph_name),
                    error_message
                ) ||
                !set_python_dict_item(
                    frame_dict,
                    "node_name",
                    unicode_from_utf8(frame.node_name),
                    error_message
                ) ||
                !set_python_dict_item(
                    frame_dict,
                    "session_id",
                    unicode_from_utf8(frame.session_id),
                    error_message
                ) ||
                !set_python_dict_item(
                    frame_dict,
                    "session_revision",
                    PyLong_FromUnsignedLongLong(frame.session_revision),
                    error_message
                )) {
                Py_XDECREF(frame_dict);
                Py_DECREF(namespaces);
                Py_DECREF(dict);
                Py_DECREF(list);
                return nullptr;
            }
            if (PyList_Append(namespaces, frame_dict) != 0) {
                Py_DECREF(frame_dict);
                Py_DECREF(namespaces);
                Py_DECREF(dict);
                Py_DECREF(list);
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return nullptr;
            }
            Py_DECREF(frame_dict);
        }

        if (!set_python_dict_item(dict, "node_name", unicode_from_utf8(name), error_message) ||
            !set_python_dict_item(dict, "graph_name", unicode_from_utf8(graph_name), error_message) ||
            !set_python_dict_item(
                dict,
                "session_id",
                unicode_from_utf8(event.session_id),
                error_message
            ) ||
            !set_python_dict_item(
                dict,
                "session_revision",
                PyLong_FromUnsignedLongLong(event.session_revision),
                error_message
            ) ||
            !set_python_dict_item(dict, "namespaces", namespaces, error_message)) {
            Py_DECREF(dict);
            Py_DECREF(list);
            return nullptr;
        }

        if (PyList_Append(list, dict) != 0) {
            Py_DECREF(dict);
            Py_DECREF(list);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        Py_DECREF(dict);
    }
    return list;
}

PyObject* GraphHandle::build_details_dict(const RunArtifacts& artifacts, std::string* error_message) const {
    PyObject* dict = PyDict_New();
    PyDict_SetItemString(dict, "state", artifacts.state);
    PyDict_SetItemString(dict, "trace", artifacts.trace);

    PyObject* summary = PyDict_New();
    const char* status_str = "unknown";
    switch (artifacts.result.status) {
        case ExecutionStatus::Completed: status_str = "completed"; break;
        case ExecutionStatus::Failed: status_str = "failed"; break;
        case ExecutionStatus::Paused: status_str = "paused"; break;
        case ExecutionStatus::Cancelled: status_str = "cancelled"; break;
        case ExecutionStatus::Running: status_str = "running"; break;
        default: break;
    }
    PyDict_SetItemString(summary, "status", unicode_from_utf8(status_str));
    PyDict_SetItemString(summary, "steps_executed", PyLong_FromUnsignedLongLong(artifacts.result.steps_executed));
    PyDict_SetItemString(summary, "run_id", PyLong_FromUnsignedLongLong(artifacts.run_id));
    PyDict_SetItemString(
        summary,
        "checkpoint_id",
        PyLong_FromUnsignedLongLong(artifacts.result.last_checkpoint_id)
    );

    PyDict_SetItemString(dict, "summary", summary);
    Py_DECREF(summary);

    PyObject* proof = PyDict_New();
    PyDict_SetItemString(
        proof,
        "snapshot_digest",
        PyLong_FromUnsignedLongLong(artifacts.proof.snapshot_digest)
    );
    PyDict_SetItemString(
        proof,
        "trace_digest",
        PyLong_FromUnsignedLongLong(artifacts.proof.trace_digest)
    );
    PyDict_SetItemString(
        proof,
        "combined_digest",
        PyLong_FromUnsignedLongLong(artifacts.proof.combined_digest)
    );
    PyDict_SetItemString(dict, "proof", proof);
    Py_DECREF(proof);

    return dict;
}

bool GraphHandle::build_initial_envelope(PyObject* input_state, PyObject* config, InputEnvelope* envelope, std::string* error_message) {
    if (input_state && input_state != Py_None) {
        if (!PyDict_Check(input_state)) {
            *error_message = "input_state must be a dict";
            return false;
        }
        if (!convert_mapping_to_patch(input_state, envelope->initial_blobs, envelope->initial_strings, &envelope->initial_patch, error_message)) {
            return false;
        }
    }

    if (config != nullptr && config != Py_None) {
        std::vector<std::byte> pickle_bytes;
        if (!dump_object_as_pickle_bytes(config, &pickle_bytes, error_message)) {
            return false;
        }
        envelope->runtime_config_payload.resize(pickle_bytes.size() + 1U);
        envelope->runtime_config_payload.front() = kPickleBlobTag;
        if (!pickle_bytes.empty()) {
            std::memcpy(
                envelope->runtime_config_payload.data() + 1,
                pickle_bytes.data(),
                pickle_bytes.size()
            );
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    envelope->initial_field_count = state_names_->size();
    return true;
}

bool GraphHandle::convert_mapping_to_patch(
    PyObject* mapping,
    BlobStore& blobs,
    StringInterner& strings,
    StatePatch* patch,
    std::string* error_message,
    PyObject* fast_state_key_lookup
) {
    const Py_ssize_t mapping_size = PyDict_Size(mapping);
    if (mapping_size < 0) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return false;
    }
    if (mapping_size == 0) {
        return true;
    }

    patch->updates.reserve(
        patch->updates.size() + static_cast<std::size_t>(mapping_size)
    );

    PyObject* global_state_key_lookup = state_key_lookup_;
    PyObject *key, *value;
    Py_ssize_t pos = 0;
    while (PyDict_Next(mapping, &pos, &key, &value)) {
        if (!PyUnicode_Check(key)) {
            if (error_message != nullptr) {
                *error_message = "state updates must use string keys";
            }
            return false;
        }

        StateKey state_key = 0;
        bool found = false;
        if (!lookup_state_key_in_python_dict(
                fast_state_key_lookup,
                key,
                &state_key,
                &found,
                error_message
            )) {
            return false;
        }
        if (!found &&
            global_state_key_lookup != nullptr &&
            global_state_key_lookup != fast_state_key_lookup) {
            if (!lookup_state_key_in_python_dict(
                    global_state_key_lookup,
                    key,
                    &state_key,
                    &found,
                    error_message
                )) {
                return false;
            }
            if (found &&
                fast_state_key_lookup != nullptr &&
                !cache_state_key_in_python_dict(
                    fast_state_key_lookup,
                    key,
                    state_key,
                    error_message
                )) {
                return false;
            }
        }

        if (!found) {
            std::string key_str = python_string(key, error_message);
            if (error_message != nullptr && !error_message->empty()) {
                return false;
            }
            state_key = ensure_state_key(key_str);
            if (fast_state_key_lookup != nullptr &&
                !cache_state_key_in_python_dict(
                    fast_state_key_lookup,
                    key,
                    state_key,
                    error_message
                )) {
                return false;
            }
        }

        Value val;
        if (!convert_python_value(value, blobs, strings, &val, error_message)) {
            return false;
        }

        patch->updates.push_back({state_key, std::move(val)});
    }
    return true;
}

std::string GraphHandle::node_name(NodeId node_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& binding : node_bindings_) {
        if (binding.node_id == node_id) return binding.name;
    }
    return "unknown";
}

std::shared_ptr<PyObject> make_owned_python_callback(PyObject* callback) {
    Py_XINCREF(callback);
    return std::shared_ptr<PyObject>(
        callback,
        [](PyObject* object) {
            if (object == nullptr || Py_IsInitialized() == 0) {
                return;
            }
            const PyGILState_STATE gil = PyGILState_Ensure();
            Py_DECREF(object);
            PyGILState_Release(gil);
        }
    );
}

bool parse_python_bool_default(PyObject* value, bool default_value, bool* output, std::string* error_message) {
    if (value == nullptr || value == Py_None) {
        *output = default_value;
        return true;
    }
    const int truth = PyObject_IsTrue(value);
    if (truth < 0) {
        *error_message = GraphHandle::fetch_python_error();
        return false;
    }
    *output = truth != 0;
    return true;
}

bool parse_python_uint32_default(
    PyObject* value,
    uint32_t default_value,
    uint32_t* output,
    std::string* error_message
) {
    if (value == nullptr || value == Py_None) {
        *output = default_value;
        return true;
    }
    const unsigned long long parsed = PyLong_AsUnsignedLongLong(value);
    if (PyErr_Occurred() != nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        PyErr_Clear();
        return false;
    }
    *output = static_cast<uint32_t>(parsed);
    return true;
}

bool parse_python_uint16_default(
    PyObject* value,
    uint16_t default_value,
    uint16_t* output,
    std::string* error_message
) {
    uint32_t parsed = default_value;
    if (!parse_python_uint32_default(value, default_value, &parsed, error_message)) {
        return false;
    }
    *output = static_cast<uint16_t>(parsed);
    return true;
}

bool parse_python_float_default(
    PyObject* value,
    float default_value,
    float* output,
    std::string* error_message
) {
    if (value == nullptr || value == Py_None) {
        *output = default_value;
        return true;
    }
    const double parsed = PyFloat_AsDouble(value);
    if (PyErr_Occurred() != nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        PyErr_Clear();
        return false;
    }
    *output = static_cast<float>(parsed);
    return true;
}

bool parse_python_bytes_payload(
    PyObject* value,
    std::vector<std::byte>* output,
    std::string* error_message
) {
    output->clear();
    if (value == nullptr || value == Py_None) {
        return true;
    }

    Py_buffer view{};
    if (PyObject_GetBuffer(value, &view, PyBUF_SIMPLE) != 0) {
        *error_message = "expected a bytes-like payload";
        PyErr_Clear();
        return false;
    }
    const auto* begin = static_cast<const std::byte*>(view.buf);
    output->assign(begin, begin + static_cast<std::size_t>(view.len));
    PyBuffer_Release(&view);
    return true;
}

bool tool_response_from_python_result(
    PyObject* result,
    ToolInvocationContext& context,
    ToolResponse* response,
    std::string* error_message
) {
    if (!PyMapping_Check(result)) {
        *error_message = "python tool adapters must return a mapping result";
        return false;
    }

    PyObject* ok_object = PyMapping_GetItemString(result, "ok");
    PyObject* output_object = PyMapping_GetItemString(result, "output");
    PyObject* flags_object = PyMapping_GetItemString(result, "flags");
    PyObject* attempts_object = PyMapping_GetItemString(result, "attempts");
    PyObject* latency_object = PyMapping_GetItemString(result, "latency_ns");

    if (PyErr_Occurred() != nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        PyErr_Clear();
        Py_XDECREF(ok_object);
        Py_XDECREF(output_object);
        Py_XDECREF(flags_object);
        Py_XDECREF(attempts_object);
        Py_XDECREF(latency_object);
        return false;
    }

    std::vector<std::byte> output_bytes;
    bool ok = true;
    uint32_t flags = 0U;
    uint16_t attempts = 0U;
    uint32_t latency = 0U;
    const bool parsed =
        parse_python_bool_default(ok_object, true, &ok, error_message) &&
        parse_python_bytes_payload(output_object, &output_bytes, error_message) &&
        parse_python_uint32_default(flags_object, 0U, &flags, error_message) &&
        parse_python_uint16_default(attempts_object, 0U, &attempts, error_message) &&
        parse_python_uint32_default(latency_object, 0U, &latency, error_message);

    Py_XDECREF(ok_object);
    Py_XDECREF(output_object);
    Py_XDECREF(flags_object);
    Py_XDECREF(attempts_object);
    Py_XDECREF(latency_object);

    if (!parsed) {
        return false;
    }

    response->ok = ok;
    response->output = output_bytes.empty()
        ? BlobRef{}
        : context.blobs.append(output_bytes.data(), output_bytes.size());
    response->flags = flags;
    response->attempts = attempts;
    response->latency_ns = latency;
    return true;
}

bool model_response_from_python_result(
    PyObject* result,
    ModelInvocationContext& context,
    ModelResponse* response,
    std::string* error_message
) {
    if (!PyMapping_Check(result)) {
        *error_message = "python model adapters must return a mapping result";
        return false;
    }

    PyObject* ok_object = PyMapping_GetItemString(result, "ok");
    PyObject* output_object = PyMapping_GetItemString(result, "output");
    PyObject* confidence_object = PyMapping_GetItemString(result, "confidence");
    PyObject* token_usage_object = PyMapping_GetItemString(result, "token_usage");
    PyObject* flags_object = PyMapping_GetItemString(result, "flags");
    PyObject* attempts_object = PyMapping_GetItemString(result, "attempts");
    PyObject* latency_object = PyMapping_GetItemString(result, "latency_ns");

    if (PyErr_Occurred() != nullptr) {
        *error_message = GraphHandle::fetch_python_error();
        PyErr_Clear();
        Py_XDECREF(ok_object);
        Py_XDECREF(output_object);
        Py_XDECREF(confidence_object);
        Py_XDECREF(token_usage_object);
        Py_XDECREF(flags_object);
        Py_XDECREF(attempts_object);
        Py_XDECREF(latency_object);
        return false;
    }

    std::vector<std::byte> output_bytes;
    bool ok = true;
    float confidence = 1.0F;
    uint32_t token_usage = 0U;
    uint32_t flags = 0U;
    uint16_t attempts = 0U;
    uint32_t latency = 0U;
    const bool parsed =
        parse_python_bool_default(ok_object, true, &ok, error_message) &&
        parse_python_bytes_payload(output_object, &output_bytes, error_message) &&
        parse_python_float_default(confidence_object, 1.0F, &confidence, error_message) &&
        parse_python_uint32_default(token_usage_object, 0U, &token_usage, error_message) &&
        parse_python_uint32_default(flags_object, 0U, &flags, error_message) &&
        parse_python_uint16_default(attempts_object, 0U, &attempts, error_message) &&
        parse_python_uint32_default(latency_object, 0U, &latency, error_message);

    Py_XDECREF(ok_object);
    Py_XDECREF(output_object);
    Py_XDECREF(confidence_object);
    Py_XDECREF(token_usage_object);
    Py_XDECREF(flags_object);
    Py_XDECREF(attempts_object);
    Py_XDECREF(latency_object);

    if (!parsed) {
        return false;
    }

    response->ok = ok;
    response->output = output_bytes.empty()
        ? BlobRef{}
        : context.blobs.append(output_bytes.data(), output_bytes.size());
    response->confidence = confidence;
    response->token_usage = token_usage;
    response->flags = flags;
    response->attempts = attempts;
    response->latency_ns = latency;
    return true;
}

bool GraphHandle::register_python_tool(std::string_view name, PyObject* callback, ToolPolicy policy, AdapterMetadata metadata, std::string* error_message) {
    if (name.empty()) {
        *error_message = "tool name must be a non-empty string";
        return false;
    }
    if (callback == nullptr || !PyCallable_Check(callback)) {
        *error_message = "tool callback must be callable";
        return false;
    }

    const std::string tool_name(name);
    auto owned_callback = make_owned_python_callback(callback);
    tools().register_tool(
        tool_name,
        policy,
        std::move(metadata),
        [owned_callback, tool_name](const ToolRequest& request, ToolInvocationContext& context) {
            ToolResponse response{};
            response.ok = false;
            response.flags = kToolFlagHandlerException;

            const std::vector<std::byte> input_bytes = request.input.empty()
                ? std::vector<std::byte>{}
                : context.blobs.read_bytes(request.input);

            const PyGILState_STATE gil = PyGILState_Ensure();
            PyObject* input_object = PyBytes_FromStringAndSize(
                reinterpret_cast<const char*>(input_bytes.data()),
                static_cast<Py_ssize_t>(input_bytes.size())
            );
            if (input_object == nullptr) {
                PyGILState_Release(gil);
                return response;
            }

            PyObject* result = PyObject_CallFunctionObjArgs(owned_callback.get(), input_object, nullptr);
            Py_DECREF(input_object);
            if (result == nullptr) {
                PyErr_Print();
                PyGILState_Release(gil);
                return response;
            }

            std::string parse_error;
            const bool parsed = tool_response_from_python_result(
                result,
                context,
                &response,
                &parse_error
            );
            Py_DECREF(result);
            if (!parsed) {
                PyErr_Clear();
                response.ok = false;
                response.flags = kToolFlagValidationError;
            }
            PyGILState_Release(gil);
            return response;
        }
    );
    return true;
}

bool GraphHandle::register_python_model(std::string_view name, PyObject* callback, ModelPolicy policy, AdapterMetadata metadata, std::string* error_message) {
    if (name.empty()) {
        *error_message = "model name must be a non-empty string";
        return false;
    }
    if (callback == nullptr || !PyCallable_Check(callback)) {
        *error_message = "model callback must be callable";
        return false;
    }

    const std::string model_name(name);
    auto owned_callback = make_owned_python_callback(callback);
    models().register_model(
        model_name,
        policy,
        std::move(metadata),
        [owned_callback, model_name](const ModelRequest& request, ModelInvocationContext& context) {
            ModelResponse response{};
            response.ok = false;
            response.flags = kModelFlagHandlerException;

            const std::vector<std::byte> prompt_bytes = request.prompt.empty()
                ? std::vector<std::byte>{}
                : context.blobs.read_bytes(request.prompt);
            const std::vector<std::byte> schema_bytes = request.schema.empty()
                ? std::vector<std::byte>{}
                : context.blobs.read_bytes(request.schema);

            const PyGILState_STATE gil = PyGILState_Ensure();
            PyObject* prompt_object = PyBytes_FromStringAndSize(
                reinterpret_cast<const char*>(prompt_bytes.data()),
                static_cast<Py_ssize_t>(prompt_bytes.size())
            );
            PyObject* schema_object = PyBytes_FromStringAndSize(
                reinterpret_cast<const char*>(schema_bytes.data()),
                static_cast<Py_ssize_t>(schema_bytes.size())
            );
            PyObject* max_tokens_object = PyLong_FromUnsignedLong(request.max_tokens);
            if (prompt_object == nullptr || schema_object == nullptr || max_tokens_object == nullptr) {
                Py_XDECREF(prompt_object);
                Py_XDECREF(schema_object);
                Py_XDECREF(max_tokens_object);
                PyGILState_Release(gil);
                return response;
            }

            PyObject* result = PyObject_CallFunctionObjArgs(
                owned_callback.get(),
                prompt_object,
                schema_object,
                max_tokens_object,
                nullptr
            );
            Py_DECREF(prompt_object);
            Py_DECREF(schema_object);
            Py_DECREF(max_tokens_object);
            if (result == nullptr) {
                PyErr_Print();
                PyGILState_Release(gil);
                return response;
            }

            std::string parse_error;
            const bool parsed = model_response_from_python_result(
                result,
                context,
                &response,
                &parse_error
            );
            Py_DECREF(result);
            if (!parsed) {
                PyErr_Clear();
                response.ok = false;
                response.flags = kModelFlagValidationError;
            }
            PyGILState_Release(gil);
            return response;
        }
    );
    return true;
}

NodeResult python_bootstrap_executor(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult python_node_executor(ExecutionContext& ctx) {
    GraphHandle* handle = GraphHandleRegistry::instance().find(ctx.graph_id);
    if (!handle) {
        return NodeResult{NodeResult::HardFail};
    }

    NodeBinding* binding = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const auto it = handle->node_binding_indices_.find(ctx.node_id);
        if (it != handle->node_binding_indices_.end()) {
            binding = &handle->node_bindings_[it->second];
        }
    }

    if (!binding || !binding->callback) {
        return NodeResult::success();
    }

    const PyGILState_STATE gil = PyGILState_Ensure();

    PyObject* runtime_capsule = create_runtime_view_proxy(ctx);
    if (runtime_capsule == nullptr) {
        PyGILState_Release(gil);
        return NodeResult{NodeResult::HardFail};
    }
    auto* runtime_view = reinterpret_cast<RuntimeViewProxy*>(runtime_capsule);

    std::shared_ptr<std::vector<std::string>> state_names;
    std::shared_ptr<std::unordered_map<std::string, StateKey>> state_keys;
    PyObject* state_key_lookup = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mutex_);
        state_names = handle->state_names_;
        state_keys = handle->state_keys_by_name_;
        state_key_lookup = handle->state_key_lookup_;
    }

    PyObject* py_state = create_borrowed_state_proxy(
        handle,
        ctx.state,
        ctx.blobs,
        ctx.strings,
        runtime_view,
        runtime_capsule,
        state_names,
        state_keys,
        state_key_lookup
    );
    if (py_state == nullptr) {
        Py_DECREF(runtime_capsule);
        PyGILState_Release(gil);
        return NodeResult{NodeResult::HardFail};
    }

    PyObject* base_config = nullptr;
    {
        std::lock_guard<std::mutex> lock(handle->mutex_);
        auto it = handle->active_configs_.find(ctx.run_id);
        if (it != handle->active_configs_.end()) {
            base_config = it->second;
            Py_INCREF(base_config);
        }
    }
    if (base_config == nullptr && !ctx.runtime_config_payload.empty()) {
        std::string config_error;
        if (tagged_blob_has_payload(ctx.runtime_config_payload, kPickleBlobTag)) {
            base_config = load_object_from_pickle_buffer(
                ctx.runtime_config_payload.data(),
                ctx.runtime_config_payload.size(),
                &config_error
            );
        } else if (tagged_blob_has_payload(ctx.runtime_config_payload, kJsonBlobTag)) {
            base_config = load_object_from_json_buffer(
                ctx.runtime_config_payload.data(),
                ctx.runtime_config_payload.size(),
                &config_error
            );
        }
        if (base_config == nullptr) {
            PyErr_Clear();
        }
    }
    if (base_config == nullptr) {
        base_config = PyDict_New();
    }
    if (base_config == nullptr) {
        runtime_view->active.store(false, std::memory_order_release);
        Py_DECREF(py_state);
        Py_DECREF(runtime_capsule);
        PyGILState_Release(gil);
        return NodeResult{NodeResult::HardFail};
    }
    PyObject* py_config = create_config_proxy(base_config, runtime_capsule);
    Py_DECREF(base_config);
    if (py_config == nullptr) {
        runtime_view->active.store(false, std::memory_order_release);
        Py_DECREF(py_state);
        Py_DECREF(runtime_capsule);
        PyGILState_Release(gil);
        return NodeResult{NodeResult::HardFail};
    }

    PyObject* args = PyTuple_Pack(2, py_state, py_config);
    if (args == nullptr) {
        runtime_view->active.store(false, std::memory_order_release);
        Py_DECREF(py_state);
        Py_DECREF(py_config);
        Py_DECREF(runtime_capsule);
        PyGILState_Release(gil);
        return NodeResult{NodeResult::HardFail};
    }
    PyObject* result = PyObject_CallObject(binding->callback, args);

    runtime_view->active.store(false, std::memory_order_release);

    Py_DECREF(args);
    Py_DECREF(py_state);
    Py_DECREF(py_config);
    Py_DECREF(runtime_capsule);

    NodeResult node_result{NodeResult::Success};

    if (!result) {
        std::cerr << "Python node execution failed: " << fetch_python_error() << std::endl;
        PyGILState_Release(gil);
        return NodeResult{NodeResult::HardFail};
    }

    PyObject *patch_obj = nullptr;
    PyObject *route_obj = nullptr;
    bool should_wait = false;

    if (PyDict_Check(result)) {
        patch_obj = result;
    } else if (PyTuple_Check(result)) {
        Py_ssize_t size = PyTuple_Size(result);
        if (size >= 1) patch_obj = PyTuple_GetItem(result, 0);
        if (size >= 2) route_obj = PyTuple_GetItem(result, 1);
        if (size >= 3) {
            PyObject* wait_value = PyTuple_GetItem(result, 2);
            if (wait_value && PyObject_IsTrue(wait_value)) should_wait = true;
        }
    } else if (PyUnicode_Check(result)) {
        route_obj = result;
    }

    std::string err;
    if (patch_obj && PyDict_Check(patch_obj)) {
        PyObject* patch_key_lookup = nullptr;
        if (PyDict_Size(patch_obj) > 0) {
            if (binding != nullptr && binding->patch_key_lookup == nullptr) {
                binding->patch_key_lookup = PyDict_New();
                if (binding->patch_key_lookup == nullptr) {
                    err = fetch_python_error();
                }
            }
            if (binding != nullptr) {
                patch_key_lookup = binding->patch_key_lookup;
            }
        }

        if (err.empty() &&
            !handle->convert_mapping_to_patch(
                patch_obj,
                ctx.blobs,
                ctx.strings,
                &node_result.patch,
                &err,
                patch_key_lookup
            )) {
            std::cerr << "Python patch conversion failed: " << err << std::endl;
            Py_DECREF(result);
            PyGILState_Release(gil);
            return NodeResult{NodeResult::HardFail};
        }
    }

    if (should_wait) {
        node_result.status = NodeResult::Waiting;
    } else if (route_obj && route_obj != Py_None) {
        if (PyUnicode_Check(route_obj)) {
            std::string next_name = PyUnicode_AsUTF8(route_obj);
            std::lock_guard<std::mutex> lock(handle->mutex_);
            auto it = handle->node_ids_by_name_.find(next_name);
            if (it != handle->node_ids_by_name_.end()) {
                node_result.next_override = it->second;
            } else if (next_name == kInternalEndNodeName) {
                node_result.flags |= node_policy_mask(NodePolicyFlag::StopAfterNode);
            }
        } else if (PyBool_Check(route_obj) && route_obj == Py_True) {
            node_result.flags |= node_policy_mask(NodePolicyFlag::StopAfterNode);
        }
    }

    Py_DECREF(result);
    PyGILState_Release(gil);
    return node_result;
}

NodeResult native_stop_executor(ExecutionContext&) {
    NodeResult result = NodeResult::success();
    result.flags |= node_policy_mask(NodePolicyFlag::StopAfterNode);
    return result;
}

GraphHandle* create_graph_handle(const char* name, std::size_t worker_count) {
    return new GraphHandle(std::make_unique<ExecutionEngine>(worker_count), name ? name : "");
}

PyObject* create_graph_capsule(GraphHandle* handle) {
    return PyCapsule_New(handle, kGraphCapsuleName, [](PyObject* capsule) {
        delete static_cast<GraphHandle*>(PyCapsule_GetPointer(capsule, kGraphCapsuleName));
    });
}

GraphHandle* graph_handle_from_capsule(PyObject* capsule) {
    return static_cast<GraphHandle*>(PyCapsule_GetPointer(capsule, kGraphCapsuleName));
}

bool parse_python_bytes_like(PyObject* object, std::vector<std::byte>* value, std::string* error_message) {
    if (PyBytes_Check(object)) {
        char* buffer; Py_ssize_t size;
        PyBytes_AsStringAndSize(object, &buffer, &size);
        value->assign(reinterpret_cast<std::byte*>(buffer), reinterpret_cast<std::byte*>(buffer) + size);
        return true;
    }
    if (error_message != nullptr) {
        *error_message = "expected a bytes object";
    }
    return false;
}

bool set_python_dict_item(PyObject* dict, const char* key, PyObject* value, std::string* error_message) {
    if (value == nullptr) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = fetch_python_error();
        }
        return false;
    }
    const int rc = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    if (rc != 0) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return false;
    }
    return true;
}

PyObject* adapter_capabilities_to_python(uint64_t capabilities, std::string* error_message) {
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    const auto append_capability = [&](AdapterCapability capability, const char* name) -> bool {
        if ((capabilities & static_cast<uint64_t>(capability)) == 0U) {
            return true;
        }
        PyObject* item = unicode_from_utf8(name);
        if (item == nullptr) {
            return false;
        }
        const int rc = PyList_Append(list, item);
        Py_DECREF(item);
        return rc == 0;
    };

    if (!append_capability(kAdapterCapabilitySync, "sync") ||
        !append_capability(kAdapterCapabilityAsync, "async") ||
        !append_capability(kAdapterCapabilityStreaming, "streaming") ||
        !append_capability(kAdapterCapabilityStructuredRequest, "structured_request") ||
        !append_capability(kAdapterCapabilityStructuredResponse, "structured_response") ||
        !append_capability(kAdapterCapabilityCheckpointSafe, "checkpoint_safe") ||
        !append_capability(kAdapterCapabilityExternalNetwork, "external_network") ||
        !append_capability(kAdapterCapabilityLocalFilesystem, "local_filesystem") ||
        !append_capability(kAdapterCapabilityJsonSchema, "json_schema") ||
        !append_capability(kAdapterCapabilityToolCalling, "tool_calling") ||
        !append_capability(kAdapterCapabilitySql, "sql") ||
        !append_capability(kAdapterCapabilityChatMessages, "chat_messages")) {
        Py_DECREF(list);
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    return list;
}

PyObject* adapter_metadata_to_python(const AdapterMetadata& metadata, std::string* error_message) {
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    if (!set_python_dict_item(dict, "provider", unicode_from_utf8(metadata.provider), error_message) ||
        !set_python_dict_item(dict, "implementation", unicode_from_utf8(metadata.implementation), error_message) ||
        !set_python_dict_item(dict, "display_name", unicode_from_utf8(metadata.display_name), error_message) ||
        !set_python_dict_item(
            dict,
            "transport",
            unicode_from_utf8(adapter_transport_name(metadata.transport)),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "auth",
            unicode_from_utf8(adapter_auth_name(metadata.auth)),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "capabilities",
            adapter_capabilities_to_python(metadata.capabilities, error_message),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "request_format",
            unicode_from_utf8(metadata.request_format),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "response_format",
            unicode_from_utf8(metadata.response_format),
            error_message
        )) {
        Py_DECREF(dict);
        return nullptr;
    }

    return dict;
}

PyObject* tool_policy_to_python(const ToolPolicy& policy, std::string* error_message) {
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }
    if (!set_python_dict_item(dict, "retry_limit", PyLong_FromUnsignedLong(policy.retry_limit), error_message) ||
        !set_python_dict_item(dict, "timeout_ms", PyLong_FromUnsignedLong(policy.timeout_ms), error_message) ||
        !set_python_dict_item(
            dict,
            "max_input_bytes",
            PyLong_FromUnsignedLongLong(policy.max_input_bytes),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "max_output_bytes",
            PyLong_FromUnsignedLongLong(policy.max_output_bytes),
            error_message
        )) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

PyObject* model_policy_to_python(const ModelPolicy& policy, std::string* error_message) {
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }
    if (!set_python_dict_item(dict, "retry_limit", PyLong_FromUnsignedLong(policy.retry_limit), error_message) ||
        !set_python_dict_item(dict, "timeout_ms", PyLong_FromUnsignedLong(policy.timeout_ms), error_message) ||
        !set_python_dict_item(
            dict,
            "max_prompt_bytes",
            PyLong_FromUnsignedLongLong(policy.max_prompt_bytes),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "max_output_bytes",
            PyLong_FromUnsignedLongLong(policy.max_output_bytes),
            error_message
        )) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

PyObject* blob_to_python_bytes(BlobRef blob, const BlobStore& blobs, std::string* error_message) {
    const std::vector<std::byte> payload = blob.empty()
        ? std::vector<std::byte>{}
        : blobs.read_bytes(blob);
    PyObject* value = PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(payload.data()),
        static_cast<Py_ssize_t>(payload.size())
    );
    if (value == nullptr && error_message != nullptr) {
        *error_message = fetch_python_error();
    }
    return value;
}

PyObject* tool_response_to_python(const ToolResponse& response, const BlobStore& blobs, std::string* error_message) {
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    const ToolErrorCategory category = classify_tool_response_flags(response.flags);
    if (!set_python_dict_item(dict, "ok", PyBool_FromLong(response.ok ? 1 : 0), error_message) ||
        !set_python_dict_item(dict, "output", blob_to_python_bytes(response.output, blobs, error_message), error_message) ||
        !set_python_dict_item(dict, "flags", PyLong_FromUnsignedLong(response.flags), error_message) ||
        !set_python_dict_item(dict, "attempts", PyLong_FromUnsignedLong(response.attempts), error_message) ||
        !set_python_dict_item(
            dict,
            "latency_ns",
            PyLong_FromUnsignedLongLong(response.latency_ns),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "error_category",
            unicode_from_utf8(tool_error_category_name(category)),
            error_message
        )) {
        Py_DECREF(dict);
        return nullptr;
    }

    return dict;
}

PyObject* model_response_to_python(const ModelResponse& response, const BlobStore& blobs, std::string* error_message) {
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    const ModelErrorCategory category = classify_model_response_flags(response.flags);
    if (!set_python_dict_item(dict, "ok", PyBool_FromLong(response.ok ? 1 : 0), error_message) ||
        !set_python_dict_item(dict, "output", blob_to_python_bytes(response.output, blobs, error_message), error_message) ||
        !set_python_dict_item(dict, "confidence", PyFloat_FromDouble(response.confidence), error_message) ||
        !set_python_dict_item(
            dict,
            "token_usage",
            PyLong_FromUnsignedLong(response.token_usage),
            error_message
        ) ||
        !set_python_dict_item(dict, "flags", PyLong_FromUnsignedLong(response.flags), error_message) ||
        !set_python_dict_item(dict, "attempts", PyLong_FromUnsignedLong(response.attempts), error_message) ||
        !set_python_dict_item(
            dict,
            "latency_ns",
            PyLong_FromUnsignedLongLong(response.latency_ns),
            error_message
        ) ||
        !set_python_dict_item(
            dict,
            "error_category",
            unicode_from_utf8(model_error_category_name(category)),
            error_message
        )) {
        Py_DECREF(dict);
        return nullptr;
    }

    return dict;
}

PyObject* list_registered_tools(const ToolRegistry& registry, std::string* error_message) {
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    for (const auto& named_spec : registry.registered_tools()) {
        PyObject* spec = PyDict_New();
        if (spec == nullptr) {
            Py_DECREF(list);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        if (!set_python_dict_item(spec, "name", unicode_from_utf8(named_spec.name), error_message) ||
            !set_python_dict_item(
                spec,
                "policy",
                tool_policy_to_python(named_spec.spec.policy, error_message),
                error_message
            ) ||
            !set_python_dict_item(
                spec,
                "metadata",
                adapter_metadata_to_python(named_spec.spec.metadata, error_message),
                error_message
            )) {
            Py_DECREF(spec);
            Py_DECREF(list);
            return nullptr;
        }
        if (PyList_Append(list, spec) != 0) {
            Py_DECREF(spec);
            Py_DECREF(list);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        Py_DECREF(spec);
    }
    return list;
}

PyObject* list_registered_models(const ModelRegistry& registry, std::string* error_message) {
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    for (const auto& named_spec : registry.registered_models()) {
        PyObject* spec = PyDict_New();
        if (spec == nullptr) {
            Py_DECREF(list);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        if (!set_python_dict_item(spec, "name", unicode_from_utf8(named_spec.name), error_message) ||
            !set_python_dict_item(
                spec,
                "policy",
                model_policy_to_python(named_spec.spec.policy, error_message),
                error_message
            ) ||
            !set_python_dict_item(
                spec,
                "metadata",
                adapter_metadata_to_python(named_spec.spec.metadata, error_message),
                error_message
            )) {
            Py_DECREF(spec);
            Py_DECREF(list);
            return nullptr;
        }
        if (PyList_Append(list, spec) != 0) {
            Py_DECREF(spec);
            Py_DECREF(list);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        Py_DECREF(spec);
    }
    return list;
}

PyObject* describe_registered_tool(const ToolRegistry& registry, std::string_view name, std::string* error_message) {
    const auto spec = registry.describe_tool(name);
    if (!spec.has_value()) {
        Py_RETURN_NONE;
    }

    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }
    if (!set_python_dict_item(dict, "name", unicode_from_utf8(spec->name), error_message) ||
        !set_python_dict_item(dict, "policy", tool_policy_to_python(spec->spec.policy, error_message), error_message) ||
        !set_python_dict_item(
            dict,
            "metadata",
            adapter_metadata_to_python(spec->spec.metadata, error_message),
            error_message
        )) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

PyObject* invoke_tool_registry(
    ToolRegistry& registry,
    std::string_view name,
    const std::vector<std::byte>& input,
    std::string* error_message
) {
    try {
        BlobStore blobs;
        StringInterner strings;
        ToolInvocationContext context{blobs, strings};
        const BlobRef input_ref = input.empty() ? BlobRef{} : blobs.append(input.data(), input.size());
        const ToolResponse response = registry.invoke(
            ToolRequest{strings.intern(name), input_ref},
            context
        );
        return tool_response_to_python(response, blobs, error_message);
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return nullptr;
    }
}

PyObject* runtime_invoke_tool(
    PyObject* capsule,
    std::string_view name,
    const std::vector<std::byte>& input,
    std::string* error_message
) {
    ExecutionContext* ctx = runtime_context_from_capsule(capsule, error_message);
    if (ctx == nullptr) {
        if (error_message != nullptr) {
            if (error_message->empty()) {
                *error_message = "invalid runtime capsule";
            }
        }
        return nullptr;
    }

    try {
        ToolInvocationContext context{ctx->blobs, ctx->strings};
        const BlobRef input_ref = input.empty() ? BlobRef{} : ctx->blobs.append(input.data(), input.size());
        const ToolResponse response = ctx->tools.invoke(
            ToolRequest{ctx->strings.intern(name), input_ref},
            context
        );
        return tool_response_to_python(response, ctx->blobs, error_message);
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return nullptr;
    }
}

PyObject* describe_registered_model(const ModelRegistry& registry, std::string_view name, std::string* error_message) {
    const auto spec = registry.describe_model(name);
    if (!spec.has_value()) {
        Py_RETURN_NONE;
    }

    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }
    if (!set_python_dict_item(dict, "name", unicode_from_utf8(spec->name), error_message) ||
        !set_python_dict_item(dict, "policy", model_policy_to_python(spec->spec.policy, error_message), error_message) ||
        !set_python_dict_item(
            dict,
            "metadata",
            adapter_metadata_to_python(spec->spec.metadata, error_message),
            error_message
        )) {
        Py_DECREF(dict);
        return nullptr;
    }
    return dict;
}

PyObject* invoke_model_registry(
    ModelRegistry& registry,
    std::string_view name,
    const std::vector<std::byte>& prompt,
    const std::vector<std::byte>& schema,
    uint32_t max_tokens,
    std::string* error_message
) {
    try {
        BlobStore blobs;
        StringInterner strings;
        ModelInvocationContext context{blobs, strings};
        const BlobRef prompt_ref = prompt.empty() ? BlobRef{} : blobs.append(prompt.data(), prompt.size());
        const BlobRef schema_ref = schema.empty() ? BlobRef{} : blobs.append(schema.data(), schema.size());
        const ModelResponse response = registry.invoke(
            ModelRequest{strings.intern(name), prompt_ref, schema_ref, max_tokens},
            context
        );
        return model_response_to_python(response, blobs, error_message);
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return nullptr;
    }
}

PyObject* runtime_invoke_model(
    PyObject* capsule,
    std::string_view name,
    const std::vector<std::byte>& prompt,
    const std::vector<std::byte>& schema,
    uint32_t max_tokens,
    std::string* error_message
) {
    ExecutionContext* ctx = runtime_context_from_capsule(capsule, error_message);
    if (ctx == nullptr) {
        if (error_message != nullptr) {
            if (error_message->empty()) {
                *error_message = "invalid runtime capsule";
            }
        }
        return nullptr;
    }

    try {
        ModelInvocationContext context{ctx->blobs, ctx->strings};
        const BlobRef prompt_ref = prompt.empty() ? BlobRef{} : ctx->blobs.append(prompt.data(), prompt.size());
        const BlobRef schema_ref = schema.empty() ? BlobRef{} : ctx->blobs.append(schema.data(), schema.size());
        const ModelResponse response = ctx->models.invoke(
            ModelRequest{ctx->strings.intern(name), prompt_ref, schema_ref, max_tokens},
            context
        );
        return model_response_to_python(response, ctx->blobs, error_message);
    } catch (const std::exception& error) {
        if (error_message != nullptr) {
            *error_message = error.what();
        }
        return nullptr;
    }
}

PyObject* runtime_record_once(PyObject* capsule, std::string_view key, PyObject* request, PyObject* producer, std::string* error_message) {
    ExecutionContext* ctx = runtime_context_from_capsule(capsule, error_message);
    if (!ctx) {
        if (error_message != nullptr && error_message->empty()) {
            *error_message = "Invalid runtime capsule";
        }
        return nullptr;
    }

    GraphHandle* handle = GraphHandleRegistry::instance().find(ctx->graph_id);
    if (!handle) {
        *error_message = "Graph handle not found";
        return nullptr;
    }

    Value req_val;
    if (!handle->convert_python_value(request, ctx->blobs, ctx->strings, &req_val, error_message)) {
        return nullptr;
    }

    BlobRef req_ref = std::holds_alternative<BlobRef>(req_val) ? std::get<BlobRef>(req_val) : BlobRef{};

    auto producer_fn = [ctx, producer, handle, error_message]() -> BlobRef {
        const PyGILState_STATE gil = PyGILState_Ensure();
        PyObject* res = PyObject_CallObject(producer, nullptr);
        if (!res) {
            *error_message = fetch_python_error();
            PyGILState_Release(gil);
            return BlobRef{};
        }
        Value v;
        handle->convert_python_value(res, ctx->blobs, ctx->strings, &v, error_message);
        Py_DECREF(res);
        PyGILState_Release(gil);
        return std::holds_alternative<BlobRef>(v) ? std::get<BlobRef>(v) : BlobRef{};
    };

    try {
        auto result = ctx->record_blob_effect_once(key, req_ref, producer_fn);
        PyObject* dict = PyDict_New();
        PyDict_SetItemString(dict, "replayed", PyBool_FromLong(result.replayed));
        PyObject* py_val = handle->convert_value_to_python(Value{result.output}, ctx->blobs, ctx->strings, error_message);
        PyDict_SetItemString(dict, "value", py_val);
        Py_DECREF(py_val);
        return dict;
    } catch (const std::exception& e) {
        *error_message = e.what();
        return nullptr;
    }
}

} // namespace agentcore::python_binding
