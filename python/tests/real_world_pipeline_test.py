from agentcore import ChatPromptTemplate, Command, ContextSpec, END, START, StateGraph


TOOL_CALLS = []
MODEL_CALLS = []

KNOWLEDGE_BASE = [
    {
        "id": "runbook-checkout",
        "title": "Checkout API rollback runbook",
        "text": "If checkout errors rise after deployment, freeze rollout, compare error budget, and prepare rollback.",
        "tags": ["checkout", "rollback", "incident"],
    },
    {
        "id": "memo-cache-risk",
        "title": "Cart cache risk memo",
        "text": "Cart cache inconsistency can amplify checkout failures and should be validated before declaring recovery.",
        "tags": ["cart", "risk", "cache"],
    },
    {
        "id": "slo-customer-impact",
        "title": "Customer impact SLO",
        "text": "Customer-visible payment failures require incident commander approval and a written customer-impact summary.",
        "tags": ["payment", "customer", "slo"],
    },
]


def search_knowledge_base(request, metadata):
    TOOL_CALLS.append((request, dict(metadata)))
    query = str(request.get("query", "")).lower()
    limit = int(request.get("limit", 3))
    scored = []
    for doc in KNOWLEDGE_BASE:
        haystack = " ".join([doc["title"], doc["text"], " ".join(doc["tags"])]).lower()
        score = sum(1 for token in query.replace("-", " ").split() if token and token in haystack)
        if score:
            scored.append((score, doc))
    selected = [doc for _, doc in sorted(scored, key=lambda item: (-item[0], item[1]["id"]))[:limit]]
    if not selected:
        selected = KNOWLEDGE_BASE[:limit]
    return {
        "documents": selected,
        "query": request.get("query"),
        "tool": metadata["name"],
    }


def briefing_model(prompt, schema, metadata):
    MODEL_CALLS.append((prompt, schema, dict(metadata)))
    assert isinstance(prompt, str)
    assert "planner analysis" in prompt
    assert "risk analysis" in prompt
    assert "runbook-checkout" in prompt
    return {
        "output": {
            "decision": "prepare rollback with customer-impact note",
            "briefing": (
                "Use the rollback runbook, verify cart-cache risk, and include a "
                "customer-impact summary before closure."
            ),
            "schema_style": schema["style"],
            "prompt_contains_citations": "[C" in prompt,
        },
        "confidence": 0.91,
        "token_usage": len(prompt.split()),
    }


def build_specialist_graph():
    specialist_context = ContextSpec(
        goal_key="query",
        include=[
            "messages.recent",
            "claims.supported",
            "evidence.relevant",
            "knowledge.neighborhood",
            "state.documents",
        ],
        subject="Incident",
        budget_items=20,
        budget_tokens=2200,
        require_citations=True,
    )

    def analyze(state, config, runtime):
        role = str(state["role"])
        session_id = str(state["session_id"])
        query = str(state["query"])
        documents = list(state["documents"])

        neighborhood = runtime.knowledge.neighborhood("Incident", limit=8)
        assert any(triple["object"] == "checkout API" for triple in neighborhood["triples"])
        assert any(triple["object"] == "customer impact" for triple in neighborhood["triples"])

        runtime.intelligence.upsert_task(
            f"{session_id}:analysis",
            title=f"{role} incident analysis",
            owner=role,
            status="in_progress",
            priority=8 if role == "planner" else 7,
            confidence=0.82,
        )
        runtime.intelligence.upsert_claim(
            f"{session_id}:recommendation",
            subject="Incident",
            relation="has_recommendation",
            object=f"{role} recommendation",
            status="supported",
            confidence=0.84,
            statement={
                "role": role,
                "documents": [doc["id"] for doc in documents],
            },
        )
        runtime.intelligence.add_evidence(
            f"{session_id}:evidence",
            kind="retrieval",
            source="kb_search",
            claim_key=f"{session_id}:recommendation",
            content={"document_ids": [doc["id"] for doc in documents]},
            confidence=0.88,
        )
        runtime.knowledge.upsert_triple(
            "Incident",
            "reviewed_by",
            role,
            payload={"session_id": session_id},
        )

        context = runtime.context.view()
        prompt = context.to_prompt(system=f"You are the {role} specialist.")
        assert "Incident affects checkout API" in prompt
        assert f"{role} recommendation" in prompt

        memory = list(state.get("memory", []))
        memory.append({
            "role": role,
            "query": query,
            "context_digest": context.digest,
        })
        return {
            "analysis": f"{role} analysis: {query}; docs={','.join(doc['id'] for doc in documents)}",
            "memory": memory,
            "context_digest": context.digest,
            "context_items": len(context),
            "kg_triples_seen": len(neighborhood["triples"]),
        }

    graph = StateGraph(dict, name="real_world_specialist_child", worker_count=2)
    graph.add_node("analyze", analyze, context=specialist_context)
    graph.add_edge(START, "analyze")
    graph.add_edge("analyze", END)
    return graph


