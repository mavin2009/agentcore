from __future__ import annotations

import json
from collections import deque
from collections.abc import Iterable
from typing import Any

from .base import (
    GraphStore,
    GraphStoreCapabilities,
    GraphStoreUnavailableError,
    normalize_entities,
    normalize_query,
    normalize_triples,
)
from .types import GraphEntity, GraphNeighborhood, GraphTriple


def _load_driver():
    try:
        from neo4j import GraphDatabase
    except ImportError as exc:
        raise GraphStoreUnavailableError(
            "Neo4j support requires the optional neo4j driver. "
            "Install with `pip install agentcore-graph[neo4j]` or `pip install neo4j`."
        ) from exc
    return GraphDatabase


def _json_dumps(value: Any) -> str:
    if value is None:
        return ""
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def _json_loads(value: Any) -> Any:
    if not value:
        return None
    try:
        return json.loads(str(value))
    except json.JSONDecodeError:
        return None


def _properties_loads(value: Any) -> dict[str, Any]:
    decoded = _json_loads(value)
    return decoded if isinstance(decoded, dict) else {}


def _entity_to_record(entity: GraphEntity) -> dict[str, Any]:
    return {
        "label": entity.label,
        "payload_json": _json_dumps(entity.payload),
        "properties_json": _json_dumps(entity.properties),
    }


def _triple_to_record(triple: GraphTriple) -> dict[str, Any]:
    return {
        "subject": triple.subject,
        "relation": triple.relation,
        "object": triple.object,
        "payload_json": _json_dumps(triple.payload),
        "properties_json": _json_dumps(triple.properties),
    }


def _record_to_entity(record: Any, prefix: str) -> GraphEntity:
    return GraphEntity(
        record[f"{prefix}_label"],
        payload=_json_loads(record.get(f"{prefix}_payload_json")),
        properties=_properties_loads(record.get(f"{prefix}_properties_json")),
    )


def _record_to_triple(record: Any) -> GraphTriple:
    return GraphTriple(
        record["subject"],
        record["relation"],
        record["object"],
        payload=_json_loads(record.get("payload_json")),
        properties=_properties_loads(record.get("properties_json")),
    )


