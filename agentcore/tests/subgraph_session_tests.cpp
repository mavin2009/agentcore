#include "agentcore/execution/engine.h"
#include "agentcore/execution/proof.h"
#include "agentcore/execution/subgraph/session_runtime.h"
#include "agentcore/graph/graph_ir.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace agentcore {

namespace {

enum PersistentParentStateKey : StateKey {
    kPersistentParentSessionId = 0,
    kPersistentParentInput = 1,
    kPersistentParentRound = 2,
    kPersistentParentOutput = 3,
    kPersistentParentCounter = 4,
    kPersistentParentPreviousKnowledge = 5,
    kPersistentParentReactiveHits = 6,
    kPersistentParentMemoized = 7,
    kPersistentParentTotal = 8,
    kPersistentParentLeftSeen = 9,
    kPersistentParentRightSeen = 10
};

enum PersistentChildStateKey : StateKey {
    kPersistentChildSessionId = 0,
    kPersistentChildInput = 1,
    kPersistentChildOutput = 2,
    kPersistentChildCounter = 3,
    kPersistentChildPreviousKnowledge = 4,
    kPersistentChildReactiveHits = 5,
    kPersistentChildMemoized = 6,
    kPersistentChildWaitAttempt = 7
};

std::atomic<int> g_persistent_session_memo_calls{0};
std::atomic<int> g_persistent_session_mismatch_calls{0};

int64_t read_int_field(const WorkflowState& state, StateKey key) {
    if (key >= state.size()) {
        return 0;
    }
    if (!std::holds_alternative<int64_t>(state.load(key))) {
        return 0;
    }
    return std::get<int64_t>(state.load(key));
}

std::string session_id_from_value(const WorkflowState& state, const StringInterner& strings, StateKey key) {
    if (key >= state.size()) {
        return {};
    }
    const Value& value = state.load(key);
    if (std::holds_alternative<InternedStringId>(value)) {
        return std::string(strings.resolve(std::get<InternedStringId>(value)));
    }
    if (std::holds_alternative<int64_t>(value)) {
        return std::to_string(std::get<int64_t>(value));
    }
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    }
    return {};
}

std::string read_blob_string_field(const StateStore& state_store, StateKey key) {
    const WorkflowState& state = state_store.get_current_state();
    assert(key < state.size());
    assert(std::holds_alternative<BlobRef>(state.load(key)));
    return std::string(state_store.blobs().read_string(std::get<BlobRef>(state.load(key))));
}

NodeResult stop_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult fanout_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult persistent_session_child_node(ExecutionContext& context) {
    const std::string session_id = session_id_from_value(
        context.state,
        context.strings,
        kPersistentChildSessionId
    );
    assert(!session_id.empty());

    const int64_t counter = read_int_field(context.state, kPersistentChildCounter);
    const int64_t input_value = read_int_field(context.state, kPersistentChildInput);
    const std::vector<const KnowledgeTriple*> prior_triples = context.knowledge_graph.match(
        context.strings.intern(session_id),
        context.strings.intern("session_tick"),
        std::nullopt
    );
    const RecordedEffectResult effect = context.record_text_effect_once(
        "persistent-session::memo::" + session_id,
        "request::memo",
        [&]() {
            g_persistent_session_memo_calls.fetch_add(1);
            return std::string("memo::") + session_id;
        }
    );

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentChildCounter, counter + 1});
    patch.updates.push_back(FieldUpdate{kPersistentChildOutput, input_value + counter + 1});
    patch.updates.push_back(FieldUpdate{
        kPersistentChildPreviousKnowledge,
        static_cast<int64_t>(prior_triples.size())
    });
    patch.updates.push_back(FieldUpdate{kPersistentChildMemoized, effect.output});
    patch.knowledge_graph.triples.push_back(KnowledgeTripleWrite{
        context.strings.intern(session_id),
        context.strings.intern("session_tick"),
        context.strings.intern("artifact_" + std::to_string(counter + 1)),
        {}
    });
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult persistent_session_reactive_node(ExecutionContext& context) {
    const int64_t hits = read_int_field(context.state, kPersistentChildReactiveHits);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentChildReactiveHits, hits + 1});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult persistent_session_wait_child_node(ExecutionContext& context) {
    const int64_t wait_attempt = read_int_field(context.state, kPersistentChildWaitAttempt);
    if (wait_attempt == 0) {
        StatePatch patch;
        patch.updates.push_back(FieldUpdate{kPersistentChildWaitAttempt, int64_t{1}});
        return NodeResult::waiting(std::move(patch), 0.84F);
    }

    const int64_t counter = read_int_field(context.state, kPersistentChildCounter);
    const int64_t input_value = read_int_field(context.state, kPersistentChildInput);
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentChildCounter, counter + 1});
    patch.updates.push_back(FieldUpdate{kPersistentChildOutput, input_value + counter + 1});
    return NodeResult::success(std::move(patch), 0.97F);
}

