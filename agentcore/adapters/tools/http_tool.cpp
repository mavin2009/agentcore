#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/state/state_store.h"

#include <fstream>
#include <sstream>
#include <unordered_map>

namespace agentcore {

namespace {

std::unordered_map<std::string, std::string> parse_request_map(std::string_view body) {
    std::unordered_map<std::string, std::string> values;
    std::istringstream input{std::string(body)};
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[line.substr(0, separator)] = line.substr(separator + 1U);
    }
    return values;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

AdapterMetadata default_http_tool_metadata(const HttpToolAdapterOptions& options) {
    AdapterMetadata metadata;
    metadata.provider = "agentcore";
    metadata.implementation = "http_tool";
    metadata.display_name = "HTTP Tool Adapter";
    metadata.transport = AdapterTransportKind::Http;
    metadata.auth = AdapterAuthKind::None;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredRequest) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe);
    if (options.enable_mock_scheme) {
        metadata.capabilities |= static_cast<uint64_t>(kAdapterCapabilityStructuredResponse);
    }
    if (options.enable_file_scheme) {
        metadata.capabilities |= static_cast<uint64_t>(kAdapterCapabilityLocalFilesystem);
    }
    metadata.request_format = "line_map:url,method,body";
    metadata.response_format = "raw_text_or_json_text";
    return metadata;
}

} // namespace

void register_http_tool_adapter(
    ToolRegistry& registry,
    std::string_view tool_name,
    const HttpToolAdapterOptions& options
) {
    registry.register_tool(
        tool_name,
        options.policy,
        resolve_adapter_metadata(options.metadata, default_http_tool_metadata(options)),
        [options](const ToolRequest& request, ToolInvocationContext& context) {
        const std::string_view payload = context.blobs.read_string(request.input);
        const auto values = parse_request_map(payload);
        const auto url_iterator = values.find("url");
        if (url_iterator == values.end()) {
            return ToolResponse{false, {}, kToolFlagValidationError};
        }

        const std::string& url = url_iterator->second;
        const std::string method = values.count("method") == 0U ? "GET" : values.at("method");
        const std::string body = values.count("body") == 0U ? "" : values.at("body");

        if (options.enable_mock_scheme && starts_with(url, "mock://")) {
            if (url == "mock://echo") {
                return ToolResponse{true, context.blobs.append_string(body), kToolFlagNone};
            }
            if (url == "mock://json") {
                const std::string response =
                    std::string("{\"method\":\"") + method +
                    "\",\"url\":\"" + url +
                    "\",\"body\":\"" + body + "\"}";
                return ToolResponse{true, context.blobs.append_string(response), kToolFlagNone};
            }
            return ToolResponse{false, {}, kToolFlagUnsupportedRequest};
        }

        if (options.enable_file_scheme && starts_with(url, "file://")) {
            const std::string path = url.substr(7U);
            std::ifstream input(path.c_str(), std::ios::binary);
            if (!input.is_open()) {
                return ToolResponse{false, {}, kToolFlagValidationError};
            }
            std::ostringstream contents;
            contents << input.rdbuf();
            return ToolResponse{true, context.blobs.append_string(contents.str()), kToolFlagNone};
        }

        return ToolResponse{false, {}, kToolFlagUnsupportedRequest};
    });
}

} // namespace agentcore
