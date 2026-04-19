#include "agentcore/state/state_store.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <cstring>
#include <istream>
#include <memory>
#include <ostream>
#include <stdexcept>

namespace agentcore {

namespace {

template <typename T>
void write_pod(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!output) {
        throw std::runtime_error("failed to write serialized state");
    }
}

template <typename T>
T read_pod(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!input) {
        throw std::runtime_error("failed to read serialized state");
    }
    return value;
}

void write_string(std::ostream& output, std::string_view value) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(value.size()));
    if (!value.empty()) {
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        if (!output) {
            throw std::runtime_error("failed to write serialized string");
        }
    }
}

std::string read_string(std::istream& input) {
    const uint64_t size = read_pod<uint64_t>(input);
    std::string value(size, '\0');
    if (size != 0U) {
        input.read(value.data(), static_cast<std::streamsize>(size));
        if (!input) {
            throw std::runtime_error("failed to read serialized string");
        }
    }
    return value;
}

void write_blob_ref(std::ostream& output, const BlobRef& ref) {
    write_pod(output, ref.pool_id);
    write_pod(output, ref.offset);
    write_pod(output, ref.size);
}

BlobRef read_blob_ref(std::istream& input) {
    return BlobRef{
        read_pod<uint32_t>(input),
        read_pod<uint32_t>(input),
        read_pod<uint32_t>(input)
    };
}

bool blob_refs_equal(const BlobStore& blobs, BlobRef left, BlobRef right) {
    if (left.pool_id == right.pool_id &&
        left.offset == right.offset &&
        left.size == right.size) {
        return true;
    }
    if (left.empty() || right.empty()) {
        return left.empty() && right.empty();
    }
    return blobs.read_bytes(left) == blobs.read_bytes(right);
}

struct TripleDeltaKey {
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    InternedStringId object_label{0};

    bool operator==(const TripleDeltaKey& other) const noexcept {
        return subject_label == other.subject_label &&
            relation == other.relation &&
            object_label == other.object_label;
    }
};

struct TripleDeltaKeyHash {
    std::size_t operator()(const TripleDeltaKey& key) const noexcept {
        std::size_t hash = static_cast<std::size_t>(key.subject_label);
        hash = (hash * 1315423911U) ^ static_cast<std::size_t>(key.relation);
        hash = (hash * 2654435761U) ^ static_cast<std::size_t>(key.object_label);
        return hash;
    }
};

void write_value(std::ostream& output, const Value& value) {
    const uint8_t index = static_cast<uint8_t>(value.index());
    write_pod(output, index);
    switch (value.index()) {
        case 0:
            break;
        case 1:
            write_pod(output, std::get<int64_t>(value));
            break;
        case 2:
            write_pod(output, std::get<double>(value));
            break;
        case 3:
            write_pod(output, std::get<bool>(value));
            break;
        case 4:
            write_blob_ref(output, std::get<BlobRef>(value));
            break;
        case 5:
            write_pod(output, std::get<InternedStringId>(value));
            break;
        default:
            throw std::runtime_error("unsupported value variant index");
    }
}

Value read_value(std::istream& input) {
    const uint8_t index = read_pod<uint8_t>(input);
    switch (index) {
        case 0:
            return std::monostate{};
        case 1:
            return read_pod<int64_t>(input);
        case 2:
            return read_pod<double>(input);
        case 3:
            return read_pod<bool>(input);
        case 4:
            return read_blob_ref(input);
        case 5:
            return read_pod<InternedStringId>(input);
        default:
            throw std::runtime_error("unsupported serialized value variant index");
    }
}

void write_knowledge_graph_patch(std::ostream& output, const KnowledgeGraphPatch& patch) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.entities.size()));
    for (const KnowledgeEntityWrite& entity : patch.entities) {
        write_pod(output, entity.label);
        write_blob_ref(output, entity.payload);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.triples.size()));
    for (const KnowledgeTripleWrite& triple : patch.triples) {
        write_pod(output, triple.subject_label);
        write_pod(output, triple.relation);
        write_pod(output, triple.object_label);
        write_blob_ref(output, triple.payload);
    }
}