NodeResult persistent_session_mismatch_child_node(ExecutionContext& context) {
    const std::string session_id = session_id_from_value(
        context.state,
        context.strings,
        kPersistentChildSessionId
    );
    assert(!session_id.empty());
    const int64_t input_value = read_int_field(context.state, kPersistentChildInput);
    const RecordedEffectResult effect = context.record_text_effect_once(
        "persistent-session::mismatch::" + session_id,
        "request::" + std::to_string(input_value),
        [&]() {
            g_persistent_session_mismatch_calls.fetch_add(1);
            return std::string("mismatch::") + session_id;
        }
    );

    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentChildMemoized, effect.output});
    patch.updates.push_back(FieldUpdate{kPersistentChildOutput, input_value});
    return NodeResult::success(std::move(patch), 0.96F);
}

NodeResult persistent_session_parent_loop_node(ExecutionContext&) {
    return NodeResult::success();
}

NodeResult persistent_session_parent_revisit_node(ExecutionContext& context) {
    const int64_t round = read_int_field(context.state, kPersistentParentRound);
    if (round == 0) {
        StatePatch patch;
        patch.updates.push_back(FieldUpdate{kPersistentParentRound, int64_t{1}});
        NodeResult result = NodeResult::success(std::move(patch), 0.98F);
        result.next_override = 1U;
        return result;
    }
    return NodeResult::success({}, 0.98F);
}

NodeResult persistent_session_parent_revisit_with_input_change_node(ExecutionContext& context) {
    const int64_t round = read_int_field(context.state, kPersistentParentRound);
    if (round == 0) {
        StatePatch patch;
        patch.updates.push_back(FieldUpdate{kPersistentParentRound, int64_t{1}});
        patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{2}});
        NodeResult result = NodeResult::success(std::move(patch), 0.98F);
        result.next_override = 1U;
        return result;
    }
    return NodeResult::success({}, 0.98F);
}

NodeResult prepare_left_session_node(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentParentSessionId, int64_t{101}});
    patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{10}});
    patch.updates.push_back(FieldUpdate{kPersistentParentLeftSeen, true});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult prepare_right_session_node(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentParentSessionId, int64_t{202}});
    patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{20}});
    patch.updates.push_back(FieldUpdate{kPersistentParentRightSeen, true});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult prepare_shared_left_session_node(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentParentSessionId, int64_t{303}});
    patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{10}});
    patch.updates.push_back(FieldUpdate{kPersistentParentLeftSeen, true});
    return NodeResult::success(std::move(patch), 0.95F);
}

NodeResult prepare_shared_right_session_node(ExecutionContext& context) {
    StatePatch patch;
    patch.updates.push_back(FieldUpdate{kPersistentParentSessionId, int64_t{303}});
    patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{20}});
    patch.updates.push_back(FieldUpdate{kPersistentParentRightSeen, true});
    return NodeResult::success(std::move(patch), 0.95F);
}

