#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/graph/graph_ir.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace agentcore {

namespace {

enum ProofStateKey : StateKey {
    kJoinFieldA = 0,
    kJoinFieldB = 1,
    kJoinFieldC = 2,
    kJoinFieldD = 3,
    kJoinSummary = 4,
    kJoinAsyncStarted = 5,
    kJoinAsyncResult = 6,
    kJoinSyncValue = 7,
    kJoinAccumulator = 8,
    kJoinConsensus = 9
};

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult fanout_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult join_branch_a_node(ExecutionContext& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinFieldA, int64_t{1}});
    patch.updates.push_back(FieldUpdate{kJoinAccumulator, int64_t{1}});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("join_branch_a"),
        context.strings.intern("writes"),
        context.strings.intern("artifact_a"),
        context.blobs.append_string("a")
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult join_branch_b_node(ExecutionContext& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinFieldB, int64_t{2}});
    patch.updates.push_back(FieldUpdate{kJoinAccumulator, int64_t{2}});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("join_branch_b"),
        context.strings.intern("writes"),
        context.strings.intern("artifact_b"),
        context.blobs.append_string("b")
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult join_branch_c_node(ExecutionContext& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinFieldC, int64_t{3}});
    patch.updates.push_back(FieldUpdate{kJoinAccumulator, int64_t{3}});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("join_branch_c"),
        context.strings.intern("writes"),
        context.strings.intern("artifact_c"),
        context.blobs.append_string("c")
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

NodeResult join_branch_d_node(ExecutionContext& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinFieldD, int64_t{4}});
    patch.updates.push_back(FieldUpdate{kJoinAccumulator, int64_t{4}});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("join_branch_d"),
        context.strings.intern("writes"),
        context.strings.intern("artifact_d"),
        context.blobs.append_string("d")
    });
    return NodeResult::success(std::move(patch), 0.98F);
}

int64_t read_int_field(const WorkflowState& state, StateKey key) {
    if (state.fields.size() <= key || !std::holds_alternative<int64_t>(state.fields[key])) {
        return 0;
    }
    return std::get<int64_t>(state.fields[key]);
}

NodeResult summarize_join_node(ExecutionContext& context) {
    const int64_t total = read_int_field(context.state, kJoinAccumulator);

    const std::string summary = "sum=" + std::to_string(total) +
        " triples=" + std::to_string(context.knowledge_graph.triple_count());
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinSummary, context.blobs.append_string(summary)});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult sync_join_writer_node(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinSyncValue, int64_t{7}});
    patch.updates.push_back(FieldUpdate{kJoinConsensus, true});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("sync_branch"),
        context.strings.intern("joins"),
        context.strings.intern("knowledge_graph"),
        context.blobs.append_string("sync")
    });
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult async_join_writer_node(ExecutionContext& context) {
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
        patch.updates.push_back(FieldUpdate{kJoinAsyncResult, response->output});
        patch.updates.push_back(FieldUpdate{kJoinConsensus, true});
        patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
            context.strings.intern("async_branch"),
            context.strings.intern("joins"),
            context.strings.intern("knowledge_graph"),
            context.blobs.append_string("async")
        });
        return NodeResult::success(std::move(patch), 0.96F);
    }

    const BlobRef request_blob = context.blobs.append_string("join-async");
    const AsyncToolHandle handle = context.tools.begin_invoke_async(
        ToolRequest{context.strings.intern("async_echo"), request_blob},
        tool_context
    );
    assert(handle.valid());

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinAsyncStarted, true});
    return NodeResult::waiting_on_tool(handle, std::move(patch), 0.70F);
}

NodeResult summarize_async_join_node(ExecutionContext& context) {
    const int64_t sync_value = read_int_field(context.state, kJoinSyncValue);
    const bool consensus = context.state.fields.size() > kJoinConsensus &&
        std::holds_alternative<bool>(context.state.fields[kJoinConsensus]) &&
        std::get<bool>(context.state.fields[kJoinConsensus]);
    std::string async_value;
    if (context.state.fields.size() > kJoinAsyncResult &&
        std::holds_alternative<BlobRef>(context.state.fields[kJoinAsyncResult])) {
        async_value = std::string(
            context.blobs.read_string(std::get<BlobRef>(context.state.fields[kJoinAsyncResult]))
        );
    }

    const std::string summary = "sync=" + std::to_string(sync_value) +
        " async=" + async_value +
        " consensus=" + std::string(consensus ? "true" : "false") +
        " triples=" + std::to_string(context.knowledge_graph.triple_count());
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinSummary, context.blobs.append_string(summary)});
    return NodeResult::success(std::move(patch), 0.99F);
}

