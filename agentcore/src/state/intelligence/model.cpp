#include "agentcore/state/intelligence/model.h"

#include <algorithm>
#include <array>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace agentcore {

namespace {

template <typename T>
void write_pod(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!output) {
        throw std::runtime_error("failed to write serialized intelligence state");
    }
}

template <typename T>
T read_pod(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!input) {
        throw std::runtime_error("failed to read serialized intelligence state");
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

template <typename EntryT>
const EntryT* find_by_id(const std::vector<EntryT>& entries, uint32_t id) {
    if (id == 0U) {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(id - 1U);
    if (index >= entries.size()) {
        return nullptr;
    }
    return &entries[index];
}

template <typename StatusT>
std::optional<StatusT> parse_named_enum(
    std::string_view name,
    const std::pair<std::string_view, StatusT>* values,
    std::size_t count
) noexcept {
    const auto iterator = std::find_if(
        values,
        values + count,
        [name](const auto& item) {
            return item.first == name;
        }
    );
    if (iterator == values + count) {
        return std::nullopt;
    }
    return iterator->second;
}

template <typename StatusT>
const char* enum_name(
    StatusT value,
    const std::pair<std::string_view, StatusT>* values,
    std::size_t count,
    const char* fallback
) noexcept {
    const auto iterator = std::find_if(
        values,
        values + count,
        [value](const auto& item) {
            return item.second == value;
        }
    );
    return iterator == values + count ? fallback : iterator->first.data();
}

constexpr std::pair<std::string_view, IntelligenceTaskStatus> kTaskStatuses[] = {
    {"open", IntelligenceTaskStatus::Open},
    {"in_progress", IntelligenceTaskStatus::InProgress},
    {"blocked", IntelligenceTaskStatus::Blocked},
    {"completed", IntelligenceTaskStatus::Completed},
    {"cancelled", IntelligenceTaskStatus::Cancelled},
};

constexpr std::pair<std::string_view, IntelligenceClaimStatus> kClaimStatuses[] = {
    {"proposed", IntelligenceClaimStatus::Proposed},
    {"supported", IntelligenceClaimStatus::Supported},
    {"disputed", IntelligenceClaimStatus::Disputed},
    {"confirmed", IntelligenceClaimStatus::Confirmed},
    {"rejected", IntelligenceClaimStatus::Rejected},
};

constexpr std::pair<std::string_view, IntelligenceDecisionStatus> kDecisionStatuses[] = {
    {"pending", IntelligenceDecisionStatus::Pending},
    {"selected", IntelligenceDecisionStatus::Selected},
    {"superseded", IntelligenceDecisionStatus::Superseded},
    {"rejected", IntelligenceDecisionStatus::Rejected},
};

constexpr std::pair<std::string_view, IntelligenceMemoryLayer> kMemoryLayers[] = {
    {"working", IntelligenceMemoryLayer::Working},
    {"episodic", IntelligenceMemoryLayer::Episodic},
    {"semantic", IntelligenceMemoryLayer::Semantic},
    {"procedural", IntelligenceMemoryLayer::Procedural},
};

template <typename EntryT, typename WriteT>
void ensure_non_empty_key(const WriteT& write, const char* message) {
    if (write.key == 0U) {
        throw std::invalid_argument(message);
    }
}

constexpr std::size_t kTaskStatusBucketCount =
    static_cast<std::size_t>(IntelligenceTaskStatus::Cancelled) + 1U;
constexpr std::size_t kClaimStatusBucketCount =
    static_cast<std::size_t>(IntelligenceClaimStatus::Rejected) + 1U;
constexpr std::size_t kDecisionStatusBucketCount =
    static_cast<std::size_t>(IntelligenceDecisionStatus::Rejected) + 1U;
constexpr std::size_t kMemoryLayerBucketCount =
    static_cast<std::size_t>(IntelligenceMemoryLayer::Procedural) + 1U;

constexpr std::size_t bucket_index(IntelligenceTaskStatus status) noexcept {
    return static_cast<std::size_t>(status);
}

constexpr std::size_t bucket_index(IntelligenceClaimStatus status) noexcept {
    return static_cast<std::size_t>(status);
}

constexpr std::size_t bucket_index(IntelligenceDecisionStatus status) noexcept {
    return static_cast<std::size_t>(status);
}

constexpr std::size_t bucket_index(IntelligenceMemoryLayer layer) noexcept {
    return static_cast<std::size_t>(layer);
}

template <typename IdT>
void insert_sorted_id(std::vector<IdT>& ids, IdT id) {
    const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
    if (iterator == ids.end() || *iterator != id) {
        ids.insert(iterator, id);
    }
}

template <typename IdT>
void erase_sorted_id(std::vector<IdT>& ids, IdT id) {
    const auto iterator = std::lower_bound(ids.begin(), ids.end(), id);
    if (iterator != ids.end() && *iterator == id) {
        ids.erase(iterator);
    }
}

template <typename IdT>
void add_index_entry(
    std::unordered_map<InternedStringId, std::vector<IdT>>& index,
    InternedStringId key,
    IdT id
) {
    if (key == 0U) {
        return;
    }
    insert_sorted_id(index[key], id);
}

template <typename IdT>
void remove_index_entry(
    std::unordered_map<InternedStringId, std::vector<IdT>>& index,
    InternedStringId key,
    IdT id
) {
    if (key == 0U) {
        return;
    }

    const auto iterator = index.find(key);
    if (iterator == index.end()) {
        return;
    }
    erase_sorted_id(iterator->second, id);
    if (iterator->second.empty()) {
        index.erase(iterator);
    }
}

template <typename IdT>
const std::vector<IdT>& index_view(
    const std::unordered_map<InternedStringId, std::vector<IdT>>& index,
    InternedStringId key
) noexcept {
    static const std::vector<IdT> kEmpty;
    const auto iterator = index.find(key);
    if (iterator == index.end()) {
        return kEmpty;
    }
    return iterator->second;
}

} // namespace

const char* intelligence_task_status_name(IntelligenceTaskStatus status) noexcept {
    return enum_name(status, kTaskStatuses, std::size(kTaskStatuses), "open");
}

const char* intelligence_claim_status_name(IntelligenceClaimStatus status) noexcept {
    return enum_name(status, kClaimStatuses, std::size(kClaimStatuses), "proposed");
}

const char* intelligence_decision_status_name(IntelligenceDecisionStatus status) noexcept {
    return enum_name(status, kDecisionStatuses, std::size(kDecisionStatuses), "pending");
}

const char* intelligence_memory_layer_name(IntelligenceMemoryLayer layer) noexcept {
    return enum_name(layer, kMemoryLayers, std::size(kMemoryLayers), "working");
}

std::optional<IntelligenceTaskStatus> parse_intelligence_task_status(std::string_view name) noexcept {
    return parse_named_enum(name, kTaskStatuses, std::size(kTaskStatuses));
}

std::optional<IntelligenceClaimStatus> parse_intelligence_claim_status(std::string_view name) noexcept {
    return parse_named_enum(name, kClaimStatuses, std::size(kClaimStatuses));
}

std::optional<IntelligenceDecisionStatus> parse_intelligence_decision_status(std::string_view name) noexcept {
    return parse_named_enum(name, kDecisionStatuses, std::size(kDecisionStatuses));
}

std::optional<IntelligenceMemoryLayer> parse_intelligence_memory_layer(std::string_view name) noexcept {
    return parse_named_enum(name, kMemoryLayers, std::size(kMemoryLayers));
}

struct IntelligenceStore::Storage {
    std::vector<IntelligenceTask> tasks;
    std::unordered_map<InternedStringId, IntelligenceTaskId> task_ids_by_key;
    std::unordered_map<InternedStringId, std::vector<IntelligenceTaskId>> task_ids_by_owner;
    std::array<std::vector<IntelligenceTaskId>, kTaskStatusBucketCount> task_ids_by_status;

    std::vector<IntelligenceClaim> claims;
    std::unordered_map<InternedStringId, IntelligenceClaimId> claim_ids_by_key;
    std::array<std::vector<IntelligenceClaimId>, kClaimStatusBucketCount> claim_ids_by_status;
    std::unordered_map<InternedStringId, std::vector<IntelligenceClaimId>> claim_ids_by_subject;
    std::unordered_map<InternedStringId, std::vector<IntelligenceClaimId>> claim_ids_by_relation;
    std::unordered_map<InternedStringId, std::vector<IntelligenceClaimId>> claim_ids_by_object;

    std::vector<IntelligenceEvidence> evidence;
    std::unordered_map<InternedStringId, IntelligenceEvidenceId> evidence_ids_by_key;
    std::unordered_map<InternedStringId, std::vector<IntelligenceEvidenceId>> evidence_ids_by_source;
    std::unordered_map<InternedStringId, std::vector<IntelligenceEvidenceId>> evidence_ids_by_task_key;
    std::unordered_map<InternedStringId, std::vector<IntelligenceEvidenceId>> evidence_ids_by_claim_key;

    std::vector<IntelligenceDecision> decisions;
    std::unordered_map<InternedStringId, IntelligenceDecisionId> decision_ids_by_key;
    std::array<std::vector<IntelligenceDecisionId>, kDecisionStatusBucketCount> decision_ids_by_status;
    std::unordered_map<InternedStringId, std::vector<IntelligenceDecisionId>> decision_ids_by_task_key;
    std::unordered_map<InternedStringId, std::vector<IntelligenceDecisionId>> decision_ids_by_claim_key;

    std::vector<IntelligenceMemoryEntry> memories;
    std::unordered_map<InternedStringId, IntelligenceMemoryId> memory_ids_by_key;
    std::array<std::vector<IntelligenceMemoryId>, kMemoryLayerBucketCount> memory_ids_by_layer;
    std::unordered_map<InternedStringId, std::vector<IntelligenceMemoryId>> memory_ids_by_scope;
    std::unordered_map<InternedStringId, std::vector<IntelligenceMemoryId>> memory_ids_by_task_key;
    std::unordered_map<InternedStringId, std::vector<IntelligenceMemoryId>> memory_ids_by_claim_key;

    void index_task(const IntelligenceTask& task);
    void reindex_task(const IntelligenceTask& before, const IntelligenceTask& after);
    void index_claim(const IntelligenceClaim& claim);
    void reindex_claim(const IntelligenceClaim& before, const IntelligenceClaim& after);
    void index_evidence(const IntelligenceEvidence& evidence);
    void reindex_evidence(const IntelligenceEvidence& before, const IntelligenceEvidence& after);
    void index_decision(const IntelligenceDecision& decision);
    void reindex_decision(const IntelligenceDecision& before, const IntelligenceDecision& after);
    void index_memory(const IntelligenceMemoryEntry& memory);
    void reindex_memory(const IntelligenceMemoryEntry& before, const IntelligenceMemoryEntry& after);
};

void IntelligenceStore::Storage::index_task(const IntelligenceTask& task) {
    add_index_entry(task_ids_by_owner, task.owner, task.id);
    insert_sorted_id(task_ids_by_status[bucket_index(task.status)], task.id);
}

void IntelligenceStore::Storage::reindex_task(const IntelligenceTask& before, const IntelligenceTask& after) {
    if (before.owner != after.owner) {
        remove_index_entry(task_ids_by_owner, before.owner, before.id);
        add_index_entry(task_ids_by_owner, after.owner, after.id);
    }
    if (before.status != after.status) {
        erase_sorted_id(task_ids_by_status[bucket_index(before.status)], before.id);
        insert_sorted_id(task_ids_by_status[bucket_index(after.status)], after.id);
    }
}

void IntelligenceStore::Storage::index_claim(const IntelligenceClaim& claim) {
    insert_sorted_id(claim_ids_by_status[bucket_index(claim.status)], claim.id);
    add_index_entry(claim_ids_by_subject, claim.subject_label, claim.id);
    add_index_entry(claim_ids_by_relation, claim.relation, claim.id);
    add_index_entry(claim_ids_by_object, claim.object_label, claim.id);
}

void IntelligenceStore::Storage::reindex_claim(const IntelligenceClaim& before, const IntelligenceClaim& after) {
    if (before.status != after.status) {
        erase_sorted_id(claim_ids_by_status[bucket_index(before.status)], before.id);
        insert_sorted_id(claim_ids_by_status[bucket_index(after.status)], after.id);
    }
    if (before.subject_label != after.subject_label) {
        remove_index_entry(claim_ids_by_subject, before.subject_label, before.id);
        add_index_entry(claim_ids_by_subject, after.subject_label, after.id);
    }
    if (before.relation != after.relation) {
        remove_index_entry(claim_ids_by_relation, before.relation, before.id);
        add_index_entry(claim_ids_by_relation, after.relation, after.id);
    }
    if (before.object_label != after.object_label) {
        remove_index_entry(claim_ids_by_object, before.object_label, before.id);
        add_index_entry(claim_ids_by_object, after.object_label, after.id);
    }
}

void IntelligenceStore::Storage::index_evidence(const IntelligenceEvidence& evidence) {
    add_index_entry(evidence_ids_by_source, evidence.source, evidence.id);
    add_index_entry(evidence_ids_by_task_key, evidence.task_key, evidence.id);
    add_index_entry(evidence_ids_by_claim_key, evidence.claim_key, evidence.id);
}

void IntelligenceStore::Storage::reindex_evidence(
    const IntelligenceEvidence& before,
    const IntelligenceEvidence& after
) {
    if (before.source != after.source) {
        remove_index_entry(evidence_ids_by_source, before.source, before.id);
        add_index_entry(evidence_ids_by_source, after.source, after.id);
    }
    if (before.task_key != after.task_key) {
        remove_index_entry(evidence_ids_by_task_key, before.task_key, before.id);
        add_index_entry(evidence_ids_by_task_key, after.task_key, after.id);
    }
    if (before.claim_key != after.claim_key) {
        remove_index_entry(evidence_ids_by_claim_key, before.claim_key, before.id);
        add_index_entry(evidence_ids_by_claim_key, after.claim_key, after.id);
    }
}

void IntelligenceStore::Storage::index_decision(const IntelligenceDecision& decision) {
    insert_sorted_id(decision_ids_by_status[bucket_index(decision.status)], decision.id);
    add_index_entry(decision_ids_by_task_key, decision.task_key, decision.id);
    add_index_entry(decision_ids_by_claim_key, decision.claim_key, decision.id);
}

void IntelligenceStore::Storage::reindex_decision(
    const IntelligenceDecision& before,
    const IntelligenceDecision& after
) {
    if (before.status != after.status) {
        erase_sorted_id(decision_ids_by_status[bucket_index(before.status)], before.id);
        insert_sorted_id(decision_ids_by_status[bucket_index(after.status)], after.id);
    }
    if (before.task_key != after.task_key) {
        remove_index_entry(decision_ids_by_task_key, before.task_key, before.id);
        add_index_entry(decision_ids_by_task_key, after.task_key, after.id);
    }
    if (before.claim_key != after.claim_key) {
        remove_index_entry(decision_ids_by_claim_key, before.claim_key, before.id);
        add_index_entry(decision_ids_by_claim_key, after.claim_key, after.id);
    }
}

void IntelligenceStore::Storage::index_memory(const IntelligenceMemoryEntry& memory) {
    insert_sorted_id(memory_ids_by_layer[bucket_index(memory.layer)], memory.id);
    add_index_entry(memory_ids_by_scope, memory.scope, memory.id);
    add_index_entry(memory_ids_by_task_key, memory.task_key, memory.id);
    add_index_entry(memory_ids_by_claim_key, memory.claim_key, memory.id);
}

void IntelligenceStore::Storage::reindex_memory(
    const IntelligenceMemoryEntry& before,
    const IntelligenceMemoryEntry& after
) {
    if (before.layer != after.layer) {
        erase_sorted_id(memory_ids_by_layer[bucket_index(before.layer)], before.id);
        insert_sorted_id(memory_ids_by_layer[bucket_index(after.layer)], after.id);
    }
    if (before.scope != after.scope) {
        remove_index_entry(memory_ids_by_scope, before.scope, before.id);
        add_index_entry(memory_ids_by_scope, after.scope, after.id);
    }
    if (before.task_key != after.task_key) {
        remove_index_entry(memory_ids_by_task_key, before.task_key, before.id);
        add_index_entry(memory_ids_by_task_key, after.task_key, after.id);
    }
    if (before.claim_key != after.claim_key) {
        remove_index_entry(memory_ids_by_claim_key, before.claim_key, before.id);
        add_index_entry(memory_ids_by_claim_key, after.claim_key, after.id);
    }
}

IntelligenceStore::IntelligenceStore() : storage_(std::make_shared<Storage>()) {}

void IntelligenceStore::ensure_unique() {
    if (!storage_.unique()) {
        storage_ = std::make_shared<Storage>(*storage_);
    }
}

IntelligenceTaskId IntelligenceStore::upsert_task(const IntelligenceTaskWrite& write) {
    ensure_non_empty_key<IntelligenceTask>(write, "intelligence task key must not be empty");
    ensure_unique();

    bool created = false;
    auto iterator = storage_->task_ids_by_key.find(write.key);
    if (iterator == storage_->task_ids_by_key.end()) {
        const IntelligenceTaskId id = static_cast<IntelligenceTaskId>(storage_->tasks.size() + 1U);
        storage_->tasks.push_back(IntelligenceTask{.id = id, .key = write.key});
        storage_->task_ids_by_key.emplace(write.key, id);
        iterator = storage_->task_ids_by_key.find(write.key);
        created = true;
    }

    IntelligenceTask& task = storage_->tasks[static_cast<std::size_t>(iterator->second - 1U)];
    const IntelligenceTask before = task;
    if ((write.field_mask & intelligence_fields::kTaskTitle) != 0U) {
        task.title = write.title;
    }
    if ((write.field_mask & intelligence_fields::kTaskOwner) != 0U) {
        task.owner = write.owner;
    }
    if ((write.field_mask & intelligence_fields::kTaskPayload) != 0U) {
        task.payload = write.payload;
    }
    if ((write.field_mask & intelligence_fields::kTaskResult) != 0U) {
        task.result = write.result;
    }
    if ((write.field_mask & intelligence_fields::kTaskStatus) != 0U) {
        task.status = write.status;
    }
    if ((write.field_mask & intelligence_fields::kTaskPriority) != 0U) {
        task.priority = write.priority;
    }
    if ((write.field_mask & intelligence_fields::kTaskConfidence) != 0U) {
        task.confidence = write.confidence;
    }
    if (created) {
        storage_->index_task(task);
    } else {
        storage_->reindex_task(before, task);
    }
    return task.id;
}

IntelligenceClaimId IntelligenceStore::upsert_claim(const IntelligenceClaimWrite& write) {
    ensure_non_empty_key<IntelligenceClaim>(write, "intelligence claim key must not be empty");
    ensure_unique();

    bool created = false;
    auto iterator = storage_->claim_ids_by_key.find(write.key);
    if (iterator == storage_->claim_ids_by_key.end()) {
        const IntelligenceClaimId id = static_cast<IntelligenceClaimId>(storage_->claims.size() + 1U);
        storage_->claims.push_back(IntelligenceClaim{.id = id, .key = write.key});
        storage_->claim_ids_by_key.emplace(write.key, id);
        iterator = storage_->claim_ids_by_key.find(write.key);
        created = true;
    }

    IntelligenceClaim& claim = storage_->claims[static_cast<std::size_t>(iterator->second - 1U)];
    const IntelligenceClaim before = claim;
    if ((write.field_mask & intelligence_fields::kClaimSubject) != 0U) {
        claim.subject_label = write.subject_label;
    }
    if ((write.field_mask & intelligence_fields::kClaimRelation) != 0U) {
        claim.relation = write.relation;
    }
    if ((write.field_mask & intelligence_fields::kClaimObject) != 0U) {
        claim.object_label = write.object_label;
    }
    if ((write.field_mask & intelligence_fields::kClaimStatement) != 0U) {
        claim.statement = write.statement;
    }
    if ((write.field_mask & intelligence_fields::kClaimStatus) != 0U) {
        claim.status = write.status;
    }
    if ((write.field_mask & intelligence_fields::kClaimConfidence) != 0U) {
        claim.confidence = write.confidence;
    }
    if (created) {
        storage_->index_claim(claim);
    } else {
        storage_->reindex_claim(before, claim);
    }
    return claim.id;
}

IntelligenceEvidenceId IntelligenceStore::upsert_evidence(const IntelligenceEvidenceWrite& write) {
    ensure_non_empty_key<IntelligenceEvidence>(write, "intelligence evidence key must not be empty");
    ensure_unique();

    bool created = false;
    auto iterator = storage_->evidence_ids_by_key.find(write.key);
    if (iterator == storage_->evidence_ids_by_key.end()) {
        const IntelligenceEvidenceId id =
            static_cast<IntelligenceEvidenceId>(storage_->evidence.size() + 1U);
        storage_->evidence.push_back(IntelligenceEvidence{.id = id, .key = write.key});
        storage_->evidence_ids_by_key.emplace(write.key, id);
        iterator = storage_->evidence_ids_by_key.find(write.key);
        created = true;
    }

    IntelligenceEvidence& evidence =
        storage_->evidence[static_cast<std::size_t>(iterator->second - 1U)];
    const IntelligenceEvidence before = evidence;
    if ((write.field_mask & intelligence_fields::kEvidenceKind) != 0U) {
        evidence.kind = write.kind;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceSource) != 0U) {
        evidence.source = write.source;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceContent) != 0U) {
        evidence.content = write.content;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceTaskKey) != 0U) {
        evidence.task_key = write.task_key;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceClaimKey) != 0U) {
        evidence.claim_key = write.claim_key;
    }
    if ((write.field_mask & intelligence_fields::kEvidenceConfidence) != 0U) {
        evidence.confidence = write.confidence;
    }
    if (created) {
        storage_->index_evidence(evidence);
    } else {
        storage_->reindex_evidence(before, evidence);
    }
    return evidence.id;
}

