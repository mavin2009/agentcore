from __future__ import annotations

import hashlib
import json
import threading
from collections.abc import Mapping
from dataclasses import dataclass, field
from typing import Any, Sequence

from ._context_graph import ContextGraphIndex


_DEFAULT_CONTEXT_INCLUDE = (
    "messages.recent",
    "tasks.agenda",
    "claims.supported",
    "evidence.relevant",
    "decisions.selected",
    "memories.working",
)

_VALID_FRESHNESS = {"committed", "staged"}
_CONTEXT_REGISTRY_LOCK = threading.Lock()
_CONTEXT_COLLECTION_DEPTH = 0
_CONTEXT_VIEWS_BY_RUN: dict[int, list[dict[str, Any]]] = {}


@dataclass(frozen=True)
class ContextSpec:
    """Declarative context assembly request for a graph node."""

    goal_key: str | None = None
    include: Sequence[str] | str | None = field(default_factory=lambda: _DEFAULT_CONTEXT_INCLUDE)
    budget_tokens: int | None = None
    budget_items: int | None = 32
    require_citations: bool = False
    freshness: str = "staged"
    message_key: str = "messages"
    limit_per_source: int = 5
    task_key: str | None = None
    claim_key: str | None = None
    owner: str | None = None
    scope: str | None = None
    subject: str | None = None
    relation: str | None = None
    object: str | None = None
    source: str | None = None

    def __post_init__(self) -> None:
        include = self.include
        if include is None:
            normalized_include = _DEFAULT_CONTEXT_INCLUDE
        elif isinstance(include, str):
            normalized_include = (include,)
        else:
            normalized_include = tuple(str(entry) for entry in include)
        if not normalized_include:
            raise ValueError("ContextSpec.include must contain at least one selector")
        object.__setattr__(self, "include", normalized_include)

        freshness = str(self.freshness).strip().lower()
        if freshness not in _VALID_FRESHNESS:
            raise ValueError("ContextSpec.freshness must be 'committed' or 'staged'")
        object.__setattr__(self, "freshness", freshness)

        if self.budget_tokens is not None and int(self.budget_tokens) < 0:
            raise ValueError("ContextSpec.budget_tokens must be >= 0")
        if self.budget_items is not None and int(self.budget_items) < 0:
            raise ValueError("ContextSpec.budget_items must be >= 0")
        if int(self.limit_per_source) < 0:
            raise ValueError("ContextSpec.limit_per_source must be >= 0")

    @classmethod
    def from_value(cls, value: "ContextSpec | Mapping[str, Any] | None") -> "ContextSpec | None":
        if value is None:
            return None
        if isinstance(value, ContextSpec):
            return value
        if isinstance(value, Mapping):
            return cls(**dict(value.items()))
        raise TypeError("context must be a ContextSpec, mapping, or None")

    def replace(self, **updates: Any) -> "ContextSpec":
        raw = self.to_dict()
        raw.update(updates)
        return ContextSpec(**raw)

    def to_dict(self) -> dict[str, Any]:
        return {
            "goal_key": self.goal_key,
            "include": list(self.include or ()),
            "budget_tokens": self.budget_tokens,
            "budget_items": self.budget_items,
            "require_citations": self.require_citations,
            "freshness": self.freshness,
            "message_key": self.message_key,
            "limit_per_source": self.limit_per_source,
            "task_key": self.task_key,
            "claim_key": self.claim_key,
            "owner": self.owner,
            "scope": self.scope,
            "subject": self.subject,
            "relation": self.relation,
            "object": self.object,
            "source": self.source,
        }


