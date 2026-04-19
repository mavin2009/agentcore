#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/graph/graph_ir.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace agentcore {

namespace {

enum PersistentSessionBenchmarkStateKey : StateKey {
    kSessionBenchmarkSessionId = 0,
    kSessionBenchmarkInput = 1,
    kSessionBenchmarkOutput = 2,
    kSessionBenchmarkVisits = 3,
    kSessionBenchmarkRound = 4,
    kSessionBenchmarkResumeAttempt = 5
};

enum PersistentSessionBenchmarkChildStateKey : StateKey {
    kSessionBenchmarkChildSessionId = 0,
    kSessionBenchmarkChildInput = 1,
    kSessionBenchmarkChildOutput = 2,
    kSessionBenchmarkChildVisits = 3,
    kSessionBenchmarkChildResumeAttempt = 4
};

constexpr std::size_t kPersistentSessionBenchmarkBranches = 16U;

std::atomic<uint64_t> g_persistent_session_benchmark_producer_calls{0U};

struct PersistentSessionParallelBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t stream_read_ns{0};
    uint64_t committed_sessions{0};
    uint64_t stream_events{0};
    uint64_t namespaced_events{0};
    uint64_t session_tagged_events{0};
    RunProofDigest proof{};
};

struct PersistentSessionResumeBenchmarkRun {
    uint64_t elapsed_ns{0};
    uint64_t resume_ns{0};
    uint64_t committed_sessions{0};
    uint64_t stream_events{0};
    uint64_t namespaced_events{0};
    uint64_t session_tagged_events{0};
    uint64_t producer_calls_direct{0};
    uint64_t producer_calls_resumed{0};
    int64_t final_output{0};
    RunProofDigest direct_proof{};
    RunProofDigest resumed_proof{};
};