GraphDefinition make_parallel_join_graph() {
    GraphDefinition graph;
    graph.id = 101;
    graph.name = "proof_parallel_join";
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
        NodeDefinition{2, NodeKind::Compute, "parallel_a", 0U, 0U, 0U, join_branch_a_node, {}},
        NodeDefinition{3, NodeKind::Compute, "parallel_b", 0U, 0U, 0U, join_branch_b_node, {}},
        NodeDefinition{4, NodeKind::Compute, "parallel_c", 0U, 0U, 0U, join_branch_c_node, {}},
        NodeDefinition{5, NodeKind::Compute, "parallel_d", 0U, 0U, 0U, join_branch_d_node, {}},
        NodeDefinition{
            6,
            NodeKind::Aggregate,
            "join_summary",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_join_node,
            {},
            std::vector<FieldMergeRule>{
                FieldMergeRule{kJoinAccumulator, JoinMergeStrategy::SumInt64}
            }
        },
        NodeDefinition{7, NodeKind::Control, "stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
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
    return graph;
}

GraphDefinition make_async_join_graph() {
    GraphDefinition graph;
    graph.id = 102;
    graph.name = "proof_async_join_restart";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_async_join",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "sync_join_writer", 0U, 0U, 0U, sync_join_writer_node, {}},
        NodeDefinition{3, NodeKind::Tool, "async_join_writer", 0U, 0U, 0U, async_join_writer_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "async_join_summary",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_async_join_node,
            {},
            std::vector<FieldMergeRule>{
                FieldMergeRule{kJoinConsensus, JoinMergeStrategy::LogicalAnd}
            }
        },
        NodeDefinition{5, NodeKind::Control, "stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3, 2, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{4, 3, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{5, 4, 5, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{3});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{4});
    graph.bind_outgoing_edges(4, std::vector<EdgeId>{5});
    graph.sort_edges_by_priority();
    return graph;
}

RunProofDigest digest_for_result(const ExecutionEngine& engine, RunId run_id, CheckpointId checkpoint_id) {
    const auto record = engine.checkpoints().get(checkpoint_id);
    assert(record.has_value());
    return compute_run_proof_digest(*record, engine.trace().events_for_run(run_id));
}

std::vector<TraceEvent> append_trace_events(
    std::vector<TraceEvent> left,
    const std::vector<TraceEvent>& right
) {
    left.insert(left.end(), right.begin(), right.end());
    return left;
}

struct TimedRun {
    RunResult result{};
    uint64_t elapsed_ns{0};
    RunProofDigest digest{};
};

TimedRun run_parallel_proof(std::size_t worker_count) {
    ExecutionEngine engine(worker_count);
    InputEnvelope input;
    input.initial_field_count = 10;
    std::string validation_error;
    const GraphDefinition graph = make_parallel_join_graph();
    assert(graph.validate(&validation_error));

    const RunId run_id = engine.start(graph, input);
    const auto started_at = std::chrono::steady_clock::now();
    const RunResult result = engine.run_to_completion(run_id);
    const auto ended_at = std::chrono::steady_clock::now();

    return TimedRun{
        result,
        static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended_at - started_at).count()
        ),
        digest_for_result(engine, run_id, result.last_checkpoint_id)
    };
}

