#include "agentcore/adapters/adapter_factories.h"

namespace agentcore {

namespace {

AdapterMetadata default_grok_chat_metadata() {
    AdapterMetadata metadata;
    metadata.provider = "xai";
    metadata.implementation = "grok_chat";
    metadata.display_name = "xAI Grok Chat Model Adapter";
    metadata.transport = AdapterTransportKind::Http;
    metadata.auth = AdapterAuthKind::BearerToken;
    metadata.capabilities =
        static_cast<uint64_t>(kAdapterCapabilitySync) |
        static_cast<uint64_t>(kAdapterCapabilityAsync) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredRequest) |
        static_cast<uint64_t>(kAdapterCapabilityStructuredResponse) |
        static_cast<uint64_t>(kAdapterCapabilityCheckpointSafe) |
        static_cast<uint64_t>(kAdapterCapabilityExternalNetwork) |
        static_cast<uint64_t>(kAdapterCapabilityJsonSchema) |
        static_cast<uint64_t>(kAdapterCapabilityChatMessages);
    metadata.request_format = "openai_chat_completions_json";
    metadata.response_format = "openai_chat_completion_json";
    return metadata;
}

} // namespace

void register_grok_chat_model_adapter(
    ModelRegistry& registry,
    std::string_view model_name,
    const GrokChatModelAdapterOptions& options
) {
    OpenAiChatModelAdapterOptions forwarded;
    forwarded.policy = options.policy;
    forwarded.transport = options.transport;
    forwarded.provider_model_name = options.provider_model_name;
    forwarded.endpoint_path = options.endpoint_path;
    forwarded.system_prompt = options.system_prompt;
    forwarded.include_json_schema = options.include_json_schema;
    forwarded.metadata = resolve_adapter_metadata(options.metadata, default_grok_chat_metadata());

    if (forwarded.transport.base_url.empty()) {
        forwarded.transport.base_url = "https://api.x.ai/v1";
    }
    if (forwarded.transport.bearer_token.empty() && forwarded.transport.bearer_token_env_var.empty()) {
        forwarded.transport.bearer_token_env_var = "XAI_API_KEY";
    }

    register_openai_chat_model_adapter(registry, model_name, forwarded);
}

} // namespace agentcore
