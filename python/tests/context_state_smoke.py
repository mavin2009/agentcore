from agentcore.graph import ContextSpec, END, START, StateGraph


def seed_context_records(state, config, runtime):
    runtime.knowledge.upsert_triple(
        "AgentCore",
        "has_feature",
        "native knowledge graph",
        payload={"source": "native-kg"},
    )
    staged_knowledge = runtime.knowledge.query(subject="AgentCore", limit=2)
    assert staged_knowledge["staged"] is True
    assert any(
        triple["object"] == "native knowledge graph"
        for triple in staged_knowledge["triples"]
    )
    intelligence = runtime.intelligence
    intelligence.upsert_task(
        "task:answer",
        title="Answer customer question",
        owner="support",
        status="open",
        priority=7,
        confidence=0.9,
    )
    intelligence.upsert_claim(
        "claim:fast",
        subject="agentcore",
        relation="supports",
        object="fast graph execution",
        status="supported",
        confidence=0.86,
        statement={"why": "native graph execution"},
    )
    intelligence.upsert_claim(
        "claim:python-only",
        subject="agentcore",
        relation="supports",
        object="python-only execution",
        status="supported",
        confidence=0.2,
        statement={"why": "conflicting fixture claim"},
    )
    intelligence.add_evidence(
        "evidence:bench",
        kind="benchmark",
        source="local-smoke",
        claim_key="claim:fast",
        content={"result": "faster on persistent sessions"},
        confidence=0.92,
    )
    intelligence.record_decision(
        "decision:route",
        task_key="task:answer",
        claim_key="claim:fast",
        status="selected",
        summary={"route": "answer"},
        confidence=0.81,
    )
    intelligence.remember(
        "memory:working",
        layer="working",
        scope="question",
        task_key="task:answer",
        claim_key="claim:fast",
        content={"note": "prefer cited context"},
        importance=0.77,
    )
    return {
        "messages": [
            {"id": "user", "role": "user", "content": state["question"]},
            {"id": "assistant-draft", "role": "assistant", "content": "draft"},
        ],
        "knowledge": {
            "triples": [
                {
                    "subject": "AgentCore",
                    "relation": "has_feature",
                    "object": "persistent sessions",
                }
            ]
        },
        "seeded": True,
    }


def answer_with_context(state, config, runtime):
    native_knowledge = runtime.knowledge.query(subject="AgentCore", limit=4)
    assert any(
        triple["object"] == "native knowledge graph"
        for triple in native_knowledge["triples"]
    )
    view = runtime.context.view()
    prompt = view.to_prompt(system="Answer with citations.")
    messages = view.to_messages(system="System message")
    assert view.digest
    assert view.provenance
    assert view.conflicts
    assert "Potential context conflicts" in prompt
    assert "[C1]" in prompt
    assert messages[0]["role"] == "system"
    assert any(item["kind"] == "message" for item in view.items)
    assert any(item["kind"] == "claim" for item in view.items)
    assert any(item["kind"] == "knowledge" for item in view.items)
    assert "native knowledge graph" in prompt
    return {
        "answer": prompt,
        "context_digest": view.digest,
        "context_item_count": len(view),
        "context_conflict_count": len(view.conflicts),
    }


context_spec = ContextSpec(
    goal_key="question",
    include=[
        "messages.recent",
        "tasks.agenda",
        "claims.all",
        "evidence.relevant",
        "decisions.selected",
        "memories.working",
        "knowledge.neighborhood",
    ],
    budget_items=16,
    budget_tokens=1200,
    require_citations=True,
    owner="support",
    scope="question",
)

graph = StateGraph(dict, name="python_context_state_smoke", worker_count=2)
graph.add_node("seed", seed_context_records)
graph.add_node("answer", answer_with_context, context=context_spec)
graph.add_edge(START, "seed")
graph.add_edge("seed", "answer")
graph.add_edge("answer", END)
compiled = graph.compile()

details = compiled.invoke_with_metadata({"question": "Why is AgentCore useful?"})
state = details["state"]
assert state["context_item_count"] >= 6
assert state["context_conflict_count"] == 1
assert state["context_digest"]
assert "Answer with citations." in state["answer"]
assert "Goal:" in state["answer"]
assert details["context"]["count"] == 1
assert details["context"]["combined_digest"]
assert details["proof"]["context_digest"] == details["context"]["combined_digest"]
view_details = details["context"]["views"][0]
assert view_details["digest"] == state["context_digest"]
assert view_details["budget"]["included_items"] == state["context_item_count"]
assert view_details["conflicts"][0]["subject"] == "agentcore"
answer_events = [event for event in details["trace"] if event["node_name"] == "answer"]
assert answer_events and answer_events[0]["context_digest"]
assert answer_events[0]["context_views"][0]["digest"] == state["context_digest"]

stream_events = list(compiled.stream({"question": "Why is AgentCore useful?"}))
stream_answer_events = [event for event in stream_events if event["node_name"] == "answer"]
assert stream_answer_events and stream_answer_events[0]["context_digest"]

print("context_state_smoke: ok")
