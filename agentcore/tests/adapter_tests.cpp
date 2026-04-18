#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/state/state_store.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

namespace agentcore {

namespace {

std::string read_blob_as_string(const BlobStore& blobs, BlobRef ref) {
    return std::string(blobs.read_string(ref));
}

bool has_header(
    const std::vector<HttpHeader>& headers,
    std::string_view name,
    std::string_view value
) {
    for (const HttpHeader& header : headers) {
        if (header.name == name && header.value == value) {
            return true;
        }
    }
    return false;
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    StateStore store;
    BlobStore& blobs = store.blobs();
    StringInterner& strings = store.strings();
    ToolInvocationContext tool_context{blobs, strings};
    ModelInvocationContext model_context{blobs, strings};

    ToolRegistry tools;
    register_http_tool_adapter(tools, "http");
    register_sqlite_tool_adapter(tools, "sqlite");
    register_http_json_tool_adapter(
        tools,
        "http_json",
        JsonHttpToolAdapterOptions{
            {},
            HttpTransportOptions{
                [](const HttpRequest& request) {
                    assert(request.method == "POST");
                    assert(request.url == "https://api.example.test/v1/echo");
                    assert(has_header(request.headers, "Authorization", "Bearer test-tool-token"));
                    assert(has_header(request.headers, "Content-Type", "application/json"));
                    assert(has_header(request.headers, "Accept", "application/json"));
                    assert(has_header(request.headers, "X-Trace", "trace-123"));
                    assert(request.body == "{\"hello\":\"world\"}");
                    return HttpResponse{
                        true,
                        200L,
                        {},
                        "{\"echoed\":true}",
                        "",
                        0U
                    };
                },
                "https://api.example.test",
                {},
                "test-tool-token"
            },
            "POST",
            {}
        }
    );

    const auto http_spec = tools.describe_tool("http");
    assert(http_spec.has_value());
    assert(http_spec->spec.metadata.provider == "agentcore");
    assert(http_spec->spec.metadata.implementation == "http_tool");
    assert(http_spec->spec.metadata.transport == AdapterTransportKind::Http);
    assert(http_spec->spec.metadata.auth == AdapterAuthKind::None);
    assert(adapter_has_capability(http_spec->spec.metadata, kAdapterCapabilitySync));
    assert(adapter_has_capability(http_spec->spec.metadata, kAdapterCapabilityAsync));
    assert(adapter_has_capability(http_spec->spec.metadata, kAdapterCapabilityStructuredRequest));
    assert(adapter_has_capability(http_spec->spec.metadata, kAdapterCapabilityLocalFilesystem));
    assert(http_spec->spec.metadata.request_format == "line_map:url,method,body");
    assert(http_spec->spec.metadata.response_format == "raw_text_or_json_text");

    const auto sqlite_spec = tools.describe_tool("sqlite");
    assert(sqlite_spec.has_value());
    assert(sqlite_spec->spec.metadata.transport == AdapterTransportKind::Database);
    assert(sqlite_spec->spec.metadata.auth == AdapterAuthKind::ConnectionString);
    assert(adapter_has_capability(sqlite_spec->spec.metadata, kAdapterCapabilitySql));
    assert(adapter_has_capability(sqlite_spec->spec.metadata, kAdapterCapabilityCheckpointSafe));
    assert(!tools.describe_tool("missing").has_value());

    const auto http_json_spec = tools.describe_tool("http_json");
    assert(http_json_spec.has_value());
    assert(http_json_spec->spec.metadata.implementation == "http_json_tool");
    assert(http_json_spec->spec.metadata.auth == AdapterAuthKind::BearerToken);
    assert(adapter_has_capability(http_json_spec->spec.metadata, kAdapterCapabilityStructuredResponse));

    const ToolResponse http_response = tools.invoke(
        ToolRequest{
            strings.intern("http"),
            blobs.append_string("url=mock://echo\nbody=hello-agentcore")
        },
        tool_context
    );
    assert(http_response.ok);
    assert(read_blob_as_string(blobs, http_response.output) == "hello-agentcore");
    assert(classify_tool_response_flags(http_response.flags) == ToolErrorCategory::None);
    assert(tool_error_category_name(classify_tool_response_flags(http_response.flags)) == "none");

    const ToolResponse http_json_response = tools.invoke(
        ToolRequest{
            strings.intern("http_json"),
            blobs.append_string(
                "path=/v1/echo\n"
                "json={\"hello\":\"world\"}\n"
                "header.X-Trace=trace-123"
            )
        },
        tool_context
    );
    assert(http_json_response.ok);
    assert(read_blob_as_string(blobs, http_json_response.output) == "{\"echoed\":true}");

    const ToolResponse put_response = tools.invoke(
        ToolRequest{
            strings.intern("sqlite"),
            blobs.append_string("action=put\ntable=kv\nkey=runtime\nvalue=fast")
        },
        tool_context
    );
    assert(put_response.ok);
    assert(read_blob_as_string(blobs, put_response.output) == "ok");

    const ToolResponse get_response = tools.invoke(
        ToolRequest{
            strings.intern("sqlite"),
            blobs.append_string("action=get\ntable=kv\nkey=runtime")
        },
        tool_context
    );
    assert(get_response.ok);
    assert(read_blob_as_string(blobs, get_response.output) == "fast");

    const ToolResponse sqlite_missing_response = tools.invoke(
        ToolRequest{
            strings.intern("sqlite"),
            blobs.append_string("action=get\ntable=kv\nkey=missing")
        },
        tool_context
    );
    assert(!sqlite_missing_response.ok);
    assert(classify_tool_response_flags(sqlite_missing_response.flags) == ToolErrorCategory::Validation);
    assert(tool_error_category_name(classify_tool_response_flags(sqlite_missing_response.flags)) == "validation");

    const ToolResponse missing_tool_response = tools.invoke(
        ToolRequest{
            strings.intern("missing_tool"),
            blobs.append_string("url=mock://echo")
        },
        tool_context
    );
    assert(!missing_tool_response.ok);
    assert(classify_tool_response_flags(missing_tool_response.flags) == ToolErrorCategory::MissingHandler);

    ToolRegistry unauthenticated_tools;
    register_http_json_tool_adapter(
        unauthenticated_tools,
        "http_json_missing_auth",
        JsonHttpToolAdapterOptions{
            {},
            HttpTransportOptions{
                nullptr,
                "https://api.example.test"
            },
            "POST",
            {}
        }
    );
    const ToolResponse missing_auth_tool_response = unauthenticated_tools.invoke(
        ToolRequest{
            strings.intern("http_json_missing_auth"),
            blobs.append_string("path=/v1/echo\njson={}")
        },
        tool_context
    );
    assert(!missing_auth_tool_response.ok);
    assert(classify_tool_response_flags(missing_auth_tool_response.flags) == ToolErrorCategory::Validation);

    ModelRegistry models;
    register_local_model_adapter(models, "local");
    register_llm_http_adapter(models, "llm");
    register_openai_chat_model_adapter(
        models,
        "openai_chat",
        OpenAiChatModelAdapterOptions{
            {},
            HttpTransportOptions{
                [](const HttpRequest& request) {
                    assert(request.method == "POST");
                    assert(request.url == "https://api.openai.test/v1/chat/completions");
                    assert(has_header(request.headers, "Authorization", "Bearer sk-test"));
                    assert(has_header(request.headers, "Content-Type", "application/json"));
                    assert(request.body.find("\"model\":\"gpt-test\"") != std::string::npos);
                    assert(request.body.find("\"role\":\"system\"") != std::string::npos);
                    assert(request.body.find("You are terse.") != std::string::npos);
                    assert(request.body.find("\"role\":\"user\"") != std::string::npos);
                    assert(request.body.find("explain adapters") != std::string::npos);
                    assert(request.body.find("\"response_format\"") != std::string::npos);
                    return HttpResponse{
                        true,
                        200L,
                        {},
                        "{\"choices\":[{\"message\":{\"content\":\"adapter answer\"}}],\"usage\":{\"total_tokens\":42}}",
                        "",
                        0U
                    };
                },
                "https://api.openai.test/v1",
                {},
                "sk-test"
            },
            "gpt-test",
            "/chat/completions",
            "You are terse.",
            true,
            {}
        }
    );

    const auto local_spec = models.describe_model("local");
    assert(local_spec.has_value());
    assert(local_spec->spec.metadata.provider == "agentcore");
    assert(local_spec->spec.metadata.transport == AdapterTransportKind::InProcess);
    assert(adapter_has_capability(local_spec->spec.metadata, kAdapterCapabilityStructuredRequest));
    assert(local_spec->spec.metadata.response_format == "text");

    const auto llm_spec = models.describe_model("llm");
    assert(llm_spec.has_value());
    assert(llm_spec->spec.metadata.transport == AdapterTransportKind::Http);
    assert(llm_spec->spec.metadata.auth == AdapterAuthKind::BearerToken);
    assert(adapter_has_capability(llm_spec->spec.metadata, kAdapterCapabilityJsonSchema));
    assert(!models.describe_model("missing").has_value());

    const auto openai_spec = models.describe_model("openai_chat");
    assert(openai_spec.has_value());
    assert(openai_spec->spec.metadata.provider == "openai_compatible");
    assert(adapter_has_capability(openai_spec->spec.metadata, kAdapterCapabilityChatMessages));
    assert(adapter_has_capability(openai_spec->spec.metadata, kAdapterCapabilityExternalNetwork));

    const ModelResponse local_response = models.invoke(
        ModelRequest{
            strings.intern("local"),
            blobs.append_string("planner step"),
            {},
            32U
        },
        model_context
    );
    assert(local_response.ok);
    assert(read_blob_as_string(blobs, local_response.output) == "local::planner step");
    assert(classify_model_response_flags(local_response.flags) == ModelErrorCategory::None);

    const ModelResponse llm_response = models.invoke(
        ModelRequest{
            strings.intern("llm"),
            blobs.append_string("prompt"),
            blobs.append_string("{\"shape\":\"json\"}"),
            64U
        },
        model_context
    );
    assert(llm_response.ok);
    assert(read_blob_as_string(blobs, llm_response.output) == "llm_http::prompt::schema={\"shape\":\"json\"}");

    const ModelResponse openai_response = models.invoke(
        ModelRequest{
            strings.intern("openai_chat"),
            blobs.append_string("explain adapters"),
            blobs.append_string("{\"type\":\"object\"}"),
            128U
        },
        model_context
    );
    assert(openai_response.ok);
    assert(read_blob_as_string(blobs, openai_response.output) == "adapter answer");
    assert(openai_response.token_usage == 42U);

    const ModelResponse invalid_model_response = models.invoke(
        ModelRequest{
            strings.intern("llm"),
            {},
            blobs.append_string("{\"shape\":\"json\"}"),
            64U
        },
        model_context
    );
    assert(!invalid_model_response.ok);
    assert(classify_model_response_flags(invalid_model_response.flags) == ModelErrorCategory::Validation);
    assert(model_error_category_name(classify_model_response_flags(invalid_model_response.flags)) == "validation");

    const ModelResponse missing_model_response = models.invoke(
        ModelRequest{
            strings.intern("missing_model"),
            blobs.append_string("prompt"),
            {},
            16U
        },
        model_context
    );
    assert(!missing_model_response.ok);
    assert(classify_model_response_flags(missing_model_response.flags) == ModelErrorCategory::MissingHandler);

    ModelRegistry unauthenticated_models;
    register_openai_chat_model_adapter(
        unauthenticated_models,
        "openai_missing_auth",
        OpenAiChatModelAdapterOptions{
            {},
            HttpTransportOptions{
                nullptr,
                "https://api.openai.test/v1"
            },
            "gpt-test"
        }
    );
    const ModelResponse missing_auth_model_response = unauthenticated_models.invoke(
        ModelRequest{
            strings.intern("openai_missing_auth"),
            blobs.append_string("prompt"),
            {},
            32U
        },
        model_context
    );
    assert(!missing_auth_model_response.ok);
    assert(classify_model_response_flags(missing_auth_model_response.flags) == ModelErrorCategory::Validation);

    std::cout << "adapter tests passed" << std::endl;
    return 0;
}
