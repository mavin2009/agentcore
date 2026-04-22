import asyncio

from agentcore.graph import (
    Command,
    END,
    START,
    IntelligenceRouter,
    IntelligenceRule,
    IntelligenceSubscription,
    RuntimeContext,
    StateGraph,
)


async def planner(state, config):
    history = list(state.get("history", []))
    next_count = int(state.get("count", 0)) + 1
    history.append(f"step-{next_count}")
    payload = dict(state.get("payload", {}))
    payload["last"] = history[-1]
    configurable = dict(config.get("configurable", {}))
    await asyncio.sleep(0)
    return {
        "count": next_count,
        "history": history,
        "payload": payload,
        "run_label": configurable.get("run_label", "unset"),
        "tag_count": len(config.get("tags", [])),
    }


async def route(state, config):
    await asyncio.sleep(0)
    target_count = int(dict(config.get("configurable", {})).get("target_count", 2))
    return END if state["count"] >= target_count else "planner"


graph = StateGraph(dict, name="python_state_graph_api_smoke", worker_count=2)
graph.add_node("planner", planner)
graph.add_edge(START, "planner")
graph.add_conditional_edges("planner", route, {END: END, "planner": "planner"})
compiled = graph.compile()

final_state = compiled.invoke(
    {
        "count": 0,
        "history": [],
        "payload": {"seed": True},
        "binary": b"agentcore",
    },
    config={
        "configurable": {"run_label": "sync", "target_count": 2},
        "tags": ["smoke", "sync"],
    },
)
print(f"DEBUG: final_state['count'] = {final_state.get('count')}")
assert final_state["count"] == 2
assert final_state["history"] == ["step-1", "step-2"]
assert final_state["payload"] == {"seed": True, "last": "step-2"}
assert final_state["binary"] == b"agentcore"
assert final_state["run_label"] == "sync"
assert final_state["tag_count"] == 2

details = compiled.invoke_with_metadata(
    {"count": 0, "history": [], "payload": {}},
    config={"configurable": {"run_label": "details", "target_count": 2}, "tags": ["meta"]},
)
assert details["summary"]["status"] == "completed"
assert details["summary"]["steps_executed"] >= 2
assert details["proof"]["combined_digest"] != 0
assert [event["node_name"] for event in details["trace"]] == ["planner", "planner"]

events = list(
    compiled.stream(
        {"count": 0, "history": [], "payload": {}},
        config={"configurable": {"run_label": "stream", "target_count": 2}, "tags": ["stream"]},
    )
)
assert [event["node_name"] for event in events] == ["planner", "planner"]
for event in events:
    print(f"DEBUG: event['node_name']={event.get('node_name')}, event['graph_name']='{event.get('graph_name')}'")
assert all(event["graph_name"] == "python_state_graph_api_smoke" for event in events)

batch_results = compiled.batch(
    [{"count": 0, "history": [], "payload": {}}, {"count": 0, "history": [], "payload": {}}],
    config=[
        {"configurable": {"run_label": "batch-a", "target_count": 2}, "tags": ["batch"]},
        {"configurable": {"run_label": "batch-b", "target_count": 3}, "tags": ["batch"]},
    ],
)
assert [result["count"] for result in batch_results] == [2, 3]
assert [result["run_label"] for result in batch_results] == ["batch-a", "batch-b"]


