from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping

from ..graph import CompiledStateGraph, END, START, StateGraph


def _normalize_string_mapping(
    value: Mapping[str, str] | None,
    *,
    field_name: str,
) -> dict[str, str]:
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


def _resolve_session_id_from(
    *,
    session_mode: str,
    session_id_from: str | None,
    inputs: Mapping[str, str],
    specialist_name: str,
) -> str | None:
    normalized_mode = str(session_mode).strip().lower()
    if normalized_mode not in {"ephemeral", "persistent"}:
        raise ValueError("session_mode must be either 'ephemeral' or 'persistent'")
    if normalized_mode == "ephemeral":
        if session_id_from is not None:
            raise ValueError("ephemeral specialists must not declare session_id_from")
        return None

    if isinstance(session_id_from, str) and session_id_from:
        return session_id_from

    inferred = [parent_key for parent_key, child_key in inputs.items() if child_key == "session_id"]
    if len(inferred) == 1:
        return inferred[0]
    if len(inferred) > 1:
        raise ValueError(
            "persistent specialists with multiple parent-to-session_id bindings must "
            f"declare session_id_from explicitly for {specialist_name!r}"
        )
    raise ValueError(
        "persistent specialists require session_id_from or a single input binding "
        f"mapping a parent field to 'session_id' for {specialist_name!r}"
    )


@dataclass(frozen=True)
class Specialist:
    name: str
    graph: StateGraph | CompiledStateGraph
    inputs: Mapping[str, str] = field(default_factory=dict)
    outputs: Mapping[str, str] = field(default_factory=dict)
    namespace: str | None = None
    propagate_knowledge_graph: bool = False
    session_mode: str = "persistent"
    session_id_from: str | None = None
    prepare: Any | None = None
    prepare_kind: str = "compute"


class SpecialistTeam:
    def __init__(
        self,
        state_schema: Any = None,
        *,
        name: str | None = None,
        worker_count: int = 4,
    ):
        self._state_schema = state_schema
        self._name = name or getattr(state_schema, "__name__", "agentcore_specialist_team")
        self._worker_count = max(1, int(worker_count))
        self._dispatch_name = "dispatch"
        self._dispatch_action: Any | None = None
        self._dispatch_kind = "control"
        self._aggregate_name = "aggregate"
        self._aggregate_action: Any | None = None
        self._aggregate_kind = "aggregate"
        self._aggregate_merge: dict[str, str] = {}
        self._specialists: list[Specialist] = []

    def set_dispatch(
        self,
        name: str = "dispatch",
        action: Any | None = None,
        *,
        kind: str = "control",
    ) -> "SpecialistTeam":
        self._dispatch_name = name
        self._dispatch_action = action
        self._dispatch_kind = kind
        return self

    def set_aggregate(
        self,
        name: str = "aggregate",
        action: Any | None = None,
        *,
        merge: Mapping[str, str] | None = None,
        kind: str = "aggregate",
    ) -> "SpecialistTeam":
        self._aggregate_name = name
        self._aggregate_action = action
        self._aggregate_merge = _normalize_string_mapping(merge, field_name="merge")
        self._aggregate_kind = kind
        return self

    def add_specialist(
        self,
        specialist: Specialist | str,
        graph: StateGraph | CompiledStateGraph | None = None,
        *,
        inputs: Mapping[str, str] | None = None,
        outputs: Mapping[str, str] | None = None,
        namespace: str | None = None,
        propagate_knowledge_graph: bool = False,
        session_mode: str = "persistent",
        session_id_from: str | None = None,
        prepare: Any | None = None,
        prepare_kind: str = "compute",
    ) -> "SpecialistTeam":
        if isinstance(specialist, Specialist):
            spec = Specialist(
                name=specialist.name,
                graph=specialist.graph,
                inputs=_normalize_string_mapping(specialist.inputs, field_name="inputs"),
                outputs=_normalize_string_mapping(specialist.outputs, field_name="outputs"),
                namespace=specialist.namespace,
                propagate_knowledge_graph=bool(specialist.propagate_knowledge_graph),
                session_mode=str(specialist.session_mode).strip().lower(),
                session_id_from=specialist.session_id_from,
                prepare=specialist.prepare,
                prepare_kind=specialist.prepare_kind,
            )
        else:
            if graph is None:
                raise TypeError("graph is required when adding a specialist by name")
            spec = Specialist(
                name=str(specialist),
                graph=graph,
                inputs=_normalize_string_mapping(inputs, field_name="inputs"),
                outputs=_normalize_string_mapping(outputs, field_name="outputs"),
                namespace=namespace,
                propagate_knowledge_graph=bool(propagate_knowledge_graph),
                session_mode=str(session_mode).strip().lower(),
                session_id_from=session_id_from,
                prepare=prepare,
                prepare_kind=prepare_kind,
            )
        self._specialists.append(spec)
        return self

    def build(self) -> StateGraph:
        if not self._specialists:
            raise ValueError("a specialist team requires at least one specialist")

        graph = StateGraph(
            self._state_schema,
            name=self._name,
            worker_count=self._worker_count,
        )
        reserved_names: set[str] = set()

        def reserve(name: str, *, role: str) -> None:
            normalized_name = str(name)
            if not normalized_name:
                raise ValueError(f"{role} names must be non-empty strings")
            if normalized_name in {START, END}:
                raise ValueError(f"{role} name {normalized_name!r} is reserved")
            if normalized_name in reserved_names:
                raise ValueError(f"duplicate {role} name: {normalized_name!r}")
            reserved_names.add(normalized_name)

        reserve(self._dispatch_name, role="dispatch")
        reserve(self._aggregate_name, role="aggregate")
        graph.add_fanout(
            self._dispatch_name,
            self._dispatch_action,
            kind=self._dispatch_kind,
            create_join_scope=True,
        )
        graph.add_join(
            self._aggregate_name,
            self._aggregate_action,
            merge=self._aggregate_merge,
            kind=self._aggregate_kind,
        )
        graph.add_edge(START, self._dispatch_name)

        for spec in self._specialists:
            reserve(spec.name, role="specialist")
            prepare_node_name: str | None = None
            if spec.prepare is not None:
                prepare_node_name = f"{spec.name}__prepare"
                reserve(prepare_node_name, role="specialist prepare")
                graph.add_node(
                    prepare_node_name,
                    spec.prepare,
                    kind=spec.prepare_kind,
                )

            inputs = _normalize_string_mapping(spec.inputs, field_name="inputs")
            outputs = _normalize_string_mapping(spec.outputs, field_name="outputs")
            resolved_session_id = _resolve_session_id_from(
                session_mode=spec.session_mode,
                session_id_from=spec.session_id_from,
                inputs=inputs,
                specialist_name=spec.name,
            )

            graph.add_subgraph(
                spec.name,
                spec.graph,
                namespace=spec.namespace or spec.name,
                inputs=inputs,
                outputs=outputs,
                propagate_knowledge_graph=bool(spec.propagate_knowledge_graph),
                session_mode=spec.session_mode,
                session_id_from=resolved_session_id,
            )

            first_hop = prepare_node_name or spec.name
            graph.add_edge(self._dispatch_name, first_hop)
            if prepare_node_name is not None:
                graph.add_edge(prepare_node_name, spec.name)
            graph.add_edge(spec.name, self._aggregate_name)

        graph.add_edge(self._aggregate_name, END)
        return graph

    def compile(self, *, worker_count: int | None = None) -> CompiledStateGraph:
        graph = self.build()
        return graph.compile(worker_count=worker_count)
