#include "agentcore/runtime/tool_api.h"

#include "agentcore/state/state_store.h"

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <utility>

namespace agentcore {

namespace {

template <typename T>
std::shared_future<T> make_ready_future(T value) {
    std::promise<T> promise;
    std::future<T> future = promise.get_future();
    promise.set_value(std::move(value));
    return future.share();
}

ToolRegistry::AsyncToolResult run_tool_with_spec(
    const ToolRegistry::ToolSpec& spec,
    std::string_view tool_name,
    const std::vector<std::byte>& input_bytes
) {
    BlobStore blobs;
    StringInterner strings;
    ToolInvocationContext local_context{blobs, strings};
    const BlobRef input_ref = input_bytes.empty()
        ? BlobRef{}
        : blobs.append(input_bytes.data(), input_bytes.size());
    const ToolRequest local_request{strings.intern(tool_name), input_ref};

    ToolResponse last_response{false, {}, kToolFlagRetriesExhausted, 0U, 0U};
    const uint16_t max_attempts = static_cast<uint16_t>(spec.policy.retry_limit + 1U);
    for (uint16_t attempt = 1; attempt <= max_attempts; ++attempt) {
        const auto started_at = std::chrono::steady_clock::now();
        try {
            ToolResponse response = spec.handler(local_request, local_context);
            const auto ended_at = std::chrono::steady_clock::now();
            response.attempts = attempt;
            response.latency_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
            );

            if (spec.policy.timeout_ms != 0U &&
                response.latency_ns > (static_cast<uint64_t>(spec.policy.timeout_ms) * 1000000ULL)) {
                response.ok = false;
                response.flags |= kToolFlagTimeoutExceeded;
            }
            if (response.output.size > spec.policy.max_output_bytes) {
                response.ok = false;
                response.flags |= kToolFlagOutputTooLarge;
            }
            if (response.ok) {
                return ToolRegistry::AsyncToolResult{
                    true,
                    blobs.read_bytes(response.output),
                    response.flags,
                    response.attempts,
                    response.latency_ns
                };
            }

            last_response = response;
        } catch (const std::exception&) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ToolResponse{
                false,
                {},
                static_cast<uint32_t>(kToolFlagHandlerException | kToolFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        } catch (...) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ToolResponse{
                false,
                {},
                static_cast<uint32_t>(kToolFlagHandlerException | kToolFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        }
    }

    last_response.flags |= kToolFlagRetriesExhausted;
    return ToolRegistry::AsyncToolResult{
        last_response.ok,
        blobs.read_bytes(last_response.output),
        last_response.flags,
        last_response.attempts,
        last_response.latency_ns
    };
}

} // namespace

void ToolRegistry::register_tool(std::string_view name, ToolHandler handler) {
    register_tool(name, ToolPolicy{}, std::move(handler));
}

void ToolRegistry::register_tool(std::string_view name, ToolPolicy policy, ToolHandler handler) {
    AdapterMetadata metadata;
    metadata.provider = "custom";
    metadata.implementation = std::string(name);
    metadata.display_name = std::string(name);
    metadata.transport = AdapterTransportKind::InProcess;
    metadata.auth = AdapterAuthKind::None;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe);
    metadata.request_format = "blob";
    metadata.response_format = "blob";
    register_tool(name, std::move(policy), std::move(metadata), std::move(handler));
}

void ToolRegistry::register_tool(
    std::string_view name,
    ToolPolicy policy,
    AdapterMetadata metadata,
    ToolHandler handler
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (metadata.implementation.empty()) {
        metadata.implementation = std::string(name);
    }
    if (metadata.display_name.empty()) {
        metadata.display_name = std::string(name);
    }
    handlers_[std::string(name)] = ToolSpec{policy, std::move(metadata), std::move(handler)};
}

void ToolRegistry::set_async_completion_listener(AsyncToolCompletionListener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    async_completion_listener_ = std::move(listener);
}

bool ToolRegistry::has_tool(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.find(std::string(name)) != handlers_.end();
}

ToolResponse ToolRegistry::invoke(const ToolRequest& request, ToolInvocationContext& context) const {
    const std::string_view name = context.strings.resolve(request.tool_name);
    ToolSpec spec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handlers_.find(std::string(name));
        if (iterator == handlers_.end()) {
            return ToolResponse{false, {}, kToolFlagMissingHandler, 0U, 0U};
        }
        spec = iterator->second;
    }