KnowledgeGraphPatch read_knowledge_graph_patch(std::istream& input) {
    KnowledgeGraphPatch patch;

    const uint64_t entity_count = read_pod<uint64_t>(input);
    patch.entities.reserve(static_cast<std::size_t>(entity_count));
    for (uint64_t index = 0; index < entity_count; ++index) {
        patch.entities.push_back(KnowledgeEntityWrite{
            read_pod<InternedStringId>(input),
            read_blob_ref(input)
        });
    }

    const uint64_t triple_count = read_pod<uint64_t>(input);
    patch.triples.reserve(static_cast<std::size_t>(triple_count));
    for (uint64_t index = 0; index < triple_count; ++index) {
        patch.triples.push_back(KnowledgeTripleWrite{
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_blob_ref(input)
        });
    }

    return patch;
}

void write_state_patch(std::ostream& output, const StatePatch& patch) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.updates.size()));
    for (const FieldUpdate& update : patch.updates) {
        write_pod(output, update.key);
        write_value(output, update.value);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.new_blobs.size()));
    for (const BlobRef& blob : patch.new_blobs) {
        write_blob_ref(output, blob);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.task_records.size()));
    for (const TaskRecord& record : patch.task_records) {
        write_pod(output, record.key);
        write_blob_ref(output, record.request);
        write_blob_ref(output, record.output);
        write_pod(output, record.flags);
    }

    write_knowledge_graph_patch(output, patch.knowledge_graph);
    write_pod(output, patch.flags);
}

StatePatch read_state_patch(std::istream& input) {
    StatePatch patch;

    const uint64_t update_count = read_pod<uint64_t>(input);
    patch.updates.reserve(static_cast<std::size_t>(update_count));
    for (uint64_t index = 0; index < update_count; ++index) {
        patch.updates.push_back(FieldUpdate{
            read_pod<StateKey>(input),
            read_value(input)
        });
    }

    const uint64_t blob_count = read_pod<uint64_t>(input);
    patch.new_blobs.reserve(static_cast<std::size_t>(blob_count));
    for (uint64_t index = 0; index < blob_count; ++index) {
        patch.new_blobs.push_back(read_blob_ref(input));
    }

    const uint64_t task_record_count = read_pod<uint64_t>(input);
    patch.task_records.reserve(static_cast<std::size_t>(task_record_count));
    for (uint64_t index = 0; index < task_record_count; ++index) {
        patch.task_records.push_back(TaskRecord{
            read_pod<InternedStringId>(input),
            read_blob_ref(input),
            read_blob_ref(input),
            read_pod<uint32_t>(input)
        });
    }

    patch.knowledge_graph = read_knowledge_graph_patch(input);
    patch.flags = read_pod<uint32_t>(input);
    return patch;
}

void write_workflow_state(std::ostream& output, const WorkflowState& state) {
    const uint64_t version = state.version.load(std::memory_order_acquire);
    const uint64_t field_count = state.total_capacity.load(std::memory_order_acquire);
    write_pod(output, version);
    write_pod(output, field_count);
    for (uint64_t index = 0; index < field_count; ++index) {
        write_value(output, state.load(static_cast<StateKey>(index)));
    }
}

WorkflowState read_workflow_state(std::istream& input) {
    WorkflowState state;
    const uint64_t version = read_pod<uint64_t>(input);
    const uint64_t field_count = read_pod<uint64_t>(input);
    state.resize(static_cast<std::size_t>(field_count));
    state.version.store(version, std::memory_order_release);
    for (uint64_t index = 0; index < field_count; ++index) {
        state.store(static_cast<StateKey>(index), read_value(input));
    }
    return state;
}

} // namespace

