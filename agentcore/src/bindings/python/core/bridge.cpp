#include "bridge.h"
#include "agentcore/state/context/context_graph.h"
#include "agentcore/state/intelligence/ops.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/state/state_store.h"
#include "agentcore/execution/proof.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace agentcore::python_binding {

BlobRef python_object_to_blob_payload(PyObject* value, BlobStore& blobs, std::string* error_message);

namespace {

constexpr std::byte kPickleBlobTag{0x01};
constexpr std::byte kJsonBlobTag{0x02};
constexpr std::byte kBytesBlobTag{0x03};
constexpr std::byte kSequenceBlobTag{0x04};
constexpr std::byte kMessageBlobTag{0x05};

[[nodiscard]] const char* node_kind_name(NodeKind kind) noexcept {
    switch (kind) {
        case NodeKind::Compute: return "compute";
        case NodeKind::Control: return "control";
        case NodeKind::Tool: return "tool";
        case NodeKind::Model: return "model";
        case NodeKind::Aggregate: return "aggregate";
        case NodeKind::Human: return "human";
        case NodeKind::Subgraph: return "subgraph";
    }
    return "unknown";
}

[[nodiscard]] const char* node_result_status_name(NodeResult::Status status) noexcept {
    switch (status) {
        case NodeResult::Success: return "success";
        case NodeResult::SoftFail: return "soft_fail";
        case NodeResult::HardFail: return "hard_fail";
        case NodeResult::Waiting: return "waiting";
        case NodeResult::Cancelled: return "cancelled";
    }
    return "unknown";
}

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

void append_u64(std::vector<std::byte>& output, uint64_t value) {
    for (std::size_t byte_index = 0; byte_index < sizeof(uint64_t); ++byte_index) {
        output.push_back(static_cast<std::byte>((value >> (byte_index * 8U)) & 0xFFU));
    }
}

bool read_u64(const std::byte* data, std::size_t size, std::size_t* offset, uint64_t* value) {
    if (*offset > size || size - *offset < sizeof(uint64_t)) {
        return false;
    }
    uint64_t decoded = 0;
    for (std::size_t byte_index = 0; byte_index < sizeof(uint64_t); ++byte_index) {
        decoded |= static_cast<uint64_t>(
            std::to_integer<unsigned char>(data[*offset + byte_index])
        ) << (byte_index * 8U);
    }
    *value = decoded;
    *offset += sizeof(uint64_t);
    return true;
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

PyObject* load_object_from_pickle_payload(const std::byte* bytes, std::size_t size, std::string* err) {
    PyObject* p = get_pickle_module();
    if (!p) { *err = fetch_python_error(); return nullptr; }
    static PyObject* loads = PyObject_GetAttrString(p, "loads");

    PyObject* b = PyBytes_FromStringAndSize(
        reinterpret_cast<const char*>(bytes),
        static_cast<Py_ssize_t>(size)
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

bool extract_message_id(PyObject* message, std::string* id, std::string* err) {
    id->clear();

    PyObject* borrowed_id = nullptr;
    PyObject* owned_id = nullptr;
    if (PyDict_Check(message)) {
        borrowed_id = PyDict_GetItemString(message, "id");
    } else {
        owned_id = PyObject_GetAttrString(message, "id");
        if (owned_id == nullptr) {
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                PyErr_Clear();
            } else if (err != nullptr) {
                *err = fetch_python_error();
                return false;
            }
        }
    }

    PyObject* id_obj = owned_id != nullptr ? owned_id : borrowed_id;
    if (id_obj == nullptr || id_obj == Py_None) {
        Py_XDECREF(owned_id);
        return true;
    }

    PyObject* id_text = nullptr;
    if (PyUnicode_Check(id_obj)) {
        id_text = id_obj;
        Py_INCREF(id_text);
    } else {
        id_text = PyObject_Str(id_obj);
        if (id_text == nullptr) {
            Py_XDECREF(owned_id);
            if (err != nullptr) {
                *err = fetch_python_error();
            }
            return false;
        }
    }

    std::string id_error;
    *id = python_string(id_text, &id_error);
    Py_DECREF(id_text);
    Py_XDECREF(owned_id);
    if (!id_error.empty()) {
        if (err != nullptr) {
            *err = std::move(id_error);
        }
        return false;
    }
    return true;
}

bool dump_python_sequence_as_blob(PyObject* value, BlobStore& blobs, BlobRef* output, std::string* err) {
    PyObject* sequence = PySequence_Fast(value, "expected a Python sequence");
    if (sequence == nullptr) {
        if (err != nullptr) {
            *err = fetch_python_error();
        }
        return false;
    }

    const Py_ssize_t item_count = PySequence_Fast_GET_SIZE(sequence);
    PyObject** items = PySequence_Fast_ITEMS(sequence);

    std::vector<std::byte> bytes;
    bytes.reserve(1U + sizeof(uint64_t) + static_cast<std::size_t>(item_count) * sizeof(uint64_t));
    bytes.push_back(kSequenceBlobTag);
    append_u64(bytes, static_cast<uint64_t>(item_count));

    for (Py_ssize_t index = 0; index < item_count; ++index) {
        std::vector<std::byte> item_bytes;
        if (!dump_object_as_pickle_bytes(items[index], &item_bytes, err)) {
            Py_DECREF(sequence);
            return false;
        }
        append_u64(bytes, static_cast<uint64_t>(item_bytes.size()));
        bytes.insert(bytes.end(), item_bytes.begin(), item_bytes.end());
    }

    Py_DECREF(sequence);
    *output = blobs.append(bytes.data(), bytes.size());
    return true;
}

bool dump_python_messages_as_blob(PyObject* value, BlobStore& blobs, BlobRef* output, std::string* err) {
    PyObject* sequence = nullptr;
    if (PyList_Check(value) || PyTuple_Check(value)) {
        sequence = PySequence_Fast(value, "message state values must be a sequence");
    } else {
        sequence = PyTuple_Pack(1, value);
    }
    if (sequence == nullptr) {
        if (err != nullptr) {
            *err = fetch_python_error();
        }
        return false;
    }

    const Py_ssize_t item_count = PySequence_Fast_GET_SIZE(sequence);
    PyObject** items = PySequence_Fast_ITEMS(sequence);

    std::vector<std::byte> bytes;
    bytes.reserve(1U + sizeof(uint64_t) + static_cast<std::size_t>(item_count) * (2U * sizeof(uint64_t)));
    bytes.push_back(kMessageBlobTag);
    append_u64(bytes, static_cast<uint64_t>(item_count));

    for (Py_ssize_t index = 0; index < item_count; ++index) {
        std::string id;
        if (!extract_message_id(items[index], &id, err)) {
            Py_DECREF(sequence);
            return false;
        }

        std::vector<std::byte> item_bytes;
        if (!dump_object_as_pickle_bytes(items[index], &item_bytes, err)) {
            Py_DECREF(sequence);
            return false;
        }

        append_u64(bytes, static_cast<uint64_t>(id.size()));
        const auto* id_bytes = reinterpret_cast<const std::byte*>(id.data());
        bytes.insert(bytes.end(), id_bytes, id_bytes + id.size());
        append_u64(bytes, static_cast<uint64_t>(item_bytes.size()));
        bytes.insert(bytes.end(), item_bytes.begin(), item_bytes.end());
    }

    Py_DECREF(sequence);
    *output = blobs.append(bytes.data(), bytes.size());
    return true;
}

PyObject* load_sequence_blob_to_python_list(const std::byte* bytes, std::size_t size, std::string* err) {
    if (bytes == nullptr || size < 1U + sizeof(uint64_t) || bytes[0] != kSequenceBlobTag) {
        if (err != nullptr) {
            *err = "sequence payload is empty or invalid";
        }
        return nullptr;
    }

    std::size_t offset = 1U;
    uint64_t item_count = 0;
    if (!read_u64(bytes, size, &offset, &item_count)) {
        if (err != nullptr) {
            *err = "sequence payload is missing item count";
        }
        return nullptr;
    }
    if (item_count > static_cast<uint64_t>(std::numeric_limits<Py_ssize_t>::max())) {
        if (err != nullptr) {
            *err = "sequence payload has too many items for Python";
        }
        return nullptr;
    }

    PyObject* list = PyList_New(static_cast<Py_ssize_t>(item_count));
    if (list == nullptr) {
        if (err != nullptr) {
            *err = fetch_python_error();
        }
        return nullptr;
    }

    for (uint64_t index = 0; index < item_count; ++index) {
        uint64_t item_size = 0;
        if (!read_u64(bytes, size, &offset, &item_size) ||
            item_size > static_cast<uint64_t>(size - offset)) {
            Py_DECREF(list);
            if (err != nullptr) {
                *err = "sequence payload is truncated";
            }
            return nullptr;
        }

        PyObject* item = load_object_from_pickle_payload(
            bytes + offset,
            static_cast<std::size_t>(item_size),
            err
        );
        if (item == nullptr) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(index), item);
        offset += static_cast<std::size_t>(item_size);
    }

    if (offset != size) {
        Py_DECREF(list);
        if (err != nullptr) {
            *err = "sequence payload has trailing bytes";
        }
        return nullptr;
    }

    return list;
}

PyObject* load_message_blob_to_python_list(const std::byte* bytes, std::size_t size, std::string* err) {
    if (bytes == nullptr || size < 1U + sizeof(uint64_t) || bytes[0] != kMessageBlobTag) {
        if (err != nullptr) {
            *err = "message payload is empty or invalid";
        }
        return nullptr;
    }

    std::size_t offset = 1U;
    uint64_t item_count = 0;
    if (!read_u64(bytes, size, &offset, &item_count)) {
        if (err != nullptr) {
            *err = "message payload is missing item count";
        }
        return nullptr;
    }
    if (item_count > static_cast<uint64_t>(std::numeric_limits<Py_ssize_t>::max())) {
        if (err != nullptr) {
            *err = "message payload has too many items for Python";
        }
        return nullptr;
    }

    PyObject* list = PyList_New(static_cast<Py_ssize_t>(item_count));
    if (list == nullptr) {
        if (err != nullptr) {
            *err = fetch_python_error();
        }
        return nullptr;
    }

    for (uint64_t index = 0; index < item_count; ++index) {
        uint64_t id_size = 0;
        if (!read_u64(bytes, size, &offset, &id_size) ||
            id_size > static_cast<uint64_t>(size - offset)) {
            Py_DECREF(list);
            if (err != nullptr) {
                *err = "message payload is truncated at id";
            }
            return nullptr;
        }
        offset += static_cast<std::size_t>(id_size);

        uint64_t payload_size = 0;
        if (!read_u64(bytes, size, &offset, &payload_size) ||
            payload_size > static_cast<uint64_t>(size - offset)) {
            Py_DECREF(list);
            if (err != nullptr) {
                *err = "message payload is truncated";
            }
            return nullptr;
        }

        PyObject* item = load_object_from_pickle_payload(
            bytes + offset,
            static_cast<std::size_t>(payload_size),
            err
        );
        if (item == nullptr) {
            Py_DECREF(list);
            return nullptr;
        }
        PyList_SET_ITEM(list, static_cast<Py_ssize_t>(index), item);
        offset += static_cast<std::size_t>(payload_size);
    }

    if (offset != size) {
        Py_DECREF(list);
        if (err != nullptr) {
            *err = "message payload has trailing bytes";
        }
        return nullptr;
    }

    return list;
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

struct RuntimeKnowledgePathCacheKey {
    InternedStringId subject{0};
    InternedStringId relation{0};
    uint32_t limit{0};
    std::size_t entity_count{0};
    std::size_t triple_count{0};
    std::size_t pending_entity_count{0};
    std::size_t pending_triple_count{0};

    bool operator==(const RuntimeKnowledgePathCacheKey& other) const noexcept {
        return subject == other.subject &&
               relation == other.relation &&
               limit == other.limit &&
               entity_count == other.entity_count &&
               triple_count == other.triple_count &&
               pending_entity_count == other.pending_entity_count &&
               pending_triple_count == other.pending_triple_count;
    }
};

struct RuntimeKnowledgePathCacheKeyHash {
    std::size_t operator()(const RuntimeKnowledgePathCacheKey& key) const noexcept {
        std::size_t hash = static_cast<std::size_t>(key.subject);
        hash = (hash * 1315423911U) ^ static_cast<std::size_t>(key.relation);
        hash = (hash * 2654435761U) ^ static_cast<std::size_t>(key.limit);
        hash = (hash * 11400714819323198485ULL) ^ key.entity_count;
        hash = (hash * 1099511628211ULL) ^ key.triple_count;
        hash = (hash * 1469598103934665603ULL) ^ key.pending_entity_count;
        hash = (hash * 780291637U) ^ key.pending_triple_count;
        return hash;
    }
};

struct RuntimeKnowledgePathCacheTriple {
    KnowledgeTripleId id{0};
    InternedStringId subject{0};
    InternedStringId relation{0};
    InternedStringId object{0};
    BlobRef payload{};
};

struct RuntimeKnowledgePathCache {
    std::unordered_map<
        RuntimeKnowledgePathCacheKey,
        std::vector<RuntimeKnowledgePathCacheTriple>,
        RuntimeKnowledgePathCacheKeyHash
    > entries;
};

struct RuntimeContextGraphCache {
    uint64_t key{0};
    bool valid{false};
    IntelligenceStore intelligence;
    KnowledgeGraphStore knowledge_graph;
    std::unique_ptr<ContextGraphIndex> index;
    PyObject* result{nullptr};

    ~RuntimeContextGraphCache() {
        Py_XDECREF(result);
    }

    void clear_result() noexcept {
        Py_XDECREF(result);
        result = nullptr;
    }
};

struct RuntimeViewProxy {
    PyObject_HEAD
    ExecutionContext* runtime{nullptr};
    const WorkflowState* state{nullptr};
    const BlobStore* blobs{nullptr};
    const StringInterner* strings{nullptr};
    const IntelligenceStore* intelligence{nullptr};
    KnowledgeGraphPatch pending_knowledge_graph_patch;
    IntelligencePatch pending_intelligence_patch;
    RuntimeKnowledgePathCache knowledge_path_cache;
    RuntimeContextGraphCache context_graph_cache;
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
    self->pending_knowledge_graph_patch.~KnowledgeGraphPatch();
    self->pending_intelligence_patch.~IntelligencePatch();
    self->knowledge_path_cache.~RuntimeKnowledgePathCache();
    self->context_graph_cache.~RuntimeContextGraphCache();
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
        self->intelligence = &context.intelligence;
        new (&self->pending_knowledge_graph_patch) KnowledgeGraphPatch();
        new (&self->pending_intelligence_patch) IntelligencePatch();
        new (&self->knowledge_path_cache) RuntimeKnowledgePathCache();
        new (&self->context_graph_cache) RuntimeContextGraphCache();
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

PyObject* runtime_identity_impl(PyObject* capsule, std::string* error_message) {
    ExecutionContext* ctx = runtime_context_from_capsule(capsule, error_message);
    if (ctx == nullptr) {
        return nullptr;
    }

    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    auto set_item = [dict, error_message](const char* key, PyObject* value) {
        if (value == nullptr) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return false;
        }
        const int status = PyDict_SetItemString(dict, key, value);
        Py_DECREF(value);
        if (status != 0) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return false;
        }
        return true;
    };

    if (!set_item("run_id", PyLong_FromUnsignedLongLong(ctx->run_id)) ||
        !set_item("graph_id", PyLong_FromUnsignedLongLong(ctx->graph_id)) ||
        !set_item("node_id", PyLong_FromUnsignedLong(ctx->node_id)) ||
        !set_item("branch_id", PyLong_FromUnsignedLong(ctx->branch_id))) {
        Py_DECREF(dict);
        return nullptr;
    }

    return dict;
}

bool require_spec_dict(PyObject* spec, std::string* error_message) {
    if (spec != nullptr && PyDict_Check(spec)) {
        return true;
    }
    if (error_message != nullptr) {
        *error_message = "intelligence specification must be a dict";
    }
    return false;
}

bool parse_required_spec_string_id(
    PyObject* spec,
    const char* field_name,
    StringInterner& strings,
    InternedStringId* output,
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr || value == Py_None) {
        if (error_message != nullptr) {
            *error_message = std::string(field_name) + " is required";
        }
        return false;
    }
    if (!PyUnicode_Check(value)) {
        if (error_message != nullptr) {
            *error_message = std::string(field_name) + " must be a string";
        }
        return false;
    }
    *output = strings.intern(PyUnicode_AsUTF8(value));
    return true;
}

bool parse_optional_spec_string_id(
    PyObject* spec,
    const char* field_name,
    StringInterner& strings,
    InternedStringId* output,
    uint32_t mask_value,
    uint32_t* field_mask,
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr) {
        return true;
    }
    if (value == Py_None) {
        *output = 0U;
    } else {
        if (!PyUnicode_Check(value)) {
            if (error_message != nullptr) {
                *error_message = std::string(field_name) + " must be a string or None";
            }
            return false;
        }
        *output = strings.intern(PyUnicode_AsUTF8(value));
    }
    *field_mask |= mask_value;
    return true;
}

