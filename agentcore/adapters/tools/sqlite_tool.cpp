#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/state/state_store.h"

#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace agentcore {

namespace {

struct KeyValueDatabase {
    std::mutex mutex;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> tables;
};

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

} // namespace

void register_sqlite_tool_adapter(
    ToolRegistry& registry,
    std::string_view tool_name,
    const SqliteToolAdapterOptions& options
) {
    const auto database = std::make_shared<KeyValueDatabase>();
    registry.register_tool(tool_name, options.policy, [database](const ToolRequest& request, ToolInvocationContext& context) {
        const std::string_view payload = context.blobs.read_string(request.input);
        const auto values = parse_request_map(payload);

        const auto action_iterator = values.find("action");
        const auto table_iterator = values.find("table");
        const auto key_iterator = values.find("key");
        if (action_iterator == values.end() || table_iterator == values.end() || key_iterator == values.end()) {
            return ToolResponse{false, {}, kToolFlagValidationError};
        }

        const std::string& action = action_iterator->second;
        const std::string& table = table_iterator->second;
        const std::string& key = key_iterator->second;

        std::lock_guard<std::mutex> lock(database->mutex);
        auto& table_map = database->tables[table];

        if (action == "put") {
            const auto value_iterator = values.find("value");
            if (value_iterator == values.end()) {
                return ToolResponse{false, {}, kToolFlagValidationError};
            }
            table_map[key] = value_iterator->second;
            return ToolResponse{true, context.blobs.append_string("ok"), kToolFlagNone};
        }

        if (action == "get") {
            const auto existing = table_map.find(key);
            if (existing == table_map.end()) {
                return ToolResponse{false, {}, kToolFlagValidationError};
            }
            return ToolResponse{true, context.blobs.append_string(existing->second), kToolFlagNone};
        }

        if (action == "delete") {
            const std::size_t erased = table_map.erase(key);
            return ToolResponse{true, context.blobs.append_string(erased == 0U ? "0" : "1"), kToolFlagNone};
        }

        return ToolResponse{false, {}, kToolFlagUnsupportedRequest};
    });
}

} // namespace agentcore