int64_t read_int_field(const WorkflowState& state, StateKey key) {
    if (key >= state.size() || !std::holds_alternative<int64_t>(state.load(key))) {
        return 0;
    }
    return std::get<int64_t>(state.load(key));
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

std::vector<TraceEvent> normalize_trace_for_resume_proof(std::vector<TraceEvent> events) {
    for (TraceEvent& event : events) {
        event.checkpoint_id = 0U;
    }
    return events;
}

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult fanout_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult prepare_parallel_session_node(ExecutionContext& context) {
    const int64_t session_index =
        static_cast<int64_t>(context.node_id) - static_cast<int64_t>(1U);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kSessionBenchmarkSessionId, session_index});
    patch.updates.push_back(FieldUpdate{kSessionBenchmarkInput, int64_t{100} + session_index});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult persistent_session_benchmark_child_node(ExecutionContext& context) {
    const int64_t session_id = read_int_field(context.state, kSessionBenchmarkChildSessionId);
    const int64_t input_value = read_int_field(context.state, kSessionBenchmarkChildInput);
    const int64_t visits = read_int_field(context.state, kSessionBenchmarkChildVisits) + 1;

    const RecordedEffectResult memoized = context.record_text_effect_once(
        "benchmark-session::" + std::to_string(session_id),
        "request::stable",
        [&]() {
            g_persistent_session_benchmark_producer_calls.fetch_add(1U);
            return std::string("memo::") + std::to_string(session_id);
        }
    );
    (void)memoized;

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kSessionBenchmarkChildVisits, visits});
    patch.updates.push_back(FieldUpdate{
        kSessionBenchmarkChildOutput,
        input_value + visits
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult persistent_session_wait_child_node(ExecutionContext& context) {
    const int64_t attempt = read_int_field(context.state, kSessionBenchmarkChildResumeAttempt);
    if (attempt == 0) {
        StatePatch patch;
        patch.updates.push_back(FieldUpdate{kSessionBenchmarkChildResumeAttempt, int64_t{1}});
        return NodeResult::waiting(std::move(patch), 0.90F);
    }
    return persistent_session_benchmark_child_node(context);
}

NodeResult revisit_persistent_session_node(ExecutionContext& context) {
    const int64_t round = read_int_field(context.state, kSessionBenchmarkRound);
    if (round == 0) {
        StatePatch patch;
        patch.updates.push_back(FieldUpdate{kSessionBenchmarkRound, int64_t{1}});
        patch.updates.push_back(FieldUpdate{
            kSessionBenchmarkInput,
            read_int_field(context.state, kSessionBenchmarkInput) + 1
        });
        NodeResult result = NodeResult::success(std::move(patch), 0.98F);
        result.next_override = 1U;
        return result;
    }
    return NodeResult::success({}, 0.98F);
}

GraphDefinition make_persistent_session_benchmark_child_graph(
    GraphId graph_id,
    NodeExecutorFn executor
) {
    GraphDefinition graph;
    graph.id = graph_id;
    graph.name = "persistent_session_benchmark_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Compute,
            "session_step",
            0U,
            0U,
            0U,
            executor,
            {}
        },
        NodeDefinition{
            2U,
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
    return graph;
}

GraphDefinition make_parallel_persistent_session_benchmark_parent_graph(
    GraphId child_graph_id,
    std::size_t branch_count
) {
    GraphDefinition graph;
    graph.id = 980U;
    graph.name = "parallel_persistent_session_benchmark_parent";
    graph.entry = 1U;
    graph.nodes.push_back(NodeDefinition{
        1U,
        NodeKind::Control,
        "fanout_sessions",
        node_policy_mask(NodePolicyFlag::AllowFanOut),
        0U,
        0U,
        fanout_node,
        {}
    });

    std::vector<EdgeId> fanout_edges;
    fanout_edges.reserve(branch_count);
    NodeId next_node_id = 2U;
    EdgeId next_edge_id = 1U;
    for (std::size_t index = 0; index < branch_count; ++index) {
        const NodeId prepare_node_id = next_node_id++;
        graph.nodes.push_back(NodeDefinition{
            prepare_node_id,
            NodeKind::Compute,
            "prepare_session_" + std::to_string(index + 1U),
            0U,
            0U,
            0U,
            prepare_parallel_session_node,
            {}
        });
        graph.edges.push_back(EdgeDefinition{
            next_edge_id,
            1U,
            prepare_node_id,
            EdgeKind::OnSuccess,
            nullptr,
            static_cast<uint16_t>(1000U - index)
        });
        fanout_edges.push_back(next_edge_id++);
    }

    const NodeId subgraph_node_id = next_node_id;
    graph.nodes.push_back(NodeDefinition{
        subgraph_node_id,
        NodeKind::Subgraph,
        "persistent_specialist",
        node_policy_mask(NodePolicyFlag::StopAfterNode),
        0U,
        0U,
        nullptr,
        {},
        {},
        {},
        SubgraphBinding{
            child_graph_id,
            "persistent_specialist",
            std::vector<SubgraphStateBinding>{
                SubgraphStateBinding{kSessionBenchmarkSessionId, kSessionBenchmarkChildSessionId},
                SubgraphStateBinding{kSessionBenchmarkInput, kSessionBenchmarkChildInput}
            },
            std::vector<SubgraphStateBinding>{
                SubgraphStateBinding{kSessionBenchmarkOutput, kSessionBenchmarkChildOutput},
                SubgraphStateBinding{kSessionBenchmarkVisits, kSessionBenchmarkChildVisits}
            },
            false,
            6U,
            SubgraphSessionMode::Persistent,
            kSessionBenchmarkSessionId
        }
    });

    for (NodeId prepare_node_id = 2U; prepare_node_id < subgraph_node_id; ++prepare_node_id) {
        graph.edges.push_back(EdgeDefinition{
            next_edge_id,
            prepare_node_id,
            subgraph_node_id,
            EdgeKind::OnSuccess,
            nullptr,
            100U
        });
        graph.bind_outgoing_edges(prepare_node_id, std::vector<EdgeId>{next_edge_id});
        ++next_edge_id;
    }

    graph.bind_outgoing_edges(1U, fanout_edges);
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_resumable_persistent_session_benchmark_parent_graph(GraphId child_graph_id) {
    GraphDefinition graph;
    graph.id = 981U;
    graph.name = "resumable_persistent_session_benchmark_parent";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Subgraph,
            "persistent_specialist",
            0U,
            0U,
            0U,
            nullptr,
            {},
            {},
            {},
            SubgraphBinding{
                child_graph_id,
                "persistent_specialist",
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kSessionBenchmarkSessionId, kSessionBenchmarkChildSessionId},
                    SubgraphStateBinding{kSessionBenchmarkInput, kSessionBenchmarkChildInput}
                },
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kSessionBenchmarkOutput, kSessionBenchmarkChildOutput},
                    SubgraphStateBinding{kSessionBenchmarkVisits, kSessionBenchmarkChildVisits},
                    SubgraphStateBinding{
                        kSessionBenchmarkResumeAttempt,
                        kSessionBenchmarkChildResumeAttempt
                    }
                },
                false,
                6U,
                SubgraphSessionMode::Persistent,
                kSessionBenchmarkSessionId
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Control,
            "revisit",
            0U,
            0U,
            0U,
            revisit_persistent_session_node,
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
    return graph;
}

