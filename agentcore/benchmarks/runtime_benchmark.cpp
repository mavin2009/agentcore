#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/graph/graph_ir.h"
#include "agentcore/state/context/context_graph.h"
#include "agentcore/state/intelligence/ops.h"
#include "agentcore/state/knowledge_graph.h"
#include "agentcore/state/state_store.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <variant>

namespace agentcore {

namespace {

constexpr ExecutionProfile kBenchmarkExecutionProfile = ExecutionProfile::Balanced;

const char* benchmark_profile_name() noexcept {
    switch (kBenchmarkExecutionProfile) {
        case ExecutionProfile::Strict:
            return "strict";
        case ExecutionProfile::Balanced:
            return "balanced";
        case ExecutionProfile::Fast:
            return "fast";
    }
    return "balanced";
}

ExecutionEngine make_benchmark_engine(std::size_t worker_count, bool inline_scheduler = false) {
    return ExecutionEngine(ExecutionEngineOptions{
        worker_count,
        inline_scheduler,
        kBenchmarkExecutionProfile
    });
}

enum BenchmarkStateKey : StateKey {
    kSummary = 0,
    kAccumulator = 1
};

enum RoutingBenchmarkStateKey : StateKey {
    kRoutingIteration = 0,
    kRoutingRouteKey = 1,
    kRoutingAccumulator = 2,
    kRoutingShouldStop = 3
};

enum SubgraphBenchmarkStateKey : StateKey {
    kSubgraphBenchmarkInput = 0,
    kSubgraphBenchmarkOutput = 1,
    kSubgraphBenchmarkSummary = 2
};

enum SubgraphBenchmarkChildStateKey : StateKey {
    kSubgraphBenchmarkChildInput = 0,
    kSubgraphBenchmarkChildOutput = 1
};

enum ResumableSubgraphBenchmarkStateKey : StateKey {
    kResumableSubgraphBenchmarkInput = 0,
    kResumableSubgraphBenchmarkOutput = 1,
    kResumableSubgraphBenchmarkSummary = 2
};

enum ResumableSubgraphBenchmarkChildStateKey : StateKey {
    kResumableSubgraphBenchmarkChildInput = 0,
    kResumableSubgraphBenchmarkChildOutput = 1,
    kResumableSubgraphBenchmarkChildAttempt = 2
};

enum AsyncSubgraphBenchmarkStateKey : StateKey {
    kAsyncSubgraphBenchmarkResult = 0,
    kAsyncSubgraphBenchmarkSummary = 1
};

enum AsyncMultiWaitSubgraphBenchmarkStateKey : StateKey {
    kAsyncMultiWaitSubgraphBenchmarkResult = 0,
    kAsyncMultiWaitSubgraphBenchmarkSummary = 1
};

enum AsyncMultiWaitSubgraphBenchmarkChildStateKey : StateKey {
    kAsyncMultiWaitSubgraphBenchmarkChildLeft = 0,
    kAsyncMultiWaitSubgraphBenchmarkChildRight = 1,
    kAsyncMultiWaitSubgraphBenchmarkChildSummary = 2
};

enum MemoizationBenchmarkStateKey : StateKey {
    kMemoizationBenchmarkInput = 0,
    kMemoizationBenchmarkOutput = 1,
    kMemoizationBenchmarkVisits = 2
};

constexpr int kRoutingRouteCount = 16;
constexpr int64_t kRoutingIterations = 128;
constexpr int kFrontierSubscriberCount = 64;
constexpr int kFrontierHotSubscriber = 37;
constexpr std::size_t kForkBenchmarkCopies = 24U;
constexpr std::size_t kForkBenchmarkBlobBytes = 256U * 1024U;
constexpr int kForkBenchmarkEntityCount = 1024;
constexpr int kForkBenchmarkTripleCount = 2048;
constexpr std::size_t kSubgraphBenchmarkRuns = 64U;
constexpr std::size_t kRecordedEffectBenchmarkIterations = 4096U;
constexpr std::size_t kMemoizationBenchmarkVisitLimit = 64U;
constexpr std::size_t kMemoizationBenchmarkMixRounds = 262144U;
constexpr std::size_t kKnowledgeReasoningBenchmarkTriples = 8192U;
constexpr std::size_t kKnowledgeReasoningBenchmarkIterations = 4096U;
constexpr std::size_t kKnowledgeIngestBenchmarkTriples = 10000U;
constexpr std::size_t kKnowledgeIngestBenchmarkBatchSize = 100U;
constexpr std::size_t kIntelligenceBenchmarkRecords = 4096U;
constexpr std::size_t kIntelligenceBenchmarkIterations = 4096U;
constexpr std::size_t kContextGraphBenchmarkRecords = 1024U;
constexpr std::size_t kContextGraphBenchmarkIterations = 1024U;

std::atomic<uint64_t> g_memoization_baseline_invocations{0U};
std::atomic<uint64_t> g_memoization_hit_invocations{0U};
std::atomic<uint64_t> g_memoization_invalidation_invocations{0U};

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult fanout_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult write_branch_node(ExecutionContext& context, int64_t value, const char* branch_name) {
    std::this_thread::sleep_for(std::chrono::milliseconds(75));

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kAccumulator, value});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern(branch_name),
        context.strings.intern("writes"),
        context.strings.intern("artifact"),
        context.blobs.append_string(branch_name)
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult branch_a_node(ExecutionContext& context) {
    return write_branch_node(context, 1, "branch_a");
}

NodeResult branch_b_node(ExecutionContext& context) {
    return write_branch_node(context, 2, "branch_b");
}

NodeResult branch_c_node(ExecutionContext& context) {
    return write_branch_node(context, 3, "branch_c");
}

NodeResult branch_d_node(ExecutionContext& context) {
    return write_branch_node(context, 4, "branch_d");
}

int64_t read_int_field(const WorkflowState& state, StateKey key) {
    if (state.size() <= key || !std::holds_alternative<int64_t>(state.load(key))) {
        return 0;
    }
    return std::get<int64_t>(state.load(key));
}

int64_t benchmark_mix_value(int64_t input) {
    uint64_t value = static_cast<uint64_t>(input) ^ 0x9e3779b97f4a7c15ULL;
    for (std::size_t round = 0; round < kMemoizationBenchmarkMixRounds; ++round) {
        value ^= value >> 33U;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33U;
        value += 0x9e3779b97f4a7c15ULL + static_cast<uint64_t>(round);
    }
    return static_cast<int64_t>(value & 0x3fffffffffffffffULL);
}

bool read_bool_field(const WorkflowState& state, StateKey key) {
    return state.size() > key &&
        std::holds_alternative<bool>(state.load(key)) &&
        std::get<bool>(state.load(key));
}

std::vector<TraceEvent> append_trace_events(
    std::vector<TraceEvent> left,
    const std::vector<TraceEvent>& right
) {
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

std::vector<TraceEvent> trace_events_through_checkpoint(
    const std::vector<TraceEvent>& events,
    CheckpointId checkpoint_id
) {
    std::vector<TraceEvent> prefix;
    for (const TraceEvent& event : events) {
        if (event.checkpoint_id <= checkpoint_id) {
            prefix.push_back(event);
        }
    }
    return prefix;
}

NodeResult summarize_node(ExecutionContext& context) {
    const int64_t total = read_int_field(context.state, kAccumulator);
    const std::string summary = "sum=" + std::to_string(total) +
        " triples=" + std::to_string(context.knowledge_graph.triple_count());

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kSummary, context.blobs.append_string(summary)});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult subgraph_benchmark_child_node(ExecutionContext& context) {
    const int64_t input_value = read_int_field(context.state, kSubgraphBenchmarkChildInput);

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kSubgraphBenchmarkChildOutput,
        input_value + 11
    });
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("subgraph"),
        context.strings.intern("bench"),
        context.strings.intern("artifact"),
        {}
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult summarize_subgraph_benchmark_node(ExecutionContext& context) {
    const int64_t output_value = read_int_field(context.state, kSubgraphBenchmarkOutput);

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kSubgraphBenchmarkSummary,
        context.blobs.append_string("subgraph=" + std::to_string(output_value))
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult resumable_subgraph_benchmark_child_node(ExecutionContext& context) {
    const int64_t attempt = read_int_field(context.state, kResumableSubgraphBenchmarkChildAttempt);

    StatePatch patch;
    if (attempt == 0) {
        patch.updates.push_back(FieldUpdate{kResumableSubgraphBenchmarkChildAttempt, int64_t{1}});
        return NodeResult::waiting(std::move(patch), 0.90F);
    }

    const int64_t input_value = read_int_field(context.state, kResumableSubgraphBenchmarkChildInput);
    patch.updates.push_back(FieldUpdate{
        kResumableSubgraphBenchmarkChildOutput,
        input_value + 17
    });
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("resumable_subgraph"),
        context.strings.intern("bench"),
        context.strings.intern("artifact"),
        {}
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult summarize_resumable_subgraph_benchmark_node(ExecutionContext& context) {
    const int64_t output_value = read_int_field(context.state, kResumableSubgraphBenchmarkOutput);

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kResumableSubgraphBenchmarkSummary,
        context.blobs.append_string("resumable-subgraph=" + std::to_string(output_value))
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult async_subgraph_benchmark_child_node(ExecutionContext& context) {
    ToolInvocationContext tool_context{context.blobs, context.strings};
    if (context.pending_async.has_value() &&
        context.pending_async->kind == AsyncOperationKind::Tool) {
        const auto response = context.tools.take_async_result(
            AsyncToolHandle{context.pending_async->handle_id},
            tool_context
        );
        assert(response.has_value());
        assert(response->ok);

        StatePatch patch;
        patch.updates.push_back(FieldUpdate{kAsyncSubgraphBenchmarkResult, response->output});
        return NodeResult::success(std::move(patch), 0.96F);
    }

    const BlobRef request_blob = context.blobs.append_string("benchmark-async");
    const AsyncToolHandle handle = context.tools.begin_invoke_async(
        ToolRequest{context.strings.intern("async_echo"), request_blob},
        tool_context
    );
    assert(handle.valid());
    return NodeResult::waiting_on_tool(handle, {}, 0.70F);
}

NodeResult summarize_async_subgraph_benchmark_node(ExecutionContext& context) {
    std::string async_value;
    if (context.state.size() > kAsyncSubgraphBenchmarkResult &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncSubgraphBenchmarkResult))) {
        async_value = std::string(
            context.blobs.read_string(std::get<BlobRef>(context.state.load(kAsyncSubgraphBenchmarkResult)))
        );
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kAsyncSubgraphBenchmarkSummary,
        context.blobs.append_string("async-subgraph=" + async_value)
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult async_multi_wait_benchmark_child_writer_node(
    ExecutionContext& context,
    StateKey result_key,
    std::string_view request_name,
    std::string_view branch_name
) {
    ToolInvocationContext tool_context{context.blobs, context.strings};
    if (context.pending_async.has_value() &&
        context.pending_async->kind == AsyncOperationKind::Tool) {
        const auto response = context.tools.take_async_result(
            AsyncToolHandle{context.pending_async->handle_id},
            tool_context
        );
        assert(response.has_value());
        assert(response->ok);

        StatePatch patch;
        patch.updates.push_back(FieldUpdate{result_key, response->output});
        patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
            context.strings.intern(branch_name),
            context.strings.intern("bench"),
            context.strings.intern("async_branch"),
            response->output
        });
        return NodeResult::success(std::move(patch), 0.96F);
    }

    const BlobRef request_blob = context.blobs.append_string(std::string(request_name));
    const AsyncToolHandle handle = context.tools.begin_invoke_async(
        ToolRequest{context.strings.intern("async_echo"), request_blob},
        tool_context
    );
    assert(handle.valid());
    return NodeResult::waiting_on_tool(handle, {}, 0.70F);
}

NodeResult async_multi_wait_benchmark_left_node(ExecutionContext& context) {
    return async_multi_wait_benchmark_child_writer_node(
        context,
        kAsyncMultiWaitSubgraphBenchmarkChildLeft,
        "join-left",
        "async_multi_wait_left"
    );
}

NodeResult async_multi_wait_benchmark_right_node(ExecutionContext& context) {
    return async_multi_wait_benchmark_child_writer_node(
        context,
        kAsyncMultiWaitSubgraphBenchmarkChildRight,
        "join-right",
        "async_multi_wait_right"
    );
}

NodeResult summarize_async_multi_wait_benchmark_child_node(ExecutionContext& context) {
    std::string left_value;
    if (context.state.size() > kAsyncMultiWaitSubgraphBenchmarkChildLeft &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncMultiWaitSubgraphBenchmarkChildLeft))) {
        left_value = std::string(
            context.blobs.read_string(
                std::get<BlobRef>(context.state.load(kAsyncMultiWaitSubgraphBenchmarkChildLeft))
            )
        );
    }

    std::string right_value;
    if (context.state.size() > kAsyncMultiWaitSubgraphBenchmarkChildRight &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncMultiWaitSubgraphBenchmarkChildRight))) {
        right_value = std::string(
            context.blobs.read_string(
                std::get<BlobRef>(context.state.load(kAsyncMultiWaitSubgraphBenchmarkChildRight))
            )
        );
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kAsyncMultiWaitSubgraphBenchmarkChildSummary,
        context.blobs.append_string("left=" + left_value + " right=" + right_value)
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult summarize_async_multi_wait_benchmark_parent_node(ExecutionContext& context) {
    std::string child_summary;
    if (context.state.size() > kAsyncMultiWaitSubgraphBenchmarkResult &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncMultiWaitSubgraphBenchmarkResult))) {
        child_summary = std::string(
            context.blobs.read_string(
                std::get<BlobRef>(context.state.load(kAsyncMultiWaitSubgraphBenchmarkResult))
            )
        );
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kAsyncMultiWaitSubgraphBenchmarkSummary,
        context.blobs.append_string("multi-wait-subgraph=" + child_summary)
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult memoization_baseline_node(ExecutionContext& context) {
    g_memoization_baseline_invocations.fetch_add(1U);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kMemoizationBenchmarkOutput,
        benchmark_mix_value(read_int_field(context.state, kMemoizationBenchmarkInput))
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult memoization_hit_node(ExecutionContext& context) {
    g_memoization_hit_invocations.fetch_add(1U);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kMemoizationBenchmarkOutput,
        benchmark_mix_value(read_int_field(context.state, kMemoizationBenchmarkInput))
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult memoization_invalidation_node(ExecutionContext& context) {
    g_memoization_invalidation_invocations.fetch_add(1U);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kMemoizationBenchmarkOutput,
        benchmark_mix_value(read_int_field(context.state, kMemoizationBenchmarkInput))
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult memoization_stable_router_node(ExecutionContext& context) {
    const int64_t next_visit = read_int_field(context.state, kMemoizationBenchmarkVisits) + 1;
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kMemoizationBenchmarkVisits, next_visit});

    NodeResult result = NodeResult::success(std::move(patch), 1.0F);
    result.next_override = next_visit < static_cast<int64_t>(kMemoizationBenchmarkVisitLimit)
        ? NodeId{1U}
        : NodeId{3U};
    return result;
}

NodeResult memoization_invalidation_router_node(ExecutionContext& context) {
    const int64_t next_visit = read_int_field(context.state, kMemoizationBenchmarkVisits) + 1;
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kMemoizationBenchmarkVisits, next_visit});

    NodeResult result = NodeResult::success({}, 1.0F);
    if (next_visit < static_cast<int64_t>(kMemoizationBenchmarkVisitLimit)) {
        patch.updates.push_back(FieldUpdate{
            kMemoizationBenchmarkInput,
            read_int_field(context.state, kMemoizationBenchmarkInput) + 1
        });
        result.patch = std::move(patch);
        result.next_override = NodeId{1U};
        return result;
    }

    result.patch = std::move(patch);
    result.next_override = NodeId{3U};
    return result;
}

std::string frontier_signal_name(int index) {
    return "frontier_signal_" + std::to_string(index);
}

NodeResult emit_frontier_signal_node(ExecutionContext& context) {
    StatePatch patch;
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("agentcore"),
        context.strings.intern(frontier_signal_name(kFrontierHotSubscriber)),
        context.strings.intern("artifact"),
        context.blobs.append_string("frontier")
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

template <int SubscriberIndex>
NodeResult frontier_watch_node(ExecutionContext& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const InternedStringId subject = context.strings.intern("agentcore");
    const InternedStringId relation = context.strings.intern(frontier_signal_name(SubscriberIndex));
    const std::vector<const KnowledgeTriple*> matches =
        context.knowledge_graph.match(subject, relation, std::nullopt);
    if (matches.empty()) {
        return NodeResult::success({}, 0.90F);
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kAccumulator, int64_t{SubscriberIndex + 1}});
    return NodeResult::success(std::move(patch), 0.97F);
}