def write_intelligence(state, config, runtime: RuntimeContext):
    intelligence = runtime.intelligence

    snapshot_before = intelligence.snapshot()
    assert snapshot_before["counts"] == {
        "tasks": 0,
        "claims": 0,
        "evidence": 0,
        "decisions": 0,
        "memories": 0,
    }

    assert intelligence.summary()["counts"]["tasks"] == 0

    intelligence.upsert_task(
        "task:draft-answer",
        title="Draft answer",
        owner="planner",
        details={"question": state.get("question", "unknown")},
        status="in_progress",
        priority=3,
        confidence=0.6,
    )
    intelligence.upsert_task(
        "task:urgent-followup",
        title="Urgent follow-up",
        owner="planner",
        details={"question": state.get("question", "unknown"), "priority": "urgent"},
        status="open",
        priority=1,
        confidence=0.95,
    )
    intelligence.upsert_task(
        "task:review-summary",
        title="Review summary",
        owner="reviewer",
        details={"question": state.get("question", "unknown")},
        status="completed",
        priority=5,
        confidence=0.7,
    )
    intelligence.upsert_claim(
        "claim:agentcore-native",
        subject="agentcore",
        relation="is",
        object="native_runtime",
        statement={"text": "AgentCore is a native runtime"},
        status="supported",
        confidence=0.95,
    )
    intelligence.add_evidence(
        "evidence:runtime-benchmark",
        kind="benchmark",
        source="state_graph_api_smoke",
        content={"speedup_x": 2.0},
        task_key="task:draft-answer",
        claim_key="claim:agentcore-native",
        confidence=0.8,
    )
    intelligence.record_decision(
        "decision:publish-summary",
        task_key="task:draft-answer",
        claim_key="claim:agentcore-native",
        summary={"action": "document"},
        status="selected",
        confidence=0.7,
    )
    intelligence.remember(
        "memory:semantic:agentcore-native",
        layer="semantic",
        scope="runtime",
        content={"fact": "AgentCore is native"},
        task_key="task:draft-answer",
        claim_key="claim:agentcore-native",
        importance=0.75,
    )
    intelligence.remember(
        "memory:semantic:low-priority",
        layer="semantic",
        scope="runtime",
        content={"fact": "Low priority memory"},
        task_key="task:review-summary",
        claim_key="claim:agentcore-native",
        importance=0.2,
    )
    intelligence.remember(
        "memory:semantic:high-priority",
        layer="semantic",
        scope="runtime",
        content={"fact": "High priority memory"},
        task_key="task:urgent-followup",
        claim_key="claim:agentcore-native",
        importance=0.95,
    )

    snapshot_after = intelligence.snapshot()
    assert snapshot_after["counts"]["tasks"] == 3
    assert snapshot_after["counts"]["claims"] == 1
    assert snapshot_after["counts"]["evidence"] == 1
    assert snapshot_after["counts"]["decisions"] == 1
    assert snapshot_after["counts"]["memories"] == 3

    in_progress_tasks = intelligence.query(kind="tasks", status="in_progress")
    assert in_progress_tasks["counts"]["tasks"] == 1
    assert in_progress_tasks["tasks"][0]["key"] == "task:draft-answer"

    semantic_claims = intelligence.query(
        kind="claims",
        subject="agentcore",
        relation="is",
        object="native_runtime",
    )
    assert semantic_claims["counts"]["claims"] == 1
    assert semantic_claims["claims"][0]["key"] == "claim:agentcore-native"
    assert intelligence.count(
        kind="claims",
        subject="agentcore",
        relation="is",
        object="native_runtime",
    ) == 1

    semantic_memories = intelligence.query(kind="memories", layer="semantic", scope="runtime")
    assert semantic_memories["counts"]["memories"] == 3
    assert [memory["key"] for memory in semantic_memories["memories"]] == [
        "memory:semantic:agentcore-native",
        "memory:semantic:low-priority",
        "memory:semantic:high-priority",
    ]

    related_to_task = intelligence.related(task_key="task:draft-answer")
    assert related_to_task["counts"] == {
        "tasks": 1,
        "claims": 1,
        "evidence": 1,
        "decisions": 1,
        "memories": 1,
    }

    planner_agenda = intelligence.agenda(owner="planner")
    assert planner_agenda["counts"]["tasks"] == 2
    assert planner_agenda["tasks"][0]["key"] == "task:draft-answer"
    assert planner_agenda["tasks"][1]["key"] == "task:urgent-followup"

    next_planner_task = intelligence.next_task(owner="planner")
    assert next_planner_task is not None
    assert next_planner_task["key"] == "task:draft-answer"

    open_planner_agenda = intelligence.agenda(owner="planner", status="open")
    assert open_planner_agenda["counts"]["tasks"] == 1
    assert open_planner_agenda["tasks"][0]["key"] == "task:urgent-followup"

    recalled_memories = intelligence.recall(scope="runtime", layer="semantic")
    assert recalled_memories["counts"]["memories"] == 3
    assert [memory["key"] for memory in recalled_memories["memories"]] == [
        "memory:semantic:high-priority",
        "memory:semantic:agentcore-native",
        "memory:semantic:low-priority",
    ]

    focus = intelligence.focus(owner="planner", scope="runtime", limit=3)
    assert focus["counts"] == {
        "tasks": 3,
        "claims": 1,
        "evidence": 1,
        "decisions": 1,
        "memories": 3,
    }
    assert [task["key"] for task in focus["tasks"]] == [
        "task:draft-answer",
        "task:urgent-followup",
        "task:review-summary",
    ]
    assert focus["claims"][0]["key"] == "claim:agentcore-native"
    assert focus["memories"][0]["key"] == "memory:semantic:high-priority"

    semantic_focus = intelligence.focus(
        subject="agentcore",
        relation="is",
        object="native_runtime",
        limit=3,
    )
    assert semantic_focus["counts"] == {
        "tasks": 3,
        "claims": 1,
        "evidence": 1,
        "decisions": 1,
        "memories": 3,
    }
    assert semantic_focus["claims"][0]["key"] == "claim:agentcore-native"

    return {"intelligence_written": True}


