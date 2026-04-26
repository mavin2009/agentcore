from __future__ import annotations

from collections.abc import Iterable
from typing import Any

from .base import GraphStore
from .memory import InMemoryGraphStore
from .types import GraphEntity, GraphTriple


def _normalize_store_name(name: Any) -> str:
    normalized = str(name).strip()
    if not normalized:
        raise ValueError("graph store name must be a non-empty string")
    return normalized


class GraphStoreRegistryView:
    """Graph-owned registry for optional external graph stores."""

    def __init__(self):
        self._stores: dict[str, GraphStore] = {}

    def register(self, name: str, store: GraphStore) -> GraphStore:
        normalized_name = _normalize_store_name(name)
        if not isinstance(store, GraphStore):
            required = ("upsert_entities", "upsert_triples", "query", "neighborhood")
            if not all(hasattr(store, attribute) for attribute in required):
                raise TypeError("graph store must implement the GraphStore interface")
        self._stores[normalized_name] = store
        return store

    def register_memory(
        self,
        name: str = "memory",
        *,
        entities: Iterable[GraphEntity | str | dict[str, Any]] | None = None,
        triples: Iterable[GraphTriple | tuple[Any, ...] | dict[str, Any]] | None = None,
    ) -> InMemoryGraphStore:
        store = InMemoryGraphStore(entities=entities, triples=triples, name=name)
        self.register(name, store)
        return store

    def register_neo4j(
        self,
        name: str = "neo4j",
        *,
        uri: str | None = None,
        auth: Any | None = None,
        database: str | None = None,
        driver: Any | None = None,
        from_env: bool = False,
    ) -> GraphStore:
        from .neo4j import Neo4jGraphStore

        if from_env:
            store = Neo4jGraphStore.from_env(name=name)
        else:
            if uri is None and driver is None:
                raise ValueError("uri or driver is required when from_env=False")
            store = Neo4jGraphStore(
                "" if uri is None else str(uri),
                auth=auth,
                database=database,
                driver=driver,
                name=name,
            )
        self.register(name, store)
        return store

    def get(self, name: str) -> GraphStore:
        normalized_name = _normalize_store_name(name)
        try:
            return self._stores[normalized_name]
        except KeyError as exc:
            available = ", ".join(sorted(self._stores)) or "<none>"
            raise KeyError(f"unknown graph store {normalized_name!r}; available stores: {available}") from exc

    def remove(self, name: str, *, close: bool = False) -> GraphStore:
        normalized_name = _normalize_store_name(name)
        store = self._stores.pop(normalized_name)
        if close:
            store.close()
        return store

    def list(self) -> list[dict[str, Any]]:
        return [
            {
                "name": name,
                "metadata": store.metadata(),
                "capabilities": store.capabilities().to_dict(),
            }
            for name, store in sorted(self._stores.items())
        ]

    def close_all(self) -> None:
        for store in list(self._stores.values()):
            store.close()
