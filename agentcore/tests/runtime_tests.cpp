#include "agentcore/adapters/adapter_factories.h"
#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/graph/graph_ir.h"

#include <atomic>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace agentcore {

namespace {

bool verbose_runtime_test_logging_enabled() {
    static const bool enabled = std::getenv("AGENTCORE_RUNTIME_TEST_VERBOSE") != nullptr;
    return enabled;
}

void verbose_runtime_test_log(std::string_view message) {
    if (verbose_runtime_test_logging_enabled()) {
        std::cout << "[runtime_test_verbose] " << message << '\n';
    }
}

void verbose_log_snapshot_summary(std::string_view label, const RunSnapshot& snapshot) {
    if (!verbose_runtime_test_logging_enabled()) {
        return;
    }

    verbose_runtime_test_log(
        std::string(label) +
        " status=" + std::to_string(static_cast<uint32_t>(snapshot.status)) +
        " branches=" + std::to_string(snapshot.branches.size()) +
        " pending_tasks=" + std::to_string(snapshot.pending_tasks.size()) +
        " next_branch_id=" + std::to_string(snapshot.next_branch_id) +
        " next_split_id=" + std::to_string(snapshot.next_split_id)
    );
    for (const BranchSnapshot& branch : snapshot.branches) {
        verbose_runtime_test_log(
            std::string(label) +
            " branch=" + std::to_string(branch.frame.active_branch_id) +
            " node=" + std::to_string(branch.frame.current_node) +
            " step=" + std::to_string(branch.frame.step_index) +
            " checkpoint=" + std::to_string(branch.frame.checkpoint_id) +
            " status=" + std::to_string(static_cast<uint32_t>(branch.frame.status)) +
            " pending_async=" + std::to_string(branch.pending_async.has_value() ? 1 : 0) +
            " pending_group=" + std::to_string(branch.pending_async_group.size()) +
            " pending_subgraph=" + std::to_string(branch.pending_subgraph.has_value() ? 1 : 0)
        );
    }
}

enum ResumeStateKey : StateKey {
    kResumeAttempt = 0,
    kResumeDone = 1
};

enum CheckpointCadenceStateKey : StateKey {
    kCadenceIteration = 0,
    kCadenceRouteKey = 1,
    kCadenceAccumulator = 2,
    kCadenceShouldStop = 3
};

constexpr int kCadenceRouteCount = 8;
constexpr int64_t kCadenceIterations = 24;

enum RecordedEffectStateKey : StateKey {
    kRecordedEffectOutput = 0,
    kRecordedEffectVisit = 1,
    kRecordedEffectDone = 2
};

std::atomic<int> g_recorded_effect_invocations{0};

int64_t read_int_field(const WorkflowState& state, StateKey key);
bool read_bool_field(const WorkflowState& state, StateKey key);

void configure_checkpoint_storage(ExecutionEngine& engine, const std::string& path, bool use_sqlite) {
#if defined(AGENTCORE_HAVE_SQLITE3)
    if (use_sqlite) {
        engine.enable_sqlite_checkpoint_persistence(path);
        return;
    }
#else
    (void)use_sqlite;
#endif
    engine.enable_checkpoint_persistence(path);
}

NodeResult wait_then_continue_node(ExecutionContext& context) {
    const Value attempts_val = context.state.load(kResumeAttempt);
    const Value* attempts_value = std::holds_alternative<std::monostate>(attempts_val) ? nullptr : &attempts_val;
    const bool first_visit = attempts_value == nullptr ||
        !std::holds_alternative<int64_t>(*attempts_value) ||
        std::get<int64_t>(*attempts_value) == 0;

    StatePatch patch;
    if (first_visit) {
        patch.updates.push_back(FieldUpdate{kResumeAttempt, int64_t{1}});
        return NodeResult::waiting(std::move(patch), 1.0F);
    }

    patch.updates.push_back(FieldUpdate{kResumeDone, true});
    return NodeResult::success(std::move(patch), 1.0F);
}

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult recorded_effect_wait_node(ExecutionContext& context) {
    const int64_t visit = read_int_field(context.state, kRecordedEffectVisit);
    const RecordedEffectResult effect = context.record_text_effect_once(
        "sync-effect::write-once",
        "request::write-once",
        []() {
            g_recorded_effect_invocations.fetch_add(1);
            return std::string("outcome::write-once");
        }
    );

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kRecordedEffectOutput, effect.output});
    patch.updates.push_back(FieldUpdate{kRecordedEffectVisit, visit + 1});
    if (visit == 0) {
        return NodeResult::waiting(std::move(patch), 0.93F);
    }

    patch.updates.push_back(FieldUpdate{kRecordedEffectDone, true});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult recorded_effect_request_mismatch_node(ExecutionContext& context) {
    const int64_t visit = read_int_field(context.state, kRecordedEffectVisit);
    const std::string request = visit == 0 ? "request::first" : "request::second";
    const RecordedEffectResult effect = context.record_text_effect_once(
        "sync-effect::mismatch",
        request,
        []() {
            g_recorded_effect_invocations.fetch_add(1);
            return std::string("outcome::mismatch");
        }
    );

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kRecordedEffectOutput, effect.output});
    patch.updates.push_back(FieldUpdate{kRecordedEffectVisit, visit + 1});
    if (visit == 0) {
        return NodeResult::waiting(std::move(patch), 0.90F);
    }

    patch.updates.push_back(FieldUpdate{kRecordedEffectDone, true});
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult cadence_router_node(ExecutionContext& context) {
    const int64_t iteration = read_int_field(context.state, kCadenceIteration);

    StatePatch patch;
    if (iteration >= kCadenceIterations) {
        patch.updates.push_back(FieldUpdate{kCadenceShouldStop, true});
        return NodeResult::success(std::move(patch), 1.0F);
    }

    const int64_t next_iteration = iteration + 1;
    patch.updates.push_back(FieldUpdate{kCadenceIteration, next_iteration});
    patch.updates.push_back(FieldUpdate{kCadenceRouteKey, next_iteration % kCadenceRouteCount});
    patch.updates.push_back(FieldUpdate{kCadenceShouldStop, false});
    return NodeResult::success(std::move(patch), 0.98F);
}

enum KnowledgeGraphTestStateKey : StateKey {
    kKnowledgeGraphSummary = 0
};

enum ReactiveKnowledgeStateKey : StateKey {
    kReactivePrimarySeen = 0,
    kReactiveOtherSeen = 1
};

enum ReactiveIntelligenceStateKey : StateKey {
    kReactiveSupportedClaimCount = 0,
    kReactiveRejectedSeen = 1
};

enum AsyncStateKey : StateKey {
    kAsyncResult = 0,
    kAsyncStarted = 1
};

enum SubgraphStateKey : StateKey {
    kSubgraphInput = 0,
    kSubgraphOutput = 1,
    kSubgraphSummary = 2
};

enum SubgraphChildStateKey : StateKey {
    kSubgraphChildInput = 0,
    kSubgraphChildOutput = 1
};

enum ResumableSubgraphStateKey : StateKey {
    kResumableSubgraphInput = 0,
    kResumableSubgraphOutput = 1,
    kResumableSubgraphSummary = 2
};

enum ResumableSubgraphChildStateKey : StateKey {
    kResumableSubgraphChildInput = 0,
    kResumableSubgraphChildOutput = 1,
    kResumableSubgraphChildAttempt = 2
};

enum AsyncSubgraphStateKey : StateKey {
    kAsyncSubgraphResult = 0,
    kAsyncSubgraphSummary = 1
};

enum AsyncSubgraphMultiWaitStateKey : StateKey {
    kAsyncSubgraphMultiWaitResult = 0,
    kAsyncSubgraphMultiWaitSummary = 1
};

enum AsyncSubgraphMultiWaitChildStateKey : StateKey {
    kAsyncSubgraphMultiWaitChildLeft = 0,
    kAsyncSubgraphMultiWaitChildRight = 1,
    kAsyncSubgraphMultiWaitChildSummary = 2
};

enum JoinProofStateKey : StateKey {
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

std::atomic<int> g_active_parallel_nodes{0};
std::atomic<int> g_max_parallel_nodes{0};

NodeResult ingest_kg_node(ExecutionContext& context) {
    const InternedStringId runtime = context.strings.intern("agentcore");
    const InternedStringId reference_runtime = context.strings.intern("reference_runtime");
    const InternedStringId relation = context.strings.intern("faster_than");

    StatePatch patch;
    patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
        runtime,
        context.blobs.append_string("native graph runtime")
    });
    patch.knowledge_graph.entities.push_back(KnowledgeEntityWrite{
        reference_runtime,
        context.blobs.append_string("python graph runtime")
    });
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        runtime,
        relation,
        reference_runtime,
        context.blobs.append_string("native execution")
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult summarize_kg_node(ExecutionContext& context) {
    const InternedStringId runtime = context.strings.intern("agentcore");
    const InternedStringId relation = context.strings.intern("faster_than");
    const std::vector<const KnowledgeTriple*> matches =
        context.knowledge_graph.match(runtime, relation, std::nullopt);
    assert(matches.size() == 1U);

    const KnowledgeEntity* object = context.knowledge_graph.find_entity(matches.front()->object);
    assert(object != nullptr);

    std::string summary = "agentcore faster_than ";
    summary += std::string(context.strings.resolve(object->label));

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kKnowledgeGraphSummary, context.blobs.append_string(summary)});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult emit_reactive_kg_node(ExecutionContext& context) {
    StatePatch patch;
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("agentcore"),
        context.strings.intern("primary_signal"),
        context.strings.intern("artifact"),
        context.blobs.append_string("delta")
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult reactive_primary_node(ExecutionContext&) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kReactivePrimarySeen, true});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult reactive_other_node(ExecutionContext&) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kReactiveOtherSeen, true});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult emit_supported_claim_node(
    ExecutionContext& context,
    std::string_view claim_key,
    std::string_view statement
) {
    StatePatch patch;
    patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        context.strings.intern(claim_key),
        context.strings.intern("agentcore"),
        context.strings.intern("supports"),
        context.strings.intern("reactive_intelligence"),
        context.blobs.append_string(std::string(statement)),
        IntelligenceClaimStatus::Supported,
        0.92F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult emit_supported_claim_primary_node(ExecutionContext& context) {
    return emit_supported_claim_node(context, "claim:primary", "primary-signal");
}

NodeResult emit_supported_claim_alpha_node(ExecutionContext& context) {
    return emit_supported_claim_node(context, "claim:alpha", "alpha-signal");
}

NodeResult emit_supported_claim_beta_node(ExecutionContext& context) {
    return emit_supported_claim_node(context, "claim:beta", "beta-signal");
}

NodeResult emit_rejected_claim_node(ExecutionContext& context) {
    StatePatch patch;
    patch.intelligence.claims.push_back(IntelligenceClaimWrite{
        context.strings.intern("claim:rejected"),
        context.strings.intern("agentcore"),
        context.strings.intern("supports"),
        context.strings.intern("reactive_intelligence"),
        context.blobs.append_string("rejected-signal"),
        IntelligenceClaimStatus::Rejected,
        0.81F,
        intelligence_fields::kClaimSubject |
            intelligence_fields::kClaimRelation |
            intelligence_fields::kClaimObject |
            intelligence_fields::kClaimStatement |
            intelligence_fields::kClaimStatus |
            intelligence_fields::kClaimConfidence
    });
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult reactive_supported_claim_node(ExecutionContext& context) {
    const int64_t seen = read_int_field(context.state, kReactiveSupportedClaimCount);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kReactiveSupportedClaimCount, seen + 1});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult reactive_rejected_claim_node(ExecutionContext&) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kReactiveRejectedSeen, true});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult emit_primary_signal_node(ExecutionContext& context, std::string_view object_label) {
    StatePatch patch;
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("agentcore"),
        context.strings.intern("primary_signal"),
        context.strings.intern(object_label),
        {}
    });
    return NodeResult::success(std::move(patch), 0.96F);
}

NodeResult emit_duplicate_primary_signal_node(ExecutionContext& context) {
    return emit_primary_signal_node(context, "artifact");
}

NodeResult emit_primary_signal_alpha_node(ExecutionContext& context) {
    return emit_primary_signal_node(context, "artifact_alpha");
}

NodeResult emit_primary_signal_beta_node(ExecutionContext& context) {
    return emit_primary_signal_node(context, "artifact_beta");
}

NodeResult emit_primary_signal_gamma_node(ExecutionContext& context) {
    return emit_primary_signal_node(context, "artifact_gamma");
}