#define AGENTCORE_FRONTIER_WATCH_CASE(N) \
    case N: \
        return frontier_watch_node<N>;

NodeExecutorFn frontier_watch_executor_for_index(int index) {
    switch (index) {
        AGENTCORE_FRONTIER_WATCH_CASE(0)
        AGENTCORE_FRONTIER_WATCH_CASE(1)
        AGENTCORE_FRONTIER_WATCH_CASE(2)
        AGENTCORE_FRONTIER_WATCH_CASE(3)
        AGENTCORE_FRONTIER_WATCH_CASE(4)
        AGENTCORE_FRONTIER_WATCH_CASE(5)
        AGENTCORE_FRONTIER_WATCH_CASE(6)
        AGENTCORE_FRONTIER_WATCH_CASE(7)
        AGENTCORE_FRONTIER_WATCH_CASE(8)
        AGENTCORE_FRONTIER_WATCH_CASE(9)
        AGENTCORE_FRONTIER_WATCH_CASE(10)
        AGENTCORE_FRONTIER_WATCH_CASE(11)
        AGENTCORE_FRONTIER_WATCH_CASE(12)
        AGENTCORE_FRONTIER_WATCH_CASE(13)
        AGENTCORE_FRONTIER_WATCH_CASE(14)
        AGENTCORE_FRONTIER_WATCH_CASE(15)
        AGENTCORE_FRONTIER_WATCH_CASE(16)
        AGENTCORE_FRONTIER_WATCH_CASE(17)
        AGENTCORE_FRONTIER_WATCH_CASE(18)
        AGENTCORE_FRONTIER_WATCH_CASE(19)
        AGENTCORE_FRONTIER_WATCH_CASE(20)
        AGENTCORE_FRONTIER_WATCH_CASE(21)
        AGENTCORE_FRONTIER_WATCH_CASE(22)
        AGENTCORE_FRONTIER_WATCH_CASE(23)
        AGENTCORE_FRONTIER_WATCH_CASE(24)
        AGENTCORE_FRONTIER_WATCH_CASE(25)
        AGENTCORE_FRONTIER_WATCH_CASE(26)
        AGENTCORE_FRONTIER_WATCH_CASE(27)
        AGENTCORE_FRONTIER_WATCH_CASE(28)
        AGENTCORE_FRONTIER_WATCH_CASE(29)
        AGENTCORE_FRONTIER_WATCH_CASE(30)
        AGENTCORE_FRONTIER_WATCH_CASE(31)
        AGENTCORE_FRONTIER_WATCH_CASE(32)
        AGENTCORE_FRONTIER_WATCH_CASE(33)
        AGENTCORE_FRONTIER_WATCH_CASE(34)
        AGENTCORE_FRONTIER_WATCH_CASE(35)
        AGENTCORE_FRONTIER_WATCH_CASE(36)
        AGENTCORE_FRONTIER_WATCH_CASE(37)
        AGENTCORE_FRONTIER_WATCH_CASE(38)
        AGENTCORE_FRONTIER_WATCH_CASE(39)
        AGENTCORE_FRONTIER_WATCH_CASE(40)
        AGENTCORE_FRONTIER_WATCH_CASE(41)
        AGENTCORE_FRONTIER_WATCH_CASE(42)
        AGENTCORE_FRONTIER_WATCH_CASE(43)
        AGENTCORE_FRONTIER_WATCH_CASE(44)
        AGENTCORE_FRONTIER_WATCH_CASE(45)
        AGENTCORE_FRONTIER_WATCH_CASE(46)
        AGENTCORE_FRONTIER_WATCH_CASE(47)
        AGENTCORE_FRONTIER_WATCH_CASE(48)
        AGENTCORE_FRONTIER_WATCH_CASE(49)
        AGENTCORE_FRONTIER_WATCH_CASE(50)
        AGENTCORE_FRONTIER_WATCH_CASE(51)
        AGENTCORE_FRONTIER_WATCH_CASE(52)
        AGENTCORE_FRONTIER_WATCH_CASE(53)
        AGENTCORE_FRONTIER_WATCH_CASE(54)
        AGENTCORE_FRONTIER_WATCH_CASE(55)
        AGENTCORE_FRONTIER_WATCH_CASE(56)
        AGENTCORE_FRONTIER_WATCH_CASE(57)
        AGENTCORE_FRONTIER_WATCH_CASE(58)
        AGENTCORE_FRONTIER_WATCH_CASE(59)
        AGENTCORE_FRONTIER_WATCH_CASE(60)
        AGENTCORE_FRONTIER_WATCH_CASE(61)
        AGENTCORE_FRONTIER_WATCH_CASE(62)
        AGENTCORE_FRONTIER_WATCH_CASE(63)
        default:
            return nullptr;
    }
}

#undef AGENTCORE_FRONTIER_WATCH_CASE

GraphDefinition make_naive_frontier_benchmark_graph() {
    GraphDefinition graph;
    graph.id = 403;
    graph.name = "naive_frontier_benchmark_graph";
    graph.entry = 1;
    graph.nodes.push_back(NodeDefinition{
        1,
        NodeKind::Control,
        "emit_and_fanout",
        node_policy_mask(NodePolicyFlag::AllowFanOut),
        0U,
        0U,
        emit_frontier_signal_node,
        {}
    });
    for (int subscriber_index = 0; subscriber_index < kFrontierSubscriberCount; ++subscriber_index) {
        graph.nodes.push_back(NodeDefinition{
            static_cast<NodeId>(2 + subscriber_index),
            NodeKind::Aggregate,
            "naive_watch_" + std::to_string(subscriber_index),
            0U,
            0U,
            0U,
            frontier_watch_executor_for_index(subscriber_index),
            {}
        });
    }
    const NodeId stop_node_id = static_cast<NodeId>(2 + kFrontierSubscriberCount);
    graph.nodes.push_back(NodeDefinition{
        stop_node_id,
        NodeKind::Control,
        "stop",
        node_policy_mask(NodePolicyFlag::StopAfterNode),
        0U,
        0U,
        stop_node,
        {}
    });

    EdgeId edge_id = 1;
    std::vector<EdgeId> root_edges;
    root_edges.reserve(static_cast<std::size_t>(kFrontierSubscriberCount));
    for (int subscriber_index = 0; subscriber_index < kFrontierSubscriberCount; ++subscriber_index) {
        root_edges.push_back(edge_id);
        graph.edges.push_back(EdgeDefinition{
            edge_id++,
            1,
            static_cast<NodeId>(2 + subscriber_index),
            EdgeKind::OnSuccess,
            nullptr,
            static_cast<uint16_t>(1000 - subscriber_index)
        });
    }
    graph.bind_outgoing_edges(1, root_edges);
    for (int subscriber_index = 0; subscriber_index < kFrontierSubscriberCount; ++subscriber_index) {
        graph.edges.push_back(EdgeDefinition{
            edge_id,
            static_cast<NodeId>(2 + subscriber_index),
            stop_node_id,
            EdgeKind::OnSuccess,
            nullptr,
            100U
        });
        graph.bind_outgoing_edges(static_cast<NodeId>(2 + subscriber_index), std::vector<EdgeId>{edge_id});
        ++edge_id;
    }
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_reactive_frontier_benchmark_graph() {
    GraphDefinition graph;
    graph.id = 404;
    graph.name = "reactive_frontier_benchmark_graph";
    graph.entry = 1;
    graph.nodes.push_back(NodeDefinition{
        1,
        NodeKind::Compute,
        "emit_signal",
        0U,
        0U,
        0U,
        emit_frontier_signal_node,
        {}
    });
    graph.nodes.push_back(NodeDefinition{
        2,
        NodeKind::Control,
        "root_stop",
        node_policy_mask(NodePolicyFlag::StopAfterNode),
        0U,
        0U,
        stop_node,
        {}
    });
    for (int subscriber_index = 0; subscriber_index < kFrontierSubscriberCount; ++subscriber_index) {
        graph.nodes.push_back(NodeDefinition{
            static_cast<NodeId>(3 + subscriber_index),
            NodeKind::Aggregate,
            "reactive_watch_" + std::to_string(subscriber_index),
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph),
            0U,
            0U,
            frontier_watch_executor_for_index(subscriber_index),
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    frontier_signal_name(subscriber_index),
                    {}
                }
            }
        });
    }
    const NodeId reactive_stop_id = static_cast<NodeId>(3 + kFrontierSubscriberCount);
    graph.nodes.push_back(NodeDefinition{
        reactive_stop_id,
        NodeKind::Control,
        "reactive_stop",
        node_policy_mask(NodePolicyFlag::StopAfterNode),
        0U,
        0U,
        stop_node,
        {}
    });

    EdgeId edge_id = 1;
    graph.edges.push_back(EdgeDefinition{edge_id++, 1, 2, EdgeKind::OnSuccess, nullptr, 100U});
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    for (int subscriber_index = 0; subscriber_index < kFrontierSubscriberCount; ++subscriber_index) {
        graph.edges.push_back(EdgeDefinition{
            edge_id,
            static_cast<NodeId>(3 + subscriber_index),
            reactive_stop_id,
            EdgeKind::OnSuccess,
            nullptr,
            100U
        });
        graph.bind_outgoing_edges(static_cast<NodeId>(3 + subscriber_index), std::vector<EdgeId>{edge_id});
        ++edge_id;
    }
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_parallel_benchmark_graph() {
    GraphDefinition graph;
    graph.id = 401;
    graph.name = "runtime_benchmark_graph";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "branch_a", 0U, 0U, 0U, branch_a_node, {}},
        NodeDefinition{3, NodeKind::Compute, "branch_b", 0U, 0U, 0U, branch_b_node, {}},
        NodeDefinition{4, NodeKind::Compute, "branch_c", 0U, 0U, 0U, branch_c_node, {}},
        NodeDefinition{5, NodeKind::Compute, "branch_d", 0U, 0U, 0U, branch_d_node, {}},
        NodeDefinition{
            6,
            NodeKind::Aggregate,
            "summarize",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_node,
            {},
            std::vector<FieldMergeRule>{
                FieldMergeRule{kAccumulator, JoinMergeStrategy::SumInt64}
            }
        },
        NodeDefinition{
            7,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3, 1, 4, EdgeKind::OnSuccess, nullptr, 80U},
        EdgeDefinition{4, 1, 5, EdgeKind::OnSuccess, nullptr, 70U},
        EdgeDefinition{5, 2, 6, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{6, 3, 6, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{7, 4, 6, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{8, 5, 6, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{9, 6, 7, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2, 3, 4});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{5});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{6});
    graph.bind_outgoing_edges(4, std::vector<EdgeId>{7});
    graph.bind_outgoing_edges(5, std::vector<EdgeId>{8});
    graph.bind_outgoing_edges(6, std::vector<EdgeId>{9});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_subgraph_benchmark_child_graph() {
    GraphDefinition graph;
    graph.id = 405U;
    graph.name = "subgraph_benchmark_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Compute,
            "child_compute",
            0U,
            0U,
            0U,
            subgraph_benchmark_child_node,
            {}
        },
        NodeDefinition{
            2U,
            NodeKind::Control,
            "child_stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_subgraph_benchmark_parent_graph() {
    GraphDefinition graph;
    graph.id = 406U;
    graph.name = "subgraph_benchmark_parent";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Subgraph,
            "planner_subgraph",
            0U,
            0U,
            0U,
            nullptr,
            {},
            {},
            {},
            SubgraphBinding{
                405U,
                "planner_subgraph",
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kSubgraphBenchmarkInput, kSubgraphBenchmarkChildInput}
                },
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kSubgraphBenchmarkOutput, kSubgraphBenchmarkChildOutput}
                },
                true,
                2U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_subgraph",
            0U,
            0U,
            0U,
            summarize_subgraph_benchmark_node,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2U, 2U, 3U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.bind_outgoing_edges(2U, std::vector<EdgeId>{2U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_resumable_subgraph_benchmark_child_graph() {
    GraphDefinition graph;
    graph.id = 407U;
    graph.name = "resumable_subgraph_benchmark_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Human,
            "child_wait_then_compute",
            0U,
            0U,
            0U,
            resumable_subgraph_benchmark_child_node,
            {}
        },
        NodeDefinition{
            2U,
            NodeKind::Control,
            "child_stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_resumable_subgraph_benchmark_parent_graph() {
    GraphDefinition graph;
    graph.id = 408U;
    graph.name = "resumable_subgraph_benchmark_parent";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Subgraph,
            "resumable_planner_subgraph",
            0U,
            0U,
            0U,
            nullptr,
            {},
            {},
            {},
            SubgraphBinding{
                407U,
                "resumable_planner_subgraph",
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{
                        kResumableSubgraphBenchmarkInput,
                        kResumableSubgraphBenchmarkChildInput
                    }
                },
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{
                        kResumableSubgraphBenchmarkOutput,
                        kResumableSubgraphBenchmarkChildOutput
                    }
                },
                true,
                3U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_resumable_subgraph",
            0U,
            0U,
            0U,
            summarize_resumable_subgraph_benchmark_node,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2U, 2U, 3U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.bind_outgoing_edges(2U, std::vector<EdgeId>{2U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_async_subgraph_benchmark_child_graph() {
    GraphDefinition graph;
    graph.id = 409U;
    graph.name = "async_subgraph_benchmark_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Tool,
            "child_async_tool",
            0U,
            0U,
            0U,
            async_subgraph_benchmark_child_node,
            {}
        },
        NodeDefinition{
            2U,
            NodeKind::Control,
            "child_stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_async_subgraph_benchmark_parent_graph() {
    GraphDefinition graph;
    graph.id = 410U;
    graph.name = "async_subgraph_benchmark_parent";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Subgraph,
            "async_planner_subgraph",
            0U,
            0U,
            0U,
            nullptr,
            {},
            {},
            {},
            SubgraphBinding{
                409U,
                "async_planner_subgraph",
                {},
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kAsyncSubgraphBenchmarkResult, kAsyncSubgraphBenchmarkResult}
                },
                false,
                2U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_async_subgraph",
            0U,
            0U,
            0U,
            summarize_async_subgraph_benchmark_node,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2U, 2U, 3U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.bind_outgoing_edges(2U, std::vector<EdgeId>{2U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_async_multi_wait_subgraph_benchmark_child_graph() {
    GraphDefinition graph;
    graph.id = 411U;
    graph.name = "async_multi_wait_subgraph_benchmark_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Control,
            "fanout_async_children",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{
            2U,
            NodeKind::Tool,
            "child_async_left",
            0U,
            0U,
            0U,
            async_multi_wait_benchmark_left_node,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Tool,
            "child_async_right",
            0U,
            0U,
            0U,
            async_multi_wait_benchmark_right_node,
            {}
        },
        NodeDefinition{
            4U,
            NodeKind::Aggregate,
            "child_async_join",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_async_multi_wait_benchmark_child_node,
            {}
        },
        NodeDefinition{
            5U,
            NodeKind::Control,
            "child_stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2U, 1U, 3U, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3U, 2U, 4U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{4U, 3U, 4U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{5U, 4U, 5U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U, 2U});
    graph.bind_outgoing_edges(2U, std::vector<EdgeId>{3U});
    graph.bind_outgoing_edges(3U, std::vector<EdgeId>{4U});
    graph.bind_outgoing_edges(4U, std::vector<EdgeId>{5U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_async_multi_wait_subgraph_benchmark_parent_graph() {
    GraphDefinition graph;
    graph.id = 412U;
    graph.name = "async_multi_wait_subgraph_benchmark_parent";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Subgraph,
            "async_multi_wait_subgraph",
            0U,
            0U,
            0U,
            nullptr,
            {},
            {},
            {},
            SubgraphBinding{
                411U,
                "async_multi_wait_subgraph",
                {},
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{
                        kAsyncMultiWaitSubgraphBenchmarkResult,
                        kAsyncMultiWaitSubgraphBenchmarkChildSummary
                    }
                },
                false,
                3U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_async_multi_wait_subgraph",
            0U,
            0U,
            0U,
            summarize_async_multi_wait_benchmark_parent_node,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2U, 2U, 3U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.bind_outgoing_edges(2U, std::vector<EdgeId>{2U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

GraphDefinition make_memoization_benchmark_graph(
    GraphId graph_id,
    const char* graph_name,
    NodeExecutorFn compute_executor,
    NodeExecutorFn router_executor,
    NodeMemoizationPolicy memoization
) {
    GraphDefinition graph;
    graph.id = graph_id;
    graph.name = graph_name;
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Compute,
            "memoized_work",
            0U,
            0U,
            0U,
            compute_executor,
            {},
            {},
            {},
            std::nullopt,
            std::move(memoization)
        },
        NodeDefinition{
            2U,
            NodeKind::Control,
            "memoized_router",
            0U,
            0U,
            0U,
            router_executor,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Control,
            "stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

NodeResult routing_router_node(ExecutionContext& context) {
    const int64_t iteration = read_int_field(context.state, kRoutingIteration);

    StatePatch patch;
    if (iteration >= kRoutingIterations) {
        patch.updates.push_back(FieldUpdate{kRoutingShouldStop, true});
        return NodeResult::success(std::move(patch), 1.0F);
    }

    const int64_t next_iteration = iteration + 1;
    patch.updates.push_back(FieldUpdate{kRoutingIteration, next_iteration});
    patch.updates.push_back(FieldUpdate{kRoutingRouteKey, next_iteration % kRoutingRouteCount});
    patch.updates.push_back(FieldUpdate{kRoutingShouldStop, false});
    return NodeResult::success(std::move(patch), 0.99F);
}

template <int RouteIndex>
bool routing_route_selected(const WorkflowState& state) {
    return read_int_field(state, kRoutingRouteKey) == RouteIndex;
}

bool routing_should_stop(const WorkflowState& state) {
    return read_bool_field(state, kRoutingShouldStop);
}

template <int RouteIndex>
NodeResult routing_branch_node(ExecutionContext& context) {
    const int64_t accumulator = read_int_field(context.state, kRoutingAccumulator);

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kRoutingAccumulator,
        accumulator + static_cast<int64_t>(RouteIndex + 1)
    });
    return NodeResult::success(std::move(patch), 0.95F);
}

#define AGENTCORE_ROUTE_CASE(N) \
    case N: \
        return routing_route_selected<N>;

ConditionFn routing_condition_for_index(int route_index) {
    switch (route_index) {
        AGENTCORE_ROUTE_CASE(0)
        AGENTCORE_ROUTE_CASE(1)
        AGENTCORE_ROUTE_CASE(2)
        AGENTCORE_ROUTE_CASE(3)
        AGENTCORE_ROUTE_CASE(4)
        AGENTCORE_ROUTE_CASE(5)
        AGENTCORE_ROUTE_CASE(6)
        AGENTCORE_ROUTE_CASE(7)
        AGENTCORE_ROUTE_CASE(8)
        AGENTCORE_ROUTE_CASE(9)
        AGENTCORE_ROUTE_CASE(10)
        AGENTCORE_ROUTE_CASE(11)
        AGENTCORE_ROUTE_CASE(12)
        AGENTCORE_ROUTE_CASE(13)
        AGENTCORE_ROUTE_CASE(14)
        AGENTCORE_ROUTE_CASE(15)
        default:
            return nullptr;
    }
}

#undef AGENTCORE_ROUTE_CASE

#define AGENTCORE_BRANCH_CASE(N) \
    case N: \
        return routing_branch_node<N>;

NodeExecutorFn routing_executor_for_index(int route_index) {
    switch (route_index) {
        AGENTCORE_BRANCH_CASE(0)
        AGENTCORE_BRANCH_CASE(1)
        AGENTCORE_BRANCH_CASE(2)
        AGENTCORE_BRANCH_CASE(3)
        AGENTCORE_BRANCH_CASE(4)
        AGENTCORE_BRANCH_CASE(5)
        AGENTCORE_BRANCH_CASE(6)
        AGENTCORE_BRANCH_CASE(7)
        AGENTCORE_BRANCH_CASE(8)
        AGENTCORE_BRANCH_CASE(9)
        AGENTCORE_BRANCH_CASE(10)
        AGENTCORE_BRANCH_CASE(11)
        AGENTCORE_BRANCH_CASE(12)
        AGENTCORE_BRANCH_CASE(13)
        AGENTCORE_BRANCH_CASE(14)
        AGENTCORE_BRANCH_CASE(15)
        default:
            return nullptr;
    }
}

#undef AGENTCORE_BRANCH_CASE

GraphDefinition make_routing_benchmark_graph() {
    GraphDefinition graph;
    graph.id = 402;
    graph.name = "routing_benchmark_graph";
    graph.entry = 1;

    graph.nodes.push_back(NodeDefinition{
        1,
        NodeKind::Control,
        "router",
        0U,
        0U,
        0U,
        routing_router_node,
        {}
    });

    for (int route_index = 0; route_index < kRoutingRouteCount; ++route_index) {
        graph.nodes.push_back(NodeDefinition{
            static_cast<NodeId>(2 + route_index),
            NodeKind::Compute,
            "route_" + std::to_string(route_index),
            0U,
            0U,
            0U,
            routing_executor_for_index(route_index),
            {}
        });
    }

    const NodeId stop_node_id = static_cast<NodeId>(2 + kRoutingRouteCount);
    graph.nodes.push_back(NodeDefinition{
        stop_node_id,
        NodeKind::Control,
        "stop",
        node_policy_mask(NodePolicyFlag::StopAfterNode),
        0U,
        0U,
        stop_node,
        {}
    });

    EdgeId next_edge_id = 1;
    std::vector<EdgeId> router_edges;
    router_edges.push_back(next_edge_id);
    graph.edges.push_back(EdgeDefinition{
        next_edge_id++,
        1,
        stop_node_id,
        EdgeKind::Conditional,
        routing_should_stop,
        1000U
    });

    for (int route_index = 0; route_index < kRoutingRouteCount; ++route_index) {
        router_edges.push_back(next_edge_id);
        graph.edges.push_back(EdgeDefinition{
            next_edge_id++,
            1,
            static_cast<NodeId>(2 + route_index),
            EdgeKind::Conditional,
            routing_condition_for_index(route_index),
            static_cast<uint16_t>(900U - route_index)
        });
    }

    graph.bind_outgoing_edges(1, router_edges);
    for (int route_index = 0; route_index < kRoutingRouteCount; ++route_index) {
        graph.edges.push_back(EdgeDefinition{
            next_edge_id,
            static_cast<NodeId>(2 + route_index),
            1,
            EdgeKind::Always,
            nullptr,
            100U
        });
        graph.bind_outgoing_edges(
            static_cast<NodeId>(2 + route_index),
            std::vector<EdgeId>{next_edge_id}
        );
        ++next_edge_id;
    }

    graph.sort_edges_by_priority();
    graph.compile_runtime();
    return graph;
}

int64_t expected_routing_accumulator() {
    int64_t total = 0;
    for (int64_t iteration = 1; iteration <= kRoutingIterations; ++iteration) {
        total += (iteration % kRoutingRouteCount) + 1;
    }
    return total;
}

StateStore materialize_state_store_copy(const StateStore& source) {
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    source.serialize(stream);
    stream.seekg(0);
    return StateStore::deserialize(stream);
}

StateStore make_fork_benchmark_state() {
    StateStore store(2);

    std::string artifact(kForkBenchmarkBlobBytes, 'x');
    artifact.front() = 'A';
    artifact.back() = 'Z';

    const BlobRef artifact_ref = store.blobs().append_string(artifact);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kSummary, artifact_ref});
    patch.updates.push_back(FieldUpdate{kAccumulator, int64_t{0}});

    std::vector<InternedStringId> entity_labels;
    entity_labels.reserve(static_cast<std::size_t>(kForkBenchmarkEntityCount));
    for (int index = 0; index < kForkBenchmarkEntityCount; ++index) {
        const InternedStringId label = store.strings().intern("entity_" + std::to_string(index));
        entity_labels.push_back(label);
        patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{label, {}});
    }

    const InternedStringId relation = store.strings().intern("connects_to");
    for (int index = 0; index < kForkBenchmarkTripleCount; ++index) {
        const InternedStringId subject = entity_labels[static_cast<std::size_t>(index % kForkBenchmarkEntityCount)];
        const InternedStringId object =
            entity_labels[static_cast<std::size_t>((index * 17) % kForkBenchmarkEntityCount)];
        patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
            subject,
            relation,
            object,
            {}
        });
    }

    static_cast<void>(store.apply(patch));
    return store;
}

struct BenchmarkRun {
    uint64_t elapsed_ns{0};
    std::string summary;
    RunProofDigest proof{};
};

struct RoutingBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t trace_events{0};
    uint64_t checkpoint_records{0};
    uint64_t resumable_checkpoints{0};
    int64_t accumulator{0};
    RunProofDigest proof{};
};

struct StateForkBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t forks{0};
    int64_t accumulator_sum{0};
    bool blobs_shared_after_patch{false};
    bool strings_shared_after_patch{false};
    bool knowledge_graph_shared_after_patch{false};
};

struct FrontierBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t trace_events{0};
    uint64_t triggered_watchers{0};
};

struct QueueStatusBenchmarkRun {
    uint64_t queued_tasks{0};
    uint64_t probe_iterations{0};
    uint64_t legacy_probe_ns{0};
    uint64_t indexed_probe_ns{0};
    bool same_result{false};
};

struct SubgraphBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t stream_read_ns{0};
    uint64_t stream_events{0};
    uint64_t namespaced_events{0};
    int64_t output_value{0};
    RunProofDigest proof{};
};

struct ResumableSubgraphBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t resume_ns{0};
    uint64_t stream_read_ns{0};
    uint64_t stream_events{0};
    uint64_t namespaced_events{0};
    int64_t output_value{0};
    RunProofDigest proof{};
};

struct AsyncSubgraphBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t stream_read_ns{0};
    uint64_t stream_events{0};
    uint64_t namespaced_events{0};
    RunProofDigest proof{};
};

struct AsyncMultiWaitSubgraphBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t stream_read_ns{0};
    uint64_t stream_events{0};
    uint64_t namespaced_events{0};
    RunProofDigest proof{};
};

