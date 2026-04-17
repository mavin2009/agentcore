#ifndef AGENTCORE_STATE_STORE_H
#define AGENTCORE_STATE_STORE_H

#include "agentcore/core/types.h"
#include "agentcore/state/knowledge_graph.h"
#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentcore {

struct WorkflowState {
    std::vector<Value> fields;         // indexed by StateKey
    uint64_t version{0};
};

struct FieldUpdate {
    StateKey key{0};
    Value value;
};

struct StatePatch {
    std::vector<FieldUpdate> updates;
    std::vector<BlobRef> new_blobs;
    KnowledgeGraphPatch knowledge_graph;
    uint32_t flags{0};

    [[nodiscard]] bool empty() const {
        return updates.empty() && new_blobs.empty() && knowledge_graph.empty();
    }
};

struct StateApplyResult {
    uint64_t patch_log_offset{0};
    bool state_changed{false};
    KnowledgeGraphDeltaSummary knowledge_graph_delta;
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
    [[nodiscard]] uint64_t append(uint64_t state_version, const StatePatch& patch);
    [[nodiscard]] const PatchLogEntry* find(uint64_t offset) const;
    [[nodiscard]] const std::vector<PatchLogEntry>& entries() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static PatchLog deserialize(std::istream& input);

private:
    std::vector<PatchLogEntry> entries_;
};

class StateStore {
public:
    explicit StateStore(std::size_t initial_field_count = 0);

    void reset(std::size_t initial_field_count = 0);
    void ensure_field_capacity(std::size_t field_count);
    [[nodiscard]] StateApplyResult apply_with_summary(const StatePatch& patch);
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
    struct SharedBacking {
        bool blobs{false};
        bool strings{false};
        bool knowledge_graph{false};
    };
    [[nodiscard]] SharedBacking shared_backing_with(const StateStore& other) const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static StateStore deserialize(std::istream& input);

private:
    WorkflowState current_state_;
    BlobStore blob_store_;
    PatchLog patch_log_;
    StringInterner string_interner_;
    KnowledgeGraphStore knowledge_graph_;
};

} // namespace agentcore

#endif // AGENTCORE_STATE_STORE_H
