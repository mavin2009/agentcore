from __future__ import annotations

import asyncio
import concurrent.futures
import inspect
import operator
from collections.abc import Mapping
from dataclasses import dataclass, field
from typing import Annotated, Any, Iterable, Sequence, TypedDict, get_args, get_origin, get_type_hints

from ..context import (
    ContextAccessor,
    ContextSpec,
    ContextView,
    _attach_context_metadata,
    _begin_context_collection,
    _decorate_stream_event_iter,
    _end_context_collection,
)
from ..adapters.registry import (
    ModelRegistryView,
    ToolRegistryView,
    _decode_response_details,
    _raise_adapter_error,
    encode_adapter_payload,
)
from ..graphstores import GraphNeighborhood, GraphStoreRegistryView
from ..prompts.templates import _coerce_prompt_value_for_model_input
from .. import _agentcore_native as _native

START = "__start__"
END = "__end__"
_RUNTIME_CONFIG_KEY = getattr(_native, "_INTERNAL_RUNTIME_CONFIG_KEY", "__agentcore_runtime__")

_VALID_NODE_KINDS = {
    "compute",
    "control",
    "tool",
    "model",
    "aggregate",
    "human",
}
_VALID_MERGE_STRATEGIES = {
    "require_equal",
    "require_single_writer",
    "last_writer_wins",
    "first_writer_wins",
    "sum_int64",
    "max_int64",
    "min_int64",
    "logical_or",
    "logical_and",
    "concat_sequence",
    "merge_messages",
}
_VALID_INTELLIGENCE_KINDS = {
    "all",
    "task",
    "tasks",
    "claim",
    "claims",
    "evidence",
    "decision",
    "decisions",
    "memory",
    "memories",
}

_MISSING = object()


@dataclass(frozen=True)
class Command:
    update: dict[str, Any] | None = None
    goto: str | None = None
    wait: bool = False


def _coerce_message_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return list(value)
    if isinstance(value, tuple):
        return list(value)
    return [value]


def _message_id(value: Any) -> str | None:
    raw_id = value.get("id") if isinstance(value, Mapping) else getattr(value, "id", None)
    if raw_id is None:
        return None
    text = str(raw_id)
    return text if text else None


def add_messages(left: Any = None, right: Any = None) -> list[Any]:
    """Merge message lists by appending new messages and replacing matching ids."""

    merged: list[Any] = []
    index_by_id: dict[str, int] = {}
    for message in [*_coerce_message_list(left), *_coerce_message_list(right)]:
        message_id = _message_id(message)
        if message_id is not None and message_id in index_by_id:
            merged[index_by_id[message_id]] = message
            continue
        if message_id is not None:
            index_by_id[message_id] = len(merged)
        merged.append(message)
    return merged


add_messages.__agentcore_reducer__ = "add_messages"  # type: ignore[attr-defined]


class MessagesState(TypedDict, total=False):
    messages: Annotated[list[dict[str, Any]], add_messages]


@dataclass(frozen=True)
class IntelligenceRule:
    goto: str
    kind: str | None = None
    key: str | None = None
    key_prefix: str | None = None
    task_key: str | None = None
    claim_key: str | None = None
    subject: str | None = None
    relation: str | None = None
    object: str | None = None
    owner: str | None = None
    source: str | None = None
    scope: str | None = None
    status: str | None = None
    layer: str | None = None
    min_confidence: float | None = None
    min_importance: float | None = None
    min_count: int = 1
    max_count: int | None = None


@dataclass(frozen=True)
class IntelligenceSubscription:
    kind: str | None = None
    key: str | None = None
    key_prefix: str | None = None
    task_key: str | None = None
    claim_key: str | None = None
    subject: str | None = None
    relation: str | None = None
    object: str | None = None
    owner: str | None = None
    source: str | None = None
    scope: str | None = None
    status: str | None = None
    layer: str | None = None
    min_confidence: float | None = None
    min_importance: float | None = None


@dataclass(frozen=True)
class IntelligenceRouter:
    rules: Sequence[IntelligenceRule | Mapping[str, Any]]
    default: str | None = None

    def __call__(self, state: Any, config: Any, runtime: "RuntimeContext") -> str | None:
        return runtime.intelligence.route(self.rules, default=self.default)


@dataclass
class _SubgraphSpec:
    graph: StateGraph | CompiledStateGraph
    namespace: str | None = None
    inputs: dict[str, str] = field(default_factory=dict)
    outputs: dict[str, str] = field(default_factory=dict)
    propagate_knowledge_graph: bool = False
    session_mode: str = "ephemeral"
    session_id_from: str | None = None


@dataclass
class _NodeSpec:
    action: Any | None = None
    kind: str = "compute"
    stop_after: bool = False
    allow_fan_out: bool = False
    create_join_scope: bool = False
    join_incoming_branches: bool = False
    deterministic: bool = False
    read_keys: tuple[str, ...] = ()
    cache_size: int = 16
    merge: dict[str, str] = field(default_factory=dict)
    intelligence_subscriptions: tuple[dict[str, Any], ...] = ()
    context: ContextSpec | None = None
    subgraph: _SubgraphSpec | None = None


def _normalize_config(config: Any) -> dict[str, Any]:
    if config is None:
        return {}
    if isinstance(config, dict):
        return dict(config)
    if hasattr(config, "items"):
        return dict(config.items())
    raise TypeError("config must be a mapping or None")


def _coerce_telemetry_observer(telemetry: Any) -> Any | None:
    if telemetry is None or telemetry is False:
        return None
    if telemetry is True:
        from ..observability import OpenTelemetryObserver

        return OpenTelemetryObserver()
    if hasattr(telemetry, "capture_details") and hasattr(telemetry, "capture_stream"):
        return telemetry
    raise TypeError(
        "telemetry must be None, False, True, or an observer with capture_details(...) and capture_stream(...)"
    )


def _normalize_update(value: Any) -> dict[str, Any]:
    if value is None:
        return {}
    if type(value) is dict:
        return value
    if isinstance(value, dict):
        return dict(value)
    if hasattr(value, "items"):
        return dict(value.items())
    raise TypeError("node updates must be mappings or None")


def _normalize_result(result: Any) -> tuple[dict[str, Any], str | None, bool]:
    if isinstance(result, Command):
        return _normalize_update(result.update), result.goto, bool(result.wait)
    if result is None:
        return {}, None, False
    if isinstance(result, str):
        return {}, result, False
    if isinstance(result, (tuple, list)):
        if len(result) != 2:
            raise TypeError("tuple/list node results must contain exactly two elements")
        return _normalize_update(result[0]), None if result[1] is None else str(result[1]), False
    return _normalize_update(result), None, False


def _positional_arity(function: Any) -> int | None:
    try:
        signature = inspect.signature(function)
    except (TypeError, ValueError):
        return 2

    positional_count = 0
    for parameter in signature.parameters.values():
        if parameter.kind == inspect.Parameter.VAR_POSITIONAL:
            return None
        if parameter.kind in (
            inspect.Parameter.POSITIONAL_ONLY,
            inspect.Parameter.POSITIONAL_OR_KEYWORD,
        ):
            positional_count += 1
    return positional_count


def _compile_callback_invoker(function: Any) -> Any:
    positional_arity = _positional_arity(function)
    if positional_arity is None or positional_arity >= 3:
        def invoke(state: Any, config: Any, runtime: RuntimeContext) -> Any:
            return function(state, config, runtime)

        return invoke

    if positional_arity >= 2:
        def invoke(state: Any, config: Any, runtime: RuntimeContext) -> Any:
            return function(state, config)

        return invoke

    def invoke(state: Any, config: Any, runtime: RuntimeContext) -> Any:
        return function(state)

    return invoke


def _run_awaitable_blocking(awaitable: Any) -> Any:
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        return asyncio.run(awaitable)

    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        return executor.submit(asyncio.run, awaitable).result()


def _resolve_maybe_awaitable(value: Any) -> Any:
    if inspect.isawaitable(value):
        return _run_awaitable_blocking(value)
    return value


def _build_intelligence_filter_spec(
    *,
    kind: str | None,
    key: str | None,
    key_prefix: str | None,
    task_key: str | None,
    claim_key: str | None,
    subject: str | None,
    relation: str | None,
    object: str | None,
    owner: str | None,
    source: str | None,
    scope: str | None,
    status: str | None,
    layer: str | None,
    min_confidence: float | None,
    min_importance: float | None,
    limit: int | None,
) -> dict[str, Any]:
    spec: dict[str, Any] = {}
    normalized_kind: str | None = None
    if kind is not None:
        normalized_kind = str(kind).strip().lower()
        if normalized_kind not in _VALID_INTELLIGENCE_KINDS:
            raise ValueError(
                "kind must be one of all, task, tasks, claim, claims, evidence, decision, decisions, memory, memories"
            )
        spec["kind"] = normalized_kind
    if key is not None:
        spec["key"] = str(key)
    if key_prefix is not None:
        spec["key_prefix"] = str(key_prefix)
    if task_key is not None:
        spec["task_key"] = str(task_key)
    if claim_key is not None:
        spec["claim_key"] = str(claim_key)
    if subject is not None:
        if normalized_kind not in {None, "all", "claim", "claims"}:
            raise ValueError("subject filters require kind='claims' or a cross-kind surface")
        spec["subject"] = str(subject)
    if relation is not None:
        if normalized_kind not in {None, "all", "claim", "claims"}:
            raise ValueError("relation filters require kind='claims' or a cross-kind surface")
        spec["relation"] = str(relation)
    if object is not None:
        if normalized_kind not in {None, "all", "claim", "claims"}:
            raise ValueError("object filters require kind='claims' or a cross-kind surface")
        spec["object"] = str(object)
    if owner is not None:
        spec["owner"] = str(owner)
    if source is not None:
        spec["source"] = str(source)
    if scope is not None:
        spec["scope"] = str(scope)
    if status is not None:
        if normalized_kind not in {"task", "tasks", "claim", "claims", "decision", "decisions"}:
            raise ValueError("status filters require kind='tasks', 'claims', or 'decisions'")
        spec["status"] = str(status)
    if layer is not None:
        if normalized_kind not in {"memory", "memories"}:
            raise ValueError("layer filters require kind='memories'")
        spec["layer"] = str(layer)
    if min_confidence is not None:
        spec["min_confidence"] = float(min_confidence)
    if min_importance is not None:
        if normalized_kind not in {"memory", "memories"}:
            raise ValueError("min_importance filters require kind='memories'")
        spec["min_importance"] = float(min_importance)
    if limit is not None:
        if int(limit) < 0:
            raise ValueError("limit must be >= 0")
        spec["limit"] = int(limit)
    return spec


