# Graph Store Integration

AgentCore has native knowledge-graph state for execution-time decisions, context assembly, checkpoints, subgraphs, and replay. The graph-store layer is for data that lives outside the runtime, such as Neo4j or another graph database.

The key design choice is explicit hydration. External graph data is not automatically merged into runtime state. A node decides what to load, and the loaded triples become ordinary native `runtime.knowledge` writes for that step.

## When To Use This

Use graph stores when:

- your source of truth for entities and relationships lives in a graph database
- a node needs a bounded neighborhood as prompt or decision context
- tests need deterministic graph-shaped fixtures without running a database
- a workflow should write selected runtime knowledge back to an external graph

Keep native `runtime.knowledge` as the execution-time graph. Keep Neo4j or another graph database as the external system of record when you need durability, offline graph exploration, or access from other services.

## Minimal Example

```python
from agentcore.graph import END, START, StateGraph


def load_context(state, config, runtime):
    loaded = runtime.knowledge.load_neighborhood(
        "Incident",
        store="ops_graph",
        depth=2,
        limit=20,
    )
    return {"loaded_triples": len(loaded["triples"])}


graph = StateGraph(dict, name="graph_store_demo")
graph.add_node("load", load_context)
graph.add_edge(START, "load")
graph.add_edge("load", END)

compiled = graph.compile()
compiled.graph_stores.register_memory(
    "ops_graph",
    triples=[
        ("Incident", "affects", "checkout API"),
        ("checkout API", "depends_on", "payment service"),
    ],
)

result = compiled.invoke({})
```

The store registry is graph-owned. For `StateGraph` subgraphs compiled as part of the parent graph, the same registry is available inside child callbacks. If you pass an already compiled child graph, register graph stores on that compiled child if its callbacks need direct access.

## Runtime API

Inside callbacks:

- `runtime.graph_stores.list()`
- `runtime.graph_stores.get(name)`
- `runtime.knowledge.load_query(store=..., subject=None, relation=None, object=None, direction="match", depth=1, limit=None, properties=None)`
- `runtime.knowledge.load_neighborhood(entity=None, *, store, subject=None, relation=None, depth=1, limit=None, properties=None)`
- `runtime.knowledge.sync_to_store(store, *, subject=None, relation=None, object=None, direction="match", limit=None)`

On a compiled graph:

- `compiled.graph_stores.register(name, store)`
- `compiled.graph_stores.register_memory(name="memory", *, entities=None, triples=None)`
- `compiled.graph_stores.register_neo4j(name="neo4j", *, uri=None, auth=None, database=None, driver=None, from_env=False)`
- `compiled.graph_stores.get(name)`
- `compiled.graph_stores.list()`
- `compiled.graph_stores.remove(name, close=False)`
- `compiled.graph_stores.close_all()`

## Neo4j

Install the optional dependency:

```bash
python3 -m pip install "agentcore-graph[neo4j]"
```

Register a store:

```python
compiled.graph_stores.register_neo4j(
    "ops_graph",
    uri="bolt://localhost:7687",
    auth=("neo4j", "password"),
)
```

Environment-based registration reads `NEO4J_URI`, `NEO4J_USER`, `NEO4J_PASSWORD`, and `NEO4J_DATABASE`:

```python
compiled.graph_stores.register_neo4j("ops_graph", from_env=True)
```

The Neo4j adapter uses:

- node label: `AgentCoreEntity`
- node key: `agentcore_id`
- relationship type: `AGENTCORE_RELATION`
- relationship property: `relation`

That means domain relation names are data, not Cypher syntax. This avoids interpolating arbitrary relation strings into relationship types and keeps the adapter portable across domain schemas.

## Custom Backends

Implement the `GraphStore` interface from `agentcore.graphstores`:

```python
from agentcore.graphstores import GraphNeighborhood, GraphStore


class MyGraphStore(GraphStore):
    def upsert_entities(self, entities):
        ...

    def upsert_triples(self, triples):
        ...

    def query(
        self,
        *,
        subject=None,
        relation=None,
        object=None,
        direction="match",
        depth=1,
        limit=None,
        properties=None,
    ):
        return GraphNeighborhood(...)
```

Register it with:

```python
compiled.graph_stores.register("my_graph", MyGraphStore())
```

The runtime expects stores to work in entities, triples, and neighborhoods. A backend can translate that contract into Cypher, Gremlin, SQL, a graph-vector index, a local file, or a service call.

## Validation

The executable coverage for this integration is:

```bash
PYTHONPATH=./build/python python3 ./python/tests/graph_store_pipeline_test.py
```

That test validates:

- in-memory graph-store hydration into native runtime knowledge
- context assembly over hydrated triples
- explicit sync from native knowledge back into the external store
- parent-compiled subgraph access to the graph-store registry
- Neo4j adapter parameterization for arbitrary relation names

### Live Neo4j Check

Before treating Neo4j support as deployment-ready in a release candidate, run the optional live integration test against a real Neo4j process. It validates:

- batch entity and triple writes through the official Neo4j driver
- first-hop and second-hop neighborhood reads
- exact subject/relation/object filtering
- arbitrary relation strings stored as data, not interpolated Cypher syntax
- runtime hydration from Neo4j into native `runtime.knowledge`
- context assembly over hydrated triples
- explicit sync-back from native knowledge into Neo4j

This copy-paste flow starts Neo4j in the background on a random local Bolt port, runs the test, and removes the container:

```bash
python3 -m pip install "agentcore-graph[neo4j]"

name="agentcore-neo4j-test"
password="agentcore-test-password"
cleanup() { docker rm -f "$name" >/dev/null 2>&1 || true; }
trap cleanup EXIT

docker rm -f "$name" >/dev/null 2>&1 || true
docker run -d --rm --name "$name" \
  -p 127.0.0.1::7687 \
  -e NEO4J_AUTH="neo4j/$password" \
  neo4j:5-community
port="$(docker port "$name" 7687/tcp | sed -E 's/.*:([0-9]+)$/\1/')"

AGENTCORE_REQUIRE_NEO4J=1 \
AGENTCORE_NEO4J_URI="bolt://127.0.0.1:$port" \
AGENTCORE_NEO4J_USER=neo4j \
AGENTCORE_NEO4J_PASSWORD="$password" \
PYTHONPATH=./build/python \
python3 ./python/tests/neo4j_graph_store_integration_test.py
```

The live test creates only `AgentCoreEntity` nodes and removes those nodes before and after the run. If `AGENTCORE_NEO4J_URI` is not set, the test exits successfully with a skip message so it can live beside the normal smoke tests without requiring Docker.
