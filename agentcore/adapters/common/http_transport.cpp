#include "agentcore/adapters/common/http_transport.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <sstream>

#ifdef AGENTCORE_HAVE_CURL
#include <curl/curl.h>
#endif

namespace agentcore {

namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0U, prefix.size()) == prefix;
}

std::string trim_copy(std::string_view value) {
    std::size_t start = 0U;
    std::size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
        --end;
    }
    return std::string(value.substr(start, end - start));
}

std::optional<std::string> resolve_auth_value(std::string_view inline_value, std::string_view env_var) {
    if (!inline_value.empty()) {
        return std::string(inline_value);
    }
    if (env_var.empty()) {
        return std::nullopt;
    }
    const char* resolved = std::getenv(std::string(env_var).c_str());
    if (resolved == nullptr || resolved[0] == '\0') {
        return std::nullopt;
    }
    return std::string(resolved);
}

bool apply_http_auth_headers(
    const HttpTransportOptions& options,
    AdapterAuthKind auth_kind,
    std::vector<HttpHeader>& headers,
    std::string& error_message
) {
    if (auth_kind == AdapterAuthKind::None || auth_kind == AdapterAuthKind::Unknown) {
        return true;
    }

    if (auth_kind == AdapterAuthKind::BearerToken) {
        const auto token = resolve_auth_value(options.bearer_token, options.bearer_token_env_var);
        if (!token.has_value()) {
            error_message = "missing bearer token";
            return false;
        }
        headers.push_back(HttpHeader{"Authorization", std::string("Bearer ") + *token});
        return true;
    }

    if (auth_kind == AdapterAuthKind::ApiKey) {
        const auto api_key = resolve_auth_value(options.api_key, options.api_key_env_var);
        if (!api_key.has_value()) {
            error_message = "missing api key";
            return false;
        }
        const std::string header_name = options.api_key_header.empty()
            ? std::string("x-api-key")
            : options.api_key_header;
        headers.push_back(HttpHeader{header_name, *api_key});
        return true;
    }

    error_message = "unsupported auth configuration";
    return false;
}

#ifdef AGENTCORE_HAVE_CURL
size_t curl_write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* output = static_cast<std::string*>(userdata);
    output->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t curl_header_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::vector<HttpHeader>*>(userdata);
    const std::string_view line(buffer, size * nitems);
    const std::size_t separator = line.find(':');
    if (separator != std::string_view::npos) {
        headers->push_back(HttpHeader{
            trim_copy(line.substr(0U, separator)),
            trim_copy(line.substr(separator + 1U))
        });
    }
    return size * nitems;
}

HttpResponse perform_curl_request(const HttpRequest& request) {
    HttpResponse response;
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        response.error_message = "curl_easy_init failed";
        return response;
    }

    struct curl_slist* curl_headers = nullptr;
    for (const HttpHeader& header : request.headers) {
        const std::string rendered = header.name + ": " + header.value;
        curl_headers = curl_slist_append(curl_headers, rendered.c_str());
    }

    std::string response_body;
    std::vector<HttpHeader> response_headers;
    const auto started_at = std::chrono::steady_clock::now();

    curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, curl_headers);
    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, request.method.c_str());
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, &curl_write_callback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, &curl_header_callback);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    if (!request.body.empty()) {
        curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
        curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
    }
    if (request.timeout_ms != 0U) {
        curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout_ms));
    }

    const CURLcode result = curl_easy_perform(handle);
    const auto ended_at = std::chrono::steady_clock::now();
    response.latency_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
    );

    if (result != CURLE_OK) {
        response.transport_ok = false;
        response.error_message = curl_easy_strerror(result);
        curl_slist_free_all(curl_headers);
        curl_easy_cleanup(handle);
        return response;
    }

    long status_code = 0L;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status_code);
    response.transport_ok = true;
    response.status_code = status_code;
    response.body = std::move(response_body);
    response.headers = std::move(response_headers);

    curl_slist_free_all(curl_headers);
    curl_easy_cleanup(handle);
    return response;
}
#endif

} // namespace

