#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/state/state_store.h"

#include <string>

namespace agentcore {

void register_llm_http_adapter(
    ModelRegistry& registry,
    std::string_view model_name,
    const LlmHttpAdapterOptions& options
) {
    registry.register_model(model_name, options.policy, [options](const ModelRequest& request, ModelInvocationContext& context) {
        if (!options.enable_mock_transport) {
            return ModelResponse{false, {}, 0.0F, 0U, kModelFlagUnsupportedRequest};
        }

        const std::string_view prompt = context.blobs.read_string(request.prompt);
        const std::string_view schema = context.blobs.read_string(request.schema);
        if (prompt.empty()) {
            return ModelResponse{false, {}, 0.0F, 0U, kModelFlagValidationError};
        }

        std::string output = "llm_http::";
        output += std::string(prompt);
        if (!schema.empty()) {
            output += "::schema=";
            output += std::string(schema);
        }

        return ModelResponse{
            true,
            context.blobs.append_string(output),
            0.9F,
            static_cast<uint32_t>(prompt.size()),
            kModelFlagNone
        };
    });
}

} // namespace agentcore
