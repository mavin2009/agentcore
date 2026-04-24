#ifndef AGENTCORE_ADAPTER_METADATA_H
#define AGENTCORE_ADAPTER_METADATA_H

#include <cstdint>
#include <string>
#include <string_view>

namespace agentcore {

enum class AdapterTransportKind : uint8_t {
    Unknown = 0,
    InProcess,
    Stdio,
    Http,
    Database,
    FileSystem
};

enum class AdapterAuthKind : uint8_t {
    Unknown = 0,
    None,
    ApiKey,
    BearerToken,
    Basic,
    ConnectionString,
    FilePath
};

enum AdapterCapability : uint64_t {
    kAdapterCapabilityNone = 0ULL,
    kAdapterCapabilitySync = 1ULL << 0,
    kAdapterCapabilityAsync = 1ULL << 1,
    kAdapterCapabilityStreaming = 1ULL << 2,
    kAdapterCapabilityStructuredRequest = 1ULL << 3,
    kAdapterCapabilityStructuredResponse = 1ULL << 4,
    kAdapterCapabilityCheckpointSafe = 1ULL << 5,
    kAdapterCapabilityExternalNetwork = 1ULL << 6,
    kAdapterCapabilityLocalFilesystem = 1ULL << 7,
    kAdapterCapabilityJsonSchema = 1ULL << 8,
    kAdapterCapabilityToolCalling = 1ULL << 9,
    kAdapterCapabilitySql = 1ULL << 10,
    kAdapterCapabilityChatMessages = 1ULL << 11
};

struct AdapterMetadata {
    std::string provider;
    std::string implementation;
    std::string display_name;
    AdapterTransportKind transport{AdapterTransportKind::Unknown};
    AdapterAuthKind auth{AdapterAuthKind::Unknown};
    uint64_t capabilities{kAdapterCapabilityNone};
    std::string request_format;
    std::string response_format;
};

[[nodiscard]] inline bool adapter_has_capability(
    const AdapterMetadata& metadata,
    AdapterCapability capability
) noexcept {
    return (metadata.capabilities & static_cast<uint64_t>(capability)) != 0ULL;
}

[[nodiscard]] inline AdapterMetadata resolve_adapter_metadata(
    const AdapterMetadata& overrides,
    AdapterMetadata defaults
) {
    if (!overrides.provider.empty()) {
        defaults.provider = overrides.provider;
    }
    if (!overrides.implementation.empty()) {
        defaults.implementation = overrides.implementation;
    }
    if (!overrides.display_name.empty()) {
        defaults.display_name = overrides.display_name;
    }
    if (overrides.transport != AdapterTransportKind::Unknown) {
        defaults.transport = overrides.transport;
    }
    if (overrides.auth != AdapterAuthKind::Unknown) {
        defaults.auth = overrides.auth;
    }
    if (overrides.capabilities != kAdapterCapabilityNone) {
        defaults.capabilities = overrides.capabilities;
    }
    if (!overrides.request_format.empty()) {
        defaults.request_format = overrides.request_format;
    }
    if (!overrides.response_format.empty()) {
        defaults.response_format = overrides.response_format;
    }
    return defaults;
}

[[nodiscard]] inline std::string_view adapter_transport_name(AdapterTransportKind kind) noexcept {
    switch (kind) {
        case AdapterTransportKind::InProcess:
            return "in_process";
        case AdapterTransportKind::Stdio:
            return "stdio";
        case AdapterTransportKind::Http:
            return "http";
        case AdapterTransportKind::Database:
            return "database";
        case AdapterTransportKind::FileSystem:
            return "filesystem";
        case AdapterTransportKind::Unknown:
        default:
            return "unknown";
    }
}

[[nodiscard]] inline std::string_view adapter_auth_name(AdapterAuthKind kind) noexcept {
    switch (kind) {
        case AdapterAuthKind::None:
            return "none";
        case AdapterAuthKind::ApiKey:
            return "api_key";
        case AdapterAuthKind::BearerToken:
            return "bearer_token";
        case AdapterAuthKind::Basic:
            return "basic";
        case AdapterAuthKind::ConnectionString:
            return "connection_string";
        case AdapterAuthKind::FilePath:
            return "file_path";
        case AdapterAuthKind::Unknown:
        default:
            return "unknown";
    }
}

} // namespace agentcore

#endif // AGENTCORE_ADAPTER_METADATA_H
