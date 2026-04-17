#include "agentcore/runtime/model_api.h"

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

ModelRegistry::AsyncModelResult run_model_with_spec(
    const ModelRegistry::ModelSpec& spec,
    std::string_view model_name,
    const std::vector<std::byte>& prompt_bytes,
    const std::vector<std::byte>& schema_bytes,
    uint32_t max_tokens
) {
    BlobStore blobs;
    StringInterner strings;
    ModelInvocationContext local_context{blobs, strings};
    const BlobRef prompt_ref = prompt_bytes.empty()
        ? BlobRef{}
        : blobs.append(prompt_bytes.data(), prompt_bytes.size());
    const BlobRef schema_ref = schema_bytes.empty()
        ? BlobRef{}
        : blobs.append(schema_bytes.data(), schema_bytes.size());
    const ModelRequest local_request{strings.intern(model_name), prompt_ref, schema_ref, max_tokens};

    ModelResponse last_response{false, {}, 0.0F, 0U, kModelFlagRetriesExhausted, 0U, 0U};
    const uint16_t max_attempts = static_cast<uint16_t>(spec.policy.retry_limit + 1U);
    for (uint16_t attempt = 1; attempt <= max_attempts; ++attempt) {
        const auto started_at = std::chrono::steady_clock::now();
        try {
            ModelResponse response = spec.handler(local_request, local_context);
            const auto ended_at = std::chrono::steady_clock::now();
            response.attempts = attempt;
            response.latency_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
            );

            if (spec.policy.timeout_ms != 0U &&
                response.latency_ns > (static_cast<uint64_t>(spec.policy.timeout_ms) * 1000000ULL)) {
                response.ok = false;
                response.flags |= kModelFlagTimeoutExceeded;
            }
            if (response.output.size > spec.policy.max_output_bytes) {
                response.ok = false;
                response.flags |= kModelFlagOutputTooLarge;
            }
            if (response.ok) {
                return ModelRegistry::AsyncModelResult{
                    true,
                    blobs.read_bytes(response.output),
                    response.confidence,
                    response.token_usage,
                    response.flags,
                    response.attempts,
                    response.latency_ns
                };
            }

            last_response = response;
        } catch (const std::exception&) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ModelResponse{
                false,
                {},
                0.0F,
                0U,
                static_cast<uint32_t>(kModelFlagHandlerException | kModelFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        } catch (...) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ModelResponse{
                false,
                {},
                0.0F,
                0U,
                static_cast<uint32_t>(kModelFlagHandlerException | kModelFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        }
    }

    last_response.flags |= kModelFlagRetriesExhausted;
    return ModelRegistry::AsyncModelResult{
        last_response.ok,
        blobs.read_bytes(last_response.output),
        last_response.confidence,
        last_response.token_usage,
        last_response.flags,
        last_response.attempts,
        last_response.latency_ns
    };
}

} // namespace

void ModelRegistry::register_model(std::string_view name, ModelHandler handler) {
    register_model(name, ModelPolicy{}, std::move(handler));
}

void ModelRegistry::register_model(std::string_view name, ModelPolicy policy, ModelHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[std::string(name)] = ModelSpec{policy, std::move(handler)};
}

void ModelRegistry::set_async_completion_listener(AsyncModelCompletionListener listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    async_completion_listener_ = std::move(listener);
}

bool ModelRegistry::has_model(std::string_view name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.find(std::string(name)) != handlers_.end();
}

