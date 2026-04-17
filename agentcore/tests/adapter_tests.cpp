#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/runtime/model_api.h"
#include "agentcore/runtime/tool_api.h"
#include "agentcore/state/state_store.h"

#include <cassert>
#include <iostream>
#include <string>

namespace agentcore {

namespace {

std::string read_blob_as_string(const BlobStore& blobs, BlobRef ref) {
    return std::string(blobs.read_string(ref));
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

    const ToolResponse http_response = tools.invoke(
        ToolRequest{
            strings.intern("http"),
            blobs.append_string("url=mock://echo\nbody=hello-agentcore")
        },
        tool_context
    );
    assert(http_response.ok);
    assert(read_blob_as_string(blobs, http_response.output) == "hello-agentcore");

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

    ModelRegistry models;
    register_local_model_adapter(models, "local");
    register_llm_http_adapter(models, "llm");

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

    std::cout << "adapter tests passed" << std::endl;
    return 0;
}
