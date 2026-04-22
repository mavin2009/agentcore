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

bool values_equal(const Value& left, const Value& right) {
    if (left.index() != right.index()) {
        return false;
    }
    return left == right;
}

bool intelligence_task_blob_changed(
    const IntelligenceTask& task,
    const IntelligenceTaskWrite& write,
    uint32_t mask,
    const BlobStore& blobs
) {
    if ((write.field_mask & mask) == 0U) {
        return false;
    }
    if (mask == intelligence_fields::kTaskPayload) {
        return !blob_refs_equal(blobs, task.payload, write.payload);
    }
    return !blob_refs_equal(blobs, task.result, write.result);
}

uint32_t intelligence_task_changed_fields(
    const IntelligenceTask& task,
    const IntelligenceTaskWrite& write,
    const BlobStore& blobs
) {
    uint32_t changed = 0U;
    if ((write.field_mask & intelligence_fields::kTaskTitle) != 0U && task.title != write.title) {
        changed |= intelligence_fields::kTaskTitle;
    }
    if ((write.field_mask & intelligence_fields::kTaskOwner) != 0U && task.owner != write.owner) {
        changed |= intelligence_fields::kTaskOwner;
    }
    if (intelligence_task_blob_changed(task, write, intelligence_fields::kTaskPayload, blobs)) {
        changed |= intelligence_fields::kTaskPayload;
    }
    if (intelligence_task_blob_changed(task, write, intelligence_fields::kTaskResult, blobs)) {
        changed |= intelligence_fields::kTaskResult;
    }
    if ((write.field_mask & intelligence_fields::kTaskStatus) != 0U && task.status != write.status) {
        changed |= intelligence_fields::kTaskStatus;
    }
    if ((write.field_mask & intelligence_fields::kTaskPriority) != 0U && task.priority != write.priority) {
        changed |= intelligence_fields::kTaskPriority;
    }
    if ((write.field_mask & intelligence_fields::kTaskConfidence) != 0U && task.confidence != write.confidence) {
        changed |= intelligence_fields::kTaskConfidence;
    }
    return changed;
}

uint32_t intelligence_claim_changed_fields(
    const IntelligenceClaim& claim,
    const IntelligenceClaimWrite& write,
    const BlobStore& blobs
) {
    uint32_t changed = 0U;
    if ((write.field_mask & intelligence_fields::kClaimSubject) != 0U &&
        claim.subject_label != write.subject_label) {
        changed |= intelligence_fields::kClaimSubject;
    }
    if ((write.field_mask & intelligence_fields::kClaimRelation) != 0U &&
        claim.relation != write.relation) {
        changed |= intelligence_fields::kClaimRelation;
    }
    if ((write.field_mask & intelligence_fields::kClaimObject) != 0U &&
        claim.object_label != write.object_label) {
        changed |= intelligence_fields::kClaimObject;
    }
    if ((write.field_mask & intelligence_fields::kClaimStatement) != 0U &&
        !blob_refs_equal(blobs, claim.statement, write.statement)) {
        changed |= intelligence_fields::kClaimStatement;
    }
    if ((write.field_mask & intelligence_fields::kClaimStatus) != 0U && claim.status != write.status) {
        changed |= intelligence_fields::kClaimStatus;
    }
    if ((write.field_mask & intelligence_fields::kClaimConfidence) != 0U &&
        claim.confidence != write.confidence) {
        changed |= intelligence_fields::kClaimConfidence;
    }
    return changed;
}

uint32_t intelligence_evidence_changed_fields(
    const IntelligenceEvidence& evidence,
    const IntelligenceEvidenceWrite& write,
    const BlobStore& blobs
) {
    uint32_t changed = 0U;
    if ((write.field_mask & intelligence_fields::kEvidenceKind) != 0U && evidence.kind != write.kind) {
        changed |= intelligence_fields::kEvidenceKind;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceSource) != 0U &&
        evidence.source != write.source) {
        changed |= intelligence_fields::kEvidenceSource;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceContent) != 0U &&
        !blob_refs_equal(blobs, evidence.content, write.content)) {
        changed |= intelligence_fields::kEvidenceContent;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceTaskKey) != 0U &&
        evidence.task_key != write.task_key) {
        changed |= intelligence_fields::kEvidenceTaskKey;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceClaimKey) != 0U &&
        evidence.claim_key != write.claim_key) {
        changed |= intelligence_fields::kEvidenceClaimKey;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceConfidence) != 0U &&
        evidence.confidence != write.confidence) {
        changed |= intelligence_fields::kEvidenceConfidence;
    }
    return changed;
}

