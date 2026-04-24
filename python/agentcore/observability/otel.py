from __future__ import annotations

import importlib
import importlib.metadata
import threading
import time
from collections.abc import Callable, Mapping, Sequence
from typing import Any


def _package_version() -> str | None:
    try:
        return importlib.metadata.version("agentcore-graph")
    except importlib.metadata.PackageNotFoundError:
        return None


def _coerce_span_attribute(value: Any) -> Any:
    if value is None:
        return None
    if isinstance(value, (bool, int, float, str)):
        return value
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray, memoryview)):
        normalized: list[Any] = []
        for item in value:
            coerced = _coerce_span_attribute(item)
            if coerced is None:
                continue
            if not isinstance(coerced, (bool, int, float, str)):
                return str(list(value))
            normalized.append(coerced)
        return normalized
    return str(value)


def _namespace_path(event: Mapping[str, Any]) -> str | None:
    frames = event.get("namespaces")
    if not isinstance(frames, Sequence):
        return None
    parts: list[str] = []
    for frame in frames:
        if not isinstance(frame, Mapping):
            continue
        graph_name = str(frame.get("graph_name", "") or "")
        node_name = str(frame.get("node_name", "") or "")
        session_id = str(frame.get("session_id", "") or "")
        revision = int(frame.get("session_revision", 0) or 0)
        segment = f"{graph_name}:{node_name}" if graph_name or node_name else ""
        if session_id:
            segment += f"[{session_id}@{revision}]"
        if segment:
            parts.append(segment)
    return " > ".join(parts) if parts else None


def _status_is_error(status: str | None) -> bool:
    return str(status or "").strip().lower() in {"failed", "hard_fail", "cancelled"}