struct RecordedEffectBenchmarkRun {
    uint64_t miss_elapsed_ns{0};
    uint64_t hit_elapsed_ns{0};
    uint64_t miss_producer_calls{0};
    uint64_t hit_producer_calls{0};
    uint64_t journal_records{0};
};

struct MemoizationBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t trace_events{0};
    uint64_t executor_invocations{0};
    int64_t final_input{0};
    int64_t final_output{0};
    int64_t final_visits{0};
};

struct KnowledgeReasoningBenchmarkRun {
    uint64_t triples{0};
    uint64_t iterations{0};
    uint64_t exact_lookup_ns{0};
    uint64_t constrained_lookup_ns{0};
    uint64_t neighbor_scan_ns{0};
    uint64_t exact_matches{0};
    uint64_t constrained_matches{0};
    uint64_t neighbor_total{0};
    bool exact_matched{false};
    bool constrained_matched{false};
    bool neighbor_matched{false};
};

struct KnowledgeIngestBenchmarkRun {
    uint64_t triples{0};
    uint64_t batches{0};
    uint64_t elapsed_ns{0};
    uint64_t created_entities{0};
    uint64_t created_triples{0};
    uint64_t final_entities{0};
    uint64_t final_triples{0};
    bool exact_matched{false};
};

struct IntelligenceQueryBenchmarkRun {
    uint64_t records{0};
    uint64_t iterations{0};
    uint64_t task_query_ns{0};
    uint64_t claim_semantic_query_ns{0};
    uint64_t supporting_claims_query_ns{0};
    uint64_t action_candidates_query_ns{0};
    uint64_t agenda_query_ns{0};
    uint64_t recall_query_ns{0};
    uint64_t focus_query_ns{0};
    uint64_t related_query_ns{0};
    uint64_t related_hops2_query_ns{0};
    uint64_t route_select_ns{0};
    uint64_t task_matches{0};
    uint64_t claim_semantic_matches{0};
    uint64_t supporting_claims_matches{0};
    uint64_t action_candidates_matches{0};
    uint64_t agenda_matches{0};
    uint64_t recall_matches{0};
    uint64_t focus_total_records{0};
    uint64_t related_total_records{0};
    uint64_t related_hops2_total_records{0};
    bool route_matched{false};
    bool claim_semantic_matched{false};
    bool supporting_claims_top_matched{false};
    bool action_candidates_top_matched{false};
    bool agenda_top_matched{false};
    bool recall_top_matched{false};
    bool focus_claim_top_matched{false};
    bool focus_task_top_matched{false};
    bool focus_memory_top_matched{false};
    bool related_hops2_expanded{false};
};

struct ContextGraphBenchmarkRun {
    uint64_t records{0};
    uint64_t iterations{0};
    uint64_t cold_rank_ns{0};
    uint64_t warm_rank_ns{0};
    uint64_t selected_records{0};
    bool claim_matched{false};
    bool evidence_matched{false};
    bool knowledge_matched{false};
};

BenchmarkRun run_parallel_benchmark_once(const GraphDefinition& graph, std::size_t worker_count) {
    ExecutionEngine engine = make_benchmark_engine(worker_count);
    const RunId run_id = engine.start(graph, InputEnvelope{2U});

    const auto started_at = std::chrono::steady_clock::now();
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();
    assert(result.status == ExecutionStatus::Completed);

    const WorkflowState& state = engine.state(run_id);
    assert(state.size() > kSummary);
    assert(std::holds_alternative<BlobRef>(state.load(kSummary)));

    const std::string summary = std::string(
        engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.load(kSummary)))
    );
    const auto record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(record.has_value());
    assert(record->resumable());
    const RunProofDigest proof = compute_run_proof_digest(
        *record,
        engine.trace().events_for_run(run_id)
    );
    return BenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        summary,
        proof
    };
}

RoutingBenchmarkRun run_routing_benchmark_once(
    const GraphDefinition& graph,
    CheckpointPolicy checkpoint_policy
) {
    ExecutionEngine engine = make_benchmark_engine(1U);
    engine.set_checkpoint_policy(checkpoint_policy);
    const RunId run_id = engine.start(graph, InputEnvelope{4U});

    const auto started_at = std::chrono::steady_clock::now();
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();
    assert(result.status == ExecutionStatus::Completed);

    const WorkflowState& state = engine.state(run_id);
    const auto trace_events = engine.trace().events_for_run(run_id);
    const auto record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(record.has_value());
    assert(record->resumable());
    const RunProofDigest proof = compute_run_proof_digest(
        *record,
        trace_events
    );
    return RoutingBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(trace_events.size()),
        static_cast<uint64_t>(engine.checkpoints().size()),
        static_cast<uint64_t>(engine.checkpoints().resumable_count()),
        read_int_field(state, kRoutingAccumulator),
        proof
    };
}

StateForkBenchmarkRun run_state_fork_benchmark_once(bool materialized_copy) {
    const StateStore source = make_fork_benchmark_state();

    std::vector<StateStore> forks;
    forks.reserve(kForkBenchmarkCopies);

    const auto started_at = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < kForkBenchmarkCopies; ++index) {
        StateStore fork = materialized_copy ? materialize_state_store_copy(source) : source;
        StatePatch patch;
        patch.updates.push_back(FieldUpdate{kAccumulator, static_cast<int64_t>(index + 1U)});
        static_cast<void>(fork.apply(patch));
        forks.push_back(std::move(fork));
    }
    const auto ended_at = std::chrono::steady_clock::now();

    int64_t accumulator_sum = 0;
    for (std::size_t index = 0; index < forks.size(); ++index) {
        accumulator_sum += read_int_field(forks[index].get_current_state(), kAccumulator);
    }

    const StateStore::SharedBacking shared = source.shared_backing_with(forks.front());
    return StateForkBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(forks.size()),
        accumulator_sum,
        shared.blobs,
        shared.strings,
        shared.knowledge_graph
    };
}

FrontierBenchmarkRun run_frontier_benchmark_once(const GraphDefinition& graph, NodeId watcher_base_id) {
    ExecutionEngine engine = make_benchmark_engine(1U);
    const RunId run_id = engine.start(graph, InputEnvelope{2U});

    const auto started_at = std::chrono::steady_clock::now();
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();
    assert(result.status == ExecutionStatus::Completed);

    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    uint64_t triggered_watchers = 0;
    for (const TraceEvent& event : trace_events) {
        if (event.node_id >= watcher_base_id &&
            event.node_id < static_cast<NodeId>(watcher_base_id + kFrontierSubscriberCount)) {
            ++triggered_watchers;
        }
    }

    return FrontierBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(trace_events.size()),
        triggered_watchers
    };
}

QueueStatusBenchmarkRun run_queue_status_benchmark_once() {
    constexpr RunId kRunId = 77U;
    constexpr std::size_t kQueuedTasks = 4096U;
    constexpr std::size_t kProbeIterations = 5000U;

    Scheduler scheduler(1U);
    for (std::size_t index = 0; index < kQueuedTasks; ++index) {
        scheduler.enqueue_task(ScheduledTask{
            kRunId,
            static_cast<NodeId>((index % 2048U) + 1U),
            static_cast<uint32_t>(index % 32U),
            static_cast<uint64_t>(index)
        });
    }

    std::size_t legacy_hits = 0U;
    const auto legacy_started_at = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < kProbeIterations; ++iteration) {
        legacy_hits += scheduler.tasks_for_run(kRunId).empty() ? 0U : 1U;
    }
    const auto legacy_ended_at = std::chrono::steady_clock::now();

    std::size_t indexed_hits = 0U;
    const auto indexed_started_at = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < kProbeIterations; ++iteration) {
        indexed_hits += scheduler.has_tasks_for_run(kRunId) ? 1U : 0U;
    }
    const auto indexed_ended_at = std::chrono::steady_clock::now();

    assert(legacy_hits == indexed_hits);
    assert(scheduler.task_count_for_run(kRunId) == kQueuedTasks);

    return QueueStatusBenchmarkRun{
        static_cast<uint64_t>(kQueuedTasks),
        static_cast<uint64_t>(kProbeIterations),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(legacy_ended_at - legacy_started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(indexed_ended_at - indexed_started_at).count()
        ),
        legacy_hits == indexed_hits
    };
}