IntelligenceDecisionId IntelligenceStore::upsert_decision(const IntelligenceDecisionWrite& write) {
    ensure_non_empty_key<IntelligenceDecision>(write, "intelligence decision key must not be empty");
    ensure_unique();

    bool created = false;
    auto iterator = storage_->decision_ids_by_key.find(write.key);
    if (iterator == storage_->decision_ids_by_key.end()) {
        const IntelligenceDecisionId id =
            static_cast<IntelligenceDecisionId>(storage_->decisions.size() + 1U);
        storage_->decisions.push_back(IntelligenceDecision{.id = id, .key = write.key});
        storage_->decision_ids_by_key.emplace(write.key, id);
        iterator = storage_->decision_ids_by_key.find(write.key);
        created = true;
    }

    IntelligenceDecision& decision =
        storage_->decisions[static_cast<std::size_t>(iterator->second - 1U)];
    const IntelligenceDecision before = decision;
    if ((write.field_mask & intelligence_fields::kDecisionTaskKey) != 0U) {
        decision.task_key = write.task_key;
    }
    if ((write.field_mask & intelligence_fields::kDecisionClaimKey) != 0U) {
        decision.claim_key = write.claim_key;
    }
    if ((write.field_mask & intelligence_fields::kDecisionSummary) != 0U) {
        decision.summary = write.summary;
    }
    if ((write.field_mask & intelligence_fields::kDecisionStatus) != 0U) {
        decision.status = write.status;
    }
    if ((write.field_mask & intelligence_fields::kDecisionConfidence) != 0U) {
        decision.confidence = write.confidence;
    }
    if (created) {
        storage_->index_decision(decision);
    } else {
        storage_->reindex_decision(before, decision);
    }
    return decision.id;
}