def read_intelligence(state, config, runtime: RuntimeContext):
    assert state.get("intelligence_written") is True
    snapshot = runtime.intelligence.snapshot()
    assert snapshot["counts"]["tasks"] == 3
    assert snapshot["tasks"][0]["title"] == "Draft answer"
    assert snapshot["claims"][0]["subject"] == "agentcore"
    assert snapshot["evidence"][0]["content"]["speedup_x"] == 2.0
    assert snapshot["decisions"][0]["status"] == "selected"
    assert snapshot["memories"][0]["layer"] == "semantic"
    assert runtime.intelligence.summary()["decision_status"]["selected"] == 1
    assert runtime.intelligence.next_task(owner="planner")["key"] == "task:draft-answer"
    assert runtime.intelligence.recall(scope="runtime", limit=1)["memories"][0]["key"] == "memory:semantic:high-priority"
    assert runtime.intelligence.focus(owner="planner", scope="runtime", limit=2)["tasks"][0]["key"] == "task:draft-answer"
    return {
        "intelligence_written": True,
        "intelligence_task_status": snapshot["tasks"][0]["status"],
        "intelligence_memory_count": snapshot["counts"]["memories"],
        "intelligence_task_count": snapshot["counts"]["tasks"],
        "intelligence_decision_status": snapshot["decisions"][0]["status"],
    }


intelligence_graph = StateGraph(dict, name="python_intelligence_runtime_smoke", worker_count=2)
intelligence_graph.add_node("write", write_intelligence)
intelligence_graph.add_node("read", read_intelligence)
intelligence_graph.add_edge(START, "write")
intelligence_graph.add_edge("write", "read")
intelligence_graph.add_edge("read", END)
intelligence_compiled = intelligence_graph.compile()

intelligence_details = intelligence_compiled.invoke_with_metadata({"question": "what is AgentCore?"})
assert intelligence_details["state"].get("intelligence_written") is True
assert intelligence_details["state"].get("intelligence_task_status") == "in_progress"
assert intelligence_details["state"].get("intelligence_memory_count") == 3
assert intelligence_details["state"].get("intelligence_task_count") == 3
assert intelligence_details["state"].get("intelligence_decision_status") == "selected"
assert intelligence_details["intelligence"]["counts"] == {
    "tasks": 3,
    "claims": 1,
    "evidence": 1,
    "decisions": 1,
    "memories": 3,
}
assert intelligence_details["intelligence"]["tasks"][0]["title"] == "Draft answer"
assert intelligence_details["intelligence"]["evidence"][0]["content"]["speedup_x"] == 2.0


def validate_supporting_claims(state, config, runtime: RuntimeContext):
    intelligence = runtime.intelligence

    intelligence.upsert_task(
        "task:supporting-claims",
        title="Supporting claims task",
        owner="support-owner",
        status="in_progress",
        priority=4,
        confidence=0.8,
    )
    intelligence.upsert_claim(
        "claim:supporting:strong",
        subject="agentcore",
        relation="supports",
        object="supporting-claim-ranking",
        status="supported",
        confidence=0.9,
    )
    intelligence.upsert_claim(
        "claim:supporting:weak",
        subject="agentcore",
        relation="supports",
        object="supporting-claim-ranking",
        status="supported",
        confidence=0.7,
    )
    intelligence.add_evidence(
        "evidence:supporting:strong",
        kind="benchmark",
        source="supporting-claims",
        task_key="task:supporting-claims",
        claim_key="claim:supporting:strong",
        content={"strength": "strong"},
        confidence=0.96,
    )
    intelligence.record_decision(
        "decision:supporting:strong",
        task_key="task:supporting-claims",
        claim_key="claim:supporting:strong",
        summary={"selected": True},
        status="selected",
        confidence=0.94,
    )
    intelligence.remember(
        "memory:supporting:strong",
        layer="semantic",
        scope="supporting-claims",
        task_key="task:supporting-claims",
        claim_key="claim:supporting:strong",
        content={"memory": "strong"},
        importance=0.9,
    )
    intelligence.add_evidence(
        "evidence:supporting:weak",
        kind="observation",
        source="supporting-claims",
        task_key="task:supporting-claims",
        claim_key="claim:supporting:weak",
        content={"strength": "weak"},
        confidence=0.45,
    )

    ranked_task_claims = intelligence.supporting_claims(
        task_key="task:supporting-claims",
        limit=2,
    )
    assert ranked_task_claims["counts"]["claims"] == 2
    assert [claim["key"] for claim in ranked_task_claims["claims"]] == [
        "claim:supporting:strong",
        "claim:supporting:weak",
    ]

    ranked_semantic_claims = intelligence.supporting_claims(
        subject="agentcore",
        relation="supports",
        object="supporting-claim-ranking",
        limit=2,
    )
    assert ranked_semantic_claims["counts"]["claims"] == 2
    assert ranked_semantic_claims["claims"][0]["key"] == "claim:supporting:strong"
    assert ranked_semantic_claims["claims"][1]["key"] == "claim:supporting:weak"
    return {"best_supporting_claim": ranked_semantic_claims["claims"][0]["key"]}


