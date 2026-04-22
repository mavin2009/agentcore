#ifndef AGENTCORE_STATE_INTELLIGENCE_MODEL_H
#define AGENTCORE_STATE_INTELLIGENCE_MODEL_H

#include "agentcore/core/types.h"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <vector>

namespace agentcore {

using IntelligenceTaskId = uint32_t;
using IntelligenceClaimId = uint32_t;
using IntelligenceEvidenceId = uint32_t;
using IntelligenceDecisionId = uint32_t;
using IntelligenceMemoryId = uint32_t;

enum class IntelligenceTaskStatus : uint8_t {
    Open = 0,
    InProgress = 1,
    Blocked = 2,
    Completed = 3,
    Cancelled = 4
};

enum class IntelligenceClaimStatus : uint8_t {
    Proposed = 0,
    Supported = 1,
    Disputed = 2,
    Confirmed = 3,
    Rejected = 4
};

enum class IntelligenceDecisionStatus : uint8_t {
    Pending = 0,
    Selected = 1,
    Superseded = 2,
    Rejected = 3
};

enum class IntelligenceMemoryLayer : uint8_t {
    Working = 0,
    Episodic = 1,
    Semantic = 2,
    Procedural = 3
};

[[nodiscard]] const char* intelligence_task_status_name(IntelligenceTaskStatus status) noexcept;
[[nodiscard]] const char* intelligence_claim_status_name(IntelligenceClaimStatus status) noexcept;
[[nodiscard]] const char* intelligence_decision_status_name(IntelligenceDecisionStatus status) noexcept;
[[nodiscard]] const char* intelligence_memory_layer_name(IntelligenceMemoryLayer layer) noexcept;

[[nodiscard]] std::optional<IntelligenceTaskStatus> parse_intelligence_task_status(std::string_view name) noexcept;
[[nodiscard]] std::optional<IntelligenceClaimStatus> parse_intelligence_claim_status(std::string_view name) noexcept;
[[nodiscard]] std::optional<IntelligenceDecisionStatus> parse_intelligence_decision_status(std::string_view name) noexcept;
[[nodiscard]] std::optional<IntelligenceMemoryLayer> parse_intelligence_memory_layer(std::string_view name) noexcept;

namespace intelligence_fields {

constexpr uint32_t kTaskTitle = 1U << 0;
constexpr uint32_t kTaskOwner = 1U << 1;
constexpr uint32_t kTaskPayload = 1U << 2;
constexpr uint32_t kTaskResult = 1U << 3;
constexpr uint32_t kTaskStatus = 1U << 4;
constexpr uint32_t kTaskPriority = 1U << 5;
constexpr uint32_t kTaskConfidence = 1U << 6;

constexpr uint32_t kClaimSubject = 1U << 0;
constexpr uint32_t kClaimRelation = 1U << 1;
constexpr uint32_t kClaimObject = 1U << 2;
constexpr uint32_t kClaimStatement = 1U << 3;
constexpr uint32_t kClaimStatus = 1U << 4;
constexpr uint32_t kClaimConfidence = 1U << 5;

constexpr uint32_t kEvidenceKind = 1U << 0;
constexpr uint32_t kEvidenceSource = 1U << 1;
constexpr uint32_t kEvidenceContent = 1U << 2;
constexpr uint32_t kEvidenceTaskKey = 1U << 3;
constexpr uint32_t kEvidenceClaimKey = 1U << 4;
constexpr uint32_t kEvidenceConfidence = 1U << 5;

constexpr uint32_t kDecisionTaskKey = 1U << 0;
constexpr uint32_t kDecisionClaimKey = 1U << 1;
constexpr uint32_t kDecisionSummary = 1U << 2;
constexpr uint32_t kDecisionStatus = 1U << 3;
constexpr uint32_t kDecisionConfidence = 1U << 4;

constexpr uint32_t kMemoryLayer = 1U << 0;
constexpr uint32_t kMemoryScope = 1U << 1;
constexpr uint32_t kMemoryContent = 1U << 2;
constexpr uint32_t kMemoryTaskKey = 1U << 3;
constexpr uint32_t kMemoryClaimKey = 1U << 4;
constexpr uint32_t kMemoryImportance = 1U << 5;

} // namespace intelligence_fields

struct IntelligenceTask {
    IntelligenceTaskId id{0};
    InternedStringId key{0};
    InternedStringId title{0};
    InternedStringId owner{0};
    BlobRef payload{};
    BlobRef result{};
    IntelligenceTaskStatus status{IntelligenceTaskStatus::Open};
    int32_t priority{0};
    float confidence{0.0F};
};

struct IntelligenceClaim {
    IntelligenceClaimId id{0};
    InternedStringId key{0};
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    InternedStringId object_label{0};
    BlobRef statement{};
    IntelligenceClaimStatus status{IntelligenceClaimStatus::Proposed};
    float confidence{0.0F};
};

struct IntelligenceEvidence {
    IntelligenceEvidenceId id{0};
    InternedStringId key{0};
    InternedStringId kind{0};
    InternedStringId source{0};
    BlobRef content{};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    float confidence{0.0F};
};

struct IntelligenceDecision {
    IntelligenceDecisionId id{0};
    InternedStringId key{0};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    BlobRef summary{};
    IntelligenceDecisionStatus status{IntelligenceDecisionStatus::Pending};
    float confidence{0.0F};
};

struct IntelligenceMemoryEntry {
    IntelligenceMemoryId id{0};
    InternedStringId key{0};
    IntelligenceMemoryLayer layer{IntelligenceMemoryLayer::Working};
    InternedStringId scope{0};
    BlobRef content{};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    float importance{0.0F};
};

struct IntelligenceTaskWrite {
    InternedStringId key{0};
    InternedStringId title{0};
    InternedStringId owner{0};
    BlobRef payload{};
    BlobRef result{};
    IntelligenceTaskStatus status{IntelligenceTaskStatus::Open};
    int32_t priority{0};
    float confidence{0.0F};
    uint32_t field_mask{0};
};

struct IntelligenceClaimWrite {
    InternedStringId key{0};
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    InternedStringId object_label{0};
    BlobRef statement{};
    IntelligenceClaimStatus status{IntelligenceClaimStatus::Proposed};
    float confidence{0.0F};
    uint32_t field_mask{0};
};

struct IntelligenceEvidenceWrite {
    InternedStringId key{0};
    InternedStringId kind{0};
    InternedStringId source{0};
    BlobRef content{};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    float confidence{0.0F};
    uint32_t field_mask{0};
};

struct IntelligenceDecisionWrite {
    InternedStringId key{0};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    BlobRef summary{};
    IntelligenceDecisionStatus status{IntelligenceDecisionStatus::Pending};
    float confidence{0.0F};
    uint32_t field_mask{0};
};

struct IntelligenceMemoryWrite {
    InternedStringId key{0};
    IntelligenceMemoryLayer layer{IntelligenceMemoryLayer::Working};
    InternedStringId scope{0};
    BlobRef content{};
    InternedStringId task_key{0};
    InternedStringId claim_key{0};
    float importance{0.0F};
    uint32_t field_mask{0};
};

struct IntelligencePatch {
    std::vector<IntelligenceTaskWrite> tasks;
    std::vector<IntelligenceClaimWrite> claims;
    std::vector<IntelligenceEvidenceWrite> evidence;
    std::vector<IntelligenceDecisionWrite> decisions;
    std::vector<IntelligenceMemoryWrite> memories;