bool parse_optional_spec_blob(
    PyObject* spec,
    const char* field_name,
    BlobStore& blobs,
    BlobRef* output,
    uint32_t mask_value,
    uint32_t* field_mask,
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr) {
        return true;
    }
    *output = python_object_to_blob_payload(value, blobs, error_message);
    if (value != Py_None && output->empty() && error_message != nullptr && !error_message->empty()) {
        return false;
    }
    *field_mask |= mask_value;
    return true;
}

bool parse_optional_spec_string(
    PyObject* spec,
    const char* field_name,
    std::string* output,
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr || value == Py_None) {
        return true;
    }
    if (!PyUnicode_Check(value)) {
        if (error_message != nullptr) {
            *error_message = std::string(field_name) + " must be a string";
        }
        return false;
    }
    *output = PyUnicode_AsUTF8(value);
    return true;
}

bool parse_optional_spec_float(
    PyObject* spec,
    const char* field_name,
    float* output,
    uint32_t mask_value,
    uint32_t* field_mask,
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr) {
        return true;
    }
    if (value == Py_None) {
        *output = 0.0F;
    } else {
        const double raw = PyFloat_AsDouble(value);
        if (PyErr_Occurred() != nullptr) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            PyErr_Clear();
            return false;
        }
        *output = static_cast<float>(raw);
    }
    *field_mask |= mask_value;
    return true;
}

bool parse_optional_spec_int32(
    PyObject* spec,
    const char* field_name,
    int32_t* output,
    uint32_t mask_value,
    uint32_t* field_mask,
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr) {
        return true;
    }
    if (value == Py_None) {
        *output = 0;
    } else {
        const long raw = PyLong_AsLong(value);
        if (PyErr_Occurred() != nullptr) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            PyErr_Clear();
            return false;
        }
        *output = static_cast<int32_t>(raw);
    }
    *field_mask |= mask_value;
    return true;
}

bool parse_optional_query_limit(
    PyObject* spec,
    const char* field_name,
    uint32_t* output,
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr || value == Py_None) {
        return true;
    }
    const unsigned long raw = PyLong_AsUnsignedLong(value);
    if (PyErr_Occurred() != nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        PyErr_Clear();
        return false;
    }
    *output = static_cast<uint32_t>(raw);
    return true;
}

template <typename EnumT>
bool parse_optional_named_enum(
    PyObject* spec,
    const char* field_name,
    EnumT* output,
    uint32_t mask_value,
    uint32_t* field_mask,
    std::optional<EnumT> (*parser)(std::string_view),
    std::string* error_message
) {
    PyObject* value = PyDict_GetItemString(spec, field_name);
    if (value == nullptr) {
        return true;
    }
    if (!PyUnicode_Check(value)) {
        if (error_message != nullptr) {
            *error_message = std::string(field_name) + " must be a string";
        }
        return false;
    }
    const char* utf8 = PyUnicode_AsUTF8(value);
    const auto parsed = parser(utf8 == nullptr ? std::string_view{} : std::string_view(utf8));
    if (!parsed.has_value()) {
        if (error_message != nullptr) {
            *error_message = std::string("unsupported ") + field_name + " value";
        }
        return false;
    }
    *output = *parsed;
    *field_mask |= mask_value;
    return true;
}

bool parse_intelligence_record_kind(
    std::string_view name,
    IntelligenceRecordKind* kind,
    std::string* error_message
) {
    if (name == "all") {
        *kind = IntelligenceRecordKind::All;
        return true;
    }
    if (name == "task" || name == "tasks") {
        *kind = IntelligenceRecordKind::Tasks;
        return true;
    }
    if (name == "claim" || name == "claims") {
        *kind = IntelligenceRecordKind::Claims;
        return true;
    }
    if (name == "evidence") {
        *kind = IntelligenceRecordKind::Evidence;
        return true;
    }
    if (name == "decision" || name == "decisions") {
        *kind = IntelligenceRecordKind::Decisions;
        return true;
    }
    if (name == "memory" || name == "memories") {
        *kind = IntelligenceRecordKind::Memories;
        return true;
    }
    if (error_message != nullptr) {
        *error_message = "kind must be one of all, tasks, claims, evidence, decisions, memories";
    }
    return false;
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

struct StreamIterator {
    PyObject_HEAD
    PyObject* owner_ref{nullptr};
    const GraphHandle* handle{nullptr};
    std::vector<TraceEvent> events;
    std::size_t index{0U};
    bool include_subgraphs{true};
};

void StreamIterator_dealloc(StreamIterator* self) {
    self->events.~vector<TraceEvent>();
    Py_XDECREF(self->owner_ref);
    Py_TYPE(self)->tp_free(reinterpret_cast<PyObject*>(self));
}

PyObject* StreamIterator_iter(PyObject* self) {
    Py_INCREF(self);
    return self;
}

PyObject* StreamIterator_iternext(StreamIterator* self) {
    while (self->index < self->events.size()) {
        PyObject* result = nullptr;
        std::string error_message;
        if (!self->handle->build_trace_event_dict(
                self->events[self->index++],
                self->include_subgraphs,
                &result,
                &error_message
            )) {
            PyErr_SetString(
                PyExc_RuntimeError,
                error_message.empty() ? "failed to build stream event" : error_message.c_str()
            );
            return nullptr;
        }
        if (result != nullptr) {
            return result;
        }
    }

    PyErr_SetNone(PyExc_StopIteration);
    return nullptr;
}

static PyTypeObject StreamIteratorType = {
    PyVarObject_HEAD_INIT(nullptr, 0)
    "agentcore.StreamIterator",            /* tp_name */
    sizeof(StreamIterator),                /* tp_basicsize */
    0,                                     /* tp_itemsize */
    (destructor)StreamIterator_dealloc,    /* tp_dealloc */
    0,                                     /* tp_print */
    0,                                     /* tp_getattr */
    0,                                     /* tp_setattr */
    0,                                     /* tp_reserved */
    0,                                     /* tp_repr */
    0,                                     /* tp_as_number */
    0,                                     /* tp_as_sequence */
    0,                                     /* tp_as_mapping */
    0,                                     /* tp_hash */
    0,                                     /* tp_call */
    0,                                     /* tp_str */
    0,                                     /* tp_getattro */
    0,                                     /* tp_setattro */
    0,                                     /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,                    /* tp_flags */
    "Stream Iterator",                     /* tp_doc */
    0,                                     /* tp_traverse */
    0,                                     /* tp_clear */
    0,                                     /* tp_richcompare */
    0,                                     /* tp_weaklistoffset */
    StreamIterator_iter,                   /* tp_iter */
    (iternextfunc)StreamIterator_iternext, /* tp_iternext */
    0,                                     /* tp_methods */
    0,                                     /* tp_members */
    0,                                     /* tp_getset */
    0,                                     /* tp_base */
    0,                                     /* tp_dict */
    0,                                     /* tp_descr_get */
    0,                                     /* tp_descr_set */
    0,                                     /* tp_dictoffset */
    0,                                     /* tp_init */
    0,                                     /* tp_alloc */
    0,                                     /* tp_new */
    0,                                     /* tp_free */
};

PyObject* create_stream_iterator(
    PyObject* owner_ref,
    const GraphHandle* handle,
    std::vector<TraceEvent> events,
    bool include_subgraphs,
    std::string* error_message
) {
    if (PyType_Ready(&StreamIteratorType) < 0) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    StreamIterator* self = PyObject_New(StreamIterator, &StreamIteratorType);
    if (self == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    self->owner_ref = owner_ref;
    Py_XINCREF(self->owner_ref);
    self->handle = handle;
    new (&self->events) std::vector<TraceEvent>(std::move(events));
    self->index = 0U;
    self->include_subgraphs = include_subgraphs;
    return reinterpret_cast<PyObject*>(self);
}

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

PyObject* runtime_identity(PyObject* capsule, std::string* error_message) {
    return runtime_identity_impl(capsule, error_message);
}

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

BlobRef python_object_to_blob_payload(
    PyObject* value,
    BlobStore& blobs,
    std::string* error_message
) {
    if (value == nullptr || value == Py_None) {
        return {};
    }
    if (PyBytes_Check(value)) {
        char* buffer = nullptr;
        Py_ssize_t size = 0;
        if (PyBytes_AsStringAndSize(value, &buffer, &size) != 0) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return {};
        }
        return append_tagged_blob(
            blobs,
            kBytesBlobTag,
            reinterpret_cast<const std::byte*>(buffer),
            static_cast<std::size_t>(size)
        );
    }

    std::vector<std::byte> pickle_bytes;
    if (!dump_object_as_pickle_bytes(value, &pickle_bytes, error_message)) {
        return {};
    }
    return append_tagged_blob(blobs, kPickleBlobTag, pickle_bytes.data(), pickle_bytes.size());
}

PyObject* intelligence_blob_to_python(
    const GraphHandle& handle,
    BlobRef ref,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    if (ref.empty()) {
        Py_RETURN_NONE;
    }
    return handle.convert_value_to_python(Value{ref}, blobs, strings, error_message);
}

PyObject* intelligence_snapshot_to_python(
    const GraphHandle& handle,
    const IntelligenceSnapshot& snapshot,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    const auto append_common_key = [&](PyObject* target, const char* field_name, InternedStringId key) -> bool {
        PyObject* value = nullptr;
        if (key == 0U) {
            Py_INCREF(Py_None);
            value = Py_None;
        } else {
            value = unicode_from_utf8(strings.resolve(key));
        }
        return set_python_dict_item(target, field_name, value, error_message);
    };

    const auto build_tasks = [&]() -> PyObject* {
        PyObject* list = PyList_New(0);
        if (list == nullptr) {
            return nullptr;
        }
        for (const IntelligenceTask& task : snapshot.tasks) {
            PyObject* item = PyDict_New();
            if (item == nullptr ||
                !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(task.id), error_message) ||
                !append_common_key(item, "key", task.key) ||
                !append_common_key(item, "title", task.title) ||
                !append_common_key(item, "owner", task.owner) ||
                !set_python_dict_item(
                    item,
                    "status",
                    unicode_from_utf8(intelligence_task_status_name(task.status)),
                    error_message
                ) ||
                !set_python_dict_item(item, "priority", PyLong_FromLong(task.priority), error_message) ||
                !set_python_dict_item(item, "confidence", PyFloat_FromDouble(task.confidence), error_message) ||
                !set_python_dict_item(
                    item,
                    "payload",
                    intelligence_blob_to_python(handle, task.payload, blobs, strings, error_message),
                    error_message
                ) ||
                !set_python_dict_item(
                    item,
                    "result",
                    intelligence_blob_to_python(handle, task.result, blobs, strings, error_message),
                    error_message
                )) {
                Py_XDECREF(item);
                Py_DECREF(list);
                return nullptr;
            }
            if (PyList_Append(list, item) != 0) {
                Py_DECREF(item);
                Py_DECREF(list);
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return nullptr;
            }
            Py_DECREF(item);
        }
        return list;
    };

    const auto build_claims = [&]() -> PyObject* {
        PyObject* list = PyList_New(0);
        if (list == nullptr) {
            return nullptr;
        }
        for (const IntelligenceClaim& claim : snapshot.claims) {
            PyObject* item = PyDict_New();
            if (item == nullptr ||
                !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(claim.id), error_message) ||
                !append_common_key(item, "key", claim.key) ||
                !append_common_key(item, "subject", claim.subject_label) ||
                !append_common_key(item, "relation", claim.relation) ||
                !append_common_key(item, "object", claim.object_label) ||
                !set_python_dict_item(
                    item,
                    "status",
                    unicode_from_utf8(intelligence_claim_status_name(claim.status)),
                    error_message
                ) ||
                !set_python_dict_item(item, "confidence", PyFloat_FromDouble(claim.confidence), error_message) ||
                !set_python_dict_item(
                    item,
                    "statement",
                    intelligence_blob_to_python(handle, claim.statement, blobs, strings, error_message),
                    error_message
                )) {
                Py_XDECREF(item);
                Py_DECREF(list);
                return nullptr;
            }
            if (PyList_Append(list, item) != 0) {
                Py_DECREF(item);
                Py_DECREF(list);
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return nullptr;
            }
            Py_DECREF(item);
        }
        return list;
    };

    const auto build_evidence = [&]() -> PyObject* {
        PyObject* list = PyList_New(0);
        if (list == nullptr) {
            return nullptr;
        }
        for (const IntelligenceEvidence& evidence : snapshot.evidence) {
            PyObject* item = PyDict_New();
            if (item == nullptr ||
                !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(evidence.id), error_message) ||
                !append_common_key(item, "key", evidence.key) ||
                !append_common_key(item, "kind", evidence.kind) ||
                !append_common_key(item, "source", evidence.source) ||
                !append_common_key(item, "task_key", evidence.task_key) ||
                !append_common_key(item, "claim_key", evidence.claim_key) ||
                !set_python_dict_item(item, "confidence", PyFloat_FromDouble(evidence.confidence), error_message) ||
                !set_python_dict_item(
                    item,
                    "content",
                    intelligence_blob_to_python(handle, evidence.content, blobs, strings, error_message),
                    error_message
                )) {
                Py_XDECREF(item);
                Py_DECREF(list);
                return nullptr;
            }
            if (PyList_Append(list, item) != 0) {
                Py_DECREF(item);
                Py_DECREF(list);
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return nullptr;
            }
            Py_DECREF(item);
        }
        return list;
    };

    const auto build_decisions = [&]() -> PyObject* {
        PyObject* list = PyList_New(0);
        if (list == nullptr) {
            return nullptr;
        }
        for (const IntelligenceDecision& decision : snapshot.decisions) {
            PyObject* item = PyDict_New();
            if (item == nullptr ||
                !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(decision.id), error_message) ||
                !append_common_key(item, "key", decision.key) ||
                !append_common_key(item, "task_key", decision.task_key) ||
                !append_common_key(item, "claim_key", decision.claim_key) ||
                !set_python_dict_item(
                    item,
                    "status",
                    unicode_from_utf8(intelligence_decision_status_name(decision.status)),
                    error_message
                ) ||
                !set_python_dict_item(item, "confidence", PyFloat_FromDouble(decision.confidence), error_message) ||
                !set_python_dict_item(
                    item,
                    "summary",
                    intelligence_blob_to_python(handle, decision.summary, blobs, strings, error_message),
                    error_message
                )) {
                Py_XDECREF(item);
                Py_DECREF(list);
                return nullptr;
            }
            if (PyList_Append(list, item) != 0) {
                Py_DECREF(item);
                Py_DECREF(list);
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return nullptr;
            }
            Py_DECREF(item);
        }
        return list;
    };

    const auto build_memories = [&]() -> PyObject* {
        PyObject* list = PyList_New(0);
        if (list == nullptr) {
            return nullptr;
        }
        for (const IntelligenceMemoryEntry& memory : snapshot.memories) {
            PyObject* item = PyDict_New();
            if (item == nullptr ||
                !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(memory.id), error_message) ||
                !append_common_key(item, "key", memory.key) ||
                !append_common_key(item, "scope", memory.scope) ||
                !append_common_key(item, "task_key", memory.task_key) ||
                !append_common_key(item, "claim_key", memory.claim_key) ||
                !set_python_dict_item(
                    item,
                    "layer",
                    unicode_from_utf8(intelligence_memory_layer_name(memory.layer)),
                    error_message
                ) ||
                !set_python_dict_item(item, "importance", PyFloat_FromDouble(memory.importance), error_message) ||
                !set_python_dict_item(
                    item,
                    "content",
                    intelligence_blob_to_python(handle, memory.content, blobs, strings, error_message),
                    error_message
                )) {
                Py_XDECREF(item);
                Py_DECREF(list);
                return nullptr;
            }
            if (PyList_Append(list, item) != 0) {
                Py_DECREF(item);
                Py_DECREF(list);
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return nullptr;
            }
            Py_DECREF(item);
        }
        return list;
    };

    PyObject* counts = PyDict_New();
    if (counts == nullptr ||
        !set_python_dict_item(counts, "tasks", PyLong_FromUnsignedLong(snapshot.tasks.size()), error_message) ||
        !set_python_dict_item(counts, "claims", PyLong_FromUnsignedLong(snapshot.claims.size()), error_message) ||
        !set_python_dict_item(counts, "evidence", PyLong_FromUnsignedLong(snapshot.evidence.size()), error_message) ||
        !set_python_dict_item(counts, "decisions", PyLong_FromUnsignedLong(snapshot.decisions.size()), error_message) ||
        !set_python_dict_item(counts, "memories", PyLong_FromUnsignedLong(snapshot.memories.size()), error_message)) {
        Py_XDECREF(counts);
        Py_DECREF(dict);
        return nullptr;
    }
    if (!set_python_dict_item(dict, "counts", counts, error_message) ||
        !set_python_dict_item(dict, "tasks", build_tasks(), error_message) ||
        !set_python_dict_item(dict, "claims", build_claims(), error_message) ||
        !set_python_dict_item(dict, "evidence", build_evidence(), error_message) ||
        !set_python_dict_item(dict, "decisions", build_decisions(), error_message) ||
        !set_python_dict_item(dict, "memories", build_memories(), error_message)) {
        Py_DECREF(dict);
        return nullptr;
    }

    return dict;
}