WorkflowState::WorkflowState(const WorkflowState& other)
    : segments(), total_capacity(0), version(0) {
    *this = other;
}

WorkflowState& WorkflowState::operator=(const WorkflowState& other) {
    if (this == &other) {
        return *this;
    }
    segments = other.segments;
    total_capacity.store(other.total_capacity.load(std::memory_order_acquire), std::memory_order_release);
    version.store(other.version.load(std::memory_order_acquire), std::memory_order_release);
    return *this;
}

void WorkflowState::resize(std::size_t new_size) {
    std::size_t current_capacity = total_capacity.load(std::memory_order_acquire);
    while (current_capacity < new_size) {
        segments.emplace_back(std::make_shared<Segment>());
        current_capacity += kSegmentSize;
    }
    total_capacity.store(current_capacity, std::memory_order_release);
}

Value WorkflowState::load(StateKey key) const noexcept {
    const std::size_t segment_index = key / kSegmentSize;
    const std::size_t element_index = key % kSegmentSize;
    if (segment_index >= segments.size()) {
        return std::monostate{};
    }
    return segments[segment_index]->values[element_index];
}

void WorkflowState::store(StateKey key, Value value) noexcept {
    const std::size_t segment_index = key / kSegmentSize;
    const std::size_t element_index = key % kSegmentSize;
    if (segment_index >= segments.size()) {
        resize(static_cast<std::size_t>(key) + 1U);
    }
    if (!segments[segment_index].unique()) {
        segments[segment_index] = std::make_shared<Segment>(*segments[segment_index]);
    }
    segments[segment_index]->values[element_index] = std::move(value);
}

struct StringInterner::Storage {
    std::vector<std::string> values;
    std::unordered_map<std::string, InternedStringId> ids_by_value;
};

StringInterner::StringInterner() : storage_(std::make_shared<Storage>()) {
    storage_->values.push_back("");
    storage_->ids_by_value.emplace("", 0U);
}

void StringInterner::ensure_unique() {
    if (!storage_.unique()) {
        storage_ = std::make_shared<Storage>(*storage_);
    }
}

InternedStringId StringInterner::intern(std::string_view value) {
    const auto existing = storage_->ids_by_value.find(std::string(value));
    if (existing != storage_->ids_by_value.end()) {
        return existing->second;
    }

    ensure_unique();
    const auto next_id = static_cast<InternedStringId>(storage_->values.size());
    storage_->values.emplace_back(value);
    storage_->ids_by_value.emplace(storage_->values.back(), next_id);
    return next_id;
}

std::string_view StringInterner::resolve(InternedStringId id) const {
    if (id >= storage_->values.size()) {
        return {};
    }
    return storage_->values[id];
}

std::size_t StringInterner::size() const noexcept {
    return storage_->values.size();
}

bool StringInterner::shares_storage_with(const StringInterner& other) const noexcept {
    return storage_ == other.storage_;
}

void StringInterner::serialize(std::ostream& output) const {
    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->values.size()));
    for (const std::string& value : storage_->values) {
        write_string(output, value);
    }
}

StringInterner StringInterner::deserialize(std::istream& input) {
    StringInterner interner;
    interner.storage_ = std::make_shared<Storage>();

    const uint64_t value_count = read_pod<uint64_t>(input);
    interner.storage_->values.reserve(static_cast<std::size_t>(value_count));
    for (uint64_t index = 0; index < value_count; ++index) {
        interner.storage_->values.push_back(read_string(input));
        interner.storage_->ids_by_value.emplace(
            interner.storage_->values.back(),
            static_cast<InternedStringId>(index)
        );
    }

    if (interner.storage_->values.empty()) {
        interner.storage_->values.push_back("");
        interner.storage_->ids_by_value.emplace("", 0U);
    }
    return interner;
}

struct BlobStore::Storage {
    uint32_t pool_id{1};
    std::vector<std::byte> bytes;
};

BlobStore::BlobStore() : storage_(std::make_shared<Storage>()) {}

