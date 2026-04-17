#ifndef AGENTCORE_TOOL_API_H
#define AGENTCORE_TOOL_API_H

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

struct AsyncToolHandle {
    uint64_t id{0};

    [[nodiscard]] bool valid() const noexcept { return id != 0U; }
};

struct AsyncToolSnapshot {
    AsyncToolHandle handle{};
    std::string tool_name;
    std::vector<std::byte> input;
    bool result_ready{false};
    bool ok{false};
    std::vector<std::byte> output;
    uint32_t flags{0};
    uint16_t attempts{0};
    uint64_t latency_ns{0};
};

struct ToolRequest {
    InternedStringId tool_name{0};
    BlobRef input{};
};

struct ToolResponse {
    bool ok{false};
    BlobRef output{};
    uint32_t flags{0};
    uint16_t attempts{0};
    uint64_t latency_ns{0};
};

enum ToolResponseFlag : uint32_t {
    kToolFlagNone = 0U,
    kToolFlagMissingHandler = 1U << 0,
    kToolFlagHandlerException = 1U << 1,
    kToolFlagInputTooLarge = 1U << 2,
    kToolFlagOutputTooLarge = 1U << 3,
    kToolFlagRetriesExhausted = 1U << 4,
    kToolFlagTimeoutExceeded = 1U << 5,
    kToolFlagValidationError = 1U << 6,
    kToolFlagUnsupportedRequest = 1U << 7
};

struct ToolPolicy {
    uint16_t retry_limit{0};
    uint32_t timeout_ms{0};
    std::size_t max_input_bytes{64U * 1024U};
    std::size_t max_output_bytes{1024U * 1024U};
};

struct ToolInvocationContext {
    BlobStore& blobs;
    StringInterner& strings;
};

using ToolHandler = std::function<ToolResponse(const ToolRequest&, ToolInvocationContext&)>;
using AsyncToolCompletionListener = std::function<void(AsyncToolHandle)>;

class ToolRegistry {
public:
    explicit ToolRegistry(std::size_t async_worker_count = 0, std::size_t max_async_queue_depth = 0)
        : executor_(async_worker_count, max_async_queue_depth) {}

    void register_tool(std::string_view name, ToolHandler handler);
    void register_tool(std::string_view name, ToolPolicy policy, ToolHandler handler);
    void set_async_completion_listener(AsyncToolCompletionListener listener);
    [[nodiscard]] bool has_tool(std::string_view name) const;
    [[nodiscard]] ToolResponse invoke(const ToolRequest& request, ToolInvocationContext& context) const;
    [[nodiscard]] AsyncToolHandle begin_invoke_async(
        const ToolRequest& request,
        ToolInvocationContext& context
    ) const;
    [[nodiscard]] bool is_async_ready(AsyncToolHandle handle) const;
    [[nodiscard]] bool has_async_handle(AsyncToolHandle handle) const;
    [[nodiscard]] std::optional<AsyncToolSnapshot> export_async_operation(AsyncToolHandle handle) const;
    [[nodiscard]] AsyncToolHandle restore_async_operation(const AsyncToolSnapshot& snapshot) const;
    [[nodiscard]] std::optional<ToolResponse> take_async_result(
        AsyncToolHandle handle,
        ToolInvocationContext& context
    ) const;
    [[nodiscard]] std::size_t size() const noexcept;
    struct ToolSpec {
        ToolPolicy policy{};
        ToolHandler handler;
    };

    struct NamedToolSpec {
        std::string name;
        ToolSpec spec{};
    };

    struct AsyncToolResult {
        bool ok{false};
        std::vector<std::byte> output;
        uint32_t flags{0};
        uint16_t attempts{0};
        uint64_t latency_ns{0};
    };

    struct AsyncToolOperation {
        std::string tool_name;
        std::vector<std::byte> input_bytes;
        std::shared_future<AsyncToolResult> future;
    };

    [[nodiscard]] std::vector<NamedToolSpec> registered_tools() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ToolSpec> handlers_;
    mutable std::unordered_map<uint64_t, AsyncToolOperation> async_operations_;
    mutable uint64_t next_async_handle_id_{1};
    mutable AsyncTaskExecutor executor_;
    AsyncToolCompletionListener async_completion_listener_;
};

} // namespace agentcore

#endif // AGENTCORE_TOOL_API_H