GraphDefinition make_persistent_session_child_graph(GraphId graph_id, NodeExecutorFn executor) {
    GraphDefinition graph;
    graph.id = graph_id;
    graph.name = "persistent_session_child";
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Compute,
            "session_child_step",
            0U,
            0U,
            0U,
            executor,
            {}
        },
        NodeDefinition{
            2U,
            NodeKind::Control,
            "session_child_stop",
            node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            stop_node,
            {}
        },
        NodeDefinition{
            3U,
            NodeKind::Aggregate,
            "session_child_reactive",
            node_policy_mask(NodePolicyFlag::ReactToKnowledgeGraph) |
                node_policy_mask(NodePolicyFlag::StopAfterNode),
            0U,
            0U,
            persistent_session_reactive_node,
            {},
            {},
            std::vector<KnowledgeSubscription>{
                KnowledgeSubscription{
                    KnowledgeSubscriptionKind::TriplePattern,
                    {},
                    {},
                    "session_tick",
                    {}
                }
            }
        }
    };
    graph.edges = {
        EdgeDefinition{1U, 1U, 2U, EdgeKind::OnSuccess, nullptr, 100U}
    };
    graph.bind_outgoing_edges(1U, std::vector<EdgeId>{1U});
    graph.sort_edges_by_priority();
    return graph;
}

GraphDefinition make_persistent_session_parent_graph(GraphId child_graph_id, NodeExecutorFn revisit_executor) {
    GraphDefinition graph;
    graph.id = 961U;
    graph.name = "persistent_session_parent";
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
                    SubgraphStateBinding{kPersistentParentSessionId, kPersistentChildSessionId},
                    SubgraphStateBinding{kPersistentParentInput, kPersistentChildInput}
                },
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kPersistentParentOutput, kPersistentChildOutput},
                    SubgraphStateBinding{kPersistentParentCounter, kPersistentChildCounter},
                    SubgraphStateBinding{kPersistentParentPreviousKnowledge, kPersistentChildPreviousKnowledge},
                    SubgraphStateBinding{kPersistentParentReactiveHits, kPersistentChildReactiveHits},
                    SubgraphStateBinding{kPersistentParentMemoized, kPersistentChildMemoized}
                },
                true,
                8U,
                SubgraphSessionMode::Persistent,
                kPersistentParentSessionId
            }
        },
        NodeDefinition{
            2U,
            NodeKind::Control,
            "revisit_or_finish",
            0U,
            0U,
            0U,
            revisit_executor,
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

GraphDefinition make_parallel_persistent_parent_graph(
    GraphId child_graph_id,
    NodeExecutorFn left_prepare,
    NodeExecutorFn right_prepare,
    GraphId graph_id,
    std::string_view graph_name
) {
    GraphDefinition graph;
    graph.id = graph_id;
    graph.name = std::string(graph_name);
    graph.entry = 1U;
    graph.nodes = {
        NodeDefinition{
            1U,
            NodeKind::Control,
            "fanout_sessions",
            node_policy_mask(NodePolicyFlag::AllowFanOut),
            0U,
            0U,
            fanout_node,
            {}
        },
        NodeDefinition{2U, NodeKind::Compute, "prepare_left", 0U, 0U, 0U, left_prepare, {}},
        NodeDefinition{3U, NodeKind::Compute, "prepare_right", 0U, 0U, 0U, right_prepare, {}},
        NodeDefinition{
            4U,
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
                    SubgraphStateBinding{kPersistentParentSessionId, kPersistentChildSessionId},
                    SubgraphStateBinding{kPersistentParentInput, kPersistentChildInput}
                },
                std::vector<SubgraphStateBinding>{
                    SubgraphStateBinding{kPersistentParentOutput, kPersistentChildOutput},
                    SubgraphStateBinding{kPersistentParentCounter, kPersistentChildCounter},
                    SubgraphStateBinding{kPersistentParentPreviousKnowledge, kPersistentChildPreviousKnowledge},
                    SubgraphStateBinding{kPersistentParentReactiveHits, kPersistentChildReactiveHits}
                },
                true,
                8U,
                SubgraphSessionMode::Persistent,
                kPersistentParentSessionId
            }
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

std::set<std::string> collect_committed_session_ids(const RunSnapshot& snapshot) {
    std::set<std::string> ids;
    for (const CommittedSubgraphSessionSnapshot& session : snapshot.committed_subgraph_sessions) {
        ids.insert(session.session_id);
    }
    return ids;
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

void test_persistent_subgraph_session_reuse_and_stream_metadata() {
    g_persistent_session_memo_calls.store(0);

    ExecutionEngine engine(4);
    engine.register_graph(make_persistent_session_child_graph(960U, persistent_session_child_node));

    InputEnvelope input;
    input.initial_field_count = 11U;
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentSessionId, int64_t{101}});
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{5}});
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentRound, int64_t{0}});

    StateStore session_probe_state(11U);
    static_cast<void>(session_probe_state.apply(input.initial_patch));
    const GraphDefinition parent_graph =
        make_persistent_session_parent_graph(960U, persistent_session_parent_revisit_node);
    assert(parent_graph.nodes.front().subgraph.has_value());
    assert(parent_graph.nodes.front().subgraph->session_mode == SubgraphSessionMode::Persistent);
    assert(parent_graph.nodes.front().subgraph->session_id_source_key.has_value());
    const std::optional<std::string> resolved_probe_session_id = resolve_subgraph_session_id(
        session_probe_state,
        *parent_graph.nodes.front().subgraph,
        nullptr
    );
    assert(resolved_probe_session_id.has_value());
    assert(*resolved_probe_session_id == "101");

    const RunId run_id = engine.start(parent_graph, input);
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const StateStore& state_store = engine.state_store(run_id);
    const WorkflowState& state = state_store.get_current_state();
    const RunSnapshot snapshot = engine.inspect(run_id);
    assert(read_int_field(state, kPersistentParentOutput) == 7);
    assert(read_int_field(state, kPersistentParentCounter) == 2);
    assert(read_int_field(state, kPersistentParentPreviousKnowledge) == 1);
    assert(read_int_field(state, kPersistentParentReactiveHits) == 2);
    assert(read_blob_string_field(state_store, kPersistentParentMemoized) == "memo::101");
    assert(g_persistent_session_memo_calls.load() == 1);

    assert(snapshot.committed_subgraph_sessions.size() == 1U);
    assert(snapshot.committed_subgraph_sessions.front().session_id == "101");
    assert(snapshot.committed_subgraph_sessions.front().session_revision == 2U);
    assert(!snapshot.committed_subgraph_sessions.front().snapshot_bytes.empty());

    StreamCursor cursor;
    const std::vector<StreamEvent> stream_events = engine.stream_events(run_id, cursor, StreamReadOptions{true});
    std::set<uint64_t> seen_revisions;
    std::size_t namespaced_events = 0U;
    for (const StreamEvent& event : stream_events) {
        if (event.namespaces.empty()) {
            continue;
        }
        ++namespaced_events;
        assert(event.session_id == "101");
        assert(event.namespaces.front().session_id == "101");
        assert(event.namespaces.front().node_name == "persistent_specialist");
        seen_revisions.insert(event.session_revision);
    }
    assert(namespaced_events >= 2U);
    const std::set<uint64_t> expected_revisions{1U, 2U};
    assert(seen_revisions == expected_revisions);

    const auto record = engine.checkpoints().get(result.last_checkpoint_id);
    assert(record.has_value());
    assert(record->resumable());
    const RunProofDigest proof = compute_run_proof_digest(*record, engine.trace().events_for_run(run_id));
    assert(proof.combined_digest != 0U);
}