void BlobStore::ensure_unique() {
    if (!storage_.unique()) {
        storage_ = std::make_shared<Storage>(*storage_);
    }
}

BlobRef BlobStore::append(const std::byte* bytes, std::size_t size) {
    ensure_unique();
    const auto offset = static_cast<uint32_t>(storage_->bytes.size());
    storage_->bytes.insert(storage_->bytes.end(), bytes, bytes + size);
    return BlobRef{storage_->pool_id, offset, static_cast<uint32_t>(size)};
}

BlobRef BlobStore::append_string(std::string_view text) {
    const auto* raw_bytes = reinterpret_cast<const std::byte*>(text.data());
    return append(raw_bytes, text.size());
}

std::vector<std::byte> BlobStore::read_bytes(BlobRef ref) const {
    const std::size_t start = ref.offset;
    const std::size_t end = start + ref.size;
    if (ref.pool_id != storage_->pool_id || end > storage_->bytes.size()) {
        return {};
    }

    return std::vector<std::byte>(
        storage_->bytes.begin() + static_cast<std::ptrdiff_t>(start),
        storage_->bytes.begin() + static_cast<std::ptrdiff_t>(end)
    );
}

std::pair<const std::byte*, std::size_t> BlobStore::read_buffer(BlobRef ref) const {
    const std::size_t start = ref.offset;
    const std::size_t end = start + ref.size;
    if (ref.pool_id != storage_->pool_id || end > storage_->bytes.size()) {
        return {nullptr, 0};
    }
    return {storage_->bytes.data() + start, ref.size};
}

std::string_view BlobStore::read_string(BlobRef ref) const {
    return read_string_view(ref);
}

std::string_view BlobStore::read_string_view(BlobRef ref) const {
    const std::size_t start = ref.offset;
    const std::size_t end = start + ref.size;
    if (ref.pool_id != storage_->pool_id || end > storage_->bytes.size()) {
        return {};
    }

    const auto* text = reinterpret_cast<const char*>(storage_->bytes.data() + start);
    return std::string_view(text, ref.size);
}

std::size_t BlobStore::size_bytes() const noexcept {
    return storage_->bytes.size();
}

bool BlobStore::shares_storage_with(const BlobStore& other) const noexcept {
    return storage_ == other.storage_;
}

void BlobStore::serialize(std::ostream& output) const {
    write_pod(output, storage_->pool_id);
    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->bytes.size()));
    if (!storage_->bytes.empty()) {
        output.write(
            reinterpret_cast<const char*>(storage_->bytes.data()),
            static_cast<std::streamsize>(storage_->bytes.size())
        );
        if (!output) {
            throw std::runtime_error("failed to write serialized blob store");
        }
    }
}

BlobStore BlobStore::deserialize(std::istream& input) {
    BlobStore store;
    store.storage_ = std::make_shared<Storage>();
    store.storage_->pool_id = read_pod<uint32_t>(input);
    const uint64_t storage_size = read_pod<uint64_t>(input);
    store.storage_->bytes.resize(static_cast<std::size_t>(storage_size));
    if (storage_size != 0U) {
        input.read(
            reinterpret_cast<char*>(store.storage_->bytes.data()),
            static_cast<std::streamsize>(storage_size)
        );
        if (!input) {
            throw std::runtime_error("failed to read serialized blob store");
        }
    }
    return store;
}

struct PatchLog::Storage {
    mutable std::mutex mutex;
    std::vector<PatchLogEntry> entries;
};

PatchLog::PatchLog() : storage_(std::make_shared<Storage>()) {}

PatchLog::PatchLog(const PatchLog& other) : storage_(other.storage_) {}

PatchLog& PatchLog::operator=(const PatchLog& other) {
    if (this == &other) {
        return *this;
    }
    storage_ = other.storage_;
    return *this;
}

