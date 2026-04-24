from __future__ import annotations

from agentcore import Command, END, START, StateGraph
from agentcore.observability import OpenTelemetryObserver


class FakeStatus:
    def __init__(self, code, description=None):
        self.code = code
        self.description = description


class FakeStatusCode:
    ERROR = "ERROR"
    UNSET = "UNSET"


class FakeSpanKind:
    INTERNAL = "INTERNAL"


class FakeSpan:
    def __init__(self, tracer, name, *, attributes=None, kind=None):
        self.tracer = tracer
        self.name = name
        self.kind = kind
        self.parent = tracer.stack[-1] if tracer.stack else None
        self.attributes = dict(attributes or {})
        self.status = None
        self.exceptions = []

    def __enter__(self):
        self.tracer.stack.append(self)
        return self

    def __exit__(self, exc_type, exc, tb):
        popped = self.tracer.stack.pop()
        assert popped is self
        if exc is not None:
            self.record_exception(exc)
        return False

    def set_attribute(self, key, value):
        self.attributes[key] = value

    def set_attributes(self, attributes):
        self.attributes.update(dict(attributes))

    def set_status(self, status):
        self.status = status

    def record_exception(self, exc):
        self.exceptions.append(str(exc))


class FakeTracer:
    def __init__(self):
        self.spans = []
        self.stack = []

    def start_as_current_span(self, name, *, attributes=None, kind=None):
        span = FakeSpan(self, name, attributes=attributes, kind=kind)
        self.spans.append(span)
        return span


class FakeCounter:
    def __init__(self, name):
        self.name = name
        self.measurements = []

    def add(self, value, attributes=None):
        self.measurements.append((value, dict(attributes or {})))


class FakeHistogram:
    def __init__(self, name):
        self.name = name
        self.measurements = []

    def record(self, value, attributes=None):
        self.measurements.append((value, dict(attributes or {})))


class FakeMeter:
    def __init__(self):
        self.counters = {}
        self.histograms = {}

    def create_counter(self, name, **kwargs):
        counter = FakeCounter(name)
        self.counters[name] = counter
        return counter

    def create_histogram(self, name, **kwargs):
        histogram = FakeHistogram(name)
        self.histograms[name] = histogram
        return histogram


def build_counter_graph():
    def planner(state, config, runtime):
        count = int(state.get("count", 0)) + 1
        history = list(state.get("history", []))
        history.append(f"step-{count}")
        return {"count": count, "history": history}

    def route(state, config):
        return END if int(state.get("count", 0)) >= 2 else "planner"

    graph = StateGraph(dict, name="python_otel_counter_smoke", worker_count=2)
    graph.add_node("planner", planner)
    graph.add_edge(START, "planner")
    graph.add_conditional_edges("planner", route, {END: END, "planner": "planner"})
    return graph.compile()


def build_pause_graph():
    def worker(state, config, runtime):
        if not bool(state.get("ready", False)):
            return Command(update={"ready": True}, wait=True)

        count = int(state.get("count", 0)) + 1
        history = list(state.get("history", []))
        history.append(f"resume-{count}")
        return {"count": count, "history": history}

    def route(state, config):
        return END if int(state.get("count", 0)) >= 2 else "worker"

    graph = StateGraph(dict, name="python_otel_resume_smoke", worker_count=2)
    graph.add_node("worker", worker)
    graph.add_edge(START, "worker")
    graph.add_conditional_edges("worker", route, {END: END, "worker": "worker"})
    return graph.compile()


fake_tracer = FakeTracer()
fake_meter = FakeMeter()
observer = OpenTelemetryObserver(
    tracer=fake_tracer,
    meter=fake_meter,
    emit_node_spans=True,
    emit_metrics=True,
)
observer._otel_status = FakeStatus
observer._otel_status_code = FakeStatusCode
observer._otel_span_kind = FakeSpanKind


compiled = build_counter_graph()
details = compiled.invoke_with_metadata(
    {"count": 0, "history": []},
    config={"tags": ["otel", "smoke"]},
    telemetry=observer,
)
assert details["state"]["count"] == 2
assert [event["node_name"] for event in details["trace"]] == ["planner", "planner"]

final_state = compiled.invoke(
    {"count": 0, "history": []},
    config={"tags": ["otel", "invoke"]},
    telemetry=observer,
)
assert final_state["count"] == 2

events = list(
    compiled.stream(
        {"count": 0, "history": []},
        config={"tags": ["otel", "stream"]},
        telemetry=observer,
    )
)
assert [event["node_name"] for event in events] == ["planner", "planner"]

paused_graph = build_pause_graph()
paused = paused_graph.invoke_until_pause_with_metadata(
    {"ready": False, "count": 0, "history": []},
    telemetry=observer,
)
assert paused["summary"]["status"] == "paused"
resumed = paused_graph.resume_with_metadata(paused["summary"]["checkpoint_id"], telemetry=observer)
assert resumed["summary"]["status"] == "completed"
assert resumed["state"]["count"] == 2

root_spans = [span for span in fake_tracer.spans if span.parent is None]
root_names = [span.name for span in root_spans]
assert "agentcore.invoke_with_metadata" in root_names
assert "agentcore.invoke" in root_names
assert "agentcore.stream" in root_names
assert "agentcore.invoke_until_pause_with_metadata" in root_names
assert "agentcore.resume_with_metadata" in root_names

invoke_with_metadata_span = next(span for span in root_spans if span.name == "agentcore.invoke_with_metadata")
assert invoke_with_metadata_span.attributes["agentcore.graph.name"] == "python_otel_counter_smoke"
assert invoke_with_metadata_span.attributes["agentcore.operation"] == "invoke_with_metadata"
assert invoke_with_metadata_span.attributes["agentcore.run.status"] == "completed"
assert invoke_with_metadata_span.attributes["agentcore.trace.event_count"] == 2
assert invoke_with_metadata_span.attributes["agentcore.proof.combined_digest"] != 0
assert invoke_with_metadata_span.status.code == "UNSET"

paused_span = next(span for span in root_spans if span.name == "agentcore.invoke_until_pause_with_metadata")
assert paused_span.attributes["agentcore.run.status"] == "paused"

planner_spans = [
    span for span in fake_tracer.spans if span.name == "planner" and span.parent is not None
]
assert len(planner_spans) >= 2
assert all(span.parent is not None for span in planner_spans)
assert all(span.attributes["agentcore.node.name"] == "planner" for span in planner_spans)
assert all(span.attributes["agentcore.result"] == "success" for span in planner_spans)
assert all(span.attributes["agentcore.node.kind"] == "compute" for span in planner_spans)
assert all(span.attributes["agentcore.duration_ns"] >= 0 for span in planner_spans)

assert set(fake_meter.counters) == {
    "agentcore.run.executions",
    "agentcore.trace.events",
    "agentcore.node.executions",
}
assert set(fake_meter.histograms) == {
    "agentcore.run.duration",
    "agentcore.node.duration",
}
assert len(fake_meter.counters["agentcore.run.executions"].measurements) == 5
assert len(fake_meter.histograms["agentcore.run.duration"].measurements) == 5
assert len(fake_meter.counters["agentcore.node.executions"].measurements) >= 8
assert len(fake_meter.histograms["agentcore.node.duration"].measurements) >= 8

print("python opentelemetry smoke passed")