IntelligenceMemoryId IntelligenceStore::upsert_memory(const IntelligenceMemoryWrite& write) {
    ensure_non_empty_key<IntelligenceMemoryEntry>(write, "intelligence memory key must not be empty");
    ensure_unique();

    bool created = false;
    auto iterator = storage_->memory_ids_by_key.find(write.key);
    if (iterator == storage_->memory_ids_by_key.end()) {
        const IntelligenceMemoryId id =
            static_cast<IntelligenceMemoryId>(storage_->memories.size() + 1U);
        storage_->memories.push_back(IntelligenceMemoryEntry{.id = id, .key = write.key});
        storage_->memory_ids_by_key.emplace(write.key, id);
        iterator = storage_->memory_ids_by_key.find(write.key);
        created = true;
    }

    IntelligenceMemoryEntry& memory =
        storage_->memories[static_cast<std::size_t>(iterator->second - 1U)];
    const IntelligenceMemoryEntry before = memory;
    if ((write.field_mask & intelligence_fields::kMemoryLayer) != 0U) {
        memory.layer = write.layer;
    }
    if ((write.field_mask & intelligence_fields::kMemoryScope) != 0U) {
        memory.scope = write.scope;
    }
    if ((write.field_mask & intelligence_fields::kMemoryContent) != 0U) {
        memory.content = write.content;
    }
    if ((write.field_mask & intelligence_fields::kMemoryTaskKey) != 0U) {
        memory.task_key = write.task_key;
    }
    if ((write.field_mask & intelligence_fields::kMemoryClaimKey) != 0U) {
        memory.claim_key = write.claim_key;
    }
    if ((write.field_mask & intelligence_fields::kMemoryImportance) != 0U) {
        memory.importance = write.importance;
    }
    if (created) {
        storage_->index_memory(memory);
    } else {
        storage_->reindex_memory(before, memory);
    }
    return memory.id;
}