std::unordered_map<std::string, std::string> parse_line_value_map(std::string_view body) {
    std::unordered_map<std::string, std::string> values;
    std::istringstream input{std::string(body)};
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        values[line.substr(0U, separator)] = line.substr(separator + 1U);
    }
    return values;
}

std::string join_http_url(std::string_view base_url, std::string_view path) {
    if (starts_with(path, "http://") || starts_with(path, "https://")) {
        return std::string(path);
    }
    if (base_url.empty()) {
        return std::string(path);
    }
    const bool base_has_slash = !base_url.empty() && base_url.back() == '/';
    const bool path_has_slash = !path.empty() && path.front() == '/';
    if (base_has_slash && path_has_slash) {
        return std::string(base_url.substr(0U, base_url.size() - 1U)) + std::string(path);
    }
    if (!base_has_slash && !path_has_slash && !path.empty()) {
        return std::string(base_url) + "/" + std::string(path);
    }
    return std::string(base_url) + std::string(path);
}

std::string json_escape_string(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8U);
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }
    return escaped;
}

std::optional<std::string> extract_json_string_field(std::string_view json, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const std::size_t key_pos = json.find(pattern);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t colon_pos = json.find(':', key_pos + pattern.size());
    if (colon_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t quote_pos = json.find('"', colon_pos + 1U);
    if (quote_pos == std::string_view::npos) {
        return std::nullopt;
    }

    std::string output;
    bool escaping = false;
    for (std::size_t index = quote_pos + 1U; index < json.size(); ++index) {
        const char ch = json[index];
        if (escaping) {
            switch (ch) {
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                default:
                    output.push_back(ch);
                    break;
            }
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return output;
        }
        output.push_back(ch);
    }
    return std::nullopt;
}

std::optional<uint32_t> extract_json_uint_field(std::string_view json, std::string_view key) {
    const std::string pattern = "\"" + std::string(key) + "\"";
    const std::size_t key_pos = json.find(pattern);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t colon_pos = json.find(':', key_pos + pattern.size());
    if (colon_pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t value_pos = colon_pos + 1U;
    while (value_pos < json.size() && std::isspace(static_cast<unsigned char>(json[value_pos])) != 0) {
        ++value_pos;
    }
    std::size_t end_pos = value_pos;
    while (end_pos < json.size() && std::isdigit(static_cast<unsigned char>(json[end_pos])) != 0) {
        ++end_pos;
    }
    if (end_pos == value_pos) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(std::stoul(std::string(json.substr(value_pos, end_pos - value_pos))));
}

std::optional<std::string> extract_openai_message_content(std::string_view json) {
    const std::size_t message_pos = json.find("\"message\"");
    if (message_pos == std::string_view::npos) {
        return extract_json_string_field(json, "content");
    }
    return extract_json_string_field(json.substr(message_pos), "content");
}

HttpResponse invoke_http_transport(
    const HttpTransportOptions& options,
    const HttpRequest& request,
    AdapterAuthKind auth_kind
) {
    HttpRequest resolved = request;
    resolved.url = join_http_url(options.base_url, request.url);
    resolved.headers.insert(
        resolved.headers.begin(),
        options.default_headers.begin(),
        options.default_headers.end()
    );

    HttpResponse response;
    if (!apply_http_auth_headers(options, auth_kind, resolved.headers, response.error_message)) {
        response.transport_ok = false;
        return response;
    }

    if (options.transport) {
        return options.transport(resolved);
    }

#ifdef AGENTCORE_HAVE_CURL
    return perform_curl_request(resolved);
#else
    response.transport_ok = false;
    response.error_message = "no HTTP transport configured and libcurl support is unavailable";
    return response;
#endif
}

} // namespace agentcore