supporting_claims_graph = StateGraph(dict, name="python_supporting_claims_smoke", worker_count=2)
supporting_claims_graph.add_node("rank", validate_supporting_claims)
supporting_claims_graph.add_edge(START, "rank")
supporting_claims_graph.add_edge("rank", END)
supporting_claims_details = supporting_claims_graph.compile().invoke_with_metadata({})
assert supporting_claims_details["state"]["best_supporting_claim"] == "claim:supporting:strong"
assert supporting_claims_details["intelligence"]["claims"][0]["key"] == "claim:supporting:strong"


def validate_action_candidates(state, config, runtime: RuntimeContext):
    intelligence = runtime.intelligence

    intelligence.upsert_task(
        "task:action:strong",
        title="Strong action",
        owner="action-owner",
        status="open",
        priority=4,
        confidence=0.8,
    )
    intelligence.upsert_task(
        "task:action:weak",
        title="Weak action",
        owner="action-owner",
        status="open",
        priority=4,
        confidence=0.8,
    )
    intelligence.upsert_claim(
        "claim:action:strong",
        subject="agentcore",
        relation="supports",
        object="action-ranking",
        status="supported",
        confidence=0.9,
    )
    intelligence.upsert_claim(
        "claim:action:weak",
        subject="agentcore",
        relation="supports",
        object="action-ranking",
        status="supported",
        confidence=0.7,
    )
    intelligence.add_evidence(
        "evidence:action:strong",
        kind="benchmark",
        source="action-candidates",
        task_key="task:action:strong",
        claim_key="claim:action:strong",
        content={"strength": "strong"},
        confidence=0.95,
    )
    intelligence.record_decision(
        "decision:action:strong",
        task_key="task:action:strong",
        claim_key="claim:action:strong",
        summary={"selected": True},
        status="selected",
        confidence=0.94,
    )
    intelligence.remember(
        "memory:action:strong",
        layer="semantic",
        scope="action-ranking",
        task_key="task:action:strong",
        claim_key="claim:action:strong",
        content={"memory": "strong"},
        importance=0.9,
    )
    intelligence.add_evidence(
        "evidence:action:weak",
        kind="benchmark",
        source="action-candidates",
        task_key="task:action:weak",
        claim_key="claim:action:weak",
        content={"strength": "weak"},
        confidence=0.42,
    )

    ranked_actions = intelligence.action_candidates(
        owner="action-owner",
        subject="agentcore",
        relation="supports",
        object="action-ranking",
        limit=2,
    )
    assert ranked_actions["counts"]["tasks"] == 2
    assert [task["key"] for task in ranked_actions["tasks"]] == [
        "task:action:strong",
        "task:action:weak",
    ]
    return {"best_action_candidate": ranked_actions["tasks"][0]["key"]}


action_candidates_graph = StateGraph(dict, name="python_action_candidates_smoke", worker_count=2)
action_candidates_graph.add_node("rank", validate_action_candidates)
action_candidates_graph.add_edge(START, "rank")
action_candidates_graph.add_edge("rank", END)
action_candidates_details = action_candidates_graph.compile().invoke_with_metadata({})
assert action_candidates_details["state"]["best_action_candidate"] == "task:action:strong"
assert action_candidates_details["intelligence"]["tasks"][0]["key"] == "task:action:strong"


