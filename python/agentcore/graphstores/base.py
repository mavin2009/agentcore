from __future__ import annotations

from collections.abc import Iterable
from dataclasses import dataclass, field
from typing import Any

from .types import GraphEntity, GraphNeighborhood, GraphQuery, GraphTriple, coerce_entity, coerce_triple


class GraphStoreError(RuntimeError):
    """Base exception for graph-store adapter failures."""


class GraphStoreUnavailableError(GraphStoreError):
    """Raised when an optional backend dependency or service is unavailable."""


@dataclass(frozen=True)
class GraphStoreCapabilities:
    name: str
    batch_upsert: bool = False
    depth: bool = False
    property_filters: bool = False
    transactions: bool = False
    native_query: bool = False
    metadata: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        result = {
            "name": self.name,
            "batch_upsert": self.batch_upsert,
            "depth": self.depth,
            "property_filters": self.property_filters,
            "transactions": self.transactions,
            "native_query": self.native_query,
        }
        if self.metadata:
            result["metadata"] = dict(self.metadata)
        return result


class GraphStore:
    """Backend-neutral interface for graph database adapters.

    The contract is intentionally expressed in entities, relation triples, and
    neighborhoods so runtime code can hydrate native knowledge state without
    depending on a specific query language.
    """

    def capabilities(self) -> GraphStoreCapabilities:
        return GraphStoreCapabilities(name=type(self).__name__)

    def metadata(self) -> dict[str, Any]:
        return {"adapter": type(self).__name__}

    def upsert_entity(self, entity: GraphEntity | str | dict[str, Any]) -> int:
        return self.upsert_entities([entity])

    def upsert_entities(self, entities: Iterable[GraphEntity | str | dict[str, Any]]) -> int:
        raise NotImplementedError

    def upsert_triple(self, triple: GraphTriple | tuple[Any, ...] | dict[str, Any]) -> int:
        return self.upsert_triples([triple])

    def upsert_triples(self, triples: Iterable[GraphTriple | tuple[Any, ...] | dict[str, Any]]) -> int:
        raise NotImplementedError

    def query(
        self,
        *,
        subject: str | None = None,
        relation: str | None = None,
        object: str | None = None,
        direction: str = "match",
        depth: int = 1,
        limit: int | None = None,
        properties: dict[str, Any] | None = None,
    ) -> GraphNeighborhood:
        raise NotImplementedError

    def neighborhood(
        self,
        entity: str,
        *,
        relation: str | None = None,
        depth: int = 1,
        limit: int | None = None,
        properties: dict[str, Any] | None = None,
    ) -> GraphNeighborhood:
        return self.query(
            subject=str(entity),
            relation=relation,
            direction="neighborhood",
            depth=depth,
            limit=limit,
            properties=properties,
        )

    def close(self) -> None:
        return None


def normalize_entities(entities: Iterable[GraphEntity | str | dict[str, Any]]) -> list[GraphEntity]:
    return [coerce_entity(entity) for entity in entities]


def normalize_triples(triples: Iterable[GraphTriple | tuple[Any, ...] | dict[str, Any]]) -> list[GraphTriple]:
    return [coerce_triple(triple) for triple in triples]


def normalize_query(
    *,
    subject: str | None = None,
    relation: str | None = None,
    object: str | None = None,
    direction: str = "match",
    depth: int = 1,
    limit: int | None = None,
    properties: dict[str, Any] | None = None,
) -> GraphQuery:
    return GraphQuery(
        subject=subject,
        relation=relation,
        object=object,
        direction=direction,
        depth=depth,
        limit=limit,
        properties={} if properties is None else properties,
    )
