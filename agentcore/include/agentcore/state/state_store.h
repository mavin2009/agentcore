#ifndef AGENTCORE_STATE_STORE_H
#define AGENTCORE_STATE_STORE_H

#include "agentcore/core/types.h"
#include "agentcore/state/intelligence/model.h"
#include "agentcore/state/journal/task_journal.h"
#include "agentcore/state/knowledge_graph.h"
#include <atomic>
#include <mutex>
#include <array>
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentcore {

struct WorkflowState {
    static constexpr std::size_t kSegmentSize = 64;
    struct Segment {
        std::array<Value, kSegmentSize> values;

        Segment() {
            values.fill(std::monostate{});
        }
    };

    struct RevisionSegment {
        std::array<uint64_t, kSegmentSize> values;

        RevisionSegment() {
            values.fill(0U);
        }
    };

    std::vector<std::shared_ptr<Segment>> segments;
    std::vector<std::shared_ptr<RevisionSegment>> revision_segments;
    std::atomic<std::size_t> total_capacity{0};
    std::atomic<uint64_t> version{0};

    WorkflowState() = default;
    WorkflowState(const WorkflowState& other);
    WorkflowState& operator=(const WorkflowState& other);
    WorkflowState(WorkflowState&&) noexcept = default;
    WorkflowState& operator=(WorkflowState&&) noexcept = default;

    void resize(std::size_t new_size);
    [[nodiscard]] std::size_t size() const noexcept { return total_capacity.load(std::memory_order_acquire); }
    [[nodiscard]] Value load(StateKey key) const noexcept;
    [[nodiscard]] uint64_t field_revision(StateKey key) const noexcept;
    void store(StateKey key, Value value) noexcept;
    void set_field_revision(StateKey key, uint64_t revision) noexcept;
    void bump_field_revision(StateKey key) noexcept;
};

struct FieldUpdate {
    StateKey key{0};
    Value value;
};

struct StatePatch {
    std::vector<FieldUpdate> updates;
    std::vector<BlobRef> new_blobs;
    std::vector<TaskRecord> task_records;
    KnowledgeGraphPatch knowledge_graph;
    IntelligencePatch intelligence;
    uint32_t flags{0};

    [[nodiscard]] bool empty() const {
        return updates.empty() &&
            new_blobs.empty() &&
            task_records.empty() &&
            knowledge_graph.empty() &&
            intelligence.empty();
    }
};

struct StateApplyResult {
    uint64_t patch_log_offset{0};
    bool state_changed{false};
    std::vector<StateKey> changed_keys;
    KnowledgeGraphDeltaSummary knowledge_graph_delta;
    IntelligenceDeltaSummary intelligence_delta;
};

class StringInterner {
public:
    StringInterner();

    [[nodiscard]] InternedStringId intern(std::string_view value);
    [[nodiscard]] std::string_view resolve(InternedStringId id) const;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool shares_storage_with(const StringInterner& other) const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static StringInterner deserialize(std::istream& input);

private:
    struct Storage;

    void ensure_unique();

    std::shared_ptr<Storage> storage_;
};

class BlobStore {
public:
    BlobStore();

    [[nodiscard]] BlobRef append(const std::byte* bytes, std::size_t size);
    [[nodiscard]] BlobRef append_string(std::string_view text);
    [[nodiscard]] std::vector<std::byte> read_bytes(BlobRef ref) const;
    [[nodiscard]] std::string_view read_string(BlobRef ref) const;
    [[nodiscard]] std::pair<const std::byte*, std::size_t> read_buffer(BlobRef ref) const;
    [[nodiscard]] std::string_view read_string_view(BlobRef ref) const;
    [[nodiscard]] std::size_t size_bytes() const noexcept;
    [[nodiscard]] bool shares_storage_with(const BlobStore& other) const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static BlobStore deserialize(std::istream& input);

private:
    struct Storage;

    void ensure_unique();

    std::shared_ptr<Storage> storage_;
};

struct PatchLogEntry {
    uint64_t offset{0};
    uint64_t state_version{0};
    StatePatch patch;
};

class PatchLog {
public:
    PatchLog();
    uint64_t append(uint64_t state_version, const StatePatch& patch);
    [[nodiscard]] const PatchLogEntry* find(uint64_t offset) const;
    [[nodiscard]] std::vector<PatchLogEntry> entries() const;
    [[nodiscard]] std::vector<PatchLogEntry> entries_from(uint64_t offset) const;
    [[nodiscard]] std::size_t size() const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static PatchLog deserialize(std::istream& input);

    PatchLog(const PatchLog& other);
    PatchLog& operator=(const PatchLog& other);
    PatchLog(PatchLog&&) noexcept = default;
    PatchLog& operator=(PatchLog&&) noexcept = default;

private:
    struct Storage;

    void ensure_unique();

    std::shared_ptr<Storage> storage_;
};

class StateStore {
public:
    explicit StateStore(std::size_t initial_field_count = 0);

    void reset(std::size_t initial_field_count = 0);
    void ensure_field_capacity(std::size_t field_count);
    [[nodiscard]] StateApplyResult apply_with_summary(const StatePatch& patch);
    [[nodiscard]] StateApplyResult apply_with_summary(
        const StatePatch& state_patch,
        const StatePatch& log_patch
    );
    [[nodiscard]] uint64_t apply(const StatePatch& patch);
    [[nodiscard]] const WorkflowState& get_current_state() const;
    [[nodiscard]] WorkflowState& mutable_state();
    [[nodiscard]] const Value* find(StateKey key) const;
    [[nodiscard]] BlobStore& blobs() noexcept;
    [[nodiscard]] const BlobStore& blobs() const noexcept;
    [[nodiscard]] PatchLog& patch_log() noexcept;
    [[nodiscard]] const PatchLog& patch_log() const noexcept;
    [[nodiscard]] StringInterner& strings() noexcept;
    [[nodiscard]] const StringInterner& strings() const noexcept;
    [[nodiscard]] KnowledgeGraphStore& knowledge_graph() noexcept;
    [[nodiscard]] const KnowledgeGraphStore& knowledge_graph() const noexcept;
    [[nodiscard]] IntelligenceStore& intelligence() noexcept;
    [[nodiscard]] const IntelligenceStore& intelligence() const noexcept;
    [[nodiscard]] TaskJournal& task_journal() noexcept;
    [[nodiscard]] const TaskJournal& task_journal() const noexcept;
    struct SharedBacking {
        bool blobs{false};
        bool strings{false};
        bool knowledge_graph{false};
        bool intelligence{false};
    };
    [[nodiscard]] SharedBacking shared_backing_with(const StateStore& other) const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static StateStore deserialize(std::istream& input);

    StateStore(const StateStore& other);
    StateStore& operator=(const StateStore& other);
    StateStore(StateStore&&) noexcept = default;
    StateStore& operator=(StateStore&&) noexcept = default;

private:
    WorkflowState current_state_;
    BlobStore blob_store_;
    PatchLog patch_log_;
    StringInterner string_interner_;
    KnowledgeGraphStore knowledge_graph_;
    IntelligenceStore intelligence_;
    TaskJournal task_journal_;
};

} // namespace agentcore

#endif // AGENTCORE_STATE_STORE_H
