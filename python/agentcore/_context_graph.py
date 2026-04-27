from __future__ import annotations

import hashlib
import json
import threading
from collections import defaultdict
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Any


_RANK_CACHE_LOCK = threading.Lock()
_RANK_CACHE: dict[str, list[str]] = {}
_MAX_RANK_CACHE_ENTRIES = 128


_KIND_PRIORITY = {
    "task": 900,
    "claim": 860,
    "evidence": 830,
    "decision": 790,
    "memory": 760,
    "knowledge": 730,
    "message": 680,
    "state": 620,
}


@dataclass(frozen=True)
class _ContextNode:
    node_id: str
    item: dict[str, Any]
    kind: str
    key: str
    source: str
    record: Mapping[str, Any]
    selector_index: int


class ContextGraphIndex:
    """Internal typed context graph used to rank existing ContextSpec candidates."""

    def __init__(
        self,
        items: Sequence[Mapping[str, Any]],
        *,
        spec: Mapping[str, Any],
        goal: Any = None,
    ):
        self._spec = dict(spec.items())
        self._goal_text = _norm_text(goal)
        self._nodes = _dedupe_nodes(items)
        self._selector_order = {
            str(selector): index
            for index, selector in enumerate(self._spec.get("include") or ())
        }
        self._adjacency: dict[str, list[tuple[str, int]]] = defaultdict(list)
        self._build_edges()

    def rank(self, *, limit: int | None = None) -> list[dict[str, Any]]:
        if not self._nodes:
            return []

        requested_limit = len(self._nodes) if limit is None else max(0, int(limit))
        if requested_limit == 0:
            return []

        cache_key = self._cache_key(requested_limit)
        cached_order = _rank_cache_get(cache_key)
        if cached_order is not None:
            by_id = {node.node_id: node for node in self._nodes}
            return [dict(by_id[node_id].item) for node_id in cached_order if node_id in by_id][:requested_limit]

        base_scores = {node.node_id: self._base_score(node) for node in self._nodes}
        activated = dict(base_scores)
        frontier = dict(base_scores)
        for depth in (1, 2):
            propagated: dict[str, int] = {}
            decay = 4 if depth == 1 else 7
            for node_id, score in frontier.items():
                if score <= 0:
                    continue
                for neighbor_id, edge_weight in self._adjacency.get(node_id, ()):
                    propagated[neighbor_id] = max(
                        propagated.get(neighbor_id, 0),
                        (score // decay) + edge_weight,
                    )
            for node_id, score in propagated.items():
                activated[node_id] = max(activated.get(node_id, 0), score)
            frontier = propagated

        sorted_nodes = sorted(
            self._nodes,
            key=lambda node: (
                -activated.get(node.node_id, 0),
                self._selector_order.get(node.source, node.selector_index),
                -_KIND_PRIORITY.get(node.kind, 0),
                node.kind,
                node.key,
                node.node_id,
            ),
        )
        selected_nodes = _balanced_top_k(sorted_nodes, requested_limit)
        order = [node.node_id for node in selected_nodes]
        _rank_cache_put(cache_key, order)
        return [dict(node.item) for node in selected_nodes]

    def _cache_key(self, limit: int) -> str:
        payload = {
            "limit": limit,
            "spec": self._spec,
            "goal": self._goal_text,
            "nodes": [
                {
                    "id": node.node_id,
                    "kind": node.kind,
                    "key": node.key,
                    "source": node.source,
                    "record": _stable_record(node.record),
                }
                for node in self._nodes
            ],
        }
        encoded = json.dumps(payload, sort_keys=True, separators=(",", ":"), default=str)
        return hashlib.blake2b(encoded.encode("utf-8"), digest_size=16).hexdigest()

    def _build_edges(self) -> None:
        by_key: dict[str, list[_ContextNode]] = defaultdict(list)
        by_task_key: dict[str, list[_ContextNode]] = defaultdict(list)
        by_claim_key: dict[str, list[_ContextNode]] = defaultdict(list)
        by_semantic: dict[str, list[_ContextNode]] = defaultdict(list)

        for node in self._nodes:
            by_key[node.key].append(node)
            record = node.record
            for field_name in ("task_key", "claim_key"):
                value = _field(record, field_name)
                if not value:
                    continue
                if field_name == "task_key":
                    by_task_key[value].append(node)
                else:
                    by_claim_key[value].append(node)
            for label in _semantic_labels(node):
                by_semantic[label].append(node)

        for node in self._nodes:
            if node.kind == "task":
                for linked in by_task_key.get(node.key, ()):
                    self._add_edge(node, linked, 520)
            if node.kind == "claim":
                for linked in by_claim_key.get(node.key, ()):
                    self._add_edge(node, linked, 560)
                for label in _semantic_labels(node):
                    for linked in by_semantic.get(label, ()):
                        if linked.node_id != node.node_id:
                            self._add_edge(node, linked, 210)
            if node.kind == "knowledge":
                for label in _semantic_labels(node):
                    for linked in by_semantic.get(label, ()):
                        if linked.node_id != node.node_id:
                            self._add_edge(node, linked, 160)

            task_key = _field(node.record, "task_key")
            claim_key = _field(node.record, "claim_key")
            if task_key:
                for linked in by_key.get(task_key, ()):
                    self._add_edge(node, linked, 440)
            if claim_key:
                for linked in by_key.get(claim_key, ()):
                    self._add_edge(node, linked, 500)

    def _add_edge(self, left: _ContextNode, right: _ContextNode, weight: int) -> None:
        if left.node_id == right.node_id:
            return
        self._adjacency[left.node_id].append((right.node_id, weight))
        self._adjacency[right.node_id].append((left.node_id, max(1, weight - 80)))

    def _base_score(self, node: _ContextNode) -> int:
        record = node.record
        score = _KIND_PRIORITY.get(node.kind, 500)
        score += max(0, 100 - (node.selector_index * 8))
        score += _status_score(record)
        score += int(_numeric(record.get("confidence")) * 180)
        score += int(_numeric(record.get("importance")) * 160)
        score += int(_numeric(record.get("priority")) * 12)

        if self._matches_seed(node):
            score += 2500
        if self._goal_text:
            text = _norm_text(node.item.get("text") or record)
            if text and any(token in text for token in self._goal_text.split() if len(token) > 3):
                score += 240
        return score

    def _matches_seed(self, node: _ContextNode) -> bool:
        spec_task = _norm_scalar(self._spec.get("task_key"))
        spec_claim = _norm_scalar(self._spec.get("claim_key"))
        spec_subject = _norm_scalar(self._spec.get("subject"))
        spec_relation = _norm_scalar(self._spec.get("relation"))
        spec_object = _norm_scalar(self._spec.get("object"))
        spec_owner = _norm_scalar(self._spec.get("owner"))
        spec_scope = _norm_scalar(self._spec.get("scope"))
        spec_source = _norm_scalar(self._spec.get("source"))

        record = node.record
        if spec_task and (node.key == spec_task or _field(record, "task_key") == spec_task):
            return True
        if spec_claim and (node.key == spec_claim or _field(record, "claim_key") == spec_claim):
            return True
        if spec_subject and _field(record, "subject") == spec_subject:
            return True
        if spec_relation and _field(record, "relation") == spec_relation:
            return True
        if spec_object and _field(record, "object") == spec_object:
            return True
        if spec_owner and _field(record, "owner") == spec_owner:
            return True
        if spec_scope and _field(record, "scope") == spec_scope:
            return True
        if spec_source and _field(record, "source") == spec_source:
            return True
        return False


def context_graph_cache_info() -> dict[str, int]:
    with _RANK_CACHE_LOCK:
        return {"entries": len(_RANK_CACHE)}


def _dedupe_nodes(items: Sequence[Mapping[str, Any]]) -> list[_ContextNode]:
    nodes: list[_ContextNode] = []
    seen: set[str] = set()
    for index, item_value in enumerate(items):
        item = dict(item_value.items())
        kind = str(item.get("kind") or "context")
        key = str(item.get("key") or item.get("id") or index)
        source = str(item.get("source") or kind)
        node_id = f"{kind}:{key}"
        if node_id in seen:
            continue
        seen.add(node_id)
        record_value = item.get("record")
        record = dict(record_value.items()) if isinstance(record_value, Mapping) else {"value": record_value}
        nodes.append(_ContextNode(node_id, item, kind, key, source, record, index))
    return nodes


def _balanced_top_k(nodes: Sequence[_ContextNode], limit: int) -> list[_ContextNode]:
    if limit <= 0:
        return []
    kinds = {node.kind for node in nodes}
    if len(kinds) <= 1:
        return list(nodes[:limit])

    per_kind_cap = max(2, (limit // max(1, len(kinds))) + 2)
    selected: list[_ContextNode] = []
    selected_ids: set[str] = set()
    kind_counts: dict[str, int] = defaultdict(int)
    for node in nodes:
        if len(selected) >= limit:
            break
        if kind_counts[node.kind] >= per_kind_cap:
            continue
        selected.append(node)
        selected_ids.add(node.node_id)
        kind_counts[node.kind] += 1

    if len(selected) < limit:
        for node in nodes:
            if len(selected) >= limit:
                break
            if node.node_id in selected_ids:
                continue
            selected.append(node)
            selected_ids.add(node.node_id)
    return selected


def _semantic_labels(node: _ContextNode) -> set[str]:
    record = node.record
    labels = {
        _field(record, "subject"),
        _field(record, "relation"),
        _field(record, "object"),
    }
    if node.kind == "knowledge":
        labels.update({_field(record, "source"), _field(record, "target"), _field(record, "value")})
    return {label for label in labels if label}


def _field(record: Mapping[str, Any], name: str) -> str:
    value = record.get(name)
    if value is None:
        return ""
    return str(value)


def _status_score(record: Mapping[str, Any]) -> int:
    status = str(record.get("status") or "").lower()
    if status in {"selected", "supported", "confirmed", "open", "in_progress"}:
        return 260
    if status in {"rejected", "unsupported", "cancelled"}:
        return -180
    return 0


def _numeric(value: Any) -> float:
    if value is None:
        return 0.0
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0


def _norm_scalar(value: Any) -> str:
    if value is None:
        return ""
    return str(value)


def _norm_text(value: Any) -> str:
    if value is None:
        return ""
    return str(value).lower()


def _stable_record(record: Mapping[str, Any]) -> Any:
    try:
        json.dumps(record, sort_keys=True, default=str)
        return record
    except TypeError:
        return {str(key): str(value) for key, value in record.items()}


def _rank_cache_get(key: str) -> list[str] | None:
    with _RANK_CACHE_LOCK:
        value = _RANK_CACHE.get(key)
        if value is None:
            return None
        return list(value)


def _rank_cache_put(key: str, order: list[str]) -> None:
    with _RANK_CACHE_LOCK:
        if len(_RANK_CACHE) >= _MAX_RANK_CACHE_ENTRIES:
            _RANK_CACHE.clear()
        _RANK_CACHE[key] = list(order)