RecordedEffectBenchmarkRun run_recorded_effect_benchmark_once() {
    std::vector<std::byte> runtime_config_payload;
    ToolRegistry tools;
    ModelRegistry models;
    TraceSink trace;
    CancellationToken cancel;

    StateStore miss_store(1U);
    ScratchArena miss_scratch;
    uint64_t miss_producer_calls = 0U;
    const auto miss_started_at = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < kRecordedEffectBenchmarkIterations; ++iteration) {
        std::vector<TaskRecord> staged_records;
        ExecutionContext context{
            miss_store.get_current_state(),
            1U,
            1U,
            1U,
            1U,
            runtime_config_payload,
            miss_scratch,
            miss_store.blobs(),
            miss_store.strings(),
            miss_store.knowledge_graph(),
            miss_store.intelligence(),
            miss_store.task_journal(),
            tools,
            models,
            trace,
            Deadline{},
            cancel,
            std::nullopt,
            &staged_records
        };

        const std::string key = "recorded-effect::miss::" + std::to_string(iteration);
        const std::string request = "request::" + std::to_string(iteration);
        const std::string output = "output::" + std::to_string(iteration);
        const RecordedEffectResult effect = context.record_text_effect_once(
            key,
            request,
            [&]() {
                ++miss_producer_calls;
                return output;
            }
        );
        assert(!effect.replayed);
        assert(staged_records.size() == 1U);

        StatePatch patch;
        patch.task_records = std::move(staged_records);
        static_cast<void>(miss_store.apply(patch));
        miss_scratch.reset();
    }
    const auto miss_ended_at = std::chrono::steady_clock::now();

    StateStore hit_store(1U);
    ScratchArena hit_scratch;
    uint64_t hit_producer_calls = 0U;
    {
        std::vector<TaskRecord> staged_records;
        ExecutionContext context{
            hit_store.get_current_state(),
            2U,
            1U,
            1U,
            1U,
            runtime_config_payload,
            hit_scratch,
            hit_store.blobs(),
            hit_store.strings(),
            hit_store.knowledge_graph(),
            hit_store.intelligence(),
            hit_store.task_journal(),
            tools,
            models,
            trace,
            Deadline{},
            cancel,
            std::nullopt,
            &staged_records
        };
        const RecordedEffectResult primed = context.record_text_effect_once(
            "recorded-effect::hit::shared",
            "request::steady",
            [&]() {
                ++hit_producer_calls;
                return std::string("output::steady");
            }
        );
        assert(!primed.replayed);
        StatePatch patch;
        patch.task_records = std::move(staged_records);
        static_cast<void>(hit_store.apply(patch));
        hit_scratch.reset();
    }

    const auto hit_started_at = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0; iteration < kRecordedEffectBenchmarkIterations; ++iteration) {
        std::vector<TaskRecord> staged_records;
        ExecutionContext context{
            hit_store.get_current_state(),
            2U,
            1U,
            1U,
            1U,
            runtime_config_payload,
            hit_scratch,
            hit_store.blobs(),
            hit_store.strings(),
            hit_store.knowledge_graph(),
            hit_store.intelligence(),
            hit_store.task_journal(),
            tools,
            models,
            trace,
            Deadline{},
            cancel,
            std::nullopt,
            &staged_records
        };
        const RecordedEffectResult effect = context.record_text_effect_once(
            "recorded-effect::hit::shared",
            "request::steady",
            [&]() {
                ++hit_producer_calls;
                return std::string("output::steady");
            }
        );
        assert(effect.replayed);
        assert(staged_records.empty());
        hit_scratch.reset();
    }
    const auto hit_ended_at = std::chrono::steady_clock::now();

    assert(hit_producer_calls == 1U);
    return RecordedEffectBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(miss_ended_at - miss_started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(hit_ended_at - hit_started_at).count()
        ),
        miss_producer_calls,
        hit_producer_calls,
        static_cast<uint64_t>(miss_store.task_journal().size())
    };
}

MemoizationBenchmarkRun run_memoization_benchmark_once(
    const GraphDefinition& graph,
    int64_t initial_input,
    std::atomic<uint64_t>* invocation_counter
) {
    assert(invocation_counter != nullptr);
    invocation_counter->store(0U);

    ExecutionEngine engine = make_benchmark_engine(1U);
    InputEnvelope input{3U};
    input.initial_patch.updates.push_back(FieldUpdate{kMemoizationBenchmarkInput, initial_input});
    input.initial_patch.updates.push_back(FieldUpdate{kMemoizationBenchmarkVisits, int64_t{0}});

    const RunId run_id = engine.start(graph, input);
    const auto started_at = std::chrono::steady_clock::now();
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();
    assert(result.status == ExecutionStatus::Completed);

    const WorkflowState& state = engine.state(run_id);
    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    return MemoizationBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(trace_events.size()),
        invocation_counter->load(),
        read_int_field(state, kMemoizationBenchmarkInput),
        read_int_field(state, kMemoizationBenchmarkOutput),
        read_int_field(state, kMemoizationBenchmarkVisits)
    };
}

KnowledgeReasoningBenchmarkRun run_knowledge_reasoning_benchmark_once() {
    KnowledgeGraphStore graph;
    StringInterner strings;
    BlobStore blobs;

    const InternedStringId hot_subject = strings.intern("kg:hot-subject");
    const InternedStringId hot_relation = strings.intern("kg:rel:hot");
    InternedStringId exact_subject = 0U;
    InternedStringId exact_relation = 0U;
    InternedStringId exact_object = 0U;

    for (std::size_t index = 0; index < kKnowledgeReasoningBenchmarkTriples; ++index) {
        const bool hot_edge = index < (kKnowledgeReasoningBenchmarkTriples / 4U);
        const InternedStringId subject = hot_edge
            ? hot_subject
            : strings.intern("kg:subject:" + std::to_string(index % 512U));
        const InternedStringId relation = hot_edge
            ? hot_relation
            : strings.intern("kg:rel:" + std::to_string(index % 64U));
        const InternedStringId object = strings.intern("kg:object:" + std::to_string(index));
        graph.upsert_triple(
            subject,
            relation,
            object,
            blobs.append_string("payload:" + std::to_string(index))
        );
        if (index == kKnowledgeReasoningBenchmarkTriples - 7U) {
            exact_subject = subject;
            exact_relation = relation;
            exact_object = object;
        }
    }

    const auto exact_started_at = std::chrono::steady_clock::now();
    uint64_t exact_matches = 0U;
    for (std::size_t iteration = 0; iteration < kKnowledgeReasoningBenchmarkIterations; ++iteration) {
        exact_matches += graph.match(exact_subject, exact_relation, exact_object).size();
    }
    const auto exact_ended_at = std::chrono::steady_clock::now();

    const auto constrained_started_at = std::chrono::steady_clock::now();
    uint64_t constrained_matches = 0U;
    for (std::size_t iteration = 0; iteration < kKnowledgeReasoningBenchmarkIterations; ++iteration) {
        constrained_matches += graph.match(std::nullopt, exact_relation, exact_object).size();
    }
    const auto constrained_ended_at = std::chrono::steady_clock::now();

    const auto neighbor_started_at = std::chrono::steady_clock::now();
    uint64_t neighbor_total = 0U;
    for (std::size_t iteration = 0; iteration < kKnowledgeReasoningBenchmarkIterations; ++iteration) {
        neighbor_total += graph.neighbors(hot_subject, hot_relation).size();
    }
    const auto neighbor_ended_at = std::chrono::steady_clock::now();

    return KnowledgeReasoningBenchmarkRun{
        static_cast<uint64_t>(kKnowledgeReasoningBenchmarkTriples),
        static_cast<uint64_t>(kKnowledgeReasoningBenchmarkIterations),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                exact_ended_at - exact_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                constrained_ended_at - constrained_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                neighbor_ended_at - neighbor_started_at
            ).count()
        ),
        exact_matches,
        constrained_matches,
        neighbor_total,
        exact_matches == kKnowledgeReasoningBenchmarkIterations,
        constrained_matches == kKnowledgeReasoningBenchmarkIterations,
        neighbor_total == (
            (kKnowledgeReasoningBenchmarkTriples / 4U) *
            kKnowledgeReasoningBenchmarkIterations
        )
    };
}

KnowledgeIngestBenchmarkRun run_knowledge_ingest_benchmark_once() {
    StateStore store;
    StringInterner& strings = store.strings();
    BlobStore& blobs = store.blobs();

    std::vector<StatePatch> patches;
    patches.reserve(
        (kKnowledgeIngestBenchmarkTriples + kKnowledgeIngestBenchmarkBatchSize - 1U) /
        kKnowledgeIngestBenchmarkBatchSize
    );

    InternedStringId exact_subject = 0U;
    InternedStringId exact_relation = 0U;
    InternedStringId exact_object = 0U;

    for (std::size_t offset = 0; offset < kKnowledgeIngestBenchmarkTriples;) {
        StatePatch patch;
        patch.knowledge_graph.triples.reserve(kKnowledgeIngestBenchmarkBatchSize);
        const std::size_t batch_end = std::min(
            offset + kKnowledgeIngestBenchmarkBatchSize,
            kKnowledgeIngestBenchmarkTriples
        );
        for (; offset < batch_end; ++offset) {
            const InternedStringId subject =
                strings.intern("ingest:subject:" + std::to_string(offset % 512U));
            const InternedStringId relation =
                strings.intern("ingest:rel:" + std::to_string(offset % 32U));
            const InternedStringId object =
                strings.intern("ingest:object:" + std::to_string(offset));
            patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
                subject,
                relation,
                object,
                blobs.append_string("ingest-payload:" + std::to_string(offset))
            });
            if (offset == kKnowledgeIngestBenchmarkTriples - 3U) {
                exact_subject = subject;
                exact_relation = relation;
                exact_object = object;
            }
        }
        patches.push_back(std::move(patch));
    }

    uint64_t created_entities = 0U;
    uint64_t created_triples = 0U;
    const auto started_at = std::chrono::steady_clock::now();
    for (const StatePatch& patch : patches) {
        const StateApplyResult result = store.apply_with_summary(patch);
        for (const KnowledgeEntityDelta& delta : result.knowledge_graph_delta.entities) {
            created_entities += delta.created ? 1U : 0U;
        }
        for (const KnowledgeTripleDelta& delta : result.knowledge_graph_delta.triples) {
            created_triples += delta.created ? 1U : 0U;
        }
    }
    const auto ended_at = std::chrono::steady_clock::now();

    const std::vector<const KnowledgeTriple*> exact_matches =
        store.knowledge_graph().match(exact_subject, exact_relation, exact_object);

    return KnowledgeIngestBenchmarkRun{
        static_cast<uint64_t>(kKnowledgeIngestBenchmarkTriples),
        static_cast<uint64_t>(patches.size()),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                ended_at - started_at
            ).count()
        ),
        created_entities,
        created_triples,
        static_cast<uint64_t>(store.knowledge_graph().entity_count()),
        static_cast<uint64_t>(store.knowledge_graph().triple_count()),
        exact_matches.size() == 1U
    };
}

ContextGraphBenchmarkRun run_context_graph_benchmark_once() {
    IntelligenceStore intelligence;
    KnowledgeGraphStore knowledge_graph;
    StringInterner strings;

    const InternedStringId subject = strings.intern("Incident");
    const InternedStringId relation = strings.intern("affects");
    const InternedStringId object = strings.intern("checkout API");
    const InternedStringId owner = strings.intern("ops");
    const InternedStringId source = strings.intern("observability");
    const InternedStringId scope = strings.intern("incident");
    const InternedStringId owned_by = strings.intern("owned_by");
    const InternedStringId expected_task_key = strings.intern("task:context:17");
    const InternedStringId expected_claim_key = strings.intern("claim:context:17");
    const InternedStringId expected_evidence_key = strings.intern("evidence:context:17");

    for (std::size_t index = 0U; index < kContextGraphBenchmarkRecords; ++index) {
        const std::string suffix = std::to_string(index);
        const InternedStringId task_key = strings.intern("task:context:" + suffix);
        const InternedStringId claim_key = strings.intern("claim:context:" + suffix);
        const InternedStringId evidence_key = strings.intern("evidence:context:" + suffix);
        const InternedStringId decision_key = strings.intern("decision:context:" + suffix);
        const InternedStringId memory_key = strings.intern("memory:context:" + suffix);
        const bool hot_record = index == 17U;
        const float confidence = hot_record ? 0.99F : 0.45F + static_cast<float>(index % 10U) * 0.04F;
        const InternedStringId record_object =
            hot_record ? object : strings.intern("service:" + std::to_string(index % 64U));

        intelligence.upsert_task(IntelligenceTaskWrite{
            task_key,
            strings.intern("Context graph task " + suffix),
            (index % 2U == 0U || hot_record) ? owner : strings.intern("support"),
            {},
            {},
            index % 3U == 0U ? IntelligenceTaskStatus::Completed : IntelligenceTaskStatus::InProgress,
            hot_record ? 50 : static_cast<int32_t>(index % 8U),
            confidence,
            intelligence_fields::kTaskTitle |
                intelligence_fields::kTaskOwner |
                intelligence_fields::kTaskStatus |
                intelligence_fields::kTaskPriority |
                intelligence_fields::kTaskConfidence
        });
        intelligence.upsert_claim(IntelligenceClaimWrite{
            claim_key,
            subject,
            (index % 2U == 0U || hot_record) ? relation : strings.intern("mentions"),
            record_object,
            {},
            hot_record ? IntelligenceClaimStatus::Confirmed : IntelligenceClaimStatus::Supported,
            confidence,
            intelligence_fields::kClaimSubject |
                intelligence_fields::kClaimRelation |
                intelligence_fields::kClaimObject |
                intelligence_fields::kClaimStatus |
                intelligence_fields::kClaimConfidence
        });
        intelligence.upsert_evidence(IntelligenceEvidenceWrite{
            evidence_key,
            strings.intern("log"),
            source,
            {},
            task_key,
            claim_key,
            confidence,
            intelligence_fields::kEvidenceKind |
                intelligence_fields::kEvidenceSource |
                intelligence_fields::kEvidenceTaskKey |
                intelligence_fields::kEvidenceClaimKey |
                intelligence_fields::kEvidenceConfidence
        });
        intelligence.upsert_decision(IntelligenceDecisionWrite{
            decision_key,
            task_key,
            claim_key,
            {},
            hot_record ? IntelligenceDecisionStatus::Selected : IntelligenceDecisionStatus::Pending,
            confidence,
            intelligence_fields::kDecisionTaskKey |
                intelligence_fields::kDecisionClaimKey |
                intelligence_fields::kDecisionStatus |
                intelligence_fields::kDecisionConfidence
        });
        intelligence.upsert_memory(IntelligenceMemoryWrite{
            memory_key,
            IntelligenceMemoryLayer::Working,
            scope,
            {},
            task_key,
            claim_key,
            confidence,
            intelligence_fields::kMemoryLayer |
                intelligence_fields::kMemoryScope |
                intelligence_fields::kMemoryTaskKey |
                intelligence_fields::kMemoryClaimKey |
                intelligence_fields::kMemoryImportance
        });
        knowledge_graph.upsert_triple(subject, relation, record_object);
        knowledge_graph.upsert_triple(
            record_object,
            owned_by,
            hot_record ? strings.intern("payments team") : strings.intern("team:" + std::to_string(index % 16U))
        );
    }

    ContextQueryPlan plan;
    static_cast<void>(context_query_plan_add_selector(plan, "tasks.agenda"));
    static_cast<void>(context_query_plan_add_selector(plan, "claims.confirmed"));
    static_cast<void>(context_query_plan_add_selector(plan, "evidence.relevant"));
    static_cast<void>(context_query_plan_add_selector(plan, "decisions.selected"));
    static_cast<void>(context_query_plan_add_selector(plan, "memories.working"));
    static_cast<void>(context_query_plan_add_selector(plan, "knowledge.neighborhood"));
    plan.task_key = expected_task_key;
    plan.claim_key = expected_claim_key;
    plan.subject_label = subject;
    plan.relation = relation;
    plan.object_label = object;
    plan.owner = owner;
    plan.source = source;
    plan.scope = scope;
    plan.limit = 24U;
    plan.limit_per_source = 5U;

    const auto cold_started_at = std::chrono::steady_clock::now();
    const ContextGraphIndex context_index(intelligence, knowledge_graph, plan);
    const ContextGraphResult cold_result = context_index.rank();
    const auto cold_ended_at = std::chrono::steady_clock::now();

    ContextGraphResult warm_result;
    const auto warm_started_at = std::chrono::steady_clock::now();
    for (std::size_t iteration = 0U; iteration < kContextGraphBenchmarkIterations; ++iteration) {
        warm_result = context_index.rank();
    }
    const auto warm_ended_at = std::chrono::steady_clock::now();

    bool claim_matched = false;
    bool evidence_matched = false;
    bool knowledge_matched = false;
    for (const ContextGraphRecordRef& ref : warm_result.records) {
        if (ref.kind == ContextRecordKind::Claim) {
            const IntelligenceClaim* claim = intelligence.find_claim(ref.id);
            claim_matched = claim_matched || (claim != nullptr && claim->key == expected_claim_key);
        }
        if (ref.kind == ContextRecordKind::Evidence) {
            const IntelligenceEvidence* evidence = intelligence.find_evidence(ref.id);
            evidence_matched = evidence_matched || (evidence != nullptr && evidence->key == expected_evidence_key);
        }
        knowledge_matched = knowledge_matched || ref.kind == ContextRecordKind::Knowledge;
    }

    return ContextGraphBenchmarkRun{
        static_cast<uint64_t>(kContextGraphBenchmarkRecords),
        static_cast<uint64_t>(kContextGraphBenchmarkIterations),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                cold_ended_at - cold_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                warm_ended_at - warm_started_at
            ).count()
        ),
        static_cast<uint64_t>(cold_result.records.size()),
        claim_matched,
        evidence_matched,
        knowledge_matched
    };
}