def _normalize_intelligence_subscription(
    subscription: IntelligenceSubscription | Mapping[str, Any]
) -> dict[str, Any]:
    if isinstance(subscription, IntelligenceSubscription):
        raw = {
            "kind": subscription.kind,
            "key": subscription.key,
            "key_prefix": subscription.key_prefix,
            "task_key": subscription.task_key,
            "claim_key": subscription.claim_key,
            "subject": subscription.subject,
            "relation": subscription.relation,
            "object": subscription.object,
            "owner": subscription.owner,
            "source": subscription.source,
            "scope": subscription.scope,
            "status": subscription.status,
            "layer": subscription.layer,
            "min_confidence": subscription.min_confidence,
            "min_importance": subscription.min_importance,
        }
    elif isinstance(subscription, Mapping):
        raw = dict(subscription.items())
    else:
        raise TypeError(
            "intelligence_subscriptions must contain IntelligenceSubscription instances or mappings"
        )

    spec = _build_intelligence_filter_spec(
        kind=None if raw.get("kind") is None else str(raw.get("kind")),
        key=None if raw.get("key") is None else str(raw.get("key")),
        key_prefix=None if raw.get("key_prefix") is None else str(raw.get("key_prefix")),
        task_key=None if raw.get("task_key") is None else str(raw.get("task_key")),
        claim_key=None if raw.get("claim_key") is None else str(raw.get("claim_key")),
        subject=None if raw.get("subject") is None else str(raw.get("subject")),
        relation=None if raw.get("relation") is None else str(raw.get("relation")),
        object=None if raw.get("object") is None else str(raw.get("object")),
        owner=None if raw.get("owner") is None else str(raw.get("owner")),
        source=None if raw.get("source") is None else str(raw.get("source")),
        scope=None if raw.get("scope") is None else str(raw.get("scope")),
        status=None if raw.get("status") is None else str(raw.get("status")),
        layer=None if raw.get("layer") is None else str(raw.get("layer")),
        min_confidence=None if raw.get("min_confidence") is None else float(raw.get("min_confidence")),
        min_importance=None if raw.get("min_importance") is None else float(raw.get("min_importance")),
        limit=None,
    )
    if not spec:
        raise ValueError(
            "intelligence subscriptions must constrain kind or at least one filter field"
        )
    return spec