void PatchLog::ensure_unique() {
    if (storage_.unique()) {
        return;
    }

    auto cloned = std::make_shared<Storage>();
    {
        std::lock_guard<std::mutex> lock(storage_->mutex);
        cloned->entries = storage_->entries;
    }
    storage_ = std::move(cloned);
}

uint64_t PatchLog::append(uint64_t state_version, const StatePatch& patch) {
    ensure_unique();
    std::lock_guard<std::mutex> lock(storage_->mutex);
    const auto offset = static_cast<uint64_t>(storage_->entries.size());
    storage_->entries.push_back(PatchLogEntry{offset, state_version, patch});
    return offset;
}

const PatchLogEntry* PatchLog::find(uint64_t offset) const {
    std::lock_guard<std::mutex> lock(storage_->mutex);
    if (offset >= storage_->entries.size()) {
        return nullptr;
    }
    return &storage_->entries[static_cast<std::size_t>(offset)];
}

std::vector<PatchLogEntry> PatchLog::entries() const {
    std::lock_guard<std::mutex> lock(storage_->mutex);
    return storage_->entries;
}

std::vector<PatchLogEntry> PatchLog::entries_from(uint64_t offset) const {
    std::lock_guard<std::mutex> lock(storage_->mutex);
    if (offset >= storage_->entries.size()) {
        return {};
    }
    return std::vector<PatchLogEntry>(
        storage_->entries.begin() + static_cast<std::ptrdiff_t>(offset),
        storage_->entries.end()
    );
}

std::size_t PatchLog::size() const noexcept {
    std::lock_guard<std::mutex> lock(storage_->mutex);
    return storage_->entries.size();
}

void PatchLog::serialize(std::ostream& output) const {
    std::lock_guard<std::mutex> lock(storage_->mutex);
    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->entries.size()));
    for (const PatchLogEntry& entry : storage_->entries) {
        write_pod(output, entry.offset);
        write_pod(output, entry.state_version);
        write_state_patch(output, entry.patch);
    }
}

PatchLog PatchLog::deserialize(std::istream& input) {
    PatchLog log;
    log.storage_ = std::make_shared<Storage>();
    const uint64_t entry_count = read_pod<uint64_t>(input);
    log.storage_->entries.reserve(static_cast<std::size_t>(entry_count));
    for (uint64_t index = 0; index < entry_count; ++index) {
        log.storage_->entries.push_back(PatchLogEntry{
            read_pod<uint64_t>(input),
            read_pod<uint64_t>(input),
            read_state_patch(input)
        });
    }
    return log;
}

StateStore::StateStore(std::size_t initial_field_count) {
    reset(initial_field_count);
}

void StateStore::reset(std::size_t initial_field_count) {
    current_state_ = WorkflowState{};
    current_state_.resize(initial_field_count);
    current_state_.version.store(0, std::memory_order_release);
    blob_store_ = BlobStore{};
    patch_log_ = PatchLog{};
    string_interner_ = StringInterner{};
    knowledge_graph_ = KnowledgeGraphStore{};
    task_journal_.clear();
}

StateStore::StateStore(const StateStore& other)
    : current_state_(other.current_state_),
      blob_store_(other.blob_store_),
      patch_log_(other.patch_log_),
      string_interner_(other.string_interner_),
      knowledge_graph_(other.knowledge_graph_),
      task_journal_(other.task_journal_) {}

StateStore& StateStore::operator=(const StateStore& other) {
    if (this == &other) {
        return *this;
    }
    current_state_ = other.current_state_;
    blob_store_ = other.blob_store_;
    patch_log_ = other.patch_log_;
    string_interner_ = other.string_interner_;
    knowledge_graph_ = other.knowledge_graph_;
    task_journal_ = other.task_journal_;
    return *this;
}

void StateStore::ensure_field_capacity(std::size_t field_count) {
    if (current_state_.total_capacity.load(std::memory_order_acquire) < field_count) {
        current_state_.resize(field_count);
    }
}

uint64_t StateStore::apply(const StatePatch& patch) {
    return apply_with_summary(patch).patch_log_offset;
}

