# Python Quickstart

This guide uses the local native build produced by CMake. The Python API is a compact builder layered over the native runtime, so the graph definition happens in Python while execution, scheduling, patch application, streaming, and subgraph runtime stay in C++.

## Build The Python Package

From the repository root:

```bash
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON
cmake --build build -j
```

The build emits a package under `./build/python/agentcore`. To use it directly from the build tree:

```bash
PYTHONPATH=./build/python python3 -c "from agentcore.graph import StateGraph; print('ok')"
```

If you want a pip-installable package artifact instead of using the build tree directly, the published distribution name is `agentcore-graph` while the Python import package remains `agentcore`:

```bash
CC=cc CXX=c++ python3 -m pip wheel . -w dist
python3 -m pip install --target /tmp/agentcore-wheel-test dist/agentcore_graph-*.whl
PYTHONPATH=/tmp/agentcore-wheel-test python3 -c "from agentcore.graph import StateGraph; print('ok')"
```

That wheel includes the `agentcore` package and the compatibility namespace under `agentcore_langgraph_native`. It intentionally does not install a top-level `langgraph` package into the environment.

For published releases, the repository also includes cibuildwheel-based automation in `./.github/workflows/` that builds Linux `manylinux_2_28` wheels for CPython 3.9-3.12 and runs the Python smoke coverage against those built wheels before release.

## Define A Minimal Graph

```python
from agentcore.graph import END, START, StateGraph


def step(state, config):
    next_count = int(state.get("count", 0)) + 1
    return {"count": next_count}


def route(state, config):
    return END if state["count"] >= 3 else "step"


graph = StateGraph(dict, name="counter", worker_count=2)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_conditional_edges("step", route, {END: END, "step": "step"})

compiled = graph.compile()
final_state = compiled.invoke({"count": 0})
```

The important pieces are:

- `StateGraph(...)` defines the graph and the native worker count used when it is compiled.
- `add_node(...)` registers a node callback. The callback may be synchronous or async.
- `add_edge(...)` and `add_conditional_edges(...)` describe routing.
- `compile()` lowers the graph into the native runtime.
- `invoke()` executes one run and returns the final state mapping.

If a node is deterministic for a declared subset of state, you can opt into native memoization:

```python
graph.add_node(
    "score",
    score_candidate,
    deterministic=True,
    read_keys=["candidate_id", "rubric_version"],
    cache_size=32,
)
```

That cache lives in the native runtime, not in Python. The current implementation keys it by the declared `read_keys` plus the runtime config payload and invalidates cached entries when one of those state keys changes.

## Use Higher-Level Builders For Common Shapes

`StateGraph` remains the core Python API, but the package now also includes an optional pattern layer for workflow shapes that otherwise require a lot of repetitive builder code.

### Sequential Pipelines

```python
from agentcore import PipelineGraph


pipeline = PipelineGraph(dict, name="draft_pipeline", worker_count=2)
pipeline.add_step("seed", lambda state, config: {"topic": "native-runtime"})
pipeline.add_step("draft", lambda state, config: {"draft": f"draft::{state['topic']}"})
pipeline.add_step("finalize", lambda state, config: {"final": f"{state['draft']}::final"})

compiled = pipeline.compile()
result = compiled.invoke({})
```

`PipelineGraph` is useful when the only manual wiring would have been `START -> step1 -> step2 -> ... -> END`.

### Specialist Teams

```python
from agentcore import SpecialistTeam, StateGraph
from agentcore.graph import END, START


specialist = StateGraph(dict, name="specialist_child", worker_count=2)
specialist.add_node("answer", lambda state, config: {"answer": f"{state['session_id']}::{state['query']}"})
specialist.add_edge(START, "answer")
specialist.add_edge("answer", END)

team = SpecialistTeam(dict, name="review_team", worker_count=4)
team.set_dispatch("dispatch", lambda state, config: {"topic": "scheduler"})
team.add_specialist(
    "planner",
    specialist,
    prepare=lambda state, config: {
        "planner_session_id": "planner",
        "planner_query": f"plan::{state['topic']}",
    },
    inputs={
        "planner_session_id": "session_id",
        "planner_query": "query",
    },
    outputs={"planner_answer": "answer"},
)
team.add_specialist(
    "critic",
    specialist,
    prepare=lambda state, config: {
        "critic_session_id": "critic",
        "critic_query": f"critique::{state['topic']}",
    },
    inputs={
        "critic_session_id": "session_id",
        "critic_query": "query",
    },
    outputs={"critic_answer": "answer"},
)
team.set_aggregate(
    "synthesize",
    lambda state, config: {
        "summary": f"{state['planner_answer']}|{state['critic_answer']}",
    },
)

compiled = team.compile()
details = compiled.invoke_with_metadata({})
```

`SpecialistTeam` exists for a specific reason: specialist flows usually want the same structure every time, namely dispatch, optional specialist-specific preparation, persistent child-session execution, and a single aggregation point. The helper keeps that structure explicit while avoiding repeated fan-out/join/subgraph wiring in application code.