PersistentSessionParallelBenchmarkRun run_parallel_persistent_session_benchmark_once(
    const GraphDefinition& parent_graph,
    const GraphDefinition& child_graph
) {
    ExecutionEngine engine(4U);
    engine.register_graph(child_graph);

    InputEnvelope input;
    input.initial_field_count = 6U;

    const auto started_at = std::chrono::steady_clock::now();
    const RunId run_id = engine.start(parent_graph, input);
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();
    assert(result.status == ExecutionStatus::Completed);

    const RunSnapshot snapshot = engine.inspect(run_id);
    assert(snapshot.committed_subgraph_sessions.size() == kPersistentSessionBenchmarkBranches);

    StreamCursor cursor;
    const auto stream_started_at = std::chrono::steady_clock::now();
    const std::vector<StreamEvent> stream_events =
        engine.stream_events(run_id, cursor, StreamReadOptions{true});
    const auto stream_ended_at = std::chrono::steady_clock::now();

    uint64_t namespaced_events = 0U;
    uint64_t session_tagged_events = 0U;
    std::set<std::string> distinct_sessions;
    for (const StreamEvent& event : stream_events) {
        if (!event.namespaces.empty()) {
            ++namespaced_events;
        }
        if (!event.session_id.empty()) {
            ++session_tagged_events;
            distinct_sessions.insert(event.session_id);
        }
    }
    assert(distinct_sessions.size() == kPersistentSessionBenchmarkBranches);

    const auto final_record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(final_record.has_value());
    assert(final_record->resumable());
    return PersistentSessionParallelBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                stream_ended_at - stream_started_at
            ).count()
        ),
        static_cast<uint64_t>(snapshot.committed_subgraph_sessions.size()),
        static_cast<uint64_t>(stream_events.size()),
        namespaced_events,
        session_tagged_events,
        compute_run_proof_digest(*final_record, engine.trace().events_for_run(run_id))
    };
}