StateApplyResult StateStore::apply_with_summary(const StatePatch& patch) {
    StateApplyResult result;
    result.patch_log_offset = static_cast<uint64_t>(patch_log_.size());
    if (patch.empty()) {
        return result;
    }

    for (const FieldUpdate& update : patch.updates) {
        current_state_.store(update.key, update.value);
    }

    if (!patch.knowledge_graph.empty()) {
        KnowledgeGraphStore working_graph = knowledge_graph_;
        std::unordered_map<InternedStringId, std::size_t> entity_delta_indices;
        std::unordered_map<TripleDeltaKey, std::size_t, TripleDeltaKeyHash> triple_delta_indices;

        auto record_entity_delta = [&](KnowledgeEntityId id,
                                       InternedStringId label,
                                       bool created,
                                       bool payload_changed) {
            if (!created && !payload_changed) {
                return;
            }
            const auto [iterator, inserted] =
                entity_delta_indices.emplace(label, result.knowledge_graph_delta.entities.size());
            if (inserted) {
                result.knowledge_graph_delta.entities.push_back(KnowledgeEntityDelta{
                    id,
                    label,
                    created,
                    payload_changed
                });
                return;
            }

            KnowledgeEntityDelta& delta = result.knowledge_graph_delta.entities[iterator->second];
            delta.id = id;
            delta.created = delta.created || created;
            delta.payload_changed = delta.payload_changed || payload_changed;
        };

        auto record_triple_delta = [&](const KnowledgeTriple& triple,
                                       InternedStringId subject_label,
                                       InternedStringId relation,
                                       InternedStringId object_label,
                                       bool created,
                                       bool payload_changed) {
            if (!created && !payload_changed) {
                return;
            }
            const TripleDeltaKey key{subject_label, relation, object_label};
            const auto [iterator, inserted] =
                triple_delta_indices.emplace(key, result.knowledge_graph_delta.triples.size());
            if (inserted) {
                result.knowledge_graph_delta.triples.push_back(KnowledgeTripleDelta{
                    triple.id,
                    triple.subject,
                    subject_label,
                    relation,
                    triple.object,
                    object_label,
                    created,
                    payload_changed
                });
                return;
            }

            KnowledgeTripleDelta& delta = result.knowledge_graph_delta.triples[iterator->second];
            delta.id = triple.id;
            delta.subject = triple.subject;
            delta.object = triple.object;
            delta.created = delta.created || created;
            delta.payload_changed = delta.payload_changed || payload_changed;
        };

        for (const KnowledgeEntityWrite& entity : patch.knowledge_graph.entities) {
            const KnowledgeEntity* existing = working_graph.find_entity_by_label(entity.label);
            const bool created = existing == nullptr;
            const bool payload_changed = !entity.payload.empty() &&
                (existing == nullptr || !blob_refs_equal(blob_store_, existing->payload, entity.payload));
            if (!created && !payload_changed) {
                continue;
            }

            const KnowledgeEntityId id = working_graph.upsert_entity(entity.label, entity.payload);
            record_entity_delta(id, entity.label, created, payload_changed);
        }

        for (const KnowledgeTripleWrite& triple : patch.knowledge_graph.triples) {
            const KnowledgeEntity* subject_before = working_graph.find_entity_by_label(triple.subject_label);
            const KnowledgeEntity* object_before = working_graph.find_entity_by_label(triple.object_label);
            const KnowledgeTriple* existing = working_graph.find_triple_by_labels(
                triple.subject_label,
                triple.relation,
                triple.object_label
            );

            const bool subject_created = subject_before == nullptr;
            const bool object_created = object_before == nullptr;
            const bool created = existing == nullptr;
            const bool payload_changed = !triple.payload.empty() &&
                (existing == nullptr || !blob_refs_equal(blob_store_, existing->payload, triple.payload));
            if (!subject_created && !object_created && !created && !payload_changed) {
                continue;
            }

            const KnowledgeTripleId id = working_graph.upsert_triple(
                triple.subject_label,
                triple.relation,
                triple.object_label,
                triple.payload
            );

            if (subject_created) {
                const KnowledgeEntity* subject = working_graph.find_entity_by_label(triple.subject_label);
                if (subject != nullptr) {
                    record_entity_delta(subject->id, triple.subject_label, true, false);
                }
            }
            if (object_created) {
                const KnowledgeEntity* object = working_graph.find_entity_by_label(triple.object_label);
                if (object != nullptr) {
                    record_entity_delta(object->id, triple.object_label, true, false);
                }
            }

            const KnowledgeTriple* applied = working_graph.find_triple(id);
            if (applied != nullptr) {
                record_triple_delta(
                    *applied,
                    triple.subject_label,
                    triple.relation,
                    triple.object_label,
                    created,
                    payload_changed
                );
            }
        }

        knowledge_graph_ = std::move(working_graph);
    }

    const bool task_journal_changed = task_journal_.apply(patch.task_records);
    result.state_changed =
        !patch.updates.empty() ||
        !patch.new_blobs.empty() ||
        !result.knowledge_graph_delta.empty() ||
        task_journal_changed;
    if (!result.state_changed) {
        return result;
    }

    current_state_.version.fetch_add(1U, std::memory_order_acq_rel);
    result.patch_log_offset = patch_log_.append(current_state_.version.load(std::memory_order_acquire), patch);
    return result;
}