NodeResult fanout_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult subgraph_child_compute_node(ExecutionContext& context) {
    const int64_t input_value = read_int_field(context.state, kSubgraphChildInput);

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kSubgraphChildOutput, input_value + 41});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("subgraph"),
        context.strings.intern("computed"),
        context.strings.intern("artifact"),
        context.blobs.append_string("child")
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult summarize_subgraph_parent_node(ExecutionContext& context) {
    const int64_t output_value = read_int_field(context.state, kSubgraphOutput);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kSubgraphSummary,
        context.blobs.append_string("subgraph-output=" + std::to_string(output_value))
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult resumable_subgraph_child_node(ExecutionContext& context) {
    const int64_t attempt = read_int_field(context.state, kResumableSubgraphChildAttempt);

    StatePatch patch;
    if (attempt == 0) {
        patch.updates.push_back(FieldUpdate{kResumableSubgraphChildAttempt, int64_t{1}});
        return NodeResult::waiting(std::move(patch), 0.91F);
    }

    const int64_t input_value = read_int_field(context.state, kResumableSubgraphChildInput);
    patch.updates.push_back(FieldUpdate{
        kResumableSubgraphChildOutput,
        input_value + 99
    });
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern("resumable_subgraph"),
        context.strings.intern("completed"),
        context.strings.intern("artifact"),
        context.blobs.append_string("nested-resume")
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult summarize_resumable_subgraph_parent_node(ExecutionContext& context) {
    const int64_t output_value = read_int_field(context.state, kResumableSubgraphOutput);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kResumableSubgraphSummary,
        context.blobs.append_string("resumable-subgraph-output=" + std::to_string(output_value))
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult summarize_async_subgraph_parent_node(ExecutionContext& context) {
    std::string async_value;
    if (context.state.size() > kAsyncSubgraphResult &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncSubgraphResult))) {
        async_value = std::string(
            context.blobs.read_string(std::get<BlobRef>(context.state.load(kAsyncSubgraphResult)))
        );
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kAsyncSubgraphSummary,
        context.blobs.append_string("async-subgraph=" + async_value)
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult async_multi_wait_child_writer_node(
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
            context.strings.intern("completed"),
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

NodeResult async_multi_wait_left_node(ExecutionContext& context) {
    return async_multi_wait_child_writer_node(
        context,
        kAsyncSubgraphMultiWaitChildLeft,
        "join-left",
        "async_multi_wait_left"
    );
}

NodeResult async_multi_wait_right_node(ExecutionContext& context) {
    return async_multi_wait_child_writer_node(
        context,
        kAsyncSubgraphMultiWaitChildRight,
        "join-right",
        "async_multi_wait_right"
    );
}

NodeResult summarize_multi_wait_child_node(ExecutionContext& context) {
    std::string left_value;
    if (context.state.size() > kAsyncSubgraphMultiWaitChildLeft &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncSubgraphMultiWaitChildLeft))) {
        left_value = std::string(
            context.blobs.read_string(
                std::get<BlobRef>(context.state.load(kAsyncSubgraphMultiWaitChildLeft))
            )
        );
    }

    std::string right_value;
    if (context.state.size() > kAsyncSubgraphMultiWaitChildRight &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncSubgraphMultiWaitChildRight))) {
        right_value = std::string(
            context.blobs.read_string(
                std::get<BlobRef>(context.state.load(kAsyncSubgraphMultiWaitChildRight))
            )
        );
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kAsyncSubgraphMultiWaitChildSummary,
        context.blobs.append_string("left=" + left_value + " right=" + right_value)
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult summarize_multi_wait_parent_node(ExecutionContext& context) {
    std::string child_summary;
    if (context.state.size() > kAsyncSubgraphMultiWaitResult &&
        std::holds_alternative<BlobRef>(context.state.load(kAsyncSubgraphMultiWaitResult))) {
        child_summary = std::string(
            context.blobs.read_string(std::get<BlobRef>(context.state.load(kAsyncSubgraphMultiWaitResult)))
        );
    }

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kAsyncSubgraphMultiWaitSummary,
        context.blobs.append_string("multi-wait-subgraph=" + child_summary)
    });
    return NodeResult::success(std::move(patch), 0.99F);
}

NodeResult parallel_branch_node(ExecutionContext&) {
    const int active = g_active_parallel_nodes.fetch_add(1) + 1;
    int observed_max = g_max_parallel_nodes.load();
    while (active > observed_max &&
           !g_max_parallel_nodes.compare_exchange_weak(observed_max, active)) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    g_active_parallel_nodes.fetch_sub(1);
    return NodeResult::success();
}

NodeResult parallel_trace_left_node(ExecutionContext& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    context.trace.emit(TraceEvent{
        0U,
        0U,
        0U,
        context.run_id,
        context.graph_id,
        context.node_id,
        context.branch_id,
        0U,
        NodeResult::Success,
        0.91F,
        0U,
        1U << 18U,
        {},
        0U,
        {}
    });
    return NodeResult::success({}, 0.91F);
}

NodeResult parallel_trace_right_node(ExecutionContext& context) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    context.trace.emit(TraceEvent{
        0U,
        0U,
        0U,
        context.run_id,
        context.graph_id,
        context.node_id,
        context.branch_id,
        0U,
        NodeResult::Success,
        0.92F,
        0U,
        1U << 17U,
        {},
        0U,
        {}
    });
    return NodeResult::success({}, 0.92F);
}

NodeResult async_tool_roundtrip_node(ExecutionContext& context) {
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
        patch.updates.push_back(FieldUpdate{kAsyncResult, response->output});
        return NodeResult::success(std::move(patch), 0.96F);
    }

    const BlobRef request_blob = context.blobs.append_string("async-fast-path");
    const AsyncToolHandle handle = context.tools.begin_invoke_async(
        ToolRequest{context.strings.intern("async_echo"), request_blob},
        tool_context
    );
    assert(handle.valid());

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kAsyncStarted, true});
    return NodeResult::waiting_on_tool(handle, std::move(patch), 0.70F);
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
    if (state.size() <= key || !std::holds_alternative<int64_t>(state.load(key))) {
        return 0;
    }
    return std::get<int64_t>(state.load(key));
}

bool read_bool_field(const WorkflowState& state, StateKey key) {
    return state.size() > key &&
        std::holds_alternative<bool>(state.load(key)) &&
        std::get<bool>(state.load(key));
}

template <int RouteIndex>
bool cadence_route_selected(const WorkflowState& state) {
    return read_int_field(state, kCadenceRouteKey) == RouteIndex;
}

bool cadence_should_stop(const WorkflowState& state) {
    return read_bool_field(state, kCadenceShouldStop);
}

template <int RouteIndex>
NodeResult cadence_branch_node(ExecutionContext& context) {
    const int64_t accumulator = read_int_field(context.state, kCadenceAccumulator);

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{
        kCadenceAccumulator,
        accumulator + static_cast<int64_t>(RouteIndex + 1)
    });
    return NodeResult::success(std::move(patch), 0.94F);
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
    const bool consensus = context.state.size() > kJoinConsensus &&
        std::holds_alternative<bool>(context.state.load(kJoinConsensus)) &&
        std::get<bool>(context.state.load(kJoinConsensus));
    std::string async_value;
    if (context.state.size() > kJoinAsyncResult &&
        std::holds_alternative<BlobRef>(context.state.load(kJoinAsyncResult))) {
        async_value = std::string(
            context.blobs.read_string(std::get<BlobRef>(context.state.load(kJoinAsyncResult)))
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

NodeResult conflicting_join_left_node(ExecutionContext&) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinAccumulator, int64_t{11}});
    return NodeResult::success(std::move(patch), 0.90F);
}

NodeResult conflicting_join_right_node(ExecutionContext&) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kJoinAccumulator, int64_t{29}});
    return NodeResult::success(std::move(patch), 0.90F);
}

GraphDefinition make_resume_graph() {
    GraphDefinition graph;
    graph.id = 10;
    graph.name = "resume_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Human, "wait_then_continue", 0U, 0U, 0U, wait_then_continue_node, {}},
        NodeDefinition{2, NodeKind::Control, "stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_recorded_effect_graph(NodeExecutorFn executor = recorded_effect_wait_node) {
    GraphDefinition graph;
    graph.id = 13U;
    graph.name = "recorded_effect_graph";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{1U, NodeKind::Human, "record_effect", 0U, 0U, 0U, executor, {}}
    };
    graph.sort_edges_by_priority();
    return graph;
}

#define AGENTCORE_CADENCE_ROUTE_CASE(N) \
    case N: \
        return cadence_route_selected<N>;

ConditionFn cadence_condition_for_index(int route_index) {
    switch (route_index) {
        AGENTCORE_CADENCE_ROUTE_CASE(0)
        AGENTCORE_CADENCE_ROUTE_CASE(1)
        AGENTCORE_CADENCE_ROUTE_CASE(2)
        AGENTCORE_CADENCE_ROUTE_CASE(3)
        AGENTCORE_CADENCE_ROUTE_CASE(4)
        AGENTCORE_CADENCE_ROUTE_CASE(5)
        AGENTCORE_CADENCE_ROUTE_CASE(6)
        AGENTCORE_CADENCE_ROUTE_CASE(7)
        default:
            return nullptr;
    }
}

#undef AGENTCORE_CADENCE_ROUTE_CASE

#define AGENTCORE_CADENCE_BRANCH_CASE(N) \
    case N: \
        return cadence_branch_node<N>;

NodeExecutorFn cadence_executor_for_index(int route_index) {
    switch (route_index) {
        AGENTCORE_CADENCE_BRANCH_CASE(0)
        AGENTCORE_CADENCE_BRANCH_CASE(1)
        AGENTCORE_CADENCE_BRANCH_CASE(2)
        AGENTCORE_CADENCE_BRANCH_CASE(3)
        AGENTCORE_CADENCE_BRANCH_CASE(4)
        AGENTCORE_CADENCE_BRANCH_CASE(5)
        AGENTCORE_CADENCE_BRANCH_CASE(6)
        AGENTCORE_CADENCE_BRANCH_CASE(7)
        default:
            return nullptr;
    }
}

#undef AGENTCORE_CADENCE_BRANCH_CASE

GraphDefinition make_checkpoint_cadence_graph() {
    GraphDefinition graph;
    graph.id = 22;
    graph.name = "checkpoint_cadence_test";
    graph.entry = 1;

    graph.nodes.push_back(NodeDefinition{
        1,
        NodeKind::Control,
        "cadence_router",
        0U,
        0U,
        0U,
        cadence_router_node,
        {}
    });

    for (int route_index = 0; route_index < kCadenceRouteCount; ++route_index) {
        graph.nodes.push_back(NodeDefinition{
            static_cast<NodeId>(2 + route_index),
            NodeKind::Compute,
            "cadence_route_" + std::to_string(route_index),
            0U,
            0U,
            0U,
            cadence_executor_for_index(route_index),
            {}
        });
    }

    const NodeId stop_node_id = static_cast<NodeId>(2 + kCadenceRouteCount);
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
        cadence_should_stop,
        1000U
    });

    for (int route_index = 0; route_index < kCadenceRouteCount; ++route_index) {
        router_edges.push_back(next_edge_id);
        graph.edges.push_back(EdgeDefinition{
            next_edge_id++,
            1,
            static_cast<NodeId>(2 + route_index),
            EdgeKind::Conditional,
            cadence_condition_for_index(route_index),
            static_cast<uint16_t>(900U - route_index)
        });
    }

    graph.bind_outgoing_edges(1, router_edges);
    for (int route_index = 0; route_index < kCadenceRouteCount; ++route_index) {
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
    return graph;
}

int64_t expected_cadence_accumulator() {
    int64_t total = 0;
    for (int64_t iteration = 1; iteration <= kCadenceIterations; ++iteration) {
        total += (iteration % kCadenceRouteCount) + 1;
    }
    return total;
}

GraphDefinition make_knowledge_graph_test_graph() {
    GraphDefinition graph;
    graph.id = 11;
    graph.name = "knowledge_graph_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "ingest_kg", 0U, 0U, 0U, ingest_kg_node, {}},
        NodeDefinition{2, NodeKind::Aggregate, "summarize_kg", 0U, 0U, 0U, summarize_kg_node, {}},
        NodeDefinition{3, NodeKind::Control, "stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 2, 3, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{2});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_subgraph_child_graph() {
    GraphDefinition graph;
    graph.id = 901U;
    graph.name = "subgraph_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Compute,
            "child_compute",
            0U,
            0U,
            0U,
            subgraph_child_compute_node,
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
    return graph;
}

GraphDefinition make_subgraph_parent_graph() {
    GraphDefinition graph;
    graph.id = 902U;
    graph.name = "subgraph_parent";
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
                901U,
                "planner_subgraph",
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kSubgraphInput, kSubgraphChildInput}
                },
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kSubgraphOutput, kSubgraphChildOutput}
                },
                true,
                2U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_parent",
            0U,
            0U,
            0U,
            summarize_subgraph_parent_node,
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

GraphDefinition make_resumable_subgraph_child_graph() {
    GraphDefinition graph;
    graph.id = 903U;
    graph.name = "resumable_subgraph_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Human,
            "child_wait_then_compute",
            0U,
            0U,
            0U,
            resumable_subgraph_child_node,
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
    return graph;
}

GraphDefinition make_resumable_subgraph_parent_graph() {
    GraphDefinition graph;
    graph.id = 904U;
    graph.name = "resumable_subgraph_parent";
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
                903U,
                "resumable_planner_subgraph",
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kResumableSubgraphInput, kResumableSubgraphChildInput}
                },
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kResumableSubgraphOutput, kResumableSubgraphChildOutput}
                },
                true,
                3U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_resumable_parent",
            0U,
            0U,
            0U,
            summarize_resumable_subgraph_parent_node,
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

GraphDefinition make_async_subgraph_child_graph() {
    GraphDefinition graph;
    graph.id = 905U;
    graph.name = "async_subgraph_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Tool,
            "child_async_tool",
            0U,
            0U,
            0U,
            async_tool_roundtrip_node,
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
    return graph;
}

GraphDefinition make_async_subgraph_parent_graph() {
    GraphDefinition graph;
    graph.id = 906U;
    graph.name = "async_subgraph_parent";
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
                905U,
                "async_planner_subgraph",
                {},
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kAsyncSubgraphResult, kAsyncResult}
                },
                false,
                2U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_async_parent",
            0U,
            0U,
            0U,
            summarize_async_subgraph_parent_node,
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

GraphDefinition make_async_multi_wait_subgraph_child_graph() {
    GraphDefinition graph;
    graph.id = 907U;
    graph.name = "async_multi_wait_subgraph_child";
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
            async_multi_wait_left_node,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Tool,
            "child_async_right",
            0U,
            0U,
            0U,
            async_multi_wait_right_node,
            {}
        },
        NodeDefinition{
            4U,
            NodeKind::Aggregate,
            "child_async_join",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_multi_wait_child_node,
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
    return graph;
}

GraphDefinition make_async_multi_wait_subgraph_parent_graph() {
    GraphDefinition graph;
    graph.id = 908U;
    graph.name = "async_multi_wait_subgraph_parent";
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
                907U,
                "async_multi_wait_subgraph",
                {},
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{
                        kAsyncSubgraphMultiWaitResult,
                        kAsyncSubgraphMultiWaitChildSummary
                    }
                },
                false,
                3U
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Aggregate,
            "summarize_multi_wait_parent",
            0U,
            0U,
            0U,
            summarize_multi_wait_parent_node,
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