ModelResponse ModelRegistry::invoke(const ModelRequest& request, ModelInvocationContext& context) const {
    const std::string_view name = context.strings.resolve(request.model_name);
    ModelSpec spec;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handlers_.find(std::string(name));
        if (iterator == handlers_.end()) {
            return ModelResponse{false, {}, 0.0F, 0U, kModelFlagMissingHandler, 0U, 0U};
        }
        spec = iterator->second;
    }

    const std::size_t prompt_bytes = request.prompt.size + request.schema.size;
    if (prompt_bytes > spec.policy.max_prompt_bytes) {
        return ModelResponse{false, {}, 0.0F, 0U, kModelFlagPromptTooLarge, 0U, 0U};
    }

    ModelResponse last_response{false, {}, 0.0F, 0U, kModelFlagRetriesExhausted, 0U, 0U};
    const uint16_t max_attempts = static_cast<uint16_t>(spec.policy.retry_limit + 1U);
    for (uint16_t attempt = 1; attempt <= max_attempts; ++attempt) {
        const auto started_at = std::chrono::steady_clock::now();
        try {
            ModelResponse response = spec.handler(request, context);
            const auto ended_at = std::chrono::steady_clock::now();
            response.attempts = attempt;
            response.latency_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
            );

            if (spec.policy.timeout_ms != 0U &&
                response.latency_ns > (static_cast<uint64_t>(spec.policy.timeout_ms) * 1000000ULL)) {
                response.ok = false;
                response.flags |= kModelFlagTimeoutExceeded;
            }
            if (response.output.size > spec.policy.max_output_bytes) {
                response.ok = false;
                response.flags |= kModelFlagOutputTooLarge;
            }
            if (response.ok) {
                return response;
            }

            last_response = response;
        } catch (const std::exception&) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ModelResponse{
                false,
                {},
                0.0F,
                0U,
                static_cast<uint32_t>(kModelFlagHandlerException | kModelFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        } catch (...) {
            const auto ended_at = std::chrono::steady_clock::now();
            last_response = ModelResponse{
                false,
                {},
                0.0F,
                0U,
                static_cast<uint32_t>(kModelFlagHandlerException | kModelFlagRetriesExhausted),
                attempt,
                static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
                )
            };
        }
    }

    last_response.flags |= kModelFlagRetriesExhausted;
    return last_response;
}