IntelligenceQueryBenchmarkRun run_intelligence_query_benchmark_once() {
    StateStore store(1U);
    BlobStore& blobs = store.blobs();
    StringInterner& strings = store.strings();

    const InternedStringId benchmark_relation = strings.intern("supports");
    const InternedStringId benchmark_subject = strings.intern("agentcore");
    const InternedStringId benchmark_object = strings.intern("intelligence");
    const InternedStringId semantic_subject = strings.intern("agentcore-focus");
    const InternedStringId semantic_relation = strings.intern("supports");
    const InternedStringId semantic_object = strings.intern("intelligence-focus");
    const InternedStringId benchmark_source = strings.intern("native-benchmark");
    const InternedStringId benchmark_scope = strings.intern("scope:2");
    const InternedStringId benchmark_owner = strings.intern("specialist:2");
    const InternedStringId focus_task_key = strings.intern("task:bench:focus");
    const InternedStringId focus_claim_key = strings.intern("claim:bench:focus");
    const InternedStringId focus_evidence_key = strings.intern("evidence:bench:focus");
    const InternedStringId focus_decision_key = strings.intern("decision:bench:focus");
    const InternedStringId focus_memory_key = strings.intern("memory:bench:focus");
    const InternedStringId supporting_task_key = strings.intern("task:bench:supporting");
    const InternedStringId supporting_claim_strong_key = strings.intern("claim:bench:supporting:strong");
    const InternedStringId supporting_claim_weak_key = strings.intern("claim:bench:supporting:weak");
    const InternedStringId supporting_evidence_strong_key = strings.intern("evidence:bench:supporting:strong");
    const InternedStringId supporting_evidence_weak_key = strings.intern("evidence:bench:supporting:weak");
    const InternedStringId supporting_decision_strong_key = strings.intern("decision:bench:supporting:strong");
    const InternedStringId supporting_memory_strong_key = strings.intern("memory:bench:supporting:strong");
    const InternedStringId action_owner = strings.intern("action-owner");
    const InternedStringId action_object = strings.intern("action-ranking");
    const InternedStringId action_task_strong_key = strings.intern("task:bench:action:strong");
    const InternedStringId action_task_weak_key = strings.intern("task:bench:action:weak");
    const InternedStringId action_claim_strong_key = strings.intern("claim:bench:action:strong");
    const InternedStringId action_claim_weak_key = strings.intern("claim:bench:action:weak");
    const InternedStringId action_evidence_strong_key = strings.intern("evidence:bench:action:strong");
    const InternedStringId action_evidence_weak_key = strings.intern("evidence:bench:action:weak");
    const InternedStringId action_decision_strong_key = strings.intern("decision:bench:action:strong");
    const InternedStringId action_memory_strong_key = strings.intern("memory:bench:action:strong");
    const InternedStringId related_hops2_task_key = strings.intern("task:bench:related:seed");
    const InternedStringId related_hops2_claim_key = strings.intern("claim:bench:related:shared");
    const InternedStringId related_hops2_task_second_key = strings.intern("task:bench:related:second");
    const InternedStringId related_hops2_evidence_key = strings.intern("evidence:bench:related:seed");
    const InternedStringId related_hops2_memory_key = strings.intern("memory:bench:related:second");

    StatePatch patch;
    patch.intelligence.tasks.reserve(kIntelligenceBenchmarkRecords);
    patch.intelligence.claims.reserve(kIntelligenceBenchmarkRecords);
    patch.intelligence.evidence.reserve(kIntelligenceBenchmarkRecords);
    patch.intelligence.decisions.reserve(kIntelligenceBenchmarkRecords);
    patch.intelligence.memories.reserve(kIntelligenceBenchmarkRecords);

    for (std::size_t index = 0; index < kIntelligenceBenchmarkRecords; ++index) {
        const std::string suffix = std::to_string(index);
        const InternedStringId task_key = strings.intern("task:bench:" + suffix);
        const InternedStringId claim_key = strings.intern("claim:bench:" + suffix);
        const InternedStringId evidence_key = strings.intern("evidence:bench:" + suffix);
        const InternedStringId decision_key = strings.intern("decision:bench:" + suffix);
        const InternedStringId memory_key = strings.intern("memory:bench:" + suffix);
        const InternedStringId owner = strings.intern("specialist:" + std::to_string(index % 16U));
        const InternedStringId scope = strings.intern("scope:" + std::to_string(index % 8U));
        const IntelligenceTaskStatus task_status =
            index % 2U == 0U ? IntelligenceTaskStatus::Completed : IntelligenceTaskStatus::InProgress;
        const IntelligenceClaimStatus claim_status =
            index % 2U == 0U ? IntelligenceClaimStatus::Supported : IntelligenceClaimStatus::Proposed;
        const IntelligenceDecisionStatus decision_status =
            index % 2U == 0U ? IntelligenceDecisionStatus::Selected : IntelligenceDecisionStatus::Pending;
        const IntelligenceMemoryLayer memory_layer =
            index % 2U == 0U ? IntelligenceMemoryLayer::Semantic : IntelligenceMemoryLayer::Working;

        patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
            task_key,
            strings.intern("Task " + suffix),
            owner,
            blobs.append_string("{\"task\":" + suffix + "}"),
            {},
            task_status,
            static_cast<int32_t>(index % 5U),
            0.8F,
            intelligence_fields::kTaskTitle |
                intelligence_fields::kTaskOwner |
                intelligence_fields::kTaskPayload |
                intelligence_fields::kTaskStatus |
                intelligence_fields::kTaskPriority |
                intelligence_fields::kTaskConfidence
        });
        patch.intelligence.claims.push_back(IntelligenceClaimWrite{
            claim_key,
            benchmark_subject,
            benchmark_relation,
            benchmark_object,
            blobs.append_string("{\"claim\":" + suffix + "}"),
            claim_status,
            0.75F,
            intelligence_fields::kClaimSubject |
                intelligence_fields::kClaimRelation |
                intelligence_fields::kClaimObject |
                intelligence_fields::kClaimStatement |
                intelligence_fields::kClaimStatus |
                intelligence_fields::kClaimConfidence
        });
        patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
            evidence_key,
            strings.intern("benchmark"),
            benchmark_source,
            blobs.append_string("{\"evidence\":" + suffix + "}"),
            task_key,
            claim_key,
            0.7F,
            intelligence_fields::kEvidenceKind |
                intelligence_fields::kEvidenceSource |
                intelligence_fields::kEvidenceContent |
                intelligence_fields::kEvidenceTaskKey |
                intelligence_fields::kEvidenceClaimKey |
                intelligence_fields::kEvidenceConfidence
        });
        patch.intelligence.decisions.push_back(IntelligenceDecisionWrite{
            decision_key,
            task_key,
            claim_key,
            blobs.append_string("{\"decision\":" + suffix + "}"),
            decision_status,
            0.7F,
            intelligence_fields::kDecisionTaskKey |
                intelligence_fields::kDecisionClaimKey |
                intelligence_fields::kDecisionSummary |
                intelligence_fields::kDecisionStatus |
                intelligence_fields::kDecisionConfidence
        });
        patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
            memory_key,
            memory_layer,
            scope,
            blobs.append_string("{\"memory\":" + suffix + "}"),
            task_key,
            claim_key,
            0.6F,
            intelligence_fields::kMemoryLayer |
                intelligence_fields::kMemoryScope |
                intelligence_fields::kMemoryContent |
                intelligence_fields::kMemoryTaskKey |
                intelligence_fields::kMemoryClaimKey |
                intelligence_fields::kMemoryImportance
        });
    }
    static_cast<void>(store.apply(patch));

    StatePatch focus_patch;
    focus_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        focus_task_key,
        strings.intern("Focus benchmark"),
        benchmark_owner,
        blobs.append_string("{\"task\":\"focus\"}"),
        {},
        IntelligenceTaskStatus::InProgress,
        100,
        0.99F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        focus_claim_key,
        semantic_subject,
        semantic_relation,
        semantic_object,
        blobs.append_string("{\"claim\":\"focus\"}"),
        IntelligenceClaimStatus::Supported,
        0.98F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    focus_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        focus_evidence_key,
        strings.intern("benchmark"),
        benchmark_source,
        blobs.append_string("{\"evidence\":\"focus\"}"),
        focus_task_key,
        focus_claim_key,
        0.98F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    focus_patch.intelligence.decisions.push_back(IntelligenceDecisionWrite{
        focus_decision_key,
        focus_task_key,
        focus_claim_key,
        blobs.append_string("{\"decision\":\"focus\"}"),
        IntelligenceDecisionStatus::Selected,
        0.96F,
        intelligence_fields::kDecisionTaskKey |
            intelligence_fields::kDecisionClaimKey |
            intelligence_fields::kDecisionSummary |
            intelligence_fields::kDecisionStatus |
            intelligence_fields::kDecisionConfidence
    });
    focus_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        focus_memory_key,
        IntelligenceMemoryLayer::Semantic,
        benchmark_scope,
        blobs.append_string("{\"memory\":\"focus\"}"),
        focus_task_key,
        strings.intern("claim:bench:0"),
        1.0F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    focus_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        supporting_task_key,
        strings.intern("Supporting benchmark"),
        strings.intern("support-owner"),
        blobs.append_string("{\"task\":\"supporting\"}"),
        {},
        IntelligenceTaskStatus::InProgress,
        8,
        0.88F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        supporting_claim_strong_key,
        benchmark_subject,
        benchmark_relation,
        strings.intern("support-ranking"),
        blobs.append_string("{\"claim\":\"support-strong\"}"),
        IntelligenceClaimStatus::Supported,
        0.9F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    focus_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        supporting_claim_weak_key,
        benchmark_subject,
        benchmark_relation,
        strings.intern("support-ranking"),
        blobs.append_string("{\"claim\":\"support-weak\"}"),
        IntelligenceClaimStatus::Supported,
        0.7F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    focus_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        supporting_evidence_strong_key,
        strings.intern("benchmark"),
        strings.intern("supporting"),
        blobs.append_string("{\"support\":\"strong\"}"),
        supporting_task_key,
        supporting_claim_strong_key,
        0.96F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    focus_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        supporting_evidence_weak_key,
        strings.intern("benchmark"),
        strings.intern("supporting"),
        blobs.append_string("{\"support\":\"weak\"}"),
        supporting_task_key,
        supporting_claim_weak_key,
        0.42F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    focus_patch.intelligence.decisions.push_back(IntelligenceDecisionWrite{
        supporting_decision_strong_key,
        supporting_task_key,
        supporting_claim_strong_key,
        blobs.append_string("{\"selected\":true}"),
        IntelligenceDecisionStatus::Selected,
        0.94F,
        intelligence_fields::kDecisionTaskKey |
            intelligence_fields::kDecisionClaimKey |
            intelligence_fields::kDecisionSummary |
            intelligence_fields::kDecisionStatus |
            intelligence_fields::kDecisionConfidence
    });
    focus_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        supporting_memory_strong_key,
        IntelligenceMemoryLayer::Semantic,
        strings.intern("scope:supporting"),
        blobs.append_string("{\"support\":\"memory\"}"),
        supporting_task_key,
        supporting_claim_strong_key,
        0.9F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    focus_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        action_task_strong_key,
        strings.intern("Action benchmark strong"),
        action_owner,
        blobs.append_string("{\"task\":\"action-strong\"}"),
        {},
        IntelligenceTaskStatus::Open,
        4,
        0.8F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        action_task_weak_key,
        strings.intern("Action benchmark weak"),
        action_owner,
        blobs.append_string("{\"task\":\"action-weak\"}"),
        {},
        IntelligenceTaskStatus::Open,
        4,
        0.8F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        action_claim_strong_key,
        benchmark_subject,
        benchmark_relation,
        action_object,
        blobs.append_string("{\"claim\":\"action-strong\"}"),
        IntelligenceClaimStatus::Supported,
        0.9F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    focus_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        action_claim_weak_key,
        benchmark_subject,
        benchmark_relation,
        action_object,
        blobs.append_string("{\"claim\":\"action-weak\"}"),
        IntelligenceClaimStatus::Supported,
        0.7F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    focus_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        action_evidence_strong_key,
        strings.intern("benchmark"),
        strings.intern("action-candidates"),
        blobs.append_string("{\"action\":\"strong\"}"),
        action_task_strong_key,
        action_claim_strong_key,
        0.95F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    focus_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        action_evidence_weak_key,
        strings.intern("benchmark"),
        strings.intern("action-candidates"),
        blobs.append_string("{\"action\":\"weak\"}"),
        action_task_weak_key,
        action_claim_weak_key,
        0.42F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    focus_patch.intelligence.decisions.push_back(IntelligenceDecisionWrite{
        action_decision_strong_key,
        action_task_strong_key,
        action_claim_strong_key,
        blobs.append_string("{\"selected\":true}"),
        IntelligenceDecisionStatus::Selected,
        0.94F,
        intelligence_fields::kDecisionTaskKey |
            intelligence_fields::kDecisionClaimKey |
            intelligence_fields::kDecisionSummary |
            intelligence_fields::kDecisionStatus |
            intelligence_fields::kDecisionConfidence
    });
    focus_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        action_memory_strong_key,
        IntelligenceMemoryLayer::Semantic,
        strings.intern("scope:action"),
        blobs.append_string("{\"action\":\"memory\"}"),
        action_task_strong_key,
        action_claim_strong_key,
        0.9F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    focus_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        related_hops2_task_key,
        strings.intern("Related seed benchmark"),
        strings.intern("related-owner"),
        blobs.append_string("{\"task\":\"related-seed\"}"),
        {},
        IntelligenceTaskStatus::InProgress,
        2,
        0.85F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_patch.intelligence.tasks.push_back(IntelligenceTaskWrite{
        related_hops2_task_second_key,
        strings.intern("Related second benchmark"),
        strings.intern("related-owner"),
        blobs.append_string("{\"task\":\"related-second\"}"),
        {},
        IntelligenceTaskStatus::Open,
        1,
        0.7F,
        intelligence_fields::kTaskTitle |
            intelligence_fields::kTaskOwner |
            intelligence_fields::kTaskPayload |
            intelligence_fields::kTaskStatus |
            intelligence_fields::kTaskPriority |
            intelligence_fields::kTaskConfidence
    });
    focus_patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        related_hops2_claim_key,
        benchmark_subject,
        benchmark_relation,
        benchmark_object,
        blobs.append_string("{\"claim\":\"related\"}"),
        IntelligenceClaimStatus::Supported,
        0.82F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    focus_patch.intelligence.evidence.push_back(IntelligenceEvidenceWrite{
        related_hops2_evidence_key,
        strings.intern("bridge"),
        benchmark_source,
        blobs.append_string("{\"edge\":\"related-seed\"}"),
        related_hops2_task_key,
        related_hops2_claim_key,
        0.78F,
        intelligence_fields::kEvidenceKind |
            intelligence_fields::kEvidenceSource |
            intelligence_fields::kEvidenceContent |
            intelligence_fields::kEvidenceTaskKey |
            intelligence_fields::kEvidenceClaimKey |
            intelligence_fields::kEvidenceConfidence
    });
    focus_patch.intelligence.memories.push_back(IntelligenceMemoryWrite{
        related_hops2_memory_key,
        IntelligenceMemoryLayer::Semantic,
        strings.intern("scope:related"),
        blobs.append_string("{\"edge\":\"related-second\"}"),
        related_hops2_task_second_key,
        related_hops2_claim_key,
        0.66F,
        intelligence_fields::kMemoryLayer |
            intelligence_fields::kMemoryScope |
            intelligence_fields::kMemoryContent |
            intelligence_fields::kMemoryTaskKey |
            intelligence_fields::kMemoryClaimKey |
            intelligence_fields::kMemoryImportance
    });
    static_cast<void>(store.apply(focus_patch));

    const InternedStringId anchor_task_key =
        strings.intern("task:bench:" + std::to_string(kIntelligenceBenchmarkRecords / 2U));
    const InternedStringId related_hops2_anchor_task_key = related_hops2_task_key;
    IntelligenceQuery task_query;
    task_query.kind = IntelligenceRecordKind::Tasks;
    task_query.owner = benchmark_owner;
    task_query.task_status = IntelligenceTaskStatus::Completed;
    task_query.min_confidence = 0.75F;

    IntelligenceQuery supported_claim_query;
    supported_claim_query.kind = IntelligenceRecordKind::Claims;
    supported_claim_query.claim_status = IntelligenceClaimStatus::Supported;
    supported_claim_query.min_confidence = 0.7F;

    IntelligenceQuery semantic_claim_query;
    semantic_claim_query.kind = IntelligenceRecordKind::Claims;
    semantic_claim_query.subject_label = semantic_subject;
    semantic_claim_query.relation = semantic_relation;
    semantic_claim_query.object_label = semantic_object;
    semantic_claim_query.limit = 1U;

    IntelligenceQuery supporting_claims_query;
    supporting_claims_query.kind = IntelligenceRecordKind::Claims;
    supporting_claims_query.task_key = supporting_task_key;
    supporting_claims_query.limit = 2U;

    IntelligenceQuery action_candidates_query;
    action_candidates_query.owner = action_owner;
    action_candidates_query.subject_label = benchmark_subject;
    action_candidates_query.relation = benchmark_relation;
    action_candidates_query.object_label = action_object;
    action_candidates_query.limit = 2U;

    IntelligenceQuery agenda_query;
    agenda_query.owner = benchmark_owner;
    agenda_query.limit = 8U;

    IntelligenceQuery recall_query;
    recall_query.scope = benchmark_scope;
    recall_query.memory_layer = IntelligenceMemoryLayer::Semantic;
    recall_query.limit = 8U;

    IntelligenceQuery focus_query;
    focus_query.owner = benchmark_owner;
    focus_query.scope = benchmark_scope;
    focus_query.limit = 8U;

    const std::size_t expected_task_matches = kIntelligenceBenchmarkRecords / 16U;
    const auto task_query_started_at = std::chrono::steady_clock::now();
    std::size_t task_matches = 0U;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot result =
            query_intelligence_records(store.intelligence(), store.strings(), task_query);
        task_matches += result.tasks.size();
    }
    const auto task_query_ended_at = std::chrono::steady_clock::now();

    const auto claim_semantic_query_started_at = std::chrono::steady_clock::now();
    std::size_t claim_semantic_matches = 0U;
    bool claim_semantic_matched = true;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot result =
            query_intelligence_records(store.intelligence(), store.strings(), semantic_claim_query);
        claim_semantic_matches += result.claims.size();
        claim_semantic_matched = claim_semantic_matched &&
            result.claims.size() == 1U &&
            result.claims.front().key == focus_claim_key;
    }
    const auto claim_semantic_query_ended_at = std::chrono::steady_clock::now();

    const auto supporting_claims_query_started_at = std::chrono::steady_clock::now();
    std::size_t supporting_claims_matches = 0U;
    bool supporting_claims_top_matched = true;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot result =
            supporting_intelligence_claims(store.intelligence(), store.strings(), supporting_claims_query);
        supporting_claims_matches += result.claims.size();
        supporting_claims_top_matched = supporting_claims_top_matched &&
            result.claims.size() == 2U &&
            result.claims.front().key == supporting_claim_strong_key &&
            result.claims[1].key == supporting_claim_weak_key;
    }
    const auto supporting_claims_query_ended_at = std::chrono::steady_clock::now();

    const auto action_candidates_query_started_at = std::chrono::steady_clock::now();
    std::size_t action_candidates_matches = 0U;
    bool action_candidates_top_matched = true;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot result =
            action_intelligence_tasks(store.intelligence(), store.strings(), action_candidates_query);
        action_candidates_matches += result.tasks.size();
        action_candidates_top_matched = action_candidates_top_matched &&
            result.tasks.size() == 2U &&
            result.tasks.front().key == action_task_strong_key &&
            result.tasks[1].key == action_task_weak_key;
    }
    const auto action_candidates_query_ended_at = std::chrono::steady_clock::now();

    const auto agenda_query_started_at = std::chrono::steady_clock::now();
    std::size_t agenda_matches = 0U;
    bool agenda_top_matched = true;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot agenda =
            agenda_intelligence_tasks(store.intelligence(), store.strings(), agenda_query);
        agenda_matches += agenda.tasks.size();
        agenda_top_matched = agenda_top_matched &&
            !agenda.tasks.empty() &&
            agenda.tasks.front().key == focus_task_key;
    }
    const auto agenda_query_ended_at = std::chrono::steady_clock::now();

    const auto recall_query_started_at = std::chrono::steady_clock::now();
    std::size_t recall_matches = 0U;
    bool recall_top_matched = true;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot recall =
            recall_intelligence_memories(store.intelligence(), store.strings(), recall_query);
        recall_matches += recall.memories.size();
        recall_top_matched = recall_top_matched &&
            !recall.memories.empty() &&
            recall.memories.front().key == focus_memory_key;
    }
    const auto recall_query_ended_at = std::chrono::steady_clock::now();

    const auto focus_query_started_at = std::chrono::steady_clock::now();
    std::size_t focus_total_records = 0U;
    bool focus_claim_top_matched = true;
    bool focus_task_top_matched = true;
    bool focus_memory_top_matched = true;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot focus =
            focus_intelligence_records(store.intelligence(), store.strings(), focus_query);
        focus_total_records += focus.tasks.size() +
            focus.claims.size() +
            focus.evidence.size() +
            focus.decisions.size() +
            focus.memories.size();
        focus_claim_top_matched = focus_claim_top_matched &&
            !focus.claims.empty() &&
            focus.claims.front().key == focus_claim_key;
        focus_task_top_matched = focus_task_top_matched &&
            !focus.tasks.empty() &&
            focus.tasks.front().key == focus_task_key;
        focus_memory_top_matched = focus_memory_top_matched &&
            !focus.memories.empty() &&
            focus.memories.front().key == focus_memory_key;
    }
    const auto focus_query_ended_at = std::chrono::steady_clock::now();

    const auto related_query_started_at = std::chrono::steady_clock::now();
    std::size_t related_total_records = 0U;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot related =
            related_intelligence_records(store.intelligence(), anchor_task_key, 0U);
        related_total_records += related.tasks.size() +
            related.claims.size() +
            related.evidence.size() +
            related.decisions.size() +
            related.memories.size();
    }
    const auto related_query_ended_at = std::chrono::steady_clock::now();

    const auto related_hops2_query_started_at = std::chrono::steady_clock::now();
    std::size_t related_hops2_total_records = 0U;
    bool related_hops2_expanded = true;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        const IntelligenceSnapshot related =
            related_intelligence_records(store.intelligence(), related_hops2_anchor_task_key, 0U, 0U, 2U);
        related_hops2_total_records += related.tasks.size() +
            related.claims.size() +
            related.evidence.size() +
            related.decisions.size() +
            related.memories.size();
        related_hops2_expanded = related_hops2_expanded &&
            related.tasks.size() == 2U &&
            related.tasks[1].key == related_hops2_task_second_key &&
            related.claims.size() == 1U &&
            related.evidence.size() == 1U &&
            related.memories.size() == 1U;
    }
    const auto related_hops2_query_ended_at = std::chrono::steady_clock::now();

    const auto route_started_at = std::chrono::steady_clock::now();
    std::optional<std::string> selected_route;
    for (std::size_t iteration = 0; iteration < kIntelligenceBenchmarkIterations; ++iteration) {
        selected_route = select_intelligence_route(
            store.intelligence(),
            store.strings(),
            {
                IntelligenceRouteRule{supported_claim_query, 1U, std::nullopt, "publish"},
                IntelligenceRouteRule{IntelligenceQuery{}, 1U, std::nullopt, "fallback"}
            }
        );
    }
    const auto route_ended_at = std::chrono::steady_clock::now();

    assert(task_matches == expected_task_matches * kIntelligenceBenchmarkIterations);
    assert(claim_semantic_matches == kIntelligenceBenchmarkIterations);
    assert(supporting_claims_matches == 2U * kIntelligenceBenchmarkIterations);
    assert(action_candidates_matches == 2U * kIntelligenceBenchmarkIterations);
    assert(agenda_matches == 8U * kIntelligenceBenchmarkIterations);
    assert(recall_matches == 8U * kIntelligenceBenchmarkIterations);
    assert(focus_total_records == 40U * kIntelligenceBenchmarkIterations);
    assert(related_total_records == 5U * kIntelligenceBenchmarkIterations);
    assert(related_hops2_total_records == 5U * kIntelligenceBenchmarkIterations);
    assert(selected_route.has_value());
    assert(*selected_route == "publish");
    assert(claim_semantic_matched);
    assert(supporting_claims_top_matched);
    assert(action_candidates_top_matched);
    assert(agenda_top_matched);
    assert(recall_top_matched);
    assert(focus_claim_top_matched);
    assert(focus_task_top_matched);
    assert(focus_memory_top_matched);
    assert(related_hops2_expanded);

    return IntelligenceQueryBenchmarkRun{
        static_cast<uint64_t>(kIntelligenceBenchmarkRecords),
        static_cast<uint64_t>(kIntelligenceBenchmarkIterations),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                task_query_ended_at - task_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                claim_semantic_query_ended_at - claim_semantic_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                supporting_claims_query_ended_at - supporting_claims_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                action_candidates_query_ended_at - action_candidates_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                agenda_query_ended_at - agenda_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                recall_query_ended_at - recall_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                focus_query_ended_at - focus_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                related_query_ended_at - related_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                related_hops2_query_ended_at - related_hops2_query_started_at
            ).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                route_ended_at - route_started_at
            ).count()
        ),
        static_cast<uint64_t>(task_matches),
        static_cast<uint64_t>(claim_semantic_matches),
        static_cast<uint64_t>(supporting_claims_matches),
        static_cast<uint64_t>(action_candidates_matches),
        static_cast<uint64_t>(agenda_matches),
        static_cast<uint64_t>(recall_matches),
        static_cast<uint64_t>(focus_total_records),
        static_cast<uint64_t>(related_total_records),
        static_cast<uint64_t>(related_hops2_total_records),
        selected_route.has_value() && *selected_route == "publish",
        claim_semantic_matched,
        supporting_claims_top_matched,
        action_candidates_top_matched,
        agenda_top_matched,
        recall_top_matched,
        focus_claim_top_matched,
        focus_task_top_matched,
        focus_memory_top_matched,
        related_hops2_expanded
    };
}