RunProofDigest run_async_restart_proof() {
    InputEnvelope input;
    input.initial_field_count = 10;
    std::string validation_error;
    const GraphDefinition graph = make_async_join_graph();
    assert(graph.validate(&validation_error));

    ExecutionEngine direct_engine(2);
    direct_engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        return ToolResponse{
            true,
            context.blobs.append_string(std::string(context.blobs.read_string(request.input))),
            kToolFlagNone
        };
    });
    const RunId direct_run_id = direct_engine.start(graph, input);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const RunProofDigest direct_digest =
        digest_for_result(direct_engine, direct_run_id, direct_result.last_checkpoint_id);

    const std::string checkpoint_path = "/tmp/agentcore_runtime_proof_suite.bin";
    std::remove(checkpoint_path.c_str());

    RunId resumed_run_id = 0U;
    CheckpointId resumed_checkpoint_id = 0U;
    std::vector<TraceEvent> restart_trace;
    {
        ExecutionEngine first_engine(2);
        first_engine.enable_checkpoint_persistence(checkpoint_path);
        first_engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
            return ToolResponse{
                true,
                context.blobs.append_string(std::string(context.blobs.read_string(request.input))),
                kToolFlagNone
            };
        });

        resumed_run_id = first_engine.start(graph, input);
        StepResult first_step{};
        do {
            first_step = first_engine.step(resumed_run_id);
        } while (first_step.progressed && first_step.node_status != NodeResult::Waiting);
        assert(first_step.node_status == NodeResult::Waiting);
        resumed_checkpoint_id = first_step.checkpoint_id;
        assert(resumed_checkpoint_id != 0U);
        restart_trace = first_engine.trace().events_for_run(resumed_run_id);
    }

    ExecutionEngine resumed_engine(2);
    resumed_engine.register_graph(graph);
    resumed_engine.enable_checkpoint_persistence(checkpoint_path);
    resumed_engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        return ToolResponse{
            true,
            context.blobs.append_string(std::string(context.blobs.read_string(request.input))),
            kToolFlagNone
        };
    });
    assert(resumed_engine.load_persisted_checkpoints() >= 1U);

    const ResumeResult resume_result = resumed_engine.resume(resumed_checkpoint_id);
    assert(resume_result.resumed);
    const RunResult resumed_result = resumed_engine.run_to_completion(resumed_run_id);
    assert(resumed_result.status == ExecutionStatus::Completed);
    const auto resumed_record = resumed_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    const RunProofDigest resumed_digest = compute_run_proof_digest(
        *resumed_record,
        append_trace_events(restart_trace, resumed_engine.trace().events_for_run(resumed_run_id))
    );

    assert(direct_digest.snapshot_digest == resumed_digest.snapshot_digest);
    assert(direct_digest.trace_digest == resumed_digest.trace_digest);
    assert(direct_digest.combined_digest == resumed_digest.combined_digest);

    std::remove(checkpoint_path.c_str());
    return direct_digest;
}

} // namespace

} // namespace agentcore

int main() {
    using namespace agentcore;

    const TimedRun sequential = run_parallel_proof(1);
    const TimedRun parallel = run_parallel_proof(4);

    assert(sequential.result.status == ExecutionStatus::Completed);
    assert(parallel.result.status == ExecutionStatus::Completed);
    assert(sequential.digest.snapshot_digest == parallel.digest.snapshot_digest);
    assert(sequential.digest.trace_digest == parallel.digest.trace_digest);
    assert(sequential.digest.combined_digest == parallel.digest.combined_digest);
    assert(parallel.elapsed_ns < sequential.elapsed_ns);

    const double speedup = static_cast<double>(sequential.elapsed_ns) /
        static_cast<double>(parallel.elapsed_ns == 0U ? 1U : parallel.elapsed_ns);
    assert(speedup > 1.5);

    const RunProofDigest restart_digest = run_async_restart_proof();

    std::cout << "runtime_proof_suite\n";
    std::cout << "structural_validation=1\n";
    std::cout << "merge_policy_validation=1\n";
    std::cout << "join_parallel_deterministic=1\n";
    std::cout << "join_restart_equivalent=1\n";
    std::cout << "knowledge_graph_merge=1\n";
    std::cout << "sequential_ns=" << sequential.elapsed_ns << "\n";
    std::cout << "parallel_ns=" << parallel.elapsed_ns << "\n";
    std::cout << "speedup_x=" << speedup << "\n";
    std::cout << "deterministic_digest=" << sequential.digest.combined_digest << "\n";
    std::cout << "restart_digest=" << restart_digest.combined_digest << "\n";
    return 0;
}