class Neo4jGraphStore(GraphStore):
    """Neo4j-backed graph store using safe generic relationship records.

    Arbitrary relation names are stored as a relationship property rather than
    interpolated into Cypher relationship types. This keeps the adapter safe and
    portable while still allowing domain-specific relation labels.
    """

    def __init__(
        self,
        uri: str,
        *,
        auth: Any | None = None,
        database: str | None = None,
        driver: Any | None = None,
        name: str = "neo4j",
    ):
        self._name = str(name)
        self._database = None if database is None else str(database)
        if driver is None:
            graph_database = _load_driver()
            self._driver = graph_database.driver(str(uri), auth=auth)
            self._owns_driver = True
        else:
            self._driver = driver
            self._owns_driver = False

    @classmethod
    def from_env(
        cls,
        *,
        uri_env: str = "NEO4J_URI",
        user_env: str = "NEO4J_USER",
        password_env: str = "NEO4J_PASSWORD",
        database_env: str = "NEO4J_DATABASE",
        name: str = "neo4j",
    ) -> "Neo4jGraphStore":
        import os

        uri = os.getenv(uri_env)
        if not uri:
            raise GraphStoreUnavailableError(f"{uri_env} is required to create a Neo4jGraphStore")
        user = os.getenv(user_env)
        password = os.getenv(password_env)
        auth = (user, password) if user is not None or password is not None else None
        return cls(uri, auth=auth, database=os.getenv(database_env), name=name)

    def capabilities(self) -> GraphStoreCapabilities:
        return GraphStoreCapabilities(
            name="neo4j",
            batch_upsert=True,
            depth=True,
            property_filters=True,
            transactions=True,
            native_query=True,
            metadata={"relationship_model": "generic_property_relation"},
        )

    def metadata(self) -> dict[str, Any]:
        return {
            "adapter": "neo4j",
            "name": self._name,
            "database": self._database,
            "relationship_type": "AGENTCORE_RELATION",
            "entity_label": "AgentCoreEntity",
        }

    def upsert_entities(self, entities: Iterable[GraphEntity | str | dict[str, Any]]) -> int:
        normalized = normalize_entities(entities)
        if not normalized:
            return 0
        records = [_entity_to_record(entity) for entity in normalized]
        self._execute_write(
            """
            UNWIND $entities AS entity
            MERGE (n:AgentCoreEntity {agentcore_id: entity.label})
            SET n.label = entity.label,
                n.payload_json = entity.payload_json,
                n.properties_json = entity.properties_json
            """,
            entities=records,
        )
        return len(records)

    def upsert_triples(self, triples: Iterable[GraphTriple | tuple[Any, ...] | dict[str, Any]]) -> int:
        normalized = normalize_triples(triples)
        if not normalized:
            return 0
        records = [_triple_to_record(triple) for triple in normalized]
        self._execute_write(
            """
            UNWIND $triples AS triple
            MERGE (s:AgentCoreEntity {agentcore_id: triple.subject})
            SET s.label = triple.subject
            MERGE (o:AgentCoreEntity {agentcore_id: triple.object})
            SET o.label = triple.object
            MERGE (s)-[r:AGENTCORE_RELATION {relation: triple.relation}]->(o)
            SET r.payload_json = triple.payload_json,
                r.properties_json = triple.properties_json
            """,
            triples=records,
        )
        return len(records)

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
        if query.direction == "neighborhood":
            return self._neighborhood(query)
        records = self._execute_read(self._direct_query_cypher(query.direction), **self._query_params(query))
        return self._build_result(records, query.subject, query.properties)

    def close(self) -> None:
        if self._owns_driver:
            self._driver.close()

    def _direct_query_cypher(self, direction: str) -> str:
        if direction == "incoming":
            subject_clause = "($subject IS NULL OR o.agentcore_id = $subject)"
            object_clause = "($object IS NULL OR s.agentcore_id = $object)"
        elif direction == "both":
            subject_clause = (
                "($subject IS NULL OR s.agentcore_id = $subject OR o.agentcore_id = $subject)"
            )
            object_clause = "($object IS NULL OR s.agentcore_id = $object OR o.agentcore_id = $object)"
        else:
            subject_clause = "($subject IS NULL OR s.agentcore_id = $subject)"
            object_clause = "($object IS NULL OR o.agentcore_id = $object)"
        return f"""
            MATCH (s:AgentCoreEntity)-[r:AGENTCORE_RELATION]->(o:AgentCoreEntity)
            WHERE {subject_clause}
              AND {object_clause}
              AND ($relation IS NULL OR r.relation = $relation)
            RETURN
              s.label AS source_label,
              s.payload_json AS source_payload_json,
              s.properties_json AS source_properties_json,
              o.label AS target_label,
              o.payload_json AS target_payload_json,
              o.properties_json AS target_properties_json,
              s.label AS subject,
              r.relation AS relation,
              o.label AS object,
              r.payload_json AS payload_json,
              r.properties_json AS properties_json
            ORDER BY subject, relation, object
            LIMIT $limit
        """

    def _query_params(self, query: Any) -> dict[str, Any]:
        return {
            "subject": query.subject,
            "relation": query.relation,
            "object": query.object,
            "limit": 1_000_000 if query.limit is None else int(query.limit),
        }

    def _neighborhood(self, query: Any) -> GraphNeighborhood:
        if query.subject is None:
            records = self._execute_read(self._direct_query_cypher("both"), **self._query_params(query))
            return self._build_result(records, None, query.properties)

        visited: set[str] = {query.subject}
        queue: deque[tuple[str, int]] = deque([(query.subject, 0)])
        collected: dict[tuple[str, str, str], dict[str, Any]] = {}
        while queue:
            entity, current_depth = queue.popleft()
            if current_depth >= query.depth:
                continue
            step = normalize_query(
                subject=entity,
                relation=query.relation,
                direction="both",
                limit=None,
            )
            records = self._execute_read(self._direct_query_cypher("both"), **self._query_params(step))
            for record in records:
                triple = _record_to_triple(record)
                if query.properties and not all(
                    triple.properties.get(key) == value for key, value in query.properties.items()
                ):
                    continue
                key = (triple.subject, triple.relation, triple.object)
                collected.setdefault(key, record)
                for label in (triple.subject, triple.object):
                    if label not in visited:
                        visited.add(label)
                        queue.append((label, current_depth + 1))
                if query.limit is not None and len(collected) >= query.limit:
                    return self._build_result(collected.values(), query.subject, {})

        records = [collected[key] for key in sorted(collected)]
        if query.limit is not None:
            records = records[:query.limit]
        return self._build_result(records, query.subject, {})

    def _build_result(
        self,
        records: Iterable[Any],
        subject: str | None,
        properties: dict[str, Any],
    ) -> GraphNeighborhood:
        entities: dict[str, GraphEntity] = {}
        triples: dict[tuple[str, str, str], GraphTriple] = {}
        for record in records:
            triple = _record_to_triple(record)
            if properties and not all(triple.properties.get(key) == value for key, value in properties.items()):
                continue
            source = _record_to_entity(record, "source")
            target = _record_to_entity(record, "target")
            entities[source.label] = source
            entities[target.label] = target
            triples[(triple.subject, triple.relation, triple.object)] = triple
        if subject is not None and subject not in entities:
            entities[subject] = GraphEntity(subject)
        return GraphNeighborhood(
            entities=tuple(entities[key] for key in sorted(entities)),
            triples=tuple(triples[key] for key in sorted(triples)),
            store=self._name,
            subject=subject,
            metadata={"adapter": "neo4j"},
        )

    def _execute_read(self, cypher: str, **parameters: Any) -> list[Any]:
        with self._driver.session(database=self._database) as session:
            result = session.run(cypher, **parameters)
            return [dict(record) for record in result]

    def _execute_write(self, cypher: str, **parameters: Any) -> None:
        with self._driver.session(database=self._database) as session:
            execute_write = getattr(session, "execute_write", None)
            if execute_write is not None:
                execute_write(lambda tx: list(tx.run(cypher, **parameters)))
                return
            write_transaction = getattr(session, "write_transaction", None)
            if write_transaction is not None:
                write_transaction(lambda tx: list(tx.run(cypher, **parameters)))
                return
            list(session.run(cypher, **parameters))