SubgraphBenchmarkRun run_subgraph_benchmark_once(
    const GraphDefinition& parent_graph,
    const GraphDefinition& child_graph
) {
    ExecutionEngine engine = make_benchmark_engine(2U);
    engine.register_graph(child_graph);

    const auto started_at = std::chrono::steady_clock::now();
    RunId run_id = 0U;
    RunResult result;
    for (std::size_t iteration = 0; iteration < kSubgraphBenchmarkRuns; ++iteration) {
        InputEnvelope input;
        input.initial_field_count = 3U;
        input.initial_patch.updates.push_back(FieldUpdate{
            kSubgraphBenchmarkInput,
            int64_t{static_cast<int64_t>(iteration + 1U)}
        });
        run_id = engine.start(parent_graph, input);
        result = engine.run_to_completion(run_id);
        assert(result.status == ExecutionStatus::Completed);
    }
    const auto ended_at = std::chrono::steady_clock::now();

    StreamCursor cursor;
    const auto stream_started_at = std::chrono::steady_clock::now();
    const std::vector<StreamEvent> stream_events =
        engine.stream_events(run_id, cursor, StreamReadOptions{true});
    const auto stream_ended_at = std::chrono::steady_clock::now();

    uint64_t namespaced_events = 0U;
    for (const StreamEvent& event : stream_events) {
        if (!event.namespaces.empty()) {
            ++namespaced_events;
        }
    }

    const WorkflowState& state = engine.state(run_id);
    const auto final_record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(final_record.has_value());
    return SubgraphBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stream_ended_at - stream_started_at).count()
        ),
        static_cast<uint64_t>(stream_events.size()),
        namespaced_events,
        read_int_field(state, kSubgraphBenchmarkOutput),
        compute_run_proof_digest(*final_record, engine.trace().events_for_run(run_id))
    };
}

ResumableSubgraphBenchmarkRun run_resumable_subgraph_benchmark_once(
    const GraphDefinition& parent_graph,
    const GraphDefinition& child_graph
) {
    InputEnvelope input;
    input.initial_field_count = 3U;
    input.initial_patch.updates.push_back(FieldUpdate{
        kResumableSubgraphBenchmarkInput,
        int64_t{11}
    });

    ExecutionEngine direct_engine = make_benchmark_engine(1U);
    direct_engine.register_graph(child_graph);
    const RunId direct_run_id = direct_engine.start(parent_graph, input);
    const StepResult direct_wait = direct_engine.step(direct_run_id);
    assert(direct_wait.progressed);
    assert(direct_wait.node_status == NodeResult::Waiting);
    const ResumeResult direct_resume = direct_engine.resume(direct_wait.checkpoint_id);
    assert(direct_resume.resumed);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const auto direct_record = direct_engine.checkpoints().get(direct_result.last_checkpoint_id);
    assert(direct_record.has_value());
    const RunProofDigest direct_digest = compute_run_proof_digest(
        *direct_record,
        direct_engine.trace().events_for_run(direct_run_id)
    );

    StreamCursor direct_cursor;
    const auto stream_started_at = std::chrono::steady_clock::now();
    const std::vector<StreamEvent> direct_stream =
        direct_engine.stream_events(direct_run_id, direct_cursor, StreamReadOptions{true});
    const auto stream_ended_at = std::chrono::steady_clock::now();

    uint64_t namespaced_events = 0U;
    for (const StreamEvent& event : direct_stream) {
        if (!event.namespaces.empty()) {
            ++namespaced_events;
        }
    }

    const std::string checkpoint_path = "/tmp/agentcore_resumable_subgraph_benchmark.bin";
    std::remove(checkpoint_path.c_str());

    RunId resumed_run_id = 0U;
    CheckpointId resumed_checkpoint_id = 0U;
    std::vector<TraceEvent> prefix_trace;

    const auto started_at = std::chrono::steady_clock::now();
    {
        ExecutionEngine first_engine = make_benchmark_engine(1U);
        first_engine.register_graph(child_graph);
        first_engine.enable_checkpoint_persistence(checkpoint_path);

        resumed_run_id = first_engine.start(parent_graph, input);
        const StepResult first_wait = first_engine.step(resumed_run_id);
        assert(first_wait.progressed);
        assert(first_wait.node_status == NodeResult::Waiting);
        resumed_checkpoint_id = first_wait.checkpoint_id;
        prefix_trace = trace_events_through_checkpoint(
            first_engine.trace().events_for_run(resumed_run_id),
            resumed_checkpoint_id
        );
    }

    ExecutionEngine resumed_engine = make_benchmark_engine(1U);
    resumed_engine.register_graph(parent_graph);
    resumed_engine.register_graph(child_graph);
    resumed_engine.enable_checkpoint_persistence(checkpoint_path);
    assert(resumed_engine.load_persisted_checkpoints() >= 1U);

    const auto resume_started_at = std::chrono::steady_clock::now();
    const ResumeResult resume_result = resumed_engine.resume(resumed_checkpoint_id);
    assert(resume_result.resumed);
    const RunResult resumed_result = resumed_engine.run_to_completion(resumed_run_id);
    const auto resume_ended_at = std::chrono::steady_clock::now();
    assert(resumed_result.status == ExecutionStatus::Completed);
    const auto ended_at = resume_ended_at;

    const auto resumed_record = resumed_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    const RunProofDigest resumed_digest = compute_run_proof_digest(
        *resumed_record,
        append_trace_events(
            prefix_trace,
            resumed_engine.trace().events_for_run(resumed_run_id)
        )
    );
    assert(resumed_digest.combined_digest == direct_digest.combined_digest);

    std::remove(checkpoint_path.c_str());

    return ResumableSubgraphBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(resume_ended_at - resume_started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stream_ended_at - stream_started_at).count()
        ),
        static_cast<uint64_t>(direct_stream.size()),
        namespaced_events,
        read_int_field(direct_engine.state(direct_run_id), kResumableSubgraphBenchmarkOutput),
        direct_digest
    };
}

