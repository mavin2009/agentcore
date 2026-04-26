from __future__ import annotations

from collections import deque
from collections.abc import Iterable
from threading import RLock
from typing import Any

from .base import GraphStore, GraphStoreCapabilities, normalize_entities, normalize_query, normalize_triples
from .types import GraphEntity, GraphNeighborhood, GraphTriple


class InMemoryGraphStore(GraphStore):
    """Deterministic in-process graph store useful for tests and small local graphs."""

    def __init__(
        self,
        *,
        entities: Iterable[GraphEntity | str | dict[str, Any]] | None = None,
        triples: Iterable[GraphTriple | tuple[Any, ...] | dict[str, Any]] | None = None,
        name: str = "memory",
    ):
        self._name = str(name)
        self._entities: dict[str, GraphEntity] = {}
        self._triples: dict[tuple[str, str, str], GraphTriple] = {}
        self._lock = RLock()
        if entities is not None:
            self.upsert_entities(entities)
        if triples is not None:
            self.upsert_triples(triples)

    @classmethod
    def from_triples(
        cls,
        triples: Iterable[GraphTriple | tuple[Any, ...] | dict[str, Any]],
        *,
        entities: Iterable[GraphEntity | str | dict[str, Any]] | None = None,
        name: str = "memory",
    ) -> "InMemoryGraphStore":
        return cls(entities=entities, triples=triples, name=name)

    def capabilities(self) -> GraphStoreCapabilities:
        return GraphStoreCapabilities(
            name="in_memory",
            batch_upsert=True,
            depth=True,
            property_filters=True,
            transactions=False,
            native_query=False,
            metadata={"deterministic": True, "durable": False},
        )

    def metadata(self) -> dict[str, Any]:
        with self._lock:
            return {
                "adapter": "in_memory",
                "name": self._name,
                "entities": len(self._entities),
                "triples": len(self._triples),
            }

    def upsert_entities(self, entities: Iterable[GraphEntity | str | dict[str, Any]]) -> int:
        normalized = normalize_entities(entities)
        with self._lock:
            for entity in normalized:
                previous = self._entities.get(entity.label)
                if previous is None:
                    self._entities[entity.label] = entity
                    continue
                payload = entity.payload if entity.payload is not None else previous.payload
                properties = {**previous.properties, **entity.properties}
                self._entities[entity.label] = GraphEntity(entity.label, payload=payload, properties=properties)
        return len(normalized)

    def upsert_triples(self, triples: Iterable[GraphTriple | tuple[Any, ...] | dict[str, Any]]) -> int:
        normalized = normalize_triples(triples)
        with self._lock:
            for triple in normalized:
                self._ensure_entity(triple.subject)
                self._ensure_entity(triple.object)
                self._triples[(triple.subject, triple.relation, triple.object)] = triple
        return len(normalized)

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
        query = normalize_query(
            subject=subject,
            relation=relation,
            object=object,
            direction=direction,
            depth=depth,
            limit=limit,
            properties=properties,
        )
        with self._lock:
            if query.direction == "neighborhood":
                return self._neighborhood(query)
            triples = self._filter_triples(query)
            return self._build_result(triples, query.subject)

    def _ensure_entity(self, label: str) -> None:
        if label not in self._entities:
            self._entities[label] = GraphEntity(label)

    def _filter_triples(self, query: Any) -> list[GraphTriple]:
        triples = list(self._triples.values())
        if query.direction == "incoming" and query.subject is not None and query.object is None:
            triples = [triple for triple in triples if triple.object == query.subject]
        elif query.direction == "outgoing" and query.subject is not None:
            triples = [triple for triple in triples if triple.subject == query.subject]
        elif query.direction == "both" and query.subject is not None and query.object is None:
            triples = [
                triple for triple in triples
                if triple.subject == query.subject or triple.object == query.subject
            ]
        else:
            if query.subject is not None:
                triples = [triple for triple in triples if triple.subject == query.subject]
            if query.object is not None:
                triples = [triple for triple in triples if triple.object == query.object]

        if query.relation is not None:
            triples = [triple for triple in triples if triple.relation == query.relation]
        if query.properties:
            triples = [
                triple for triple in triples
                if all(triple.properties.get(key) == value for key, value in query.properties.items())
            ]

        triples.sort(key=lambda triple: (triple.subject, triple.relation, triple.object))
        if query.limit is not None:
            triples = triples[:query.limit]
        return triples

    def _neighborhood(self, query: Any) -> GraphNeighborhood:
        if query.subject is None:
            triples = self._filter_triples(query)
            return self._build_result(triples, None)

        visited: set[str] = {query.subject}
        queue: deque[tuple[str, int]] = deque([(query.subject, 0)])
        collected: dict[tuple[str, str, str], GraphTriple] = {}
        while queue:
            entity, depth = queue.popleft()
            if depth >= query.depth:
                continue
            adjacent_query = normalize_query(
                subject=entity,
                relation=query.relation,
                direction="both",
                limit=None,
                properties=query.properties,
            )
            for triple in self._filter_triples(adjacent_query):
                key = (triple.subject, triple.relation, triple.object)
                if key not in collected:
                    collected[key] = triple
                for label in (triple.subject, triple.object):
                    if label not in visited:
                        visited.add(label)
                        queue.append((label, depth + 1))
                if query.limit is not None and len(collected) >= query.limit:
                    return self._build_result(list(collected.values()), query.subject)

        triples = sorted(collected.values(), key=lambda triple: (triple.subject, triple.relation, triple.object))
        if query.limit is not None:
            triples = triples[:query.limit]
        return self._build_result(triples, query.subject)

    def _build_result(self, triples: Iterable[GraphTriple], subject: str | None) -> GraphNeighborhood:
        triple_tuple = tuple(sorted(triples, key=lambda triple: (triple.subject, triple.relation, triple.object)))
        labels = {label for triple in triple_tuple for label in (triple.subject, triple.object)}
        if subject is not None:
            labels.add(subject)
        entities = tuple(self._entities[label] for label in sorted(labels) if label in self._entities)
        return GraphNeighborhood(
            entities=entities,
            triples=triple_tuple,
            store=self._name,
            subject=subject,
            metadata={"adapter": "in_memory"},
        )