    [[nodiscard]] bool empty() const noexcept {
        return tasks.empty() &&
            claims.empty() &&
            evidence.empty() &&
            decisions.empty() &&
            memories.empty();
    }
};

struct IntelligenceDelta {
    uint32_t id{0};
    InternedStringId key{0};
    bool created{false};
    uint32_t updated_fields{0};
};

struct IntelligenceDeltaSummary {
    std::vector<IntelligenceDelta> tasks;
    std::vector<IntelligenceDelta> claims;
    std::vector<IntelligenceDelta> evidence;
    std::vector<IntelligenceDelta> decisions;
    std::vector<IntelligenceDelta> memories;

    [[nodiscard]] bool empty() const noexcept {
        return tasks.empty() &&
            claims.empty() &&
            evidence.empty() &&
            decisions.empty() &&
            memories.empty();
    }
};

struct IntelligenceSnapshot {
    std::vector<IntelligenceTask> tasks;
    std::vector<IntelligenceClaim> claims;
    std::vector<IntelligenceEvidence> evidence;
    std::vector<IntelligenceDecision> decisions;
    std::vector<IntelligenceMemoryEntry> memories;
};

class IntelligenceStore {
public:
    IntelligenceStore();

    IntelligenceTaskId upsert_task(const IntelligenceTaskWrite& write);
    IntelligenceClaimId upsert_claim(const IntelligenceClaimWrite& write);
    IntelligenceEvidenceId upsert_evidence(const IntelligenceEvidenceWrite& write);
    IntelligenceDecisionId upsert_decision(const IntelligenceDecisionWrite& write);
    IntelligenceMemoryId upsert_memory(const IntelligenceMemoryWrite& write);
    void apply(const IntelligencePatch& patch);

