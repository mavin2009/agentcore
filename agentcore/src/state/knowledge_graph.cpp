#include "agentcore/state/knowledge_graph.h"

#include <algorithm>
#include <istream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace agentcore {

namespace {

template <typename T>
void write_pod(std::ostream& output, const T& value) {
    output.write(reinterpret_cast<const char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!output) {
        throw std::runtime_error("failed to write serialized knowledge graph");
    }
}

template <typename T>
T read_pod(std::istream& input) {
    T value{};
    input.read(reinterpret_cast<char*>(&value), static_cast<std::streamsize>(sizeof(T)));
    if (!input) {
        throw std::runtime_error("failed to read serialized knowledge graph");
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

} // namespace

struct KnowledgeGraphStore::ReasoningIndex {
    uint64_t version{0};
    std::size_t entity_count{0};
    std::size_t triple_count{0};
    std::vector<uint32_t> outgoing_offsets;
    std::vector<uint32_t> incoming_offsets;
    std::vector<KnowledgeTripleId> outgoing_triples;
    std::vector<KnowledgeTripleId> incoming_triples;
    std::vector<uint32_t> outgoing_degree;
    std::vector<uint32_t> incoming_degree;
    std::unordered_map<InternedStringId, uint32_t> relation_degree;
};

struct KnowledgeGraphStore::Storage {
    std::vector<KnowledgeEntity> entities;
    std::vector<KnowledgeTriple> triples;
    std::unordered_map<InternedStringId, KnowledgeEntityId> entity_by_label;
    std::unordered_map<TripleKey, KnowledgeTripleId, TripleKeyHash> triple_by_key;
    TripleIndex outgoing_index;
    TripleIndex incoming_index;
    std::unordered_map<InternedStringId, std::vector<KnowledgeTripleId>> relation_index;
    uint64_t version{1};
    mutable ReasoningIndex reasoning_index;
};

std::size_t KnowledgeGraphStore::TripleKeyHash::operator()(const TripleKey& key) const noexcept {
    std::size_t hash = static_cast<std::size_t>(key.subject);
    hash = (hash * 1315423911U) ^ static_cast<std::size_t>(key.relation);
    hash = (hash * 2654435761U) ^ static_cast<std::size_t>(key.object);
    return hash;
}

KnowledgeGraphStore::KnowledgeGraphStore() : storage_(std::make_shared<Storage>()) {}

void KnowledgeGraphStore::ensure_unique() {
    if (!storage_.unique()) {
        storage_ = std::make_shared<Storage>(*storage_);
    }
}

const KnowledgeGraphStore::ReasoningIndex& KnowledgeGraphStore::reasoning_index() const {
    ReasoningIndex& index = storage_->reasoning_index;
    if (index.version == storage_->version &&
        index.entity_count == storage_->entities.size() &&
        index.triple_count == storage_->triples.size()) {
        return index;
    }

    index = ReasoningIndex{};
    index.version = storage_->version;
    index.entity_count = storage_->entities.size();
    index.triple_count = storage_->triples.size();
    index.outgoing_offsets.assign(index.entity_count + 2U, 0U);
    index.incoming_offsets.assign(index.entity_count + 2U, 0U);
    index.outgoing_degree.assign(index.entity_count + 1U, 0U);
    index.incoming_degree.assign(index.entity_count + 1U, 0U);
    index.relation_degree.reserve(storage_->relation_index.size());

    for (const KnowledgeTriple& triple : storage_->triples) {
        if (triple.subject <= index.entity_count) {
            ++index.outgoing_offsets[static_cast<std::size_t>(triple.subject) + 1U];
            ++index.outgoing_degree[triple.subject];
        }
        if (triple.object <= index.entity_count) {
            ++index.incoming_offsets[static_cast<std::size_t>(triple.object) + 1U];
            ++index.incoming_degree[triple.object];
        }
        ++index.relation_degree[triple.relation];
    }

    for (std::size_t offset = 1U; offset < index.outgoing_offsets.size(); ++offset) {
        index.outgoing_offsets[offset] += index.outgoing_offsets[offset - 1U];
        index.incoming_offsets[offset] += index.incoming_offsets[offset - 1U];
    }

    index.outgoing_triples.assign(storage_->triples.size(), 0U);
    index.incoming_triples.assign(storage_->triples.size(), 0U);
    std::vector<uint32_t> outgoing_cursor = index.outgoing_offsets;
    std::vector<uint32_t> incoming_cursor = index.incoming_offsets;
    for (const KnowledgeTriple& triple : storage_->triples) {
        if (triple.subject <= index.entity_count) {
            index.outgoing_triples[outgoing_cursor[triple.subject]++] = triple.id;
        }
        if (triple.object <= index.entity_count) {
            index.incoming_triples[incoming_cursor[triple.object]++] = triple.id;
        }
    }

    return index;
}

KnowledgeEntityId KnowledgeGraphStore::resolve_or_create_entity(InternedStringId label) {
    ensure_unique();
    const auto existing = storage_->entity_by_label.find(label);
    if (existing != storage_->entity_by_label.end()) {
        return existing->second;
    }

    const KnowledgeEntityId id = static_cast<KnowledgeEntityId>(storage_->entities.size() + 1U);
    storage_->entities.push_back(KnowledgeEntity{id, label, {}});
    storage_->entity_by_label[label] = id;
    ++storage_->version;
    return id;
}

KnowledgeEntityId KnowledgeGraphStore::upsert_entity(InternedStringId label, BlobRef payload) {
    const KnowledgeEntityId id = resolve_or_create_entity(label);
    KnowledgeEntity& entity = storage_->entities[static_cast<std::size_t>(id - 1U)];
    if (!payload.empty()) {
        entity.payload = payload;
    }
    return id;
}

KnowledgeTripleId KnowledgeGraphStore::upsert_triple(
    InternedStringId subject_label,
    InternedStringId relation,
    InternedStringId object_label,
    BlobRef payload
) {
    ensure_unique();
    const KnowledgeEntityId subject = resolve_or_create_entity(subject_label);
    const KnowledgeEntityId object = resolve_or_create_entity(object_label);
    const TripleKey key{subject, relation, object};
    const auto existing = storage_->triple_by_key.find(key);
    if (existing != storage_->triple_by_key.end()) {
        KnowledgeTriple& triple = storage_->triples[static_cast<std::size_t>(existing->second - 1U)];
        if (!payload.empty()) {
            triple.payload = payload;
        }
        return existing->second;
    }

    const KnowledgeTripleId id = static_cast<KnowledgeTripleId>(storage_->triples.size() + 1U);
    storage_->triples.push_back(KnowledgeTriple{id, subject, relation, object, payload});
    storage_->triple_by_key[key] = id;
    storage_->outgoing_index[subject].push_back(id);
    storage_->incoming_index[object].push_back(id);
    storage_->relation_index[relation].push_back(id);
    ++storage_->version;
    return id;
}

void KnowledgeGraphStore::apply(const KnowledgeGraphPatch& patch) {
    if (patch.empty()) {
        return;
    }

    ensure_unique();
    for (const KnowledgeEntityWrite& entity : patch.entities) {
        upsert_entity(entity.label, entity.payload);
    }
    for (const KnowledgeTripleWrite& triple : patch.triples) {
        upsert_triple(triple.subject_label, triple.relation, triple.object_label, triple.payload);
    }
}

const KnowledgeEntity* KnowledgeGraphStore::find_entity(KnowledgeEntityId id) const {
    if (id == 0U || id > storage_->entities.size()) {
        return nullptr;
    }
    return &storage_->entities[static_cast<std::size_t>(id - 1U)];
}

const KnowledgeEntity* KnowledgeGraphStore::find_entity_by_label(InternedStringId label) const {
    const auto iterator = storage_->entity_by_label.find(label);
    if (iterator == storage_->entity_by_label.end()) {
        return nullptr;
    }
    return find_entity(iterator->second);
}

const KnowledgeTriple* KnowledgeGraphStore::find_triple(KnowledgeTripleId id) const {
    if (id == 0U || id > storage_->triples.size()) {
        return nullptr;
    }
    return &storage_->triples[static_cast<std::size_t>(id - 1U)];
}

const KnowledgeTriple* KnowledgeGraphStore::find_triple_by_labels(
    InternedStringId subject_label,
    InternedStringId relation,
    InternedStringId object_label
) const {
    const KnowledgeEntity* subject = find_entity_by_label(subject_label);
    const KnowledgeEntity* object = find_entity_by_label(object_label);
    if (subject == nullptr || object == nullptr) {
        return nullptr;
    }

    const auto iterator = storage_->triple_by_key.find(TripleKey{subject->id, relation, object->id});
    if (iterator == storage_->triple_by_key.end()) {
        return nullptr;
    }
    return find_triple(iterator->second);
}

std::vector<const KnowledgeTriple*> KnowledgeGraphStore::outgoing(
    KnowledgeEntityId subject,
    std::optional<InternedStringId> relation
) const {
    std::vector<const KnowledgeTriple*> triples;
    const ReasoningIndex& index = reasoning_index();
    if (subject == 0U || subject > index.entity_count) {
        return triples;
    }

    const std::size_t begin = index.outgoing_offsets[subject];
    const std::size_t end = index.outgoing_offsets[static_cast<std::size_t>(subject) + 1U];
    triples.reserve(end - begin);
    for (std::size_t offset = begin; offset < end; ++offset) {
        const KnowledgeTripleId triple_id = index.outgoing_triples[offset];
        const KnowledgeTriple* triple = find_triple(triple_id);
        if (triple == nullptr) {
            continue;
        }
        if (relation.has_value() && triple->relation != *relation) {
            continue;
        }
        triples.push_back(triple);
    }
    return triples;
}

std::vector<const KnowledgeTriple*> KnowledgeGraphStore::incoming(
    KnowledgeEntityId object,
    std::optional<InternedStringId> relation
) const {
    std::vector<const KnowledgeTriple*> triples;
    const ReasoningIndex& index = reasoning_index();
    if (object == 0U || object > index.entity_count) {
        return triples;
    }

    const std::size_t begin = index.incoming_offsets[object];
    const std::size_t end = index.incoming_offsets[static_cast<std::size_t>(object) + 1U];
    triples.reserve(end - begin);
    for (std::size_t offset = begin; offset < end; ++offset) {
        const KnowledgeTripleId triple_id = index.incoming_triples[offset];
        const KnowledgeTriple* triple = find_triple(triple_id);
        if (triple == nullptr) {
            continue;
        }
        if (relation.has_value() && triple->relation != *relation) {
            continue;
        }
        triples.push_back(triple);
    }
    return triples;
}

std::vector<const KnowledgeTriple*> KnowledgeGraphStore::select_candidate_set(
    std::optional<KnowledgeEntityId> subject,
    std::optional<InternedStringId> relation,
    std::optional<KnowledgeEntityId> object
) const {
    enum class CandidateKind : uint8_t {
        All,
        Outgoing,
        Incoming,
        Relation
    };

    struct CandidatePlan {
        CandidateKind kind{CandidateKind::All};
        std::size_t begin{0};
        std::size_t end{0};
        const std::vector<KnowledgeTripleId>* relation_ids{nullptr};
        std::size_t size{0};
        bool set{false};
    };

    const ReasoningIndex& index = reasoning_index();
    CandidatePlan best;
    const auto consider = [&](CandidatePlan candidate) {
        if (!best.set || candidate.size < best.size) {
            best = candidate;
            best.set = true;
        }
    };

    if (subject.has_value()) {
        if (*subject == 0U || *subject > index.entity_count) {
            return {};
        }
        const std::size_t begin = index.outgoing_offsets[*subject];
        const std::size_t end = index.outgoing_offsets[static_cast<std::size_t>(*subject) + 1U];
        consider(CandidatePlan{CandidateKind::Outgoing, begin, end, nullptr, end - begin, true});
    }
    if (object.has_value()) {
        if (*object == 0U || *object > index.entity_count) {
            return {};
        }
        const std::size_t begin = index.incoming_offsets[*object];
        const std::size_t end = index.incoming_offsets[static_cast<std::size_t>(*object) + 1U];
        consider(CandidatePlan{CandidateKind::Incoming, begin, end, nullptr, end - begin, true});
    }
    if (relation.has_value()) {
        const auto relation_iterator = storage_->relation_index.find(*relation);
        if (relation_iterator == storage_->relation_index.end()) {
            return {};
        }
        consider(CandidatePlan{
            CandidateKind::Relation,
            0U,
            relation_iterator->second.size(),
            &relation_iterator->second,
            relation_iterator->second.size(),
            true
        });
    }

    std::vector<const KnowledgeTriple*> triples;
    if (best.set) {
        triples.reserve(best.size);
        const auto append_id = [&](KnowledgeTripleId triple_id) {
            const KnowledgeTriple* triple = find_triple(triple_id);
            if (triple != nullptr) {
                triples.push_back(triple);
            }
        };
        if (best.kind == CandidateKind::Outgoing) {
            for (std::size_t offset = best.begin; offset < best.end; ++offset) {
                append_id(index.outgoing_triples[offset]);
            }
        } else if (best.kind == CandidateKind::Incoming) {
            for (std::size_t offset = best.begin; offset < best.end; ++offset) {
                append_id(index.incoming_triples[offset]);
            }
        } else if (best.relation_ids != nullptr) {
            for (KnowledgeTripleId triple_id : *best.relation_ids) {
                append_id(triple_id);
            }
        }
        return triples;
    }

    triples.reserve(storage_->triples.size());
    for (const KnowledgeTriple& triple : storage_->triples) {
        triples.push_back(&triple);
    }
    return triples;
}

std::vector<const KnowledgeTriple*> KnowledgeGraphStore::match(
    std::optional<InternedStringId> subject_label,
    std::optional<InternedStringId> relation,
    std::optional<InternedStringId> object_label
) const {
    std::optional<KnowledgeEntityId> subject;
    std::optional<KnowledgeEntityId> object;

    if (subject_label.has_value()) {
        const KnowledgeEntity* entity = find_entity_by_label(*subject_label);
        if (entity == nullptr) {
            return {};
        }
        subject = entity->id;
    }

    if (object_label.has_value()) {
        const KnowledgeEntity* entity = find_entity_by_label(*object_label);
        if (entity == nullptr) {
            return {};
        }
        object = entity->id;
    }

    if (subject.has_value() && relation.has_value() && object.has_value()) {
        const auto iterator = storage_->triple_by_key.find(TripleKey{*subject, *relation, *object});
        if (iterator == storage_->triple_by_key.end()) {
            return {};
        }
        const KnowledgeTriple* triple = find_triple(iterator->second);
        if (triple == nullptr) {
            return {};
        }
        return {triple};
    }

    std::vector<const KnowledgeTriple*> matches = select_candidate_set(subject, relation, object);
    matches.erase(
        std::remove_if(matches.begin(), matches.end(), [&](const KnowledgeTriple* triple) {
            if (subject.has_value() && triple->subject != *subject) {
                return true;
            }
            if (relation.has_value() && triple->relation != *relation) {
                return true;
            }
            if (object.has_value() && triple->object != *object) {
                return true;
            }
            return false;
        }),
        matches.end()
    );
    return matches;
}

std::vector<KnowledgeEntityId> KnowledgeGraphStore::neighbors(
    InternedStringId subject_label,
    std::optional<InternedStringId> relation
) const {
    const KnowledgeEntity* entity = find_entity_by_label(subject_label);
    if (entity == nullptr) {
        return {};
    }

    std::vector<KnowledgeEntityId> neighbors_list;
    for (const KnowledgeTriple* triple : outgoing(entity->id, relation)) {
        neighbors_list.push_back(triple->object);
    }
    return neighbors_list;
}

std::size_t KnowledgeGraphStore::entity_count() const noexcept {
    return storage_->entities.size();
}

std::size_t KnowledgeGraphStore::triple_count() const noexcept {
    return storage_->triples.size();
}

bool KnowledgeGraphStore::shares_storage_with(const KnowledgeGraphStore& other) const noexcept {
    return storage_ == other.storage_;
}

void KnowledgeGraphStore::serialize(std::ostream& output) const {
    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->entities.size()));
    for (const KnowledgeEntity& entity : storage_->entities) {
        write_pod(output, entity.id);
        write_pod(output, entity.label);
        write_blob_ref(output, entity.payload);
    }

