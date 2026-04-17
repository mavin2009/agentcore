#ifndef AGENTCORE_ADAPTER_FACTORIES_H
#define AGENTCORE_ADAPTER_FACTORIES_H

#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"

#include <string_view>

namespace agentcore {

struct HttpToolAdapterOptions {
    ToolPolicy policy{};
    bool enable_mock_scheme{true};
    bool enable_file_scheme{true};
};

struct SqliteToolAdapterOptions {
    ToolPolicy policy{};
};

struct LocalModelAdapterOptions {
    ModelPolicy policy{};
    uint32_t default_max_tokens{256};
};

struct LlmHttpAdapterOptions {
    ModelPolicy policy{};
    bool enable_mock_transport{true};
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

} // namespace agentcore

#endif // AGENTCORE_ADAPTER_FACTORIES_H