void IntelligenceStore::apply(const IntelligencePatch& patch) {
    for (const IntelligenceTaskWrite& write : patch.tasks) {
        upsert_task(write);
    }
    for (const IntelligenceClaimWrite& write : patch.claims) {
        upsert_claim(write);
    }
    for (const IntelligenceEvidenceWrite& write : patch.evidence) {
        upsert_evidence(write);
    }
    for (const IntelligenceDecisionWrite& write : patch.decisions) {
        upsert_decision(write);
    }
    for (const IntelligenceMemoryWrite& write : patch.memories) {
        upsert_memory(write);
    }
}

const IntelligenceTask* IntelligenceStore::find_task(IntelligenceTaskId id) const {
    return find_by_id(storage_->tasks, id);
}

const IntelligenceTask* IntelligenceStore::find_task_by_key(InternedStringId key) const {
    const auto iterator = storage_->task_ids_by_key.find(key);
    return iterator == storage_->task_ids_by_key.end() ? nullptr : find_task(iterator->second);
}

const IntelligenceClaim* IntelligenceStore::find_claim(IntelligenceClaimId id) const {
    return find_by_id(storage_->claims, id);
}

const IntelligenceClaim* IntelligenceStore::find_claim_by_key(InternedStringId key) const {
    const auto iterator = storage_->claim_ids_by_key.find(key);
    return iterator == storage_->claim_ids_by_key.end() ? nullptr : find_claim(iterator->second);
}