def validate_intelligence_related_hops(state, config, runtime: RuntimeContext):
    intelligence = runtime.intelligence

    intelligence.upsert_task(
        "task:related:seed",
        title="Related seed task",
        owner="related-owner",
        details={"kind": "seed"},
        status="in_progress",
        priority=2,
        confidence=0.7,
    )
    intelligence.upsert_task(
        "task:related:second",
        title="Related second task",
        owner="related-owner",
        details={"kind": "second"},
        status="open",
        priority=1,
        confidence=0.65,
    )
    intelligence.upsert_claim(
        "claim:related:shared",
        subject="agentcore",
        relation="supports",
        object="related-hops",
        statement={"edge": "shared"},
        status="supported",
        confidence=0.8,
    )
    intelligence.add_evidence(
        "evidence:related:seed",
        kind="bridge",
        source="related-hops",
        content={"edge": "seed"},
        task_key="task:related:seed",
        claim_key="claim:related:shared",
        confidence=0.7,
    )
    intelligence.remember(
        "memory:related:second",
        layer="semantic",
        scope="related-scope",
        content={"edge": "second"},
        task_key="task:related:second",
        claim_key="claim:related:shared",
        importance=0.6,
    )

    hop1 = intelligence.related(task_key="task:related:seed", hops=1)
    assert [task["key"] for task in hop1["tasks"]] == ["task:related:seed"]
    assert [claim["key"] for claim in hop1["claims"]] == ["claim:related:shared"]
    assert [evidence["key"] for evidence in hop1["evidence"]] == ["evidence:related:seed"]
    assert hop1["memories"] == []

    hop2 = intelligence.related(task_key="task:related:seed", hops=2)
    assert [task["key"] for task in hop2["tasks"]] == [
        "task:related:seed",
        "task:related:second",
    ]
    assert [claim["key"] for claim in hop2["claims"]] == ["claim:related:shared"]
    assert [evidence["key"] for evidence in hop2["evidence"]] == ["evidence:related:seed"]
    assert [memory["key"] for memory in hop2["memories"]] == ["memory:related:second"]
    return {
        "related_hops_ok": True,
        "related_hop2_task_count": len(hop2["tasks"]),
    }


intelligence_related_graph = StateGraph(dict, name="python_intelligence_related_hops", worker_count=2)
intelligence_related_graph.add_node("related", validate_intelligence_related_hops)
intelligence_related_graph.add_edge(START, "related")
intelligence_related_graph.add_edge("related", END)
intelligence_related_result = intelligence_related_graph.compile().invoke({})
assert intelligence_related_result["related_hops_ok"] is True
assert intelligence_related_result["related_hop2_task_count"] == 2


def validate_intelligence_focus_ranking(state, config, runtime: RuntimeContext):
    intelligence = runtime.intelligence

    intelligence.upsert_task(
        "task:focus:weak",
        title="Weak task",
        owner="focus-owner",
        details={"class": "weak"},
        status="open",
        priority=4,
        confidence=0.8,
    )
    intelligence.upsert_task(
        "task:focus:decisive",
        title="Decisive task",
        owner="focus-owner",
        details={"class": "decisive"},
        status="open",
        priority=4,
        confidence=0.8,
    )
    intelligence.upsert_claim(
        "claim:focus:weak",
        subject="agentcore",
        relation="supports",
        object="focus-ranking",
        statement={"support": "weak"},
        status="supported",
        confidence=0.8,
    )
    intelligence.upsert_claim(
        "claim:focus:decisive",
        subject="agentcore",
        relation="supports",
        object="focus-ranking",
        statement={"support": "decisive"},
        status="supported",
        confidence=0.8,
    )

    for index in range(4):
        intelligence.add_evidence(
            f"evidence:focus:weak:{index}",
            kind="observation",
            source="focus-suite",
            content={"weak_evidence": index},
            task_key="task:focus:weak",
            claim_key="claim:focus:weak",
            confidence=0.2 + (index * 0.02),
        )
        intelligence.remember(
            f"memory:focus:weak:{index}",
            layer="working",
            scope="focus-scope",
            content={"weak_memory": index},
            task_key="task:focus:weak",
            claim_key="claim:focus:weak",
            importance=0.1 + (index * 0.02),
        )

    intelligence.add_evidence(
        "evidence:focus:decisive",
        kind="benchmark",
        source="focus-suite",
        content={"decisive_evidence": True},
        task_key="task:focus:decisive",
        claim_key="claim:focus:decisive",
        confidence=0.97,
    )
    intelligence.record_decision(
        "decision:focus:decisive",
        task_key="task:focus:decisive",
        claim_key="claim:focus:decisive",
        summary={"selected": True},
        status="selected",
        confidence=0.93,
    )
    intelligence.remember(
        "memory:focus:decisive",
        layer="semantic",
        scope="focus-scope",
        content={"decisive_memory": True},
        task_key="task:focus:decisive",
        claim_key="claim:focus:decisive",
        importance=0.88,
    )

    focus = intelligence.focus(owner="focus-owner", scope="focus-scope", limit=2)
    assert [task["key"] for task in focus["tasks"]] == [
        "task:focus:decisive",
        "task:focus:weak",
    ]
    assert [claim["key"] for claim in focus["claims"]] == [
        "claim:focus:decisive",
        "claim:focus:weak",
    ]
    assert focus["evidence"][0]["key"] == "evidence:focus:decisive"
    assert focus["decisions"][0]["key"] == "decision:focus:decisive"
    assert focus["memories"][0]["key"] == "memory:focus:decisive"
    return {
        "focus_ranking_ok": True,
        "focus_top_task": focus["tasks"][0]["key"],
        "focus_top_claim": focus["claims"][0]["key"],
    }


