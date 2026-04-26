from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any


_VALID_DIRECTIONS = frozenset({"match", "outgoing", "incoming", "both", "neighborhood"})


def _as_payload(value: Any) -> Any | None:
    return None if value is None else value


def _as_properties(value: Any) -> dict[str, Any]:
    if value is None:
        return {}
    if not hasattr(value, "items"):
        raise TypeError("graph store properties must be a mapping or None")
    result: dict[str, Any] = {}
    for key, item in value.items():
        if not isinstance(key, str):
            raise TypeError("graph store property keys must be strings")
        result[key] = item
    return result


def _normalize_text(value: Any, *, field_name: str) -> str:
    text = str(value).strip()
    if not text:
        raise ValueError(f"{field_name} must be a non-empty string")
    return text


def normalize_direction(value: Any = "match") -> str:
    direction = str(value).strip().lower()
    if direction not in _VALID_DIRECTIONS:
        allowed = ", ".join(sorted(_VALID_DIRECTIONS))
        raise ValueError(f"direction must be one of: {allowed}")
    return direction


@dataclass(frozen=True)
class GraphEntity:
    label: str
    payload: Any | None = None
    properties: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "label", _normalize_text(self.label, field_name="entity label"))
        object.__setattr__(self, "payload", _as_payload(self.payload))
        object.__setattr__(self, "properties", _as_properties(self.properties))

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {"label": self.label}
        if self.payload is not None:
            result["payload"] = self.payload
        if self.properties:
            result["properties"] = dict(self.properties)
        return result


@dataclass(frozen=True)
class GraphTriple:
    subject: str
    relation: str
    object: str
    payload: Any | None = None
    properties: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "subject", _normalize_text(self.subject, field_name="triple subject"))
        object.__setattr__(self, "relation", _normalize_text(self.relation, field_name="triple relation"))
        object.__setattr__(self, "object", _normalize_text(self.object, field_name="triple object"))
        object.__setattr__(self, "payload", _as_payload(self.payload))
        object.__setattr__(self, "properties", _as_properties(self.properties))

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "subject": self.subject,
            "relation": self.relation,
            "object": self.object,
        }
        if self.payload is not None:
            result["payload"] = self.payload
        if self.properties:
            result["properties"] = dict(self.properties)
        return result


@dataclass(frozen=True)
class GraphNeighborhood:
    entities: tuple[GraphEntity, ...] = ()
    triples: tuple[GraphTriple, ...] = ()
    store: str | None = None
    subject: str | None = None
    metadata: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "entities", tuple(coerce_entity(entity) for entity in self.entities))
        object.__setattr__(self, "triples", tuple(coerce_triple(triple) for triple in self.triples))
        object.__setattr__(self, "metadata", _as_properties(self.metadata))
        if self.store is not None:
            object.__setattr__(self, "store", str(self.store))
        if self.subject is not None:
            object.__setattr__(self, "subject", str(self.subject))

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "entities": [entity.to_dict() for entity in self.entities],
            "triples": [triple.to_dict() for triple in self.triples],
        }
        if self.store is not None:
            result["store"] = self.store
        if self.subject is not None:
            result["subject"] = self.subject
        if self.metadata:
            result["metadata"] = dict(self.metadata)
        return result


@dataclass(frozen=True)
class GraphQuery:
    subject: str | None = None
    relation: str | None = None
    object: str | None = None
    direction: str = "match"
    depth: int = 1
    limit: int | None = None
    properties: dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "direction", normalize_direction(self.direction))
        depth = int(self.depth)
        if depth < 1:
            raise ValueError("depth must be >= 1")
        object.__setattr__(self, "depth", depth)
        if self.limit is not None:
            limit = int(self.limit)
            if limit < 0:
                raise ValueError("limit must be >= 0")
            object.__setattr__(self, "limit", limit)
        object.__setattr__(self, "properties", _as_properties(self.properties))
        if self.subject is not None:
            object.__setattr__(self, "subject", str(self.subject))
        if self.relation is not None:
            object.__setattr__(self, "relation", str(self.relation))
        if self.object is not None:
            object.__setattr__(self, "object", str(self.object))


def coerce_entity(value: Any) -> GraphEntity:
    if isinstance(value, GraphEntity):
        return value
    if isinstance(value, str):
        return GraphEntity(value)
    if isinstance(value, Mapping):
        label = value.get("label", value.get("id", value.get("name")))
        if label is None:
            raise ValueError("entity mappings require a label, id, or name")
        properties = value.get("properties")
        if properties is None:
            properties = {
                str(key): item
                for key, item in value.items()
                if key not in {"label", "id", "name", "payload"}
            }
        return GraphEntity(
            str(label),
            payload=value.get("payload"),
            properties=properties,
        )
    raise TypeError("entities must be GraphEntity, string, or mapping values")


def coerce_triple(value: Any) -> GraphTriple:
    if isinstance(value, GraphTriple):
        return value
    if isinstance(value, Mapping):
        missing = [key for key in ("subject", "relation", "object") if key not in value]
        if missing:
            raise ValueError(f"triple mappings require keys: {', '.join(missing)}")
        properties = value.get("properties")
        if properties is None:
            properties = {
                str(key): item
                for key, item in value.items()
                if key not in {"subject", "relation", "object", "payload"}
            }
        return GraphTriple(
            value["subject"],
            value["relation"],
            value["object"],
            payload=value.get("payload"),
            properties=properties,
        )
    if isinstance(value, Sequence) and not isinstance(value, (bytes, bytearray, memoryview, str)):
        if len(value) == 3:
            return GraphTriple(value[0], value[1], value[2])
        if len(value) == 4:
            return GraphTriple(value[0], value[1], value[2], payload=value[3])
        raise ValueError("triple sequences must have length 3 or 4")
    raise TypeError("triples must be GraphTriple, mapping, or tuple/list values")