## Inspect Execution

The compiled graph supports several useful inspection surfaces:

- `invoke_with_metadata(...)` returns the final state plus a summary, trace, and proof digest.
- `invoke_until_pause_with_metadata(...)` runs until the graph pauses and returns the paused summary, trace, and checkpoint metadata.
- `resume_with_metadata(...)` restores from a checkpoint created by a paused run and returns the resumed result plus metadata.
- `stream(...)` yields public execution events in run order.
- `astream(...)` is the async wrapper for streamed events.

Example:

```python
details = compiled.invoke_with_metadata({"count": 0})
events = list(compiled.stream({"count": 0}))
```

Today the Python stream surface supports `stream_mode="events"` only.

Each streamed event and trace event may also carry subgraph session metadata:

- `session_id`
- `session_revision`
- `namespaces`

## Node Return Forms

Python node callbacks can return any of the following:

- a mapping of field updates
- `None`
- a node name string
- `Command(update=..., goto=..., wait=True|False)`
- a `(updates, goto)` tuple
- an awaitable that resolves to one of the above

Callbacks may accept:

- `state`
- `(state, config)`
- `(state, config, runtime)`

The runtime normalizes the result into a native state patch and optional routing override.

`Command(wait=True)` is the Python surface for intentionally yielding a native waiting result without introducing an application-specific sentinel in the state.

## Recorded Once-Only Work

If a Python node needs to perform synchronous work that should not be repeated after the same run revisits the node, declare a third callback parameter and use `RuntimeContext`.

```python
from agentcore.graph import RuntimeContext


def step(state, config, runtime: RuntimeContext):
    details = runtime.record_once_with_metadata(
        "example::lookup",
        {"query": "stable-input"},
        lambda: {"answer": "cached-output"},
    )
    return {
        "answer": details["value"]["answer"],
        "replayed": details["replayed"],
    }
```

This seam is intentionally narrow:

- the request is serialized and validated for the same key
- the producer runs only on the first committed occurrence
- later visits for the same key and request replay the prior output
- the recorded outcome is committed through the same native state/checkpoint path as other updates

## Register Native Adapters From Python

Compiled graphs now expose graph-owned native registries through `.tools` and `.models`.

```python
compiled = graph.compile()

compiled.tools.register_sqlite("kv_store")
compiled.models.register_local("summarizer", default_max_tokens=64)

print(compiled.tools.list())
print(compiled.models.describe("summarizer"))
```

Those registry views are configuration and inspection surfaces for the same native tool/model registries used by the execution engine. They are per compiled graph, which means subgraph execution inherits the parent run's runtime registries through the native engine rather than relying on Python-global adapter state.

If you need a custom adapter without writing C++, you can also register Python-backed handlers directly into those native registries:

```python
async def python_tool(request, meta):
    return {"upper": str(request["text"]).upper(), "adapter": meta["name"]}


async def python_model(prompt, schema, meta):
    return {
        "output": {"summary": f"{prompt['topic']}::{schema['style']}::{meta['max_tokens']}"},
        "confidence": 0.75,
        "token_usage": len(prompt["topic"]),
    }


compiled.tools.register(
    "py_upper",
    python_tool,
    decode_input="json",
    metadata={"request_format": "json", "response_format": "json"},
)
compiled.models.register(
    "py_summarizer",
    python_model,
    decode_prompt="json",
    decode_schema="json",
    metadata={"request_format": "json", "response_format": "json"},
)
```

That shape is deliberate. The handler ergonomics stay Python-friendly, but registration still lands in the native registry, so direct invocation, `RuntimeContext` invocation, metadata inspection, and subgraph inheritance all continue to use one validated adapter path.

You can also invoke registered adapters directly from Python:

```python
value = compiled.tools.invoke(
    "kv_store",
    "action=put\ntable=memory\nkey=topic\nvalue=native-runtime",
    decode="text",
)
summary = compiled.models.invoke(
    "summarizer",
    "topic::native-runtime",
    max_tokens=12,
    decode="text",
)
```

And from inside a node callback via `RuntimeContext`:

```python
def step(state, config, runtime):
    runtime.invoke_tool(
        "kv_store",
        "action=put\ntable=memory\nkey=topic\nvalue=adapters",
        decode="text",
    )
    summary = runtime.invoke_model(
        "summarizer",
        "topic::adapters",
        max_tokens=10,
        decode="text",
    )
    return {"summary": summary}
```

The built-in registration helpers currently cover:

- `compiled.tools.register(...)`
- `compiled.tools.register_http(...)`
- `compiled.tools.register_sqlite(...)`
- `compiled.tools.register_http_json(...)`
- `compiled.models.register(...)`
- `compiled.models.register_local(...)`
- `compiled.models.register_llm_http(...)`
- `compiled.models.register_openai_chat(...)`
- `compiled.models.register_grok_chat(...)`
- `compiled.models.register_gemini_generate_content(...)`

Provider notes:

- `compiled.models.register_grok_chat(...)` uses the shared chat-completions HTTP seam and defaults to the xAI base URL plus `XAI_API_KEY` environment lookup when explicit bearer-token settings are not provided.
- `compiled.models.register_gemini_generate_content(...)` uses Gemini's direct `generateContent` REST surface and defaults to the Gemini API base URL plus `GEMINI_API_KEY` lookup with the `x-goog-api-key` header.

## Fan-Out, Join, And Subgraphs

The builder includes first-class helpers for control-flow shapes that would otherwise require a lot of boilerplate:

- `add_fanout(...)` for explicit branch creation
- `add_join(...)` for merge-aware join points
- `add_subgraph(...)` for native subgraph composition

The executable coverage for those shapes lives in [`../../python/tests/agent_workflows_smoke.py`](../../python/tests/agent_workflows_smoke.py). That file includes:

- a single-agent loop
- a multi-agent handoff flow
- subgraph composition with namespaced stream events
- persistent subgraph sessions with distinct session IDs
- repeated reuse of the same persistent specialist session
- pause/resume preserving child session-local memory
- concurrent subgraph execution through `abatch(...)`
- fan-out and join with merge strategies

If you use `add_subgraph(...)`, the runtime propagates the Python `config` into child runs. That config must therefore be pickle-serializable.

## Persistent Subgraph Sessions

`add_subgraph(...)` supports two session modes:

- `session_mode="ephemeral"` is the default. Every invocation gets a fresh child snapshot.
- `session_mode="persistent"` reuses a committed child snapshot keyed by a parent state field.

The exact builder signature is:

```python
graph.add_subgraph(
    name,
    graph,
    *,
    inputs=None,
    outputs=None,
    namespace=None,
    propagate_knowledge_graph=False,
    session_mode="ephemeral",
    session_id_from=None,
)
```

Rules:

- `session_mode="persistent"` requires `session_id_from="some_parent_field"`.
- `session_mode="ephemeral"` requires `session_id_from=None`.
- `session_id_from` is a parent state field name, not a callable.

Example:

```python
specialist = StateGraph(dict, name="specialist", worker_count=2)
specialist.add_node("answer", lambda state, config: {"visits": int(state.get("visits", 0)) + 1})
specialist.add_edge(START, "answer")
specialist.add_edge("answer", END)

parent = StateGraph(dict, name="parent", worker_count=4)
parent.add_subgraph(
    "specialist_session",
    specialist,
    namespace="specialist_session",
    inputs={"specialist_id": "session_id", "prompt": "query"},
    outputs={"visits": "visits"},
    session_mode="persistent",
    session_id_from="specialist_id",
)
```

The runtime uses isolated child-session snapshots rather than shared mutable child state. That keeps a persistent child session restart-safe and deterministic:

- the child session restores from its last committed snapshot
- current input bindings are overlaid onto that child state before execution
- the child snapshot commits only after successful child completion
- failure, cancellation, or waiting leaves the last committed child snapshot unchanged

When `propagate_knowledge_graph=True` is set on a persistent subgraph, the child knowledge graph is seeded from the parent only when the session is first created. Later invocations reuse the committed child-local knowledge graph. Parent and child knowledge graphs remain separate unless you move data through explicit output bindings.

Concurrent reuse of the same `(subgraph node, session_id)` inside one run is rejected rather than merged. Distinct session IDs may run in parallel.

You can inspect that session identity directly from metadata and streamed events:

```python
details = parent.compile().invoke_with_metadata(
    {"specialist_id": "planner", "prompt": "draft a plan"},
)
session_events = [event for event in details["trace"] if event.get("session_id")]

for event in session_events:
    print(event["node_name"], event["session_id"], event["session_revision"])
```

## Pause And Resume

The Python binding exposes a small pause/resume seam over the native checkpoint model:

```python
from agentcore.graph import Command


def specialist(state, config):
    if not state.get("ready", False):
        return Command(update={"ready": True}, wait=True)
    return {"done": True}


compiled = graph.compile()
paused = compiled.invoke_until_pause_with_metadata({"ready": False})
resumed = compiled.resume_with_metadata(paused["summary"]["checkpoint_id"])
```

This is useful for:

- human-in-the-loop nodes
- external wait points
- validating checkpoint/resume behavior from Python

For persistent subgraphs, resumed execution restores both parent state and the committed child-session snapshot that was current at the pause point.

## Batch And Concurrency

There are two distinct concurrency layers to keep in mind:

- `worker_count` controls native worker parallelism inside a compiled graph
- `abatch(...)` schedules multiple graph invocations concurrently from Python

Use `batch(...)` when deterministic in-process sequencing is more useful than outer concurrency. Use `abatch(...)` when you want multiple invocations active at the same time.

## Where To Look Next

- API summary: [`../reference/api.md`](../reference/api.md)
- Runtime model: [`../concepts/runtime-model.md`](../concepts/runtime-model.md)
- Validation commands: [`../operations/validation.md`](../operations/validation.md)
