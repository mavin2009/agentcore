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

## Use Declared Schema Reducers

If your state schema is a declared `TypedDict` or similar annotation-bearing type, AgentCore can infer a small reducer subset directly from supported `Annotated[...]` metadata on join barriers.

```python
import operator
from typing_extensions import Annotated, TypedDict

from agentcore.graph import START, StateGraph


class ReviewState(TypedDict, total=False):
    score: Annotated[int, operator.add]
    notes: Annotated[list[str], operator.add]
    approved: Annotated[bool, operator.or_]
    summary: str


graph = StateGraph(ReviewState, name="schema_join", worker_count=4)
graph.add_fanout("fanout")
graph.add_node("left", lambda state, config: {"score": 1, "notes": ["left"], "approved": False})
graph.add_node("right", lambda state, config: {"score": 2, "notes": ["right"], "approved": True})
graph.add_join(
    "join",
    lambda state, config: {
        "summary": f"{state['score']}::{','.join(state['notes'])}::{state['approved']}",
    },
)
graph.add_edge(START, "fanout")
graph.add_edge("fanout", "left")
graph.add_edge("fanout", "right")
graph.add_edge(["left", "right"], "join")
graph.set_finish_point("join")
```

The currently inferred reducer subset is deliberately small:

- `Annotated[int, operator.add]`
- `Annotated[int, min]`
- `Annotated[int, max]`
- `Annotated[list[T], operator.add]`
- `Annotated[list[dict], add_messages]`
- `Annotated[bool, operator.or_]`
- `Annotated[bool, operator.and_]`

List concatenation is handled by the native join engine as tagged sequence concatenation and is returned to Python as a regular list. For merge behavior outside that subset, keep the graph explicit with `merge={...}` on the join node or by using a dedicated aggregation step.

## Use Message State

Agent-style message history can use `MessagesState` or an explicit `Annotated[..., add_messages]` field. At join barriers, this compiles to a native message merge: new ids append, matching ids replace the existing message in place, and messages without ids append.

```python
from agentcore.graph import START, MessagesState, StateGraph


class AgentState(MessagesState, total=False):
    summary: str


graph = StateGraph(AgentState, name="message_join", worker_count=4)
graph.add_fanout("fanout")
graph.add_node("draft", lambda state, config: {
    "messages": [{"id": "draft", "role": "assistant", "content": "draft answer"}],
})
graph.add_node("revise", lambda state, config: {
    "messages": [{"id": "draft", "role": "assistant", "content": "revised answer"}],
})
graph.add_join("join", lambda state, config: {
    "summary": state["messages"][-1]["content"],
})
graph.add_edge(START, "fanout")
graph.add_edge("fanout", "draft")
graph.add_edge("fanout", "revise")
graph.add_edge(["draft", "revise"], "join")
graph.set_finish_point("join")
```

## Reuse Prompt Templates

Prompt construction is intentionally a small Python layer over the native model registry rather than a second orchestration system. Templates render into the same values already accepted by `compiled.models.invoke(...)` and `runtime.invoke_model(...)`.

```python
from agentcore import ChatPromptTemplate, PromptTemplate, StateGraph
from agentcore.graph import END, START


summary_prompt = PromptTemplate(
    "Summarize the request in {style} style.\n\nRequest:\n{request}"
).partial(style="brief")

review_prompt = ChatPromptTemplate.from_messages(
    [
        ("system", "You are a careful code reviewer."),
        ("user", "Review this diff:\n{diff}"),
    ]
)


def summarize(state, config, runtime):
    prompt = summary_prompt.render(request=state["request"])
    review = review_prompt.render(diff=state["diff"])
    summary = runtime.invoke_model(
        "summarizer",
        prompt,
        decode="text",
    )
    critique = runtime.invoke_model(
        "reviewer",
        review,
        decode="text",
    )
    return {"summary": summary, "critique": critique}


graph = StateGraph(dict, name="prompt_demo")
graph.add_node("summarize", summarize)
graph.add_edge(START, "summarize")
graph.add_edge("summarize", END)
```

Rendered prompt objects can also be passed to `compiled.models.invoke(...)` directly:

```python
compiled = graph.compile()

reply = compiled.models.invoke(
    "summarizer",
    summary_prompt.render(request="Explain checkpoint commit rules."),
    decode="text",
)
```

For the built-in native chat adapters, rendered chat prompts default to role-prefixed text because the current provider adapters consume text prompt payloads. If you register a custom Python-backed model handler that expects structured messages, use:

```python
messages = review_prompt.render(diff="...").to_model_input(mode="messages")
```

That returns a JSON-friendly message list such as `[{ "role": "system", "content": "..." }, ...]`.

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

Metadata returned by `invoke_with_metadata(...)` also includes `details["intelligence"]` when the run commits intelligence records.

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

## Use The Intelligence State Model

If a workflow needs structured reasoning state instead of one large JSON field, Python nodes can use the grouped `runtime.intelligence` view.

