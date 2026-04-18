from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Iterable, Mapping

from ..graph import CompiledStateGraph, END, START, StateGraph


@dataclass(frozen=True)
class PipelineStep:
    name: str
    action: Any | None = None
    kind: str = "compute"
    stop_after: bool = False
    allow_fan_out: bool = False
    create_join_scope: bool = False
    join_incoming_branches: bool = False
    merge: Mapping[str, Any] = field(default_factory=dict)


class PipelineGraph:
    def __init__(
        self,
        state_schema: Any = None,
        *,
        name: str | None = None,
        worker_count: int = 1,
    ):
        self._state_schema = state_schema
        self._name = name or getattr(state_schema, "__name__", "agentcore_pipeline")
        self._worker_count = max(1, int(worker_count))
        self._steps: list[PipelineStep] = []

    def add_step(
        self,
        name: str,
        action: Any | None = None,
        *,
        kind: str = "compute",
        stop_after: bool = False,
        allow_fan_out: bool = False,
        create_join_scope: bool = False,
        join_incoming_branches: bool = False,
        merge: Mapping[str, Any] | None = None,
    ) -> "PipelineGraph":
        self._steps.append(
            PipelineStep(
                name=name,
                action=action,
                kind=kind,
                stop_after=bool(stop_after),
                allow_fan_out=bool(allow_fan_out),
                create_join_scope=bool(create_join_scope),
                join_incoming_branches=bool(join_incoming_branches),
                merge=dict(merge or {}),
            )
        )
        return self

    def extend(self, steps: Iterable[PipelineStep]) -> "PipelineGraph":
        for step in steps:
            if not isinstance(step, PipelineStep):
                raise TypeError("extend(...) expects PipelineStep instances")
            self.add_step(
                step.name,
                step.action,
                kind=step.kind,
                stop_after=step.stop_after,
                allow_fan_out=step.allow_fan_out,
                create_join_scope=step.create_join_scope,
                join_incoming_branches=step.join_incoming_branches,
                merge=step.merge,
            )
        return self

    def build(self) -> StateGraph:
        if not self._steps:
            raise ValueError("a pipeline requires at least one step")

        graph = StateGraph(
            self._state_schema,
            name=self._name,
            worker_count=self._worker_count,
        )
        seen_names: set[str] = set()
        previous_name: str | None = None

        for step in self._steps:
            normalized_name = str(step.name)
            if not normalized_name:
                raise ValueError("pipeline step names must be non-empty strings")
            if normalized_name in {START, END}:
                raise ValueError(f"pipeline step name {normalized_name!r} is reserved")
            if normalized_name in seen_names:
                raise ValueError(f"duplicate pipeline step name: {normalized_name!r}")
            seen_names.add(normalized_name)

            graph.add_node(
                normalized_name,
                step.action,
                kind=step.kind,
                stop_after=step.stop_after,
                allow_fan_out=step.allow_fan_out,
                create_join_scope=step.create_join_scope,
                join_incoming_branches=step.join_incoming_branches,
                merge=dict(step.merge),
            )

            if previous_name is None:
                graph.add_edge(START, normalized_name)
            else:
                graph.add_edge(previous_name, normalized_name)
            previous_name = normalized_name

        graph.add_edge(previous_name, END)
        return graph

    def compile(self, *, worker_count: int | None = None) -> CompiledStateGraph:
        graph = self.build()
        return graph.compile(worker_count=worker_count)