const WorkflowState& StateStore::get_current_state() const {
    return current_state_;
}

WorkflowState& StateStore::mutable_state() {
    return current_state_;
}

const Value* StateStore::find(StateKey key) const {
    static thread_local Value last_found;
    last_found = current_state_.load(key);
    if (std::holds_alternative<std::monostate>(last_found)) {
        return nullptr;
    }
    return &last_found;
}

BlobStore& StateStore::blobs() noexcept {
    return blob_store_;
}

const BlobStore& StateStore::blobs() const noexcept {
    return blob_store_;
}

PatchLog& StateStore::patch_log() noexcept {
    return patch_log_;
}

const PatchLog& StateStore::patch_log() const noexcept {
    return patch_log_;
}

StringInterner& StateStore::strings() noexcept {
    return string_interner_;
}

const StringInterner& StateStore::strings() const noexcept {
    return string_interner_;
}

KnowledgeGraphStore& StateStore::knowledge_graph() noexcept {
    return knowledge_graph_;
}

const KnowledgeGraphStore& StateStore::knowledge_graph() const noexcept {
    return knowledge_graph_;
}

TaskJournal& StateStore::task_journal() noexcept {
    return task_journal_;
}

const TaskJournal& StateStore::task_journal() const noexcept {
    return task_journal_;
}

StateStore::SharedBacking StateStore::shared_backing_with(const StateStore& other) const noexcept {
    return SharedBacking{
        blob_store_.shares_storage_with(other.blob_store_),
        string_interner_.shares_storage_with(other.string_interner_),
        knowledge_graph_.shares_storage_with(other.knowledge_graph_)
    };
}

void StateStore::serialize(std::ostream& output) const {
    write_workflow_state(output, current_state_);
    blob_store_.serialize(output);
    patch_log_.serialize(output);
    string_interner_.serialize(output);
    knowledge_graph_.serialize(output);
    task_journal_.serialize(output);
}

StateStore StateStore::deserialize(std::istream& input) {
    StateStore store;
    store.current_state_ = read_workflow_state(input);
    store.blob_store_ = BlobStore::deserialize(input);
    store.patch_log_ = PatchLog::deserialize(input);
    store.string_interner_ = StringInterner::deserialize(input);
    store.knowledge_graph_ = KnowledgeGraphStore::deserialize(input);
    store.task_journal_ = TaskJournal::deserialize(input);
    return store;
}

} // namespace agentcore
