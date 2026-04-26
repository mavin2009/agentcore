import os
import time

from agentcore import ContextSpec, END, START, StateGraph
from agentcore.graphstores import GraphTriple, Neo4jGraphStore


URI = os.getenv("AGENTCORE_NEO4J_URI")
USER = os.getenv("AGENTCORE_NEO4J_USER", "neo4j")
PASSWORD = os.getenv("AGENTCORE_NEO4J_PASSWORD")
DATABASE = os.getenv("AGENTCORE_NEO4J_DATABASE")
REQUIRE = os.getenv("AGENTCORE_REQUIRE_NEO4J", "").strip().lower() in {"1", "true", "yes"}

if not URI:
    if REQUIRE:
        raise RuntimeError("AGENTCORE_NEO4J_URI is required when AGENTCORE_REQUIRE_NEO4J=1")
    print("neo4j_graph_store_integration_test: skipped; AGENTCORE_NEO4J_URI is not set")
    raise SystemExit(0)

if PASSWORD is None:
    if REQUIRE:
        raise RuntimeError("AGENTCORE_NEO4J_PASSWORD is required when AGENTCORE_REQUIRE_NEO4J=1")
    print("neo4j_graph_store_integration_test: skipped; AGENTCORE_NEO4J_PASSWORD is not set")
    raise SystemExit(0)


store = Neo4jGraphStore(
    URI,
    auth=(USER, PASSWORD),
    database=DATABASE,
    name="ops_graph",
)


def wait_for_neo4j() -> None:
    last_error = None
    for _ in range(90):
        try:
            with store._driver.session(database=DATABASE) as session:
                record = session.run("RETURN 1 AS ok").single()
                if record is not None and record["ok"] == 1:
                    return
        except Exception as exc:
            last_error = exc
            time.sleep(1)
    raise RuntimeError(f"Neo4j did not become ready: {last_error!r}")


def clear_agentcore_graph() -> None:
    with store._driver.session(database=DATABASE) as session:
        session.run("MATCH (n:AgentCoreEntity) DETACH DELETE n")


wait_for_neo4j()
clear_agentcore_graph()

store.upsert_entities([
    {"label": "Incident", "payload": {"kind": "incident"}},
    {"label": "checkout API", "payload": {"kind": "service"}},
])
store.upsert_triples([
    GraphTriple("Incident", "affects", "checkout API", payload={"source": "neo4j-test"}),
    GraphTriple("Incident", "requires", "customer notification"),
    GraphTriple("checkout API", "depends_on", "payment service"),
    GraphTriple("payment service", "runs_on", "primary database"),
])

first_hop = store.neighborhood("Incident", depth=1, limit=10).to_dict()
assert {
    (triple["subject"], triple["relation"], triple["object"])
    for triple in first_hop["triples"]
} >= {
    ("Incident", "affects", "checkout API"),
    ("Incident", "requires", "customer notification"),
}

second_hop = store.neighborhood("Incident", depth=2, limit=10).to_dict()
assert ("checkout API", "depends_on", "payment service") in {
    (triple["subject"], triple["relation"], triple["object"])
    for triple in second_hop["triples"]
}

filtered = store.query(
    subject="Incident",
    relation="affects",
    object="checkout API",
    limit=5,
).to_dict()
assert len(filtered["triples"]) == 1
assert filtered["triples"][0]["payload"] == {"source": "neo4j-test"}

malicious_relation = "x`) MATCH (n) DETACH DELETE n //"
store.upsert_triples([
    GraphTriple("Incident", malicious_relation, "safe object"),
])
malicious_query = store.query(subject="Incident", relation=malicious_relation, limit=5).to_dict()
assert len(malicious_query["triples"]) == 1


def hydrate_from_neo4j(state, config, runtime):
    loaded = runtime.knowledge.load_neighborhood(
        "Incident",
        store="ops_graph",
        depth=2,
        limit=20,
    )
    loaded_edges = {
        (triple["subject"], triple["relation"], triple["object"])
        for triple in loaded["triples"]
    }
    assert ("Incident", "affects", "checkout API") in loaded_edges
    assert ("checkout API", "depends_on", "payment service") in loaded_edges
    runtime.knowledge.upsert_triple("Incident", "mitigated_by", "rollback")
    synced = runtime.knowledge.sync_to_store(
        "ops_graph",
        subject="Incident",
        direction="outgoing",
        limit=20,
    )
    return {
        "loaded_triples": len(loaded["triples"]),
        "synced_triples": synced["triples"],
    }


context_spec = ContextSpec(
    goal_key="goal",
    include=["knowledge.neighborhood", "state.loaded_triples"],
    subject="Incident",
    require_citations=True,
)


def plan_from_loaded_graph(state, config, runtime):
    prompt = runtime.context.view().to_prompt(system="Use cited graph facts.")
    assert "Incident affects checkout API" in prompt
    assert "Incident mitigated_by rollback" in prompt
    return {
        "plan": "rollback with customer notification",
        "prompt_has_citations": "[C" in prompt,
    }


graph = StateGraph(dict, name="neo4j_graph_store_integration")
graph.add_node("hydrate", hydrate_from_neo4j)
graph.add_node("plan", plan_from_loaded_graph, context=context_spec)
graph.add_edge(START, "hydrate")
graph.add_edge("hydrate", "plan")
graph.add_edge("plan", END)

compiled = graph.compile()
compiled.graph_stores.register("ops_graph", store)

details = compiled.invoke_with_metadata({"goal": "stabilize checkout"})
state = details["state"]
assert details["summary"]["status"] == "completed"
assert state["loaded_triples"] >= 4
assert state["synced_triples"] >= 4
assert state["plan"] == "rollback with customer notification"
assert state["prompt_has_citations"] is True

persisted = store.query(
    subject="Incident",
    relation="mitigated_by",
    object="rollback",
    limit=5,
).to_dict()
assert len(persisted["triples"]) == 1

clear_agentcore_graph()
store.close()

print("neo4j_graph_store_integration_test: ok")
