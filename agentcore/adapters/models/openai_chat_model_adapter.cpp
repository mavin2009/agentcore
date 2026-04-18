#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/state/state_store.h"

#include <string>

namespace agentcore {

namespace {

AdapterMetadata default_openai_chat_metadata() {
    AdapterMetadata metadata;
    metadata.provider = "openai_compatible";
    metadata.implementation = "openai_chat";
    metadata.display_name = "OpenAI-Compatible Chat Model Adapter";
    metadata.transport = AdapterTransportKind::Http;
    metadata.auth = AdapterAuthKind::BearerToken;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredRequest) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredResponse) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe) |
        static_cast<uint64_t>(kAdapterCapabilityExternalNetwork) |
        static_cast<uint64_t>(kAdapterCapabilityJsonSchema) |
        static_cast<uint64_t>(kAdapterCapabilityChatMessages);
    metadata.request_format = "openai_chat_completions_json";
    metadata.response_format = "openai_chat_completion_json";
    return metadata;
}

uint32_t model_flags_from_http_response(const HttpResponse& response) {
    if (!response.transport_ok) {
        if (response.error_message.rfind("missing ", 0U) == 0U) {
            return kModelFlagValidationError;
        }
        return kModelFlagHandlerException;
    }
    if (response.status_code >= 200L && response.status_code < 300L) {
        return kModelFlagNone;
    }
    if (response.status_code == 408L || response.status_code == 504L) {
        return kModelFlagTimeoutExceeded;
    }
    if (response.status_code == 404L || response.status_code == 405L) {
        return kModelFlagUnsupportedRequest;
    }
    if (response.status_code >= 400L && response.status_code < 500L) {
        return kModelFlagValidationError;
    }
    return kModelFlagHandlerException;
}

std::string build_openai_request_body(
    std::string_view provider_model_name,
    std::string_view prompt,
    std::string_view schema,
    const OpenAiChatModelAdapterOptions& options
) {
    std::string body = "{\"model\":\"";
    body += json_escape_string(provider_model_name);
    body += "\",\"messages\":[";
    bool emitted_message = false;
    if (!options.system_prompt.empty()) {
        body += "{\"role\":\"system\",\"content\":\"";
        body += json_escape_string(options.system_prompt);
        body += "\"}";
        emitted_message = true;
    }
    if (emitted_message) {
        body += ",";
    }
    body += "{\"role\":\"user\",\"content\":\"";
    body += json_escape_string(prompt);
    body += "\"}";
    body += "]";
    if (options.include_json_schema && !schema.empty()) {
        body += ",\"response_format\":{\"type\":\"json_schema\",\"json_schema\":{\"name\":\"agentcore_schema\",\"schema\":";
        body += std::string(schema);
        body += "}}";
    }
    body += "}";
    return body;
}

} // namespace

void register_openai_chat_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name,
    const OpenAiChatModelAdapterOptions& options
) {
    registry.register_model(
        model_name,
        options.policy,
        resolve_adapter_metadata(options.metadata, default_openai_chat_metadata()),
        [options, registered_name = std::string(model_name)](const ModelRequest& request, ModelInvocationContext& context) {
            const std::string_view prompt = context.blobs.read_string(request.prompt);
            if (prompt.empty()) {
                return ModelResponse{false, {}, 0.0F, 0U, kModelFlagValidationError};
            }

            const std::string_view schema = context.blobs.read_string(request.schema);
            const std::string_view provider_model_name = options.provider_model_name.empty()
                ? std::string_view(registered_name)
                : std::string_view(options.provider_model_name);

            HttpRequest http_request;
            http_request.method = "POST";
            http_request.url = options.endpoint_path;
            http_request.timeout_ms = options.policy.timeout_ms;
            http_request.headers = {
                HttpHeader{"Content-Type", "application/json"},
                HttpHeader{"Accept", "application/json"}
            };
            http_request.body = build_openai_request_body(provider_model_name, prompt, schema, options);

            const HttpResponse http_response = invoke_http_transport(
                options.transport,
                http_request,
                AdapterAuthKind::BearerToken
            );
            const uint32_t flags = model_flags_from_http_response(http_response);
            if (flags != kModelFlagNone) {
                BlobRef output_ref{};
                if (!http_response.body.empty()) {
                    output_ref = context.blobs.append_string(http_response.body);
                }
                return ModelResponse{
                    false,
                    output_ref,
                    0.0F,
                    0U,
                    flags
                };
            }

            const auto content = extract_openai_message_content(http_response.body);
            if (!content.has_value()) {
                return ModelResponse{false, {}, 0.0F, 0U, kModelFlagValidationError};
            }
            const auto total_tokens = extract_json_uint_field(http_response.body, "total_tokens");
            return ModelResponse{
                true,
                context.blobs.append_string(*content),
                0.9F,
                total_tokens.value_or(0U),
                kModelFlagNone
            };
        }
    );
}

} // namespace agentcore