void test_parallel_persistent_subgraph_sessions() {
    ExecutionEngine engine(4);
    engine.register_graph(make_persistent_session_child_graph(962U, persistent_session_child_node));

    InputEnvelope input;
    input.initial_field_count = 11U;

    const RunId run_id = engine.start(
        make_parallel_persistent_parent_graph(
            962U,
            prepare_left_session_node,
            prepare_right_session_node,
            963U,
            "parallel_persistent_sessions"
        ),
        input
    );
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Completed);

    const RunSnapshot snapshot = engine.inspect(run_id);
    assert(snapshot.committed_subgraph_sessions.size() == 2U);
    const std::set<std::string> expected_session_ids{"101", "202"};
    assert(collect_committed_session_ids(snapshot) == expected_session_ids);
    assert(snapshot.branches.size() == 2U);

    std::set<int64_t> output_values;
    std::set<int64_t> reactive_hits;
    for (const BranchSnapshot& branch : snapshot.branches) {
        const WorkflowState& branch_state = branch.state_store.get_current_state();
        output_values.insert(read_int_field(branch_state, kPersistentParentOutput));
        reactive_hits.insert(read_int_field(branch_state, kPersistentParentReactiveHits));
    }
    const std::set<int64_t> expected_outputs{11, 21};
    assert(output_values == expected_outputs);
    assert(reactive_hits == std::set<int64_t>{1});

    StreamCursor cursor;
    const std::vector<StreamEvent> stream_events = engine.stream_events(run_id, cursor, StreamReadOptions{true});
    std::set<std::string> namespaced_session_ids;
    for (const StreamEvent& event : stream_events) {
        if (!event.namespaces.empty()) {
            namespaced_session_ids.insert(event.session_id);
        }
    }
    const std::set<std::string> expected_namespaced_session_ids{"101", "202"};
    assert(namespaced_session_ids == expected_namespaced_session_ids);
}

