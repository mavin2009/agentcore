# OpenTelemetry

AgentCore exposes OpenTelemetry as an opt-in Python observer over the runtime metadata and trace events it already records.

Use this integration when you want graph runs to appear beside the rest of your service telemetry: request traces, queue workers, tool calls, model calls, logs, and deployment metrics. AgentCore does not require OpenTelemetry for normal execution; the observer is attached only when you pass `telemetry=...`.

That shape is intentional:

- plain `invoke(...)` stays on the lean native path unless telemetry is requested
- `invoke_with_metadata(...)`, pause/resume, and `stream(...)` can all emit spans and metrics through the same observer
- the observer works with the global OpenTelemetry tracer and meter providers or with explicitly supplied tracer/meter instances

## Install

If you want the packaged OpenTelemetry dependencies, install the optional extra:

```bash
python3 -m pip install "agentcore-graph[otel]"
```

You can also install `opentelemetry-api` and `opentelemetry-sdk` directly if that fits your environment better.

Your application remains responsible for configuring exporters, processors, and resource attributes. AgentCore uses the active global tracer and meter providers unless you pass explicit tracer or meter objects into `OpenTelemetryObserver`.

## Basic Use

```python
from agentcore.graph import END, START, StateGraph
from agentcore.observability import OpenTelemetryObserver


def step(state, config):
    count = int(state.get("count", 0)) + 1
    return {"count": count}


def route(state, config):
    return END if state["count"] >= 2 else "step"


graph = StateGraph(dict, name="otel_demo", worker_count=2)
graph.add_node("step", step)
graph.add_edge(START, "step")
graph.add_conditional_edges("step", route, {END: END, "step": "step"})

compiled = graph.compile()
observer = OpenTelemetryObserver()

details = compiled.invoke_with_metadata(
    {"count": 0},
    telemetry=observer,
)
```

If a global tracer provider and meter provider are already configured, `OpenTelemetryObserver()` uses them automatically.

For one-off instrumentation, the compact form is also accepted:

```python
final_state = compiled.invoke({"count": 0}, telemetry=True)
```

That constructs a default `OpenTelemetryObserver()` behind the scenes.

## Where To Attach It

Common choices:

- Use `telemetry=True` for a quick local check.
- Create one `OpenTelemetryObserver()` near your application startup and pass it to graph calls.
- Disable child node spans with `emit_node_spans=False` when you only need run-level observability.
- Prefer `invoke_with_metadata(..., telemetry=observer)` when you also want proof, checkpoint, trace, or intelligence metadata returned to your application.

## What Gets Emitted

When telemetry is enabled, the observer emits:

- one run span per `invoke(...)`, `invoke_with_metadata(...)`, `invoke_until_pause_with_metadata(...)`, `resume_with_metadata(...)`, or `stream(...)`
- one child node span per visible runtime event, unless node spans are disabled
- run and node metrics through OpenTelemetry counters and histograms

Current metric names:

- `agentcore.run.executions`
- `agentcore.run.duration`
- `agentcore.trace.events`
- `agentcore.node.executions`
- `agentcore.node.duration`

Current span attributes include:

- `agentcore.graph.name`
- `agentcore.operation`
- `agentcore.run.id`
- `agentcore.run.status`
- `agentcore.run.steps_executed`
- `agentcore.trace.event_count`
- `agentcore.proof.combined_digest`
- `agentcore.node.name`
- `agentcore.node.kind`
- `agentcore.result`
- `agentcore.duration_ns`
- `agentcore.session.id`
- `agentcore.session.revision`
- `agentcore.namespace.path`

## Performance Notes

The OpenTelemetry layer is intentionally outside the native execution loop.

- Without `telemetry=...`, AgentCore does not add this observer work to the Python call path.
- With `telemetry=...`, the observer consumes the trace data already produced for metadata and streaming rather than adding another native instrumentation system.
- For `invoke(...)`, enabling telemetry routes through the same metadata path used by `invoke_with_metadata(...)` so the observer has enough information to build run and node spans.

If you want lower observer overhead while keeping run-level telemetry, disable child spans:

```python
observer = OpenTelemetryObserver(emit_node_spans=False)
```

## Validation

From the repository root:

```bash
cmake -S . -B build -DAGENTCORE_BUILD_PYTHON_BINDINGS=ON
cmake --build build -j
PYTHONPATH=./build/python python3 ./python/tests/opentelemetry_smoke.py
```

The smoke test validates:

- run spans for invoke, stream, pause, and resume
- node spans with runtime attributes
- run and node metrics
- pause/resume telemetry through the same compiled graph API
