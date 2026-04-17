#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/state/state_store.h"

#include <algorithm>
#include <string>

namespace agentcore {

void register_local_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name,
    const LocalModelAdapterOptions& options
) {
    registry.register_model(model_name, options.policy, [options](const ModelRequest& request, ModelInvocationContext& context) {
        const std::string_view prompt = context.blobs.read_string(request.prompt);
        if (prompt.empty()) {
            return ModelResponse{false, {}, 0.0F, 0U, kModelFlagValidationError};
        }

        const std::size_t output_limit = std::min<std::size_t>(
            request.max_tokens == 0U ? options.default_max_tokens : request.max_tokens,
            prompt.size()
        );
        std::string output = "local::";
        output.append(prompt.substr(0U, output_limit).data(), output_limit);
        return ModelResponse{
            true,
            context.blobs.append_string(output),
            0.85F,
            static_cast<uint32_t>(output_limit),
            kModelFlagNone
        };
    });
}

} // namespace agentcore