PersistentSessionResumeBenchmarkRun run_resumable_persistent_session_benchmark_once(
    const GraphDefinition& parent_graph,
    const GraphDefinition& child_graph
) {
    InputEnvelope input;
    input.initial_field_count = 6U;
    input.initial_patch.updates.push_back(FieldUpdate{kSessionBenchmarkSessionId, int64_t{777}});
    input.initial_patch.updates.push_back(FieldUpdate{kSessionBenchmarkInput, int64_t{20}});
    input.initial_patch.updates.push_back(FieldUpdate{kSessionBenchmarkRound, int64_t{0}});

    g_persistent_session_benchmark_producer_calls.store(0U);
    ExecutionEngine direct_engine(1U);
    direct_engine.register_graph(child_graph);
    const RunId direct_run_id = direct_engine.start(parent_graph, input);
    const auto direct_started_at = std::chrono::steady_clock::now();
    const RunResult direct_wait = direct_engine.run_to_completion(direct_run_id);
    assert(direct_wait.status == ExecutionStatus::Paused);
    const ResumeResult direct_resume = direct_engine.resume_run(direct_run_id);
    assert(direct_resume.resumed);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    const auto direct_ended_at = std::chrono::steady_clock::now();
    assert(direct_result.status == ExecutionStatus::Completed);
    const uint64_t producer_calls_direct = g_persistent_session_benchmark_producer_calls.load();
    assert(producer_calls_direct == 1U);

    const auto direct_record = direct_engine.checkpoints().get(direct_result.last_checkpoint_id);
    assert(direct_record.has_value() && direct_record->resumable());
    const RunProofDigest direct_proof = compute_run_proof_digest(
        *direct_record,
        normalize_trace_for_resume_proof(direct_engine.trace().events_for_run(direct_run_id))
    );
    const int64_t direct_output = read_int_field(
        direct_engine.state(direct_run_id),
        kSessionBenchmarkOutput
    );

    const std::string checkpoint_path = "/tmp/agentcore_persistent_session_benchmark.bin";
    std::remove(checkpoint_path.c_str());

    RunId resumed_run_id = 0U;
    CheckpointId resumed_checkpoint_id = 0U;
    std::vector<TraceEvent> prefix_trace;

    g_persistent_session_benchmark_producer_calls.store(0U);
    const auto started_at = std::chrono::steady_clock::now();
    {
        ExecutionEngine first_engine(1U);
        first_engine.register_graph(child_graph);
        first_engine.enable_checkpoint_persistence(checkpoint_path);

        resumed_run_id = first_engine.start(parent_graph, input);
        const RunResult first_wait = first_engine.run_to_completion(resumed_run_id);
        assert(first_wait.status == ExecutionStatus::Paused);
        resumed_checkpoint_id = first_wait.last_checkpoint_id;
        prefix_trace = trace_events_through_checkpoint(
            first_engine.trace().events_for_run(resumed_run_id),
            resumed_checkpoint_id
        );
    }

    ExecutionEngine resumed_engine(1U);
    resumed_engine.register_graph(parent_graph);
    resumed_engine.register_graph(child_graph);
    resumed_engine.enable_checkpoint_persistence(checkpoint_path);
    assert(resumed_engine.load_persisted_checkpoints() >= 1U);

    const auto resume_started_at = std::chrono::steady_clock::now();
    const ResumeResult resumed = resumed_engine.resume(resumed_checkpoint_id);
    assert(resumed.resumed);
    const RunResult resumed_result = resumed_engine.run_to_completion(resumed_run_id);
    const auto resume_ended_at = std::chrono::steady_clock::now();
    assert(resumed_result.status == ExecutionStatus::Completed);
    const auto ended_at = resume_ended_at;
    const uint64_t producer_calls_resumed = g_persistent_session_benchmark_producer_calls.load();
    assert(producer_calls_resumed == 1U);

    const auto resumed_record = resumed_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value() && resumed_record->resumable());
    const RunProofDigest resumed_proof = compute_run_proof_digest(
        *resumed_record,
        normalize_trace_for_resume_proof(
            append_trace_events(prefix_trace, resumed_engine.trace().events_for_run(resumed_run_id))
        )
    );
    assert(resumed_proof.combined_digest == direct_proof.combined_digest);
    const int64_t resumed_output = read_int_field(
        resumed_engine.state(resumed_run_id),
        kSessionBenchmarkOutput
    );
    assert(resumed_output == direct_output);

    StreamCursor cursor;
    const std::vector<StreamEvent> stream_events =
        resumed_engine.stream_events(resumed_run_id, cursor, StreamReadOptions{true});
    uint64_t namespaced_events = 0U;
    uint64_t session_tagged_events = 0U;
    for (const StreamEvent& event : stream_events) {
        if (!event.namespaces.empty()) {
            ++namespaced_events;
        }
        if (!event.session_id.empty()) {
            ++session_tagged_events;
        }
    }

    const RunSnapshot snapshot = resumed_engine.inspect(resumed_run_id);
    assert(snapshot.committed_subgraph_sessions.size() == 1U);
    std::remove(checkpoint_path.c_str());

    return PersistentSessionResumeBenchmarkRun{
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                resume_ended_at - resume_started_at
            ).count()
        ),
        static_cast<uint64_t>(snapshot.committed_subgraph_sessions.size()),
        static_cast<uint64_t>(stream_events.size()),
        namespaced_events,
        session_tagged_events,
        producer_calls_direct,
        producer_calls_resumed,
        resumed_output,
        direct_proof,
        resumed_proof
    };
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    std::string error_message;
    GraphDefinition parallel_child_graph =
        make_persistent_session_benchmark_child_graph(982U, persistent_session_benchmark_child_node);
    assert(parallel_child_graph.validate(&error_message));
    GraphDefinition parallel_parent_graph =
        make_parallel_persistent_session_benchmark_parent_graph(
            parallel_child_graph.id,
            kPersistentSessionBenchmarkBranches
        );
    assert(parallel_parent_graph.validate(&error_message));

    GraphDefinition resumable_child_graph =
        make_persistent_session_benchmark_child_graph(983U, persistent_session_wait_child_node);
    assert(resumable_child_graph.validate(&error_message));
    GraphDefinition resumable_parent_graph =
        make_resumable_persistent_session_benchmark_parent_graph(resumable_child_graph.id);
    assert(resumable_parent_graph.validate(&error_message));

    const PersistentSessionParallelBenchmarkRun parallel =
        run_parallel_persistent_session_benchmark_once(parallel_parent_graph, parallel_child_graph);
    const PersistentSessionResumeBenchmarkRun resumed =
        run_resumable_persistent_session_benchmark_once(resumable_parent_graph, resumable_child_graph);

    std::cout << "agentcore_persistent_subgraph_session_benchmark" << '\n'
              << "persistent_session_parallel_branches=" << kPersistentSessionBenchmarkBranches << '\n'
              << "persistent_session_parallel_ns=" << parallel.elapsed_ns << '\n'
              << "persistent_session_parallel_stream_read_ns=" << parallel.stream_read_ns << '\n'
              << "persistent_session_parallel_committed_sessions=" << parallel.committed_sessions << '\n'
              << "persistent_session_parallel_stream_events=" << parallel.stream_events << '\n'
              << "persistent_session_parallel_namespaced_events=" << parallel.namespaced_events << '\n'
              << "persistent_session_parallel_session_tagged_events=" << parallel.session_tagged_events << '\n'
              << "persistent_session_parallel_digest=" << parallel.proof.combined_digest << '\n'
              << "persistent_session_resume_ns=" << resumed.elapsed_ns << '\n'
              << "persistent_session_resume_resume_ns=" << resumed.resume_ns << '\n'
              << "persistent_session_resume_committed_sessions=" << resumed.committed_sessions << '\n'
              << "persistent_session_resume_stream_events=" << resumed.stream_events << '\n'
              << "persistent_session_resume_namespaced_events=" << resumed.namespaced_events << '\n'
              << "persistent_session_resume_session_tagged_events=" << resumed.session_tagged_events << '\n'
              << "persistent_session_resume_producer_calls_direct=" << resumed.producer_calls_direct << '\n'
              << "persistent_session_resume_producer_calls_resumed=" << resumed.producer_calls_resumed << '\n'
              << "persistent_session_resume_output=" << resumed.final_output << '\n'
              << "persistent_session_resume_direct_digest=" << resumed.direct_proof.combined_digest << '\n'
              << "persistent_session_resume_resumed_digest=" << resumed.resumed_proof.combined_digest << '\n'
              << "verified=1" << std::endl;

    return 0;
}
