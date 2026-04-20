from __future__ import annotations

import asyncio
import concurrent.futures
import inspect
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Any, Iterable, Sequence

class _LazyNative:
    def __getattr__(self, name):
        import agentcore
        return getattr(agentcore._agentcore_native, name)

_native = _LazyNative()

START = "__start__"
END = "__end__"

_MISSING = object()


@dataclass(frozen=True)
class Command:
    update: dict[str, Any] | None = None
    goto: str | None = None
    wait: bool = False


def _normalize_config(config: dict[str, Any] | None) -> dict[str, Any]:
    if config is None:
        return {}
    return dict(config)


def _normalize_update(value: Any) -> dict[str, Any]:
    if value is None:
        return {}
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
        if len(result) == 2:
            return _normalize_update(result[0]), None if result[1] is None else str(result[1]), False
        if len(result) == 3:
            return (
                _normalize_update(result[0]),
                None if result[1] is None else str(result[1]),
                bool(result[2]),
            )
        raise TypeError("tuple/list node results must contain exactly two or three elements")
    return _normalize_update(result), None, False


def _normalize_batch_configs(config: Any, size: int) -> list[dict[str, Any]]:
    if isinstance(config, Sequence) and not isinstance(config, (str, bytes, bytearray, dict)):
        if len(config) != size:
            raise ValueError("batch config sequences must match the number of inputs")
        return [_normalize_config(entry) for entry in config]
    normalized = _normalize_config(config)
    return [dict(normalized) for _ in range(size)]


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


class StateGraph:
    def __init__(self, state_schema: type, name: str | None = None, worker_count: int = 1):
        self._state_schema = state_schema
        self._name = name or getattr(state_schema, "__name__", "agentcore_compat_graph")
        self._worker_count = worker_count
        self._nodes: dict[str, Any] = {}
        self._edges: list[tuple[str, str]] = []
        self._conditional_edges: dict[str, tuple[Any, dict[str, str]]] = {}
        self._entry_point: str | None = None

    def add_node(self, name: str, action: Any) -> "StateGraph":
        self._nodes[name] = action
        if self._entry_point is None:
            self._entry_point = name
        return self

    def add_edge(self, source: str, target: str) -> "StateGraph":
        if source == START:
            self.set_entry_point(target)
            return self
        self._edges.append((source, target))
        return self

    def add_conditional_edges(
        self,
        source: str,
        path: Any,
        path_map: dict[str, str] | None = None,
    ) -> "StateGraph":
        if not callable(path):
            raise TypeError("conditional routing functions must be callable")
        self._conditional_edges[source] = (path, dict(path_map or {}))
        return self

    def set_entry_point(self, name: str) -> "StateGraph":
        self._entry_point = name
        return self

    def compile(self, *, worker_count: int | None = None) -> "CompiledStateGraph":
        native_graph = _native._create_graph(
            name=self._name,
            worker_count=self._worker_count if worker_count is None else max(1, int(worker_count)),
        )

        for node_name in self._nodes:
            _native._add_node(native_graph, node_name, self._build_callback(node_name), stop_after=False)

        if self._entry_point is not None:
            _native._set_entry_point(native_graph, self._entry_point)

        for source, target in self._edges:
            _native._add_edge(native_graph, source, self._map_target_name(target))

        _native._finalize(native_graph)
        return CompiledStateGraph(native_graph)

    def _map_target_name(self, target: str) -> str:
        if target == END:
            return _native._INTERNAL_END_NODE_NAME
        return target

    def _build_callback(self, node_name: str) -> Any:
        action = self._nodes[node_name]
        routing_spec = self._conditional_edges.get(node_name)

        def wrapped(state: dict[str, Any], config: dict[str, Any] | None = None) -> Any:
            state_view = _StateView(state)
            updates: dict[str, Any] = {}

            if action is not None:
                updates, explicit_goto, should_wait = _normalize_result(action(state_view, config))
                if should_wait:
                    return updates, None, True
                if explicit_goto is not None:
                    return updates, self._map_target_name(explicit_goto)

            if routing_spec is None:
                return updates

            route_fn, route_map = routing_spec
            merged_state = state_view if not updates else _OverlayMapping(state_view, updates)
            route_value = route_fn(merged_state, config)
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


class CompiledStateGraph:
    def __init__(self, native_graph: Any):
        self._native_graph = native_graph

    def invoke(self, input_state: dict[str, Any] | None = None, config: dict[str, Any] | None = None) -> dict[str, Any]:
        initial_state = {} if input_state is None else dict(input_state)
        return _native._invoke(
            self._native_graph,
            initial_state,
            _normalize_config(config),
        )

    def invoke_with_metadata(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
    ) -> dict[str, Any]:
        initial_state = {} if input_state is None else dict(input_state)
        return _native._invoke_with_details(
            self._native_graph,
            initial_state,
            _normalize_config(config),
            include_subgraphs=include_subgraphs,
        )

    def stream(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
        stream_mode: str = "events",
    ):
        initial_state = {} if input_state is None else dict(input_state)
        for event in _native._stream(
            self._native_graph,
            initial_state,
            _normalize_config(config),
            include_subgraphs=include_subgraphs,
        ):
            yield event

    async def ainvoke(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        return await asyncio.to_thread(self.invoke, input_state, config=config)

    def batch(
        self,
        inputs: Iterable[dict[str, Any] | None],
        *,
        config: dict[str, Any] | Sequence[dict[str, Any] | None] | None = None,
    ) -> list[dict[str, Any]]:
        normalized_inputs = [None if item is None else dict(item) for item in inputs]
        normalized_configs = _normalize_batch_configs(config, len(normalized_inputs))
        return [
            self.invoke(input_state, config=run_config)
            for input_state, run_config in zip(normalized_inputs, normalized_configs)
        ]

    async def abatch(
        self,
        inputs: Iterable[dict[str, Any] | None],
        *,
        config: dict[str, Any] | Sequence[dict[str, Any] | None] | None = None,
    ) -> list[dict[str, Any]]:
        normalized_inputs = [None if item is None else dict(item) for item in inputs]
        normalized_configs = _normalize_batch_configs(config, len(normalized_inputs))
        tasks = [
            asyncio.create_task(self.ainvoke(input_state, config=run_config))
            for input_state, run_config in zip(normalized_inputs, normalized_configs)
        ]
        return [await task for task in tasks]