PyObject* intelligence_summary_to_python(
    const IntelligenceOperationalSummary& summary,
    std::string* error_message
) {
    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    PyObject* counts = PyDict_New();
    PyObject* task_status = PyDict_New();
    PyObject* claim_status = PyDict_New();
    PyObject* decision_status = PyDict_New();
    PyObject* memory_layers = PyDict_New();
    if (counts == nullptr ||
        task_status == nullptr ||
        claim_status == nullptr ||
        decision_status == nullptr ||
        memory_layers == nullptr ||
        !set_python_dict_item(counts, "tasks", PyLong_FromUnsignedLong(summary.task_count), error_message) ||
        !set_python_dict_item(counts, "claims", PyLong_FromUnsignedLong(summary.claim_count), error_message) ||
        !set_python_dict_item(counts, "evidence", PyLong_FromUnsignedLong(summary.evidence_count), error_message) ||
        !set_python_dict_item(counts, "decisions", PyLong_FromUnsignedLong(summary.decision_count), error_message) ||
        !set_python_dict_item(counts, "memories", PyLong_FromUnsignedLong(summary.memory_count), error_message) ||
        !set_python_dict_item(task_status, "open", PyLong_FromUnsignedLong(summary.open_task_count), error_message) ||
        !set_python_dict_item(task_status, "in_progress", PyLong_FromUnsignedLong(summary.in_progress_task_count), error_message) ||
        !set_python_dict_item(task_status, "blocked", PyLong_FromUnsignedLong(summary.blocked_task_count), error_message) ||
        !set_python_dict_item(task_status, "completed", PyLong_FromUnsignedLong(summary.completed_task_count), error_message) ||
        !set_python_dict_item(task_status, "cancelled", PyLong_FromUnsignedLong(summary.cancelled_task_count), error_message) ||
        !set_python_dict_item(claim_status, "proposed", PyLong_FromUnsignedLong(summary.proposed_claim_count), error_message) ||
        !set_python_dict_item(claim_status, "supported", PyLong_FromUnsignedLong(summary.supported_claim_count), error_message) ||
        !set_python_dict_item(claim_status, "disputed", PyLong_FromUnsignedLong(summary.disputed_claim_count), error_message) ||
        !set_python_dict_item(claim_status, "confirmed", PyLong_FromUnsignedLong(summary.confirmed_claim_count), error_message) ||
        !set_python_dict_item(claim_status, "rejected", PyLong_FromUnsignedLong(summary.rejected_claim_count), error_message) ||
        !set_python_dict_item(decision_status, "pending", PyLong_FromUnsignedLong(summary.pending_decision_count), error_message) ||
        !set_python_dict_item(decision_status, "selected", PyLong_FromUnsignedLong(summary.selected_decision_count), error_message) ||
        !set_python_dict_item(decision_status, "superseded", PyLong_FromUnsignedLong(summary.superseded_decision_count), error_message) ||
        !set_python_dict_item(decision_status, "rejected", PyLong_FromUnsignedLong(summary.rejected_decision_count), error_message) ||
        !set_python_dict_item(memory_layers, "working", PyLong_FromUnsignedLong(summary.working_memory_count), error_message) ||
        !set_python_dict_item(memory_layers, "episodic", PyLong_FromUnsignedLong(summary.episodic_memory_count), error_message) ||
        !set_python_dict_item(memory_layers, "semantic", PyLong_FromUnsignedLong(summary.semantic_memory_count), error_message) ||
        !set_python_dict_item(memory_layers, "procedural", PyLong_FromUnsignedLong(summary.procedural_memory_count), error_message)) {
        Py_XDECREF(counts);
        Py_XDECREF(task_status);
        Py_XDECREF(claim_status);
        Py_XDECREF(decision_status);
        Py_XDECREF(memory_layers);
        Py_DECREF(dict);
        return nullptr;
    }
    if (!set_python_dict_item(dict, "counts", counts, error_message)) {
        Py_XDECREF(task_status);
        Py_XDECREF(claim_status);
        Py_XDECREF(decision_status);
        Py_XDECREF(memory_layers);
        Py_DECREF(dict);
        return nullptr;
    }
    counts = nullptr;
    if (!set_python_dict_item(dict, "task_status", task_status, error_message)) {
        Py_XDECREF(claim_status);
        Py_XDECREF(decision_status);
        Py_XDECREF(memory_layers);
        Py_DECREF(dict);
        return nullptr;
    }
    task_status = nullptr;
    if (!set_python_dict_item(dict, "claim_status", claim_status, error_message)) {
        Py_XDECREF(decision_status);
        Py_XDECREF(memory_layers);
        Py_DECREF(dict);
        return nullptr;
    }
    claim_status = nullptr;
    if (!set_python_dict_item(dict, "decision_status", decision_status, error_message)) {
        Py_XDECREF(memory_layers);
        Py_DECREF(dict);
        return nullptr;
    }
    decision_status = nullptr;
    if (!set_python_dict_item(dict, "memory_layers", memory_layers, error_message)) {
        Py_DECREF(dict);
        return nullptr;
    }
    memory_layers = nullptr;

    return dict;
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
        if (tagged_buffer_has_payload(buffer.first, buffer.second, kSequenceBlobTag)) {
            return load_sequence_blob_to_python_list(buffer.first, buffer.second, error_message);
        }
        if (tagged_buffer_has_payload(buffer.first, buffer.second, kMessageBlobTag)) {
            return load_message_blob_to_python_list(buffer.first, buffer.second, error_message);
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

bool GraphHandle::convert_python_value(
    PyObject* value,
    BlobStore& blobs,
    StringInterner& strings,
    Value* output,
    std::string* error_message,
    bool encode_as_messages
) {
    if (value == Py_None) {
        *output = std::monostate{};
        return true;
    }
    if (encode_as_messages) {
        BlobRef ref;
        if (!dump_python_messages_as_blob(value, blobs, &ref, error_message)) {
            return false;
        }
        *output = ref;
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
    if (PyList_Check(value)) {
        BlobRef ref;
        if (!dump_python_sequence_as_blob(value, blobs, &ref, error_message)) {
            return false;
        }
        *output = ref;
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

bool GraphHandle::add_node(
    std::string_view name,
    PyObject* callback,
    NodeKind kind,
    uint32_t policy_flags,
    const NodeMemoizationPolicy& memoization,
    const std::vector<IntelligenceSubscription>& intelligence_subscriptions,
    const std::vector<std::pair<std::string, JoinMergeStrategy>>& merge_rules,
    std::string* error_message
) {
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
        const StateKey state_key = ensure_state_key_locked(field);
        if (strategy == JoinMergeStrategy::MergeMessages) {
            message_state_keys_.insert(state_key);
        }
        rules.push_back({state_key, strategy});
    }

    node_bindings_.push_back({
        node_id,
        std::string(name),
        callback,
        kind,
        policy_flags,
        std::move(rules),
        memoization,
        intelligence_subscriptions
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
            binding.memoization,
            binding.intelligence_subscriptions
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

bool GraphHandle::execute_run_core(
    PyObject* input_state,
    PyObject* config,
    bool until_pause,
    const RunCaptureOptions& capture_options,
    RunId* run_id,
    RunResult* result,
    std::string* error_message
) {
    if (!ensure_finalized(error_message)) return false;
    if (!register_graph_hierarchy(*engine_, error_message)) return false;

    InputEnvelope envelope;
    if (!build_initial_envelope(input_state, config, &envelope, error_message)) return false;

    const RunId started_run_id = engine_->start(graph_, envelope, capture_options);
    const auto cleanup_active_config = [&]() {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_configs_.find(started_run_id);
        if (it != active_configs_.end()) {
            Py_XDECREF(it->second);
            active_configs_.erase(it);
        }
    };

    if (config && config != Py_None) {
        std::lock_guard<std::mutex> lock(mutex_);
        Py_INCREF(config);
        active_configs_[started_run_id] = config;
    }

    RunResult local_result;
    if (until_pause) {
        Py_BEGIN_ALLOW_THREADS
        while (true) {
            const StepResult step_result = engine_->step(started_run_id);
            local_result.run_id = started_run_id;
            local_result.status = step_result.status;
            local_result.last_checkpoint_id = step_result.checkpoint_id;
            if (step_result.progressed) {
                local_result.steps_executed += 1U;
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
        local_result = engine_->run_to_completion(started_run_id);
        Py_END_ALLOW_THREADS
    }

    cleanup_active_config();
    *run_id = started_run_id;
    *result = local_result;
    return true;
}

bool GraphHandle::execute_run(PyObject* input_state, PyObject* config, bool include_subgraphs, bool until_pause, RunArtifacts* artifacts, std::string* error_message) {
    RunId run_id = 0U;
    RunResult result;
    if (!execute_run_core(
            input_state,
            config,
            until_pause,
            RunCaptureOptions{},
            &run_id,
            &result,
            error_message
        )) {
        return false;
    }

    return populate_run_artifacts(run_id, result, include_subgraphs, artifacts, error_message);
}

bool GraphHandle::invoke(PyObject* input_state, PyObject* config, PyObject** result, std::string* error_message) {
    RunId run_id = 0U;
    RunResult run_result;
    if (!execute_run_core(
            input_state,
            config,
            false,
            RunCaptureOptions{false, false},
            &run_id,
            &run_result,
            error_message
        )) {
        return false;
    }

    static_cast<void>(run_result);
    const auto& store = engine_->state_store(run_id);
    *result = state_to_python_dict(
        engine_->state(run_id),
        store.blobs(),
        store.strings(),
        error_message
    );
    engine_->discard_run(run_id);
    return *result != nullptr;
}

bool GraphHandle::stream(
    PyObject* input_state,
    PyObject* config,
    bool include_subgraphs,
    PyObject* owner_ref,
    PyObject** result,
    std::string* error_message
) {
    RunId run_id = 0U;
    RunResult run_result;
    if (!execute_run_core(
            input_state,
            config,
            false,
            RunCaptureOptions{false, true},
            &run_id,
            &run_result,
            error_message
        )) {
        return false;
    }

    static_cast<void>(run_result);
    std::vector<TraceEvent> events = engine_->trace().take_events_for_run(run_id);
    engine_->discard_run(run_id);
    *result = create_stream_iterator(
        owner_ref,
        this,
        std::move(events),
        include_subgraphs,
        error_message
    );
    return *result != nullptr;
}

bool GraphHandle::invoke_with_details(PyObject* input_state, PyObject* config, bool include_subgraphs, PyObject** result, std::string* error_message) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, false, &artifacts, error_message)) return false;
    *result = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    Py_XDECREF(artifacts.intelligence);
    return *result != nullptr;
}

bool GraphHandle::invoke_until_pause_with_details(PyObject* input_state, PyObject* config, bool include_subgraphs, PyObject** result, std::string* error_message) {
    RunArtifacts artifacts;
    if (!execute_run(input_state, config, include_subgraphs, true, &artifacts, error_message)) return false;
    *result = build_details_dict(artifacts, error_message);
    Py_XDECREF(artifacts.state);
    Py_XDECREF(artifacts.trace);
    Py_XDECREF(artifacts.intelligence);
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
    Py_XDECREF(artifacts.intelligence);
    return *result != nullptr;
}

bool GraphHandle::populate_run_artifacts(RunId run_id, const RunResult& run_result, bool include_subgraphs, RunArtifacts* artifacts, std::string* error_message) {
    const auto& store = engine_->state_store(run_id);
    artifacts->state = state_to_python_dict(engine_->state(run_id), store.blobs(), store.strings(), error_message);
    artifacts->trace = build_trace_list(run_id, include_subgraphs, error_message);
    artifacts->intelligence = intelligence_snapshot_to_python(
        *this,
        store.intelligence().snapshot(),
        store.blobs(),
        store.strings(),
        error_message
    );
    artifacts->run_id = run_id;
    artifacts->result = run_result;
    // Compute proof digest if possible
    try {
        artifacts->proof = compute_run_proof_digest(engine_->inspect(run_id), engine_->trace().events_for_run(run_id));
    } catch (...) {
        artifacts->proof = {};
    }
    return artifacts->state && artifacts->trace && artifacts->intelligence;
}

bool GraphHandle::build_trace_event_dict(
    const TraceEvent& event,
    bool include_subgraphs,
    PyObject** result,
    std::string* error_message
) const {
    *result = nullptr;
    if (!include_subgraphs && !event.namespace_path.empty()) {
        return true;
    }

    const auto resolve_graph_handle = [this](GraphId graph_id) -> const GraphHandle* {
        if (graph_id == graph_.id) {
            return this;
        }
        return GraphHandleRegistry::instance().find(graph_id);
    };

    const auto resolve_graph = [this, &resolve_graph_handle](GraphId graph_id) -> const GraphDefinition& {
        const GraphHandle* handle = resolve_graph_handle(graph_id);
        return handle == nullptr ? graph_ : handle->graph_;
    };

    const NodeDefinition* node = resolve_graph(event.graph_id).find_node(event.node_id);
    if (node == nullptr) {
        return true;
    }

    const std::string name = node->name;
    if (name == kInternalBootstrapNodeName || name == kInternalEndNodeName) {
        return true;
    }

    const std::string graph_name = resolve_graph(event.graph_id).name;

    PyObject* dict = PyDict_New();
    if (dict == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return false;
    }

    PyObject* namespaces = PyList_New(static_cast<Py_ssize_t>(event.namespace_path.size()));
    if (namespaces == nullptr) {
        Py_DECREF(dict);
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return false;
    }

    Py_ssize_t namespace_index = 0;
    for (const ExecutionNamespaceRef& frame : event.namespace_path) {
        const GraphDefinition& namespace_graph = resolve_graph(frame.graph_id);
        const NodeDefinition* namespace_node = namespace_graph.find_node(frame.node_id);
        std::string namespace_node_name;
        if (namespace_node != nullptr) {
            if (namespace_node->kind == NodeKind::Subgraph &&
                namespace_node->subgraph.has_value() &&
                !namespace_node->subgraph->namespace_name.empty()) {
                namespace_node_name = namespace_node->subgraph->namespace_name;
            } else {
                namespace_node_name = namespace_node->name;
            }
        }

        PyObject* frame_dict = PyDict_New();
        if (frame_dict == nullptr ||
            !set_python_dict_item(
                frame_dict,
                "graph_name",
                unicode_from_utf8(namespace_graph.name),
                error_message
            ) ||
            !set_python_dict_item(
                frame_dict,
                "node_name",
                unicode_from_utf8(namespace_node_name),
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
            return false;
        }
        PyList_SET_ITEM(namespaces, namespace_index++, frame_dict);
    }

    const uint64_t duration_ns = event.ts_end_ns >= event.ts_start_ns
        ? (event.ts_end_ns - event.ts_start_ns)
        : 0U;

    if (!set_python_dict_item(dict, "sequence", PyLong_FromUnsignedLongLong(event.sequence), error_message) ||
        !set_python_dict_item(dict, "run_id", PyLong_FromUnsignedLongLong(event.run_id), error_message) ||
        !set_python_dict_item(dict, "graph_id", PyLong_FromUnsignedLongLong(event.graph_id), error_message) ||
        !set_python_dict_item(dict, "node_id", PyLong_FromUnsignedLongLong(event.node_id), error_message) ||
        !set_python_dict_item(dict, "branch_id", PyLong_FromUnsignedLong(event.branch_id), error_message) ||
        !set_python_dict_item(
            dict,
            "checkpoint_id",
            PyLong_FromUnsignedLongLong(event.checkpoint_id),
            error_message
        ) ||
        !set_python_dict_item(dict, "node_name", unicode_from_utf8(name), error_message) ||
        !set_python_dict_item(
            dict,
            "node_kind",
            unicode_from_utf8(node_kind_name(node->kind)),
            error_message
        ) ||
        !set_python_dict_item(dict, "graph_name", unicode_from_utf8(graph_name), error_message) ||
        !set_python_dict_item(
            dict,
            "result",
            unicode_from_utf8(node_result_status_name(event.result)),
            error_message
        ) ||
        !set_python_dict_item(dict, "ts_start_ns", PyLong_FromUnsignedLongLong(event.ts_start_ns), error_message) ||
        !set_python_dict_item(dict, "ts_end_ns", PyLong_FromUnsignedLongLong(event.ts_end_ns), error_message) ||
        !set_python_dict_item(dict, "duration_ns", PyLong_FromUnsignedLongLong(duration_ns), error_message) ||
        !set_python_dict_item(dict, "confidence", PyFloat_FromDouble(event.confidence), error_message) ||
        !set_python_dict_item(dict, "patch_count", PyLong_FromUnsignedLong(event.patch_count), error_message) ||
        !set_python_dict_item(dict, "flags", PyLong_FromUnsignedLong(event.flags), error_message) ||
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
        return false;
    }

    *result = dict;
    return true;
}

PyObject* GraphHandle::build_trace_list(RunId run_id, bool include_subgraphs, std::string* error_message) const {
    const std::vector<TraceEvent> events = engine_->trace().events_for_run_since_sequence(run_id, 1U);

    std::size_t visible_event_count = 0U;
    for (const TraceEvent& event : events) {
        PyObject* event_dict = nullptr;
        if (!build_trace_event_dict(event, include_subgraphs, &event_dict, error_message)) {
            return nullptr;
        }
        if (event_dict != nullptr) {
            ++visible_event_count;
            Py_DECREF(event_dict);
        }
    }

    PyObject* list = PyList_New(static_cast<Py_ssize_t>(visible_event_count));
    if (list == nullptr) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    Py_ssize_t event_index = 0;
    for (const TraceEvent& event : events) {
        PyObject* event_dict = nullptr;
        if (!build_trace_event_dict(event, include_subgraphs, &event_dict, error_message)) {
            Py_DECREF(list);
            return nullptr;
        }
        if (event_dict == nullptr) {
            continue;
        }
        PyList_SET_ITEM(list, event_index++, event_dict);
    }
    return list;
}

PyObject* GraphHandle::build_details_dict(const RunArtifacts& artifacts, std::string* error_message) const {
    PyObject* dict = PyDict_New();
    PyDict_SetItemString(dict, "state", artifacts.state);
    PyDict_SetItemString(dict, "trace", artifacts.trace);
    PyDict_SetItemString(dict, "intelligence", artifacts.intelligence);

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

        bool encode_as_messages = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            encode_as_messages = message_state_keys_.find(state_key) != message_state_keys_.end();
        }

        Value val;
        if (!convert_python_value(
                value,
                blobs,
                strings,
                &val,
                error_message,
                encode_as_messages
            )) {
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

    const KnowledgeGraphPatch pending_knowledge_graph_patch = runtime_view->pending_knowledge_graph_patch;
    const IntelligencePatch pending_intelligence_patch = runtime_view->pending_intelligence_patch;
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

    if (!pending_knowledge_graph_patch.empty()) {
        node_result.patch.knowledge_graph.entities.insert(
            node_result.patch.knowledge_graph.entities.end(),
            pending_knowledge_graph_patch.entities.begin(),
            pending_knowledge_graph_patch.entities.end()
        );
        node_result.patch.knowledge_graph.triples.insert(
            node_result.patch.knowledge_graph.triples.end(),
            pending_knowledge_graph_patch.triples.begin(),
            pending_knowledge_graph_patch.triples.end()
        );
    }

    if (!pending_intelligence_patch.empty()) {
        node_result.patch.intelligence.tasks.insert(
            node_result.patch.intelligence.tasks.end(),
            pending_intelligence_patch.tasks.begin(),
            pending_intelligence_patch.tasks.end()
        );
        node_result.patch.intelligence.claims.insert(
            node_result.patch.intelligence.claims.end(),
            pending_intelligence_patch.claims.begin(),
            pending_intelligence_patch.claims.end()
        );
        node_result.patch.intelligence.evidence.insert(
            node_result.patch.intelligence.evidence.end(),
            pending_intelligence_patch.evidence.begin(),
            pending_intelligence_patch.evidence.end()
        );
        node_result.patch.intelligence.decisions.insert(
            node_result.patch.intelligence.decisions.end(),
            pending_intelligence_patch.decisions.begin(),
            pending_intelligence_patch.decisions.end()
        );
        node_result.patch.intelligence.memories.insert(
            node_result.patch.intelligence.memories.end(),
            pending_intelligence_patch.memories.begin(),
            pending_intelligence_patch.memories.end()
        );
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

bool runtime_stage_knowledge_entity(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return false;
    }

    KnowledgeEntityWrite write;
    uint32_t ignored_mask = 0U;
    if (!parse_required_spec_string_id(spec, "label", view->runtime->strings, &write.label, error_message) ||
        !parse_optional_spec_blob(
            spec,
            "payload",
            view->runtime->blobs,
            &write.payload,
            0U,
            &ignored_mask,
            error_message
        )) {
        return false;
    }

    view->pending_knowledge_graph_patch.entities.push_back(write);
    return true;
}

bool runtime_stage_knowledge_triple(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return false;
    }

    KnowledgeTripleWrite write;
    uint32_t ignored_mask = 0U;
    if (!parse_required_spec_string_id(spec, "subject", view->runtime->strings, &write.subject_label, error_message) ||
        !parse_required_spec_string_id(spec, "relation", view->runtime->strings, &write.relation, error_message) ||
        !parse_required_spec_string_id(spec, "object", view->runtime->strings, &write.object_label, error_message) ||
        !parse_optional_spec_blob(
            spec,
            "payload",
            view->runtime->blobs,
            &write.payload,
            0U,
            &ignored_mask,
            error_message
        )) {
        return false;
    }

    view->pending_knowledge_graph_patch.triples.push_back(write);
    return true;
}

namespace {

struct KnowledgeOverlayKey {
    InternedStringId subject{0};
    InternedStringId relation{0};
    InternedStringId object{0};

    bool operator==(const KnowledgeOverlayKey& other) const noexcept {
        return subject == other.subject &&
               relation == other.relation &&
               object == other.object;
    }
};

struct KnowledgeOverlayKeyHash {
    std::size_t operator()(const KnowledgeOverlayKey& key) const noexcept {
        std::size_t hash = static_cast<std::size_t>(key.subject);
        hash = (hash * 1315423911U) ^ static_cast<std::size_t>(key.relation);
        hash = (hash * 2654435761U) ^ static_cast<std::size_t>(key.object);
        return hash;
    }
};

struct KnowledgeOverlayPendingTriple {
    KnowledgeOverlayKey key;
    KnowledgeTripleId id{0};
    BlobRef payload{};
    bool exists_in_base{false};
};

struct KnowledgeOverlayTripleResult {
    KnowledgeTripleId id{0};
    InternedStringId subject{0};
    InternedStringId relation{0};
    InternedStringId object{0};
    BlobRef payload{};
};

struct ScoredKnowledgePathTriple {
    RuntimeKnowledgePathCacheTriple triple;
    int64_t score{0};
    uint8_t depth{0};
    uint64_t order{0};
};

using KnowledgeOverlayPendingIndex =
    std::unordered_map<KnowledgeOverlayKey, std::size_t, KnowledgeOverlayKeyHash>;
using KnowledgeOverlaySeenSet =
    std::unordered_set<KnowledgeOverlayKey, KnowledgeOverlayKeyHash>;

[[nodiscard]] KnowledgeOverlayKey knowledge_overlay_key_from_triple(
    const KnowledgeGraphStore& graph,
    const KnowledgeTriple& triple
) {
    const KnowledgeEntity* subject = graph.find_entity(triple.subject);
    const KnowledgeEntity* object = graph.find_entity(triple.object);
    return KnowledgeOverlayKey{
        subject == nullptr ? 0U : subject->label,
        triple.relation,
        object == nullptr ? 0U : object->label
    };
}

[[nodiscard]] bool knowledge_overlay_key_matches(
    const KnowledgeOverlayKey& key,
    std::optional<InternedStringId> subject,
    std::optional<InternedStringId> relation,
    std::optional<InternedStringId> object
) noexcept {
    if (subject.has_value() && key.subject != *subject) {
        return false;
    }
    if (relation.has_value() && key.relation != *relation) {
        return false;
    }
    if (object.has_value() && key.object != *object) {
        return false;
    }
    return true;
}

[[nodiscard]] std::vector<KnowledgeOverlayPendingTriple> build_knowledge_overlay_pending_triples(
    const KnowledgeGraphStore& base,
    const KnowledgeGraphPatch& patch,
    KnowledgeOverlayPendingIndex* index
) {
    std::vector<KnowledgeOverlayPendingTriple> pending;
    pending.reserve(patch.triples.size());
    if (index == nullptr) {
        return pending;
    }
    index->clear();
    index->reserve(patch.triples.size());

    KnowledgeTripleId next_staged_id = static_cast<KnowledgeTripleId>(base.triple_count() + 1U);
    for (const KnowledgeTripleWrite& write : patch.triples) {
        const KnowledgeOverlayKey key{write.subject_label, write.relation, write.object_label};
        const auto existing_pending = index->find(key);
        if (existing_pending != index->end()) {
            KnowledgeOverlayPendingTriple& staged = pending[existing_pending->second];
            if (!write.payload.empty()) {
                staged.payload = write.payload;
            }
            continue;
        }

        const KnowledgeTriple* base_triple =
            base.find_triple_by_labels(write.subject_label, write.relation, write.object_label);
        const bool exists_in_base = base_triple != nullptr;
        const KnowledgeTripleId id = exists_in_base ? base_triple->id : next_staged_id++;
        const std::size_t offset = pending.size();
        pending.push_back(KnowledgeOverlayPendingTriple{key, id, write.payload, exists_in_base});
        index->emplace(key, offset);
    }
    return pending;
}

[[nodiscard]] std::size_t knowledge_overlay_entity_count(
    const KnowledgeGraphStore& base,
    const KnowledgeGraphPatch& patch
) {
    std::unordered_set<InternedStringId> pending_labels;
    pending_labels.reserve(patch.entities.size() + (patch.triples.size() * 2U));

    const auto add_label = [&](InternedStringId label) {
        if (label != 0U && base.find_entity_by_label(label) == nullptr) {
            pending_labels.insert(label);
        }
    };

    for (const KnowledgeEntityWrite& entity : patch.entities) {
        add_label(entity.label);
    }
    for (const KnowledgeTripleWrite& triple : patch.triples) {
        add_label(triple.subject_label);
        add_label(triple.object_label);
    }
    return base.entity_count() + pending_labels.size();
}

[[nodiscard]] std::size_t knowledge_overlay_triple_count(
    const KnowledgeGraphStore& base,
    const std::vector<KnowledgeOverlayPendingTriple>& pending
) noexcept {
    std::size_t staged_new_triples = 0U;
    for (const KnowledgeOverlayPendingTriple& triple : pending) {
        if (!triple.exists_in_base) {
            ++staged_new_triples;
        }
    }
    return base.triple_count() + staged_new_triples;
}

[[nodiscard]] int64_t knowledge_relation_rank(
    const StringInterner& strings,
    InternedStringId relation
) {
    const std::string_view name = strings.resolve(relation);
    const auto contains = [&](std::string_view needle) {
        return name.find(needle) != std::string_view::npos;
    };
    if (contains("evidence") || contains("support") || contains("prove") || contains("cite")) {
        return 320;
    }
    if (contains("cause") || contains("affect") || contains("block") || contains("depend") ||
        contains("require")) {
        return 280;
    }
    if (contains("mitigate") || contains("resolve") || contains("route") || contains("own")) {
        return 240;
    }
    if (contains("feature") || contains("has_") || contains("use") || contains("provide")) {
        return 180;
    }
    return 100;
}

[[nodiscard]] RuntimeKnowledgePathCacheTriple knowledge_path_cache_triple(
    const KnowledgeOverlayTripleResult& triple
) noexcept {
    return RuntimeKnowledgePathCacheTriple{
        triple.id,
        triple.subject,
        triple.relation,
        triple.object,
        triple.payload
    };
}

[[nodiscard]] KnowledgeOverlayTripleResult knowledge_overlay_triple_from_cache(
    const RuntimeKnowledgePathCacheTriple& triple
) noexcept {
    return KnowledgeOverlayTripleResult{
        triple.id,
        triple.subject,
        triple.relation,
        triple.object,
        triple.payload
    };
}

bool set_interned_field(
    PyObject* dict,
    const char* field_name,
    InternedStringId id,
    const StringInterner& strings,
    std::string* error_message
) {
    PyObject* value = nullptr;
    if (id == 0U) {
        Py_INCREF(Py_None);
        value = Py_None;
    } else {
        value = unicode_from_utf8(strings.resolve(id));
    }
    return set_python_dict_item(dict, field_name, value, error_message);
}

PyObject* context_task_to_python(
    const GraphHandle& handle,
    const IntelligenceTask& task,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    PyObject* item = PyDict_New();
    if (item == nullptr ||
        !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(task.id), error_message) ||
        !set_interned_field(item, "key", task.key, strings, error_message) ||
        !set_interned_field(item, "title", task.title, strings, error_message) ||
        !set_interned_field(item, "owner", task.owner, strings, error_message) ||
        !set_python_dict_item(item, "status", unicode_from_utf8(intelligence_task_status_name(task.status)), error_message) ||
        !set_python_dict_item(item, "priority", PyLong_FromLong(task.priority), error_message) ||
        !set_python_dict_item(item, "confidence", PyFloat_FromDouble(task.confidence), error_message) ||
        !set_python_dict_item(item, "payload", intelligence_blob_to_python(handle, task.payload, blobs, strings, error_message), error_message) ||
        !set_python_dict_item(item, "result", intelligence_blob_to_python(handle, task.result, blobs, strings, error_message), error_message)) {
        Py_XDECREF(item);
        return nullptr;
    }
    return item;
}

PyObject* context_claim_to_python(
    const GraphHandle& handle,
    const IntelligenceClaim& claim,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    PyObject* item = PyDict_New();
    if (item == nullptr ||
        !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(claim.id), error_message) ||
        !set_interned_field(item, "key", claim.key, strings, error_message) ||
        !set_interned_field(item, "subject", claim.subject_label, strings, error_message) ||
        !set_interned_field(item, "relation", claim.relation, strings, error_message) ||
        !set_interned_field(item, "object", claim.object_label, strings, error_message) ||
        !set_python_dict_item(item, "status", unicode_from_utf8(intelligence_claim_status_name(claim.status)), error_message) ||
        !set_python_dict_item(item, "confidence", PyFloat_FromDouble(claim.confidence), error_message) ||
        !set_python_dict_item(item, "statement", intelligence_blob_to_python(handle, claim.statement, blobs, strings, error_message), error_message)) {
        Py_XDECREF(item);
        return nullptr;
    }
    return item;
}

PyObject* context_evidence_to_python(
    const GraphHandle& handle,
    const IntelligenceEvidence& evidence,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    PyObject* item = PyDict_New();
    if (item == nullptr ||
        !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(evidence.id), error_message) ||
        !set_interned_field(item, "key", evidence.key, strings, error_message) ||
        !set_interned_field(item, "kind", evidence.kind, strings, error_message) ||
        !set_interned_field(item, "source", evidence.source, strings, error_message) ||
        !set_interned_field(item, "task_key", evidence.task_key, strings, error_message) ||
        !set_interned_field(item, "claim_key", evidence.claim_key, strings, error_message) ||
        !set_python_dict_item(item, "confidence", PyFloat_FromDouble(evidence.confidence), error_message) ||
        !set_python_dict_item(item, "content", intelligence_blob_to_python(handle, evidence.content, blobs, strings, error_message), error_message)) {
        Py_XDECREF(item);
        return nullptr;
    }
    return item;
}

PyObject* context_decision_to_python(
    const GraphHandle& handle,
    const IntelligenceDecision& decision,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    PyObject* item = PyDict_New();
    if (item == nullptr ||
        !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(decision.id), error_message) ||
        !set_interned_field(item, "key", decision.key, strings, error_message) ||
        !set_interned_field(item, "task_key", decision.task_key, strings, error_message) ||
        !set_interned_field(item, "claim_key", decision.claim_key, strings, error_message) ||
        !set_python_dict_item(item, "status", unicode_from_utf8(intelligence_decision_status_name(decision.status)), error_message) ||
        !set_python_dict_item(item, "confidence", PyFloat_FromDouble(decision.confidence), error_message) ||
        !set_python_dict_item(item, "summary", intelligence_blob_to_python(handle, decision.summary, blobs, strings, error_message), error_message)) {
        Py_XDECREF(item);
        return nullptr;
    }
    return item;
}

PyObject* context_memory_to_python(
    const GraphHandle& handle,
    const IntelligenceMemoryEntry& memory,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    PyObject* item = PyDict_New();
    if (item == nullptr ||
        !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(memory.id), error_message) ||
        !set_interned_field(item, "key", memory.key, strings, error_message) ||
        !set_interned_field(item, "scope", memory.scope, strings, error_message) ||
        !set_interned_field(item, "task_key", memory.task_key, strings, error_message) ||
        !set_interned_field(item, "claim_key", memory.claim_key, strings, error_message) ||
        !set_python_dict_item(item, "layer", unicode_from_utf8(intelligence_memory_layer_name(memory.layer)), error_message) ||
        !set_python_dict_item(item, "importance", PyFloat_FromDouble(memory.importance), error_message) ||
        !set_python_dict_item(item, "content", intelligence_blob_to_python(handle, memory.content, blobs, strings, error_message), error_message)) {
        Py_XDECREF(item);
        return nullptr;
    }
    return item;
}

PyObject* context_knowledge_to_python(
    const GraphHandle& handle,
    const KnowledgeGraphStore& knowledge_graph,
    const KnowledgeTriple& triple,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    const KnowledgeEntity* subject = knowledge_graph.find_entity(triple.subject);
    const KnowledgeEntity* object = knowledge_graph.find_entity(triple.object);
    if (subject == nullptr || object == nullptr) {
        if (error_message != nullptr) {
            *error_message = "context graph selected an invalid knowledge triple";
        }
        return nullptr;
    }
    PyObject* item = PyDict_New();
    if (item == nullptr ||
        !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(triple.id), error_message) ||
        !set_interned_field(item, "subject", subject->label, strings, error_message) ||
        !set_interned_field(item, "relation", triple.relation, strings, error_message) ||
        !set_interned_field(item, "object", object->label, strings, error_message) ||
        !set_python_dict_item(item, "payload", handle.convert_value_to_python(Value{triple.payload}, blobs, strings, error_message), error_message) ||
        !set_python_dict_item(item, "source", unicode_from_utf8("native"), error_message)) {
        Py_XDECREF(item);
        return nullptr;
    }
    return item;
}

PyObject* context_record_to_python(
    const ContextGraphRecordRef& ref,
    const GraphHandle& handle,
    const IntelligenceStore& intelligence,
    const KnowledgeGraphStore& knowledge_graph,
    const BlobStore& blobs,
    const StringInterner& strings,
    std::string* error_message
) {
    switch (ref.kind) {
        case ContextRecordKind::Task: {
            const IntelligenceTask* task = intelligence.find_task(ref.id);
            return task == nullptr ? nullptr : context_task_to_python(handle, *task, blobs, strings, error_message);
        }
        case ContextRecordKind::Claim: {
            const IntelligenceClaim* claim = intelligence.find_claim(ref.id);
            return claim == nullptr ? nullptr : context_claim_to_python(handle, *claim, blobs, strings, error_message);
        }
        case ContextRecordKind::Evidence: {
            const IntelligenceEvidence* evidence = intelligence.find_evidence(ref.id);
            return evidence == nullptr ? nullptr : context_evidence_to_python(handle, *evidence, blobs, strings, error_message);
        }
        case ContextRecordKind::Decision: {
            const IntelligenceDecision* decision = intelligence.find_decision(ref.id);
            return decision == nullptr ? nullptr : context_decision_to_python(handle, *decision, blobs, strings, error_message);
        }
        case ContextRecordKind::Memory: {
            const IntelligenceMemoryEntry* memory = intelligence.find_memory(ref.id);
            return memory == nullptr ? nullptr : context_memory_to_python(handle, *memory, blobs, strings, error_message);
        }
        case ContextRecordKind::Knowledge: {
            const KnowledgeTriple* triple = knowledge_graph.find_triple(ref.id);
            return triple == nullptr ? nullptr : context_knowledge_to_python(handle, knowledge_graph, *triple, blobs, strings, error_message);
        }
        case ContextRecordKind::Count: break;
    }
    if (error_message != nullptr && error_message->empty()) {
        *error_message = "context graph selected an unknown record kind";
    }
    return nullptr;
}

bool build_context_query_plan(
    PyObject* spec,
    RuntimeViewProxy* view,
    ContextQueryPlan* plan,
    std::string* error_message
) {
    if (!require_spec_dict(spec, error_message)) {
        return false;
    }

    if (PyObject* include_value = PyDict_GetItemString(spec, "include");
        include_value != nullptr && include_value != Py_None) {
        if (PyUnicode_Check(include_value)) {
            Py_ssize_t size = 0;
            const char* utf8 = PyUnicode_AsUTF8AndSize(include_value, &size);
            if (utf8 == nullptr) {
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return false;
            }
            static_cast<void>(context_query_plan_add_selector(*plan, std::string_view(utf8, static_cast<std::size_t>(size))));
        } else if (PySequence_Check(include_value)) {
            const Py_ssize_t size = PySequence_Size(include_value);
            if (size < 0) {
                if (error_message != nullptr) {
                    *error_message = fetch_python_error();
                }
                return false;
            }
            for (Py_ssize_t index = 0; index < size; ++index) {
                PyObject* item = PySequence_GetItem(include_value, index);
                if (item == nullptr) {
                    if (error_message != nullptr) {
                        *error_message = fetch_python_error();
                    }
                    return false;
                }
                if (!PyUnicode_Check(item)) {
                    Py_DECREF(item);
                    if (error_message != nullptr) {
                        *error_message = "ContextSpec.include entries must be strings";
                    }
                    return false;
                }
                Py_ssize_t text_size = 0;
                const char* utf8 = PyUnicode_AsUTF8AndSize(item, &text_size);
                if (utf8 == nullptr) {
                    Py_DECREF(item);
                    if (error_message != nullptr) {
                        *error_message = fetch_python_error();
                    }
                    return false;
                }
                static_cast<void>(context_query_plan_add_selector(
                    *plan,
                    std::string_view(utf8, static_cast<std::size_t>(text_size))
                ));
                Py_DECREF(item);
            }
        } else {
            if (error_message != nullptr) {
                *error_message = "ContextSpec.include must be a string or sequence of strings";
            }
            return false;
        }
    }

    uint32_t ignored_mask = 0U;
    if (!parse_optional_spec_string_id(spec, "task_key", view->runtime->strings, &plan->task_key, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "claim_key", view->runtime->strings, &plan->claim_key, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "subject", view->runtime->strings, &plan->subject_label, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "relation", view->runtime->strings, &plan->relation, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "object", view->runtime->strings, &plan->object_label, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "owner", view->runtime->strings, &plan->owner, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "source", view->runtime->strings, &plan->source, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "scope", view->runtime->strings, &plan->scope, 0U, &ignored_mask, error_message) ||
        !parse_optional_query_limit(spec, "budget_items", &plan->limit, error_message) ||
        !parse_optional_query_limit(spec, "limit_per_source", &plan->limit_per_source, error_message)) {
        return false;
    }

    if (plan->limit == 0U || plan->limit == std::numeric_limits<uint32_t>::max()) {
        plan->limit = 32U;
    }
    if (plan->limit_per_source == 0U) {
        plan->limit_per_source = 1U;
    }
    const uint32_t selector_count = std::max<uint32_t>(1U, plan->selector_count);
    plan->limit = std::max<uint32_t>(plan->limit, plan->limit_per_source * selector_count);
    return true;
}

void mix_context_cache_hash(uint64_t* hash, uint64_t value) noexcept {
    *hash ^= value + 0x9e3779b97f4a7c15ULL + (*hash << 6U) + (*hash >> 2U);
}

void mix_context_cache_blob(uint64_t* hash, BlobRef ref) noexcept {
    mix_context_cache_hash(hash, ref.pool_id);
    mix_context_cache_hash(hash, ref.offset);
    mix_context_cache_hash(hash, ref.size);
}

uint64_t context_graph_plan_hash(const ContextQueryPlan& plan) noexcept {
    uint64_t hash = 0xcbf29ce484222325ULL;
    mix_context_cache_hash(&hash, plan.include_mask);
    mix_context_cache_hash(&hash, plan.task_key);
    mix_context_cache_hash(&hash, plan.claim_key);
    mix_context_cache_hash(&hash, plan.subject_label);
    mix_context_cache_hash(&hash, plan.relation);
    mix_context_cache_hash(&hash, plan.object_label);
    mix_context_cache_hash(&hash, plan.owner);
    mix_context_cache_hash(&hash, plan.source);
    mix_context_cache_hash(&hash, plan.scope);
    mix_context_cache_hash(&hash, plan.limit);
    mix_context_cache_hash(&hash, plan.limit_per_source);
    mix_context_cache_hash(&hash, plan.selector_count);
    mix_context_cache_hash(&hash, plan.supported_claims_only ? 1U : 0U);
    mix_context_cache_hash(&hash, plan.confirmed_claims_only ? 1U : 0U);
    mix_context_cache_hash(&hash, plan.selected_decisions_only ? 1U : 0U);
    mix_context_cache_hash(&hash, plan.knowledge_neighborhood ? 1U : 0U);
    for (uint16_t order : plan.selector_order) {
        mix_context_cache_hash(&hash, order);
    }
    return hash;
}

uint64_t context_graph_patch_hash(
    const IntelligencePatch& intelligence,
    const KnowledgeGraphPatch& knowledge_graph
) noexcept {
    uint64_t hash = 0x84222325cbf29ce4ULL;
    mix_context_cache_hash(&hash, intelligence.tasks.size());
    for (const IntelligenceTaskWrite& write : intelligence.tasks) {
        mix_context_cache_hash(&hash, write.key);
        mix_context_cache_hash(&hash, write.title);
        mix_context_cache_hash(&hash, write.owner);
        mix_context_cache_blob(&hash, write.payload);
        mix_context_cache_blob(&hash, write.result);
        mix_context_cache_hash(&hash, static_cast<uint8_t>(write.status));
        mix_context_cache_hash(&hash, static_cast<uint32_t>(write.priority));
        mix_context_cache_hash(&hash, static_cast<uint32_t>(std::max(0.0F, write.confidence) * 100000.0F));
        mix_context_cache_hash(&hash, write.field_mask);
    }
    mix_context_cache_hash(&hash, intelligence.claims.size());
    for (const IntelligenceClaimWrite& write : intelligence.claims) {
        mix_context_cache_hash(&hash, write.key);
        mix_context_cache_hash(&hash, write.subject_label);
        mix_context_cache_hash(&hash, write.relation);
        mix_context_cache_hash(&hash, write.object_label);
        mix_context_cache_blob(&hash, write.statement);
        mix_context_cache_hash(&hash, static_cast<uint8_t>(write.status));
        mix_context_cache_hash(&hash, static_cast<uint32_t>(std::max(0.0F, write.confidence) * 100000.0F));
        mix_context_cache_hash(&hash, write.field_mask);
    }
    mix_context_cache_hash(&hash, intelligence.evidence.size());
    for (const IntelligenceEvidenceWrite& write : intelligence.evidence) {
        mix_context_cache_hash(&hash, write.key);
        mix_context_cache_hash(&hash, write.kind);
        mix_context_cache_hash(&hash, write.source);
        mix_context_cache_blob(&hash, write.content);
        mix_context_cache_hash(&hash, write.task_key);
        mix_context_cache_hash(&hash, write.claim_key);
        mix_context_cache_hash(&hash, static_cast<uint32_t>(std::max(0.0F, write.confidence) * 100000.0F));
        mix_context_cache_hash(&hash, write.field_mask);
    }
    mix_context_cache_hash(&hash, intelligence.decisions.size());
    for (const IntelligenceDecisionWrite& write : intelligence.decisions) {
        mix_context_cache_hash(&hash, write.key);
        mix_context_cache_hash(&hash, write.task_key);
        mix_context_cache_hash(&hash, write.claim_key);
        mix_context_cache_blob(&hash, write.summary);
        mix_context_cache_hash(&hash, static_cast<uint8_t>(write.status));
        mix_context_cache_hash(&hash, static_cast<uint32_t>(std::max(0.0F, write.confidence) * 100000.0F));
        mix_context_cache_hash(&hash, write.field_mask);
    }
    mix_context_cache_hash(&hash, intelligence.memories.size());
    for (const IntelligenceMemoryWrite& write : intelligence.memories) {
        mix_context_cache_hash(&hash, write.key);
        mix_context_cache_hash(&hash, static_cast<uint8_t>(write.layer));
        mix_context_cache_hash(&hash, write.scope);
        mix_context_cache_blob(&hash, write.content);
        mix_context_cache_hash(&hash, write.task_key);
        mix_context_cache_hash(&hash, write.claim_key);
        mix_context_cache_hash(&hash, static_cast<uint32_t>(std::max(0.0F, write.importance) * 100000.0F));
        mix_context_cache_hash(&hash, write.field_mask);
    }
    mix_context_cache_hash(&hash, knowledge_graph.entities.size());
    for (const KnowledgeEntityWrite& write : knowledge_graph.entities) {
        mix_context_cache_hash(&hash, write.label);
        mix_context_cache_blob(&hash, write.payload);
    }
    mix_context_cache_hash(&hash, knowledge_graph.triples.size());
    for (const KnowledgeTripleWrite& write : knowledge_graph.triples) {
        mix_context_cache_hash(&hash, write.subject_label);
        mix_context_cache_hash(&hash, write.relation);
        mix_context_cache_hash(&hash, write.object_label);
        mix_context_cache_blob(&hash, write.payload);
    }
    return hash;
}

uint64_t context_graph_cache_key(RuntimeViewProxy* view, const ContextQueryPlan& plan) noexcept {
    uint64_t hash = context_graph_plan_hash(plan);
    mix_context_cache_hash(&hash, view->intelligence == nullptr ? 0U : view->intelligence->task_count());
    mix_context_cache_hash(&hash, view->intelligence == nullptr ? 0U : view->intelligence->claim_count());
    mix_context_cache_hash(&hash, view->intelligence == nullptr ? 0U : view->intelligence->evidence_count());
    mix_context_cache_hash(&hash, view->intelligence == nullptr ? 0U : view->intelligence->decision_count());
    mix_context_cache_hash(&hash, view->intelligence == nullptr ? 0U : view->intelligence->memory_count());
    mix_context_cache_hash(&hash, view->runtime->knowledge_graph.entity_count());
    mix_context_cache_hash(&hash, view->runtime->knowledge_graph.triple_count());
    mix_context_cache_hash(
        &hash,
        context_graph_patch_hash(view->pending_intelligence_patch, view->pending_knowledge_graph_patch)
    );
    return hash;
}

} // namespace

PyObject* runtime_query_knowledge(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return nullptr;
    }

    GraphHandle* handle = GraphHandleRegistry::instance().find(view->runtime->graph_id);
    if (handle == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Graph handle not found";
        }
        return nullptr;
    }

    uint32_t ignored_mask = 0U;
    InternedStringId subject_id = 0U;
    InternedStringId relation_id = 0U;
    InternedStringId object_id = 0U;
    uint32_t limit = std::numeric_limits<uint32_t>::max();
    std::string direction = "match";

    if (!parse_optional_spec_string_id(
            spec,
            "subject",
            view->runtime->strings,
            &subject_id,
            0U,
            &ignored_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "relation",
            view->runtime->strings,
            &relation_id,
            0U,
            &ignored_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "object",
            view->runtime->strings,
            &object_id,
            0U,
            &ignored_mask,
            error_message
        ) ||
        !parse_optional_query_limit(spec, "limit", &limit, error_message) ||
        !parse_optional_spec_string(spec, "direction", &direction, error_message)) {
        return nullptr;
    }

    if (direction != "match" &&
        direction != "outgoing" &&
        direction != "incoming" &&
        direction != "both" &&
        direction != "neighborhood") {
        if (error_message != nullptr) {
            *error_message = "knowledge query direction must be 'match', 'outgoing', 'incoming', 'both', or 'neighborhood'";
        }
        return nullptr;
    }

    const std::optional<InternedStringId> subject =
        subject_id == 0U ? std::nullopt : std::optional<InternedStringId>{subject_id};
    const std::optional<InternedStringId> relation =
        relation_id == 0U ? std::nullopt : std::optional<InternedStringId>{relation_id};
    const std::optional<InternedStringId> object =
        object_id == 0U ? std::nullopt : std::optional<InternedStringId>{object_id};

    const KnowledgeGraphStore& graph = view->runtime->knowledge_graph;
    KnowledgeOverlayPendingIndex pending_index;
    std::vector<KnowledgeOverlayPendingTriple> pending_triples = build_knowledge_overlay_pending_triples(
        graph,
        view->pending_knowledge_graph_patch,
        &pending_index
    );
    const std::size_t overlay_triple_count = knowledge_overlay_triple_count(graph, pending_triples);

    std::vector<KnowledgeOverlayTripleResult> triples;
    triples.reserve(std::min<std::size_t>(overlay_triple_count, limit));
    KnowledgeOverlaySeenSet seen;
    seen.reserve(std::min<std::size_t>(overlay_triple_count, limit));

    const auto append_base_matches = [&](std::vector<const KnowledgeTriple*> matches) {
        for (const KnowledgeTriple* triple : matches) {
            if (triple == nullptr) {
                continue;
            }
            const KnowledgeOverlayKey key = knowledge_overlay_key_from_triple(graph, *triple);
            if (key.subject == 0U || key.object == 0U || seen.find(key) != seen.end()) {
                continue;
            }
            BlobRef payload = triple->payload;
            const auto pending = pending_index.find(key);
            if (pending != pending_index.end() && !pending_triples[pending->second].payload.empty()) {
                payload = pending_triples[pending->second].payload;
            }
            seen.insert(key);
            triples.push_back(KnowledgeOverlayTripleResult{
                triple->id,
                key.subject,
                key.relation,
                key.object,
                payload
            });
            if (triples.size() >= limit) {
                return;
            }
        }
    };

    const auto append_pending_matches = [&](
        std::optional<InternedStringId> match_subject,
        std::optional<InternedStringId> match_relation,
        std::optional<InternedStringId> match_object
    ) {
        for (const KnowledgeOverlayPendingTriple& triple : pending_triples) {
            if (triple.exists_in_base ||
                seen.find(triple.key) != seen.end() ||
                !knowledge_overlay_key_matches(triple.key, match_subject, match_relation, match_object)) {
                continue;
            }
            seen.insert(triple.key);
            triples.push_back(KnowledgeOverlayTripleResult{
                triple.id,
                triple.key.subject,
                triple.key.relation,
                triple.key.object,
                triple.payload
            });
            if (triples.size() >= limit) {
                return;
            }
        }
    };

    const auto append_matches = [&](
        std::optional<InternedStringId> match_subject,
        std::optional<InternedStringId> match_relation,
        std::optional<InternedStringId> match_object
    ) {
        append_base_matches(graph.match(match_subject, match_relation, match_object));
        if (triples.size() < limit) {
            append_pending_matches(match_subject, match_relation, match_object);
        }
    };

    if (limit > 0U) {
        const bool centered_on_subject = subject.has_value() && !object.has_value();
        if (direction == "incoming" && centered_on_subject) {
            append_matches(std::nullopt, relation, subject);
        } else if (direction == "neighborhood" && centered_on_subject) {
            const RuntimeKnowledgePathCacheKey cache_key{
                *subject,
                relation.value_or(0U),
                limit,
                graph.entity_count(),
                overlay_triple_count,
                view->pending_knowledge_graph_patch.entities.size(),
                view->pending_knowledge_graph_patch.triples.size()
            };
            const auto cached = view->knowledge_path_cache.entries.find(cache_key);
            if (cached != view->knowledge_path_cache.entries.end()) {
                for (const RuntimeKnowledgePathCacheTriple& cached_triple : cached->second) {
                    const KnowledgeOverlayKey key{
                        cached_triple.subject,
                        cached_triple.relation,
                        cached_triple.object
                    };
                    if (seen.find(key) != seen.end()) {
                        continue;
                    }
                    seen.insert(key);
                    triples.push_back(knowledge_overlay_triple_from_cache(cached_triple));
                    if (triples.size() >= limit) {
                        break;
                    }
                }
            } else {
                const std::size_t bounded_limit = limit == std::numeric_limits<uint32_t>::max()
                    ? overlay_triple_count
                    : static_cast<std::size_t>(limit);
                const std::size_t candidate_reserve =
                    std::min<std::size_t>(overlay_triple_count, bounded_limit * 4U);
                std::vector<ScoredKnowledgePathTriple> candidates;
                candidates.reserve(candidate_reserve);
                KnowledgeOverlaySeenSet candidate_seen;
                candidate_seen.reserve(candidate_reserve);
                uint64_t candidate_order = 0U;

                const auto collect_ranked_matches = [&](
                    std::optional<InternedStringId> match_subject,
                    std::optional<InternedStringId> match_relation,
                    std::optional<InternedStringId> match_object,
                    uint8_t depth,
                    int64_t direction_bonus,
                    int64_t frontier_bonus
                ) {
                    const auto append_ranked = [&](KnowledgeOverlayTripleResult triple) {
                        const KnowledgeOverlayKey key{triple.subject, triple.relation, triple.object};
                        if (seen.find(key) != seen.end() || candidate_seen.find(key) != candidate_seen.end()) {
                            return;
                        }
                        candidate_seen.insert(key);
                        const int64_t depth_base = depth == 1U ? 100000 : 70000;
                        const int64_t relation_score =
                            knowledge_relation_rank(view->runtime->strings, triple.relation) * 10;
                        const int64_t score =
                            depth_base + relation_score + direction_bonus + frontier_bonus;
                        candidates.push_back(ScoredKnowledgePathTriple{
                            knowledge_path_cache_triple(triple),
                            score,
                            depth,
                            candidate_order++
                        });
                    };

                    for (const KnowledgeTriple* base_triple : graph.match(match_subject, match_relation, match_object)) {
                        if (base_triple == nullptr) {
                            continue;
                        }
                        const KnowledgeOverlayKey key = knowledge_overlay_key_from_triple(graph, *base_triple);
                        if (key.subject == 0U || key.object == 0U) {
                            continue;
                        }
                        BlobRef payload = base_triple->payload;
                        const auto pending = pending_index.find(key);
                        if (pending != pending_index.end() && !pending_triples[pending->second].payload.empty()) {
                            payload = pending_triples[pending->second].payload;
                        }
                        append_ranked(KnowledgeOverlayTripleResult{
                            base_triple->id,
                            key.subject,
                            key.relation,
                            key.object,
                            payload
                        });
                    }

                    for (const KnowledgeOverlayPendingTriple& pending_triple : pending_triples) {
                        if (pending_triple.exists_in_base ||
                            !knowledge_overlay_key_matches(
                                pending_triple.key,
                                match_subject,
                                match_relation,
                                match_object
                            )) {
                            continue;
                        }
                        append_ranked(KnowledgeOverlayTripleResult{
                            pending_triple.id,
                            pending_triple.key.subject,
                            pending_triple.key.relation,
                            pending_triple.key.object,
                            pending_triple.payload
                        });
                    }
                };

                collect_ranked_matches(subject, relation, std::nullopt, 1U, 220, 0);
                collect_ranked_matches(std::nullopt, relation, subject, 1U, 180, 0);

                std::unordered_map<InternedStringId, uint32_t> frontier_counts;
                frontier_counts.reserve(candidates.size());
                for (const ScoredKnowledgePathTriple& candidate : candidates) {
                    if (candidate.depth != 1U) {
                        continue;
                    }
                    const RuntimeKnowledgePathCacheTriple& triple = candidate.triple;
                    if (triple.subject == *subject && triple.object != *subject) {
                        ++frontier_counts[triple.object];
                    } else if (triple.object == *subject && triple.subject != *subject) {
                        ++frontier_counts[triple.subject];
                    }
                }

                std::vector<std::pair<InternedStringId, uint32_t>> frontier;
                frontier.reserve(frontier_counts.size());
                for (const auto& entry : frontier_counts) {
                    frontier.push_back(entry);
                }
                std::sort(frontier.begin(), frontier.end(), [](const auto& left, const auto& right) {
                    if (left.second != right.second) {
                        return left.second > right.second;
                    }
                    return left.first < right.first;
                });

                constexpr std::size_t kMaxNeighborhoodFrontier = 32U;
                const std::size_t frontier_limit = std::min(frontier.size(), kMaxNeighborhoodFrontier);
                for (std::size_t index = 0U; index < frontier_limit; ++index) {
                    const InternedStringId frontier_label = frontier[index].first;
                    const int64_t frontier_bonus = static_cast<int64_t>(frontier[index].second) * 120;
                    collect_ranked_matches(frontier_label, relation, std::nullopt, 2U, 110, frontier_bonus);
                    collect_ranked_matches(std::nullopt, relation, frontier_label, 2U, 90, frontier_bonus);
                }

                std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
                    if (left.score != right.score) {
                        return left.score > right.score;
                    }
                    if (left.depth != right.depth) {
                        return left.depth < right.depth;
                    }
                    if (left.triple.subject != right.triple.subject) {
                        return left.triple.subject < right.triple.subject;
                    }
                    if (left.triple.relation != right.triple.relation) {
                        return left.triple.relation < right.triple.relation;
                    }
                    if (left.triple.object != right.triple.object) {
                        return left.triple.object < right.triple.object;
                    }
                    return left.order < right.order;
                });

                std::vector<RuntimeKnowledgePathCacheTriple> cache_value;
                cache_value.reserve(std::min<std::size_t>(candidates.size(), limit));
                for (const ScoredKnowledgePathTriple& candidate : candidates) {
                    const KnowledgeOverlayKey key{
                        candidate.triple.subject,
                        candidate.triple.relation,
                        candidate.triple.object
                    };
                    if (seen.find(key) != seen.end()) {
                        continue;
                    }
                    seen.insert(key);
                    triples.push_back(knowledge_overlay_triple_from_cache(candidate.triple));
                    cache_value.push_back(candidate.triple);
                    if (triples.size() >= limit) {
                        break;
                    }
                }

                constexpr std::size_t kMaxPathCacheEntries = 64U;
                if (view->knowledge_path_cache.entries.size() >= kMaxPathCacheEntries) {
                    view->knowledge_path_cache.entries.clear();
                }
                view->knowledge_path_cache.entries.emplace(cache_key, std::move(cache_value));
            }
        } else if (direction == "both" && centered_on_subject) {
            append_matches(subject, relation, std::nullopt);
            if (triples.size() < limit) {
                append_matches(std::nullopt, relation, subject);
            }
        } else {
            append_matches(subject, relation, object);
        }
    }

    PyObject* result = PyDict_New();
    PyObject* list = PyList_New(0);
    PyObject* counts = PyDict_New();
    if (result == nullptr || list == nullptr || counts == nullptr) {
        Py_XDECREF(result);
        Py_XDECREF(list);
        Py_XDECREF(counts);
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    for (const KnowledgeOverlayTripleResult& triple : triples) {
        PyObject* item = PyDict_New();
        if (item == nullptr ||
            !set_python_dict_item(item, "id", PyLong_FromUnsignedLong(triple.id), error_message) ||
            !set_python_dict_item(
                item,
                "subject",
                unicode_from_utf8(view->runtime->strings.resolve(triple.subject)),
                error_message
            ) ||
            !set_python_dict_item(
                item,
                "relation",
                unicode_from_utf8(view->runtime->strings.resolve(triple.relation)),
                error_message
            ) ||
            !set_python_dict_item(
                item,
                "object",
                unicode_from_utf8(view->runtime->strings.resolve(triple.object)),
                error_message
            ) ||
            !set_python_dict_item(
                item,
                "payload",
                handle->convert_value_to_python(
                    Value{triple.payload},
                    view->runtime->blobs,
                    view->runtime->strings,
                    error_message
                ),
                error_message
            ) ||
            !set_python_dict_item(item, "source", unicode_from_utf8("native"), error_message)) {
            Py_XDECREF(item);
            Py_DECREF(list);
            Py_DECREF(counts);
            Py_DECREF(result);
            return nullptr;
        }

        if (PyList_Append(list, item) != 0) {
            Py_DECREF(item);
            Py_DECREF(list);
            Py_DECREF(counts);
            Py_DECREF(result);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        Py_DECREF(item);
    }

    if (!set_python_dict_item(
            counts,
            "entities",
            PyLong_FromSize_t(knowledge_overlay_entity_count(graph, view->pending_knowledge_graph_patch)),
            error_message
        ) ||
        !set_python_dict_item(counts, "triples", PyLong_FromSize_t(overlay_triple_count), error_message) ||
        !set_python_dict_item(result, "triples", list, error_message) ||
        !set_python_dict_item(result, "counts", counts, error_message) ||
        !set_python_dict_item(
            result,
            "staged",
            PyBool_FromLong(!view->pending_knowledge_graph_patch.empty()),
            error_message
        )) {
        Py_DECREF(result);
        return nullptr;
    }
    list = nullptr;
    counts = nullptr;

    return result;
}

bool runtime_stage_task_write(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return false;
    }

    IntelligenceTaskWrite write;
    if (!parse_required_spec_string_id(spec, "key", view->runtime->strings, &write.key, error_message) ||
        !parse_optional_spec_string_id(
            spec,
            "title",
            view->runtime->strings,
            &write.title,
            intelligence_fields::kTaskTitle,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "owner",
            view->runtime->strings,
            &write.owner,
            intelligence_fields::kTaskOwner,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_blob(
            spec,
            "details",
            view->runtime->blobs,
            &write.payload,
            intelligence_fields::kTaskPayload,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_blob(
            spec,
            "result",
            view->runtime->blobs,
            &write.result,
            intelligence_fields::kTaskResult,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_named_enum(
            spec,
            "status",
            &write.status,
            intelligence_fields::kTaskStatus,
            &write.field_mask,
            parse_intelligence_task_status,
            error_message
        ) ||
        !parse_optional_spec_int32(
            spec,
            "priority",
            &write.priority,
            intelligence_fields::kTaskPriority,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_float(
            spec,
            "confidence",
            &write.confidence,
            intelligence_fields::kTaskConfidence,
            &write.field_mask,
            error_message
        )) {
        return false;
    }

    view->pending_intelligence_patch.tasks.push_back(write);
    return true;
}

bool runtime_stage_claim_write(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return false;
    }

    IntelligenceClaimWrite write;
    if (!parse_required_spec_string_id(spec, "key", view->runtime->strings, &write.key, error_message) ||
        !parse_optional_spec_string_id(
            spec,
            "subject",
            view->runtime->strings,
            &write.subject_label,
            intelligence_fields::kClaimSubject,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "relation",
            view->runtime->strings,
            &write.relation,
            intelligence_fields::kClaimRelation,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "object",
            view->runtime->strings,
            &write.object_label,
            intelligence_fields::kClaimObject,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_blob(
            spec,
            "statement",
            view->runtime->blobs,
            &write.statement,
            intelligence_fields::kClaimStatement,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_named_enum(
            spec,
            "status",
            &write.status,
            intelligence_fields::kClaimStatus,
            &write.field_mask,
            parse_intelligence_claim_status,
            error_message
        ) ||
        !parse_optional_spec_float(
            spec,
            "confidence",
            &write.confidence,
            intelligence_fields::kClaimConfidence,
            &write.field_mask,
            error_message
        )) {
        return false;
    }

    view->pending_intelligence_patch.claims.push_back(write);
    return true;
}

bool runtime_stage_evidence_write(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return false;
    }

    IntelligenceEvidenceWrite write;
    if (!parse_required_spec_string_id(spec, "key", view->runtime->strings, &write.key, error_message) ||
        !parse_optional_spec_string_id(
            spec,
            "kind",
            view->runtime->strings,
            &write.kind,
            intelligence_fields::kEvidenceKind,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "source",
            view->runtime->strings,
            &write.source,
            intelligence_fields::kEvidenceSource,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_blob(
            spec,
            "content",
            view->runtime->blobs,
            &write.content,
            intelligence_fields::kEvidenceContent,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "task_key",
            view->runtime->strings,
            &write.task_key,
            intelligence_fields::kEvidenceTaskKey,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "claim_key",
            view->runtime->strings,
            &write.claim_key,
            intelligence_fields::kEvidenceClaimKey,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_float(
            spec,
            "confidence",
            &write.confidence,
            intelligence_fields::kEvidenceConfidence,
            &write.field_mask,
            error_message
        )) {
        return false;
    }

    view->pending_intelligence_patch.evidence.push_back(write);
    return true;
}

bool runtime_stage_decision_write(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return false;
    }

    IntelligenceDecisionWrite write;
    if (!parse_required_spec_string_id(spec, "key", view->runtime->strings, &write.key, error_message) ||
        !parse_optional_spec_string_id(
            spec,
            "task_key",
            view->runtime->strings,
            &write.task_key,
            intelligence_fields::kDecisionTaskKey,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "claim_key",
            view->runtime->strings,
            &write.claim_key,
            intelligence_fields::kDecisionClaimKey,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_blob(
            spec,
            "summary",
            view->runtime->blobs,
            &write.summary,
            intelligence_fields::kDecisionSummary,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_named_enum(
            spec,
            "status",
            &write.status,
            intelligence_fields::kDecisionStatus,
            &write.field_mask,
            parse_intelligence_decision_status,
            error_message
        ) ||
        !parse_optional_spec_float(
            spec,
            "confidence",
            &write.confidence,
            intelligence_fields::kDecisionConfidence,
            &write.field_mask,
            error_message
        )) {
        return false;
    }

    view->pending_intelligence_patch.decisions.push_back(write);
    return true;
}

bool runtime_stage_memory_write(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr || !require_spec_dict(spec, error_message)) {
        return false;
    }

    IntelligenceMemoryWrite write;
    if (!parse_required_spec_string_id(spec, "key", view->runtime->strings, &write.key, error_message) ||
        !parse_optional_named_enum(
            spec,
            "layer",
            &write.layer,
            intelligence_fields::kMemoryLayer,
            &write.field_mask,
            parse_intelligence_memory_layer,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "scope",
            view->runtime->strings,
            &write.scope,
            intelligence_fields::kMemoryScope,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_blob(
            spec,
            "content",
            view->runtime->blobs,
            &write.content,
            intelligence_fields::kMemoryContent,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "task_key",
            view->runtime->strings,
            &write.task_key,
            intelligence_fields::kMemoryTaskKey,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_string_id(
            spec,
            "claim_key",
            view->runtime->strings,
            &write.claim_key,
            intelligence_fields::kMemoryClaimKey,
            &write.field_mask,
            error_message
        ) ||
        !parse_optional_spec_float(
            spec,
            "importance",
            &write.importance,
            intelligence_fields::kMemoryImportance,
            &write.field_mask,
            error_message
        )) {
        return false;
    }

    view->pending_intelligence_patch.memories.push_back(write);
    return true;
}

bool resolve_runtime_intelligence_store(
    PyObject* capsule,
    RuntimeViewProxy** out_view,
    GraphHandle** out_handle,
    IntelligenceStore* out_store,
    std::string* error_message
) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr) {
        return false;
    }

    GraphHandle* handle = GraphHandleRegistry::instance().find(view->runtime->graph_id);
    if (handle == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Graph handle not found";
        }
        return false;
    }

    *out_view = view;
    *out_handle = handle;
    *out_store = *view->intelligence;
    if (!view->pending_intelligence_patch.empty()) {
        out_store->apply(view->pending_intelligence_patch);
    }
    return true;
}

PyObject* runtime_rank_context_graph(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = runtime_view_from_object(capsule, error_message);
    if (view == nullptr) {
        return nullptr;
    }

    GraphHandle* handle = GraphHandleRegistry::instance().find(view->runtime->graph_id);
    if (handle == nullptr) {
        if (error_message != nullptr) {
            *error_message = "Graph handle not found";
        }
        return nullptr;
    }

    ContextQueryPlan plan;
    if (!build_context_query_plan(spec, view, &plan, error_message)) {
        return nullptr;
    }

    const uint64_t cache_key = context_graph_cache_key(view, plan);
    RuntimeContextGraphCache& cache = view->context_graph_cache;
    if (cache.valid && cache.key == cache_key && cache.result != nullptr) {
        Py_INCREF(cache.result);
        return cache.result;
    }

    if (!cache.valid || cache.key != cache_key || cache.index == nullptr) {
        cache.clear_result();
        cache.intelligence = *view->intelligence;
        if (!view->pending_intelligence_patch.empty()) {
            cache.intelligence.apply(view->pending_intelligence_patch);
        }
        cache.knowledge_graph = view->runtime->knowledge_graph;
        if (!view->pending_knowledge_graph_patch.empty()) {
            cache.knowledge_graph.apply(view->pending_knowledge_graph_patch);
        }
        cache.index = std::make_unique<ContextGraphIndex>(
            cache.intelligence,
            cache.knowledge_graph,
            plan
        );
        cache.key = cache_key;
        cache.valid = true;
    } else {
        cache.clear_result();
    }

    const ContextGraphResult ranked = cache.index->rank();
    PyObject* result = PyDict_New();
    PyObject* records = PyList_New(0);
    PyObject* counts = PyDict_New();
    if (result == nullptr || records == nullptr || counts == nullptr) {
        Py_XDECREF(result);
        Py_XDECREF(records);
        Py_XDECREF(counts);
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }

    std::array<uint32_t, static_cast<std::size_t>(ContextRecordKind::Count)> kind_counts{};
    for (const ContextGraphRecordRef& ref : ranked.records) {
        PyObject* item = PyDict_New();
        PyObject* record = context_record_to_python(
            ref,
            *handle,
            cache.intelligence,
            cache.knowledge_graph,
            *view->blobs,
            *view->strings,
            error_message
        );
        if (item == nullptr || record == nullptr) {
            Py_XDECREF(item);
            Py_XDECREF(record);
            Py_DECREF(records);
            Py_DECREF(counts);
            Py_DECREF(result);
            if (error_message != nullptr && error_message->empty()) {
                *error_message = "context graph selected a missing record";
            }
            return nullptr;
        }

        if (!set_python_dict_item(item, "kind", unicode_from_utf8(context_record_kind_name(ref.kind)), error_message) ||
            !set_python_dict_item(item, "score", PyLong_FromLongLong(ref.score), error_message)) {
            Py_DECREF(record);
            Py_DECREF(item);
            Py_DECREF(records);
            Py_DECREF(counts);
            Py_DECREF(result);
            return nullptr;
        }
        if (!set_python_dict_item(item, "record", record, error_message)) {
            Py_DECREF(item);
            Py_DECREF(records);
            Py_DECREF(counts);
            Py_DECREF(result);
            return nullptr;
        }
        record = nullptr;
        if (PyList_Append(records, item) != 0) {
            Py_DECREF(item);
            Py_DECREF(records);
            Py_DECREF(counts);
            Py_DECREF(result);
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        Py_DECREF(item);
        ++kind_counts[static_cast<std::size_t>(ref.kind)];
    }

    for (std::size_t index = 0U; index < kind_counts.size(); ++index) {
        const auto kind = static_cast<ContextRecordKind>(index);
        if (!set_python_dict_item(
                counts,
                context_record_kind_name(kind),
                PyLong_FromUnsignedLong(kind_counts[index]),
                error_message
            )) {
            Py_DECREF(records);
            Py_DECREF(counts);
            Py_DECREF(result);
            return nullptr;
        }
    }

    if (!set_python_dict_item(result, "records", records, error_message) ||
        !set_python_dict_item(result, "counts", counts, error_message) ||
        !set_python_dict_item(result, "native", PyBool_FromLong(1), error_message)) {
        Py_DECREF(result);
        return nullptr;
    }
    records = nullptr;
    counts = nullptr;
    Py_INCREF(result);
    cache.result = result;
    return result;
}

bool build_intelligence_query(
    PyObject* spec,
    RuntimeViewProxy* view,
    IntelligenceQuery* query,
    std::string* error_message
) {
    if (!require_spec_dict(spec, error_message)) {
        return false;
    }

    uint32_t ignored_mask = 0U;

    PyObject* kind_value = PyDict_GetItemString(spec, "kind");
    if (kind_value != nullptr && kind_value != Py_None) {
        if (!PyUnicode_Check(kind_value)) {
            if (error_message != nullptr) {
                *error_message = "kind must be a string";
            }
            return false;
        }
        const char* utf8 = PyUnicode_AsUTF8(kind_value);
        if (!parse_intelligence_record_kind(
                utf8 == nullptr ? std::string_view{} : std::string_view(utf8),
                &query->kind,
                error_message
            )) {
            return false;
        }
    }

    if (!parse_optional_spec_string_id(
            spec,
            "key",
            view->runtime->strings,
            &query->key,
            0U,
            &ignored_mask,
            error_message
        )) {
        return false;
    }
    if (!parse_optional_spec_string(spec, "key_prefix", &query->key_prefix, error_message) ||
        !parse_optional_spec_string_id(spec, "task_key", view->runtime->strings, &query->task_key, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "claim_key", view->runtime->strings, &query->claim_key, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "subject", view->runtime->strings, &query->subject_label, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "relation", view->runtime->strings, &query->relation, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "object", view->runtime->strings, &query->object_label, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "owner", view->runtime->strings, &query->owner, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "source", view->runtime->strings, &query->source, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "scope", view->runtime->strings, &query->scope, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_float(spec, "min_confidence", &query->min_confidence, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_float(spec, "min_importance", &query->min_importance, 0U, &ignored_mask, error_message) ||
        !parse_optional_query_limit(spec, "limit", &query->limit, error_message)) {
        return false;
    }

    if (PyObject* status_value = PyDict_GetItemString(spec, "status");
        status_value != nullptr && status_value != Py_None) {
        if (!PyUnicode_Check(status_value)) {
            if (error_message != nullptr) {
                *error_message = "status must be a string";
            }
            return false;
        }
        const char* utf8 = PyUnicode_AsUTF8(status_value);
        const std::string_view name = utf8 == nullptr ? std::string_view{} : std::string_view(utf8);
        switch (query->kind) {
            case IntelligenceRecordKind::Tasks: {
                query->task_status = parse_intelligence_task_status(name);
                if (!query->task_status.has_value()) {
                    if (error_message != nullptr) {
                        *error_message = "unsupported task status value";
                    }
                    return false;
                }
                break;
            }
            case IntelligenceRecordKind::Claims: {
                query->claim_status = parse_intelligence_claim_status(name);
                if (!query->claim_status.has_value()) {
                    if (error_message != nullptr) {
                        *error_message = "unsupported claim status value";
                    }
                    return false;
                }
                break;
            }
            case IntelligenceRecordKind::Decisions: {
                query->decision_status = parse_intelligence_decision_status(name);
                if (!query->decision_status.has_value()) {
                    if (error_message != nullptr) {
                        *error_message = "unsupported decision status value";
                    }
                    return false;
                }
                break;
            }
            default:
                if (error_message != nullptr) {
                    *error_message = "status filters require kind='tasks', 'claims', or 'decisions'";
                }
                return false;
        }
    }

    if (PyObject* layer_value = PyDict_GetItemString(spec, "layer");
        layer_value != nullptr && layer_value != Py_None) {
        if (!PyUnicode_Check(layer_value)) {
            if (error_message != nullptr) {
                *error_message = "layer must be a string";
            }
            return false;
        }
        const char* utf8 = PyUnicode_AsUTF8(layer_value);
        query->memory_layer = parse_intelligence_memory_layer(
            utf8 == nullptr ? std::string_view{} : std::string_view(utf8)
        );
        if (!query->memory_layer.has_value()) {
            if (error_message != nullptr) {
                *error_message = "unsupported memory layer value";
            }
            return false;
        }
    }

    if (PyObject* task_status_value = PyDict_GetItemString(spec, "task_status");
        task_status_value != nullptr && task_status_value != Py_None) {
        if (!PyUnicode_Check(task_status_value)) {
            if (error_message != nullptr) {
                *error_message = "task_status must be a string";
            }
            return false;
        }
        const char* utf8 = PyUnicode_AsUTF8(task_status_value);
        query->task_status = parse_intelligence_task_status(
            utf8 == nullptr ? std::string_view{} : std::string_view(utf8)
        );
        if (!query->task_status.has_value()) {
            if (error_message != nullptr) {
                *error_message = "unsupported task_status value";
            }
            return false;
        }
    }

    return true;
}

bool build_intelligence_route_rule(
    PyObject* spec,
    RuntimeViewProxy* view,
    IntelligenceRouteRule* rule,
    std::string* error_message
) {
    if (!build_intelligence_query(spec, view, &rule->query, error_message)) {
        return false;
    }

    PyObject* goto_value = PyDict_GetItemString(spec, "goto");
    if (goto_value == nullptr || goto_value == Py_None) {
        if (error_message != nullptr) {
            *error_message = "intelligence route rule requires goto";
        }
        return false;
    }
    if (!PyUnicode_Check(goto_value)) {
        if (error_message != nullptr) {
            *error_message = "goto must be a string";
        }
        return false;
    }
    rule->target = PyUnicode_AsUTF8(goto_value);

    if (PyObject* min_count_value = PyDict_GetItemString(spec, "min_count");
        min_count_value != nullptr && min_count_value != Py_None) {
        const unsigned long raw = PyLong_AsUnsignedLong(min_count_value);
        if (PyErr_Occurred() != nullptr) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            PyErr_Clear();
            return false;
        }
        rule->min_count = static_cast<uint32_t>(raw);
    }

    if (PyObject* max_count_value = PyDict_GetItemString(spec, "max_count");
        max_count_value != nullptr && max_count_value != Py_None) {
        const unsigned long raw = PyLong_AsUnsignedLong(max_count_value);
        if (PyErr_Occurred() != nullptr) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            PyErr_Clear();
            return false;
        }
        rule->max_count = static_cast<uint32_t>(raw);
    }

    return true;
}

PyObject* runtime_snapshot_intelligence(PyObject* capsule, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }
    return intelligence_snapshot_to_python(
        *handle,
        working_store.snapshot(),
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_query_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }

    IntelligenceQuery query;
    if (!build_intelligence_query(spec, view, &query, error_message)) {
        return nullptr;
    }

    const IntelligenceSnapshot filtered = query_intelligence_records(working_store, *view->strings, query);
    return intelligence_snapshot_to_python(
        *handle,
        filtered,
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_related_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }
    if (!require_spec_dict(spec, error_message)) {
        return nullptr;
    }

    InternedStringId task_key = 0U;
    InternedStringId claim_key = 0U;
    uint32_t limit = 0U;
    uint32_t hops = 1U;
    uint32_t ignored_mask = 0U;
    if (!parse_optional_spec_string_id(spec, "task_key", view->runtime->strings, &task_key, 0U, &ignored_mask, error_message) ||
        !parse_optional_spec_string_id(spec, "claim_key", view->runtime->strings, &claim_key, 0U, &ignored_mask, error_message) ||
        !parse_optional_query_limit(spec, "limit", &limit, error_message) ||
        !parse_optional_query_limit(spec, "hops", &hops, error_message)) {
        return nullptr;
    }
    if (task_key == 0U && claim_key == 0U) {
        if (error_message != nullptr) {
            *error_message = "related intelligence lookup requires task_key or claim_key";
        }
        return nullptr;
    }
    if (hops == 0U) {
        if (error_message != nullptr) {
            *error_message = "related intelligence hops must be >= 1";
        }
        return nullptr;
    }

    const IntelligenceSnapshot related =
        related_intelligence_records(working_store, task_key, claim_key, limit, hops);
    return intelligence_snapshot_to_python(
        *handle,
        related,
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_agenda_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }

    IntelligenceQuery query;
    if (!build_intelligence_query(spec, view, &query, error_message)) {
        return nullptr;
    }
    if (query.kind != IntelligenceRecordKind::All && query.kind != IntelligenceRecordKind::Tasks) {
        if (error_message != nullptr) {
            *error_message = "intelligence agenda only supports task filters";
        }
        return nullptr;
    }

    const IntelligenceSnapshot agenda = agenda_intelligence_tasks(working_store, *view->strings, query);
    return intelligence_snapshot_to_python(
        *handle,
        agenda,
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_supporting_claims_intelligence(
    PyObject* capsule,
    PyObject* spec,
    std::string* error_message
) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }

    IntelligenceQuery query;
    if (!build_intelligence_query(spec, view, &query, error_message)) {
        return nullptr;
    }
    if (query.kind != IntelligenceRecordKind::All && query.kind != IntelligenceRecordKind::Claims) {
        if (error_message != nullptr) {
            *error_message = "supporting claim retrieval only supports claim filters";
        }
        return nullptr;
    }

    const IntelligenceSnapshot ranked_claims =
        supporting_intelligence_claims(working_store, *view->strings, query);
    return intelligence_snapshot_to_python(
        *handle,
        ranked_claims,
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_action_candidates_intelligence(
    PyObject* capsule,
    PyObject* spec,
    std::string* error_message
) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }

    IntelligenceQuery query;
    if (!build_intelligence_query(spec, view, &query, error_message)) {
        return nullptr;
    }

    const IntelligenceSnapshot ranked_tasks =
        action_intelligence_tasks(working_store, *view->strings, query);
    return intelligence_snapshot_to_python(
        *handle,
        ranked_tasks,
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_recall_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }

    IntelligenceQuery query;
    if (!build_intelligence_query(spec, view, &query, error_message)) {
        return nullptr;
    }
    if (query.kind != IntelligenceRecordKind::All && query.kind != IntelligenceRecordKind::Memories) {
        if (error_message != nullptr) {
            *error_message = "intelligence recall only supports memory filters";
        }
        return nullptr;
    }

    const IntelligenceSnapshot recall = recall_intelligence_memories(working_store, *view->strings, query);
    return intelligence_snapshot_to_python(
        *handle,
        recall,
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_focus_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }

    IntelligenceQuery query;
    if (!build_intelligence_query(spec, view, &query, error_message)) {
        return nullptr;
    }
    if (query.kind != IntelligenceRecordKind::All) {
        if (error_message != nullptr) {
            *error_message = "intelligence focus operates across record kinds; use query, agenda, or recall for single-kind reads";
        }
        return nullptr;
    }

    const IntelligenceSnapshot focus = focus_intelligence_records(working_store, *view->strings, query);
    return intelligence_snapshot_to_python(
        *handle,
        focus,
        *view->blobs,
        *view->strings,
        error_message
    );
}

PyObject* runtime_intelligence_summary(PyObject* capsule, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }
    static_cast<void>(handle);
    return intelligence_summary_to_python(summarize_intelligence(working_store), error_message);
}

PyObject* runtime_count_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }
    static_cast<void>(handle);

    IntelligenceQuery query;
    if (!build_intelligence_query(spec, view, &query, error_message)) {
        return nullptr;
    }

    return PyLong_FromSize_t(count_intelligence_records(working_store, *view->strings, query));
}

PyObject* runtime_route_intelligence(PyObject* capsule, PyObject* spec, std::string* error_message) {
    RuntimeViewProxy* view = nullptr;
    GraphHandle* handle = nullptr;
    IntelligenceStore working_store;
    if (!resolve_runtime_intelligence_store(capsule, &view, &handle, &working_store, error_message)) {
        return nullptr;
    }
    static_cast<void>(handle);

    if (!require_spec_dict(spec, error_message)) {
        return nullptr;
    }

    PyObject* rules_value = PyDict_GetItemString(spec, "rules");
    if (rules_value == nullptr || rules_value == Py_None || !PySequence_Check(rules_value)) {
        if (error_message != nullptr) {
            *error_message = "intelligence route requires a rules sequence";
        }
        return nullptr;
    }

    std::vector<IntelligenceRouteRule> rules;
    const Py_ssize_t rule_count = PySequence_Size(rules_value);
    if (rule_count < 0) {
        if (error_message != nullptr) {
            *error_message = fetch_python_error();
        }
        return nullptr;
    }
    rules.reserve(static_cast<std::size_t>(rule_count));

    for (Py_ssize_t index = 0; index < rule_count; ++index) {
        PyObject* item = PySequence_GetItem(rules_value, index);
        if (item == nullptr) {
            if (error_message != nullptr) {
                *error_message = fetch_python_error();
            }
            return nullptr;
        }
        IntelligenceRouteRule rule;
        const bool ok = require_spec_dict(item, error_message) &&
            build_intelligence_route_rule(item, view, &rule, error_message);
        Py_DECREF(item);
        if (!ok) {
            return nullptr;
        }
        rules.push_back(std::move(rule));
    }

    const std::optional<std::string> selected =
        select_intelligence_route(working_store, *view->strings, rules);
    if (selected.has_value()) {
        return unicode_from_utf8(*selected);
    }

    PyObject* default_value = PyDict_GetItemString(spec, "default");
    if (default_value == nullptr || default_value == Py_None) {
        Py_RETURN_NONE;
    }
    if (!PyUnicode_Check(default_value)) {
        if (error_message != nullptr) {
            *error_message = "intelligence route default must be a string or None";
        }
        return nullptr;
    }
    return unicode_from_utf8(PyUnicode_AsUTF8(default_value));
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