intelligence_focus_graph = StateGraph(dict, name="python_intelligence_focus_ranking", worker_count=2)
intelligence_focus_graph.add_node("focus", validate_intelligence_focus_ranking)
intelligence_focus_graph.add_edge(START, "focus")
intelligence_focus_graph.add_edge("focus", END)
intelligence_focus_result = intelligence_focus_graph.compile().invoke({})
assert intelligence_focus_result["focus_ranking_ok"] is True
assert intelligence_focus_result["focus_top_task"] == "task:focus:decisive"
assert intelligence_focus_result["focus_top_claim"] == "claim:focus:decisive"


def seed_supported_claim(state, config, runtime: RuntimeContext):
    runtime.intelligence.upsert_claim(
        "claim:routing",
        subject="agentcore",
        relation="beats",
        object="baseline",
        status="supported",
        confidence=0.9,
    )
    assert runtime.intelligence.count(kind="claims", status="supported") == 1
    assert runtime.intelligence.exists(kind="claims", key="claim:routing")
    return {"claim_seeded": True}


intelligence_router_graph = StateGraph(dict, name="python_intelligence_router_smoke", worker_count=2)
intelligence_router_graph.add_node("seed", seed_supported_claim)
intelligence_router_graph.add_node("supported", lambda state, config: {"route_result": "supported"})
intelligence_router_graph.add_node("fallback", lambda state, config: {"route_result": "fallback"})
intelligence_router_graph.add_edge(START, "seed")
intelligence_router_graph.add_conditional_edges(
    "seed",
    IntelligenceRouter(
        rules=[
            IntelligenceRule(
                goto="supported",
                kind="claims",
                subject="agentcore",
                relation="beats",
                object="baseline",
                status="supported",
                min_count=1,
            )
        ],
        default="fallback",
    ),
    {"supported": "supported", "fallback": "fallback"},
)
intelligence_router_graph.add_edge("supported", END)
intelligence_router_graph.add_edge("fallback", END)
intelligence_router_result = intelligence_router_graph.compile().invoke({})
assert intelligence_router_result["route_result"] == "supported"


def inline_intelligence_route(state, config, runtime: RuntimeContext):
    runtime.intelligence.upsert_task(
        "task:inline-route",
        title="Inline route",
        status="completed",
        confidence=0.8,
    )
    target = runtime.intelligence.route(
        [
            IntelligenceRule(
                goto="done",
                kind="tasks",
                status="completed",
                min_count=1,
            )
        ],
        default="pending",
    )
    return Command(update={"inline_target": target}, goto=target)


inline_route_graph = StateGraph(dict, name="python_inline_intelligence_route_smoke", worker_count=2)
inline_route_graph.add_node("route", inline_intelligence_route)
inline_route_graph.add_node("done", lambda state, config: {"inline_result": "done"})
inline_route_graph.add_node("pending", lambda state, config: {"inline_result": "pending"})
inline_route_graph.add_edge(START, "route")
inline_route_graph.add_edge("done", END)
inline_route_graph.add_edge("pending", END)
inline_route_result = inline_route_graph.compile().invoke({})
assert inline_route_result["inline_target"] == "done"
assert inline_route_result["inline_result"] == "done"


def write_reactive_claim(state, config, runtime: RuntimeContext):
    runtime.intelligence.upsert_claim(
        "claim:reactive-python",
        subject="agentcore",
        relation="supports",
        object="python-reactive-runtime",
        status="supported",
        confidence=0.93,
    )
    return {"claim_written": True}


def consume_reactive_claim(state, config, runtime: RuntimeContext):
    return {"reactive_count": int(state.get("reactive_count", 0)) + 1}