const IntelligenceEvidence* IntelligenceStore::find_evidence(IntelligenceEvidenceId id) const {
    return find_by_id(storage_->evidence, id);
}

const IntelligenceEvidence* IntelligenceStore::find_evidence_by_key(InternedStringId key) const {
    const auto iterator = storage_->evidence_ids_by_key.find(key);
    return iterator == storage_->evidence_ids_by_key.end() ? nullptr : find_evidence(iterator->second);
}

const IntelligenceDecision* IntelligenceStore::find_decision(IntelligenceDecisionId id) const {
    return find_by_id(storage_->decisions, id);
}

const IntelligenceDecision* IntelligenceStore::find_decision_by_key(InternedStringId key) const {
    const auto iterator = storage_->decision_ids_by_key.find(key);
    return iterator == storage_->decision_ids_by_key.end() ? nullptr : find_decision(iterator->second);
}

const IntelligenceMemoryEntry* IntelligenceStore::find_memory(IntelligenceMemoryId id) const {
    return find_by_id(storage_->memories, id);
}

const IntelligenceMemoryEntry* IntelligenceStore::find_memory_by_key(InternedStringId key) const {
    const auto iterator = storage_->memory_ids_by_key.find(key);
    return iterator == storage_->memory_ids_by_key.end() ? nullptr : find_memory(iterator->second);
}