AsyncSubgraphBenchmarkRun run_async_subgraph_benchmark_once(
    const GraphDefinition& parent_graph,
    const GraphDefinition& child_graph
) {
    ExecutionEngine engine = make_benchmark_engine(1U);
    engine.register_graph(child_graph);
    engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        return ToolResponse{
            true,
            context.blobs.append_string(
                "bench::" + std::string(context.blobs.read_string(request.input))
            ),
            kToolFlagNone
        };
    });

    InputEnvelope input;
    input.initial_field_count = 2U;

    const auto started_at = std::chrono::steady_clock::now();
    const RunId run_id = engine.start(parent_graph, input);
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();
    assert(result.status == ExecutionStatus::Completed);

    StreamCursor cursor;
    const auto stream_started_at = std::chrono::steady_clock::now();
    const std::vector<StreamEvent> stream_events =
        engine.stream_events(run_id, cursor, StreamReadOptions{true});
    const auto stream_ended_at = std::chrono::steady_clock::now();

    uint64_t namespaced_events = 0U;
    for (const StreamEvent& event : stream_events) {
        if (!event.namespaces.empty()) {
            ++namespaced_events;
        }
    }

    const WorkflowState& state = engine.state(run_id);
    assert(std::holds_alternative<BlobRef>(state.load(kAsyncSubgraphBenchmarkSummary)));
    assert(
        engine.state_store(run_id).blobs().read_string(
            std::get<BlobRef>(state.load(kAsyncSubgraphBenchmarkSummary))
        ) == "async-subgraph=bench::benchmark-async"
    );

    const auto final_record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(final_record.has_value());
    return AsyncSubgraphBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stream_ended_at - stream_started_at).count()
        ),
        static_cast<uint64_t>(stream_events.size()),
        namespaced_events,
        compute_run_proof_digest(*final_record, engine.trace().events_for_run(run_id))
    };
}