    write_pod<uint64_t>(output, static_cast<uint64_t>(storage_->triples.size()));
    for (const KnowledgeTriple& triple : storage_->triples) {
        write_pod(output, triple.id);
        write_pod(output, triple.subject);
        write_pod(output, triple.relation);
        write_pod(output, triple.object);
        write_blob_ref(output, triple.payload);
    }
}

KnowledgeGraphStore KnowledgeGraphStore::deserialize(std::istream& input) {
    KnowledgeGraphStore store;
    store.clear();

    const uint64_t entity_count = read_pod<uint64_t>(input);
    store.storage_->entities.reserve(static_cast<std::size_t>(entity_count));
    for (uint64_t index = 0; index < entity_count; ++index) {
        const KnowledgeEntity entity{
            read_pod<KnowledgeEntityId>(input),
            read_pod<InternedStringId>(input),
            read_blob_ref(input)
        };
        store.storage_->entities.push_back(entity);
        store.storage_->entity_by_label[entity.label] = entity.id;
    }

    const uint64_t triple_count = read_pod<uint64_t>(input);
    store.storage_->triples.reserve(static_cast<std::size_t>(triple_count));
    for (uint64_t index = 0; index < triple_count; ++index) {
        const KnowledgeTriple triple{
            read_pod<KnowledgeTripleId>(input),
            read_pod<KnowledgeEntityId>(input),
            read_pod<InternedStringId>(input),
            read_pod<KnowledgeEntityId>(input),
            read_blob_ref(input)
        };
        store.storage_->triples.push_back(triple);
        store.storage_->triple_by_key[TripleKey{triple.subject, triple.relation, triple.object}] = triple.id;
        store.storage_->outgoing_index[triple.subject].push_back(triple.id);
        store.storage_->incoming_index[triple.object].push_back(triple.id);
        store.storage_->relation_index[triple.relation].push_back(triple.id);
    }

    return store;
}

void KnowledgeGraphStore::clear() {
    storage_ = std::make_shared<Storage>();
}

} // namespace agentcore
