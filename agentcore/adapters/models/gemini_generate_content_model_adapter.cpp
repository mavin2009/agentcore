#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/state/state_store.h"

#include <optional>
#include <string>

namespace agentcore {

namespace {

AdapterMetadata default_gemini_generate_content_metadata() {
    AdapterMetadata metadata;
    metadata.provider = "google_gemini";
    metadata.implementation = "gemini_generate_content";
    metadata.display_name = "Gemini GenerateContent Model Adapter";
    metadata.transport = AdapterTransportKind::Http;
    metadata.auth = AdapterAuthKind::ApiKey;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredRequest) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredResponse) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe) |
        static_cast<uint64_t>(kAdapterCapabilityExternalNetwork) |
        static_cast<uint64_t>(kAdapterCapabilityJsonSchema);
    metadata.request_format = "gemini_generate_content_json";
    metadata.response_format = "gemini_generate_content_response_json";
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

std::string build_gemini_endpoint_path(
    std::string_view provider_model_name,
    const GeminiGenerateContentModelAdapterOptions& options
) {
    if (!options.endpoint_path.empty()) {
        return options.endpoint_path;
    }
    return "/models/" + std::string(provider_model_name) + ":generateContent";
}

std::string build_gemini_request_body(
    std::string_view prompt,
    std::string_view schema,
    const ModelRequest& request,
    const GeminiGenerateContentModelAdapterOptions& options
) {
    std::string body = "{";
    bool emitted_field = false;
    if (!options.system_prompt.empty()) {
        body += "\"system_instruction\":{\"parts\":[{\"text\":\"";
        body += json_escape_string(options.system_prompt);
        body += "\"}]}";
        emitted_field = true;
    }

    if (emitted_field) {
        body += ",";
    }
    body += "\"contents\":[{\"parts\":[{\"text\":\"";
    body += json_escape_string(prompt);
    body += "\"}]}]";

    const bool emit_generation_config = request.max_tokens != 0U || !schema.empty();
    if (emit_generation_config) {
        body += ",\"generationConfig\":{";
        bool emitted_config = false;
        if (request.max_tokens != 0U) {
            body += "\"maxOutputTokens\":";
            body += std::to_string(request.max_tokens);
            emitted_config = true;
        }
        if (!schema.empty()) {
            if (emitted_config) {
                body += ",";
            }
            body += "\"responseMimeType\":\"application/json\",\"responseJsonSchema\":";
            body += std::string(schema);
        }
        body += "}";
    }

    body += "}";
    return body;
}

std::optional<std::string> extract_gemini_candidate_text(std::string_view json) {
    const std::size_t candidates_pos = json.find("\"candidates\"");
    if (candidates_pos == std::string_view::npos) {
        return extract_json_string_field(json, "text");
    }
    return extract_json_string_field(json.substr(candidates_pos), "text");
}

} // namespace

void register_gemini_generate_content_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name,
    const GeminiGenerateContentModelAdapterOptions& options
) {
    GeminiGenerateContentModelAdapterOptions resolved = options;
    if (resolved.transport.base_url.empty()) {
        resolved.transport.base_url = "https://generativelanguage.googleapis.com/v1beta";
    }
    if (resolved.transport.api_key.empty() && resolved.transport.api_key_env_var.empty()) {
        resolved.transport.api_key_env_var = "GEMINI_API_KEY";
    }
    if (resolved.transport.api_key_header.empty() || resolved.transport.api_key_header == "x-api-key") {
        resolved.transport.api_key_header = "x-goog-api-key";
    }

    registry.register_model(
        model_name,
        resolved.policy,
        resolve_adapter_metadata(resolved.metadata, default_gemini_generate_content_metadata()),
        [resolved, registered_name = std::string(model_name)](const ModelRequest& request, ModelInvocationContext& context) {
            const std::string_view prompt = context.blobs.read_string(request.prompt);
            if (prompt.empty()) {
                return ModelResponse{false, {}, 0.0F, 0U, kModelFlagValidationError};
            }

            const std::string_view schema = context.blobs.read_string(request.schema);
            const std::string_view provider_model_name = resolved.provider_model_name.empty()
                ? std::string_view(registered_name)
                : std::string_view(resolved.provider_model_name);

            HttpRequest http_request;
            http_request.method = "POST";
            http_request.url = build_gemini_endpoint_path(provider_model_name, resolved);
            http_request.timeout_ms = resolved.policy.timeout_ms;
            http_request.headers = {
                HttpHeader{"Content-Type", "application/json"},
                HttpHeader{"Accept", "application/json"}
            };
            http_request.body = build_gemini_request_body(prompt, schema, request, resolved);

            const HttpResponse http_response = invoke_http_transport(
                resolved.transport,
                http_request,
                AdapterAuthKind::ApiKey
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

            const auto content = extract_gemini_candidate_text(http_response.body);
            if (!content.has_value()) {
                return ModelResponse{false, {}, 0.0F, 0U, kModelFlagValidationError};
            }
            const auto total_tokens = extract_json_uint_field(http_response.body, "totalTokenCount");
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