AsyncModelHandle ModelRegistry::begin_invoke_async(
    const ModelRequest& request,
    ModelInvocationContext& context
) const {
    const std::string model_name(context.strings.resolve(request.model_name));
    const std::vector<std::byte> prompt_bytes = context.blobs.read_bytes(request.prompt);
    const std::vector<std::byte> schema_bytes = context.blobs.read_bytes(request.schema);

    ModelSpec spec;
    AsyncModelCompletionListener listener;
    AsyncModelHandle handle;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handlers_.find(model_name);
        if (iterator != handlers_.end()) {
            spec = iterator->second;
        }
        listener = async_completion_listener_;
        handle = AsyncModelHandle{next_async_handle_id_++};
    }

    AsyncModelResult immediate_failure;
    bool use_immediate_failure = false;
    if (spec.handler == nullptr) {
        immediate_failure.ok = false;
        immediate_failure.flags = kModelFlagMissingHandler;
        use_immediate_failure = true;
    } else if (prompt_bytes.size() + schema_bytes.size() > spec.policy.max_prompt_bytes) {
        immediate_failure.ok = false;
        immediate_failure.flags = kModelFlagPromptTooLarge;
        use_immediate_failure = true;
    }

    std::shared_future<AsyncModelResult> future;
    if (use_immediate_failure) {
        future = make_ready_future(std::move(immediate_failure));
    } else {
        auto promise = std::make_shared<std::promise<AsyncModelResult>>();
        future = promise->get_future().share();
        executor_.enqueue([spec,
                           model_name,
                           prompt_bytes,
                           schema_bytes,
                           max_tokens = request.max_tokens,
                           promise,
                           listener,
                           handle]() mutable {
            try {
                promise->set_value(run_model_with_spec(spec, model_name, prompt_bytes, schema_bytes, max_tokens));
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
        async_operations_.emplace(handle.id, AsyncModelOperation{
            model_name,
            prompt_bytes,
            schema_bytes,
            request.max_tokens,
            std::move(future)
        });
    }
    if (use_immediate_failure && listener) {
        listener(handle);
    }
    return handle;
}

bool ModelRegistry::is_async_ready(AsyncModelHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = async_operations_.find(handle.id);
    if (iterator == async_operations_.end()) {
        return false;
    }
    return iterator->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

bool ModelRegistry::has_async_handle(AsyncModelHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return async_operations_.find(handle.id) != async_operations_.end();
}

std::optional<AsyncModelSnapshot> ModelRegistry::export_async_operation(AsyncModelHandle handle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto iterator = async_operations_.find(handle.id);
    if (iterator == async_operations_.end()) {
        return std::nullopt;
    }

    AsyncModelSnapshot snapshot;
    snapshot.handle = handle;
    snapshot.model_name = iterator->second.model_name;
    snapshot.prompt = iterator->second.prompt_bytes;
    snapshot.schema = iterator->second.schema_bytes;
    snapshot.max_tokens = iterator->second.max_tokens;
    if (iterator->second.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const AsyncModelResult result = iterator->second.future.get();
        snapshot.result_ready = true;
        snapshot.ok = result.ok;
        snapshot.output = result.output;
        snapshot.confidence = result.confidence;
        snapshot.token_usage = result.token_usage;
        snapshot.flags = result.flags;
        snapshot.attempts = result.attempts;
        snapshot.latency_ns = result.latency_ns;
    }
    return snapshot;
}

AsyncModelHandle ModelRegistry::restore_async_operation(const AsyncModelSnapshot& snapshot) const {
    ModelSpec spec;
    bool has_spec = false;
    AsyncModelCompletionListener listener;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto iterator = handlers_.find(snapshot.model_name);
        if (iterator != handlers_.end()) {
            spec = iterator->second;
            has_spec = true;
        }
        listener = async_completion_listener_;
    }

    std::shared_future<AsyncModelResult> future;
    const bool completes_immediately = snapshot.result_ready ||
        !has_spec ||
        snapshot.prompt.size() + snapshot.schema.size() > spec.policy.max_prompt_bytes;
    if (snapshot.result_ready) {
        future = make_ready_future(AsyncModelResult{
            snapshot.ok,
            snapshot.output,
            snapshot.confidence,
            snapshot.token_usage,
            snapshot.flags,
            snapshot.attempts,
            snapshot.latency_ns
        });
    } else if (!has_spec) {
        future = make_ready_future(AsyncModelResult{
            false,
            {},
            0.0F,
            0U,
            kModelFlagMissingHandler,
            0U,
            0U
        });
    } else if (snapshot.prompt.size() + snapshot.schema.size() > spec.policy.max_prompt_bytes) {
        future = make_ready_future(AsyncModelResult{
            false,
            {},
            0.0F,
            0U,
            kModelFlagPromptTooLarge,
            0U,
            0U
        });
    } else {
        auto promise = std::make_shared<std::promise<AsyncModelResult>>();
        future = promise->get_future().share();
        executor_.enqueue([spec,
                           model_name = snapshot.model_name,
                           prompt_bytes = snapshot.prompt,
                           schema_bytes = snapshot.schema,
                           max_tokens = snapshot.max_tokens,
                           promise,
                           listener,
                           handle = snapshot.handle]() mutable {
            try {
                promise->set_value(run_model_with_spec(spec, model_name, prompt_bytes, schema_bytes, max_tokens));
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
        async_operations_[snapshot.handle.id] = AsyncModelOperation{
            snapshot.model_name,
            snapshot.prompt,
            snapshot.schema,
            snapshot.max_tokens,
            std::move(future)
        };
    }
    if (completes_immediately && listener) {
        listener(snapshot.handle);
    }
    return snapshot.handle;
}

std::optional<ModelResponse> ModelRegistry::take_async_result(
    AsyncModelHandle handle,
    ModelInvocationContext& context
) const {
    std::shared_future<AsyncModelResult> future;
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

    AsyncModelResult result = future.get();
    BlobRef output_ref{};
    if (!result.output.empty()) {
        output_ref = context.blobs.append(result.output.data(), result.output.size());
    }
    return ModelResponse{
        result.ok,
        output_ref,
        result.confidence,
        result.token_usage,
        result.flags,
        result.attempts,
        result.latency_ns
    };
}

std::size_t ModelRegistry::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return handlers_.size();
}

std::vector<ModelRegistry::NamedModelSpec> ModelRegistry::registered_models() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<NamedModelSpec> models;
    models.reserve(handlers_.size());
    for (const auto& [name, spec] : handlers_) {
        models.push_back(NamedModelSpec{name, spec});
    }
    return models;
}

} // namespace agentcore