def build_pipeline():
    child_graph = build_specialist_graph()

    def ingest(state, config, runtime):
        case_id = str(state["case_id"])
        query = str(state["incident"])
        retrieval = runtime.invoke_tool(
            "kb_search",
            {"query": query, "limit": 3},
            decode="json",
        )
        documents = retrieval["documents"]

        runtime.knowledge.upsert_triple("Incident", "affects", "checkout API")
        runtime.knowledge.upsert_triple("Incident", "requires", "rollback decision")
        runtime.knowledge.upsert_triple("Incident", "has_concern", "customer impact")
        runtime.intelligence.upsert_task(
            f"{case_id}:triage",
            title="Triage checkout incident",
            owner="incident-command",
            status="open",
            priority=9,
            confidence=0.93,
        )
        runtime.intelligence.upsert_claim(
            f"{case_id}:checkout-impact",
            subject="Incident",
            relation="affects",
            object="checkout API",
            status="supported",
            confidence=0.9,
            statement={"source": "incident report"},
        )
        runtime.intelligence.add_evidence(
            f"{case_id}:retrieval",
            kind="runbook",
            source="kb_search",
            claim_key=f"{case_id}:checkout-impact",
            content={"document_ids": [doc["id"] for doc in documents]},
            confidence=0.89,
        )

        return {
            "documents": documents,
            "retrieved_doc_ids": [doc["id"] for doc in documents],
            "messages": [{"role": "user", "content": query}],
            "incident_query": query,
            "planner_session": f"{case_id}:planner",
            "risk_session": f"{case_id}:risk",
            "planner_role": "planner",
            "risk_role": "risk",
        }

    final_context = ContextSpec(
        goal_key="incident_query",
        include=[
            "messages.recent",
            "claims.supported",
            "evidence.relevant",
            "knowledge.neighborhood",
            "state.planner_analysis",
            "state.risk_analysis",
        ],
        subject="Incident",
        budget_items=24,
        budget_tokens=2600,
        require_citations=True,
    )

    def synthesize(state, config, runtime):
        parent_kg = runtime.knowledge.neighborhood("Incident", limit=10)
        reviewed_by = [
            triple for triple in parent_kg["triples"]
            if triple["relation"] == "reviewed_by"
        ]
        assert not reviewed_by, "child knowledge graph writes must not implicitly merge into the parent"

        context = runtime.context.view()
        rendered = ChatPromptTemplate.from_messages([
            ("system", "You are the incident commander. Use cited evidence."),
            (
                "user",
                "Incident: {incident}\n\nplanner analysis:\n{planner}\n\nrisk analysis:\n{risk}\n\nContext:\n{context}",
            ),
        ]).render(
            incident=state["incident_query"],
            planner=state["planner_analysis"],
            risk=state["risk_analysis"],
            context=context.to_prompt(),
        )

        briefing = runtime.invoke_model_with_metadata(
            "briefing_model",
            rendered,
            schema={"style": "incident_briefing"},
            max_tokens=256,
            decode="json",
        )
        runtime.intelligence.record_decision(
            f"{state['case_id']}:decision",
            task_key=f"{state['case_id']}:triage",
            claim_key=f"{state['case_id']}:checkout-impact",
            status="selected",
            summary=briefing["output"],
            confidence=briefing["confidence"],
        )
        return {
            "final_briefing": briefing["output"],
            "model_confidence": briefing["confidence"],
            "model_token_usage": briefing["token_usage"],
            "final_context_digest": context.digest,
            "final_context_items": len(context),
            "parent_kg_triples": len(parent_kg["triples"]),
            "parent_reviewed_by_count": len(reviewed_by),
            "complete": True,
        }

    graph = StateGraph(dict, name="real_world_incident_pipeline", worker_count=4)
    graph.add_node("ingest", ingest, kind="tool")
    graph.add_fanout("dispatch_specialists")
    graph.add_subgraph(
        "planner_specialist",
        child_graph,
        namespace="planner_specialist",
        inputs={
            "planner_session": "session_id",
            "planner_role": "role",
            "incident_query": "query",
            "documents": "documents",
            "messages": "messages",
        },
        outputs={
            "planner_analysis": "analysis",
            "planner_memory": "memory",
            "planner_context_digest": "context_digest",
            "planner_context_items": "context_items",
            "planner_kg_triples_seen": "kg_triples_seen",
        },
        propagate_knowledge_graph=True,
        session_mode="persistent",
        session_id_from="planner_session",
    )
    graph.add_subgraph(
        "risk_specialist",
        child_graph,
        namespace="risk_specialist",
        inputs={
            "risk_session": "session_id",
            "risk_role": "role",
            "incident_query": "query",
            "documents": "documents",
            "messages": "messages",
        },
        outputs={
            "risk_analysis": "analysis",
            "risk_memory": "memory",
            "risk_context_digest": "context_digest",
            "risk_context_items": "context_items",
            "risk_kg_triples_seen": "kg_triples_seen",
        },
        propagate_knowledge_graph=True,
        session_mode="persistent",
        session_id_from="risk_session",
    )
    graph.add_node(
        "synthesize",
        synthesize,
        kind="aggregate",
        join_incoming_branches=True,
        context=final_context,
    )
    graph.add_edge(START, "ingest")
    graph.add_edge("ingest", "dispatch_specialists")
    graph.add_edge("dispatch_specialists", "planner_specialist")
    graph.add_edge("dispatch_specialists", "risk_specialist")
    graph.add_edge("planner_specialist", "synthesize")
    graph.add_edge("risk_specialist", "synthesize")
    graph.add_edge("synthesize", END)
    return graph.compile()