```python
from agentcore.graph import END, START, RuntimeContext, StateGraph


def analyze(state, config, runtime: RuntimeContext):
    runtime.intelligence.upsert_task(
        "triage",
        title="Inspect inbound request",
        owner="planner",
        status="in_progress",
        priority=4,
        details={"request_id": state["request_id"]},
    )
    runtime.intelligence.upsert_claim(
        "needs-escalation",
        subject="request",
        relation="requires",
        object="escalation",
        status="proposed",
        confidence=0.74,
        statement={"reason": "account mismatch"},
    )
    runtime.intelligence.add_evidence(
        "ticket-snippet",
        kind="ticket",
        source="support",
        claim_key="needs-escalation",
        content={"excerpt": state["excerpt"]},
        confidence=0.82,
    )
    runtime.intelligence.record_decision(
        "send-to-review",
        task_key="triage",
        claim_key="needs-escalation",
        status="selected",
        summary={"queue": "human-review"},
        confidence=0.91,
    )
    runtime.intelligence.remember(
        "customer-tone",
        layer="working",
        scope="request",
        content={"tone": "urgent"},
        importance=0.6,
    )

    summary = runtime.intelligence.summary()
    supporting_claims = runtime.intelligence.supporting_claims(task_key="triage", limit=3)
    action_candidates = runtime.intelligence.action_candidates(
        owner="planner",
        subject="request",
        relation="requires",
        object="escalation",
        limit=2,
    )
    semantic_claims = runtime.intelligence.query(
        kind="claims",
        subject="request",
        relation="requires",
        object="escalation",
    )
    related = runtime.intelligence.related(task_key="triage", hops=2)
    next_task = runtime.intelligence.next_task(owner="planner")
    recalled_memories = runtime.intelligence.recall(scope="request", limit=3)
    focus = runtime.intelligence.focus(owner="planner", scope="request", limit=3)
    return {
        "decision_count": summary["counts"]["decisions"],
        "supporting_claim_count": supporting_claims["counts"]["claims"],
        "action_candidate_key": action_candidates["tasks"][0]["key"],
        "semantic_claim_count": semantic_claims["counts"]["claims"],
        "related_claim_count": related["counts"]["claims"],
        "next_task_key": None if next_task is None else next_task["key"],
        "working_memory_count": recalled_memories["counts"]["memories"],
        "focus_claim_count": focus["counts"]["claims"],
    }


graph = StateGraph(dict, name="intelligence_demo")
graph.add_node("analyze", analyze)
graph.add_edge(START, "analyze")
graph.add_edge("analyze", END)

details = graph.compile().invoke_with_metadata(
    {"request_id": "req-7", "excerpt": "Please fix this today"}
)
print(details["intelligence"]["counts"])
print(details["intelligence"]["claims"][0]["key"])
```

The implemented snapshot structure is:

- `counts`
- `tasks`
- `claims`
- `evidence`
- `decisions`
- `memories`

This is useful when you want the runtime to preserve active work items, asserted claims, supporting evidence, accepted or rejected decisions, and short-lived or session-local memory through the same checkpoint/replay path as ordinary state and persistent subgraph sessions.

The grouped operational surface is intentionally small:

- `runtime.intelligence.snapshot()` returns the current intelligence snapshot, including staged writes from the active callback.
- `runtime.intelligence.summary()` returns counts plus task-status, claim-status, decision-status, and memory-layer tallies.
- `runtime.intelligence.count(...)` and `runtime.intelligence.exists(...)` support fast presence and threshold checks without forcing Python code to materialize and scan snapshots.
- `runtime.intelligence.query(...)` filters records natively without forcing application code to scan the whole snapshot every time. Claim queries can also match directly on `subject`, `relation`, and `object`.
- `runtime.intelligence.supporting_claims(...)` ranks claims natively from linked evidence, decisions, and memories, which is useful when a workflow wants the best-supported claims for one task instead of a raw filtered list.
- `runtime.intelligence.action_candidates(...)` returns tasks only and ranks them from the linked evidence/decision/memory support around task, claim, or semantic-claim anchors, which is useful when a workflow wants a small operational shortlist rather than a full focus set.
- `runtime.intelligence.related(...)` expands the records connected to one task or claim, with `hops=1` by default and bounded multi-hop traversal when needed.
- `runtime.intelligence.agenda(...)` returns tasks ranked natively by operational priority.
- `runtime.intelligence.next_task(...)` returns the first ranked task or `None`.
- `runtime.intelligence.recall(...)` returns memories ranked natively by importance.
- `runtime.intelligence.focus(...)` returns a bounded, cross-kind focus set around the active task/memory/evidence frontier, favoring direct anchors and decisive support over weak-link volume. Claim-semantic filters participate in those anchors.
- `runtime.intelligence.route(...)` selects the first matching route from an ordered rule list.

For conditional edges, `IntelligenceRule` and `IntelligenceRouter` provide a compact declarative layer over that same native query path.

Nodes can also subscribe to committed intelligence changes directly:

```python
from agentcore.graph import IntelligenceSubscription, START, StateGraph


def write_claim(state, config, runtime):
    runtime.intelligence.upsert_claim(
        "claim:reactive",
        subject="agentcore",
        relation="supports",
        object="reactive_runtime",
        status="supported",
    )
    return {}


def react_to_supported_claim(state, config):
    return {"reactive_seen": True}


graph = StateGraph(dict, name="intelligence_reactive")
graph.add_node("write", write_claim)
graph.add_node(
    "react",
    react_to_supported_claim,
    kind="aggregate",
    stop_after=True,
    intelligence_subscriptions=[
        IntelligenceSubscription(
            subject="agentcore",
            relation="supports",
            object="reactive_runtime",
        ),
    ],
)
graph.add_edge(START, "write")
```

That subscription is compiled into native graph metadata. When a committed intelligence delta matches it, the runtime schedules the subscribed node through the same checkpointed reactive frontier mechanism used for other graph-native reactive execution. The same `subject` / `relation` / `object` fields are also available on `IntelligenceRule` for claim-centric routing.

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

When parent and child graphs already share a declared schema, there is also a shorter shared-state form:

```python
parent.add_node("specialist_child", specialist_graph)
```

That shorthand binds the overlapping schema fields by name for both input and output. It is intended for the shared-state default only. Use `add_subgraph(...)` when you need explicit field bindings, namespaces, persistent sessions, or knowledge-graph propagation settings.

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