class ContextView:
    def __init__(
        self,
        *,
        spec: ContextSpec,
        node_name: str | None,
        goal: Any,
        items: list[dict[str, Any]],
        conflicts: list[dict[str, Any]],
        budget: dict[str, Any],
        identity: dict[str, Any] | None = None,
    ):
        self.spec = spec
        self.node_name = node_name
        self.goal = goal
        self.items = items
        self.conflicts = conflicts
        self.budget = budget
        self.identity = dict(identity or {})
        self.provenance = [
            {
                "citation": item["citation"],
                "id": item["id"],
                "kind": item["kind"],
                "key": item.get("key"),
                "source": item["source"],
            }
            for item in items
        ]
        self.digest = _stable_digest(self._digest_payload())

    def __len__(self) -> int:
        return len(self.items)

    def __iter__(self):
        return iter(self.items)

    def to_dict(self) -> dict[str, Any]:
        payload = {
            "digest": self.digest,
            "node_name": self.node_name,
            "goal": self.goal,
            "spec": self.spec.to_dict(),
            "items": [dict(item) for item in self.items],
            "provenance": [dict(item) for item in self.provenance],
            "conflicts": [dict(item) for item in self.conflicts],
            "budget": dict(self.budget),
        }
        payload.update(self.identity)
        return payload

    def to_prompt(self, *, system: str | None = None) -> str:
        lines: list[str] = []
        if system:
            lines.append(str(system).strip())
            lines.append("")
        if self.goal is not None:
            lines.append("Goal:")
            lines.append(_stringify(self.goal))
            lines.append("")
        if self.conflicts:
            lines.append("Potential context conflicts:")
            for conflict in self.conflicts:
                objects = ", ".join(_stringify(entry) for entry in conflict.get("objects", []))
                lines.append(f"- {conflict.get('subject')} {conflict.get('relation')}: {objects}")
            lines.append("")
        grouped: dict[str, list[dict[str, Any]]] = {}
        for item in self.items:
            grouped.setdefault(str(item["source"]), []).append(item)
        for source, records in grouped.items():
            lines.append(_title_for_source(source))
            for item in records:
                citation = f"[{item['citation']}] " if self.spec.require_citations else ""
                lines.append(f"- {citation}{item['text']}")
            lines.append("")
        return "\n".join(lines).rstrip()

    def to_messages(self, *, system: str | None = None) -> list[dict[str, str]]:
        messages: list[dict[str, str]] = []
        if system:
            messages.append({"role": "system", "content": str(system)})
        messages.append({"role": "user", "content": self.to_prompt()})
        return messages

    def to_model_input(self, *, mode: str = "text", system: str | None = None) -> Any:
        normalized = str(mode).strip().lower()
        if normalized == "text":
            return self.to_prompt(system=system)
        if normalized == "messages":
            return self.to_messages(system=system)
        if normalized in {"dict", "protocol"}:
            return self.to_dict()
        raise ValueError("mode must be 'text', 'messages', 'dict', or 'protocol'")

    def _digest_payload(self) -> dict[str, Any]:
        return {
            "node_name": self.node_name,
            "goal": self.goal,
            "spec": self.spec.to_dict(),
            "items": self.items,
            "conflicts": self.conflicts,
            "budget": self.budget,
            "identity": self.identity,
        }


class ContextAccessor:
    def __init__(self, runtime_context: Any):
        self._runtime_context = runtime_context
        self._spec: ContextSpec | None = None
        self._state: Mapping[str, Any] | None = None
        self._config: Mapping[str, Any] | None = None
        self._node_name: str | None = None
        self._last_view: ContextView | None = None

    @property
    def last_view(self) -> ContextView | None:
        return self._last_view

    def bind(
        self,
        *,
        spec: ContextSpec | None,
        state: Mapping[str, Any],
        config: Mapping[str, Any],
        node_name: str | None,
    ) -> None:
        self._spec = spec
        self._state = state
        self._config = config
        self._node_name = node_name

    def view(self, spec: ContextSpec | Mapping[str, Any] | None = None, **overrides: Any) -> ContextView:
        base_spec = ContextSpec.from_value(spec) or self._spec or ContextSpec()
        if overrides:
            base_spec = base_spec.replace(**overrides)
        if self._state is None:
            raise RuntimeError("context view is not bound to an active node state")
        identity = self._runtime_context._runtime_identity()
        view = assemble_context_view(
            runtime_context=self._runtime_context,
            state=self._state,
            spec=base_spec,
            node_name=self._node_name,
            identity=identity,
        )
        self._last_view = view
        _register_context_view(view)
        return view


def _begin_context_collection() -> None:
    global _CONTEXT_COLLECTION_DEPTH
    with _CONTEXT_REGISTRY_LOCK:
        _CONTEXT_COLLECTION_DEPTH += 1


def _end_context_collection() -> None:
    global _CONTEXT_COLLECTION_DEPTH
    with _CONTEXT_REGISTRY_LOCK:
        _CONTEXT_COLLECTION_DEPTH = max(0, _CONTEXT_COLLECTION_DEPTH - 1)


