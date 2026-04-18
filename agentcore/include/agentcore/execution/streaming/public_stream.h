#ifndef AGENTCORE_EXECUTION_STREAMING_PUBLIC_STREAM_H
#define AGENTCORE_EXECUTION_STREAMING_PUBLIC_STREAM_H

#include "agentcore/execution/checkpoint.h"
#include "agentcore/graph/graph_ir.h"
#include <functional>
#include <vector>

namespace agentcore {

struct StreamNamespaceFrame {
    GraphId graph_id{0};
    NodeId node_id{0};
    std::string graph_name;
    std::string node_name;
    std::string session_id;
    uint64_t session_revision{0};
};

struct StreamEvent {
    uint64_t sequence{0};
    RunId run_id{0};
    GraphId graph_id{0};
    NodeId node_id{0};
    uint32_t branch_id{0};
    CheckpointId checkpoint_id{0};
    NodeResult::Status node_status{NodeResult::Success};
    float confidence{0.0F};
    uint32_t patch_count{0};
    uint32_t flags{0};
    std::string session_id;
    uint64_t session_revision{0};
    std::vector<StreamNamespaceFrame> namespaces;
};

struct StreamCursor {
    uint64_t next_sequence{1};
};

struct StreamReadOptions {
    bool include_subgraphs{true};
};

using StreamGraphLookupFn = std::function<const GraphDefinition*(GraphId)>;

[[nodiscard]] std::vector<StreamEvent> build_public_stream_events(
    const TraceSink& trace_sink,
    RunId run_id,
    StreamCursor& cursor,
    const StreamReadOptions& options,
    const StreamGraphLookupFn& graph_lookup
);

} // namespace agentcore

#endif // AGENTCORE_EXECUTION_STREAMING_PUBLIC_STREAM_H