GraphDefinition make_knowledge_reactive_graph() {
    GraphDefinition graph;
    graph.id = 20;
    graph.name = "knowledge_reactive_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "emit_reactive_kg", 0U, 0U, 0U, emit_reactive_kg_node, {}},
        NodeDefinition{2, NodeKind::Control, "root_stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}},
        NodeDefinition{
            3,
            NodeKind::Aggregate,
            "react_primary",
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph),
            0U,
            0U,
            reactive_primary_node,
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    "primary_signal",
                    {}
                }
            }
        },
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "react_other",
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph),
            0U,
            0U,
            reactive_other_node,
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    "other_signal",
                    {}
                }
            }
        },
        NodeDefinition{5, NodeKind::Control, "reactive_stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 3, 5, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{3, 4, 5, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{2});
    graph.bind_outgoing_edges(4, std::vector<EdgeId>{3});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_knowledge_duplicate_reactive_graph() {
    GraphDefinition graph;
    graph.id = 30;
    graph.name = "knowledge_duplicate_reactive_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "emit_primary_once", 0U, 0U, 0U, emit_duplicate_primary_signal_node, {}},
        NodeDefinition{2, NodeKind::Compute, "emit_primary_once", 0U, 0U, 0U, emit_duplicate_primary_signal_node, {}},
        NodeDefinition{3, NodeKind::Compute, "emit_primary_duplicate", 0U, 0U, 0U, emit_duplicate_primary_signal_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "react_primary_once",
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph) |
                node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            reactive_primary_node,
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    "primary_signal",
                    {}
                }
            }
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 2, 3, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{2});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_knowledge_reactive_draining_graph() {
    GraphDefinition graph;
    graph.id = 31;
    graph.name = "knowledge_reactive_draining_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_reactive_signals",
            node_policy_mask(NodePolicyFlag::AllowFanOut),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "emit_primary_alpha", 0U, 0U, 0U, emit_primary_signal_alpha_node, {}},
        NodeDefinition{3, NodeKind::Compute, "emit_primary_beta", 0U, 0U, 0U, emit_primary_signal_beta_node, {}},
        NodeDefinition{4, NodeKind::Compute, "emit_primary_gamma", 0U, 0U, 0U, emit_primary_signal_gamma_node, {}},
        NodeDefinition{
            5,
            NodeKind::Aggregate,
            "react_primary_drain",
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph) |
                node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            reactive_primary_node,
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    "primary_signal",
                    {}
                }
            }
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3, 1, 4, EdgeKind::OnSuccess, nullptr, 80U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2, 3});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_knowledge_reactive_resume_graph() {
    GraphDefinition graph;
    graph.id = 32;
    graph.name = "knowledge_reactive_resume_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_resume_signals",
            node_policy_mask(NodePolicyFlag::AllowFanOut),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "emit_primary_alpha", 0U, 0U, 0U, emit_primary_signal_alpha_node, {}},
        NodeDefinition{3, NodeKind::Compute, "emit_primary_beta", 0U, 0U, 0U, emit_primary_signal_beta_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "react_primary_resume",
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph) |
                node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            reactive_primary_node,
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    "primary_signal",
                    {}
                }
            }
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_intelligence_reactive_graph() {
    GraphDefinition graph;
    graph.id = 33;
    graph.name = "intelligence_reactive_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "emit_supported_claim", 0U, 0U, 0U, emit_supported_claim_primary_node, {}},
        NodeDefinition{2, NodeKind::Control, "root_stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}},
        NodeDefinition{
            3,
            NodeKind::Aggregate,
            "react_supported_claim",
            node_policy_mask(NodePolicyFlag::ReactToIntelligence),
            0U,
            0U,
            reactive_supported_claim_node,
            {},
            {},
            {},
            std::nullopt,
            {},
            std::vector<IntelligenceSubscription>{
                IntelligenceSubscription{
                    IntelligenceSubscriptionKind::Claims,
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    0.0F,
                    0.0F,
                    std::nullopt,
                    IntelligenceClaimStatus::Supported,
                    std::nullopt,
                    std::nullopt
                }
            }
        },
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "react_rejected_claim",
            node_policy_mask(NodePolicyFlag::ReactToIntelligence),
            0U,
            0U,
            reactive_rejected_claim_node,
            {},
            {},
            {},
            std::nullopt,
            {},
            std::vector<IntelligenceSubscription>{
                IntelligenceSubscription{
                    IntelligenceSubscriptionKind::Claims,
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    0.0F,
                    0.0F,
                    std::nullopt,
                    IntelligenceClaimStatus::Rejected,
                    std::nullopt,
                    std::nullopt
                }
            }
        },
        NodeDefinition{5, NodeKind::Control, "reactive_stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 3, 5, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{3, 4, 5, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{2});
    graph.bind_outgoing_edges(4, std::vector<EdgeId>{3});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_intelligence_duplicate_reactive_graph() {
    GraphDefinition graph;
    graph.id = 34;
    graph.name = "intelligence_duplicate_reactive_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "emit_supported_claim_once", 0U, 0U, 0U, emit_supported_claim_primary_node, {}},
        NodeDefinition{2, NodeKind::Compute, "emit_supported_claim_duplicate", 0U, 0U, 0U, emit_supported_claim_primary_node, {}},
        NodeDefinition{3, NodeKind::Compute, "emit_supported_claim_duplicate_again", 0U, 0U, 0U, emit_supported_claim_primary_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "react_supported_claim_once",
            node_policy_mask(NodePolicyFlag::ReactToIntelligence) |
                node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            reactive_supported_claim_node,
            {},
            {},
            {},
            std::nullopt,
            {},
            std::vector<IntelligenceSubscription>{
                IntelligenceSubscription{
                    IntelligenceSubscriptionKind::Claims,
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    0.0F,
                    0.0F,
                    std::nullopt,
                    IntelligenceClaimStatus::Supported,
                    std::nullopt,
                    std::nullopt
                }
            }
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 2, 3, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{2});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_intelligence_reactive_resume_graph() {
    GraphDefinition graph;
    graph.id = 35;
    graph.name = "intelligence_reactive_resume_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_resume_claims",
            node_policy_mask(NodePolicyFlag::AllowFanOut),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "emit_supported_claim_alpha", 0U, 0U, 0U, emit_supported_claim_alpha_node, {}},
        NodeDefinition{3, NodeKind::Compute, "emit_supported_claim_beta", 0U, 0U, 0U, emit_supported_claim_beta_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "react_supported_claim_resume",
            node_policy_mask(NodePolicyFlag::ReactToIntelligence) |
                node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            reactive_supported_claim_node,
            {},
            {},
            {},
            std::nullopt,
            {},
            std::vector<IntelligenceSubscription>{
                IntelligenceSubscription{
                    IntelligenceSubscriptionKind::Claims,
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    {},
                    0.0F,
                    0.0F,
                    std::nullopt,
                    IntelligenceClaimStatus::Supported,
                    std::nullopt,
                    std::nullopt
                }
            }
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_intelligence_claim_graph_reactive_graph() {
    GraphDefinition graph;
    graph.id = 36;
    graph.name = "intelligence_claim_graph_reactive_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "emit_supported_claim", 0U, 0U, 0U, emit_supported_claim_primary_node, {}},
        NodeDefinition{2, NodeKind::Control, "root_stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}},
        NodeDefinition{
            3,
            NodeKind::Aggregate,
            "react_claim_graph_match",
            node_policy_mask(NodePolicyFlag::ReactToIntelligence),
            0U,
            0U,
            reactive_supported_claim_node,
            {},
            {},
            {},
            std::nullopt,
            {},
            std::vector<IntelligenceSubscription>{
                IntelligenceSubscription{
                    IntelligenceSubscriptionKind::All,
                    {},
                    {},
                    {},
                    {},
                    "agentcore",
                    "supports",
                    "reactive_intelligence",
                    {},
                    {},
                    {},
                    0.0F,
                    0.0F,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt
                }
            }
        },
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "react_claim_graph_miss",
            node_policy_mask(NodePolicyFlag::ReactToIntelligence),
            0U,
            0U,
            reactive_supported_claim_node,
            {},
            {},
            {},
            std::nullopt,
            {},
            std::vector<IntelligenceSubscription>{
                IntelligenceSubscription{
                    IntelligenceSubscriptionKind::All,
                    {},
                    {},
                    {},
                    {},
                    "other-runtime",
                    "supports",
                    "reactive_intelligence",
                    {},
                    {},
                    {},
                    0.0F,
                    0.0F,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt
                }
            }
        },
        NodeDefinition{5, NodeKind::Control, "reactive_stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 3, 5, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{3, 4, 5, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{2});
    graph.bind_outgoing_edges(4, std::vector<EdgeId>{3});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_parallel_fanout_graph() {
    GraphDefinition graph;
    graph.id = 12;
    graph.name = "parallel_fanout_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Control, "fanout", node_policy_mask(NodePolicyFlag::AllowFanOut), 0U, 0U, fanout_node, {}},
        NodeDefinition{2, NodeKind::Compute, "parallel_a", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{3, NodeKind::Compute, "parallel_b", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{4, NodeKind::Control, "stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3, 2, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{4, 3, 4, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{3});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{4});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_parallel_trace_emission_graph() {
    GraphDefinition graph;
    graph.id = 41U;
    graph.name = "parallel_trace_emission_test";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Control,
            "fanout",
            node_policy_mask(NodePolicyFlag::AllowFanOut),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2U, NodeKind::Compute, "trace_left", 0U, 0U, 0U, parallel_trace_left_node, {}},
        NodeDefinition{3U, NodeKind::Compute, "trace_right", 0U, 0U, 0U, parallel_trace_right_node, {}},
        NodeDefinition{
            4U,
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
        EdgeDefinition{2U, 1U, 3U, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3U, 2U, 4U, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{4U, 3U, 4U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U, 2U});
    graph.bind_outgoing_edges(2U, std::vector<EdgeId>{3U});
    graph.bind_outgoing_edges(3U, std::vector<EdgeId>{4U});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_async_tool_graph() {
    GraphDefinition graph;
    graph.id = 13;
    graph.name = "async_tool_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Tool, "async_tool_roundtrip", 0U, 0U, 0U, async_tool_roundtrip_node, {}},
        NodeDefinition{2, NodeKind::Control, "stop", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_parallel_join_graph() {
    GraphDefinition graph;
    graph.id = 14;
    graph.name = "parallel_join_test";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_join",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "join_a", 0U, 0U, 0U, join_branch_a_node, {}},
        NodeDefinition{3, NodeKind::Compute, "join_b", 0U, 0U, 0U, join_branch_b_node, {}},
        NodeDefinition{4, NodeKind::Compute, "join_c", 0U, 0U, 0U, join_branch_c_node, {}},
        NodeDefinition{5, NodeKind::Compute, "join_d", 0U, 0U, 0U, join_branch_d_node, {}},
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
    graph.id = 15;
    graph.name = "async_join_test";
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

GraphDefinition make_conflicting_join_without_rule_graph() {
    GraphDefinition graph;
    graph.id = 19;
    graph.name = "conflicting_join_without_rule";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_conflict",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "left_conflict", 0U, 0U, 0U, conflicting_join_left_node, {}},
        NodeDefinition{3, NodeKind::Compute, "right_conflict", 0U, 0U, 0U, conflicting_join_right_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "join_conflict",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_join_node,
            {}
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

GraphDefinition make_invalid_join_scope_terminates_early_graph() {
    GraphDefinition graph;
    graph.id = 16;
    graph.name = "invalid_join_scope_terminates_early";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_invalid_early",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "branch_to_join", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{3, NodeKind::Control, "branch_stops", node_policy_mask(NodePolicyFlag::StopAfterNode), 0U, 0U, stop_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "join_barrier",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_join_node,
            {}
        },
        NodeDefinition{5, NodeKind::Compute, "dummy_incoming", 0U, 0U, 0U, parallel_branch_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3, 2, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{4, 5, 4, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{3});
    graph.bind_outgoing_edges(5, std::vector<EdgeId>{4});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_invalid_join_scope_multiple_targets_graph() {
    GraphDefinition graph;
    graph.id = 17;
    graph.name = "invalid_join_scope_multiple_targets";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{
            1,
            NodeKind::Control,
            "fanout_invalid_targets",
            node_policy_mask(NodePolicyFlag::AllowFanOut) |
                node_policy_mask(NodePolicyFlag::CreateJoinScope),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2, NodeKind::Compute, "branch_a", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{3, NodeKind::Compute, "branch_b", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{
            4,
            NodeKind::Aggregate,
            "join_a",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_join_node,
            {}
        },
        NodeDefinition{
            5,
            NodeKind::Aggregate,
            "join_b",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_join_node,
            {}
        },
        NodeDefinition{6, NodeKind::Compute, "dummy_a", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{7, NodeKind::Compute, "dummy_b", 0U, 0U, 0U, parallel_branch_node, {}}
    };
    graph.edges = {
        EdgeDefinition{1, 1, 2, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 1, 3, EdgeKind::OnSuccess, nullptr, 90U},
        EdgeDefinition{3, 2, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{4, 6, 4, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{5, 3, 5, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{6, 7, 5, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1, 2});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{3});
    graph.bind_outgoing_edges(3, std::vector<EdgeId>{5});
    graph.bind_outgoing_edges(6, std::vector<EdgeId>{4});
    graph.bind_outgoing_edges(7, std::vector<EdgeId>{6});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_invalid_unscoped_join_barrier_graph() {
    GraphDefinition graph;
    graph.id = 18;
    graph.name = "invalid_unscoped_join_barrier";
    graph.entry = 1;
    graph.nodes = {
        NodeDefinition{1, NodeKind::Compute, "left", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{2, NodeKind::Compute, "right", 0U, 0U, 0U, parallel_branch_node, {}},
        NodeDefinition{
            3,
            NodeKind::Aggregate,
            "join_without_scope",
            node_policy_mask(NodePolicyFlag::JoinIncomingBranches),
            0U,
            0U,
            summarize_join_node,
            {}
        }
    };
    graph.edges = {
        EdgeDefinition{1, 1, 3, EdgeKind::OnSuccess, nullptr, 100U},
        EdgeDefinition{2, 2, 3, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1, std::vector<EdgeId>{1});
    graph.bind_outgoing_edges(2, std::vector<EdgeId>{2});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_invalid_duplicate_merge_rule_graph() {
    GraphDefinition graph = make_parallel_join_graph();
    for (NodeDefinition& node : graph.nodes) {
        if (node.id == 6) {
            node.field_merge_rules.push_back(FieldMergeRule{kJoinAccumulator, JoinMergeStrategy::MaxInt64});
            break;
        }
    }
    graph.id = 20;
    graph.name = "invalid_duplicate_merge_rule";
    return graph;
}

GraphDefinition make_invalid_non_join_merge_rule_graph() {
    GraphDefinition graph = make_parallel_join_graph();
    for (NodeDefinition& node : graph.nodes) {
        if (node.id == 2) {
            node.field_merge_rules.push_back(FieldMergeRule{kJoinAccumulator, JoinMergeStrategy::SumInt64});
            break;
        }
    }
    graph.id = 21;
    graph.name = "invalid_non_join_merge_rule";
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

std::size_t count_node_events(const std::vector<TraceEvent>& events, NodeId node_id) {
    return static_cast<std::size_t>(std::count_if(
        events.begin(),
        events.end(),
        [node_id](const TraceEvent& event) {
            return event.node_id == node_id;
        }
    ));
}

void test_resume_flow() {
    ExecutionEngine engine;
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_resume_graph(), input);
    const StepResult first_step = engine.step(run_id);
    assert(first_step.progressed);
    assert(first_step.status == ExecutionStatus::Paused);
    assert(first_step.node_status == NodeResult::Waiting);
    assert(first_step.checkpoint_id != 0U);

    const ResumeResult resume_result = engine.resume(first_step.checkpoint_id);
    assert(resume_result.resumed);
    assert(resume_result.run_id == run_id);

    const RunResult run_result = engine.run_to_completion(run_id);
    assert(run_result.status == ExecutionStatus::Completed);
    const WorkflowState& state = engine.state(run_id);
    assert(state.size() > kResumeDone);
    assert(std::holds_alternative<bool>(state.load(kResumeDone)));
    assert(std::get<bool>(state.load(kResumeDone)));
}

void test_fresh_run_snapshot_and_step_preserve_deferred_entry_task() {
    ExecutionEngine engine(1);
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_resume_graph(), input);
    const RunSnapshot snapshot = engine.inspect(run_id);
    verbose_log_snapshot_summary("fresh_run_snapshot", snapshot);
    assert(snapshot.status == ExecutionStatus::Running);
    assert(snapshot.pending_tasks.size() == 1U);
    assert(snapshot.pending_tasks.front().run_id == run_id);
    assert(snapshot.pending_tasks.front().node_id == 1U);
    assert(snapshot.pending_tasks.front().branch_id == 0U);
    assert(snapshot.pending_tasks.front().ready_at_ns == 0U);

    const StepResult first_step = engine.step(run_id);
    assert(first_step.progressed);
    assert(first_step.status == ExecutionStatus::Paused);
    assert(first_step.node_status == NodeResult::Waiting);

    const RunSnapshot paused_snapshot = engine.inspect(run_id);
    verbose_log_snapshot_summary("paused_snapshot", paused_snapshot);
    assert(paused_snapshot.pending_tasks.empty());
}

void test_knowledge_graph_integration() {
    ExecutionEngine engine;
    InputEnvelope input;
    input.initial_field_count = 1;

    const RunId run_id = engine.start(make_knowledge_graph_test_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);
    assert(engine.knowledge_graph(run_id).entity_count() == 2U);
    assert(engine.knowledge_graph(run_id).triple_count() == 1U);

    const WorkflowState& state = engine.state(run_id);
    assert(std::holds_alternative<BlobRef>(state.load(kKnowledgeGraphSummary)));
    const BlobRef summary_ref = std::get<BlobRef>(state.load(kKnowledgeGraphSummary));
    const std::string summary(engine.state_store(run_id).blobs().read_string(summary_ref));
    assert(summary == "agentcore faster_than reference_runtime");
}

void test_subgraph_composition_and_public_streaming() {
    ExecutionEngine engine(2);
    engine.register_graph(make_subgraph_child_graph());

    InputEnvelope input;
    input.initial_field_count = 3U;
    input.initial_patch.updates.push_back(FieldUpdate{kSubgraphInput, int64_t{5}});

    auto verify_parent_run = [&](RunId run_id, const RunResult& result) {
        assert(result.status == ExecutionStatus::Completed);

        const WorkflowState& state = engine.state(run_id);
        assert(read_int_field(state, kSubgraphOutput) == 46);
        assert(std::holds_alternative<BlobRef>(state.load(kSubgraphSummary)));
        assert(
            engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.load(kSubgraphSummary))) ==
            "subgraph-output=46"
        );

        const std::vector<const KnowledgeTriple*> propagated =
            engine.knowledge_graph(run_id).match(std::nullopt, std::nullopt, std::nullopt);
        assert(propagated.empty());

        StreamCursor cursor;
        const std::vector<StreamEvent> stream_events =
            engine.stream_events(run_id, cursor, StreamReadOptions{true});
        assert(!stream_events.empty());

        std::size_t subgraph_event_count = 0U;
        std::size_t root_event_count = 0U;
        for (const StreamEvent& event : stream_events) {
            if (event.namespaces.empty()) {
                ++root_event_count;
                assert(event.graph_id == 902U);
            } else {
                ++subgraph_event_count;
                assert(event.graph_id == 901U);
                assert(event.namespaces.size() == 1U);
                assert(event.namespaces.front().graph_id == 902U);
                assert(event.namespaces.front().node_name == "planner_subgraph");
            }
        }
        assert(root_event_count == 3U);
        assert(subgraph_event_count == 2U);

        StreamCursor root_only_cursor;
        const std::vector<StreamEvent> root_only_events =
            engine.stream_events(run_id, root_only_cursor, StreamReadOptions{false});
        assert(root_only_events.size() == root_event_count);
        for (const StreamEvent& event : root_only_events) {
            assert(event.namespaces.empty());
            assert(event.graph_id == 902U);
        }

        const auto final_record = engine.checkpoints().get(result.last_checkpoint_id);
        assert(final_record.has_value());
        return compute_run_proof_digest(*final_record, engine.trace().events_for_run(run_id));
    };

    const RunId run_id = engine.start(make_subgraph_parent_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    const RunProofDigest first_digest = verify_parent_run(run_id, result);
    assert(first_digest.combined_digest != 0U);

    const RunId repeated_run_id = engine.start(make_subgraph_parent_graph(), input);
    const RunResult repeated_result = engine.run_to_completion(repeated_run_id);
    const RunProofDigest repeated_digest = verify_parent_run(repeated_run_id, repeated_result);
    assert(repeated_digest.combined_digest != 0U);

    ExecutionEngine second_engine(2);
    second_engine.register_graph(make_subgraph_child_graph());
    const RunId second_run_id = second_engine.start(make_subgraph_parent_graph(), input);
    const RunResult second_result = second_engine.run_to_completion(second_run_id);
    assert(second_result.status == ExecutionStatus::Completed);
    const auto second_record = second_engine.checkpoints().get(second_result.last_checkpoint_id);
    assert(second_record.has_value());
    const RunProofDigest second_digest = compute_run_proof_digest(
        *second_record,
        second_engine.trace().events_for_run(second_run_id)
    );
    assert(second_digest.combined_digest == first_digest.combined_digest);
}

void test_resumable_subgraph_checkpoint_restart_equivalence() {
    verbose_runtime_test_log("resumable_subgraph: build graphs");
    const GraphDefinition child_graph = make_resumable_subgraph_child_graph();
    const GraphDefinition parent_graph = make_resumable_subgraph_parent_graph();

    InputEnvelope input;
    input.initial_field_count = 3U;
    input.initial_patch.updates.push_back(FieldUpdate{kResumableSubgraphInput, int64_t{7}});

    ExecutionEngine direct_engine(1);
    direct_engine.register_graph(child_graph);
    const RunId direct_run_id = direct_engine.start(parent_graph, input);
    verbose_runtime_test_log("resumable_subgraph: direct first step");
    const StepResult direct_wait = direct_engine.step(direct_run_id);
    assert(direct_wait.progressed);
    assert(direct_wait.node_status == NodeResult::Waiting);
    assert(direct_wait.status == ExecutionStatus::Paused);

    const auto direct_wait_record = direct_engine.checkpoints().get(direct_wait.checkpoint_id);
    assert(direct_wait_record.has_value());
    assert(direct_wait_record->resumable());
    assert(direct_wait_record->snapshot.has_value());
    assert(direct_wait_record->snapshot->branches.size() == 1U);
    assert(direct_wait_record->snapshot->branches.front().pending_subgraph.has_value());

    const PendingSubgraphExecution& direct_pending_subgraph =
        *direct_wait_record->snapshot->branches.front().pending_subgraph;
    const RunSnapshot nested_snapshot =
        deserialize_run_snapshot_bytes(direct_pending_subgraph.snapshot_bytes);
    assert(nested_snapshot.graph.id == child_graph.id);
    assert(nested_snapshot.status == ExecutionStatus::Paused);
    assert(nested_snapshot.branches.size() == 1U);
    assert(nested_snapshot.branches.front().frame.current_node == 1U);

    const ResumeResult direct_resume = direct_engine.resume(direct_wait.checkpoint_id);
    assert(direct_resume.resumed);
    verbose_runtime_test_log("resumable_subgraph: direct run_to_completion");
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const WorkflowState& direct_state = direct_engine.state(direct_run_id);
    assert(read_int_field(direct_state, kResumableSubgraphOutput) == 106);
    assert(std::holds_alternative<BlobRef>(direct_state.load(kResumableSubgraphSummary)));
    assert(
        direct_engine.state_store(direct_run_id).blobs().read_string(
            std::get<BlobRef>(direct_state.load(kResumableSubgraphSummary))
        ) == "resumable-subgraph-output=106"
    );

    StreamCursor direct_cursor;
    const std::vector<StreamEvent> direct_stream =
        direct_engine.stream_events(direct_run_id, direct_cursor, StreamReadOptions{true});
    std::size_t direct_namespaced_events = 0U;
    for (const StreamEvent& event : direct_stream) {
        if (!event.namespaces.empty()) {
            ++direct_namespaced_events;
            assert(event.namespaces.front().node_name == "resumable_planner_subgraph");
        }
    }
    assert(direct_namespaced_events == 3U);

    const auto direct_record = direct_engine.checkpoints().get(direct_result.last_checkpoint_id);
    assert(direct_record.has_value());
    assert(direct_record->snapshot.has_value());
    verbose_log_snapshot_summary("resumable_subgraph:direct", *direct_record->snapshot);
    const RunProofDigest direct_digest = compute_run_proof_digest(
        *direct_record,
        normalize_trace_for_resume_proof(direct_engine.trace().events_for_run(direct_run_id))
    );

    const std::string checkpoint_path = "/tmp/agentcore_resumable_subgraph_restart.bin";
    std::remove(checkpoint_path.c_str());

    RunId resumed_run_id = 0U;
    CheckpointId resumed_checkpoint_id = 0U;
    std::vector<TraceEvent> prefix_trace;
    {
        verbose_runtime_test_log("resumable_subgraph: persisted first engine start");
        ExecutionEngine first_engine(1);
        first_engine.register_graph(child_graph);
        first_engine.enable_checkpoint_persistence(checkpoint_path);

        resumed_run_id = first_engine.start(parent_graph, input);
        verbose_runtime_test_log("resumable_subgraph: persisted first step");
        const StepResult first_wait = first_engine.step(resumed_run_id);
        assert(first_wait.progressed);
        assert(first_wait.node_status == NodeResult::Waiting);
        assert(first_wait.status == ExecutionStatus::Paused);
        resumed_checkpoint_id = first_wait.checkpoint_id;
        prefix_trace = trace_events_through_checkpoint(
            first_engine.trace().events_for_run(resumed_run_id),
            resumed_checkpoint_id
        );

        const auto first_record = first_engine.checkpoints().get(resumed_checkpoint_id);
        assert(first_record.has_value());
        assert(first_record->resumable());
        assert(first_record->snapshot.has_value());
        assert(first_record->snapshot->branches.front().pending_subgraph.has_value());
    }

    ExecutionEngine resumed_engine(1);
    resumed_engine.register_graph(parent_graph);
    resumed_engine.register_graph(child_graph);
    resumed_engine.enable_checkpoint_persistence(checkpoint_path);
    const std::size_t loaded_records = resumed_engine.load_persisted_checkpoints();
    verbose_runtime_test_log(
        "resumable_subgraph: loaded_records=" + std::to_string(loaded_records)
    );
    assert(loaded_records >= 1U);

    const ResumeResult resume_result = resumed_engine.resume(resumed_checkpoint_id);
    verbose_runtime_test_log(
        "resumable_subgraph: resume_result resumed=" +
        std::to_string(resume_result.resumed ? 1 : 0) +
        " status=" + std::to_string(static_cast<uint32_t>(resume_result.status)) +
        " run_id=" + std::to_string(resume_result.run_id)
    );
    assert(resume_result.resumed);
    assert(resume_result.run_id == resumed_run_id);

    verbose_runtime_test_log("resumable_subgraph: resumed run_to_completion");
    const RunResult resumed_result = resumed_engine.run_to_completion(resumed_run_id);
    assert(resumed_result.status == ExecutionStatus::Completed);
    const WorkflowState& resumed_state = resumed_engine.state(resumed_run_id);
    assert(read_int_field(resumed_state, kResumableSubgraphOutput) == 106);

    const auto resumed_record = resumed_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    assert(resumed_record->resumable());
    assert(resumed_record->snapshot.has_value());
    verbose_log_snapshot_summary("resumable_subgraph:resumed", *resumed_record->snapshot);

    const std::vector<TraceEvent> resumed_trace = append_trace_events(
        prefix_trace,
        resumed_engine.trace().events_for_run(resumed_run_id)
    );
    const RunProofDigest resumed_digest = compute_run_proof_digest(
        *resumed_record,
        normalize_trace_for_resume_proof(resumed_trace)
    );
    assert(direct_digest.snapshot_digest == resumed_digest.snapshot_digest);
    assert(direct_digest.trace_digest == resumed_digest.trace_digest);
    assert(direct_digest.combined_digest == resumed_digest.combined_digest);

    std::remove(checkpoint_path.c_str());
}

void test_async_subgraph_auto_resume_through_parent_scheduler() {
    verbose_runtime_test_log("async_subgraph: build graphs");
    const GraphDefinition child_graph = make_async_subgraph_child_graph();
    const GraphDefinition parent_graph = make_async_subgraph_parent_graph();

    auto register_async_echo = [](ExecutionEngine& engine) {
        engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
            const std::string payload = std::string(context.blobs.read_string(request.input));
            if (payload == "join-left") {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            } else if (payload == "join-right") {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(35));
            }
            return ToolResponse{
                true,
                context.blobs.append_string("nested::" + payload),
                kToolFlagNone
            };
        });
    };

    InputEnvelope input;
    input.initial_field_count = 2U;

    verbose_runtime_test_log("async_subgraph: direct engine start");
    ExecutionEngine direct_engine(1);
    direct_engine.register_graph(child_graph);
    register_async_echo(direct_engine);

    const RunId direct_run_id = direct_engine.start(parent_graph, input);
    verbose_runtime_test_log("async_subgraph: direct first step");
    const StepResult first_step = direct_engine.step(direct_run_id);
    assert(first_step.progressed);
    assert(first_step.node_status == NodeResult::Waiting);
    assert(first_step.waiting);
    assert(first_step.status == ExecutionStatus::Running);

    const auto waiting_record = direct_engine.checkpoints().get(first_step.checkpoint_id);
    assert(waiting_record.has_value());
    assert(waiting_record->resumable());
    assert(waiting_record->snapshot.has_value());
    assert(waiting_record->snapshot->branches.size() == 1U);
    assert(waiting_record->snapshot->branches.front().pending_async.has_value());
    assert(waiting_record->snapshot->branches.front().pending_subgraph.has_value());

    verbose_runtime_test_log("async_subgraph: direct run_to_completion");
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const WorkflowState& direct_state = direct_engine.state(direct_run_id);
    assert(std::holds_alternative<BlobRef>(direct_state.load(kAsyncSubgraphResult)));
    assert(
        direct_engine.state_store(direct_run_id).blobs().read_string(
            std::get<BlobRef>(direct_state.load(kAsyncSubgraphResult))
        ) == "nested::async-fast-path"
    );
    assert(std::holds_alternative<BlobRef>(direct_state.load(kAsyncSubgraphSummary)));
    assert(
        direct_engine.state_store(direct_run_id).blobs().read_string(
            std::get<BlobRef>(direct_state.load(kAsyncSubgraphSummary))
        ) == "async-subgraph=nested::async-fast-path"
    );

    StreamCursor direct_cursor;
    const std::vector<StreamEvent> direct_stream =
        direct_engine.stream_events(direct_run_id, direct_cursor, StreamReadOptions{true});
    std::size_t direct_namespaced_events = 0U;
    for (const StreamEvent& event : direct_stream) {
        if (!event.namespaces.empty()) {
            ++direct_namespaced_events;
            assert(event.namespaces.front().node_name == "async_planner_subgraph");
        }
    }
    assert(direct_namespaced_events == 3U);

    const auto direct_record = direct_engine.checkpoints().get(direct_result.last_checkpoint_id);
    assert(direct_record.has_value());
    const RunProofDigest direct_digest = compute_run_proof_digest(
        *direct_record,
        normalize_trace_for_resume_proof(direct_engine.trace().events_for_run(direct_run_id))
    );

    const std::string checkpoint_path = "/tmp/agentcore_async_subgraph_restart.bin";
    std::remove(checkpoint_path.c_str());

    RunId resumed_run_id = 0U;
    CheckpointId resumed_checkpoint_id = 0U;
    std::vector<TraceEvent> prefix_trace;
    {
        verbose_runtime_test_log("async_subgraph: persisted first engine start");
        ExecutionEngine first_engine(1);
        first_engine.register_graph(child_graph);
        first_engine.enable_checkpoint_persistence(checkpoint_path);
        register_async_echo(first_engine);

        resumed_run_id = first_engine.start(parent_graph, input);
        verbose_runtime_test_log("async_subgraph: persisted first step");
        const StepResult resumed_wait = first_engine.step(resumed_run_id);
        assert(resumed_wait.progressed);
        assert(resumed_wait.node_status == NodeResult::Waiting);
        assert(resumed_wait.status == ExecutionStatus::Running);
        resumed_checkpoint_id = resumed_wait.checkpoint_id;
        prefix_trace = trace_events_through_checkpoint(
            first_engine.trace().events_for_run(resumed_run_id),
            resumed_checkpoint_id
        );
    }

    ExecutionEngine resumed_engine(1);
    resumed_engine.register_graph(parent_graph);
    resumed_engine.register_graph(child_graph);
    resumed_engine.enable_checkpoint_persistence(checkpoint_path);
    register_async_echo(resumed_engine);
    verbose_runtime_test_log("async_subgraph: load persisted checkpoints");
    assert(resumed_engine.load_persisted_checkpoints() >= 1U);

    verbose_runtime_test_log("async_subgraph: resume persisted checkpoint");
    const ResumeResult resume_result = resumed_engine.resume(resumed_checkpoint_id);
    assert(resume_result.resumed);
    verbose_runtime_test_log("async_subgraph: resumed run_to_completion");
    const RunResult resumed_result = resumed_engine.run_to_completion(resumed_run_id);
    assert(resumed_result.status == ExecutionStatus::Completed);
    const auto resumed_record = resumed_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    assert(resumed_record->resumable());

    const RunProofDigest resumed_digest = compute_run_proof_digest(
        *resumed_record,
        normalize_trace_for_resume_proof(
            append_trace_events(
                prefix_trace,
                resumed_engine.trace().events_for_run(resumed_run_id)
            )
        )
    );
    assert(direct_digest.snapshot_digest == resumed_digest.snapshot_digest);
    assert(direct_digest.trace_digest == resumed_digest.trace_digest);
    assert(direct_digest.combined_digest == resumed_digest.combined_digest);

    std::remove(checkpoint_path.c_str());
}

void test_async_multi_wait_subgraph_checkpoint_resume_equivalence() {
    const GraphDefinition child_graph = make_async_multi_wait_subgraph_child_graph();
    const GraphDefinition parent_graph = make_async_multi_wait_subgraph_parent_graph();

    auto register_async_echo = [](ExecutionEngine& engine) {
        engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
            std::this_thread::sleep_for(std::chrono::milliseconds(35));
            return ToolResponse{
                true,
                context.blobs.append_string(
                    "nested::" + std::string(context.blobs.read_string(request.input))
                ),
                kToolFlagNone
            };
        });
    };

    InputEnvelope input;
    input.initial_field_count = 2U;

    ExecutionEngine direct_engine(1);
    direct_engine.register_graph(child_graph);
    register_async_echo(direct_engine);

    const RunId direct_run_id = direct_engine.start(parent_graph, input);
    const StepResult first_step = direct_engine.step(direct_run_id);
    assert(first_step.progressed);
    assert(first_step.node_status == NodeResult::Waiting);
    assert(first_step.waiting);
    assert(first_step.status == ExecutionStatus::Running);

    const auto waiting_record = direct_engine.checkpoints().get(first_step.checkpoint_id);
    assert(waiting_record.has_value());
    assert(waiting_record->resumable());
    assert(waiting_record->snapshot.has_value());
    assert(waiting_record->snapshot->branches.size() == 1U);
    assert(!waiting_record->snapshot->branches.front().pending_async.has_value());
    assert(waiting_record->snapshot->branches.front().pending_async_group.size() == 2U);
    assert(waiting_record->snapshot->branches.front().pending_subgraph.has_value());
    assert(waiting_record->snapshot->branches.front().pending_tool_snapshots.size() == 2U);

    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const WorkflowState& direct_state = direct_engine.state(direct_run_id);
    assert(std::holds_alternative<BlobRef>(direct_state.load(kAsyncSubgraphMultiWaitSummary)));
    assert(
        direct_engine.state_store(direct_run_id).blobs().read_string(
            std::get<BlobRef>(direct_state.load(kAsyncSubgraphMultiWaitSummary))
        ) == "multi-wait-subgraph=left=nested::join-left right=nested::join-right"
    );

    StreamCursor direct_cursor;
    const std::vector<StreamEvent> direct_stream =
        direct_engine.stream_events(direct_run_id, direct_cursor, StreamReadOptions{true});
    std::size_t direct_namespaced_events = 0U;
    for (const StreamEvent& event : direct_stream) {
        if (!event.namespaces.empty()) {
            ++direct_namespaced_events;
            assert(event.namespaces.front().node_name == "async_multi_wait_subgraph");
        }
    }
    assert(direct_namespaced_events == 9U);

    const RunProofDigest direct_digest = digest_for_result(
        direct_engine,
        direct_run_id,
        direct_result.last_checkpoint_id
    );

    const std::string checkpoint_path = "/tmp/agentcore_async_multi_wait_subgraph_restart.bin";
    std::remove(checkpoint_path.c_str());

    RunId resumed_run_id = 0U;
    CheckpointId resumed_checkpoint_id = 0U;
    {
        ExecutionEngine first_engine(1);
        first_engine.register_graph(child_graph);
        first_engine.enable_checkpoint_persistence(checkpoint_path);
        register_async_echo(first_engine);

        resumed_run_id = first_engine.start(parent_graph, input);
        const StepResult resumed_wait = first_engine.step(resumed_run_id);
        assert(resumed_wait.progressed);
        assert(resumed_wait.node_status == NodeResult::Waiting);
        resumed_checkpoint_id = resumed_wait.checkpoint_id;
    }

    ExecutionEngine resumed_engine(1);
    resumed_engine.register_graph(parent_graph);
    resumed_engine.register_graph(child_graph);
    resumed_engine.enable_checkpoint_persistence(checkpoint_path);
    register_async_echo(resumed_engine);
    assert(resumed_engine.load_persisted_checkpoints() >= 1U);

    const ResumeResult resume_result = resumed_engine.resume(resumed_checkpoint_id);
    assert(resume_result.resumed);
    const RunResult resumed_result = resumed_engine.run_to_completion(resumed_run_id);
    assert(resumed_result.status == ExecutionStatus::Completed);
    const WorkflowState& resumed_state = resumed_engine.state(resumed_run_id);
    assert(std::holds_alternative<BlobRef>(resumed_state.load(kAsyncSubgraphMultiWaitSummary)));
    assert(
        resumed_engine.state_store(resumed_run_id).blobs().read_string(
            std::get<BlobRef>(resumed_state.load(kAsyncSubgraphMultiWaitSummary))
        ) == "multi-wait-subgraph=left=nested::join-left right=nested::join-right"
    );
    const auto resumed_record = resumed_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    assert(resumed_record->resumable());

    const RunProofDigest resumed_digest = compute_run_proof_digest(*resumed_record, {});
    assert(direct_digest.snapshot_digest == resumed_digest.snapshot_digest);

    std::remove(checkpoint_path.c_str());
}

void test_knowledge_graph_reactive_frontier() {
    ExecutionEngine engine(2);
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_knowledge_reactive_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    std::size_t primary_reactive_count = 0;
    std::size_t other_reactive_count = 0;
    std::size_t reactive_stop_count = 0;
    for (const TraceEvent& event : trace_events) {
        if (event.node_id == 3U) {
            ++primary_reactive_count;
        } else if (event.node_id == 4U) {
            ++other_reactive_count;
        } else if (event.node_id == 5U) {
            ++reactive_stop_count;
        }
    }

    assert(primary_reactive_count == 1U);
    assert(other_reactive_count == 0U);
    assert(reactive_stop_count == 1U);
}

void test_knowledge_graph_duplicate_writes_do_not_retrigger_frontier() {
    ExecutionEngine engine(1);
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_knowledge_duplicate_reactive_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    assert(count_node_events(trace_events, 4U) == 1U);
}

void test_knowledge_graph_reactive_frontier_drains_stably() {
    ExecutionEngine engine(1);
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_knowledge_reactive_draining_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    assert(count_node_events(trace_events, 5U) == 2U);
}

void test_knowledge_graph_reactive_checkpoint_resume_preserves_pending_frontier() {
    const std::string checkpoint_path = "/tmp/agentcore_reactive_frontier.bin";
    std::remove(checkpoint_path.c_str());

    const GraphDefinition graph = make_knowledge_reactive_resume_graph();
    CheckpointPolicy policy;
    policy.snapshot_interval_steps = 1U;

    InputEnvelope input;
    input.initial_field_count = 2;

    ExecutionEngine direct_engine(1);
    direct_engine.set_checkpoint_policy(policy);
    const RunId direct_run_id = direct_engine.start(graph, input);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const std::vector<TraceEvent> direct_trace = direct_engine.trace().events_for_run(direct_run_id);
    assert(count_node_events(direct_trace, 4U) == 2U);

    RunId resumed_run_id = 0U;
    CheckpointId frontier_checkpoint_id = 0U;
    std::vector<TraceEvent> prefix_trace;
    {
        ExecutionEngine engine(1);
        engine.set_checkpoint_policy(policy);
        engine.enable_checkpoint_persistence(checkpoint_path);

        resumed_run_id = engine.start(graph, input);
        const StepResult first_step = engine.step(resumed_run_id);
        assert(first_step.progressed);
        const StepResult second_step = engine.step(resumed_run_id);
        assert(second_step.progressed);
        const StepResult third_step = engine.step(resumed_run_id);
        assert(third_step.progressed);
        frontier_checkpoint_id = third_step.checkpoint_id;
        assert(frontier_checkpoint_id != 0U);

        const auto checkpoint_record = engine.checkpoints().get(frontier_checkpoint_id);
        assert(checkpoint_record.has_value());
        assert(checkpoint_record->resumable());
        assert(checkpoint_record->snapshot.has_value());
        assert(checkpoint_record->snapshot->reactive_frontiers.size() == 1U);
        assert(checkpoint_record->snapshot->reactive_frontiers.front().node_id == 4U);
        assert(checkpoint_record->snapshot->reactive_frontiers.front().pending_rerun);
        assert(checkpoint_record->snapshot->reactive_frontiers.front().pending_rerun_seed.has_value());

        std::size_t reactive_branch_count = 0U;
        for (const BranchSnapshot& branch : checkpoint_record->snapshot->branches) {
            if (branch.reactive_root_node_id.has_value() && *branch.reactive_root_node_id == 4U) {
                ++reactive_branch_count;
            }
        }
        assert(reactive_branch_count == 1U);

        prefix_trace = trace_events_through_checkpoint(
            engine.trace().events_for_run(resumed_run_id),
            frontier_checkpoint_id
        );
    }

    ExecutionEngine restored_engine(1);
    restored_engine.register_graph(graph);
    restored_engine.set_checkpoint_policy(policy);
    restored_engine.enable_checkpoint_persistence(checkpoint_path);
    assert(restored_engine.load_persisted_checkpoints() >= 1U);

    const ResumeResult resume_result = restored_engine.resume(frontier_checkpoint_id);
    assert(resume_result.resumed);
    assert(resume_result.run_id == resumed_run_id);

    const RunResult resumed_result = restored_engine.run_to_completion(resumed_run_id);
    assert(resumed_result.status == ExecutionStatus::Completed);
    const auto resumed_record = restored_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    assert(resumed_record->resumable());

    const std::vector<TraceEvent> resumed_trace = append_trace_events(
        prefix_trace,
        restored_engine.trace().events_for_run(resumed_run_id)
    );
    assert(count_node_events(resumed_trace, 4U) == 2U);

    const RunProofDigest direct_digest = digest_for_result(
        direct_engine,
        direct_run_id,
        direct_result.last_checkpoint_id
    );
    const RunProofDigest resumed_digest = compute_run_proof_digest(
        *resumed_record,
        resumed_trace
    );
    assert(direct_digest.snapshot_digest == resumed_digest.snapshot_digest);
    assert(direct_digest.trace_digest == resumed_digest.trace_digest);
    assert(direct_digest.combined_digest == resumed_digest.combined_digest);

    std::remove(checkpoint_path.c_str());
}

void test_intelligence_reactive_frontier() {
    ExecutionEngine engine(2);
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_intelligence_reactive_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    assert(count_node_events(trace_events, 3U) == 1U);
    assert(count_node_events(trace_events, 4U) == 0U);
    assert(count_node_events(trace_events, 5U) == 1U);
}

void test_intelligence_duplicate_writes_do_not_retrigger_frontier() {
    ExecutionEngine engine(1);
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_intelligence_duplicate_reactive_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    assert(count_node_events(trace_events, 4U) == 1U);
}

void test_intelligence_claim_graph_reactive_frontier() {
    ExecutionEngine engine(2);
    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_intelligence_claim_graph_reactive_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const std::vector<TraceEvent> trace_events = engine.trace().events_for_run(run_id);
    assert(count_node_events(trace_events, 3U) == 1U);
    assert(count_node_events(trace_events, 4U) == 0U);
    assert(count_node_events(trace_events, 5U) == 1U);
}

void test_intelligence_reactive_checkpoint_resume_preserves_pending_frontier() {
    const std::string checkpoint_path = "/tmp/agentcore_intelligence_reactive_frontier.bin";
    std::remove(checkpoint_path.c_str());

    const GraphDefinition graph = make_intelligence_reactive_resume_graph();
    CheckpointPolicy policy;
    policy.snapshot_interval_steps = 1U;

    InputEnvelope input;
    input.initial_field_count = 2;

    ExecutionEngine direct_engine(1);
    direct_engine.set_checkpoint_policy(policy);
    const RunId direct_run_id = direct_engine.start(graph, input);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const std::vector<TraceEvent> direct_trace = direct_engine.trace().events_for_run(direct_run_id);
    assert(count_node_events(direct_trace, 4U) == 2U);

    RunId resumed_run_id = 0U;
    CheckpointId frontier_checkpoint_id = 0U;
    std::vector<TraceEvent> prefix_trace;
    {
        ExecutionEngine engine(1);
        engine.set_checkpoint_policy(policy);
        engine.enable_checkpoint_persistence(checkpoint_path);

        resumed_run_id = engine.start(graph, input);
        const StepResult first_step = engine.step(resumed_run_id);
        assert(first_step.progressed);
        const StepResult second_step = engine.step(resumed_run_id);
        assert(second_step.progressed);
        const StepResult third_step = engine.step(resumed_run_id);
        assert(third_step.progressed);
        frontier_checkpoint_id = third_step.checkpoint_id;
        assert(frontier_checkpoint_id != 0U);

        const auto checkpoint_record = engine.checkpoints().get(frontier_checkpoint_id);
        assert(checkpoint_record.has_value());
        assert(checkpoint_record->resumable());
        assert(checkpoint_record->snapshot.has_value());
        assert(checkpoint_record->snapshot->reactive_frontiers.size() == 1U);
        assert(checkpoint_record->snapshot->reactive_frontiers.front().node_id == 4U);
        assert(checkpoint_record->snapshot->reactive_frontiers.front().pending_rerun);
        assert(checkpoint_record->snapshot->reactive_frontiers.front().pending_rerun_seed.has_value());

        std::size_t reactive_branch_count = 0U;
        for (const BranchSnapshot& branch : checkpoint_record->snapshot->branches) {
            if (branch.reactive_root_node_id.has_value() && *branch.reactive_root_node_id == 4U) {
                ++reactive_branch_count;
            }
        }
        assert(reactive_branch_count == 1U);

        prefix_trace = trace_events_through_checkpoint(
            engine.trace().events_for_run(resumed_run_id),
            frontier_checkpoint_id
        );
    }

    ExecutionEngine restored_engine(1);
    restored_engine.register_graph(graph);
    restored_engine.set_checkpoint_policy(policy);
    restored_engine.enable_checkpoint_persistence(checkpoint_path);
    assert(restored_engine.load_persisted_checkpoints() >= 1U);

    const ResumeResult resume_result = restored_engine.resume(frontier_checkpoint_id);
    assert(resume_result.resumed);
    assert(resume_result.run_id == resumed_run_id);

    const RunResult resumed_result = restored_engine.run_to_completion(resumed_run_id);
    assert(resumed_result.status == ExecutionStatus::Completed);
    const auto resumed_record = restored_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    assert(resumed_record->resumable());

    const std::vector<TraceEvent> resumed_trace = append_trace_events(
        prefix_trace,
        restored_engine.trace().events_for_run(resumed_run_id)
    );
    assert(count_node_events(resumed_trace, 4U) == 2U);

    const RunProofDigest direct_digest = digest_for_result(
        direct_engine,
        direct_run_id,
        direct_result.last_checkpoint_id
    );
    const RunProofDigest resumed_digest = compute_run_proof_digest(
        *resumed_record,
        resumed_trace
    );
    assert(direct_digest.snapshot_digest == resumed_digest.snapshot_digest);
    assert(direct_digest.trace_digest == resumed_digest.trace_digest);
    assert(direct_digest.combined_digest == resumed_digest.combined_digest);

    std::remove(checkpoint_path.c_str());
}

void test_parallel_scheduler_fanout() {
    g_active_parallel_nodes.store(0);
    g_max_parallel_nodes.store(0);

    ExecutionEngine engine(2);
    InputEnvelope input;
    const RunId run_id = engine.start(make_parallel_fanout_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);
    assert(g_max_parallel_nodes.load() >= 2);
}

void test_join_scope_merges_branch_state_and_knowledge_graph() {
    ExecutionEngine engine(4);
    InputEnvelope input;
    input.initial_field_count = 10;

    const RunId run_id = engine.start(make_parallel_join_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);
    assert(engine.knowledge_graph(run_id).triple_count() == 4U);

    const WorkflowState& state = engine.state(run_id);
    assert(std::holds_alternative<BlobRef>(state.load(kJoinSummary)));
    const std::string summary(
        engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.load(kJoinSummary)))
    );
    assert(summary == "sum=10 triples=4");
    assert(std::holds_alternative<int64_t>(state.load(kJoinAccumulator)));
    assert(std::get<int64_t>(state.load(kJoinAccumulator)) == 10);
}

void test_structural_join_validation() {
    std::string error_message;

    const GraphDefinition valid_parallel_join = make_parallel_join_graph();
    assert(valid_parallel_join.validate(&error_message));

    const GraphDefinition valid_async_join = make_async_join_graph();
    assert(valid_async_join.validate(&error_message));

    const GraphDefinition invalid_early = make_invalid_join_scope_terminates_early_graph();
    error_message.clear();
    assert(!invalid_early.validate(&error_message));
    assert(error_message.find("terminate") != std::string::npos);

    const GraphDefinition invalid_targets = make_invalid_join_scope_multiple_targets_graph();
    error_message.clear();
    assert(!invalid_targets.validate(&error_message));
    assert(error_message.find("single join barrier") != std::string::npos);

    const GraphDefinition invalid_unscoped_join = make_invalid_unscoped_join_barrier_graph();
    error_message.clear();
    assert(!invalid_unscoped_join.validate(&error_message));
    assert(error_message.find("validated convergence target") != std::string::npos);

    const GraphDefinition invalid_duplicate_rule = make_invalid_duplicate_merge_rule_graph();
    error_message.clear();
    assert(!invalid_duplicate_rule.validate(&error_message));
    assert(error_message.find("duplicate merge rule") != std::string::npos);

    const GraphDefinition invalid_non_join_rule = make_invalid_non_join_merge_rule_graph();
    error_message.clear();
    assert(!invalid_non_join_rule.validate(&error_message));
    assert(error_message.find("join barrier") != std::string::npos);
}

void test_join_runtime_rejects_conflicting_writes_without_rule() {
    ExecutionEngine engine(2);
    InputEnvelope input;
    input.initial_field_count = 10;

    const RunId run_id = engine.start(make_conflicting_join_without_rule_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Failed);
}

void test_hardened_adapters() {
    StateStore state_store;
    ToolInvocationContext tool_context{state_store.blobs(), state_store.strings()};
    ModelInvocationContext model_context{state_store.blobs(), state_store.strings()};

    ToolRegistry tools;
    register_http_tool_adapter(tools, "http_mock");
    register_sqlite_tool_adapter(tools, "sqlite_mock");

    ModelRegistry models;
    register_local_model_adapter(models, "local_test");
    register_llm_http_adapter(models, "llm_http_test");

    const BlobRef http_input = state_store.blobs().append_string("method=POST\nurl=mock://echo\nbody=hello");
    ToolResponse http_response = tools.invoke(
        ToolRequest{state_store.strings().intern("http_mock"), http_input},
        tool_context
    );
    assert(http_response.ok);
    assert(http_response.attempts == 1U);
    assert(state_store.blobs().read_string(http_response.output) == "hello");

    const BlobRef put_input = state_store.blobs().append_string("action=put\ntable=memory\nkey=x\nvalue=42");
    ToolResponse put_response = tools.invoke(
        ToolRequest{state_store.strings().intern("sqlite_mock"), put_input},
        tool_context
    );
    assert(put_response.ok);

    const BlobRef get_input = state_store.blobs().append_string("action=get\ntable=memory\nkey=x");
    ToolResponse get_response = tools.invoke(
        ToolRequest{state_store.strings().intern("sqlite_mock"), get_input},
        tool_context
    );
    assert(get_response.ok);
    assert(state_store.blobs().read_string(get_response.output) == "42");

    const BlobRef prompt = state_store.blobs().append_string("native runtime");
    ModelResponse local_response = models.invoke(
        ModelRequest{state_store.strings().intern("local_test"), prompt, {}, 8U},
        model_context
    );
    assert(local_response.ok);
    assert(local_response.attempts == 1U);
    assert(state_store.blobs().read_string(local_response.output) == "local::native r");

    const BlobRef schema = state_store.blobs().append_string("json");
    ModelResponse http_model_response = models.invoke(
        ModelRequest{state_store.strings().intern("llm_http_test"), prompt, schema, 32U},
        model_context
    );
    assert(http_model_response.ok);
    assert(std::string(state_store.blobs().read_string(http_model_response.output)).find("llm_http::") == 0U);

    ToolRegistry retry_tools;
    int attempts = 0;
    retry_tools.register_tool("retry_tool", ToolPolicy{1U, 0U, 1024U, 1024U}, [&attempts](const ToolRequest&, ToolInvocationContext& context) {
        attempts += 1;
        if (attempts == 1) {
            throw std::runtime_error("transient");
        }
        return ToolResponse{true, context.blobs.append_string("ok"), kToolFlagNone};
    });
    ToolResponse retry_response = retry_tools.invoke(
        ToolRequest{state_store.strings().intern("retry_tool"), {}},
        tool_context
    );
    assert(retry_response.ok);
    assert(retry_response.attempts == 2U);
}

void test_async_tool_auto_resume() {
    ExecutionEngine engine(2);
    engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return ToolResponse{
            true,
            context.blobs.append_string(std::string(context.blobs.read_string(request.input))),
            kToolFlagNone
        };
    });

    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_async_tool_graph(), input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);
    assert(result.steps_executed >= 2U);

    const WorkflowState& state = engine.state(run_id);
    assert(std::holds_alternative<bool>(state.load(kAsyncStarted)));
    assert(std::get<bool>(state.load(kAsyncStarted)));
    assert(std::holds_alternative<BlobRef>(state.load(kAsyncResult)));
    assert(
        engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.load(kAsyncResult))) ==
        "async-fast-path"
    );
}

void test_async_tool_checkpoint_resume_after_restart() {
    const std::string checkpoint_path = "/tmp/agentcore_async_restart.bin";
    std::remove(checkpoint_path.c_str());

    RunId run_id = 0U;
    CheckpointId checkpoint_id = 0U;
    {
        ExecutionEngine engine(2);
        engine.enable_checkpoint_persistence(checkpoint_path);
        engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return ToolResponse{
                true,
                context.blobs.append_string(std::string(context.blobs.read_string(request.input))),
                kToolFlagNone
            };
        });

        InputEnvelope input;
        input.initial_field_count = 2;

        run_id = engine.start(make_async_tool_graph(), input);
        const StepResult first_step = engine.step(run_id);
        assert(first_step.progressed);
        assert(first_step.node_status == NodeResult::Waiting);
        assert(first_step.waiting);
        checkpoint_id = first_step.checkpoint_id;
        assert(checkpoint_id != 0U);
    }

    ExecutionEngine restored_engine(2);
    restored_engine.register_graph(make_async_tool_graph());
    restored_engine.enable_checkpoint_persistence(checkpoint_path);
    restored_engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        return ToolResponse{
            true,
            context.blobs.append_string(std::string(context.blobs.read_string(request.input))),
            kToolFlagNone
        };
    });

    const std::size_t loaded_records = restored_engine.load_persisted_checkpoints();
    assert(loaded_records >= 1U);

    const ResumeResult resume_result = restored_engine.resume(checkpoint_id);
    assert(resume_result.resumed);
    assert(resume_result.run_id == run_id);
    assert(resume_result.message == "checkpoint restored");

    const RunResult run_result = restored_engine.run_to_completion(run_id);
    assert(run_result.status == ExecutionStatus::Completed);

    const WorkflowState& state = restored_engine.state(run_id);
    assert(std::holds_alternative<bool>(state.load(kAsyncStarted)));
    assert(std::get<bool>(state.load(kAsyncStarted)));
    assert(std::holds_alternative<BlobRef>(state.load(kAsyncResult)));
    assert(
        restored_engine.state_store(run_id).blobs().read_string(std::get<BlobRef>(state.load(kAsyncResult))) ==
        "async-fast-path"
    );

    std::remove(checkpoint_path.c_str());
}

void test_async_model_snapshot_restore() {
    StateStore state_store;
    ModelInvocationContext context{state_store.blobs(), state_store.strings()};

    ModelRegistry original_models;
    original_models.register_model("snapshot_model", [](const ModelRequest& request, ModelInvocationContext& ctx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string prompt(ctx.blobs.read_string(request.prompt));
        return ModelResponse{
            true,
            ctx.blobs.append_string("restored::" + prompt),
            0.91F,
            static_cast<uint32_t>(prompt.size()),
            kModelFlagNone
        };
    });

    const BlobRef prompt = state_store.blobs().append_string("restart me");
    const AsyncModelHandle handle = original_models.begin_invoke_async(
        ModelRequest{state_store.strings().intern("snapshot_model"), prompt, {}, 32U},
        context
    );
    assert(handle.valid());

    const std::optional<AsyncModelSnapshot> snapshot = original_models.export_async_operation(handle);
    assert(snapshot.has_value());
    assert(snapshot->handle.id == handle.id);

    ModelRegistry restored_models;
    restored_models.register_model("snapshot_model", [](const ModelRequest& request, ModelInvocationContext& ctx) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::string prompt(ctx.blobs.read_string(request.prompt));
        return ModelResponse{
            true,
            ctx.blobs.append_string("restored::" + prompt),
            0.91F,
            static_cast<uint32_t>(prompt.size()),
            kModelFlagNone
        };
    });

    const AsyncModelHandle restored_handle = restored_models.restore_async_operation(*snapshot);
    assert(restored_handle.id == handle.id);

    for (int attempt = 0; attempt < 20 && !restored_models.is_async_ready(restored_handle); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::optional<ModelResponse> response = restored_models.take_async_result(restored_handle, context);
    assert(response.has_value());
    assert(response->ok);
    assert(state_store.blobs().read_string(response->output) == "restored::restart me");
}

void test_proof_digest_determinism_across_parallelism() {
    InputEnvelope input;
    input.initial_field_count = 8;

    ExecutionEngine sequential_engine(1);
    const RunId sequential_run_id = sequential_engine.start(make_parallel_join_graph(), input);
    const RunResult sequential_result = sequential_engine.run_to_completion(sequential_run_id);
    assert(sequential_result.status == ExecutionStatus::Completed);
    const RunProofDigest sequential_digest = digest_for_result(
        sequential_engine,
        sequential_run_id,
        sequential_result.last_checkpoint_id
    );

    ExecutionEngine parallel_engine(4);
    const RunId parallel_run_id = parallel_engine.start(make_parallel_join_graph(), input);
    const RunResult parallel_result = parallel_engine.run_to_completion(parallel_run_id);
    assert(parallel_result.status == ExecutionStatus::Completed);
    const RunProofDigest parallel_digest = digest_for_result(
        parallel_engine,
        parallel_run_id,
        parallel_result.last_checkpoint_id
    );

    assert(sequential_digest.snapshot_digest == parallel_digest.snapshot_digest);
    assert(sequential_digest.trace_digest == parallel_digest.trace_digest);
    assert(sequential_digest.combined_digest == parallel_digest.combined_digest);
}

void test_node_emitted_trace_determinism_across_parallelism() {
    constexpr uint32_t kLeftTraceFlag = 1U << 18U;
    constexpr uint32_t kRightTraceFlag = 1U << 17U;

    auto run_once = [](std::size_t worker_count) {
        InputEnvelope input;
        input.initial_field_count = 4U;

        ExecutionEngine engine(worker_count);
        const RunId run_id = engine.start(make_parallel_trace_emission_graph(), input);
        const RunResult result = engine.run_to_completion(run_id);
        assert(result.status == ExecutionStatus::Completed);

        StreamCursor cursor;
        const std::vector<StreamEvent> stream_events =
            engine.stream_events(run_id, cursor, StreamReadOptions{true});
        std::vector<uint32_t> custom_trace_flags;
        for (const StreamEvent& event : stream_events) {
            if (event.flags == kLeftTraceFlag || event.flags == kRightTraceFlag) {
                custom_trace_flags.push_back(event.flags);
            }
        }

        return std::pair{
            digest_for_result(engine, run_id, result.last_checkpoint_id),
            custom_trace_flags
        };
    };

    const auto [sequential_digest, sequential_custom_trace_flags] = run_once(1U);
    const auto [parallel_digest, parallel_custom_trace_flags] = run_once(4U);
    const std::vector<uint32_t> expected_custom_trace_flags{kLeftTraceFlag, kRightTraceFlag};

    assert(sequential_digest.snapshot_digest == parallel_digest.snapshot_digest);
    assert(sequential_digest.trace_digest == parallel_digest.trace_digest);
    assert(sequential_digest.combined_digest == parallel_digest.combined_digest);
    assert(sequential_custom_trace_flags == expected_custom_trace_flags);
    assert(parallel_custom_trace_flags == sequential_custom_trace_flags);
}

void test_proof_digest_restart_equivalence() {
    ExecutionEngine direct_engine(2);
    direct_engine.tools().register_tool("async_echo", [](const ToolRequest& request, ToolInvocationContext& context) {
        std::this_thread::sleep_for(std::chrono::milliseconds(35));
        return ToolResponse{
            true,
            context.blobs.append_string(std::string(context.blobs.read_string(request.input))),
            kToolFlagNone
        };
    });

    InputEnvelope input;
    input.initial_field_count = 8;

    const RunId direct_run_id = direct_engine.start(make_async_join_graph(), input);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const RunProofDigest direct_digest = digest_for_result(
        direct_engine,
        direct_run_id,
        direct_result.last_checkpoint_id
    );

    const std::string checkpoint_path = "/tmp/agentcore_proof_digest_restart.bin";
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

        resumed_run_id = first_engine.start(make_async_join_graph(), input);
        StepResult first_step{};
        do {
            first_step = first_engine.step(resumed_run_id);
        } while (first_step.progressed && first_step.node_status != NodeResult::Waiting);
        assert(first_step.node_status == NodeResult::Waiting);
        assert(first_step.checkpoint_id != 0U);
        resumed_checkpoint_id = first_step.checkpoint_id;
        restart_trace = first_engine.trace().events_for_run(resumed_run_id);
    }

    ExecutionEngine resumed_engine(2);
    resumed_engine.register_graph(make_async_join_graph());
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
}

void test_checkpoint_cadence_preserves_resumable_restore() {
    const std::string checkpoint_path = "/tmp/agentcore_checkpoint_cadence.bin";
    std::remove(checkpoint_path.c_str());

    const GraphDefinition graph = make_checkpoint_cadence_graph();
    CheckpointPolicy policy;
    policy.snapshot_interval_steps = 7U;

    ExecutionEngine direct_engine(1);
    direct_engine.set_checkpoint_policy(policy);
    InputEnvelope input;
    input.initial_field_count = 4;

    const RunId direct_run_id = direct_engine.start(graph, input);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);
    const RunProofDigest direct_digest = digest_for_result(
        direct_engine,
        direct_run_id,
        direct_result.last_checkpoint_id
    );

    RunId run_id = 0U;
    CheckpointId cadence_checkpoint_id = 0U;
    CheckpointId metadata_only_checkpoint_id = 0U;
    std::vector<TraceEvent> restart_prefix_trace;

    {
        ExecutionEngine engine(1);
        engine.set_checkpoint_policy(policy);
        engine.enable_checkpoint_persistence(checkpoint_path);

        run_id = engine.start(graph, input);
        while (cadence_checkpoint_id == 0U) {
            const StepResult step = engine.step(run_id);
            assert(step.progressed);

            const auto record = engine.checkpoints().get(step.checkpoint_id);
            assert(record.has_value());
            if (!record->resumable()) {
                if (metadata_only_checkpoint_id == 0U) {
                    metadata_only_checkpoint_id = step.checkpoint_id;
                }
                continue;
            }

            if (step.status == ExecutionStatus::Running) {
                cadence_checkpoint_id = step.checkpoint_id;
            }
        }

        assert(metadata_only_checkpoint_id != 0U);
        const ResumeResult metadata_resume = engine.resume(metadata_only_checkpoint_id);
        assert(!metadata_resume.resumed);
        assert(metadata_resume.status == ExecutionStatus::Failed);
        assert(metadata_resume.message.find("metadata-only") != std::string::npos);

        assert(engine.checkpoints().resumable_count() < engine.checkpoints().size());
        restart_prefix_trace = engine.trace().events_for_run(run_id);
    }

    ExecutionEngine resumed_engine(1);
    resumed_engine.register_graph(graph);
    resumed_engine.set_checkpoint_policy(policy);
    resumed_engine.enable_checkpoint_persistence(checkpoint_path);
    assert(resumed_engine.load_persisted_checkpoints() >= 1U);

    const ResumeResult resume_result = resumed_engine.resume(cadence_checkpoint_id);
    assert(resume_result.resumed);
    assert(resume_result.run_id == run_id);

    const RunResult resumed_result = resumed_engine.run_to_completion(run_id);
    assert(resumed_result.status == ExecutionStatus::Completed);
    const WorkflowState& resumed_state = resumed_engine.state(run_id);
    assert(read_int_field(resumed_state, kCadenceAccumulator) == expected_cadence_accumulator());

    const auto resumed_record = resumed_engine.checkpoints().get(resumed_result.last_checkpoint_id);
    assert(resumed_record.has_value());
    assert(resumed_record->resumable());
    const RunProofDigest resumed_digest = compute_run_proof_digest(
        *resumed_record,
        append_trace_events(
            restart_prefix_trace,
            resumed_engine.trace().events_for_run(run_id)
        )
    );
    assert(direct_digest.snapshot_digest == resumed_digest.snapshot_digest);
    assert(direct_digest.trace_digest == resumed_digest.trace_digest);
    assert(direct_digest.combined_digest == resumed_digest.combined_digest);

    std::remove(checkpoint_path.c_str());
}

void test_durable_checkpoint_reload() {
    const std::string checkpoint_path = "/tmp/agentcore_checkpoint_reload.bin";
    std::remove(checkpoint_path.c_str());

    ExecutionEngine engine;
    engine.enable_checkpoint_persistence(checkpoint_path);

    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_resume_graph(), input);
    const StepResult first_step = engine.step(run_id);
    assert(first_step.progressed);
    assert(first_step.checkpoint_id != 0U);
    std::ifstream checkpoint_stream(checkpoint_path, std::ios::binary);
    assert(checkpoint_stream.good());
    checkpoint_stream.close();

    ExecutionEngine restored_engine;
    restored_engine.register_graph(make_resume_graph());
    restored_engine.enable_checkpoint_persistence(checkpoint_path);
    const std::size_t loaded_records = restored_engine.load_persisted_checkpoints();
    assert(loaded_records >= 1U);

    const ResumeResult resume_result = restored_engine.resume(first_step.checkpoint_id);
    assert(resume_result.resumed);
    assert(resume_result.run_id == run_id);

    const RunResult run_result = restored_engine.run_to_completion(run_id);
    assert(run_result.status == ExecutionStatus::Completed);
    const WorkflowState& state = restored_engine.state(run_id);
    assert(std::holds_alternative<bool>(state.load(kResumeDone)));
    assert(std::get<bool>(state.load(kResumeDone)));

    std::remove(checkpoint_path.c_str());
}

void exercise_interrupt_edit_resume_flow(bool use_sqlite) {
    const std::string checkpoint_path = use_sqlite
        ? "/tmp/agentcore_interrupt_resume.sqlite"
        : "/tmp/agentcore_interrupt_resume.bin";
    std::remove(checkpoint_path.c_str());

    ExecutionEngine engine(1);
    configure_checkpoint_storage(engine, checkpoint_path, use_sqlite);

    InputEnvelope input;
    input.initial_field_count = 2;

    const RunId run_id = engine.start(make_resume_graph(), input);
    const InterruptResult interrupt_result = engine.interrupt(run_id);
    assert(interrupt_result.interrupted);
    assert(interrupt_result.status == ExecutionStatus::Paused);
    assert(interrupt_result.checkpoint_id != 0U);

    const RunSnapshot inspected = engine.inspect(run_id);
    assert(inspected.status == ExecutionStatus::Paused);
    assert(inspected.pending_tasks.empty());
    assert(inspected.branches.size() == 1U);
    assert(inspected.branches.front().frame.status == ExecutionStatus::Paused);

    StatePatch manual_patch;
    manual_patch.updates.push_back(FieldUpdate{kResumeAttempt, int64_t{1}});
    const StateEditResult edit_result = engine.apply_state_patch(run_id, manual_patch);
    assert(edit_result.applied);
    assert(edit_result.branch_id == 0U);
    assert(edit_result.checkpoint_id > interrupt_result.checkpoint_id);

    const ResumeResult live_resume = engine.resume_run(run_id);
    assert(live_resume.resumed);
    const RunResult live_result = engine.run_to_completion(run_id);
    assert(live_result.status == ExecutionStatus::Completed);
    assert(std::get<bool>(engine.state(run_id).load(kResumeDone)));

    ExecutionEngine restored_engine(1);
    restored_engine.register_graph(make_resume_graph());
    configure_checkpoint_storage(restored_engine, checkpoint_path, use_sqlite);
    assert(restored_engine.load_persisted_checkpoints() >= 2U);

    const ResumeResult restart_resume = restored_engine.resume(edit_result.checkpoint_id);
    assert(restart_resume.resumed);
    assert(restart_resume.run_id == run_id);
    const RunResult restart_result = restored_engine.run_to_completion(run_id);
    assert(restart_result.status == ExecutionStatus::Completed);
    assert(std::get<bool>(restored_engine.state(run_id).load(kResumeDone)));

    std::remove(checkpoint_path.c_str());
}

void exercise_recorded_effect_restart_equivalence(bool use_sqlite) {
    const GraphDefinition graph = make_recorded_effect_graph();
    const CheckpointPolicy policy{1U, true, true, true, true};

    auto run_direct = [&](const std::string& checkpoint_path) {
        std::remove(checkpoint_path.c_str());
        g_recorded_effect_invocations.store(0);

        ExecutionEngine engine(1);
        engine.register_graph(graph);
        engine.set_checkpoint_policy(policy);
        configure_checkpoint_storage(engine, checkpoint_path, use_sqlite);

        InputEnvelope input;
        input.initial_field_count = 3;

        const RunId run_id = engine.start(graph, input);
        const StepResult first_step = engine.step(run_id);
        assert(first_step.progressed);
        assert(first_step.node_status == NodeResult::Waiting);
        assert(g_recorded_effect_invocations.load() == 1);

        const RunSnapshot paused = engine.inspect(run_id);
        assert(paused.branches.size() == 1U);
        assert(paused.branches.front().state_store.task_journal().size() == 1U);

        const ResumeResult live_resume = engine.resume_run(run_id);
        assert(live_resume.resumed);
        const RunResult run_result = engine.run_to_completion(run_id);
        assert(run_result.status == ExecutionStatus::Completed);
        assert(g_recorded_effect_invocations.load() == 1);

        const WorkflowState& final_state = engine.state(run_id);
        assert(std::get<bool>(final_state.load(kRecordedEffectDone)));
        assert(
            engine.state_store(run_id).blobs().read_string(
                std::get<BlobRef>(final_state.load(kRecordedEffectOutput))
            ) == "outcome::write-once"
        );
        assert(engine.state_store(run_id).task_journal().size() == 1U);

        const auto record = engine.checkpoints().get(run_result.last_checkpoint_id);
        assert(record.has_value());
        assert(record->resumable());
        const RunProofDigest digest = compute_run_proof_digest(
            *record,
            engine.trace().events_for_run(run_id)
        );
        std::remove(checkpoint_path.c_str());
        return digest;
    };

    auto run_restarted = [&](const std::string& checkpoint_path) {
        std::remove(checkpoint_path.c_str());
        g_recorded_effect_invocations.store(0);

        ExecutionEngine engine(1);
        engine.register_graph(graph);
        engine.set_checkpoint_policy(policy);
        configure_checkpoint_storage(engine, checkpoint_path, use_sqlite);

        InputEnvelope input;
        input.initial_field_count = 3;

        const RunId run_id = engine.start(graph, input);
        const StepResult first_step = engine.step(run_id);
        assert(first_step.progressed);
        assert(first_step.node_status == NodeResult::Waiting);
        assert(g_recorded_effect_invocations.load() == 1);

        const std::vector<TraceEvent> restart_prefix_trace = engine.trace().events_for_run(run_id);

        ExecutionEngine restored_engine(1);
        restored_engine.register_graph(graph);
        restored_engine.set_checkpoint_policy(policy);
        configure_checkpoint_storage(restored_engine, checkpoint_path, use_sqlite);
        assert(restored_engine.load_persisted_checkpoints() >= 1U);

        const ResumeResult resume_result = restored_engine.resume(first_step.checkpoint_id);
        assert(resume_result.resumed);
        const RunResult resumed_result = restored_engine.run_to_completion(run_id);
        assert(resumed_result.status == ExecutionStatus::Completed);
        assert(g_recorded_effect_invocations.load() == 1);

        const WorkflowState& final_state = restored_engine.state(run_id);
        assert(std::get<bool>(final_state.load(kRecordedEffectDone)));
        assert(
            restored_engine.state_store(run_id).blobs().read_string(
                std::get<BlobRef>(final_state.load(kRecordedEffectOutput))
            ) == "outcome::write-once"
        );
        assert(restored_engine.state_store(run_id).task_journal().size() == 1U);

        const auto record = restored_engine.checkpoints().get(resumed_result.last_checkpoint_id);
        assert(record.has_value());
        assert(record->resumable());
        const RunProofDigest digest = compute_run_proof_digest(
            *record,
            append_trace_events(
                restart_prefix_trace,
                restored_engine.trace().events_for_run(run_id)
            )
        );
        std::remove(checkpoint_path.c_str());
        return digest;
    };

    const std::string direct_checkpoint_path = use_sqlite
        ? "/tmp/agentcore_recorded_effect_direct.sqlite"
        : "/tmp/agentcore_recorded_effect_direct.bin";
    const std::string restart_checkpoint_path = use_sqlite
        ? "/tmp/agentcore_recorded_effect_restart.sqlite"
        : "/tmp/agentcore_recorded_effect_restart.bin";

    const RunProofDigest direct_digest = run_direct(direct_checkpoint_path);
    const RunProofDigest restarted_digest = run_restarted(restart_checkpoint_path);
    assert(direct_digest.snapshot_digest == restarted_digest.snapshot_digest);
    assert(direct_digest.trace_digest == restarted_digest.trace_digest);
    assert(direct_digest.combined_digest == restarted_digest.combined_digest);
}

void test_recorded_effect_checkpoint_restart_equivalence_with_file_checkpointer() {
    exercise_recorded_effect_restart_equivalence(false);
}

#if defined(AGENTCORE_HAVE_SQLITE3)
void test_recorded_effect_checkpoint_restart_equivalence_with_sqlite_checkpointer() {
    exercise_recorded_effect_restart_equivalence(true);
}
#endif

void test_recorded_effect_request_mismatch_fails_fast() {
    g_recorded_effect_invocations.store(0);
    ExecutionEngine engine(1);
    const GraphDefinition graph = make_recorded_effect_graph(recorded_effect_request_mismatch_node);
    engine.register_graph(graph);

    InputEnvelope input;
    input.initial_field_count = 3;

    const RunId run_id = engine.start(graph, input);
    const StepResult first_step = engine.step(run_id);
    assert(first_step.progressed);
    assert(first_step.node_status == NodeResult::Waiting);
    assert(g_recorded_effect_invocations.load() == 1);

    const ResumeResult resume_result = engine.resume_run(run_id);
    assert(resume_result.resumed);
    const RunResult final_result = engine.run_to_completion(run_id);
    assert(final_result.status == ExecutionStatus::Failed);
    assert(g_recorded_effect_invocations.load() == 1);
}

void test_interrupt_edit_resume_with_file_checkpointer() {
    exercise_interrupt_edit_resume_flow(false);
}

#if defined(AGENTCORE_HAVE_SQLITE3)
void test_interrupt_edit_resume_with_sqlite_checkpointer() {
    exercise_interrupt_edit_resume_flow(true);
}
#endif

} // namespace

} // namespace agentcore

int main() {
    const char* filter_env = std::getenv("AGENTCORE_RUNTIME_TEST_FILTER");
    const std::string_view filter = filter_env == nullptr ? std::string_view{} : std::string_view{filter_env};
    const auto should_run = [&](std::string_view name) {
        return filter.empty() || name.find(filter) != std::string_view::npos;
    };

    struct NamedTest {
        std::string_view name;
        std::function<void()> fn;
    };

    const std::vector<NamedTest> tests = {
        {"test_resume_flow", [] { agentcore::test_resume_flow(); }},
        {"test_fresh_run_snapshot_and_step_preserve_deferred_entry_task", [] {
             agentcore::test_fresh_run_snapshot_and_step_preserve_deferred_entry_task();
         }},
        {"test_knowledge_graph_integration", [] { agentcore::test_knowledge_graph_integration(); }},
        {"test_subgraph_composition_and_public_streaming", [] {
             agentcore::test_subgraph_composition_and_public_streaming();
         }},
        {"test_resumable_subgraph_checkpoint_restart_equivalence", [] {
             agentcore::test_resumable_subgraph_checkpoint_restart_equivalence();
         }},
        {"test_async_subgraph_auto_resume_through_parent_scheduler", [] {
             agentcore::test_async_subgraph_auto_resume_through_parent_scheduler();
         }},
        {"test_async_multi_wait_subgraph_checkpoint_resume_equivalence", [] {
             agentcore::test_async_multi_wait_subgraph_checkpoint_resume_equivalence();
         }},
        {"test_knowledge_graph_reactive_frontier", [] {
             agentcore::test_knowledge_graph_reactive_frontier();
         }},
        {"test_knowledge_graph_duplicate_writes_do_not_retrigger_frontier", [] {
             agentcore::test_knowledge_graph_duplicate_writes_do_not_retrigger_frontier();
         }},
        {"test_knowledge_graph_reactive_frontier_drains_stably", [] {
             agentcore::test_knowledge_graph_reactive_frontier_drains_stably();
         }},
        {"test_knowledge_graph_reactive_checkpoint_resume_preserves_pending_frontier", [] {
             agentcore::test_knowledge_graph_reactive_checkpoint_resume_preserves_pending_frontier();
         }},
        {"test_intelligence_reactive_frontier", [] {
             agentcore::test_intelligence_reactive_frontier();
         }},
        {"test_intelligence_duplicate_writes_do_not_retrigger_frontier", [] {
             agentcore::test_intelligence_duplicate_writes_do_not_retrigger_frontier();
         }},
        {"test_intelligence_claim_graph_reactive_frontier", [] {
             agentcore::test_intelligence_claim_graph_reactive_frontier();
         }},
        {"test_intelligence_reactive_checkpoint_resume_preserves_pending_frontier", [] {
             agentcore::test_intelligence_reactive_checkpoint_resume_preserves_pending_frontier();
         }},
        {"test_parallel_scheduler_fanout", [] { agentcore::test_parallel_scheduler_fanout(); }},
        {"test_join_scope_merges_branch_state_and_knowledge_graph", [] {
             agentcore::test_join_scope_merges_branch_state_and_knowledge_graph();
         }},
        {"test_structural_join_validation", [] { agentcore::test_structural_join_validation(); }},
        {"test_join_runtime_rejects_conflicting_writes_without_rule", [] {
             agentcore::test_join_runtime_rejects_conflicting_writes_without_rule();
         }},
        {"test_hardened_adapters", [] { agentcore::test_hardened_adapters(); }},
        {"test_durable_checkpoint_reload", [] { agentcore::test_durable_checkpoint_reload(); }},
        {"test_async_tool_auto_resume", [] { agentcore::test_async_tool_auto_resume(); }},
        {"test_async_tool_checkpoint_resume_after_restart", [] {
             agentcore::test_async_tool_checkpoint_resume_after_restart();
         }},
        {"test_async_model_snapshot_restore", [] { agentcore::test_async_model_snapshot_restore(); }},
        {"test_proof_digest_determinism_across_parallelism", [] {
             agentcore::test_proof_digest_determinism_across_parallelism();
         }},
        {"test_node_emitted_trace_determinism_across_parallelism", [] {
             agentcore::test_node_emitted_trace_determinism_across_parallelism();
         }},
        {"test_proof_digest_restart_equivalence", [] {
             agentcore::test_proof_digest_restart_equivalence();
         }},
        {"test_checkpoint_cadence_preserves_resumable_restore", [] {
             agentcore::test_checkpoint_cadence_preserves_resumable_restore();
         }},
        {"test_recorded_effect_checkpoint_restart_equivalence_with_file_checkpointer", [] {
             agentcore::test_recorded_effect_checkpoint_restart_equivalence_with_file_checkpointer();
         }},
#if defined(AGENTCORE_HAVE_SQLITE3)
        {"test_recorded_effect_checkpoint_restart_equivalence_with_sqlite_checkpointer", [] {
             agentcore::test_recorded_effect_checkpoint_restart_equivalence_with_sqlite_checkpointer();
         }},
#endif
        {"test_recorded_effect_request_mismatch_fails_fast", [] {
             agentcore::test_recorded_effect_request_mismatch_fails_fast();
         }},
        {"test_interrupt_edit_resume_with_file_checkpointer", [] {
             agentcore::test_interrupt_edit_resume_with_file_checkpointer();
         }},
#if defined(AGENTCORE_HAVE_SQLITE3)
        {"test_interrupt_edit_resume_with_sqlite_checkpointer", [] {
             agentcore::test_interrupt_edit_resume_with_sqlite_checkpointer();
         }},
#endif
    };

    bool ran_any = false;
    for (const NamedTest& test : tests) {
        if (!should_run(test.name)) {
            continue;
        }
        ran_any = true;
        std::cout << "[runtime_test] " << test.name << '\n';
        test.fn();
    }

    if (!ran_any) {
        std::cerr << "runtime test filter matched no tests\n";
        return 1;
    }

    std::cout << "runtime tests passed\n";
    return 0;
}