compiled = build_pipeline()
compiled.tools.register("kb_search", search_knowledge_base, decode_input="json")
compiled.models.register(
    "briefing_model",
    briefing_model,
    decode_prompt="text",
    decode_schema="json",
)

input_state = {
    "case_id": "INC-4242",
    "incident": "Checkout payment errors after deployment; assess rollback and customer impact.",
}
details = compiled.invoke_with_metadata(input_state, config={"tags": ["real-world", "incident"]})
state = details["state"]

assert details["summary"]["status"] == "completed"
assert state["complete"] is True
assert state["retrieved_doc_ids"] == [
    "runbook-checkout",
    "slo-customer-impact",
    "memo-cache-risk",
]
assert state["planner_analysis"].startswith("planner analysis:")
assert state["risk_analysis"].startswith("risk analysis:")
assert state["planner_context_digest"]
assert state["risk_context_digest"]
assert state["final_context_digest"]
assert state["planner_context_items"] >= 5
assert state["risk_context_items"] >= 5
assert state["final_context_items"] >= 6
assert state["planner_kg_triples_seen"] >= 3
assert state["risk_kg_triples_seen"] >= 3
assert state["parent_reviewed_by_count"] == 0
assert state["final_briefing"]["schema_style"] == "incident_briefing"
assert state["final_briefing"]["prompt_contains_citations"] is True
assert abs(state["model_confidence"] - 0.91) < 0.001
assert state["model_token_usage"] > 20

assert len(TOOL_CALLS) == 1
assert TOOL_CALLS[0][1]["name"] == "kb_search"
assert len(MODEL_CALLS) == 1
assert MODEL_CALLS[0][2]["name"] == "briefing_model"
assert MODEL_CALLS[0][2]["max_tokens"] == 256

context = details["context"]
assert context["count"] >= 1
assert context["combined_digest"] == details["proof"]["context_digest"]
assert any(view["node_name"] == "synthesize" for view in context["views"])
synthesis_events = [event for event in details["trace"] if event["node_name"] == "synthesize"]
assert synthesis_events and synthesis_events[0]["context_digest"]

subgraph_events = [event for event in details["trace"] if event["namespaces"]]
assert {event["session_id"] for event in subgraph_events} == {
    "INC-4242:planner",
    "INC-4242:risk",
}
assert {
    event["namespaces"][0]["node_name"]
    for event in subgraph_events
} == {"planner_specialist", "risk_specialist"}
assert all(event["session_revision"] == 1 for event in subgraph_events)

stream_events = list(compiled.stream({
    "case_id": "INC-4243",
    "incident": "Checkout payment errors after deployment; validate rollback readiness.",
}))
stream_nodes = [event["node_name"] for event in stream_events]
assert "ingest" in stream_nodes
assert "synthesize" in stream_nodes
stream_sessions = {event["session_id"] for event in stream_events if event["session_id"]}
assert {"INC-4243:planner", "INC-4243:risk"} <= stream_sessions
stream_synthesis = [event for event in stream_events if event["node_name"] == "synthesize"]
assert stream_synthesis and stream_synthesis[0]["context_digest"]

print("real_world_pipeline_test: ok")