const std::vector<IntelligenceTask>& IntelligenceStore::tasks() const noexcept {
    return storage_->tasks;
}

const std::vector<IntelligenceClaim>& IntelligenceStore::claims() const noexcept {
    return storage_->claims;
}

const std::vector<IntelligenceEvidence>& IntelligenceStore::evidence() const noexcept {
    return storage_->evidence;
}

const std::vector<IntelligenceDecision>& IntelligenceStore::decisions() const noexcept {
    return storage_->decisions;
}

const std::vector<IntelligenceMemoryEntry>& IntelligenceStore::memories() const noexcept {
    return storage_->memories;
}

const std::vector<IntelligenceTaskId>& IntelligenceStore::task_ids_for_owner(
    InternedStringId owner
) const noexcept {
    return index_view(storage_->task_ids_by_owner, owner);
}

const std::vector<IntelligenceTaskId>& IntelligenceStore::task_ids_for_status(
    IntelligenceTaskStatus status
) const noexcept {
    return storage_->task_ids_by_status[bucket_index(status)];
}

const std::vector<IntelligenceClaimId>& IntelligenceStore::claim_ids_for_status(
    IntelligenceClaimStatus status
) const noexcept {
    return storage_->claim_ids_by_status[bucket_index(status)];
}

const std::vector<IntelligenceClaimId>& IntelligenceStore::claim_ids_for_subject(
    InternedStringId subject_label
) const noexcept {
    return index_view(storage_->claim_ids_by_subject, subject_label);
}

const std::vector<IntelligenceClaimId>& IntelligenceStore::claim_ids_for_relation(
    InternedStringId relation
) const noexcept {
    return index_view(storage_->claim_ids_by_relation, relation);
}

const std::vector<IntelligenceClaimId>& IntelligenceStore::claim_ids_for_object(
    InternedStringId object_label
) const noexcept {
    return index_view(storage_->claim_ids_by_object, object_label);
}

