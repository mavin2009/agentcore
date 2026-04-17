#ifndef AGENTCORE_MODEL_API_H
#define AGENTCORE_MODEL_API_H

#include "agentcore/runtime/async_executor.h"
#include "agentcore/core/types.h"
#include <cstddef>
#include <cstdint>
#include <future>
#include <functional>
#include <optional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentcore {

class BlobStore;
class StringInterner;

struct AsyncModelHandle {
    uint64_t id{0};

    [[nodiscard]] bool valid() const noexcept { return id != 0U; }
};

struct AsyncModelSnapshot {
    AsyncModelHandle handle{};
    std::string model_name;
    std::vector<std::byte> prompt;
    std::vector<std::byte> schema;
    uint32_t max_tokens{0};
    bool result_ready{false};
    bool ok{false};
    std::vector<std::byte> output;
    float confidence{0.0F};
    uint32_t token_usage{0};
    uint32_t flags{0};
    uint16_t attempts{0};
    uint64_t latency_ns{0};
};

struct ModelRequest {
    InternedStringId model_name{0};
    BlobRef prompt{};
    BlobRef schema{};
    uint32_t max_tokens{0};
};

struct ModelResponse {
    bool ok{false};
    BlobRef output{};
    float confidence{0.0F};
    uint32_t token_usage{0};
    uint32_t flags{0};
    uint16_t attempts{0};
    uint64_t latency_ns{0};
};

enum ModelResponseFlag : uint32_t {
    kModelFlagNone = 0U,
    kModelFlagMissingHandler = 1U << 0,
    kModelFlagHandlerException = 1U << 1,
    kModelFlagPromptTooLarge = 1U << 2,
    kModelFlagOutputTooLarge = 1U << 3,
    kModelFlagRetriesExhausted = 1U << 4,
    kModelFlagTimeoutExceeded = 1U << 5,
    kModelFlagValidationError = 1U << 6,
    kModelFlagUnsupportedRequest = 1U << 7
};

struct ModelPolicy {
    uint16_t retry_limit{0};
    uint32_t timeout_ms{0};
    std::size_t max_prompt_bytes{128U * 1024U};
    std::size_t max_output_bytes{1024U * 1024U};
};

struct ModelInvocationContext {
    BlobStore& blobs;
    StringInterner& strings;
};

using ModelHandler = std::function<ModelResponse(const ModelRequest&, ModelInvocationContext&)>;
using AsyncModelCompletionListener = std::function<void(AsyncModelHandle)>;

class ModelRegistry {
public:
    explicit ModelRegistry(std::size_t async_worker_count = 0, std::size_t max_async_queue_depth = 0)
        : executor_(async_worker_count, max_async_queue_depth) {}

    void register_model(std::string_view name, ModelHandler handler);
    void register_model(std::string_view name, ModelPolicy policy, ModelHandler handler);
    void set_async_completion_listener(AsyncModelCompletionListener listener);
    [[nodiscard]] bool has_model(std::string_view name) const;
    [[nodiscard]] ModelResponse invoke(const ModelRequest& request, ModelInvocationContext& context) const;
    [[nodiscard]] AsyncModelHandle begin_invoke_async(
        const ModelRequest& request,
        ModelInvocationContext& context
    ) const;
    [[nodiscard]] bool is_async_ready(AsyncModelHandle handle) const;
    [[nodiscard]] bool has_async_handle(AsyncModelHandle handle) const;
    [[nodiscard]] std::optional<AsyncModelSnapshot> export_async_operation(AsyncModelHandle handle) const;
    [[nodiscard]] AsyncModelHandle restore_async_operation(const AsyncModelSnapshot& snapshot) const;
    [[nodiscard]] std::optional<ModelResponse> take_async_result(
        AsyncModelHandle handle,
        ModelInvocationContext& context
    ) const;
    [[nodiscard]] std::size_t size() const noexcept;
    struct ModelSpec {
        ModelPolicy policy{};
        ModelHandler handler;
    };

    struct NamedModelSpec {
        std::string name;
        ModelSpec spec{};
    };

    struct AsyncModelResult {
        bool ok{false};
        std::vector<std::byte> output;
        float confidence{0.0F};
        uint32_t token_usage{0};
        uint32_t flags{0};
        uint16_t attempts{0};
        uint64_t latency_ns{0};
    };

    struct AsyncModelOperation {
        std::string model_name;
        std::vector<std::byte> prompt_bytes;
        std::vector<std::byte> schema_bytes;
        uint32_t max_tokens{0};
        std::shared_future<AsyncModelResult> future;
    };

    [[nodiscard]] std::vector<NamedModelSpec> registered_models() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ModelSpec> handlers_;
    mutable std::unordered_map<uint64_t, AsyncModelOperation> async_operations_;
    mutable uint64_t next_async_handle_id_{1};
    mutable AsyncTaskExecutor executor_;
    AsyncModelCompletionListener async_completion_listener_;
};

} // namespace agentcore

#endif // AGENTCORE_MODEL_API_H