void test_parallel_persistent_subgraph_session_digest_determinism() {
    auto run_digest_once = []() {
        ExecutionEngine engine(4);
        engine.register_graph(make_persistent_session_child_graph(968U, persistent_session_child_node));

        InputEnvelope input;
        input.initial_field_count = 11U;

        const RunId run_id = engine.start(
            make_parallel_persistent_parent_graph(
                968U,
                prepare_left_session_node,
                prepare_right_session_node,
                969U,
                "parallel_persistent_session_digest_determinism"
            ),
            input
        );
        const RunResult result = engine.run_to_completion(run_id);
        assert(result.status == ExecutionStatus::Completed);

        const auto record = engine.checkpoints().get(result.last_checkpoint_id);
        assert(record.has_value());
        assert(record->resumable());
        return compute_run_proof_digest(*record, engine.trace().events_for_run(run_id));
    };

    const RunProofDigest expected = run_digest_once();
    assert(expected.combined_digest != 0U);
    for (std::size_t iteration = 0; iteration < 4U; ++iteration) {
        const RunProofDigest observed = run_digest_once();
        assert(observed.snapshot_digest == expected.snapshot_digest);
        assert(observed.trace_digest == expected.trace_digest);
        assert(observed.combined_digest == expected.combined_digest);
    }
}

void test_parallel_persistent_subgraph_same_session_rejected() {
    ExecutionEngine engine(4);
    engine.register_graph(make_persistent_session_child_graph(964U, persistent_session_wait_child_node));

    InputEnvelope input;
    input.initial_field_count = 11U;

    const RunId run_id = engine.start(
        make_parallel_persistent_parent_graph(
            964U,
            prepare_shared_left_session_node,
            prepare_shared_right_session_node,
            965U,
            "parallel_persistent_same_session"
        ),
        input
    );
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Failed);
}

void test_persistent_subgraph_session_recorded_effect_mismatch() {
    g_persistent_session_mismatch_calls.store(0);

    ExecutionEngine engine(2);
    engine.register_graph(make_persistent_session_child_graph(966U, persistent_session_mismatch_child_node));

    InputEnvelope input;
    input.initial_field_count = 11U;
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentSessionId, int64_t{999}});
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{1}});
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentRound, int64_t{0}});

    const RunId run_id = engine.start(
        make_persistent_session_parent_graph(966U, persistent_session_parent_revisit_with_input_change_node),
        input
    );
    const RunResult result = engine.run_to_completion(run_id);
    assert(result.status == ExecutionStatus::Failed);
    assert(g_persistent_session_mismatch_calls.load() == 1);

    const RunSnapshot snapshot = engine.inspect(run_id);
    assert(snapshot.committed_subgraph_sessions.size() == 1U);
    assert(snapshot.committed_subgraph_sessions.front().session_id == "999");
    assert(snapshot.committed_subgraph_sessions.front().session_revision == 1U);
}