const std::vector<IntelligenceEvidenceId>& IntelligenceStore::evidence_ids_for_source(
    InternedStringId source
) const noexcept {
    return index_view(storage_->evidence_ids_by_source, source);
}

const std::vector<IntelligenceEvidenceId>& IntelligenceStore::evidence_ids_for_task_key(
    InternedStringId task_key
) const noexcept {
    return index_view(storage_->evidence_ids_by_task_key, task_key);
}

const std::vector<IntelligenceEvidenceId>& IntelligenceStore::evidence_ids_for_claim_key(
    InternedStringId claim_key
) const noexcept {
    return index_view(storage_->evidence_ids_by_claim_key, claim_key);
}

const std::vector<IntelligenceDecisionId>& IntelligenceStore::decision_ids_for_status(
    IntelligenceDecisionStatus status
) const noexcept {
    return storage_->decision_ids_by_status[bucket_index(status)];
}

const std::vector<IntelligenceDecisionId>& IntelligenceStore::decision_ids_for_task_key(
    InternedStringId task_key
) const noexcept {
    return index_view(storage_->decision_ids_by_task_key, task_key);
}

const std::vector<IntelligenceDecisionId>& IntelligenceStore::decision_ids_for_claim_key(
    InternedStringId claim_key
) const noexcept {
    return index_view(storage_->decision_ids_by_claim_key, claim_key);
}

const std::vector<IntelligenceMemoryId>& IntelligenceStore::memory_ids_for_layer(
    IntelligenceMemoryLayer layer
) const noexcept {
    return storage_->memory_ids_by_layer[bucket_index(layer)];
}

const std::vector<IntelligenceMemoryId>& IntelligenceStore::memory_ids_for_scope(
    InternedStringId scope
) const noexcept {
    return index_view(storage_->memory_ids_by_scope, scope);
}

const std::vector<IntelligenceMemoryId>& IntelligenceStore::memory_ids_for_task_key(
    InternedStringId task_key
) const noexcept {
    return index_view(storage_->memory_ids_by_task_key, task_key);
}

const std::vector<IntelligenceMemoryId>& IntelligenceStore::memory_ids_for_claim_key(
    InternedStringId claim_key
) const noexcept {
    return index_view(storage_->memory_ids_by_claim_key, claim_key);
}

IntelligenceSnapshot IntelligenceStore::snapshot() const {
    return IntelligenceSnapshot{
        storage_->tasks,
        storage_->claims,
        storage_->evidence,
        storage_->decisions,
        storage_->memories
    };
}

std::size_t IntelligenceStore::task_count() const noexcept {
    return storage_->tasks.size();
}

std::size_t IntelligenceStore::claim_count() const noexcept {
    return storage_->claims.size();
}

std::size_t IntelligenceStore::evidence_count() const noexcept {
    return storage_->evidence.size();
}

std::size_t IntelligenceStore::decision_count() const noexcept {
    return storage_->decisions.size();
}

std::size_t IntelligenceStore::memory_count() const noexcept {
    return storage_->memories.size();
}

bool IntelligenceStore::shares_storage_with(const IntelligenceStore& other) const noexcept {
    return storage_ == other.storage_;
}

void IntelligenceStore::serialize(std::ostream& output) const {
    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->tasks.size()));
    for (const IntelligenceTask& task : storage_->tasks) {
        write_pod(output, task.id);
        write_pod(output, task.key);
        write_pod(output, task.title);
        write_pod(output, task.owner);
        write_blob_ref(output, task.payload);
        write_blob_ref(output, task.result);
        write_pod(output, static_cast<uint8_t>(task.status));
        write_pod(output, task.priority);
        write_pod(output, task.confidence);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->claims.size()));
    for (const IntelligenceClaim& claim : storage_->claims) {
        write_pod(output, claim.id);
        write_pod(output, claim.key);
        write_pod(output, claim.subject_label);
        write_pod(output, claim.relation);
        write_pod(output, claim.object_label);
        write_blob_ref(output, claim.statement);
        write_pod(output, static_cast<uint8_t>(claim.status));
        write_pod(output, claim.confidence);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->evidence.size()));
    for (const IntelligenceEvidence& evidence : storage_->evidence) {
        write_pod(output, evidence.id);
        write_pod(output, evidence.key);
        write_pod(output, evidence.kind);
        write_pod(output, evidence.source);
        write_blob_ref(output, evidence.content);
        write_pod(output, evidence.task_key);
        write_pod(output, evidence.claim_key);
        write_pod(output, evidence.confidence);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->decisions.size()));
    for (const IntelligenceDecision& decision : storage_->decisions) {
        write_pod(output, decision.id);
        write_pod(output, decision.key);
        write_pod(output, decision.task_key);
        write_pod(output, decision.claim_key);
        write_blob_ref(output, decision.summary);
        write_pod(output, static_cast<uint8_t>(decision.status));
        write_pod(output, decision.confidence);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->memories.size()));
    for (const IntelligenceMemoryEntry& memory : storage_->memories) {
        write_pod(output, memory.id);
        write_pod(output, memory.key);
        write_pod(output, static_cast<uint8_t>(memory.layer));
        write_pod(output, memory.scope);
        write_blob_ref(output, memory.content);
        write_pod(output, memory.task_key);
        write_pod(output, memory.claim_key);
        write_pod(output, memory.importance);
    }
}