AsyncMultiWaitSubgraphBenchmarkRun run_async_multi_wait_subgraph_benchmark_once(
    const GraphDefinition& parent_graph,
    const GraphDefinition& child_graph
) {
    ExecutionEngine engine = make_benchmark_engine(1U);
    engine.register_graph(child_graph);
    engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        return ToolResponse{
            true,
            context.blobs.append_string(
                "bench::" + std::string(context.blobs.read_string(request.input))
            ),
            kToolFlagNone
        };
    });

    InputEnvelope input;
    input.initial_field_count = 2U;

    const auto started_at = std::chrono::steady_clock::now();
    const RunId run_id = engine.start(parent_graph, input);
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();
    assert(result.status == ExecutionStatus::Completed);

    StreamCursor cursor;
    const auto stream_started_at = std::chrono::steady_clock::now();
    const std::vector<StreamEvent> stream_events =
        engine.stream_events(run_id, cursor, StreamReadOptions{true});
    const auto stream_ended_at = std::chrono::steady_clock::now();

    uint64_t namespaced_events = 0U;
    for (const StreamEvent& event : stream_events) {
        if (!event.namespaces.empty()) {
            ++namespaced_events;
        }
    }

    const WorkflowState& state = engine.state(run_id);
    assert(std::holds_alternative<BlobRef>(state.load(kAsyncMultiWaitSubgraphBenchmarkSummary)));
    assert(
        engine.state_store(run_id).blobs().read_string(
            std::get<BlobRef>(state.load(kAsyncMultiWaitSubgraphBenchmarkSummary))
        ) == "multi-wait-subgraph=left=bench::join-left right=bench::join-right"
    );

    const auto final_record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(final_record.has_value());
    return AsyncMultiWaitSubgraphBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stream_ended_at - stream_started_at).count()
        ),
        static_cast<uint64_t>(stream_events.size()),
        namespaced_events,
        compute_run_proof_digest(*final_record, engine.trace().events_for_run(run_id))
    };
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    GraphDefinition parallel_graph = make_parallel_benchmark_graph();
    std::string error_message;
    assert(parallel_graph.validate(&error_message));

    GraphDefinition routing_graph = make_routing_benchmark_graph();
    assert(routing_graph.validate(&error_message));
    const NodeDefinition* router = routing_graph.find_node(1);
    assert(router != nullptr);
    assert(routing_graph.compiled_routes_view(*router).size() == static_cast<std::size_t>(kRoutingRouteCount + 1));

    GraphDefinition naive_frontier_graph = make_naive_frontier_benchmark_graph();
    assert(naive_frontier_graph.validate(&error_message));
    GraphDefinition reactive_frontier_graph = make_reactive_frontier_benchmark_graph();
    assert(reactive_frontier_graph.validate(&error_message));
    GraphDefinition memoization_baseline_graph = make_memoization_benchmark_graph(
        480U,
        "memoization_baseline_benchmark_graph",
        memoization_baseline_node,
        memoization_stable_router_node,
        {}
    );
    assert(memoization_baseline_graph.validate(&error_message));
    GraphDefinition memoization_hit_graph = make_memoization_benchmark_graph(
        481U,
        "memoization_hit_benchmark_graph",
        memoization_hit_node,
        memoization_stable_router_node,
        NodeMemoizationPolicy{true, std::vector<StateKey>{kMemoizationBenchmarkInput}, 64U}
    );
    assert(memoization_hit_graph.validate(&error_message));
    GraphDefinition memoization_invalidation_graph = make_memoization_benchmark_graph(
        482U,
        "memoization_invalidation_benchmark_graph",
        memoization_invalidation_node,
        memoization_invalidation_router_node,
        NodeMemoizationPolicy{true, std::vector<StateKey>{kMemoizationBenchmarkInput}, 64U}
    );
    assert(memoization_invalidation_graph.validate(&error_message));
    GraphDefinition subgraph_child_graph = make_subgraph_benchmark_child_graph();
    assert(subgraph_child_graph.validate(&error_message));
    GraphDefinition subgraph_parent_graph = make_subgraph_benchmark_parent_graph();
    assert(subgraph_parent_graph.validate(&error_message));
    GraphDefinition resumable_subgraph_child_graph = make_resumable_subgraph_benchmark_child_graph();
    assert(resumable_subgraph_child_graph.validate(&error_message));
    GraphDefinition resumable_subgraph_parent_graph = make_resumable_subgraph_benchmark_parent_graph();
    assert(resumable_subgraph_parent_graph.validate(&error_message));
    GraphDefinition async_subgraph_child_graph = make_async_subgraph_benchmark_child_graph();
    assert(async_subgraph_child_graph.validate(&error_message));
    GraphDefinition async_subgraph_parent_graph = make_async_subgraph_benchmark_parent_graph();
    assert(async_subgraph_parent_graph.validate(&error_message));
    GraphDefinition async_multi_wait_subgraph_child_graph = make_async_multi_wait_subgraph_benchmark_child_graph();
    assert(async_multi_wait_subgraph_child_graph.validate(&error_message));
    GraphDefinition async_multi_wait_subgraph_parent_graph = make_async_multi_wait_subgraph_benchmark_parent_graph();
    assert(async_multi_wait_subgraph_parent_graph.validate(&error_message));

    const std::size_t worker_count = std::max<std::size_t>(
        2U,
        std::min<std::size_t>(4U, std::thread::hardware_concurrency() == 0U
            ? 4U
            : static_cast<std::size_t>(std::thread::hardware_concurrency()))
    );

    const BenchmarkRun sequential = run_parallel_benchmark_once(parallel_graph, 1U);
    const BenchmarkRun parallel = run_parallel_benchmark_once(parallel_graph, worker_count);

    assert(sequential.summary == "sum=10 triples=4");
    assert(parallel.summary == sequential.summary);

    const double parallel_speedup = parallel.elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(sequential.elapsed_ns) / static_cast<double>(parallel.elapsed_ns);

    const CheckpointPolicy routing_legacy_policy{
        1U,
        true,
        true,
        true,
        true
    };
    const CheckpointPolicy routing_optimized_policy{
        64U,
        true,
        true,
        true,
        true
    };

    const RoutingBenchmarkRun routing_legacy = run_routing_benchmark_once(
        routing_graph,
        routing_legacy_policy
    );
    const RoutingBenchmarkRun routing_optimized = run_routing_benchmark_once(
        routing_graph,
        routing_optimized_policy
    );
    assert(routing_legacy.accumulator == expected_routing_accumulator());
    assert(routing_optimized.accumulator == routing_legacy.accumulator);
    assert(routing_optimized.proof.combined_digest == routing_legacy.proof.combined_digest);
    assert(routing_optimized.resumable_checkpoints < routing_legacy.resumable_checkpoints);

    const double routing_steps_per_second = routing_optimized.elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(routing_optimized.trace_events) * 1000000000.0 /
            static_cast<double>(routing_optimized.elapsed_ns);
    const double routing_speedup = routing_optimized.elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(routing_legacy.elapsed_ns) / static_cast<double>(routing_optimized.elapsed_ns);

    const StateForkBenchmarkRun fork_materialized = run_state_fork_benchmark_once(true);
    const StateForkBenchmarkRun fork_shared = run_state_fork_benchmark_once(false);
    assert(fork_materialized.forks == kForkBenchmarkCopies);
    assert(fork_shared.forks == fork_materialized.forks);
    assert(fork_materialized.accumulator_sum == fork_shared.accumulator_sum);
    assert(!fork_materialized.blobs_shared_after_patch);
    assert(!fork_materialized.strings_shared_after_patch);
    assert(!fork_materialized.knowledge_graph_shared_after_patch);
    assert(fork_shared.blobs_shared_after_patch);
    assert(fork_shared.strings_shared_after_patch);
    assert(fork_shared.knowledge_graph_shared_after_patch);
    const double fork_speedup = fork_shared.elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(fork_materialized.elapsed_ns) / static_cast<double>(fork_shared.elapsed_ns);

    const FrontierBenchmarkRun naive_frontier = run_frontier_benchmark_once(naive_frontier_graph, 2U);
    const FrontierBenchmarkRun reactive_frontier = run_frontier_benchmark_once(reactive_frontier_graph, 3U);
    assert(naive_frontier.triggered_watchers == static_cast<uint64_t>(kFrontierSubscriberCount));
    assert(reactive_frontier.triggered_watchers == 1U);
    const double frontier_speedup = reactive_frontier.elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(naive_frontier.elapsed_ns) / static_cast<double>(reactive_frontier.elapsed_ns);

    const QueueStatusBenchmarkRun queue_status = run_queue_status_benchmark_once();
    assert(queue_status.same_result);
    const double queue_status_speedup = queue_status.indexed_probe_ns == 0U
        ? 0.0
        : static_cast<double>(queue_status.legacy_probe_ns) /
            static_cast<double>(queue_status.indexed_probe_ns);
    const RecordedEffectBenchmarkRun recorded_effect_benchmark = run_recorded_effect_benchmark_once();
    assert(recorded_effect_benchmark.miss_producer_calls == kRecordedEffectBenchmarkIterations);
    assert(recorded_effect_benchmark.hit_producer_calls == 1U);
    assert(recorded_effect_benchmark.journal_records == kRecordedEffectBenchmarkIterations);
    const double recorded_effect_hit_speedup = recorded_effect_benchmark.hit_elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(recorded_effect_benchmark.miss_elapsed_ns) /
            static_cast<double>(recorded_effect_benchmark.hit_elapsed_ns);

    const MemoizationBenchmarkRun memoization_baseline = run_memoization_benchmark_once(
        memoization_baseline_graph,
        7,
        &g_memoization_baseline_invocations
    );
    const MemoizationBenchmarkRun memoization_hit = run_memoization_benchmark_once(
        memoization_hit_graph,
        7,
        &g_memoization_hit_invocations
    );
    const MemoizationBenchmarkRun memoization_invalidation = run_memoization_benchmark_once(
        memoization_invalidation_graph,
        7,
        &g_memoization_invalidation_invocations
    );
    assert(memoization_baseline.executor_invocations == kMemoizationBenchmarkVisitLimit);
    assert(memoization_hit.executor_invocations == 1U);
    assert(memoization_invalidation.executor_invocations == kMemoizationBenchmarkVisitLimit);
    assert(memoization_baseline.final_input == 7);
    assert(memoization_hit.final_input == memoization_baseline.final_input);
    assert(memoization_baseline.final_output == memoization_hit.final_output);
    assert(memoization_baseline.final_visits == static_cast<int64_t>(kMemoizationBenchmarkVisitLimit));
    assert(memoization_hit.final_visits == memoization_baseline.final_visits);
    assert(memoization_baseline.trace_events == memoization_hit.trace_events);
    assert(memoization_invalidation.trace_events == memoization_baseline.trace_events);
    assert(
        memoization_invalidation.final_input ==
        7 + static_cast<int64_t>(kMemoizationBenchmarkVisitLimit - 1U)
    );
    assert(
        memoization_invalidation.final_output ==
        benchmark_mix_value(memoization_invalidation.final_input)
    );
    assert(
        memoization_invalidation.final_visits ==
        static_cast<int64_t>(kMemoizationBenchmarkVisitLimit)
    );
    const double memoization_hit_speedup = memoization_hit.elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(memoization_baseline.elapsed_ns) /
            static_cast<double>(memoization_hit.elapsed_ns);
    const double memoization_invalidation_vs_hit = memoization_hit.elapsed_ns == 0U
        ? 0.0
        : static_cast<double>(memoization_invalidation.elapsed_ns) /
            static_cast<double>(memoization_hit.elapsed_ns);
    const KnowledgeReasoningBenchmarkRun knowledge_reasoning_benchmark =
        run_knowledge_reasoning_benchmark_once();
    assert(knowledge_reasoning_benchmark.triples == kKnowledgeReasoningBenchmarkTriples);
    assert(knowledge_reasoning_benchmark.iterations == kKnowledgeReasoningBenchmarkIterations);
    assert(knowledge_reasoning_benchmark.exact_matched);
    assert(knowledge_reasoning_benchmark.constrained_matched);
    assert(knowledge_reasoning_benchmark.neighbor_matched);
    const KnowledgeIngestBenchmarkRun knowledge_ingest_benchmark =
        run_knowledge_ingest_benchmark_once();
    assert(knowledge_ingest_benchmark.triples == kKnowledgeIngestBenchmarkTriples);
    assert(knowledge_ingest_benchmark.final_triples == kKnowledgeIngestBenchmarkTriples);
    assert(knowledge_ingest_benchmark.created_triples == kKnowledgeIngestBenchmarkTriples);
    assert(knowledge_ingest_benchmark.exact_matched);
    const IntelligenceQueryBenchmarkRun intelligence_query_benchmark =
        run_intelligence_query_benchmark_once();
    assert(intelligence_query_benchmark.records == kIntelligenceBenchmarkRecords);
    assert(intelligence_query_benchmark.iterations == kIntelligenceBenchmarkIterations);
    assert(intelligence_query_benchmark.route_matched);
    assert(intelligence_query_benchmark.claim_semantic_matched);
    assert(intelligence_query_benchmark.supporting_claims_top_matched);
    assert(intelligence_query_benchmark.action_candidates_top_matched);
    assert(intelligence_query_benchmark.agenda_top_matched);
    assert(intelligence_query_benchmark.recall_top_matched);
    assert(intelligence_query_benchmark.focus_claim_top_matched);
    assert(intelligence_query_benchmark.focus_task_top_matched);
    assert(intelligence_query_benchmark.focus_memory_top_matched);
    assert(intelligence_query_benchmark.related_hops2_expanded);
    assert(
        intelligence_query_benchmark.task_matches ==
        (kIntelligenceBenchmarkRecords / 16U) * kIntelligenceBenchmarkIterations
    );
    assert(
        intelligence_query_benchmark.claim_semantic_matches ==
        kIntelligenceBenchmarkIterations
    );
    assert(
        intelligence_query_benchmark.supporting_claims_matches ==
        2U * kIntelligenceBenchmarkIterations
    );
    assert(
        intelligence_query_benchmark.action_candidates_matches ==
        2U * kIntelligenceBenchmarkIterations
    );
    assert(intelligence_query_benchmark.agenda_matches == 8U * kIntelligenceBenchmarkIterations);
    assert(intelligence_query_benchmark.recall_matches == 8U * kIntelligenceBenchmarkIterations);
    assert(intelligence_query_benchmark.focus_total_records == 40U * kIntelligenceBenchmarkIterations);
    assert(intelligence_query_benchmark.related_total_records == 5U * kIntelligenceBenchmarkIterations);
    assert(intelligence_query_benchmark.related_hops2_total_records == 5U * kIntelligenceBenchmarkIterations);
    const ContextGraphBenchmarkRun context_graph_benchmark = run_context_graph_benchmark_once();
    assert(context_graph_benchmark.records == kContextGraphBenchmarkRecords);
    assert(context_graph_benchmark.iterations == kContextGraphBenchmarkIterations);
    assert(context_graph_benchmark.selected_records > 0U);
    assert(context_graph_benchmark.claim_matched);
    assert(context_graph_benchmark.evidence_matched);
    assert(context_graph_benchmark.knowledge_matched);

    const SubgraphBenchmarkRun subgraph_benchmark =
        run_subgraph_benchmark_once(subgraph_parent_graph, subgraph_child_graph);
    assert(subgraph_benchmark.output_value == static_cast<int64_t>(kSubgraphBenchmarkRuns + 11U));
    assert(subgraph_benchmark.stream_events == 5U);
    assert(subgraph_benchmark.namespaced_events == 2U);
    const ResumableSubgraphBenchmarkRun resumable_subgraph_benchmark =
        run_resumable_subgraph_benchmark_once(
            resumable_subgraph_parent_graph,
            resumable_subgraph_child_graph
        );
    assert(resumable_subgraph_benchmark.output_value == 28);
    assert(resumable_subgraph_benchmark.stream_events == 7U);
    assert(resumable_subgraph_benchmark.namespaced_events == 3U);
    const AsyncSubgraphBenchmarkRun async_subgraph_benchmark =
        run_async_subgraph_benchmark_once(
            async_subgraph_parent_graph,
            async_subgraph_child_graph
        );
    assert(async_subgraph_benchmark.stream_events == 7U);
    assert(async_subgraph_benchmark.namespaced_events == 3U);
    const AsyncMultiWaitSubgraphBenchmarkRun async_multi_wait_subgraph_benchmark =
        run_async_multi_wait_subgraph_benchmark_once(
            async_multi_wait_subgraph_parent_graph,
            async_multi_wait_subgraph_child_graph
        );
    assert(async_multi_wait_subgraph_benchmark.namespaced_events == 9U);

    std::cout << "agentcore_runtime_benchmark" << '\n'
              << "execution_profile=" << benchmark_profile_name() << '\n'
              << "workers_sequential=1" << '\n'
              << "workers_parallel=" << worker_count << '\n'
              << "summary=" << parallel.summary << '\n'
              << "sequential_ns=" << sequential.elapsed_ns << '\n'
              << "parallel_ns=" << parallel.elapsed_ns << '\n'
              << "speedup_x=" << parallel_speedup << '\n'
              << "sequential_digest=" << sequential.proof.combined_digest << '\n'
              << "parallel_digest=" << parallel.proof.combined_digest << '\n'
              << "routing_routes=" << kRoutingRouteCount << '\n'
              << "routing_iterations=" << kRoutingIterations << '\n'
              << "routing_trace_events=" << routing_optimized.trace_events << '\n'
              << "routing_legacy_ns=" << routing_legacy.elapsed_ns << '\n'
              << "routing_optimized_ns=" << routing_optimized.elapsed_ns << '\n'
              << "routing_speedup_x=" << routing_speedup << '\n'
              << "routing_checkpoint_records=" << routing_optimized.checkpoint_records << '\n'
              << "routing_legacy_resumable_checkpoints=" << routing_legacy.resumable_checkpoints << '\n'
              << "routing_optimized_resumable_checkpoints=" << routing_optimized.resumable_checkpoints << '\n'
              << "routing_steps_per_second=" << routing_steps_per_second << '\n'
              << "routing_digest=" << routing_optimized.proof.combined_digest << '\n'
              << "fork_benchmark_copies=" << fork_shared.forks << '\n'
              << "fork_benchmark_blob_bytes=" << kForkBenchmarkBlobBytes << '\n'
              << "fork_benchmark_triples=" << kForkBenchmarkTripleCount << '\n'
              << "fork_materialized_ns=" << fork_materialized.elapsed_ns << '\n'
              << "fork_shared_ns=" << fork_shared.elapsed_ns << '\n'
              << "fork_speedup_x=" << fork_speedup << '\n'
              << "fork_shared_blob_backing=" << static_cast<int>(fork_shared.blobs_shared_after_patch) << '\n'
              << "fork_shared_string_backing=" << static_cast<int>(fork_shared.strings_shared_after_patch) << '\n'
              << "fork_shared_kg_backing=" << static_cast<int>(fork_shared.knowledge_graph_shared_after_patch) << '\n'
              << "frontier_subscribers=" << kFrontierSubscriberCount << '\n'
              << "frontier_hot_subscriber=" << kFrontierHotSubscriber << '\n'
              << "frontier_naive_ns=" << naive_frontier.elapsed_ns << '\n'
              << "frontier_reactive_ns=" << reactive_frontier.elapsed_ns << '\n'
              << "frontier_speedup_x=" << frontier_speedup << '\n'
              << "frontier_naive_trace_events=" << naive_frontier.trace_events << '\n'
              << "frontier_reactive_trace_events=" << reactive_frontier.trace_events << '\n'
              << "frontier_naive_triggered_watchers=" << naive_frontier.triggered_watchers << '\n'
              << "frontier_reactive_triggered_watchers=" << reactive_frontier.triggered_watchers << '\n'
              << "queue_status_probe_tasks=" << queue_status.queued_tasks << '\n'
              << "queue_status_probe_iterations=" << queue_status.probe_iterations << '\n'
              << "queue_status_probe_legacy_ns=" << queue_status.legacy_probe_ns << '\n'
              << "queue_status_probe_indexed_ns=" << queue_status.indexed_probe_ns << '\n'
              << "queue_status_probe_speedup_x=" << queue_status_speedup << '\n'
              << "recorded_effect_iterations=" << kRecordedEffectBenchmarkIterations << '\n'
              << "recorded_effect_miss_ns=" << recorded_effect_benchmark.miss_elapsed_ns << '\n'
              << "recorded_effect_hit_ns=" << recorded_effect_benchmark.hit_elapsed_ns << '\n'
              << "recorded_effect_hit_speedup_x=" << recorded_effect_hit_speedup << '\n'
              << "recorded_effect_miss_producer_calls=" << recorded_effect_benchmark.miss_producer_calls << '\n'
              << "recorded_effect_hit_producer_calls=" << recorded_effect_benchmark.hit_producer_calls << '\n'
              << "recorded_effect_journal_records=" << recorded_effect_benchmark.journal_records << '\n'
              << "memoization_visits=" << kMemoizationBenchmarkVisitLimit << '\n'
              << "memoization_mix_rounds=" << kMemoizationBenchmarkMixRounds << '\n'
              << "memoization_baseline_ns=" << memoization_baseline.elapsed_ns << '\n'
              << "memoization_hit_ns=" << memoization_hit.elapsed_ns << '\n'
              << "memoization_invalidation_ns=" << memoization_invalidation.elapsed_ns << '\n'
              << "memoization_hit_speedup_x=" << memoization_hit_speedup << '\n'
              << "memoization_invalidation_vs_hit_x=" << memoization_invalidation_vs_hit << '\n'
              << "memoization_baseline_executor_invocations=" << memoization_baseline.executor_invocations << '\n'
              << "memoization_hit_executor_invocations=" << memoization_hit.executor_invocations << '\n'
              << "memoization_invalidation_executor_invocations=" << memoization_invalidation.executor_invocations << '\n'
              << "memoization_trace_events=" << memoization_hit.trace_events << '\n'
              << "memoization_hit_output_equal=" << static_cast<int>(
                    memoization_baseline.final_output == memoization_hit.final_output
                 ) << '\n'
              << "memoization_invalidation_final_input=" << memoization_invalidation.final_input << '\n'
              << "knowledge_reasoning_triples=" << knowledge_reasoning_benchmark.triples << '\n'
              << "knowledge_reasoning_iterations=" << knowledge_reasoning_benchmark.iterations << '\n'
              << "knowledge_reasoning_exact_lookup_ns="
              << knowledge_reasoning_benchmark.exact_lookup_ns << '\n'
              << "knowledge_reasoning_constrained_lookup_ns="
              << knowledge_reasoning_benchmark.constrained_lookup_ns << '\n'
              << "knowledge_reasoning_neighbor_scan_ns="
              << knowledge_reasoning_benchmark.neighbor_scan_ns << '\n'
              << "knowledge_reasoning_exact_matches="
              << knowledge_reasoning_benchmark.exact_matches << '\n'
              << "knowledge_reasoning_constrained_matches="
              << knowledge_reasoning_benchmark.constrained_matches << '\n'
              << "knowledge_reasoning_neighbor_total="
              << knowledge_reasoning_benchmark.neighbor_total << '\n'
              << "knowledge_ingest_triples=" << knowledge_ingest_benchmark.triples << '\n'
              << "knowledge_ingest_batches=" << knowledge_ingest_benchmark.batches << '\n'
              << "knowledge_ingest_ns=" << knowledge_ingest_benchmark.elapsed_ns << '\n'
              << "knowledge_ingest_created_entities="
              << knowledge_ingest_benchmark.created_entities << '\n'
              << "knowledge_ingest_created_triples="
              << knowledge_ingest_benchmark.created_triples << '\n'
              << "knowledge_ingest_final_entities="
              << knowledge_ingest_benchmark.final_entities << '\n'
              << "knowledge_ingest_final_triples="
              << knowledge_ingest_benchmark.final_triples << '\n'
              << "knowledge_ingest_exact_matched="
              << static_cast<int>(knowledge_ingest_benchmark.exact_matched) << '\n'
              << "intelligence_benchmark_records=" << intelligence_query_benchmark.records << '\n'
              << "intelligence_benchmark_iterations=" << intelligence_query_benchmark.iterations << '\n'
              << "intelligence_task_query_ns=" << intelligence_query_benchmark.task_query_ns << '\n'
              << "intelligence_claim_semantic_query_ns="
              << intelligence_query_benchmark.claim_semantic_query_ns << '\n'
              << "intelligence_supporting_claims_query_ns="
              << intelligence_query_benchmark.supporting_claims_query_ns << '\n'
              << "intelligence_action_candidates_query_ns="
              << intelligence_query_benchmark.action_candidates_query_ns << '\n'
              << "intelligence_agenda_query_ns=" << intelligence_query_benchmark.agenda_query_ns << '\n'
              << "intelligence_recall_query_ns=" << intelligence_query_benchmark.recall_query_ns << '\n'
              << "intelligence_focus_query_ns=" << intelligence_query_benchmark.focus_query_ns << '\n'
              << "intelligence_related_query_ns=" << intelligence_query_benchmark.related_query_ns << '\n'
              << "intelligence_related_hops2_query_ns=" << intelligence_query_benchmark.related_hops2_query_ns << '\n'
              << "intelligence_route_select_ns=" << intelligence_query_benchmark.route_select_ns << '\n'
              << "intelligence_task_matches=" << intelligence_query_benchmark.task_matches << '\n'
              << "intelligence_claim_semantic_matches="
              << intelligence_query_benchmark.claim_semantic_matches << '\n'
              << "intelligence_supporting_claims_matches="
              << intelligence_query_benchmark.supporting_claims_matches << '\n'
              << "intelligence_action_candidates_matches="
              << intelligence_query_benchmark.action_candidates_matches << '\n'
              << "intelligence_agenda_matches=" << intelligence_query_benchmark.agenda_matches << '\n'
              << "intelligence_recall_matches=" << intelligence_query_benchmark.recall_matches << '\n'
              << "intelligence_focus_total_records=" << intelligence_query_benchmark.focus_total_records << '\n'
              << "intelligence_related_total_records=" << intelligence_query_benchmark.related_total_records << '\n'
              << "intelligence_related_hops2_total_records=" << intelligence_query_benchmark.related_hops2_total_records << '\n'
              << "intelligence_route_matched=" << static_cast<int>(intelligence_query_benchmark.route_matched) << '\n'
              << "intelligence_claim_semantic_matched="
              << static_cast<int>(intelligence_query_benchmark.claim_semantic_matched) << '\n'
              << "intelligence_supporting_claims_top_matched="
              << static_cast<int>(intelligence_query_benchmark.supporting_claims_top_matched) << '\n'
              << "intelligence_action_candidates_top_matched="
              << static_cast<int>(intelligence_query_benchmark.action_candidates_top_matched) << '\n'
              << "intelligence_agenda_top_matched=" << static_cast<int>(intelligence_query_benchmark.agenda_top_matched) << '\n'
              << "intelligence_recall_top_matched=" << static_cast<int>(intelligence_query_benchmark.recall_top_matched) << '\n'
              << "intelligence_focus_claim_top_matched=" << static_cast<int>(intelligence_query_benchmark.focus_claim_top_matched) << '\n'
              << "intelligence_focus_task_top_matched=" << static_cast<int>(intelligence_query_benchmark.focus_task_top_matched) << '\n'
              << "intelligence_focus_memory_top_matched=" << static_cast<int>(intelligence_query_benchmark.focus_memory_top_matched) << '\n'
              << "intelligence_related_hops2_expanded=" << static_cast<int>(intelligence_query_benchmark.related_hops2_expanded) << '\n'
              << "context_graph_records=" << context_graph_benchmark.records << '\n'
              << "context_graph_iterations=" << context_graph_benchmark.iterations << '\n'
              << "context_graph_cold_rank_ns=" << context_graph_benchmark.cold_rank_ns << '\n'
              << "context_graph_warm_rank_ns=" << context_graph_benchmark.warm_rank_ns << '\n'
              << "context_graph_warm_rank_avg_ns="
              << (context_graph_benchmark.warm_rank_ns / context_graph_benchmark.iterations) << '\n'
              << "context_graph_selected_records=" << context_graph_benchmark.selected_records << '\n'
              << "context_graph_claim_matched=" << static_cast<int>(context_graph_benchmark.claim_matched) << '\n'
              << "context_graph_evidence_matched=" << static_cast<int>(context_graph_benchmark.evidence_matched) << '\n'
              << "context_graph_knowledge_matched=" << static_cast<int>(context_graph_benchmark.knowledge_matched) << '\n'
              << "subgraph_benchmark_runs=" << kSubgraphBenchmarkRuns << '\n'
              << "subgraph_benchmark_ns=" << subgraph_benchmark.elapsed_ns << '\n'
              << "subgraph_stream_read_ns=" << subgraph_benchmark.stream_read_ns << '\n'
              << "subgraph_stream_events=" << subgraph_benchmark.stream_events << '\n'
              << "subgraph_namespaced_events=" << subgraph_benchmark.namespaced_events << '\n'
              << "subgraph_output_value=" << subgraph_benchmark.output_value << '\n'
              << "subgraph_digest=" << subgraph_benchmark.proof.combined_digest << '\n'
              << "resumable_subgraph_benchmark_ns=" << resumable_subgraph_benchmark.elapsed_ns << '\n'
              << "resumable_subgraph_resume_ns=" << resumable_subgraph_benchmark.resume_ns << '\n'
              << "resumable_subgraph_stream_read_ns=" << resumable_subgraph_benchmark.stream_read_ns << '\n'
              << "resumable_subgraph_stream_events=" << resumable_subgraph_benchmark.stream_events << '\n'
              << "resumable_subgraph_namespaced_events=" << resumable_subgraph_benchmark.namespaced_events << '\n'
              << "resumable_subgraph_output_value=" << resumable_subgraph_benchmark.output_value << '\n'
              << "resumable_subgraph_digest=" << resumable_subgraph_benchmark.proof.combined_digest << '\n'
              << "async_subgraph_benchmark_ns=" << async_subgraph_benchmark.elapsed_ns << '\n'
              << "async_subgraph_stream_read_ns=" << async_subgraph_benchmark.stream_read_ns << '\n'
              << "async_subgraph_stream_events=" << async_subgraph_benchmark.stream_events << '\n'
              << "async_subgraph_namespaced_events=" << async_subgraph_benchmark.namespaced_events << '\n'
              << "async_subgraph_digest=" << async_subgraph_benchmark.proof.combined_digest << '\n'
              << "async_multi_wait_subgraph_benchmark_ns=" << async_multi_wait_subgraph_benchmark.elapsed_ns << '\n'
              << "async_multi_wait_subgraph_stream_read_ns=" << async_multi_wait_subgraph_benchmark.stream_read_ns << '\n'
              << "async_multi_wait_subgraph_stream_events=" << async_multi_wait_subgraph_benchmark.stream_events << '\n'
              << "async_multi_wait_subgraph_namespaced_events=" << async_multi_wait_subgraph_benchmark.namespaced_events << '\n'
              << "async_multi_wait_subgraph_digest=" << async_multi_wait_subgraph_benchmark.proof.combined_digest << '\n'
              << "verified=1" << std::endl;

    return 0;
}
