#ifndef AGENTCORE_ADAPTER_FACTORIES_H
#define AGENTCORE_ADAPTER_FACTORIES_H

#include "agentcore/adapters/common/adapter_metadata.h"
#include "agentcore/adapters/common/http_transport.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"

#include <string_view>

namespace agentcore {

struct HttpToolAdapterOptions {
    ToolPolicy policy{};
    bool enable_mock_scheme{true};
    bool enable_file_scheme{true};
    AdapterMetadata metadata{};
};

struct SqliteToolAdapterOptions {
    ToolPolicy policy{};
    AdapterMetadata metadata{};
};

struct LocalModelAdapterOptions {
    ModelPolicy policy{};
    uint32_t default_max_tokens{256};
    AdapterMetadata metadata{};
};

struct LlmHttpAdapterOptions {
    ModelPolicy policy{};
    bool enable_mock_transport{true};
    AdapterMetadata metadata{};
};

struct JsonHttpToolAdapterOptions {
    ToolPolicy policy{};
    HttpTransportOptions transport{};
    std::string default_method{"POST"};
    AdapterMetadata metadata{};
};

struct OpenAiChatModelAdapterOptions {
    ModelPolicy policy{};
    HttpTransportOptions transport{};
    std::string provider_model_name;
    std::string endpoint_path{"/chat/completions"};
    std::string system_prompt;
    bool include_json_schema{true};
    AdapterMetadata metadata{};
};

struct GrokChatModelAdapterOptions {
    ModelPolicy policy{};
    HttpTransportOptions transport{};
    std::string provider_model_name;
    std::string endpoint_path{"/chat/completions"};
    std::string system_prompt;
    bool include_json_schema{true};
    AdapterMetadata metadata{};
};

struct GeminiGenerateContentModelAdapterOptions {
    ModelPolicy policy{};
    HttpTransportOptions transport{};
    std::string provider_model_name;
    std::string endpoint_path;
    std::string system_prompt;
    AdapterMetadata metadata{};
};

void register_http_tool_adapter(
    ToolRegistry& registry,
    std::string_view tool_name = "http_tool",
    const HttpToolAdapterOptions& options = {}
);

void register_sqlite_tool_adapter(
    ToolRegistry& registry,
    std::string_view tool_name = "sqlite_tool",
    const SqliteToolAdapterOptions& options = {}
);

void register_http_json_tool_adapter(
    ToolRegistry& registry,
    std::string_view tool_name = "http_json_tool",
    const JsonHttpToolAdapterOptions& options = {}
);

void register_local_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name = "local_model",
    const LocalModelAdapterOptions& options = {}
);

void register_llm_http_adapter(
    ModelRegistry& registry,
    std::string_view model_name = "llm_http",
    const LlmHttpAdapterOptions& options = {}
);

void register_openai_chat_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name = "openai_chat",
    const OpenAiChatModelAdapterOptions& options = {}
);

void register_grok_chat_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name = "grok_chat",
    const GrokChatModelAdapterOptions& options = {}
);

void register_gemini_generate_content_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name = "gemini",
    const GeminiGenerateContentModelAdapterOptions& options = {}
);

} // namespace agentcore

#endif // AGENTCORE_ADAPTER_FACTORIES_H