IntelligenceStore IntelligenceStore::deserialize(std::istream& input) {
    IntelligenceStore store;
    store.storage_ = std::make_shared<Storage>();

    const uint64_t task_count = read_pod<uint64_t>(input);
    store.storage_->tasks.reserve(static_cast<std::size_t>(task_count));
    for (uint64_t index = 0; index < task_count; ++index) {
        IntelligenceTask task;
        task.id = read_pod<IntelligenceTaskId>(input);
        task.key = read_pod<InternedStringId>(input);
        task.title = read_pod<InternedStringId>(input);
        task.owner = read_pod<InternedStringId>(input);
        task.payload = read_blob_ref(input);
        task.result = read_blob_ref(input);
        task.status = static_cast<IntelligenceTaskStatus>(read_pod<uint8_t>(input));
        task.priority = read_pod<int32_t>(input);
        task.confidence = read_pod<float>(input);
        store.storage_->task_ids_by_key.emplace(task.key, task.id);
        store.storage_->tasks.push_back(task);
        store.storage_->index_task(task);
    }

    const uint64_t claim_count = read_pod<uint64_t>(input);
    store.storage_->claims.reserve(static_cast<std::size_t>(claim_count));
    for (uint64_t index = 0; index < claim_count; ++index) {
        IntelligenceClaim claim;
        claim.id = read_pod<IntelligenceClaimId>(input);
        claim.key = read_pod<InternedStringId>(input);
        claim.subject_label = read_pod<InternedStringId>(input);
        claim.relation = read_pod<InternedStringId>(input);
        claim.object_label = read_pod<InternedStringId>(input);
        claim.statement = read_blob_ref(input);
        claim.status = static_cast<IntelligenceClaimStatus>(read_pod<uint8_t>(input));
        claim.confidence = read_pod<float>(input);
        store.storage_->claim_ids_by_key.emplace(claim.key, claim.id);
        store.storage_->claims.push_back(claim);
        store.storage_->index_claim(claim);
    }

    const uint64_t evidence_count = read_pod<uint64_t>(input);
    store.storage_->evidence.reserve(static_cast<std::size_t>(evidence_count));
    for (uint64_t index = 0; index < evidence_count; ++index) {
        IntelligenceEvidence evidence;
        evidence.id = read_pod<IntelligenceEvidenceId>(input);
        evidence.key = read_pod<InternedStringId>(input);
        evidence.kind = read_pod<InternedStringId>(input);
        evidence.source = read_pod<InternedStringId>(input);
        evidence.content = read_blob_ref(input);
        evidence.task_key = read_pod<InternedStringId>(input);
        evidence.claim_key = read_pod<InternedStringId>(input);
        evidence.confidence = read_pod<float>(input);
        store.storage_->evidence_ids_by_key.emplace(evidence.key, evidence.id);
        store.storage_->evidence.push_back(evidence);
        store.storage_->index_evidence(evidence);
    }

    const uint64_t decision_count = read_pod<uint64_t>(input);
    store.storage_->decisions.reserve(static_cast<std::size_t>(decision_count));
    for (uint64_t index = 0; index < decision_count; ++index) {
        IntelligenceDecision decision;
        decision.id = read_pod<IntelligenceDecisionId>(input);
        decision.key = read_pod<InternedStringId>(input);
        decision.task_key = read_pod<InternedStringId>(input);
        decision.claim_key = read_pod<InternedStringId>(input);
        decision.summary = read_blob_ref(input);
        decision.status = static_cast<IntelligenceDecisionStatus>(read_pod<uint8_t>(input));
        decision.confidence = read_pod<float>(input);
        store.storage_->decision_ids_by_key.emplace(decision.key, decision.id);
        store.storage_->decisions.push_back(decision);
        store.storage_->index_decision(decision);
    }

    const uint64_t memory_count = read_pod<uint64_t>(input);
    store.storage_->memories.reserve(static_cast<std::size_t>(memory_count));
    for (uint64_t index = 0; index < memory_count; ++index) {
        IntelligenceMemoryEntry memory;
        memory.id = read_pod<IntelligenceMemoryId>(input);
        memory.key = read_pod<InternedStringId>(input);
        memory.layer = static_cast<IntelligenceMemoryLayer>(read_pod<uint8_t>(input));
        memory.scope = read_pod<InternedStringId>(input);
        memory.content = read_blob_ref(input);
        memory.task_key = read_pod<InternedStringId>(input);
        memory.claim_key = read_pod<InternedStringId>(input);
        memory.importance = read_pod<float>(input);
        store.storage_->memory_ids_by_key.emplace(memory.key, memory.id);
        store.storage_->memories.push_back(memory);
        store.storage_->index_memory(memory);
    }

    return store;
}

void IntelligenceStore::clear() {
    storage_ = std::make_shared<Storage>();
}

} // namespace agentcore