    if (request.input.size > spec.policy.max_input_bytes) {
        return ToolResponse{false, {}, kToolFlagInputTooLarge, 0U, 0U};
    }

    ToolResponse last_response{false, {}, kToolFlagRetriesExhausted, 0U, 0U};
    const uint16_t max_attempts = static_cast<uint16_t>(spec.policy.retry_limit + 1U);
    for (uint16_t attempt = 1; attempt <= max_attempts; ++attempt) {
        const auto started_at = std::chrono::steady_clock::now();
        try {
            ToolResponse response = spec.handler(request, context);
            const auto ended_at = std::chrono::steady_clock::now();
            response.attempts = attempt;
            response.latency_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
            );

            if (spec.policy.timeout_ms != 0U &&
                response.latency_ns > (static_cast<uint64_t>(spec.policy.timeout_ms) * 1000000ULL)) {
                response.ok = false;
                response.flags |= kToolFlagTimeoutExceeded;
            }
            if (response.output.size > spec.policy.max_output_bytes) {
                response.ok = false;
                response.flags |= kToolFlagOutputTooLarge;
            }
            if (response.ok) {
                return response;
            }

            last_response = response;
        } catch (const std::exception&) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ToolResponse{
                false,
                {},
                static_cast<uint32_t>(kToolFlagHandlerException | kToolFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        } catch (...) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ToolResponse{
                false,
                {},
                static_cast<uint32_t>(kToolFlagHandlerException | kToolFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        }
    }

    last_response.flags |= kToolFlagRetriesExhausted;
    return last_response;
}

AsyncToolHandle ToolRegistry::begin_invoke_async(
    const ToolRequest& request,
    ToolInvocationContext& context
) const {
    const std::string tool_name(context.strings.resolve(request.tool_name));
    const std::vector<std::byte> input_bytes = context.blobs.read_bytes(request.input);

    ToolSpec spec;
    AsyncToolCompletionListener listener;
    AsyncToolHandle handle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handlers_.find(tool_name);
        if (iterator != handlers_.end()) {
            spec = iterator->second;
        }
        listener = async_completion_listener_;
        handle = AsyncToolHandle{next_async_handle_id_++};
    }

    AsyncToolResult immediate_failure;
    bool use_immediate_failure = false;
    if (spec.handler == nullptr) {
        immediate_failure.ok = false;
        immediate_failure.flags = kToolFlagMissingHandler;
        use_immediate_failure = true;
    } else if (request.input.size > spec.policy.max_input_bytes) {
        immediate_failure.ok = false;
        immediate_failure.flags = kToolFlagInputTooLarge;
        use_immediate_failure = true;
    }