class IntelligenceView:
    def __init__(self, runtime_context: "RuntimeContext"):
        self._runtime_context = runtime_context

    def _require_runtime(self) -> Any:
        self._runtime_context._require_runtime()
        return self._runtime_context._native_runtime

    def snapshot(self) -> dict[str, Any]:
        runtime = self._require_runtime()
        snapshot = _native._runtime_snapshot_intelligence(runtime)
        if not isinstance(snapshot, dict):
            raise TypeError("native runtime returned an invalid intelligence snapshot")
        return snapshot

    def summary(self) -> dict[str, Any]:
        runtime = self._require_runtime()
        result = _native._runtime_intelligence_summary(runtime)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid intelligence summary")
        return result

    def count(
        self,
        *,
        kind: str | None = None,
        key: str | None = None,
        key_prefix: str | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        owner: str | None = None,
        source: str | None = None,
        scope: str | None = None,
        status: str | None = None,
        layer: str | None = None,
        min_confidence: float | None = None,
        min_importance: float | None = None,
    ) -> int:
        runtime = self._require_runtime()
        spec = self._build_query_spec(
            kind=kind,
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            claim_key=claim_key,
            subject=subject,
            relation=relation,
            object=object,
            owner=owner,
            source=source,
            scope=scope,
            status=status,
            layer=layer,
            min_confidence=min_confidence,
            min_importance=min_importance,
            limit=None,
        )
        result = _native._runtime_count_intelligence(runtime, spec)
        if not isinstance(result, int):
            raise TypeError("native runtime returned an invalid intelligence count")
        return result

    def exists(self, **filters: Any) -> bool:
        return self.count(**filters) > 0

    def query(
        self,
        *,
        kind: str | None = None,
        key: str | None = None,
        key_prefix: str | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        owner: str | None = None,
        source: str | None = None,
        scope: str | None = None,
        status: str | None = None,
        layer: str | None = None,
        min_confidence: float | None = None,
        min_importance: float | None = None,
        limit: int | None = None,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec = self._build_query_spec(
            kind=kind,
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            claim_key=claim_key,
            subject=subject,
            relation=relation,
            object=object,
            owner=owner,
            source=source,
            scope=scope,
            status=status,
            layer=layer,
            min_confidence=min_confidence,
            min_importance=min_importance,
            limit=limit,
        )
        result = _native._runtime_query_intelligence(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid intelligence query result")
        return result

    def related(
        self,
        *,
        task_key: str | None = None,
        claim_key: str | None = None,
        limit: int | None = None,
        hops: int = 1,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec: dict[str, Any] = {}
        if task_key is not None:
            spec["task_key"] = str(task_key)
        if claim_key is not None:
            spec["claim_key"] = str(claim_key)
        if not spec:
            raise ValueError("related intelligence lookup requires task_key or claim_key")
        if limit is not None:
            if int(limit) < 0:
                raise ValueError("limit must be >= 0")
            spec["limit"] = int(limit)
        if int(hops) < 1:
            raise ValueError("hops must be >= 1")
        spec["hops"] = int(hops)
        result = _native._runtime_related_intelligence(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid related intelligence result")
        return result

    def agenda(
        self,
        *,
        key: str | None = None,
        key_prefix: str | None = None,
        task_key: str | None = None,
        owner: str | None = None,
        status: str | None = None,
        min_confidence: float | None = None,
        limit: int | None = None,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec = self._build_query_spec(
            kind="tasks",
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            claim_key=None,
            subject=None,
            relation=None,
            object=None,
            owner=owner,
            source=None,
            scope=None,
            status=status,
            layer=None,
            min_confidence=min_confidence,
            min_importance=None,
            limit=limit,
        )
        result = _native._runtime_agenda_intelligence(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid intelligence agenda result")
        return result

    def supporting_claims(
        self,
        *,
        key: str | None = None,
        key_prefix: str | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        status: str | None = None,
        min_confidence: float | None = None,
        limit: int | None = None,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec = self._build_query_spec(
            kind="claims",
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            claim_key=claim_key,
            subject=subject,
            relation=relation,
            object=object,
            owner=None,
            source=None,
            scope=None,
            status=status,
            layer=None,
            min_confidence=min_confidence,
            min_importance=None,
            limit=limit,
        )
        result = _native._runtime_supporting_claims_intelligence(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid supporting-claims result")
        return result

    def action_candidates(
        self,
        *,
        task_key: str | None = None,
        claim_key: str | None = None,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        owner: str | None = None,
        task_status: str | None = None,
        source: str | None = None,
        scope: str | None = None,
        layer: str | None = None,
        min_confidence: float | None = None,
        min_importance: float | None = None,
        limit: int | None = None,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec: dict[str, Any] = {}
        if task_key is not None:
            spec["task_key"] = str(task_key)
        if claim_key is not None:
            spec["claim_key"] = str(claim_key)
        if subject is not None:
            spec["subject"] = str(subject)
        if relation is not None:
            spec["relation"] = str(relation)
        if object is not None:
            spec["object"] = str(object)
        if owner is not None:
            spec["owner"] = str(owner)
        if task_status is not None:
            spec["task_status"] = str(task_status)
        if source is not None:
            spec["source"] = str(source)
        if scope is not None:
            spec["scope"] = str(scope)
        if layer is not None:
            spec["layer"] = str(layer)
        if min_confidence is not None:
            spec["min_confidence"] = float(min_confidence)
        if min_importance is not None:
            spec["min_importance"] = float(min_importance)
        if limit is not None:
            if int(limit) < 0:
                raise ValueError("limit must be >= 0")
            spec["limit"] = int(limit)
        result = _native._runtime_action_candidates_intelligence(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid action-candidates result")
        return result

    def next_task(
        self,
        *,
        key: str | None = None,
        key_prefix: str | None = None,
        task_key: str | None = None,
        owner: str | None = None,
        status: str | None = None,
        min_confidence: float | None = None,
    ) -> dict[str, Any] | None:
        agenda = self.agenda(
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            owner=owner,
            status=status,
            min_confidence=min_confidence,
            limit=1,
        )
        tasks = agenda.get("tasks")
        if not isinstance(tasks, list) or not tasks:
            return None
        first = tasks[0]
        if not isinstance(first, dict):
            raise TypeError("native runtime returned an invalid next-task payload")
        return first

    def recall(
        self,
        *,
        key: str | None = None,
        key_prefix: str | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        scope: str | None = None,
        layer: str | None = None,
        min_importance: float | None = None,
        limit: int | None = None,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec = self._build_query_spec(
            kind="memories",
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            claim_key=claim_key,
            subject=None,
            relation=None,
            object=None,
            owner=None,
            source=None,
            scope=scope,
            status=None,
            layer=layer,
            min_confidence=None,
            min_importance=min_importance,
            limit=limit,
        )
        result = _native._runtime_recall_intelligence(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid intelligence recall result")
        return result

    def focus(
        self,
        *,
        key: str | None = None,
        key_prefix: str | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        owner: str | None = None,
        source: str | None = None,
        scope: str | None = None,
        layer: str | None = None,
        min_confidence: float | None = None,
        min_importance: float | None = None,
        limit: int | None = 5,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec = self._build_query_spec(
            kind=None,
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            claim_key=claim_key,
            subject=subject,
            relation=relation,
            object=object,
            owner=owner,
            source=source,
            scope=scope,
            status=None,
            layer=layer,
            min_confidence=min_confidence,
            min_importance=min_importance,
            limit=limit,
        )
        result = _native._runtime_focus_intelligence(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid intelligence focus result")
        return result

    def route(
        self,
        rules: Sequence[IntelligenceRule | Mapping[str, Any]],
        *,
        default: str | None = None,
    ) -> str | None:
        runtime = self._require_runtime()
        if not rules:
            return default

        native_rules: list[dict[str, Any]] = []
        for rule in rules:
            spec = self._normalize_route_rule(rule)
            native_rules.append(spec)

        result = _native._runtime_route_intelligence(
            runtime,
            {"rules": native_rules, "default": default},
        )
        if result is None:
            return None
        if not isinstance(result, str):
            raise TypeError("native runtime returned an invalid intelligence route result")
        return result

    def upsert_task(
        self,
        key: str,
        *,
        title: str | None = None,
        owner: str | None = None,
        details: Any | None = None,
        result: Any | None = None,
        status: str | None = None,
        priority: int | None = None,
        confidence: float | None = None,
    ) -> None:
        runtime = self._require_runtime()
        spec = {"key": str(key)}
        if title is not None:
            spec["title"] = str(title)
        if owner is not None:
            spec["owner"] = str(owner)
        if details is not None:
            spec["details"] = details
        if result is not None:
            spec["result"] = result
        if status is not None:
            spec["status"] = str(status)
        if priority is not None:
            spec["priority"] = int(priority)
        if confidence is not None:
            spec["confidence"] = float(confidence)
        _native._runtime_upsert_task(runtime, spec)

    def upsert_claim(
        self,
        key: str,
        *,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        statement: Any | None = None,
        status: str | None = None,
        confidence: float | None = None,
    ) -> None:
        runtime = self._require_runtime()
        spec = {"key": str(key)}
        if subject is not None:
            spec["subject"] = str(subject)
        if relation is not None:
            spec["relation"] = str(relation)
        if object is not None:
            spec["object"] = str(object)
        if statement is not None:
            spec["statement"] = statement
        if status is not None:
            spec["status"] = str(status)
        if confidence is not None:
            spec["confidence"] = float(confidence)
        _native._runtime_upsert_claim(runtime, spec)

    def add_evidence(
        self,
        key: str,
        *,
        kind: str | None = None,
        source: str | None = None,
        content: Any | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        confidence: float | None = None,
    ) -> None:
        runtime = self._require_runtime()
        spec = {"key": str(key)}
        if kind is not None:
            spec["kind"] = str(kind)
        if source is not None:
            spec["source"] = str(source)
        if content is not None:
            spec["content"] = content
        if task_key is not None:
            spec["task_key"] = str(task_key)
        if claim_key is not None:
            spec["claim_key"] = str(claim_key)
        if confidence is not None:
            spec["confidence"] = float(confidence)
        _native._runtime_add_evidence(runtime, spec)

    def record_decision(
        self,
        key: str,
        *,
        task_key: str | None = None,
        claim_key: str | None = None,
        summary: Any | None = None,
        status: str | None = None,
        confidence: float | None = None,
    ) -> None:
        runtime = self._require_runtime()
        spec = {"key": str(key)}
        if task_key is not None:
            spec["task_key"] = str(task_key)
        if claim_key is not None:
            spec["claim_key"] = str(claim_key)
        if summary is not None:
            spec["summary"] = summary
        if status is not None:
            spec["status"] = str(status)
        if confidence is not None:
            spec["confidence"] = float(confidence)
        _native._runtime_record_decision(runtime, spec)

    def remember(
        self,
        key: str,
        *,
        layer: str | None = None,
        scope: str | None = None,
        content: Any | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        importance: float | None = None,
    ) -> None:
        runtime = self._require_runtime()
        spec = {"key": str(key)}
        if layer is not None:
            spec["layer"] = str(layer)
        if scope is not None:
            spec["scope"] = str(scope)
        if content is not None:
            spec["content"] = content
        if task_key is not None:
            spec["task_key"] = str(task_key)
        if claim_key is not None:
            spec["claim_key"] = str(claim_key)
        if importance is not None:
            spec["importance"] = float(importance)
        _native._runtime_remember(runtime, spec)

    def _build_query_spec(
        self,
        *,
        kind: str | None,
        key: str | None,
        key_prefix: str | None,
        task_key: str | None,
        claim_key: str | None,
        subject: str | None,
        relation: str | None,
        object: str | None,
        owner: str | None,
        source: str | None,
        scope: str | None,
        status: str | None,
        layer: str | None,
        min_confidence: float | None,
        min_importance: float | None,
        limit: int | None,
    ) -> dict[str, Any]:
        return _build_intelligence_filter_spec(
            kind=kind,
            key=key,
            key_prefix=key_prefix,
            task_key=task_key,
            claim_key=claim_key,
            subject=subject,
            relation=relation,
            object=object,
            owner=owner,
            source=source,
            scope=scope,
            status=status,
            layer=layer,
            min_confidence=min_confidence,
            min_importance=min_importance,
            limit=limit,
        )

    def _normalize_route_rule(self, rule: IntelligenceRule | Mapping[str, Any]) -> dict[str, Any]:
        if isinstance(rule, IntelligenceRule):
            raw = {
                "goto": rule.goto,
                "kind": rule.kind,
                "key": rule.key,
                "key_prefix": rule.key_prefix,
                "task_key": rule.task_key,
                "claim_key": rule.claim_key,
                "subject": rule.subject,
                "relation": rule.relation,
                "object": rule.object,
                "owner": rule.owner,
                "source": rule.source,
                "scope": rule.scope,
                "status": rule.status,
                "layer": rule.layer,
                "min_confidence": rule.min_confidence,
                "min_importance": rule.min_importance,
                "min_count": rule.min_count,
                "max_count": rule.max_count,
            }
        elif isinstance(rule, Mapping):
            raw = dict(rule.items())
        else:
            raise TypeError("intelligence route rules must be IntelligenceRule instances or mappings")

        goto = raw.get("goto")
        if goto is None:
            raise ValueError("intelligence route rules require goto")

        spec = self._build_query_spec(
            kind=None if raw.get("kind") is None else str(raw.get("kind")),
            key=None if raw.get("key") is None else str(raw.get("key")),
            key_prefix=None if raw.get("key_prefix") is None else str(raw.get("key_prefix")),
            task_key=None if raw.get("task_key") is None else str(raw.get("task_key")),
            claim_key=None if raw.get("claim_key") is None else str(raw.get("claim_key")),
            subject=None if raw.get("subject") is None else str(raw.get("subject")),
            relation=None if raw.get("relation") is None else str(raw.get("relation")),
            object=None if raw.get("object") is None else str(raw.get("object")),
            owner=None if raw.get("owner") is None else str(raw.get("owner")),
            source=None if raw.get("source") is None else str(raw.get("source")),
            scope=None if raw.get("scope") is None else str(raw.get("scope")),
            status=None if raw.get("status") is None else str(raw.get("status")),
            layer=None if raw.get("layer") is None else str(raw.get("layer")),
            min_confidence=None if raw.get("min_confidence") is None else float(raw.get("min_confidence")),
            min_importance=None if raw.get("min_importance") is None else float(raw.get("min_importance")),
            limit=None,
        )
        min_count = raw.get("min_count", 1)
        if int(min_count) < 0:
            raise ValueError("min_count must be >= 0")
        spec["min_count"] = int(min_count)
        if raw.get("max_count") is not None:
            if int(raw["max_count"]) < 0:
                raise ValueError("max_count must be >= 0")
            spec["max_count"] = int(raw["max_count"])
        spec["goto"] = str(goto)
        return spec


class KnowledgeView:
    def __init__(self, runtime_context: "RuntimeContext"):
        self._runtime_context = runtime_context

    def _require_runtime(self) -> Any:
        self._runtime_context._require_runtime()
        return self._runtime_context._native_runtime

    def upsert_entity(self, label: str, *, payload: Any | None = None) -> None:
        runtime = self._require_runtime()
        label_text = str(label)
        if not label_text:
            raise ValueError("knowledge entity label must be non-empty")
        spec: dict[str, Any] = {"label": label_text}
        if payload is not None:
            spec["payload"] = payload
        _native._runtime_upsert_knowledge_entity(runtime, spec)

    def upsert_triple(
        self,
        subject: str,
        relation: str,
        object: str,
        *,
        payload: Any | None = None,
    ) -> None:
        runtime = self._require_runtime()
        subject_text = str(subject)
        relation_text = str(relation)
        object_text = str(object)
        if not subject_text or not relation_text or not object_text:
            raise ValueError("knowledge triples require non-empty subject, relation, and object")
        spec: dict[str, Any] = {
            "subject": subject_text,
            "relation": relation_text,
            "object": object_text,
        }
        if payload is not None:
            spec["payload"] = payload
        _native._runtime_upsert_knowledge_triple(runtime, spec)

    def query(
        self,
        *,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        direction: str = "match",
        limit: int | None = None,
    ) -> dict[str, Any]:
        runtime = self._require_runtime()
        spec: dict[str, Any] = {"direction": str(direction)}
        if subject is not None:
            spec["subject"] = str(subject)
        if relation is not None:
            spec["relation"] = str(relation)
        if object is not None:
            spec["object"] = str(object)
        if limit is not None:
            if int(limit) < 0:
                raise ValueError("limit must be >= 0")
            spec["limit"] = int(limit)
        result = _native._runtime_query_knowledge(runtime, spec)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid knowledge query result")
        return result

    def neighborhood(
        self,
        entity: str,
        *,
        relation: str | None = None,
        limit: int | None = None,
    ) -> dict[str, Any]:
        return self.query(
            subject=str(entity),
            relation=relation,
            direction="neighborhood",
            limit=limit,
        )

    def load_query(
        self,
        *,
        store: str,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        direction: str = "match",
        depth: int = 1,
        limit: int | None = None,
        properties: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        graph_store = self._runtime_context.graph_stores.get(store)
        result = graph_store.query(
            subject=subject,
            relation=relation,
            object=object,
            direction=direction,
            depth=depth,
            limit=limit,
            properties=properties,
        )
        neighborhood = _coerce_graph_neighborhood(result)
        self._hydrate_from_neighborhood(neighborhood)
        return neighborhood.to_dict()

    def load_neighborhood(
        self,
        entity: str | None = None,
        *,
        store: str,
        subject: str | None = None,
        relation: str | None = None,
        depth: int = 1,
        limit: int | None = None,
        properties: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        target = subject if subject is not None else entity
        if target is None:
            raise ValueError("load_neighborhood requires entity or subject")
        graph_store = self._runtime_context.graph_stores.get(store)
        result = graph_store.neighborhood(
            str(target),
            relation=relation,
            depth=depth,
            limit=limit,
            properties=properties,
        )
        neighborhood = _coerce_graph_neighborhood(result)
        self._hydrate_from_neighborhood(neighborhood)
        return neighborhood.to_dict()

    def sync_to_store(
        self,
        store: str,
        *,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        direction: str = "match",
        limit: int | None = None,
    ) -> dict[str, Any]:
        graph_store = self._runtime_context.graph_stores.get(store)
        result = self.query(
            subject=subject,
            relation=relation,
            object=object,
            direction=direction,
            limit=limit,
        )
        triples = list(result.get("triples", []))
        written = graph_store.upsert_triples(triples)
        return {
            "store": str(store),
            "triples": int(written),
            "source": "native",
        }

    def _hydrate_from_neighborhood(self, neighborhood: GraphNeighborhood) -> None:
        for entity in neighborhood.entities:
            self.upsert_entity(entity.label, payload=entity.payload)
        for triple in neighborhood.triples:
            self.upsert_triple(
                triple.subject,
                triple.relation,
                triple.object,
                payload=triple.payload,
            )


def _coerce_graph_neighborhood(value: Any) -> GraphNeighborhood:
    if isinstance(value, GraphNeighborhood):
        return value
    if isinstance(value, Mapping):
        return GraphNeighborhood(
            entities=tuple(value.get("entities", ())),
            triples=tuple(value.get("triples", ())),
            store=value.get("store"),
            subject=value.get("subject"),
            metadata=dict(value.get("metadata", {})) if hasattr(value.get("metadata", {}), "items") else {},
        )
    raise TypeError("graph store query methods must return GraphNeighborhood or mapping values")


class RuntimeContext:
    def __init__(self, native_runtime: Any | None):
        self._native_runtime = native_runtime
        self._intelligence = IntelligenceView(self)
        self._knowledge = KnowledgeView(self)
        self._context = ContextAccessor(self)
        self._graph_stores: GraphStoreRegistryView | None = None

    @property
    def available(self) -> bool:
        return self._native_runtime is not None

    def record_once_with_metadata(self, key: str, request: Any, producer: Any) -> dict[str, Any]:
        self._require_runtime()
        if not isinstance(key, str) or not key:
            raise ValueError("recorded effect key must be a non-empty string")
        if not callable(producer):
            raise TypeError("producer must be callable")
        details = _native._runtime_record_once(
            self._native_runtime,
            key,
            request,
            producer,
        )
        if not isinstance(details, dict):
            raise TypeError("native runtime returned an invalid recorded-effect payload")
        return details

    def record_once(self, key: str, request: Any, producer: Any) -> Any:
        return self.record_once_with_metadata(key, request, producer)["value"]

    def _require_runtime(self) -> None:
        if not self.available:
            raise RuntimeError("runtime context is not available for this callback")

    @property
    def intelligence(self) -> "IntelligenceView":
        return self._intelligence

    @property
    def knowledge(self) -> "KnowledgeView":
        return self._knowledge

    @property
    def context(self) -> ContextAccessor:
        return self._context

    @property
    def graph_stores(self) -> GraphStoreRegistryView:
        if self._graph_stores is None:
            raise RuntimeError("graph stores are not available for this runtime context")
        return self._graph_stores

    def _bind_graph_stores(self, graph_stores: GraphStoreRegistryView) -> None:
        self._graph_stores = graph_stores

    def _bind_context(
        self,
        *,
        spec: ContextSpec | None,
        state: Mapping[str, Any],
        config: Mapping[str, Any],
        node_name: str | None,
    ) -> None:
        self._context.bind(
            spec=spec,
            state=state,
            config=config,
            node_name=node_name,
        )

    def _runtime_identity(self) -> dict[str, Any]:
        self._require_runtime()
        result = _native._runtime_identity(self._native_runtime)
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid runtime identity")
        return result

    def _rank_context_graph(self, spec: Mapping[str, Any]) -> dict[str, Any]:
        self._require_runtime()
        result = _native._runtime_rank_context_graph(self._native_runtime, dict(spec.items()))
        if not isinstance(result, dict):
            raise TypeError("native runtime returned an invalid context graph ranking")
        return result

    def snapshot_intelligence(self) -> dict[str, Any]:
        return self.intelligence.snapshot()

    def upsert_knowledge_entity(self, label: str, *, payload: Any | None = None) -> None:
        self.knowledge.upsert_entity(label, payload=payload)

    def upsert_knowledge_triple(
        self,
        subject: str,
        relation: str,
        object: str,
        *,
        payload: Any | None = None,
    ) -> None:
        self.knowledge.upsert_triple(subject, relation, object, payload=payload)

    def query_knowledge(
        self,
        *,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        direction: str = "match",
        limit: int | None = None,
    ) -> dict[str, Any]:
        return self.knowledge.query(
            subject=subject,
            relation=relation,
            object=object,
            direction=direction,
            limit=limit,
        )

    def upsert_task(
        self,
        key: str,
        *,
        title: str | None = None,
        owner: str | None = None,
        details: Any | None = None,
        result: Any | None = None,
        status: str | None = None,
        priority: int | None = None,
        confidence: float | None = None,
    ) -> None:
        self.intelligence.upsert_task(
            key,
            title=title,
            owner=owner,
            details=details,
            result=result,
            status=status,
            priority=priority,
            confidence=confidence,
        )

    def upsert_claim(
        self,
        key: str,
        *,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        statement: Any | None = None,
        status: str | None = None,
        confidence: float | None = None,
    ) -> None:
        self.intelligence.upsert_claim(
            key,
            subject=subject,
            relation=relation,
            object=object,
            statement=statement,
            status=status,
            confidence=confidence,
        )

    def add_evidence(
        self,
        key: str,
        *,
        kind: str | None = None,
        source: str | None = None,
        content: Any | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        confidence: float | None = None,
    ) -> None:
        self.intelligence.add_evidence(
            key,
            kind=kind,
            source=source,
            content=content,
            task_key=task_key,
            claim_key=claim_key,
            confidence=confidence,
        )

    def record_decision(
        self,
        key: str,
        *,
        task_key: str | None = None,
        claim_key: str | None = None,
        summary: Any | None = None,
        status: str | None = None,
        confidence: float | None = None,
    ) -> None:
        self.intelligence.record_decision(
            key,
            task_key=task_key,
            claim_key=claim_key,
            summary=summary,
            status=status,
            confidence=confidence,
        )

    def remember(
        self,
        key: str,
        *,
        layer: str | None = None,
        scope: str | None = None,
        content: Any | None = None,
        task_key: str | None = None,
        claim_key: str | None = None,
        importance: float | None = None,
    ) -> None:
        self.intelligence.remember(
            key,
            layer=layer,
            scope=scope,
            content=content,
            task_key=task_key,
            claim_key=claim_key,
            importance=importance,
        )

    def invoke_tool_with_metadata(
        self,
        name: str,
        request: Any,
        *,
        decode: str = "auto",
    ) -> dict[str, Any]:
        self._require_runtime()
        details = _native._runtime_invoke_tool_with_details(
            self._native_runtime,
            str(name),
            encode_adapter_payload(request),
        )
        return _decode_response_details(details, decode=decode)

    def invoke_tool(
        self,
        name: str,
        request: Any,
        *,
        decode: str = "auto",
    ) -> Any:
        details = self.invoke_tool_with_metadata(name, request, decode=decode)
        if not details.get("ok", False):
            _raise_adapter_error("tool", str(name), details)
        return details["output"]

    def invoke_model_with_metadata(
        self,
        name: str,
        prompt: Any,
        *,
        schema: Any | None = None,
        max_tokens: int = 0,
        decode: str = "auto",
    ) -> dict[str, Any]:
        self._require_runtime()
        normalized_prompt = _coerce_prompt_value_for_model_input(prompt)
        normalized_schema = _coerce_prompt_value_for_model_input(schema)
        details = _native._runtime_invoke_model_with_details(
            self._native_runtime,
            str(name),
            encode_adapter_payload(normalized_prompt),
            encode_adapter_payload(normalized_schema),
            int(max_tokens),
        )
        return _decode_response_details(details, decode=decode)

    def invoke_model(
        self,
        name: str,
        prompt: Any,
        *,
        schema: Any | None = None,
        max_tokens: int = 0,
        decode: str = "auto",
    ) -> Any:
        details = self.invoke_model_with_metadata(
            name,
            prompt,
            schema=schema,
            max_tokens=max_tokens,
            decode=decode,
        )
        if not details.get("ok", False):
            _raise_adapter_error("model", str(name), details)
        return details["output"]


def _extract_runtime_and_config(config: Any) -> tuple[Any, RuntimeContext]:
    runtime_accessor = getattr(config, "_agentcore_runtime_capsule", None)
    if callable(runtime_accessor):
        return config, RuntimeContext(runtime_accessor())
    normalized = _normalize_config(config)
    return normalized, RuntimeContext(normalized.pop(_RUNTIME_CONFIG_KEY, None))


class _OverlayMapping(Mapping):
    def __init__(self, base: Any, overlay: dict[str, Any]):
        self._base = base
        self._overlay = overlay

    def __getitem__(self, key: str) -> Any:
        if key in self._overlay:
            return self._overlay[key]
        return self._base[key]

    def __iter__(self):
        seen: set[str] = set()
        for key in self._overlay:
            seen.add(key)
            yield key
        for key in self._base.keys():
            if key not in seen:
                yield key

    def __len__(self) -> int:
        count = len(self._overlay)
        for key in self._base.keys():
            if key not in self._overlay:
                count += 1
        return count

    def get(self, key: str, default: Any = None) -> Any:
        if key in self._overlay:
            return self._overlay[key]
        return self._base.get(key, default)

    def copy(self) -> dict[str, Any]:
        copied = dict(self._base.items())
        copied.update(self._overlay)
        return copied


class _StateView(Mapping):
    def __init__(self, base: Any):
        self._base = base

    def __getitem__(self, key: str) -> Any:
        value = self._base.get(key, _MISSING)
        if value is _MISSING or value is None:
            raise KeyError(key)
        return value

    def __iter__(self):
        yield from self._base.keys()

    def __len__(self) -> int:
        return len(self._base.keys())

    def get(self, key: str, default: Any = None) -> Any:
        value = self._base.get(key, _MISSING)
        if value is _MISSING or value is None:
            return default
        return value

    def keys(self):
        return self._base.keys()

    def items(self):
        return self._base.items()

    def copy(self) -> dict[str, Any]:
        return dict(self._base.items())


def _normalize_batch_configs(config: Any, size: int) -> list[dict[str, Any]]:
    if isinstance(config, Sequence) and not isinstance(config, (str, bytes, bytearray, dict)):
        if len(config) != size:
            raise ValueError("batch config sequences must match the number of inputs")
        return [_normalize_config(entry) for entry in config]
    normalized = _normalize_config(config)
    return [dict(normalized) for _ in range(size)]


def _normalize_node_kind(kind: str, *, allow_subgraph: bool = False) -> str:
    normalized = str(kind).strip().lower()
    valid_kinds = _VALID_NODE_KINDS | ({"subgraph"} if allow_subgraph else set())
    if normalized not in valid_kinds:
        allowed = ", ".join(sorted(valid_kinds))
        raise ValueError(f"node kind must be one of: {allowed}")
    return normalized


def _normalize_string_mapping(value: Any, *, field_name: str) -> dict[str, str]:
    if value is None:
        return {}
    if not hasattr(value, "items"):
        raise TypeError(f"{field_name} must be a mapping or None")
    normalized: dict[str, str] = {}
    for key, mapped_value in value.items():
        if not isinstance(key, str) or not isinstance(mapped_value, str):
            raise TypeError(f"{field_name} must contain only string keys and string values")
        normalized[key] = mapped_value
    return normalized


def _normalize_merge_rules(merge: Any) -> dict[str, str]:
    normalized = _normalize_string_mapping(merge, field_name="merge")
    for field_name, strategy in normalized.items():
        normalized_strategy = strategy.strip().lower()
        if normalized_strategy not in _VALID_MERGE_STRATEGIES:
            allowed = ", ".join(sorted(_VALID_MERGE_STRATEGIES))
            raise ValueError(
                f"merge strategy for {field_name!r} must be one of: {allowed}"
            )
        normalized[field_name] = normalized_strategy
    return normalized


def _normalize_string_sequence(value: Any, *, field_name: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        raise TypeError(f"{field_name} must be a sequence of strings, not a single string")

    normalized: list[str] = []
    for entry in value:
        if not isinstance(entry, str):
            raise TypeError(f"{field_name} must contain only strings")
        normalized.append(entry)
    return tuple(normalized)


def _is_non_string_sequence(value: Any) -> bool:
    return isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray, memoryview))


def _infer_node_name(action: Any) -> str:
    if not callable(action):
        raise TypeError("node actions must be callable when no explicit name is provided")
    inferred = getattr(action, "__name__", None)
    if not isinstance(inferred, str) or not inferred or inferred == "<lambda>":
        raise ValueError(
            "callable-only node registration requires a named callable; "
            "pass an explicit node name for lambdas or anonymous callables"
        )
    return inferred


def _infer_subgraph_node_name(graph: Any) -> str:
    inferred = getattr(graph, "name", None)
    if not isinstance(inferred, str) or not inferred:
        inferred = getattr(graph, "_name", None)
    if not isinstance(inferred, str) or not inferred:
        raise ValueError("subgraph-only node registration requires a graph with a stable name")
    return inferred


def _unwrap_schema_annotation(annotation: Any) -> Any:
    while True:
        origin = get_origin(annotation)
        if origin is None:
            return annotation
        origin_name = getattr(origin, "__qualname__", getattr(origin, "__name__", ""))
        if origin_name == "Annotated":
            annotation = get_args(annotation)[0]
            continue
        if origin_name in {"Union", "UnionType"}:
            union_args = [entry for entry in get_args(annotation) if entry is not type(None)]
            if len(union_args) == 1:
                annotation = union_args[0]
                continue
        return annotation


def _schema_reducer_metadata_to_merge_strategy(annotation: Any, metadata: Any) -> str | None:
    if isinstance(metadata, str):
        normalized = metadata.strip().lower()
        if normalized in _VALID_MERGE_STRATEGIES:
            return normalized
        return None

    base = _unwrap_schema_annotation(annotation)
    base_origin = get_origin(base)
    if metadata is add_messages or getattr(metadata, "__agentcore_reducer__", None) == "add_messages":
        if base is list or base_origin is list:
            return "merge_messages"
        return None
    if metadata in {operator.add, operator.iadd}:
        if base is int:
            return "sum_int64"
        if base is list or base_origin is list:
            return "concat_sequence"
        return None
    if metadata in {operator.or_, operator.ior}:
        if base is bool:
            return "logical_or"
        return None
    if metadata in {operator.and_, operator.iand}:
        if base is bool:
            return "logical_and"
        return None
    if metadata is max and base is int:
        return "max_int64"
    if metadata is min and base is int:
        return "min_int64"
    return None


def _extract_state_schema_hints(state_schema: Any) -> dict[str, Any]:
    if state_schema is None or state_schema is dict:
        return {}
    try:
        hints = get_type_hints(state_schema, include_extras=True)
        if isinstance(hints, dict):
            return dict(hints)
    except Exception:
        pass
    raw_annotations = getattr(state_schema, "__annotations__", None)
    if isinstance(raw_annotations, dict):
        return dict(raw_annotations)
    return {}


def _extract_state_schema_fields(state_schema: Any) -> tuple[str, ...]:
    return tuple(_extract_state_schema_hints(state_schema).keys())


def _infer_schema_merge_rules(state_schema: Any) -> dict[str, str]:
    normalized: dict[str, str] = {}
    for field_name, annotation in _extract_state_schema_hints(state_schema).items():
        origin = get_origin(annotation)
        origin_name = getattr(origin, "__qualname__", getattr(origin, "__name__", ""))
        if origin_name != "Annotated":
            continue
        base, *metadata_items = get_args(annotation)
        for metadata in metadata_items:
            strategy = _schema_reducer_metadata_to_merge_strategy(base, metadata)
            if strategy is not None:
                normalized[field_name] = strategy
                break
    return normalized


def _merge_with_schema_rules(schema_merge: dict[str, str], merge: Any) -> dict[str, str]:
    normalized = dict(schema_merge)
    normalized.update(_normalize_merge_rules(merge))
    return normalized


def _apply_reducer_strategy_for_route(strategy: str, left: Any, right: Any) -> Any:
    if left is _MISSING or left is None:
        return right
    if strategy == "require_equal":
        if left != right:
            raise ValueError("state reducer require_equal saw conflicting values before routing")
        return right
    if strategy == "require_single_writer":
        raise ValueError("state reducer require_single_writer saw more than one writer before routing")
    if strategy == "last_writer_wins":
        return right
    if strategy == "first_writer_wins":
        return left
    if strategy == "sum_int64":
        return int(left) + int(right)
    if strategy == "max_int64":
        return max(int(left), int(right))
    if strategy == "min_int64":
        return min(int(left), int(right))
    if strategy == "logical_or":
        return bool(left) or bool(right)
    if strategy == "logical_and":
        return bool(left) and bool(right)
    if strategy == "concat_sequence":
        return list(left) + list(right)
    if strategy == "merge_messages":
        return add_messages(left, right)
    return right


def _apply_schema_reducer_route_overlay(
    schema_merge: dict[str, str],
    state: Mapping[str, Any],
    updates: dict[str, Any],
) -> dict[str, Any]:
    if not schema_merge or not updates:
        return updates
    reduced = dict(updates)
    for key, strategy in schema_merge.items():
        if key not in updates:
            continue
        reduced[key] = _apply_reducer_strategy_for_route(
            strategy,
            state.get(key, _MISSING),
            updates[key],
        )
    return reduced


def _normalize_compile_name(name: Any) -> str | None:
    if name is None:
        return None
    if not isinstance(name, str) or not name:
        raise TypeError("compile name must be a non-empty string or None")
    return name


def _validate_compile_compat_options(
    *,
    checkpointer: Any,
    interrupt_before: Any,
    interrupt_after: Any,
    store: Any,
) -> None:
    unsupported: list[str] = []
    if checkpointer is not None:
        unsupported.append("checkpointer")
    if interrupt_before is not None:
        unsupported.append("interrupt_before")
    if interrupt_after is not None:
        unsupported.append("interrupt_after")
    if store is not None:
        unsupported.append("store")
    if unsupported:
        joined = ", ".join(unsupported)
        raise NotImplementedError(
            f"compile(...) does not currently support {joined}; "
            "use AgentCore's native wait/resume, metadata, and persistent subgraph session surfaces instead"
        )


def _validate_direct_subgraph_node_options(
    *,
    kind: str,
    stop_after: bool,
    allow_fan_out: bool,
    create_join_scope: bool,
    join_incoming_branches: bool,
    deterministic: bool,
    read_keys: Sequence[str] | None,
    cache_size: int,
    intelligence_subscriptions: Sequence[IntelligenceSubscription | Mapping[str, Any]] | None,
    context: ContextSpec | Mapping[str, Any] | None,
    merge: dict[str, Any] | None,
) -> None:
    unsupported: list[str] = []
    if str(kind).strip().lower() != "compute":
        unsupported.append("kind")
    if stop_after:
        unsupported.append("stop_after")
    if allow_fan_out:
        unsupported.append("allow_fan_out")
    if create_join_scope:
        unsupported.append("create_join_scope")
    if join_incoming_branches:
        unsupported.append("join_incoming_branches")
    if deterministic:
        unsupported.append("deterministic")
    if read_keys:
        unsupported.append("read_keys")
    if int(cache_size) != 16:
        unsupported.append("cache_size")
    if intelligence_subscriptions:
        unsupported.append("intelligence_subscriptions")
    if context is not None:
        unsupported.append("context")
    if merge:
        unsupported.append("merge")
    if unsupported:
        raise NotImplementedError(
            "direct add_node(subgraph) supports only the shared-state default path; "
            f"use add_subgraph(...) for advanced options such as {', '.join(unsupported)}"
        )


def _infer_shared_subgraph_bindings(parent_schema: Any, graph: Any) -> dict[str, str]:
    child_schema = getattr(graph, "_state_schema", None)
    parent_fields = set(_extract_state_schema_fields(parent_schema))
    child_fields = set(_extract_state_schema_fields(child_schema))
    shared_fields = sorted(parent_fields & child_fields)
    if not shared_fields:
        raise NotImplementedError(
            "direct add_node(subgraph) requires parent and child graphs to declare overlapping schema fields; "
            "use add_subgraph(...) with explicit inputs/outputs when the shared state shape is implicit"
        )
    return {field: field for field in shared_fields}


class StateGraph:
    def __init__(self, state_schema: Any = None, *, name: str | None = None, worker_count: int = 1):
        self._state_schema = state_schema
        self._name = name or getattr(state_schema, "__name__", "agentcore_state_graph")
        self._worker_count = max(1, int(worker_count))
        self._schema_fields = _extract_state_schema_fields(state_schema)
        self._schema_merge = _infer_schema_merge_rules(state_schema)
        self._nodes: dict[str, _NodeSpec] = {}
        self._edges: list[tuple[str, str]] = []
        self._conditional_edges: dict[str, tuple[Any, dict[Any, str]]] = {}
        self._entry_point: str | None = None
        self._pending_join_targets: set[str] = set()
        self._synthetic_router_index = 0

    def add_node(
        self,
        name: str | Any,
        action: Any | None = None,
        *,
        kind: str = "compute",
        stop_after: bool = False,
        allow_fan_out: bool = False,
        create_join_scope: bool = False,
        join_incoming_branches: bool = False,
        deterministic: bool = False,
        read_keys: Sequence[str] | None = None,
        cache_size: int = 16,
        intelligence_subscriptions: Sequence[IntelligenceSubscription | Mapping[str, Any]] | None = None,
        context: ContextSpec | Mapping[str, Any] | None = None,
        merge: dict[str, Any] | None = None,
    ) -> "StateGraph":
        if action is None and isinstance(name, (StateGraph, CompiledStateGraph)):
            action = name
            name = _infer_subgraph_node_name(action)
        if action is None and not isinstance(name, str):
            action = name
            name = _infer_node_name(action)
        if not isinstance(name, str) or not name:
            raise TypeError("node name must be a non-empty string")
        if isinstance(action, (StateGraph, CompiledStateGraph)):
            _validate_direct_subgraph_node_options(
                kind=kind,
                stop_after=stop_after,
                allow_fan_out=allow_fan_out,
                create_join_scope=create_join_scope,
                join_incoming_branches=join_incoming_branches,
                deterministic=deterministic,
                read_keys=read_keys,
                cache_size=cache_size,
                intelligence_subscriptions=intelligence_subscriptions,
                context=context,
                merge=merge,
            )
            bindings = _infer_shared_subgraph_bindings(self._state_schema, action)
            return self.add_subgraph(
                name,
                action,
                inputs=bindings,
                outputs=bindings,
            )
        if action is not None and not callable(action):
            raise TypeError("node actions must be callable, subgraphs, or None")

        normalized_kind = _normalize_node_kind(kind)
        if normalized_kind == "subgraph":
            raise ValueError("use add_subgraph(...) to register subgraph nodes")

        normalized_read_keys = _normalize_string_sequence(read_keys, field_name="read_keys")
        if not deterministic and normalized_read_keys:
            raise ValueError("read_keys require deterministic=True")
        normalized_cache_size = max(1, int(cache_size))
        normalized_intelligence_subscriptions = tuple(
            _normalize_intelligence_subscription(subscription)
            for subscription in (intelligence_subscriptions or ())
        )
        normalized_context = ContextSpec.from_value(context)

        is_join_node = bool(join_incoming_branches or name in self._pending_join_targets)
        normalized_merge = _normalize_merge_rules(merge)
        effective_merge = (
            _merge_with_schema_rules(self._schema_merge, normalized_merge)
            if is_join_node
            else normalized_merge
        )

        self._nodes[name] = _NodeSpec(
            action=action,
            kind=normalized_kind,
            stop_after=bool(stop_after),
            allow_fan_out=bool(allow_fan_out),
            create_join_scope=bool(create_join_scope),
            join_incoming_branches=is_join_node,
            deterministic=bool(deterministic),
            read_keys=normalized_read_keys,
            cache_size=normalized_cache_size,
            intelligence_subscriptions=normalized_intelligence_subscriptions,
            context=normalized_context,
            merge=effective_merge,
        )
        if self._entry_point is None:
            self._entry_point = name
        return self

    def add_fanout(
        self,
        name: str,
        action: Any | None = None,
        *,
        kind: str = "control",
        create_join_scope: bool = True,
    ) -> "StateGraph":
        return self.add_node(
            name,
            action,
            kind=kind,
            allow_fan_out=True,
            create_join_scope=create_join_scope,
        )

    def add_join(
        self,
        name: str,
        action: Any | None = None,
        *,
        merge: dict[str, Any] | None = None,
        kind: str = "aggregate",
    ) -> "StateGraph":
        return self.add_node(
            name,
            action,
            kind=kind,
            join_incoming_branches=True,
            merge=merge,
        )

    def add_subgraph(
        self,
        name: str,
        graph: StateGraph | CompiledStateGraph,
        *,
        inputs: dict[str, str] | None = None,
        outputs: dict[str, str] | None = None,
        namespace: str | None = None,
        propagate_knowledge_graph: bool = False,
        session_mode: str = "ephemeral",
        session_id_from: str | None = None,
    ) -> "StateGraph":
        if not isinstance(graph, (StateGraph, CompiledStateGraph)):
            raise TypeError("subgraph must be a StateGraph or CompiledStateGraph instance")
        if graph is self:
            raise ValueError("a graph cannot include itself as a subgraph")
        normalized_session_mode = str(session_mode).strip().lower()
        if normalized_session_mode not in {"ephemeral", "persistent"}:
            raise ValueError("session_mode must be either 'ephemeral' or 'persistent'")
        if normalized_session_mode == "persistent":
            if not isinstance(session_id_from, str) or not session_id_from:
                raise ValueError("persistent subgraphs require session_id_from")
        elif session_id_from is not None:
            raise ValueError("ephemeral subgraphs must not declare session_id_from")
        if name in self._pending_join_targets:
            raise NotImplementedError(
                "LangGraph-style multi-source joins into subgraph nodes are not supported directly; "
                "add an explicit join node before the subgraph"
            )

        self._nodes[name] = _NodeSpec(
            action=None,
            kind="subgraph",
            subgraph=_SubgraphSpec(
                graph=graph,
                namespace=namespace,
                inputs=_normalize_string_mapping(inputs, field_name="inputs"),
                outputs=_normalize_string_mapping(outputs, field_name="outputs"),
                propagate_knowledge_graph=bool(propagate_knowledge_graph),
                session_mode=normalized_session_mode,
                session_id_from=session_id_from,
            ),
        )
        if self._entry_point is None:
            self._entry_point = name
        return self

    def add_edge(self, source: str | Sequence[str], target: str) -> "StateGraph":
        if _is_non_string_sequence(source):
            normalized_sources = _normalize_string_sequence(source, field_name="source")
            if not normalized_sources:
                raise ValueError("source must contain at least one node name")
            self._mark_join_target(target)
            for entry in normalized_sources:
                self.add_edge(entry, target)
            return self
        if source == START:
            self.set_entry_point(target)
            return self
        self._edges.append((source, target))
        return self

    def add_conditional_edges(
        self,
        source: str,
        path: Any,
        path_map: dict[Any, str] | None = None,
    ) -> "StateGraph":
        if not callable(path):
            raise TypeError("conditional routing functions must be callable")
        if source == START:
            return self.set_conditional_entry_point(path, path_map)
        self._conditional_edges[source] = (path, dict(path_map or {}))
        return self

    def set_entry_point(self, name: str) -> "StateGraph":
        self._entry_point = name
        return self

    def set_finish_point(self, name: str) -> "StateGraph":
        return self.add_edge(name, END)

    def set_conditional_entry_point(
        self,
        path: Any,
        path_map: dict[Any, str] | None = None,
    ) -> "StateGraph":
        if not callable(path):
            raise TypeError("conditional entry routing functions must be callable")
        router_name = self._allocate_synthetic_router_name("entry_router")
        self.add_node(router_name, None, kind="control")
        self._conditional_edges[router_name] = (path, dict(path_map or {}))
        self._entry_point = router_name
        return self

    def add_sequence(self, nodes: Sequence[Any]) -> "StateGraph":
        if not _is_non_string_sequence(nodes):
            raise TypeError("nodes must be a non-string sequence")
        ordered_names = [self._register_sequence_entry(entry) for entry in nodes]
        if not ordered_names:
            raise ValueError("nodes must contain at least one entry")
        for source, target in zip(ordered_names, ordered_names[1:]):
            self.add_edge(source, target)
        return self

    def compile(
        self,
        *,
        worker_count: int | None = None,
        name: str | None = None,
        debug: bool | None = None,
        checkpointer: Any | None = None,
        interrupt_before: Any | None = None,
        interrupt_after: Any | None = None,
        store: Any | None = None,
    ) -> "CompiledStateGraph":
        if debug is not None and not isinstance(debug, bool):
            raise TypeError("debug must be a bool or None")
        _validate_compile_compat_options(
            checkpointer=checkpointer,
            interrupt_before=interrupt_before,
            interrupt_after=interrupt_after,
            store=store,
        )
        return self._compile_internal(
            worker_count=self._worker_count if worker_count is None else max(1, int(worker_count)),
            name_override=_normalize_compile_name(name),
            subgraph_cache={},
            compile_stack=set(),
            graph_store_registry=None,
        )

    def _compile_internal(
        self,
        *,
        worker_count: int,
        name_override: str | None = None,
        subgraph_cache: dict[int, "CompiledStateGraph"],
        compile_stack: set[int],
        graph_store_registry: GraphStoreRegistryView | None,
    ) -> "CompiledStateGraph":
        graph_identity = id(self)
        if graph_identity in compile_stack:
            raise ValueError(f"cyclical subgraph composition detected while compiling {self._name!r}")

        compile_stack.add(graph_identity)
        try:
            native_graph = _native._create_graph(
                name=self._name if name_override is None else name_override,
                worker_count=worker_count,
            )
            if self._schema_merge:
                _native._set_state_reducers(native_graph, self._schema_merge)
            graph_store_registry = graph_store_registry or GraphStoreRegistryView()
            owned_subgraphs: list[CompiledStateGraph] = []
            owned_subgraph_ids: set[int] = set()

            for node_name, node_spec in self._nodes.items():
                if node_spec.subgraph is not None:
                    if node_name in self._conditional_edges:
                        raise ValueError(
                            "conditional edges are not supported directly on subgraph nodes; "
                            f"add a router node after {node_name!r}"
                        )

                    compiled_subgraph = self._compile_subgraph_reference(
                        node_spec.subgraph.graph,
                        subgraph_cache=subgraph_cache,
                        compile_stack=compile_stack,
                        graph_store_registry=graph_store_registry,
                    )
                    if id(compiled_subgraph) not in owned_subgraph_ids:
                        owned_subgraphs.append(compiled_subgraph)
                        owned_subgraph_ids.add(id(compiled_subgraph))

                    _native._add_subgraph_node(
                        native_graph,
                        node_name,
                        compiled_subgraph._native_graph,
                        namespace_name=node_spec.subgraph.namespace,
                        input_bindings=node_spec.subgraph.inputs,
                        output_bindings=node_spec.subgraph.outputs,
                        propagate_knowledge_graph=node_spec.subgraph.propagate_knowledge_graph,
                        session_mode=node_spec.subgraph.session_mode,
                        session_id_from=node_spec.subgraph.session_id_from,
                    )
                    continue

                callback = (
                    self._build_callback(node_name, graph_store_registry)
                    if self._needs_python_callback(node_name)
                    else None
                )
                native_callback_spec = (
                    self._build_native_callback_spec(node_name)
                    if callback is not None
                    else None
                )
                _native._add_node(
                    native_graph,
                    node_name,
                    callback,
                    kind=node_spec.kind,
                    stop_after=node_spec.stop_after,
                    allow_fan_out=node_spec.allow_fan_out,
                    create_join_scope=node_spec.create_join_scope,
                    join_incoming_branches=node_spec.join_incoming_branches,
                    deterministic=node_spec.deterministic,
                    read_keys=node_spec.read_keys,
                    cache_size=node_spec.cache_size,
                    intelligence_subscriptions=node_spec.intelligence_subscriptions,
                    merge_rules=node_spec.merge,
                    native_callback_spec=native_callback_spec,
                )

            if self._entry_point is not None:
                _native._set_entry_point(native_graph, self._entry_point)

            for source, target in self._edges:
                _native._add_edge(native_graph, source, self._map_target_name(target))

            _native._finalize(native_graph)
            return CompiledStateGraph(
                native_graph,
                self._name if name_override is None else name_override,
                state_schema=self._state_schema,
                owned_subgraphs=owned_subgraphs,
                graph_stores=graph_store_registry,
            )
        finally:
            compile_stack.remove(graph_identity)

    def _compile_subgraph_reference(
        self,
        graph: StateGraph | "CompiledStateGraph",
        *,
        subgraph_cache: dict[int, "CompiledStateGraph"],
        compile_stack: set[int],
        graph_store_registry: GraphStoreRegistryView,
    ) -> "CompiledStateGraph":
        if isinstance(graph, CompiledStateGraph):
            return graph

        cache_key = id(graph)
        cached = subgraph_cache.get(cache_key)
        if cached is not None:
            return cached

        compiled = graph._compile_internal(
            worker_count=graph._worker_count,
            name_override=None,
            subgraph_cache=subgraph_cache,
            compile_stack=compile_stack,
            graph_store_registry=graph_store_registry,
        )
        subgraph_cache[cache_key] = compiled
        return compiled

    def _needs_python_callback(self, node_name: str) -> bool:
        node_spec = self._nodes[node_name]
        return node_spec.subgraph is None and (
            node_spec.action is not None or node_name in self._conditional_edges
        )

    def _map_target_name(self, target: str) -> str:
        if target == END:
            return _native._INTERNAL_END_NODE_NAME
        return target

    def _mark_join_target(self, target: str) -> None:
        if not isinstance(target, str) or not target:
            raise TypeError("target must be a non-empty string")
        self._pending_join_targets.add(target)
        existing = self._nodes.get(target)
        if existing is not None:
            if existing.subgraph is not None:
                raise NotImplementedError(
                    "LangGraph-style multi-source joins into subgraph nodes are not supported directly; "
                    "add an explicit join node before the subgraph"
                )
            existing.join_incoming_branches = True
            existing.merge = _merge_with_schema_rules(self._schema_merge, existing.merge)

    def _allocate_synthetic_router_name(self, label: str) -> str:
        while True:
            name = f"__agentcore_{label}_{self._synthetic_router_index}"
            self._synthetic_router_index += 1
            if name not in self._nodes:
                return name

    def _register_sequence_entry(self, entry: Any) -> str:
        if isinstance(entry, str):
            if entry not in self._nodes:
                raise KeyError(
                    f"sequence entry {entry!r} is not a registered node; "
                    "pass a callable or a (name, action) pair to register it inline"
                )
            return entry
        if _is_non_string_sequence(entry):
            if len(entry) != 2:
                raise TypeError("sequence node tuples must contain exactly two elements")
            entry_name, entry_action = entry
            if not isinstance(entry_name, str) or not entry_name:
                raise TypeError("sequence node tuples must start with a non-empty string name")
            self.add_node(entry_name, entry_action)
            return entry_name
        if isinstance(entry, (StateGraph, CompiledStateGraph)):
            inferred_name = _infer_subgraph_node_name(entry)
        else:
            inferred_name = _infer_node_name(entry)
        self.add_node(inferred_name, entry)
        return inferred_name

    def _build_callback(self, node_name: str, graph_store_registry: GraphStoreRegistryView):
        node_spec = self._nodes[node_name]
        node_action = node_spec.action
        routing_spec = self._conditional_edges.get(node_name)
        action_arity = _positional_arity(node_action) if node_action is not None else 0
        action_invoker = _compile_callback_invoker(node_action) if node_action is not None else None
        route_invoker = None
        route_map = None
        route_arity = 0
        if routing_spec is not None:
            route_fn, route_map = routing_spec
            route_arity = _positional_arity(route_fn)
            route_invoker = _compile_callback_invoker(route_fn)

        simple_callback = (
            node_spec.context is None
            and (action_arity is not None and action_arity <= 2)
            and (route_arity is not None and route_arity <= 2)
        )

        if simple_callback:
            def wrapped(state: Any, config: Any = None) -> Any:
                state_view = _StateView(state)
                updates: dict[str, Any] = {}
                explicit_goto: str | None = None

                if action_invoker is not None:
                    node_result = action_invoker(
                        state_view,
                        {} if config is None else config,
                        None,
                    )
                    updates, explicit_goto, should_wait = _normalize_result(
                        _resolve_maybe_awaitable(node_result)
                    )
                    if should_wait:
                        return updates, None, True
                    if explicit_goto is not None:
                        return updates, self._map_target_name(explicit_goto)

                if routing_spec is None:
                    return updates

                route_updates = (
                    _apply_schema_reducer_route_overlay(self._schema_merge, state_view, updates)
                    if updates
                    else updates
                )
                merged_state = state_view if not route_updates else _OverlayMapping(state_view, route_updates)
                route_result = route_invoker(
                    merged_state,
                    {} if config is None else config,
                    None,
                )
                route_value = _resolve_maybe_awaitable(route_result)
                if route_map:
                    if route_value not in route_map:
                        raise KeyError(f"missing conditional route mapping for {route_value!r}")
                    goto_target = route_map[route_value]
                else:
                    if route_value is None:
                        return updates
                    if not isinstance(route_value, str):
                        raise TypeError("conditional routes without a path_map must return node names")
                    goto_target = route_value
                return updates, self._map_target_name(goto_target)

            return wrapped

        def wrapped(state: Any, config: Any = None) -> Any:
            normalized_config, runtime = _extract_runtime_and_config(config)
            runtime._bind_graph_stores(graph_store_registry)
            state_view = _StateView(state)
            runtime._bind_context(
                spec=node_spec.context,
                state=state_view,
                config=normalized_config,
                node_name=node_name,
            )
            updates: dict[str, Any] = {}
            explicit_goto: str | None = None

            if action_invoker is not None:
                node_result = action_invoker(
                    state_view,
                    normalized_config,
                    runtime,
                )
                updates, explicit_goto, should_wait = _normalize_result(
                    _resolve_maybe_awaitable(node_result)
                )
                if should_wait:
                    return updates, None, True
                if explicit_goto is not None:
                    return updates, self._map_target_name(explicit_goto)

            if routing_spec is None:
                return updates

            route_updates = (
                _apply_schema_reducer_route_overlay(self._schema_merge, state_view, updates)
                if updates
                else updates
            )
            merged_state = state_view if not route_updates else _OverlayMapping(state_view, route_updates)
            runtime._bind_context(
                spec=node_spec.context,
                state=merged_state,
                config=normalized_config,
                node_name=node_name,
            )
            route_result = route_invoker(
                merged_state,
                normalized_config,
                runtime,
            )
            route_value = _resolve_maybe_awaitable(route_result)
            if route_map:
                if route_value not in route_map:
                    raise KeyError(f"missing conditional route mapping for {route_value!r}")
                goto_target = route_map[route_value]
            else:
                if route_value is None:
                    return updates
                if not isinstance(route_value, str):
                    raise TypeError("conditional routes without a path_map must return node names")
                goto_target = route_value
            return updates, self._map_target_name(goto_target)

        return wrapped

    def _build_native_callback_spec(self, node_name: str) -> dict[str, Any] | None:
        node_spec = self._nodes[node_name]
        if node_spec.context is not None:
            return None

        node_action = node_spec.action
        routing_spec = self._conditional_edges.get(node_name)
        action_arity = _positional_arity(node_action) if node_action is not None else 0
        if action_arity is None or action_arity > 2:
            return None
        if node_action is not None and inspect.iscoroutinefunction(node_action):
            return None

        route_fn = None
        route_map = None
        route_arity = 0
        if routing_spec is not None:
            route_fn, route_map = routing_spec
            route_arity = _positional_arity(route_fn)
            if route_arity is None or route_arity > 2:
                return None
            if inspect.iscoroutinefunction(route_fn):
                return None
            if route_map is None:
                return None
            if self._schema_merge:
                return None

        native_route_map = (
            {key: self._map_target_name(value) for key, value in route_map.items()}
            if route_map
            else None
        )
        return {
            "action": node_action,
            "action_arity": action_arity,
            "route": route_fn,
            "route_arity": route_arity,
            "route_map": native_route_map,
        }


class CompiledStateGraph:
    def __init__(
        self,
        native_graph: Any,
        name: str,
        *,
        state_schema: Any | None = None,
        owned_subgraphs: Sequence["CompiledStateGraph"] | None = None,
        graph_stores: GraphStoreRegistryView | None = None,
    ):
        self._native_graph = native_graph
        self._name = name
        self._state_schema = state_schema
        self._owned_subgraphs = list(owned_subgraphs or [])
        self._tool_registry_view = ToolRegistryView(native_graph)
        self._model_registry_view = ModelRegistryView(native_graph)
        self._graph_store_registry_view = graph_stores or GraphStoreRegistryView()

    @property
    def name(self) -> str:
        return self._name

    @property
    def tools(self) -> ToolRegistryView:
        return self._tool_registry_view

    @property
    def models(self) -> ModelRegistryView:
        return self._model_registry_view

    @property
    def graph_stores(self) -> GraphStoreRegistryView:
        return self._graph_store_registry_view

    def invoke(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        telemetry: Any | None = None,
    ) -> dict[str, Any]:
        initial_state = {} if input_state is None else dict(input_state)
        normalized_config = _normalize_config(config)
        observer = _coerce_telemetry_observer(telemetry)
        if observer is None:
            return _native._invoke(self._native_graph, initial_state, normalized_config)

        def run_with_details():
            _begin_context_collection()
            try:
                details = _native._invoke_with_details(
                    self._native_graph,
                    initial_state,
                    normalized_config,
                    include_subgraphs=True,
                )
            finally:
                _end_context_collection()
            return _attach_context_metadata(details)

        details = observer.capture_details(
            run_with_details,
            graph_name=self._name,
            operation="invoke",
            config=normalized_config,
            include_subgraphs=True,
        )
        return dict(details["state"])

    def invoke_with_metadata(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
        telemetry: Any | None = None,
    ) -> dict[str, Any]:
        initial_state = {} if input_state is None else dict(input_state)
        normalized_config = _normalize_config(config)
        observer = _coerce_telemetry_observer(telemetry)
        if observer is None:
            _begin_context_collection()
            try:
                details = _native._invoke_with_details(
                    self._native_graph,
                    initial_state,
                    normalized_config,
                    include_subgraphs=include_subgraphs,
                )
            finally:
                _end_context_collection()
            return _attach_context_metadata(details)

        def run_with_details():
            _begin_context_collection()
            try:
                details = _native._invoke_with_details(
                    self._native_graph,
                    initial_state,
                    normalized_config,
                    include_subgraphs=include_subgraphs,
                )
            finally:
                _end_context_collection()
            return _attach_context_metadata(details)

        return observer.capture_details(
            run_with_details,
            graph_name=self._name,
            operation="invoke_with_metadata",
            config=normalized_config,
            include_subgraphs=include_subgraphs,
        )

    def invoke_until_pause_with_metadata(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
        telemetry: Any | None = None,
    ) -> dict[str, Any]:
        initial_state = {} if input_state is None else dict(input_state)
        normalized_config = _normalize_config(config)
        observer = _coerce_telemetry_observer(telemetry)
        if observer is None:
            _begin_context_collection()
            try:
                details = _native._invoke_until_pause_with_details(
                    self._native_graph,
                    initial_state,
                    normalized_config,
                    include_subgraphs=include_subgraphs,
                )
            finally:
                _end_context_collection()
            return _attach_context_metadata(details)

        def run_until_pause_with_details():
            _begin_context_collection()
            try:
                details = _native._invoke_until_pause_with_details(
                    self._native_graph,
                    initial_state,
                    normalized_config,
                    include_subgraphs=include_subgraphs,
                )
            finally:
                _end_context_collection()
            return _attach_context_metadata(details)

        return observer.capture_details(
            run_until_pause_with_details,
            graph_name=self._name,
            operation="invoke_until_pause_with_metadata",
            config=normalized_config,
            include_subgraphs=include_subgraphs,
        )

    def resume_with_metadata(
        self,
        checkpoint_id: int,
        *,
        include_subgraphs: bool = True,
        telemetry: Any | None = None,
    ) -> dict[str, Any]:
        observer = _coerce_telemetry_observer(telemetry)
        if observer is None:
            _begin_context_collection()
            try:
                details = _native._resume_with_details(
                    self._native_graph,
                    int(checkpoint_id),
                    include_subgraphs=include_subgraphs,
                )
            finally:
                _end_context_collection()
            return _attach_context_metadata(details)

        def resume_with_details():
            _begin_context_collection()
            try:
                details = _native._resume_with_details(
                    self._native_graph,
                    int(checkpoint_id),
                    include_subgraphs=include_subgraphs,
                )
            finally:
                _end_context_collection()
            return _attach_context_metadata(details)

        return observer.capture_details(
            resume_with_details,
            graph_name=self._name,
            operation="resume_with_metadata",
            config={"checkpoint_id": int(checkpoint_id)},
            include_subgraphs=include_subgraphs,
        )

    def stream(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
        stream_mode: str = "events",
        telemetry: Any | None = None,
    ):
        if stream_mode != "events":
            raise NotImplementedError("the native state graph layer currently supports only stream_mode='events'")
        initial_state = {} if input_state is None else dict(input_state)
        normalized_config = _normalize_config(config)
        observer = _coerce_telemetry_observer(telemetry)

        def run_events():
            _begin_context_collection()
            try:
                event_sequence = _native._stream(
                    self._native_graph,
                    initial_state,
                    normalized_config,
                    include_subgraphs=include_subgraphs,
                )
                yield from _decorate_stream_event_iter(event_sequence)
            finally:
                _end_context_collection()

        if observer is None:
            yield from run_events()
            return
        events = list(run_events())
        for event in observer.capture_stream(
            lambda: list(events),
            graph_name=self._name,
            operation="stream",
            config=normalized_config,
            include_subgraphs=include_subgraphs,
        ):
            yield event

    async def ainvoke(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        telemetry: Any | None = None,
    ) -> dict[str, Any]:
        return await asyncio.to_thread(self.invoke, input_state, config=config, telemetry=telemetry)

    async def ainvoke_with_metadata(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
        telemetry: Any | None = None,
    ) -> dict[str, Any]:
        return await asyncio.to_thread(
            self.invoke_with_metadata,
            input_state,
            config=config,
            include_subgraphs=include_subgraphs,
            telemetry=telemetry,
        )

    async def astream(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
        stream_mode: str = "events",
        telemetry: Any | None = None,
    ):
        events = await asyncio.to_thread(
            lambda: list(
                self.stream(
                    input_state,
                    config=config,
                    include_subgraphs=include_subgraphs,
                    stream_mode=stream_mode,
                    telemetry=telemetry,
                )
            )
        )
        for event in events:
            yield event

    def batch(
        self,
        inputs: Iterable[dict[str, Any] | None],
        *,
        config: dict[str, Any] | Sequence[dict[str, Any] | None] | None = None,
        telemetry: Any | None = None,
    ) -> list[dict[str, Any]]:
        normalized_inputs = [None if item is None else dict(item) for item in inputs]
        normalized_configs = _normalize_batch_configs(config, len(normalized_inputs))
        return [
            self.invoke(input_state, config=run_config, telemetry=telemetry)
            for input_state, run_config in zip(normalized_inputs, normalized_configs)
        ]

    async def abatch(
        self,
        inputs: Iterable[dict[str, Any] | None],
        *,
        config: dict[str, Any] | Sequence[dict[str, Any] | None] | None = None,
        telemetry: Any | None = None,
    ) -> list[dict[str, Any]]:
        normalized_inputs = [None if item is None else dict(item) for item in inputs]
        normalized_configs = _normalize_batch_configs(config, len(normalized_inputs))
        tasks = [
            asyncio.create_task(self.ainvoke(input_state, config=run_config, telemetry=telemetry))
            for input_state, run_config in zip(normalized_inputs, normalized_configs)
        ]
        return [await task for task in tasks]
