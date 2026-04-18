#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/state/state_store.h"

#include <string>

namespace agentcore {

namespace {

AdapterMetadata default_http_json_tool_metadata() {
    AdapterMetadata metadata;
    metadata.provider = "agentcore";
    metadata.implementation = "http_json_tool";
    metadata.display_name = "HTTP JSON Tool Adapter";
    metadata.transport = AdapterTransportKind::Http;
    metadata.auth = AdapterAuthKind::BearerToken;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredRequest) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredResponse) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe) |
        static_cast<uint64_t>(kAdapterCapabilityExternalNetwork);
    metadata.request_format = "line_map:path|url,method,json|body,header.*";
    metadata.response_format = "json_text";
    return metadata;
}

uint32_t tool_flags_from_http_response(const HttpResponse& response) {
    if (!response.transport_ok) {
        if (response.error_message.rfind("missing ", 0U) == 0U) {
            return kToolFlagValidationError;
        }
        return kToolFlagHandlerException;
    }
    if (response.status_code >= 200L && response.status_code < 300L) {
        return kToolFlagNone;
    }
    if (response.status_code == 408L || response.status_code == 504L) {
        return kToolFlagTimeoutExceeded;
    }
    if (response.status_code == 404L || response.status_code == 405L) {
        return kToolFlagUnsupportedRequest;
    }
    if (response.status_code >= 400L && response.status_code < 500L) {
        return kToolFlagValidationError;
    }
    return kToolFlagHandlerException;
}

} // namespace

void register_http_json_tool_adapter(
    ToolRegistry& registry,
    std::string_view tool_name,
    const JsonHttpToolAdapterOptions& options
) {
    const AdapterMetadata metadata = resolve_adapter_metadata(
        options.metadata,
        default_http_json_tool_metadata()
    );
    registry.register_tool(
        tool_name,
        options.policy,
        metadata,
        [options, auth_kind = metadata.auth](const ToolRequest& request, ToolInvocationContext& context) {
            const std::string_view payload = context.blobs.read_string(request.input);
            const auto values = parse_line_value_map(payload);

            const auto path_iterator = values.find("path");
            const auto url_iterator = values.find("url");
            if (path_iterator == values.end() && url_iterator == values.end()) {
                return ToolResponse{false, {}, kToolFlagValidationError};
            }

            HttpRequest http_request;
            http_request.method = values.count("method") == 0U ? options.default_method : values.at("method");
            http_request.url = path_iterator != values.end() ? path_iterator->second : url_iterator->second;
            if (values.count("json") != 0U) {
                http_request.body = values.at("json");
                http_request.headers.push_back(HttpHeader{"Content-Type", "application/json"});
                http_request.headers.push_back(HttpHeader{"Accept", "application/json"});
            } else if (values.count("body") != 0U) {
                http_request.body = values.at("body");
            }
            if (values.count("timeout_ms") != 0U) {
                http_request.timeout_ms = static_cast<uint32_t>(std::stoul(values.at("timeout_ms")));
            } else {
                http_request.timeout_ms = options.policy.timeout_ms;
            }

            for (const auto& [key, value] : values) {
                constexpr std::string_view prefix = "header.";
                if (key.rfind(prefix, 0U) == 0U) {
                    http_request.headers.push_back(HttpHeader{key.substr(prefix.size()), value});
                }
            }

            const HttpResponse http_response = invoke_http_transport(
                options.transport,
                http_request,
                auth_kind
            );
            const uint32_t flags = tool_flags_from_http_response(http_response);
            BlobRef output_ref{};
            if (!http_response.body.empty()) {
                output_ref = context.blobs.append_string(http_response.body);
            }
            return ToolResponse{
                flags == kToolFlagNone,
                output_ref,
                flags
            };
        }
    );
}

} // namespace agentcore