python_reactive_graph = StateGraph(dict, name="python_intelligence_reactive_smoke", worker_count=2)
python_reactive_graph.add_node("write", write_reactive_claim)
python_reactive_graph.add_node("root_stop", lambda state, config: {"root_done": True}, stop_after=True)
python_reactive_graph.add_node(
    "react_supported",
    consume_reactive_claim,
    kind="aggregate",
    stop_after=True,
    intelligence_subscriptions=[
        IntelligenceSubscription(
            subject="agentcore",
            relation="supports",
            object="python-reactive-runtime",
        ),
    ],
)
python_reactive_graph.add_node(
    "react_miss",
    consume_reactive_claim,
    kind="aggregate",
    stop_after=True,
    intelligence_subscriptions=[
        IntelligenceSubscription(
            subject="other-runtime",
            relation="supports",
            object="python-reactive-runtime",
        ),
    ],
)
python_reactive_graph.add_edge(START, "write")
python_reactive_graph.add_edge("write", "root_stop")
python_reactive_details = python_reactive_graph.compile().invoke_with_metadata({})
assert python_reactive_details["state"]["claim_written"] is True
assert [event["node_name"] for event in python_reactive_details["trace"]].count("react_supported") == 1
assert [event["node_name"] for event in python_reactive_details["trace"]].count("react_miss") == 0


def unary_step(state):
    return {"count": int(state.get("count", 0)) + 1}


def unary_route(state):
    return END if int(state.get("count", 0)) >= 2 else "step"


unary_graph = StateGraph(dict, name="python_unary_callback_smoke", worker_count=2)
unary_graph.add_node("step", unary_step)
unary_graph.add_edge(START, "step")
unary_graph.add_conditional_edges("step", unary_route, {END: END, "step": "step"})
unary_compiled = unary_graph.compile()

unary_result = unary_compiled.invoke({"count": 0})
assert unary_result["count"] == 2


def patch_cache_step(state, config):
    count = int(state.get("count", 0)) + 1
    return {
        "count": count,
        "shared_total": int(state.get("shared_total", 0)) + count,
        f"dynamic_{count}": f"value-{count}",
    }


def patch_cache_route(state, config):
    return END if int(state.get("count", 0)) >= 2 else "step"


patch_cache_graph = StateGraph(dict, name="python_patch_cache_smoke", worker_count=2)
patch_cache_graph.add_node("step", patch_cache_step)
patch_cache_graph.add_edge(START, "step")
patch_cache_graph.add_conditional_edges("step", patch_cache_route, {END: END, "step": "step"})
patch_cache_compiled = patch_cache_graph.compile()

patch_cache_result = patch_cache_compiled.invoke({"count": 0, "shared_total": 0})
assert patch_cache_result["count"] == 2
assert patch_cache_result["shared_total"] == 3
assert patch_cache_result["dynamic_1"] == "value-1"
assert patch_cache_result["dynamic_2"] == "value-2"

memoized_call_counter = {"stable": 0, "invalidated": 0}


def stable_memoized_node(state, config):
    memoized_call_counter["stable"] += 1
    return {"memo_output": int(state.get("input_value", 0)) * 10}


def stable_router(state, config):
    round_index = int(state.get("memo_round", 0))
    if round_index == 0:
        return Command(update={"memo_round": 1}, goto="memoized")
    return Command(goto=END)


stable_memo_graph = StateGraph(dict, name="python_deterministic_memo_stable", worker_count=2)
stable_memo_graph.add_node(
    "memoized",
    stable_memoized_node,
    deterministic=True,
    read_keys=("input_value",),
    cache_size=8,
)
stable_memo_graph.add_node("router", stable_router, kind="control")
stable_memo_graph.add_edge(START, "memoized")
stable_memo_graph.add_edge("memoized", "router")
stable_memo_compiled = stable_memo_graph.compile()

stable_memo_result = stable_memo_compiled.invoke({"input_value": 7, "memo_round": 0})
assert stable_memo_result["memo_output"] == 70
assert stable_memo_result["memo_round"] == 1
assert memoized_call_counter["stable"] == 1


def invalidating_memoized_node(state, config):
    memoized_call_counter["invalidated"] += 1
    return {"memo_output": int(state.get("input_value", 0)) * 10}


def invalidating_router(state, config):
    round_index = int(state.get("memo_round", 0))
    if round_index == 0:
        return Command(
            update={
                "memo_round": 1,
                "input_value": int(state.get("input_value", 0)) + 1,
            },
            goto="memoized",
        )
    return Command(goto=END)


invalidating_memo_graph = StateGraph(dict, name="python_deterministic_memo_invalidation", worker_count=2)
invalidating_memo_graph.add_node(
    "memoized",
    invalidating_memoized_node,
    deterministic=True,
    read_keys=("input_value",),
    cache_size=8,
)
invalidating_memo_graph.add_node("router", invalidating_router, kind="control")
invalidating_memo_graph.add_edge(START, "memoized")
invalidating_memo_graph.add_edge("memoized", "router")
invalidating_memo_compiled = invalidating_memo_graph.compile()