def _attach_context_metadata(details: dict[str, Any]) -> dict[str, Any]:
    summary = details.get("summary", {})
    run_id = summary.get("run_id") if isinstance(summary, Mapping) else None
    views = _pop_context_views_for_run(run_id)
    context = _build_context_summary(views)
    details["context"] = context
    proof = details.get("proof")
    if isinstance(proof, dict):
        proof["context_digest"] = context["combined_digest"]
    trace = details.get("trace")
    if isinstance(trace, list):
        _decorate_trace_events(trace, views)
    return details


def _decorate_stream_events(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    run_ids = [event.get("run_id") for event in events if isinstance(event, Mapping)]
    run_id = next((entry for entry in run_ids if entry is not None), None)
    views = _pop_context_views_for_run(run_id)
    _decorate_trace_events(events, views)
    return events


def _build_context_summary(views: list[dict[str, Any]]) -> dict[str, Any]:
    combined_digest = _stable_digest([view.get("digest") for view in views])
    by_node: dict[str, list[str]] = {}
    for view in views:
        node_name = str(view.get("node_name") or "")
        by_node.setdefault(node_name, []).append(str(view.get("digest") or ""))
    return {
        "count": len(views),
        "combined_digest": combined_digest,
        "views": views,
        "by_node": by_node,
    }


def assemble_context_view(
    *,
    runtime_context: Any,
    state: Mapping[str, Any],
    spec: ContextSpec,
    node_name: str | None,
    identity: dict[str, Any] | None = None,
) -> ContextView:
    goal = _state_get(state, spec.goal_key) if spec.goal_key else None
    items = _collect_context_graph(runtime_context, state, spec, goal=goal)
    if items is None:
        items = []
        for selector in spec.include or ():
            items.extend(_collect_selector(runtime_context, state, spec, str(selector)))

    items = _assign_citations(_apply_budget(items, spec))
    conflicts = _detect_claim_conflicts(items)
    budget = {
        "requested_tokens": spec.budget_tokens,
        "requested_items": spec.budget_items,
        "estimated_tokens": sum(int(item.get("estimated_tokens", 0)) for item in items),
        "included_items": len(items),
    }
    return ContextView(
        spec=spec,
        node_name=node_name,
        goal=goal,
        items=items,
        conflicts=conflicts,
        budget=budget,
        identity=identity,
    )


def _collect_context_graph(
    runtime_context: Any,
    state: Mapping[str, Any],
    spec: ContextSpec,
    *,
    goal: Any,
) -> list[dict[str, Any]] | None:
    selectors = [str(selector) for selector in (spec.include or ())]
    if not any(_selector_uses_context_graph(selector) for selector in selectors):
        return None

    requested_items = int(spec.budget_items) if spec.budget_items is not None else 32
    expanded_limit = max(int(spec.limit_per_source), requested_items, 8) * 3
    ranked_limit = max(requested_items, int(spec.limit_per_source) * max(1, len(selectors)))
    native_items = _collect_native_context_graph(runtime_context, spec, limit=ranked_limit)
    if native_items is not None:
        side_items: list[dict[str, Any]] = []
        try:
            for selector in selectors:
                if _selector_uses_context_graph(selector):
                    continue
                side_items.extend(_collect_selector(runtime_context, state, spec, selector))
        except (AttributeError, RuntimeError, TypeError, ValueError):
            side_items = []
        combined = native_items + side_items
        if not combined:
            return None
        if side_items:
            return ContextGraphIndex(
                combined,
                spec=spec.to_dict(),
                goal=goal,
            ).rank(limit=ranked_limit)
        return combined[:ranked_limit]

    try:
        expanded_spec = spec.replace(limit_per_source=expanded_limit)
        candidates: list[dict[str, Any]] = []
        for selector in selectors:
            candidates.extend(_collect_selector(runtime_context, state, expanded_spec, selector))
        if not candidates:
            return None
        return ContextGraphIndex(
            candidates,
            spec=spec.to_dict(),
            goal=goal,
        ).rank(limit=ranked_limit)
    except (AttributeError, RuntimeError, TypeError, ValueError):
        return None


def _collect_native_context_graph(
    runtime_context: Any,
    spec: ContextSpec,
    *,
    limit: int,
) -> list[dict[str, Any]] | None:
    try:
        result = runtime_context._rank_context_graph(spec.to_dict())
    except (AttributeError, RuntimeError, TypeError, ValueError):
        return None
    if not isinstance(result, Mapping) or not result.get("native"):
        return None
    records = result.get("records")
    if not isinstance(records, list):
        return None

    selectors = [str(selector) for selector in (spec.include or ())]
    items: list[dict[str, Any]] = []
    for entry in records:
        if not isinstance(entry, Mapping):
            continue
        kind = str(entry.get("kind") or "")
        record = entry.get("record")
        if not kind or not isinstance(record, Mapping):
            continue
        raw = dict(record.items())
        source = _native_context_source(kind, selectors)
        if kind == "knowledge":
            items.extend(_items_from_knowledge_records([raw], source=source, limit=1))
            continue
        key = raw.get("key") or raw.get("id") or f"{kind}:{len(items)}"
        items.append(_make_item(source, kind, str(key), raw))
        if len(items) >= limit:
            break
    return items[:limit]


def _native_context_source(kind: str, selectors: Sequence[str]) -> str:
    normalized_kind = kind.strip().lower()
    for selector in selectors:
        normalized = selector.strip().lower()
        if normalized_kind == "task" and normalized in {"tasks", "tasks.agenda", "actions.candidates", "intelligence.focus"}:
            return normalized
        if normalized_kind == "claim" and normalized in {"claims", "claims.all", "claims.supported", "claims.confirmed", "intelligence.focus"}:
            return normalized
        if normalized_kind == "evidence" and normalized in {"evidence", "evidence.all", "evidence.relevant", "intelligence.focus"}:
            return normalized
        if normalized_kind == "decision" and normalized in {"decisions", "decisions.all", "decisions.selected", "intelligence.focus"}:
            return normalized
        if normalized_kind == "memory" and (normalized in {"memories", "memories.recall", "intelligence.focus"} or normalized.startswith("memories.")):
            return normalized
        if normalized_kind == "knowledge" and normalized in {"knowledge", "knowledge.neighborhood"}:
            return normalized
    if normalized_kind == "task":
        return "tasks.agenda"
    if normalized_kind == "claim":
        return "claims.supported"
    if normalized_kind == "evidence":
        return "evidence.relevant"
    if normalized_kind == "decision":
        return "decisions.selected"
    if normalized_kind == "memory":
        return "memories.recall"
    if normalized_kind == "knowledge":
        return "knowledge.neighborhood"
    return normalized_kind or "context"


def _selector_uses_context_graph(selector: str) -> bool:
    normalized = selector.strip().lower()
    return (
        normalized in {
            "tasks",
            "tasks.agenda",
            "claims",
            "claims.all",
            "claims.supported",
            "claims.confirmed",
            "evidence",
            "evidence.all",
            "evidence.relevant",
            "decisions",
            "decisions.all",
            "decisions.selected",
            "memories",
            "memories.recall",
            "actions.candidates",
            "intelligence.focus",
            "knowledge",
            "knowledge.neighborhood",
        } or normalized.startswith("memories.")
    )


def _collect_selector(
    runtime_context: Any,
    state: Mapping[str, Any],
    spec: ContextSpec,
    selector: str,
) -> list[dict[str, Any]]:
    normalized = selector.strip().lower()
    limit = int(spec.limit_per_source)
    intelligence = runtime_context.intelligence

    if normalized in {"messages", "messages.recent", "messages.all"}:
        return _collect_messages(state, spec, recent=(normalized != "messages.all"), limit=limit)
    if normalized.startswith("state."):
        return _collect_state_field(state, selector[6:])
    if normalized in {"tasks", "tasks.agenda"}:
        return _items_from_records("tasks.agenda", "task", intelligence.agenda(
            task_key=spec.task_key,
            owner=spec.owner,
            limit=limit,
        ).get("tasks", []))
    if normalized in {"claims", "claims.all"}:
        return _items_from_records("claims.all", "claim", intelligence.query(
            kind="claims",
            task_key=spec.task_key,
            claim_key=spec.claim_key,
            subject=spec.subject,
            relation=spec.relation,
            object=spec.object,
            limit=limit,
        ).get("claims", []))
    if normalized in {"claims.supported", "claims.confirmed"}:
        status = "confirmed" if normalized.endswith("confirmed") else "supported"
        return _items_from_records("claims.supported", "claim", intelligence.supporting_claims(
            task_key=spec.task_key,
            claim_key=spec.claim_key,
            subject=spec.subject,
            relation=spec.relation,
            object=spec.object,
            status=status,
            limit=limit,
        ).get("claims", []))
    if normalized in {"evidence", "evidence.all", "evidence.relevant"}:
        if normalized == "evidence.relevant":
            records = intelligence.focus(
                task_key=spec.task_key,
                claim_key=spec.claim_key,
                subject=spec.subject,
                relation=spec.relation,
                object=spec.object,
                owner=spec.owner,
                source=spec.source,
                scope=spec.scope,
                limit=limit,
            ).get("evidence", [])
        else:
            records = intelligence.query(
                kind="evidence",
                task_key=spec.task_key,
                claim_key=spec.claim_key,
                source=spec.source,
                limit=limit,
            ).get("evidence", [])
        return _items_from_records("evidence.relevant", "evidence", records)
    if normalized in {"decisions", "decisions.all", "decisions.selected"}:
        return _items_from_records("decisions.selected", "decision", intelligence.query(
            kind="decisions",
            task_key=spec.task_key,
            claim_key=spec.claim_key,
            status="selected" if normalized == "decisions.selected" else None,
            limit=limit,
        ).get("decisions", []))
    if normalized.startswith("memories."):
        layer = normalized.split(".", 1)[1]
        layer_filter = None if layer in {"all", "recall"} else layer
        return _items_from_records(normalized, "memory", intelligence.recall(
            task_key=spec.task_key,
            claim_key=spec.claim_key,
            scope=spec.scope,
            layer=layer_filter,
            limit=limit,
        ).get("memories", []))
    if normalized in {"memories", "memories.recall"}:
        return _items_from_records("memories.recall", "memory", intelligence.recall(
            task_key=spec.task_key,
            claim_key=spec.claim_key,
            scope=spec.scope,
            limit=limit,
        ).get("memories", []))
    if normalized == "actions.candidates":
        return _items_from_records("actions.candidates", "task", intelligence.action_candidates(
            task_key=spec.task_key,
            claim_key=spec.claim_key,
            subject=spec.subject,
            relation=spec.relation,
            object=spec.object,
            owner=spec.owner,
            source=spec.source,
            scope=spec.scope,
            limit=limit,
        ).get("tasks", []))
    if normalized == "intelligence.focus":
        focus = intelligence.focus(
            task_key=spec.task_key,
            claim_key=spec.claim_key,
            subject=spec.subject,
            relation=spec.relation,
            object=spec.object,
            owner=spec.owner,
            source=spec.source,
            scope=spec.scope,
            limit=limit,
        )
        records: list[dict[str, Any]] = []
        for key, kind in (
            ("tasks", "task"),
            ("claims", "claim"),
            ("evidence", "evidence"),
            ("decisions", "decision"),
            ("memories", "memory"),
        ):
            records.extend(_items_from_records("intelligence.focus", kind, focus.get(key, [])))
        return records[:limit]
    if normalized in {"knowledge.neighborhood", "knowledge"}:
        return _collect_knowledge(runtime_context, state, spec, limit=limit)
    raise ValueError(f"unsupported context selector: {selector!r}")


def _collect_messages(
    state: Mapping[str, Any],
    spec: ContextSpec,
    *,
    recent: bool,
    limit: int,
) -> list[dict[str, Any]]:
    value = _state_get(state, spec.message_key)
    if value is None:
        return []
    messages = list(value) if isinstance(value, (list, tuple)) else [value]
    if limit == 0:
        selected = []
    elif recent:
        selected = messages[-limit:]
    else:
        selected = messages
    items: list[dict[str, Any]] = []
    offset = len(messages) - len(selected)
    for index, message in enumerate(selected):
        raw = dict(message.items()) if isinstance(message, Mapping) else {"content": message}
        key = raw.get("id") or f"message:{offset + index}"
        role = raw.get("role", "message")
        content = raw.get("content", raw)
        text = f"{role}: {_stringify(content)}"
        items.append(_make_item("messages.recent", "message", str(key), raw, text=text))
    return items


def _collect_state_field(state: Mapping[str, Any], field_name: str) -> list[dict[str, Any]]:
    field_name = str(field_name).strip()
    if not field_name:
        raise ValueError("state context selectors must be formatted as state.<field>")
    value = _state_get(state, field_name)
    if value is None:
        return []
    return [_make_item(f"state.{field_name}", "state", field_name, value)]


def _collect_knowledge(
    runtime_context: Any,
    state: Mapping[str, Any],
    spec: ContextSpec,
    *,
    limit: int,
) -> list[dict[str, Any]]:
    if limit == 0:
        return []

    items: list[dict[str, Any]] = []
    direction = "neighborhood" if spec.subject and spec.object is None else "match"
    try:
        native_result = runtime_context.knowledge.query(
            subject=spec.subject,
            relation=spec.relation,
            object=spec.object,
            direction=direction,
            limit=limit,
        )
    except (AttributeError, RuntimeError, TypeError, ValueError):
        native_result = {}

    if isinstance(native_result, Mapping):
        items.extend(_items_from_knowledge_records(
            native_result.get("triples", []),
            source="knowledge.neighborhood",
            limit=limit,
        ))

    remaining = max(limit - len(items), 0)
    if remaining > 0:
        items.extend(_collect_knowledge_from_state(state, limit=remaining))
    return items[:limit]


def _collect_knowledge_from_state(state: Mapping[str, Any], *, limit: int) -> list[dict[str, Any]]:
    value = (
        _state_get(state, "knowledge_graph") or
        _state_get(state, "knowledge") or
        _state_get(state, "triples")
    )
    if value is None:
        return []
    records = value
    if isinstance(value, Mapping):
        records = value.get("triples", [])
    if not isinstance(records, (list, tuple)):
        records = [records]
    return _items_from_knowledge_records(records, source="knowledge.neighborhood", limit=limit)


def _items_from_knowledge_records(
    records: Any,
    *,
    source: str,
    limit: int,
) -> list[dict[str, Any]]:
    items: list[dict[str, Any]] = []
    if not isinstance(records, (list, tuple)):
        records = [records]
    selected = records[:limit] if limit else []
    for index, record in enumerate(selected):
        raw = dict(record.items()) if isinstance(record, Mapping) else {"value": record}
        key = raw.get("key") or raw.get("id") or f"knowledge:{index}"
        subject = raw.get("subject", raw.get("source", ""))
        relation = raw.get("relation", raw.get("edge", "related_to"))
        object_value = raw.get("object", raw.get("target", raw.get("value", "")))
        text = f"{subject} {relation} {object_value}".strip()
        items.append(_make_item(source, "knowledge", str(key), raw, text=text))
    return items


def _items_from_records(source: str, kind: str, records: Any) -> list[dict[str, Any]]:
    if not isinstance(records, list):
        return []
    items: list[dict[str, Any]] = []
    for index, record in enumerate(records):
        raw = dict(record.items()) if isinstance(record, Mapping) else {"value": record}
        key = raw.get("key") or raw.get("id") or f"{kind}:{index}"
        items.append(_make_item(source, kind, str(key), raw))
    return items


def _make_item(
    source: str,
    kind: str,
    key: str,
    raw: Any,
    *,
    text: str | None = None,
) -> dict[str, Any]:
    item_text = text if text is not None else _record_text(kind, raw)
    return {
        "id": f"{kind}:{key}",
        "key": key,
        "kind": kind,
        "source": source,
        "record": raw,
        "text": item_text,
        "estimated_tokens": _estimate_tokens(item_text),
    }


def _record_text(kind: str, record: Any) -> str:
    if not isinstance(record, Mapping):
        return _stringify(record)
    if kind == "task":
        title = record.get("title") or record.get("key")
        status = record.get("status")
        owner = record.get("owner")
        return " ".join(str(part) for part in (title, f"status={status}" if status else None, f"owner={owner}" if owner else None) if part)
    if kind == "claim":
        subject = record.get("subject")
        relation = record.get("relation")
        object_value = record.get("object")
        status = record.get("status")
        confidence = record.get("confidence")
        statement = record.get("statement")
        core = " ".join(str(part) for part in (subject, relation, object_value) if part)
        detail = _stringify(statement) if statement not in (None, "") else ""
        suffix = " ".join(str(part) for part in (f"status={status}" if status else None, f"confidence={confidence}" if confidence is not None else None) if part)
        return " ".join(part for part in (core, detail, suffix) if part)
    if kind == "evidence":
        return " ".join(str(part) for part in (
            record.get("source"),
            _stringify(record.get("content")),
            f"confidence={record.get('confidence')}" if record.get("confidence") is not None else None,
        ) if part and part != "None")
    if kind == "decision":
        return " ".join(str(part) for part in (
            record.get("status"),
            _stringify(record.get("summary")),
            record.get("claim_key"),
            record.get("task_key"),
        ) if part and part != "None")
    if kind == "memory":
        return " ".join(str(part) for part in (
            record.get("layer"),
            record.get("scope"),
            _stringify(record.get("content")),
            f"importance={record.get('importance')}" if record.get("importance") is not None else None,
        ) if part and part != "None")
    return _stringify(record)


def _apply_budget(items: list[dict[str, Any]], spec: ContextSpec) -> list[dict[str, Any]]:
    if spec.budget_items is not None:
        items = items[:int(spec.budget_items)]
    if spec.budget_tokens is None:
        return items
    budget = int(spec.budget_tokens)
    selected: list[dict[str, Any]] = []
    used = 0
    for item in items:
        tokens = int(item.get("estimated_tokens", 0))
        if selected and used + tokens > budget:
            break
        selected.append(item)
        used += tokens
        if used >= budget:
            break
    return selected


def _assign_citations(items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    assigned: list[dict[str, Any]] = []
    for index, item in enumerate(items, start=1):
        copied = dict(item)
        copied["citation"] = f"C{index}"
        assigned.append(copied)
    return assigned


def _detect_claim_conflicts(items: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str], dict[str, Any]] = {}
    for item in items:
        if item.get("kind") != "claim":
            continue
        record = item.get("record")
        if not isinstance(record, Mapping):
            continue
        status = str(record.get("status", "")).lower()
        if status not in {"supported", "confirmed"}:
            continue
        subject = record.get("subject")
        relation = record.get("relation")
        object_value = record.get("object")
        if not subject or not relation or object_value is None:
            continue
        bucket = grouped.setdefault((str(subject), str(relation)), {"objects": {}, "claim_keys": []})
        bucket["objects"].setdefault(str(object_value), []).append(item.get("key"))
        bucket["claim_keys"].append(item.get("key"))

    conflicts: list[dict[str, Any]] = []
    for (subject, relation), bucket in grouped.items():
        objects = sorted(bucket["objects"].keys())
        if len(objects) <= 1:
            continue
        conflicts.append({
            "subject": subject,
            "relation": relation,
            "objects": objects,
            "claim_keys": sorted(str(key) for key in bucket["claim_keys"] if key is not None),
        })
    return conflicts


def _register_context_view(view: ContextView) -> None:
    with _CONTEXT_REGISTRY_LOCK:
        if _CONTEXT_COLLECTION_DEPTH <= 0:
            return
        run_id = view.identity.get("run_id")
        if run_id is None:
            return
        _CONTEXT_VIEWS_BY_RUN.setdefault(int(run_id), []).append(view.to_dict())


def _pop_context_views_for_run(run_id: Any) -> list[dict[str, Any]]:
    if run_id is None:
        return []
    try:
        normalized_run_id = int(run_id)
    except (TypeError, ValueError):
        return []
    with _CONTEXT_REGISTRY_LOCK:
        return _CONTEXT_VIEWS_BY_RUN.pop(normalized_run_id, [])


def _decorate_trace_events(events: list[dict[str, Any]], views: list[dict[str, Any]]) -> None:
    by_event: dict[tuple[int, int, int], list[dict[str, Any]]] = {}
    for view in views:
        try:
            key = (int(view["run_id"]), int(view["node_id"]), int(view.get("branch_id", 0)))
        except (KeyError, TypeError, ValueError):
            continue
        by_event.setdefault(key, []).append(view)
    for event in events:
        if not isinstance(event, dict):
            continue
        try:
            key = (int(event["run_id"]), int(event["node_id"]), int(event.get("branch_id", 0)))
        except (KeyError, TypeError, ValueError):
            continue
        matched = by_event.get(key)
        if not matched:
            continue
        event["context_views"] = matched
        event["context_digest"] = _stable_digest([view.get("digest") for view in matched])


def _state_get(state: Mapping[str, Any], key: str | None, default: Any = None) -> Any:
    if key is None:
        return default
    try:
        return state.get(key, default)
    except AttributeError:
        try:
            return state[key]
        except KeyError:
            return default


def _stringify(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    try:
        return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)
    except TypeError:
        return str(value)


def _estimate_tokens(text: str) -> int:
    if not text:
        return 0
    return max(1, (len(text) + 3) // 4)


def _stable_digest(value: Any) -> str:
    payload = json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def _title_for_source(source: str) -> str:
    return source.replace(".", " ").replace("_", " ").title() + ":"