uint32_t intelligence_decision_changed_fields(
    const IntelligenceDecision& decision,
    const IntelligenceDecisionWrite& write,
    const BlobStore& blobs
) {
    uint32_t changed = 0U;
    if ((write.field_mask & intelligence_fields::kDecisionTaskKey) != 0U &&
        decision.task_key != write.task_key) {
        changed |= intelligence_fields::kDecisionTaskKey;
    }
    if ((write.field_mask & intelligence_fields::kDecisionClaimKey) != 0U &&
        decision.claim_key != write.claim_key) {
        changed |= intelligence_fields::kDecisionClaimKey;
    }
    if ((write.field_mask & intelligence_fields::kDecisionSummary) != 0U &&
        !blob_refs_equal(blobs, decision.summary, write.summary)) {
        changed |= intelligence_fields::kDecisionSummary;
    }
    if ((write.field_mask & intelligence_fields::kDecisionStatus) != 0U &&
        decision.status != write.status) {
        changed |= intelligence_fields::kDecisionStatus;
    }
    if ((write.field_mask & intelligence_fields::kDecisionConfidence) != 0U &&
        decision.confidence != write.confidence) {
        changed |= intelligence_fields::kDecisionConfidence;
    }
    return changed;
}

uint32_t intelligence_memory_changed_fields(
    const IntelligenceMemoryEntry& memory,
    const IntelligenceMemoryWrite& write,
    const BlobStore& blobs
) {
    uint32_t changed = 0U;
    if ((write.field_mask & intelligence_fields::kMemoryLayer) != 0U && memory.layer != write.layer) {
        changed |= intelligence_fields::kMemoryLayer;
    }
    if ((write.field_mask & intelligence_fields::kMemoryScope) != 0U && memory.scope != write.scope) {
        changed |= intelligence_fields::kMemoryScope;
    }
    if ((write.field_mask & intelligence_fields::kMemoryContent) != 0U &&
        !blob_refs_equal(blobs, memory.content, write.content)) {
        changed |= intelligence_fields::kMemoryContent;
    }
    if ((write.field_mask & intelligence_fields::kMemoryTaskKey) != 0U &&
        memory.task_key != write.task_key) {
        changed |= intelligence_fields::kMemoryTaskKey;
    }
    if ((write.field_mask & intelligence_fields::kMemoryClaimKey) != 0U &&
        memory.claim_key != write.claim_key) {
        changed |= intelligence_fields::kMemoryClaimKey;
    }
    if ((write.field_mask & intelligence_fields::kMemoryImportance) != 0U &&
        memory.importance != write.importance) {
        changed |= intelligence_fields::kMemoryImportance;
    }
    return changed;
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

void write_intelligence_patch(std::ostream& output, const IntelligencePatch& patch) {
    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.tasks.size()));
    for (const IntelligenceTaskWrite& task : patch.tasks) {
        write_pod(output, task.key);
        write_pod(output, task.title);
        write_pod(output, task.owner);
        write_blob_ref(output, task.payload);
        write_blob_ref(output, task.result);
        write_pod(output, static_cast<uint8_t>(task.status));
        write_pod(output, task.priority);
        write_pod(output, task.confidence);
        write_pod(output, task.field_mask);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.claims.size()));
    for (const IntelligenceClaimWrite& claim : patch.claims) {
        write_pod(output, claim.key);
        write_pod(output, claim.subject_label);
        write_pod(output, claim.relation);
        write_pod(output, claim.object_label);
        write_blob_ref(output, claim.statement);
        write_pod(output, static_cast<uint8_t>(claim.status));
        write_pod(output, claim.confidence);
        write_pod(output, claim.field_mask);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.evidence.size()));
    for (const IntelligenceEvidenceWrite& evidence : patch.evidence) {
        write_pod(output, evidence.key);
        write_pod(output, evidence.kind);
        write_pod(output, evidence.source);
        write_blob_ref(output, evidence.content);
        write_pod(output, evidence.task_key);
        write_pod(output, evidence.claim_key);
        write_pod(output, evidence.confidence);
        write_pod(output, evidence.field_mask);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.decisions.size()));
    for (const IntelligenceDecisionWrite& decision : patch.decisions) {
        write_pod(output, decision.key);
        write_pod(output, decision.task_key);
        write_pod(output, decision.claim_key);
        write_blob_ref(output, decision.summary);
        write_pod(output, static_cast<uint8_t>(decision.status));
        write_pod(output, decision.confidence);
        write_pod(output, decision.field_mask);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(patch.memories.size()));
    for (const IntelligenceMemoryWrite& memory : patch.memories) {
        write_pod(output, memory.key);
        write_pod(output, static_cast<uint8_t>(memory.layer));
        write_pod(output, memory.scope);
        write_blob_ref(output, memory.content);
        write_pod(output, memory.task_key);
        write_pod(output, memory.claim_key);
        write_pod(output, memory.importance);
        write_pod(output, memory.field_mask);
    }
}