    std::shared_future<AsyncToolResult> future;
    if (use_immediate_failure) {
        future = make_ready_future(std::move(immediate_failure));
    } else {
        auto promise = std::make_shared<std::promise<AsyncToolResult>>();
        future = promise->get_future().share();
        executor_.enqueue([spec,
                           tool_name,
                           input_bytes,
                           promise,
                           listener,
                           handle]() mutable {
            try {
                promise->set_value(run_tool_with_spec(spec, tool_name, input_bytes));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            if (listener) {
                listener(handle);
            }
        });
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        async_operations_.emplace(handle.id, AsyncToolOperation{tool_name, input_bytes, std::move(future)});
    }
    if (use_immediate_failure && listener) {
        listener(handle);
    }
    return handle;
}

bool ToolRegistry::is_async_ready(AsyncToolHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = async_operations_.find(handle.id);
    if (iterator == async_operations_.end()) {
        return false;
    }
    return iterator->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

bool ToolRegistry::has_async_handle(AsyncToolHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return async_operations_.find(handle.id) != async_operations_.end();
}

std::optional<AsyncToolSnapshot> ToolRegistry::export_async_operation(AsyncToolHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = async_operations_.find(handle.id);
    if (iterator == async_operations_.end()) {
        return std::nullopt;
    }

    AsyncToolSnapshot snapshot;
    snapshot.handle = handle;
    snapshot.tool_name = iterator->second.tool_name;
    snapshot.input = iterator->second.input_bytes;
    if (iterator->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const AsyncToolResult result = iterator->second.future.get();
        snapshot.result_ready = true;
        snapshot.ok = result.ok;
        snapshot.output = result.output;
        snapshot.flags = result.flags;
        snapshot.attempts = result.attempts;
        snapshot.latency_ns = result.latency_ns;
    }
    return snapshot;
}

AsyncToolHandle ToolRegistry::restore_async_operation(const AsyncToolSnapshot& snapshot) const {
    ToolSpec spec;
    bool has_spec = false;
    AsyncToolCompletionListener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handlers_.find(snapshot.tool_name);
        if (iterator != handlers_.end()) {
            spec = iterator->second;
            has_spec = true;
        }
        listener = async_completion_listener_;
    }

    std::shared_future<AsyncToolResult> future;
    const bool completes_immediately = snapshot.result_ready ||
        !has_spec ||
        snapshot.input.size() > spec.policy.max_input_bytes;
    if (snapshot.result_ready) {
        future = make_ready_future(AsyncToolResult{
            snapshot.ok,
            snapshot.output,
            snapshot.flags,
            snapshot.attempts,
            snapshot.latency_ns
        });
    } else if (!has_spec) {
        future = make_ready_future(AsyncToolResult{
            false,
            {},
            kToolFlagMissingHandler,
            0U,
            0U
        });
    } else if (snapshot.input.size() > spec.policy.max_input_bytes) {
        future = make_ready_future(AsyncToolResult{
            false,
            {},
            kToolFlagInputTooLarge,
            0U,
            0U
        });
    } else {
        auto promise = std::make_shared<std::promise<AsyncToolResult>>();
        future = promise->get_future().share();
        executor_.enqueue([spec,
                           tool_name = snapshot.tool_name,
                           input_bytes = snapshot.input,
                           promise,
                           listener,
                           handle = snapshot.handle]() mutable {
            try {
                promise->set_value(run_tool_with_spec(spec, tool_name, input_bytes));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            if (listener) {
                listener(handle);
            }
        });
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        next_async_handle_id_ = std::max(next_async_handle_id_, snapshot.handle.id + 1U);
        async_operations_[snapshot.handle.id] = AsyncToolOperation{
            snapshot.tool_name,
            snapshot.input,
            std::move(future)
        };
    }
    if (completes_immediately && listener) {
        listener(snapshot.handle);
    }
    return snapshot.handle;
}

std::optional<ToolResponse> ToolRegistry::take_async_result(
    AsyncToolHandle handle,
    ToolInvocationContext& context
) const {
    std::shared_future<AsyncToolResult> future;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = async_operations_.find(handle.id);
        if (iterator == async_operations_.end()) {
            return std::nullopt;
        }
        if (iterator->second.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return std::nullopt;
        }

        future = std::move(iterator->second.future);
        async_operations_.erase(iterator);
    }

    AsyncToolResult result = future.get();
    BlobRef output_ref{};
    if (!result.output.empty()) {
        output_ref = context.blobs.append(result.output.data(), result.output.size());
    }
    return ToolResponse{
        result.ok,
        output_ref,
        result.flags,
        result.attempts,
        result.latency_ns
    };
}

std::size_t ToolRegistry::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

std::vector<ToolRegistry::NamedToolSpec> ToolRegistry::registered_tools() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NamedToolSpec> tools;
    tools.reserve(handlers_.size());
    for (const auto& [name, spec] : handlers_) {
        tools.push_back(NamedToolSpec{name, spec});
    }
    return tools;
}

std::optional<ToolRegistry::NamedToolSpec> ToolRegistry::describe_tool(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = handlers_.find(std::string(name));
    if (iterator == handlers_.end()) {
        return std::nullopt;
    }
    return NamedToolSpec{iterator->first, iterator->second};
}

} // namespace agentcore