    [[nodiscard]] const IntelligenceTask* find_task(IntelligenceTaskId id) const;
    [[nodiscard]] const IntelligenceTask* find_task_by_key(InternedStringId key) const;
    [[nodiscard]] const IntelligenceClaim* find_claim(IntelligenceClaimId id) const;
    [[nodiscard]] const IntelligenceClaim* find_claim_by_key(InternedStringId key) const;
    [[nodiscard]] const IntelligenceEvidence* find_evidence(IntelligenceEvidenceId id) const;
    [[nodiscard]] const IntelligenceEvidence* find_evidence_by_key(InternedStringId key) const;
    [[nodiscard]] const IntelligenceDecision* find_decision(IntelligenceDecisionId id) const;
    [[nodiscard]] const IntelligenceDecision* find_decision_by_key(InternedStringId key) const;
    [[nodiscard]] const IntelligenceMemoryEntry* find_memory(IntelligenceMemoryId id) const;
    [[nodiscard]] const IntelligenceMemoryEntry* find_memory_by_key(InternedStringId key) const;

    [[nodiscard]] const std::vector<IntelligenceTask>& tasks() const noexcept;
    [[nodiscard]] const std::vector<IntelligenceClaim>& claims() const noexcept;
    [[nodiscard]] const std::vector<IntelligenceEvidence>& evidence() const noexcept;
    [[nodiscard]] const std::vector<IntelligenceDecision>& decisions() const noexcept;
    [[nodiscard]] const std::vector<IntelligenceMemoryEntry>& memories() const noexcept;
    [[nodiscard]] const std::vector<IntelligenceTaskId>& task_ids_for_owner(
        InternedStringId owner
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceTaskId>& task_ids_for_status(
        IntelligenceTaskStatus status
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceClaimId>& claim_ids_for_status(
        IntelligenceClaimStatus status
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceClaimId>& claim_ids_for_subject(
        InternedStringId subject_label
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceClaimId>& claim_ids_for_relation(
        InternedStringId relation
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceClaimId>& claim_ids_for_object(
        InternedStringId object_label
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceEvidenceId>& evidence_ids_for_source(
        InternedStringId source
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceEvidenceId>& evidence_ids_for_task_key(
        InternedStringId task_key
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceEvidenceId>& evidence_ids_for_claim_key(
        InternedStringId claim_key
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceDecisionId>& decision_ids_for_status(
        IntelligenceDecisionStatus status
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceDecisionId>& decision_ids_for_task_key(
        InternedStringId task_key
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceDecisionId>& decision_ids_for_claim_key(
        InternedStringId claim_key
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceMemoryId>& memory_ids_for_layer(
        IntelligenceMemoryLayer layer
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceMemoryId>& memory_ids_for_scope(
        InternedStringId scope
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceMemoryId>& memory_ids_for_task_key(
        InternedStringId task_key
    ) const noexcept;
    [[nodiscard]] const std::vector<IntelligenceMemoryId>& memory_ids_for_claim_key(
        InternedStringId claim_key
    ) const noexcept;

    [[nodiscard]] IntelligenceSnapshot snapshot() const;
    [[nodiscard]] std::size_t task_count() const noexcept;
    [[nodiscard]] std::size_t claim_count() const noexcept;
    [[nodiscard]] std::size_t evidence_count() const noexcept;
    [[nodiscard]] std::size_t decision_count() const noexcept;
    [[nodiscard]] std::size_t memory_count() const noexcept;
    [[nodiscard]] bool shares_storage_with(const IntelligenceStore& other) const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static IntelligenceStore deserialize(std::istream& input);
    void clear();

private:
    struct Storage;

    void ensure_unique();

    std::shared_ptr<Storage> storage_;
};

} // namespace agentcore

#endif // AGENTCORE_STATE_INTELLIGENCE_MODEL_H