IntelligencePatch read_intelligence_patch(std::istream& input) {
    IntelligencePatch patch;

    const uint64_t task_count = read_pod<uint64_t>(input);
    patch.tasks.reserve(static_cast<std::size_t>(task_count));
    for (uint64_t index = 0; index < task_count; ++index) {
        patch.tasks.push_back(IntelligenceTaskWrite{
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_blob_ref(input),
            read_blob_ref(input),
            static_cast<IntelligenceTaskStatus>(read_pod<uint8_t>(input)),
            read_pod<int32_t>(input),
            read_pod<float>(input),
            read_pod<uint32_t>(input)
        });
    }

    const uint64_t claim_count = read_pod<uint64_t>(input);
    patch.claims.reserve(static_cast<std::size_t>(claim_count));
    for (uint64_t index = 0; index < claim_count; ++index) {
        patch.claims.push_back(IntelligenceClaimWrite{
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_blob_ref(input),
            static_cast<IntelligenceClaimStatus>(read_pod<uint8_t>(input)),
            read_pod<float>(input),
            read_pod<uint32_t>(input)
        });
    }

    const uint64_t evidence_count = read_pod<uint64_t>(input);
    patch.evidence.reserve(static_cast<std::size_t>(evidence_count));
    for (uint64_t index = 0; index < evidence_count; ++index) {
        patch.evidence.push_back(IntelligenceEvidenceWrite{
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_blob_ref(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<float>(input),
            read_pod<uint32_t>(input)
        });
    }

    const uint64_t decision_count = read_pod<uint64_t>(input);
    patch.decisions.reserve(static_cast<std::size_t>(decision_count));
    for (uint64_t index = 0; index < decision_count; ++index) {
        patch.decisions.push_back(IntelligenceDecisionWrite{
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_blob_ref(input),
            static_cast<IntelligenceDecisionStatus>(read_pod<uint8_t>(input)),
            read_pod<float>(input),
            read_pod<uint32_t>(input)
        });
    }

    const uint64_t memory_count = read_pod<uint64_t>(input);
    patch.memories.reserve(static_cast<std::size_t>(memory_count));
    for (uint64_t index = 0; index < memory_count; ++index) {
        patch.memories.push_back(IntelligenceMemoryWrite{
            read_pod<InternedStringId>(input),
            static_cast<IntelligenceMemoryLayer>(read_pod<uint8_t>(input)),
            read_pod<InternedStringId>(input),
            read_blob_ref(input),
            read_pod<InternedStringId>(input),
            read_pod<InternedStringId>(input),
            read_pod<float>(input),
            read_pod<uint32_t>(input)
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
    write_intelligence_patch(output, patch.intelligence);
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
    patch.intelligence = read_intelligence_patch(input);
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
        write_pod(output, state.field_revision(static_cast<StateKey>(index)));
    }
}

WorkflowState read_workflow_state(std::istream& input) {
    WorkflowState state;
    const uint64_t version = read_pod<uint64_t>(input);
    const uint64_t field_count = read_pod<uint64_t>(input);
    state.resize(static_cast<std::size_t>(field_count));
    state.version.store(version, std::memory_order_release);
    for (uint64_t index = 0; index < field_count; ++index) {
        const StateKey key = static_cast<StateKey>(index);
        state.store(key, read_value(input));
        state.set_field_revision(key, read_pod<uint64_t>(input));
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
    revision_segments = other.revision_segments;
    total_capacity.store(other.total_capacity.load(std::memory_order_acquire), std::memory_order_release);
    version.store(other.version.load(std::memory_order_acquire), std::memory_order_release);
    return *this;
}

void WorkflowState::resize(std::size_t new_size) {
    std::size_t current_capacity = total_capacity.load(std::memory_order_acquire);
    while (current_capacity < new_size) {
        segments.emplace_back(std::make_shared<Segment>());
        revision_segments.emplace_back(std::make_shared<RevisionSegment>());
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

uint64_t WorkflowState::field_revision(StateKey key) const noexcept {
    const std::size_t segment_index = key / kSegmentSize;
    const std::size_t element_index = key % kSegmentSize;
    if (segment_index >= revision_segments.size()) {
        return 0U;
    }
    return revision_segments[segment_index]->values[element_index];
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

void WorkflowState::set_field_revision(StateKey key, uint64_t revision) noexcept {
    const std::size_t segment_index = key / kSegmentSize;
    const std::size_t element_index = key % kSegmentSize;
    if (segment_index >= revision_segments.size()) {
        resize(static_cast<std::size_t>(key) + 1U);
    }
    if (!revision_segments[segment_index].unique()) {
        revision_segments[segment_index] = std::make_shared<RevisionSegment>(*revision_segments[segment_index]);
    }
    revision_segments[segment_index]->values[element_index] = revision;
}

void WorkflowState::bump_field_revision(StateKey key) noexcept {
    const uint64_t current_revision = field_revision(key);
    set_field_revision(key, current_revision + 1U);
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
    intelligence_.clear();
    task_journal_.clear();
}

StateStore::StateStore(const StateStore& other)
    : current_state_(other.current_state_),
      blob_store_(other.blob_store_),
      patch_log_(other.patch_log_),
      string_interner_(other.string_interner_),
      knowledge_graph_(other.knowledge_graph_),
      intelligence_(other.intelligence_),
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
    intelligence_ = other.intelligence_;
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
        const Value existing_value = current_state_.load(update.key);
        if (values_equal(existing_value, update.value)) {
            continue;
        }
        current_state_.store(update.key, update.value);
        current_state_.bump_field_revision(update.key);
        if (std::find(result.changed_keys.begin(), result.changed_keys.end(), update.key) ==
            result.changed_keys.end()) {
            result.changed_keys.push_back(update.key);
        }
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

    if (!patch.intelligence.empty()) {
        IntelligenceStore working_intelligence = intelligence_;

        for (const IntelligenceTaskWrite& task : patch.intelligence.tasks) {
            const IntelligenceTask* existing = working_intelligence.find_task_by_key(task.key);
            const bool created = existing == nullptr;
            const uint32_t changed_fields = created
                ? task.field_mask
                : intelligence_task_changed_fields(*existing, task, blob_store_);
            if (!created && changed_fields == 0U) {
                continue;
            }
            const IntelligenceTaskId id = working_intelligence.upsert_task(task);
            result.intelligence_delta.tasks.push_back(IntelligenceDelta{
                id,
                task.key,
                created,
                changed_fields
            });
        }

        for (const IntelligenceClaimWrite& claim : patch.intelligence.claims) {
            const IntelligenceClaim* existing = working_intelligence.find_claim_by_key(claim.key);
            const bool created = existing == nullptr;
            const uint32_t changed_fields = created
                ? claim.field_mask
                : intelligence_claim_changed_fields(*existing, claim, blob_store_);
            if (!created && changed_fields == 0U) {
                continue;
            }
            const IntelligenceClaimId id = working_intelligence.upsert_claim(claim);
            result.intelligence_delta.claims.push_back(IntelligenceDelta{
                id,
                claim.key,
                created,
                changed_fields
            });
        }

        for (const IntelligenceEvidenceWrite& evidence : patch.intelligence.evidence) {
            const IntelligenceEvidence* existing = working_intelligence.find_evidence_by_key(evidence.key);
            const bool created = existing == nullptr;
            const uint32_t changed_fields = created
                ? evidence.field_mask
                : intelligence_evidence_changed_fields(*existing, evidence, blob_store_);
            if (!created && changed_fields == 0U) {
                continue;
            }
            const IntelligenceEvidenceId id = working_intelligence.upsert_evidence(evidence);
            result.intelligence_delta.evidence.push_back(IntelligenceDelta{
                id,
                evidence.key,
                created,
                changed_fields
            });
        }

        for (const IntelligenceDecisionWrite& decision : patch.intelligence.decisions) {
            const IntelligenceDecision* existing = working_intelligence.find_decision_by_key(decision.key);
            const bool created = existing == nullptr;
            const uint32_t changed_fields = created
                ? decision.field_mask
                : intelligence_decision_changed_fields(*existing, decision, blob_store_);
            if (!created && changed_fields == 0U) {
                continue;
            }
            const IntelligenceDecisionId id = working_intelligence.upsert_decision(decision);
            result.intelligence_delta.decisions.push_back(IntelligenceDelta{
                id,
                decision.key,
                created,
                changed_fields
            });
        }

        for (const IntelligenceMemoryWrite& memory : patch.intelligence.memories) {
            const IntelligenceMemoryEntry* existing = working_intelligence.find_memory_by_key(memory.key);
            const bool created = existing == nullptr;
            const uint32_t changed_fields = created
                ? memory.field_mask
                : intelligence_memory_changed_fields(*existing, memory, blob_store_);
            if (!created && changed_fields == 0U) {
                continue;
            }
            const IntelligenceMemoryId id = working_intelligence.upsert_memory(memory);
            result.intelligence_delta.memories.push_back(IntelligenceDelta{
                id,
                memory.key,
                created,
                changed_fields
            });
        }

        intelligence_ = std::move(working_intelligence);
    }

    const bool task_journal_changed = task_journal_.apply(patch.task_records);
    result.state_changed =
        !result.changed_keys.empty() ||
        !patch.new_blobs.empty() ||
        !result.knowledge_graph_delta.empty() ||
        !result.intelligence_delta.empty() ||
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

IntelligenceStore& StateStore::intelligence() noexcept {
    return intelligence_;
}

const IntelligenceStore& StateStore::intelligence() const noexcept {
    return intelligence_;
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
        knowledge_graph_.shares_storage_with(other.knowledge_graph_),
        intelligence_.shares_storage_with(other.intelligence_)
    };
}

void StateStore::serialize(std::ostream& output) const {
    write_workflow_state(output, current_state_);
    blob_store_.serialize(output);
    patch_log_.serialize(output);
    string_interner_.serialize(output);
    knowledge_graph_.serialize(output);
    intelligence_.serialize(output);
    task_journal_.serialize(output);
}

StateStore StateStore::deserialize(std::istream& input) {
    StateStore store;
    store.current_state_ = read_workflow_state(input);
    store.blob_store_ = BlobStore::deserialize(input);
    store.patch_log_ = PatchLog::deserialize(input);
    store.string_interner_ = StringInterner::deserialize(input);
    store.knowledge_graph_ = KnowledgeGraphStore::deserialize(input);
    store.intelligence_ = IntelligenceStore::deserialize(input);
    store.task_journal_ = TaskJournal::deserialize(input);
    return store;
}

} // namespace agentcore
