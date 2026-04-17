#include "agentcore/execution/streaming/public_stream.h"

#include <algorithm>

namespace agentcore {

namespace {

std::string namespace_node_name(const GraphDefinition& graph, NodeId node_id) {
    const NodeDefinition* node = graph.find_node(node_id);
    if (node == nullptr) {
        return {};
    }
    if (node->kind == NodeKind::Subgraph &&
        node->subgraph.has_value() &&
        !node->subgraph->namespace_name.empty()) {
        return node->subgraph->namespace_name;
    }
    return node->name;
}

std::vector<StreamNamespaceFrame> expand_namespace_frames(
    const std::vector<ExecutionNamespaceRef>& namespace_path,
    const StreamGraphLookupFn& graph_lookup
) {
    std::vector<StreamNamespaceFrame> namespaces;
    namespaces.reserve(namespace_path.size());
    for (const ExecutionNamespaceRef& ref : namespace_path) {
        StreamNamespaceFrame frame;
        frame.graph_id = ref.graph_id;
        frame.node_id = ref.node_id;
        if (const GraphDefinition* graph = graph_lookup(ref.graph_id); graph != nullptr) {
            frame.graph_name = graph->name;
            frame.node_name = namespace_node_name(*graph, ref.node_id);
        }
        namespaces.push_back(std::move(frame));
    }
    return namespaces;
}

} // namespace

std::vector<StreamEvent> build_public_stream_events(
    const TraceSink& trace_sink,
    RunId run_id,
    StreamCursor& cursor,
    const StreamReadOptions& options,
    const StreamGraphLookupFn& graph_lookup
) {
    const std::vector<TraceEvent> trace_events =
        trace_sink.events_for_run_since_sequence(run_id, cursor.next_sequence);

    std::vector<StreamEvent> stream_events;
    stream_events.reserve(trace_events.size());
    uint64_t max_sequence = cursor.next_sequence;
    for (const TraceEvent& event : trace_events) {
        max_sequence = std::max(max_sequence, event.sequence + 1U);
        if (!options.include_subgraphs && !event.namespace_path.empty()) {
            continue;
        }

        StreamEvent stream_event;
        stream_event.sequence = event.sequence;
        stream_event.run_id = event.run_id;
        stream_event.graph_id = event.graph_id;
        stream_event.node_id = event.node_id;
        stream_event.branch_id = event.branch_id;
        stream_event.checkpoint_id = event.checkpoint_id;
        stream_event.node_status = event.result;
        stream_event.confidence = event.confidence;
        stream_event.patch_count = event.patch_count;
        stream_event.flags = event.flags;
        stream_event.namespaces = expand_namespace_frames(event.namespace_path, graph_lookup);
        stream_events.push_back(std::move(stream_event));
    }

    cursor.next_sequence = max_sequence;
    return stream_events;
}

} // namespace agentcore
