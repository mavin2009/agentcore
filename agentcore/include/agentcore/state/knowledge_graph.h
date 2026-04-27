#ifndef AGENTCORE_KNOWLEDGE_GRAPH_H
#define AGENTCORE_KNOWLEDGE_GRAPH_H

#include "agentcore/core/types.h"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace agentcore {

using KnowledgeEntityId = uint32_t;
using KnowledgeTripleId = uint32_t;

struct KnowledgeEntity {
    KnowledgeEntityId id{0};
    InternedStringId label{0};
    BlobRef payload{};
};

struct KnowledgeTriple {
    KnowledgeTripleId id{0};
    KnowledgeEntityId subject{0};
    InternedStringId relation{0};
    KnowledgeEntityId object{0};
    BlobRef payload{};
};

struct KnowledgeEntityWrite {
    InternedStringId label{0};
    BlobRef payload{};
};

struct KnowledgeTripleWrite {
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    InternedStringId object_label{0};
    BlobRef payload{};
};

struct KnowledgeGraphPatch {
    std::vector<KnowledgeEntityWrite> entities;
    std::vector<KnowledgeTripleWrite> triples;

    [[nodiscard]] bool empty() const noexcept {
        return entities.empty() && triples.empty();
    }
};

struct KnowledgeEntityDelta {
    KnowledgeEntityId id{0};
    InternedStringId label{0};
    bool created{false};
    bool payload_changed{false};
};

struct KnowledgeTripleDelta {
    KnowledgeTripleId id{0};
    KnowledgeEntityId subject{0};
    InternedStringId subject_label{0};
    InternedStringId relation{0};
    KnowledgeEntityId object{0};
    InternedStringId object_label{0};
    bool created{false};
    bool payload_changed{false};
};

struct KnowledgeGraphDeltaSummary {
    std::vector<KnowledgeEntityDelta> entities;
    std::vector<KnowledgeTripleDelta> triples;

    [[nodiscard]] bool empty() const noexcept {
        return entities.empty() && triples.empty();
    }
};

class KnowledgeGraphStore {
public:
    KnowledgeGraphStore();

    KnowledgeEntityId upsert_entity(InternedStringId label, BlobRef payload = {});
    KnowledgeTripleId upsert_triple(
        InternedStringId subject_label,
        InternedStringId relation,
        InternedStringId object_label,
        BlobRef payload = {}
    );
    void apply(const KnowledgeGraphPatch& patch);

    [[nodiscard]] const KnowledgeEntity* find_entity(KnowledgeEntityId id) const;
    [[nodiscard]] const KnowledgeEntity* find_entity_by_label(InternedStringId label) const;
    [[nodiscard]] const KnowledgeTriple* find_triple(KnowledgeTripleId id) const;
    [[nodiscard]] const KnowledgeTriple* find_triple_by_labels(
        InternedStringId subject_label,
        InternedStringId relation,
        InternedStringId object_label
    ) const;

    [[nodiscard]] std::vector<const KnowledgeTriple*> outgoing(
        KnowledgeEntityId subject,
        std::optional<InternedStringId> relation = std::nullopt
    ) const;
    [[nodiscard]] std::vector<const KnowledgeTriple*> incoming(
        KnowledgeEntityId object,
        std::optional<InternedStringId> relation = std::nullopt
    ) const;
    [[nodiscard]] std::vector<const KnowledgeTriple*> match(
        std::optional<InternedStringId> subject_label,
        std::optional<InternedStringId> relation,
        std::optional<InternedStringId> object_label
    ) const;
    [[nodiscard]] std::vector<KnowledgeEntityId> neighbors(
        InternedStringId subject_label,
        std::optional<InternedStringId> relation = std::nullopt
    ) const;

    [[nodiscard]] std::size_t entity_count() const noexcept;
    [[nodiscard]] std::size_t triple_count() const noexcept;
    [[nodiscard]] bool shares_storage_with(const KnowledgeGraphStore& other) const noexcept;
    void serialize(std::ostream& output) const;
    [[nodiscard]] static KnowledgeGraphStore deserialize(std::istream& input);
    void clear();

private:
    struct Storage;

    struct TripleKey {
        KnowledgeEntityId subject{0};
        InternedStringId relation{0};
        KnowledgeEntityId object{0};

        bool operator==(const TripleKey& other) const noexcept {
            return subject == other.subject &&
                   relation == other.relation &&
                   object == other.object;
        }
    };

    struct TripleKeyHash {
        std::size_t operator()(const TripleKey& key) const noexcept;
    };

    using TripleIndex = std::unordered_map<uint32_t, std::vector<KnowledgeTripleId>>;

    struct ReasoningIndex;

    void ensure_unique();
    [[nodiscard]] const ReasoningIndex& reasoning_index() const;
    [[nodiscard]] KnowledgeEntityId resolve_or_create_entity(InternedStringId label);
    [[nodiscard]] std::vector<const KnowledgeTriple*> select_candidate_set(
        std::optional<KnowledgeEntityId> subject,
        std::optional<InternedStringId> relation,
        std::optional<KnowledgeEntityId> object
    ) const;

    std::shared_ptr<Storage> storage_;
};

} // namespace agentcore

#endif // AGENTCORE_KNOWLEDGE_GRAPH_H