void test_persistent_subgraph_session_resume_equivalence() {
    const GraphDefinition child_graph =
        make_persistent_session_child_graph(967U, persistent_session_wait_child_node);
    const GraphDefinition parent_graph =
        make_persistent_session_parent_graph(967U, persistent_session_parent_loop_node);

    InputEnvelope input;
    input.initial_field_count = 11U;
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentSessionId, int64_t{111}});
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentInput, int64_t{41}});
    input.initial_patch.updates.push_back(FieldUpdate{kPersistentParentRound, int64_t{1}});

    ExecutionEngine direct_engine(2);
    direct_engine.register_graph(child_graph);
    const RunId direct_run_id = direct_engine.start(parent_graph, input);
    const RunResult direct_wait = direct_engine.run_to_completion(direct_run_id);
    assert(direct_wait.status == ExecutionStatus::Paused);
    const ResumeResult direct_resume = direct_engine.resume_run(direct_run_id);
    assert(direct_resume.resumed);
    const RunResult direct_result = direct_engine.run_to_completion(direct_run_id);
    assert(direct_result.status == ExecutionStatus::Completed);

    const auto direct_record = direct_engine.checkpoints().get(direct_result.last_checkpoint_id);
    assert(direct_record.has_value() && direct_record->resumable());
    const RunProofDigest direct_proof =
        compute_run_proof_digest(
            *direct_record,
            normalize_trace_for_resume_proof(direct_engine.trace().events_for_run(direct_run_id))
        );

    const std::string checkpoint_path = "/tmp/agentcore_persistent_subgraph_session_resume.bin";
    std::remove(checkpoint_path.c_str());

    ExecutionEngine persisted_engine(2);
    persisted_engine.register_graph(child_graph);
    persisted_engine.enable_checkpoint_persistence(checkpoint_path);
    const RunId persisted_run_id = persisted_engine.start(parent_graph, input);
    const RunResult persisted_wait = persisted_engine.run_to_completion(persisted_run_id);
    assert(persisted_wait.status == ExecutionStatus::Paused);

    ExecutionEngine restored_engine(2);
    restored_engine.register_graph(parent_graph);
    restored_engine.register_graph(child_graph);
    restored_engine.enable_checkpoint_persistence(checkpoint_path);
    assert(restored_engine.load_persisted_checkpoints() >= 1U);

    const auto persisted_wait_record = persisted_engine.checkpoints().get(persisted_wait.last_checkpoint_id);
    assert(persisted_wait_record.has_value());
    assert(persisted_wait_record->snapshot.has_value());
    assert(!persisted_wait_record->snapshot->branches.empty());
    assert(persisted_wait_record->snapshot->branches.front().pending_subgraph.has_value());
    assert(persisted_wait_record->snapshot->branches.front().pending_subgraph->session_id == "111");
    assert(persisted_wait_record->snapshot->branches.front().pending_subgraph->session_revision == 1U);

    const ResumeResult restored = restored_engine.resume(persisted_wait.last_checkpoint_id);
    assert(restored.resumed);
    const RunResult restored_result = restored_engine.run_to_completion(restored.run_id);
    assert(restored_result.status == ExecutionStatus::Completed);

    const auto restored_record = restored_engine.checkpoints().get(restored_result.last_checkpoint_id);
    assert(restored_record.has_value() && restored_record->resumable());
    const std::vector<TraceEvent> prefix_trace = trace_events_through_checkpoint(
        persisted_engine.trace().events_for_run(persisted_run_id),
        persisted_wait.last_checkpoint_id
    );
    const RunProofDigest restored_proof =
        compute_run_proof_digest(
            *restored_record,
            normalize_trace_for_resume_proof(
                append_trace_events(
                    prefix_trace,
                    restored_engine.trace().events_for_run(restored.run_id)
                )
            )
        );

    assert(read_int_field(direct_engine.state(direct_run_id), kPersistentParentOutput) == 42);
    assert(read_int_field(restored_engine.state(restored.run_id), kPersistentParentOutput) == 42);
    assert(direct_proof.combined_digest == restored_proof.combined_digest);

    const RunSnapshot restored_snapshot = restored_engine.inspect(restored.run_id);
    assert(restored_snapshot.committed_subgraph_sessions.size() == 1U);
    assert(restored_snapshot.committed_subgraph_sessions.front().session_id == "111");
    assert(restored_snapshot.committed_subgraph_sessions.front().session_revision == 1U);

    std::remove(checkpoint_path.c_str());
}

} // namespace

} // namespace agentcore

int main() {
    agentcore::test_persistent_subgraph_session_reuse_and_stream_metadata();
    agentcore::test_parallel_persistent_subgraph_sessions();
    agentcore::test_parallel_persistent_subgraph_session_digest_determinism();
    agentcore::test_parallel_persistent_subgraph_same_session_rejected();
    agentcore::test_persistent_subgraph_session_recorded_effect_mismatch();
    agentcore::test_persistent_subgraph_session_resume_equivalence();
    std::cout << "subgraph session tests passed\n";
    return 0;
}
