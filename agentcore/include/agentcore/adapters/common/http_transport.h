#ifndef AGENTCORE_HTTP_TRANSPORT_H
#define AGENTCORE_HTTP_TRANSPORT_H

#include "agentcore/adapters/common/adapter_metadata.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace agentcore {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method{"GET"};
    std::string url;
    std::vector<HttpHeader> headers;
    std::string body;
    uint32_t timeout_ms{0};
};

struct HttpResponse {
    bool transport_ok{false};
    long status_code{0};
    std::vector<HttpHeader> headers;
    std::string body;
    std::string error_message;
    uint64_t latency_ns{0};
};

using HttpTransport = std::function<HttpResponse(const HttpRequest&)>;

struct HttpTransportOptions {
    HttpTransport transport;
    std::string base_url;
    std::vector<HttpHeader> default_headers;
    std::string bearer_token;
    std::string bearer_token_env_var;
    std::string api_key;
    std::string api_key_env_var;
    std::string api_key_header{"x-api-key"};
};

[[nodiscard]] std::unordered_map<std::string, std::string> parse_line_value_map(std::string_view body);
[[nodiscard]] std::string join_http_url(std::string_view base_url, std::string_view path);
[[nodiscard]] std::string json_escape_string(std::string_view value);
[[nodiscard]] std::optional<std::string> extract_json_string_field(std::string_view json, std::string_view key);
[[nodiscard]] std::optional<uint32_t> extract_json_uint_field(std::string_view json, std::string_view key);
[[nodiscard]] std::optional<std::string> extract_openai_message_content(std::string_view json);
[[nodiscard]] HttpResponse invoke_http_transport(
    const HttpTransportOptions& options,
    const HttpRequest& request,
    AdapterAuthKind auth_kind
);

} // namespace agentcore

#endif // AGENTCORE_HTTP_TRANSPORT_H