invalidating_memo_result = invalidating_memo_compiled.invoke({"input_value": 7, "memo_round": 0})
assert invalidating_memo_result["input_value"] == 8
assert invalidating_memo_result["memo_output"] == 80
assert memoized_call_counter["invalidated"] == 2


async def exercise_async_surface():
    async_state = await compiled.ainvoke(
        {"count": 0, "history": [], "payload": {}},
        config={"configurable": {"run_label": "async", "target_count": 3}, "tags": ["async"]},
    )
    assert async_state["count"] == 3
    assert async_state["run_label"] == "async"
    assert async_state["tag_count"] == 1

    async_events = [
        event
        async for event in compiled.astream(
            {"count": 0, "history": [], "payload": {}},
            config={"configurable": {"run_label": "astream", "target_count": 2}, "tags": ["astream"]},
        )
    ]
    assert [event["node_name"] for event in async_events] == ["planner", "planner"]

    async_batch = await compiled.abatch(
        [{"count": 0, "history": [], "payload": {}}, {"count": 0, "history": [], "payload": {}}],
        config=[
            {"configurable": {"run_label": "abatch-a", "target_count": 2}, "tags": ["abatch"]},
            {"configurable": {"run_label": "abatch-b", "target_count": 4}, "tags": ["abatch"]},
        ],
    )
    assert [result["count"] for result in async_batch] == [2, 4]


asyncio.run(exercise_async_surface())


recorded_effect_calls = {"count": 0}
recorded_effect_replays: list[bool] = []
recorded_effect_runtime_available: list[bool] = []


def produce_memoized_value():
    recorded_effect_calls["count"] += 1
    return {
        "value": "cached-result",
        "producer_call": recorded_effect_calls["count"],
    }


def memoized_step(state, config, runtime: RuntimeContext):
    assert runtime.available is True
    recorded_effect_runtime_available.append(runtime.available)

    stable_request = {
        "memo_key": "expensive-lookup",
        "run_label": dict(config.get("configurable", {})).get("run_label", "unset"),
    }

    details = runtime.record_once_with_metadata(
        "python::memoized::expensive-lookup",
        stable_request,
        produce_memoized_value,
    )
    recorded_effect_replays.append(bool(details["replayed"]))

    visit = int(state.get("visit", 0)) + 1
    return {
        "visit": visit,
        "memoized_value": details["value"]["value"],
        "producer_call": details["value"]["producer_call"],
        "replayed": details["replayed"],
    }


def memoized_route(state, config, runtime: RuntimeContext):
    assert runtime.available is True
    return END if int(state.get("visit", 0)) >= 2 else "memoized"


memo_graph = StateGraph(dict, name="python_record_once_smoke", worker_count=2)
memo_graph.add_node("memoized", memoized_step)
memo_graph.add_edge(START, "memoized")
memo_graph.add_conditional_edges(
    "memoized",
    memoized_route,
    {END: END, "memoized": "memoized"},
)
memo_compiled = memo_graph.compile()

memo_result = memo_compiled.invoke(
    {"visit": 0},
    config={"configurable": {"run_label": "record-once-smoke"}},
)
assert memo_result["visit"] == 2
assert memo_result["memoized_value"] == "cached-result"
assert memo_result["producer_call"] == 1
assert memo_result["replayed"] is True
assert recorded_effect_calls["count"] == 1
assert recorded_effect_replays == [False, True]
assert recorded_effect_runtime_available == [True, True]

retained_runtime_state: dict[str, object] = {}


def retain_callback_views(state, config, runtime: RuntimeContext):
    retained_runtime_state["raw_state"] = state
    retained_runtime_state["snapshot"] = state.copy()
    retained_runtime_state["runtime"] = runtime
    return {"captured": int(state.get("count", 0))}


retained_graph = StateGraph(dict, name="python_retained_callback_views_smoke", worker_count=2)
retained_graph.add_node("capture", retain_callback_views)
retained_graph.add_edge(START, "capture")
retained_graph.add_edge("capture", END)
retained_compiled = retained_graph.compile()

retained_result = retained_compiled.invoke({"count": 7})
assert retained_result["captured"] == 7
assert retained_runtime_state["snapshot"] == {"count": 7}

try:
    retained_runtime_state["raw_state"].get("count")
    raise AssertionError("retained callback state should expire after the callback returns")
except RuntimeError:
    pass

try:
    retained_runtime_state["runtime"].record_once_with_metadata(
        "python::expired-runtime::smoke",
        {"count": 7},
        lambda: {"value": "stale"},
    )
    raise AssertionError("retained runtime context should expire after the callback returns")
except RuntimeError:
    pass

print("python state graph api smoke passed")