class OpenTelemetryObserver:
    """Emit AgentCore run and node telemetry through OpenTelemetry.

    The observer is opt-in and Python-side: it consumes metadata and trace events
    that AgentCore already records, instead of introducing extra native hot-path
    instrumentation when callers do not need telemetry.
    """

    def __init__(
        self,
        *,
        tracer: Any | None = None,
        meter: Any | None = None,
        tracer_provider: Any | None = None,
        meter_provider: Any | None = None,
        instrumentation_scope: str = "agentcore",
        instrumentation_version: str | None = None,
        run_span_name_prefix: str = "agentcore",
        emit_node_spans: bool = True,
        emit_metrics: bool = True,
    ) -> None:
        self._provided_tracer = tracer
        self._provided_meter = meter
        self._tracer_provider = tracer_provider
        self._meter_provider = meter_provider
        self._instrumentation_scope = str(instrumentation_scope)
        self._instrumentation_version = instrumentation_version or _package_version()
        self._run_span_name_prefix = str(run_span_name_prefix)
        self._emit_node_spans = bool(emit_node_spans)
        self._emit_metrics = bool(emit_metrics)
        self._lock = threading.Lock()
        self._otel_trace = None
        self._otel_metrics = None
        self._otel_status = None
        self._otel_status_code = None
        self._otel_span_kind = None
        self._tracer = None
        self._meter = None
        self._run_counter = None
        self._run_duration = None
        self._trace_event_counter = None
        self._node_counter = None
        self._node_duration = None

    def capture_details(
        self,
        call: Callable[[], dict[str, Any]],
        *,
        graph_name: str,
        operation: str,
        config: Mapping[str, Any] | None = None,
        include_subgraphs: bool | None = None,
    ) -> dict[str, Any]:
        tracer = self._get_tracer()
        span_name = f"{self._run_span_name_prefix}.{operation}"
        with tracer.start_as_current_span(
            span_name,
            attributes=self._run_attributes(
                graph_name=graph_name,
                operation=operation,
                config=config,
                include_subgraphs=include_subgraphs,
            ),
            kind=self._span_kind_internal(),
        ) as span:
            started_ns = time.perf_counter_ns()
            try:
                details = call()
            except Exception as exc:
                self._record_exception(span, exc)
                raise
            elapsed_ns = time.perf_counter_ns() - started_ns
            self._finalize_run_span(
                span,
                graph_name=graph_name,
                operation=operation,
                details=details,
                elapsed_ns=elapsed_ns,
            )
            return details

    def capture_stream(
        self,
        call: Callable[[], Sequence[dict[str, Any]]],
        *,
        graph_name: str,
        operation: str,
        config: Mapping[str, Any] | None = None,
        include_subgraphs: bool | None = None,
    ) -> list[dict[str, Any]]:
        tracer = self._get_tracer()
        span_name = f"{self._run_span_name_prefix}.{operation}"
        with tracer.start_as_current_span(
            span_name,
            attributes=self._run_attributes(
                graph_name=graph_name,
                operation=operation,
                config=config,
                include_subgraphs=include_subgraphs,
            ),
            kind=self._span_kind_internal(),
        ) as span:
            started_ns = time.perf_counter_ns()
            try:
                events = list(call())
            except Exception as exc:
                self._record_exception(span, exc)
                raise
            elapsed_ns = time.perf_counter_ns() - started_ns
            self._finalize_stream_span(
                span,
                graph_name=graph_name,
                operation=operation,
                events=events,
                elapsed_ns=elapsed_ns,
            )
            return events

    def _get_tracer(self) -> Any:
        if self._provided_tracer is not None:
            return self._provided_tracer
        with self._lock:
            if self._tracer is not None:
                return self._tracer
            trace_api = self._import_trace_api()
            if self._tracer_provider is None:
                tracer = trace_api.get_tracer(
                    self._instrumentation_scope,
                    self._instrumentation_version,
                )
            else:
                tracer = self._tracer_provider.get_tracer(
                    self._instrumentation_scope,
                    self._instrumentation_version,
                )
            self._tracer = tracer
            return tracer

    def _get_meter(self) -> Any | None:
        if not self._emit_metrics:
            return None
        if self._provided_meter is not None:
            return self._provided_meter
        with self._lock:
            if self._meter is not None:
                return self._meter
            metrics_api = self._import_metrics_api()
            if self._meter_provider is None:
                meter = metrics_api.get_meter(
                    self._instrumentation_scope,
                    self._instrumentation_version,
                )
            else:
                meter = self._meter_provider.get_meter(
                    self._instrumentation_scope,
                    self._instrumentation_version,
                )
            self._meter = meter
            return meter

    def _import_trace_api(self) -> Any:
        if self._otel_trace is None:
            try:
                module = importlib.import_module("opentelemetry.trace")
            except ImportError as exc:
                raise ImportError(
                    "OpenTelemetry support requires the opentelemetry Python packages. "
                    "Install agentcore-graph[otel] or add opentelemetry-api/opentelemetry-sdk."
                ) from exc
            self._otel_trace = module
            self._otel_status = getattr(module, "Status", None)
            self._otel_status_code = getattr(module, "StatusCode", None)
            self._otel_span_kind = getattr(module, "SpanKind", None)
        return self._otel_trace

    def _import_metrics_api(self) -> Any:
        if self._otel_metrics is None:
            try:
                self._otel_metrics = importlib.import_module("opentelemetry.metrics")
            except ImportError as exc:
                raise ImportError(
                    "OpenTelemetry metrics require the opentelemetry Python packages. "
                    "Install agentcore-graph[otel] or add opentelemetry-api/opentelemetry-sdk."
                ) from exc
        return self._otel_metrics

    def _span_kind_internal(self) -> Any:
        if self._otel_span_kind is None:
            if self._provided_tracer is not None:
                return None
            try:
                self._import_trace_api()
            except ImportError:
                return None
        return getattr(self._otel_span_kind, "INTERNAL", None)

    def _run_attributes(
        self,
        *,
        graph_name: str,
        operation: str,
        config: Mapping[str, Any] | None,
        include_subgraphs: bool | None,
    ) -> dict[str, Any]:
        attributes: dict[str, Any] = {
            "agentcore.graph.name": graph_name,
            "agentcore.operation": operation,
        }
        if include_subgraphs is not None:
            attributes["agentcore.include_subgraphs"] = bool(include_subgraphs)
        if isinstance(config, Mapping):
            configurable = config.get("configurable")
            if isinstance(configurable, Mapping):
                attributes["agentcore.configurable.count"] = len(configurable)
            tags = config.get("tags")
            if isinstance(tags, Sequence) and not isinstance(tags, (str, bytes, bytearray, memoryview)):
                attributes["agentcore.tags.count"] = len(tags)
        return attributes

    def _finalize_run_span(
        self,
        span: Any,
        *,
        graph_name: str,
        operation: str,
        details: Mapping[str, Any],
        elapsed_ns: int,
    ) -> None:
        summary = details.get("summary")
        summary_mapping = dict(summary.items()) if hasattr(summary, "items") else {}
        proof = details.get("proof")
        proof_mapping = dict(proof.items()) if hasattr(proof, "items") else {}
        trace_events = details.get("trace")
        events = list(trace_events) if isinstance(trace_events, Sequence) else []
        status = str(summary_mapping.get("status", "unknown") or "unknown")
        self._set_span_attributes(
            span,
            {
                "agentcore.run.id": summary_mapping.get("run_id"),
                "agentcore.run.status": status,
                "agentcore.run.steps_executed": summary_mapping.get("steps_executed"),
                "agentcore.checkpoint.id": summary_mapping.get("checkpoint_id"),
                "agentcore.trace.event_count": len(events),
                "agentcore.proof.snapshot_digest": proof_mapping.get("snapshot_digest"),
                "agentcore.proof.trace_digest": proof_mapping.get("trace_digest"),
                "agentcore.proof.combined_digest": proof_mapping.get("combined_digest"),
                "agentcore.elapsed_ns": int(elapsed_ns),
            },
        )
        self._set_span_status(span, status)
        self._record_run_metrics(
            graph_name=graph_name,
            operation=operation,
            status=status,
            elapsed_ns=int(elapsed_ns),
            event_count=len(events),
        )
        self._record_node_spans(events)
        self._record_node_metrics(events)

    def _finalize_stream_span(
        self,
        span: Any,
        *,
        graph_name: str,
        operation: str,
        events: Sequence[Mapping[str, Any]],
        elapsed_ns: int,
    ) -> None:
        run_id = None
        if events:
            run_id = events[0].get("run_id")
        self._set_span_attributes(
            span,
            {
                "agentcore.run.id": run_id,
                "agentcore.run.status": "streamed",
                "agentcore.trace.event_count": len(events),
                "agentcore.elapsed_ns": int(elapsed_ns),
            },
        )
        self._record_run_metrics(
            graph_name=graph_name,
            operation=operation,
            status="streamed",
            elapsed_ns=int(elapsed_ns),
            event_count=len(events),
        )
        self._record_node_spans(events)
        self._record_node_metrics(events)

    def _record_run_metrics(
        self,
        *,
        graph_name: str,
        operation: str,
        status: str,
        elapsed_ns: int,
        event_count: int,
    ) -> None:
        meter = self._get_meter()
        if meter is None:
            return
        attributes = {
            "agentcore.graph.name": graph_name,
            "agentcore.operation": operation,
            "agentcore.run.status": status,
        }
        if self._run_counter is None:
            self._run_counter = meter.create_counter(
                "agentcore.run.executions",
                unit="{run}",
                description="Count of AgentCore runs recorded through the OpenTelemetry observer.",
            )
        if self._run_duration is None:
            self._run_duration = meter.create_histogram(
                "agentcore.run.duration",
                unit="ns",
                description="End-to-end duration for AgentCore run operations.",
            )
        if self._trace_event_counter is None:
            self._trace_event_counter = meter.create_counter(
                "agentcore.trace.events",
                unit="{event}",
                description="Trace events observed for AgentCore runs.",
            )
        self._run_counter.add(1, attributes=attributes)
        self._run_duration.record(int(elapsed_ns), attributes=attributes)
        self._trace_event_counter.add(int(event_count), attributes=attributes)

    def _record_node_metrics(self, events: Sequence[Mapping[str, Any]]) -> None:
        meter = self._get_meter()
        if meter is None or not events:
            return
        if self._node_counter is None:
            self._node_counter = meter.create_counter(
                "agentcore.node.executions",
                unit="{node}",
                description="Count of AgentCore node executions observed through the OpenTelemetry observer.",
            )
        if self._node_duration is None:
            self._node_duration = meter.create_histogram(
                "agentcore.node.duration",
                unit="ns",
                description="Node execution duration observed through the OpenTelemetry observer.",
            )
        for event in events:
            attributes = {
                "agentcore.graph.name": event.get("graph_name"),
                "agentcore.node.name": event.get("node_name"),
                "agentcore.node.kind": event.get("node_kind"),
                "agentcore.result": event.get("result"),
            }
            self._node_counter.add(1, attributes=attributes)
            self._node_duration.record(int(event.get("duration_ns", 0) or 0), attributes=attributes)

    def _record_node_spans(self, events: Sequence[Mapping[str, Any]]) -> None:
        if not self._emit_node_spans or not events:
            return
        tracer = self._get_tracer()
        for event in events:
            node_name = str(event.get("node_name", "node") or "node")
            attributes = {
                "agentcore.run.id": event.get("run_id"),
                "agentcore.sequence": event.get("sequence"),
                "agentcore.graph.id": event.get("graph_id"),
                "agentcore.graph.name": event.get("graph_name"),
                "agentcore.node.id": event.get("node_id"),
                "agentcore.node.name": node_name,
                "agentcore.node.kind": event.get("node_kind"),
                "agentcore.branch.id": event.get("branch_id"),
                "agentcore.checkpoint.id": event.get("checkpoint_id"),
                "agentcore.result": event.get("result"),
                "agentcore.duration_ns": event.get("duration_ns"),
                "agentcore.patch.count": event.get("patch_count"),
                "agentcore.flags": event.get("flags"),
                "agentcore.confidence": event.get("confidence"),
                "agentcore.session.id": event.get("session_id"),
                "agentcore.session.revision": event.get("session_revision"),
                "agentcore.namespace.depth": len(event.get("namespaces", []) or []),
                "agentcore.namespace.path": _namespace_path(event),
                "agentcore.monotonic_start_ns": event.get("ts_start_ns"),
                "agentcore.monotonic_end_ns": event.get("ts_end_ns"),
            }
            with tracer.start_as_current_span(
                node_name,
                attributes=self._clean_attributes(attributes),
                kind=self._span_kind_internal(),
            ) as span:
                self._set_span_status(span, str(event.get("result", "") or ""))

    def _record_exception(self, span: Any, exc: BaseException) -> None:
        if hasattr(span, "record_exception"):
            span.record_exception(exc)
        self._set_span_status(span, "failed")

    def _set_span_status(self, span: Any, status: str) -> None:
        if not hasattr(span, "set_status"):
            return
        if self._otel_status is None or self._otel_status_code is None:
            try:
                self._import_trace_api()
            except ImportError:
                return
        status_code = None
        if _status_is_error(status):
            status_code = getattr(self._otel_status_code, "ERROR", None)
        else:
            status_code = getattr(self._otel_status_code, "UNSET", None)
        if status_code is None or self._otel_status is None:
            return
        description = status if _status_is_error(status) else None
        span.set_status(self._otel_status(status_code, description))

    def _set_span_attributes(self, span: Any, attributes: Mapping[str, Any]) -> None:
        if hasattr(span, "set_attributes"):
            span.set_attributes(self._clean_attributes(attributes))
            return
        for key, value in self._clean_attributes(attributes).items():
            if hasattr(span, "set_attribute"):
                span.set_attribute(key, value)

    def _clean_attributes(self, attributes: Mapping[str, Any]) -> dict[str, Any]:
        cleaned: dict[str, Any] = {}
        for key, value in attributes.items():
            if value is None:
                continue
            coerced = _coerce_span_attribute(value)
            if coerced is None:
                continue
            cleaned[str(key)] = coerced
        return cleaned
