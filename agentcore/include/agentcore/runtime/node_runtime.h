#ifndef AGENTCORE_NODE_RUNTIME_H
#define AGENTCORE_NODE_RUNTIME_H

#include "agentcore/core/types.h"
#include "agentcore/state/state_store.h"
#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace agentcore {

class ToolRegistry;
class ModelRegistry;
class TraceSink;
struct AsyncToolHandle;
struct AsyncModelHandle;

enum class AsyncOperationKind : uint8_t {
    None,
    Tool,
    Model
};

struct PendingAsyncOperation {
    AsyncOperationKind kind{AsyncOperationKind::None};
    uint64_t handle_id{0};

    [[nodiscard]] bool valid() const noexcept {
        return kind != AsyncOperationKind::None && handle_id != 0U;
    }
};

class ScratchArena {
public:
    explicit ScratchArena(std::size_t initial_capacity = 4096);

    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t));
    void reset() noexcept;
    [[nodiscard]] std::size_t bytes_used() const noexcept;

private:
    std::size_t align_cursor(std::size_t cursor, std::size_t alignment) const noexcept;

    std::vector<std::byte> storage_;
    std::size_t cursor_{0};
};

class Deadline {
public:
    explicit Deadline(uint64_t deadline_ns = 0) : deadline_ns_(deadline_ns) {}

    [[nodiscard]] uint64_t deadline_ns() const noexcept { return deadline_ns_; }
    [[nodiscard]] bool expired(uint64_t now_ns) const noexcept {
        return deadline_ns_ != 0U && now_ns >= deadline_ns_;
    }

private:
    uint64_t deadline_ns_{0};
};

class CancellationToken {
public:
    CancellationToken() = default;
    CancellationToken(const CancellationToken&) = delete;
    CancellationToken& operator=(const CancellationToken&) = delete;
    CancellationToken(CancellationToken&& other) noexcept : cancelled_(other.cancelled_.load(std::memory_order_acquire)) {}
    CancellationToken& operator=(CancellationToken&& other) noexcept {
        if (this != &other) {
            cancelled_.store(other.cancelled_.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    void request_cancel() noexcept { cancelled_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_cancelled() const noexcept { return cancelled_.load(std::memory_order_acquire); }

private:
    std::atomic<bool> cancelled_{false};
};

struct NodeResult {
    enum Status : uint8_t {
        Success,
        SoftFail,
        HardFail,
        Waiting,
        Cancelled
    } status{Success};

    StatePatch patch;
    float confidence{1.0F};
    uint32_t flags{0};
    std::optional<NodeId> next_override;
    std::optional<PendingAsyncOperation> pending_async;

    static NodeResult success(StatePatch patch = {}, float confidence = 1.0F) {
        return NodeResult{Success, std::move(patch), confidence, 0, std::nullopt, std::nullopt};
    }

    static NodeResult waiting(StatePatch patch = {}, float confidence = 1.0F) {
        return NodeResult{Waiting, std::move(patch), confidence, 0, std::nullopt, std::nullopt};
    }

    static NodeResult waiting_on_tool(
        AsyncToolHandle handle,
        StatePatch patch = {},
        float confidence = 1.0F
    );

    static NodeResult waiting_on_model(
        AsyncModelHandle handle,
        StatePatch patch = {},
        float confidence = 1.0F
    );

    [[nodiscard]] bool is_waiting_on_async() const noexcept {
        return status == Waiting && pending_async.has_value() && pending_async->valid();
    }
};

struct RecordedEffectResult {
    BlobRef request{};
    BlobRef output{};
    uint32_t flags{0};
    bool replayed{false};
};

struct ExecutionContext {
    const WorkflowState& state;
    RunId run_id;
    GraphId graph_id;
    NodeId node_id;
    uint32_t branch_id;
    const std::vector<std::byte>& runtime_config_payload;
    ScratchArena& scratch;
    BlobStore& blobs;
    StringInterner& strings;
    const KnowledgeGraphStore& knowledge_graph;
    const IntelligenceStore& intelligence;
    const TaskJournal& task_journal;
    ToolRegistry& tools;
    ModelRegistry& models;
    TraceSink& trace;
    Deadline deadline;
    CancellationToken& cancel;
    std::optional<PendingAsyncOperation> pending_async;
    std::vector<TaskRecord>* recorded_effects;

    [[nodiscard]] std::optional<RecordedEffectResult> find_recorded_effect(std::string_view key) const {
        if (key.empty()) {
            return std::nullopt;
        }
        const InternedStringId key_id = strings.intern(key);
        if (recorded_effects != nullptr) {
            const auto pending_iterator = std::find_if(
                recorded_effects->begin(),
                recorded_effects->end(),
                [key_id](const TaskRecord& record) {
                    return record.key == key_id;
                }
            );
            if (pending_iterator != recorded_effects->end()) {
                return RecordedEffectResult{
                    pending_iterator->request,
                    pending_iterator->output,
                    pending_iterator->flags,
                    true
                };
            }
        }

        if (const TaskRecord* record = task_journal.find(key_id); record != nullptr) {
            return RecordedEffectResult{record->request, record->output, record->flags, true};
        }
        return std::nullopt;
    }

    template <typename ProducerFn>
    RecordedEffectResult record_blob_effect_once(
        std::string_view key,
        BlobRef request,
        ProducerFn&& producer,
        uint32_t flags = 0U
    ) {
        if (key.empty()) {
            throw std::invalid_argument("recorded effect key must not be empty");
        }
        if (recorded_effects == nullptr) {
            throw std::runtime_error("recorded effect recorder is unavailable");
        }

        auto blob_equal = [this](BlobRef left, BlobRef right) {
            if (left == right) {
                return true;
            }
            if (left.empty() || right.empty()) {
                return left.empty() && right.empty();
            }
            return blobs.read_bytes(left) == blobs.read_bytes(right);
        };

        if (const auto existing = find_recorded_effect(key); existing.has_value()) {
            if (!blob_equal(existing->request, request)) {
                throw std::runtime_error("recorded effect request mismatch for key");
            }
            return *existing;
        }

        BlobRef output = std::invoke(std::forward<ProducerFn>(producer));
        const InternedStringId key_id = strings.intern(key);
        recorded_effects->push_back(TaskRecord{key_id, request, output, flags});
        return RecordedEffectResult{request, output, flags, false};
    }

    template <typename ProducerFn>
    RecordedEffectResult record_text_effect_once(
        std::string_view key,
        std::string_view request_text,
        ProducerFn&& producer,
        uint32_t flags = 0U
    ) {
        if (const auto existing = find_recorded_effect(key); existing.has_value()) {
            const std::string_view existing_request = existing->request.empty()
                ? std::string_view{}
                : blobs.read_string(existing->request);
            if (existing_request != request_text) {
                throw std::runtime_error("recorded effect request mismatch for key");
            }
            return *existing;
        }

        const BlobRef request = request_text.empty() ? BlobRef{} : blobs.append_string(request_text);
        return record_blob_effect_once(
            key,
            request,
            [&]() -> BlobRef {
                auto output = std::invoke(std::forward<ProducerFn>(producer));
                return blobs.append_string(std::string_view(output));
            },
            flags
        );
    }
};

} // namespace agentcore

#endif // AGENTCORE_NODE_RUNTIME_H
