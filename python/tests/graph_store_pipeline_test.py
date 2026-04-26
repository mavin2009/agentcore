from agentcore import ContextSpec, END, START, StateGraph
from agentcore.graphstores import GraphTriple, InMemoryGraphStore, Neo4jGraphStore


external_graph = InMemoryGraphStore.from_triples(
    [
        GraphTriple("Incident", "affects", "checkout API", payload={"source": "ops-graph"}),
        GraphTriple("Incident", "requires", "customer notification"),
        GraphTriple("checkout API", "depends_on", "payment service"),
        GraphTriple("payment service", "runs_on", "primary database"),
    ],
    entities=[
        {"label": "Incident", "payload": {"kind": "incident"}},
        {"label": "checkout API", "payload": {"kind": "service"}},
    ],
    name="ops_graph",
)


def retrieve_external_context(state, config, runtime):
    assert runtime.graph_stores.list()[0]["name"] == "ops_graph"

    loaded = runtime.knowledge.load_neighborhood(
        "Incident",
        store="ops_graph",
        depth=2,
        limit=8,
    )
    loaded_edges = {
        (triple["subject"], triple["relation"], triple["object"])
        for triple in loaded["triples"]
    }
    assert ("Incident", "affects", "checkout API") in loaded_edges
    assert ("checkout API", "depends_on", "payment service") in loaded_edges

    runtime.knowledge.upsert_triple(
        "Incident",
        "mitigated_by",
        "rollback",
        payload={"source": "agentcore-native"},
    )
    synced = runtime.knowledge.sync_to_store(
        "ops_graph",
        subject="Incident",
        direction="outgoing",
        limit=12,
    )
    assert synced["triples"] >= 3
    return {
        "loaded_triples": len(loaded["triples"]),
        "synced_triples": synced["triples"],
    }


planner_context = ContextSpec(
    goal_key="goal",
    include=[
        "knowledge.neighborhood",
        "state.loaded_triples",
        "state.synced_triples",
    ],
    subject="Incident",
    budget_items=12,
    budget_tokens=1200,
    require_citations=True,
)


def plan_from_hydrated_graph(state, config, runtime):
    native = runtime.knowledge.neighborhood("Incident", limit=10)
    native_edges = {
        (triple["subject"], triple["relation"], triple["object"])
        for triple in native["triples"]
    }
    assert ("Incident", "affects", "checkout API") in native_edges
    assert ("Incident", "mitigated_by", "rollback") in native_edges

    prompt = runtime.context.view().to_prompt(system="Use the operational graph.")
    assert "Incident affects checkout API" in prompt
    assert "Incident mitigated_by rollback" in prompt
    return {
        "plan": "rollback with customer notification",
        "prompt_has_citations": "[C" in prompt,
    }


graph = StateGraph(dict, name="graph_store_pipeline")
graph.add_node("retrieve", retrieve_external_context)
graph.add_node("plan", plan_from_hydrated_graph, context=planner_context)
graph.add_edge(START, "retrieve")
graph.add_edge("retrieve", "plan")
graph.add_edge("plan", END)
compiled = graph.compile()
compiled.graph_stores.register("ops_graph", external_graph)

details = compiled.invoke_with_metadata({"goal": "stabilize checkout"})
state = details["state"]

assert details["summary"]["status"] == "completed"
assert state["loaded_triples"] >= 3
assert state["synced_triples"] >= 3
assert state["plan"] == "rollback with customer notification"
assert state["prompt_has_citations"] is True

persisted = external_graph.neighborhood("Incident", depth=1, limit=10).to_dict()
persisted_edges = {
    (triple["subject"], triple["relation"], triple["object"])
    for triple in persisted["triples"]
}
assert ("Incident", "mitigated_by", "rollback") in persisted_edges


def child_loads_store(state, config, runtime):
    loaded = runtime.knowledge.load_neighborhood("Incident", store="ops_graph", depth=1, limit=6)
    return {"child_loaded_triples": len(loaded["triples"])}


child_graph = StateGraph(dict, name="graph_store_child")
child_graph.add_node("load", child_loads_store)
child_graph.add_edge(START, "load")
child_graph.add_edge("load", END)

parent_graph = StateGraph(dict, name="graph_store_parent")
parent_graph.add_subgraph(
    "child",
    child_graph,
    outputs={"child_loaded_triples": "child_loaded_triples"},
)
parent_graph.add_edge(START, "child")
parent_graph.add_edge("child", END)

compiled_parent = parent_graph.compile()
compiled_parent.graph_stores.register("ops_graph", external_graph)
parent_state = compiled_parent.invoke({})
assert parent_state["child_loaded_triples"] >= 3


class FakeTransaction:
    def __init__(self, calls):
        self._calls = calls

    def run(self, cypher, **parameters):
        self._calls.append((cypher, parameters))
        return []


class FakeSession:
    def __init__(self, calls):
        self._calls = calls

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def execute_write(self, callback):
        return callback(FakeTransaction(self._calls))

    def run(self, cypher, **parameters):
        self._calls.append((cypher, parameters))
        return []


class FakeDriver:
    def __init__(self):
        self.calls = []

    def session(self, database=None):
        return FakeSession(self.calls)


malicious_relation = "x`) MATCH (n) DETACH DELETE n //"
fake_driver = FakeDriver()
neo4j_store = Neo4jGraphStore("bolt://example.invalid", driver=fake_driver)
neo4j_store.upsert_triples([("safe subject", malicious_relation, "safe object")])
neo4j_store.query(subject="safe subject", relation=malicious_relation)

assert fake_driver.calls
for cypher, parameters in fake_driver.calls:
    assert malicious_relation not in cypher
    assert malicious_relation in str(parameters)

print("graph_store_pipeline_test: ok")
