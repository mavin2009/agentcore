from __future__ import annotations

import asyncio
import concurrent.futures
import inspect
import operator
from collections.abc import Mapping
from dataclasses import dataclass
from typing import Annotated, Any, Iterable, Sequence, TypedDict, get_args, get_origin, get_type_hints

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


def _coerce_message_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return list(value)
    if isinstance(value, tuple):
        return list(value)
    return [value]


def _message_id(value: Any) -> str | None:
    raw_id = value.get("id") if isinstance(value, Mapping) else getattr(value, "id", None)
    if raw_id is None:
        return None
    text = str(raw_id)
    return text if text else None


def add_messages(left: Any = None, right: Any = None) -> list[Any]:
    merged: list[Any] = []
    index_by_id: dict[str, int] = {}
    for message in [*_coerce_message_list(left), *_coerce_message_list(right)]:
        message_id = _message_id(message)
        if message_id is not None and message_id in index_by_id:
            merged[index_by_id[message_id]] = message
            continue
        if message_id is not None:
            index_by_id[message_id] = len(merged)
        merged.append(message)
    return merged


add_messages.__agentcore_reducer__ = "add_messages"  # type: ignore[attr-defined]


class MessagesState(TypedDict, total=False):
    messages: Annotated[list[dict[str, Any]], add_messages]


@dataclass(frozen=True)
class _CompatSubgraphSpec:
    graph: Any
    bindings: dict[str, str]


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


def _is_non_string_sequence(value: Any) -> bool:
    return isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray))


def _infer_node_name(action: Any) -> str:
    if not callable(action):
        raise TypeError("node actions must be callable when no explicit name is provided")
    inferred = getattr(action, "__name__", None)
    if not isinstance(inferred, str) or not inferred or inferred == "<lambda>":
        raise ValueError(
            "callable-only node registration requires a named callable; "
            "pass an explicit node name for lambdas or anonymous callables"
        )
    return inferred


def _infer_subgraph_node_name(graph: Any) -> str:
    inferred = getattr(graph, "name", None)
    if not isinstance(inferred, str) or not inferred:
        inferred = getattr(graph, "_name", None)
    if not isinstance(inferred, str) or not inferred:
        raise ValueError("subgraph-only node registration requires a graph with a stable name")
    return inferred


def _unwrap_schema_annotation(annotation: Any) -> Any:
    while True:
        origin = get_origin(annotation)
        if origin is None:
            return annotation
        origin_name = getattr(origin, "__qualname__", getattr(origin, "__name__", ""))
        if origin_name == "Annotated":
            annotation = get_args(annotation)[0]
            continue
        if origin_name in {"Union", "UnionType"}:
            union_args = [entry for entry in get_args(annotation) if entry is not type(None)]
            if len(union_args) == 1:
                annotation = union_args[0]
                continue
        return annotation


def _schema_reducer_metadata_to_merge_strategy(annotation: Any, metadata: Any) -> str | None:
    base = _unwrap_schema_annotation(annotation)
    base_origin = get_origin(base)
    if metadata is add_messages or getattr(metadata, "__agentcore_reducer__", None) == "add_messages":
        if base is list or base_origin is list:
            return "merge_messages"
        return None
    if metadata in {operator.add, operator.iadd}:
        if base is int:
            return "sum_int64"
        if base is list or base_origin is list:
            return "concat_sequence"
        return None
    if metadata in {operator.or_, operator.ior}:
        if base is bool:
            return "logical_or"
        return None
    if metadata in {operator.and_, operator.iand}:
        if base is bool:
            return "logical_and"
        return None
    if metadata is max and base is int:
        return "max_int64"
    if metadata is min and base is int:
        return "min_int64"
    return None


def _extract_state_schema_hints(state_schema: Any) -> dict[str, Any]:
    if state_schema is None or state_schema is dict:
        return {}
    try:
        hints = get_type_hints(state_schema, include_extras=True)
        if isinstance(hints, dict):
            return dict(hints)
    except Exception:
        pass
    raw_annotations = getattr(state_schema, "__annotations__", None)
    if isinstance(raw_annotations, dict):
        return dict(raw_annotations)
    return {}


def _extract_state_schema_fields(state_schema: Any) -> tuple[str, ...]:
    return tuple(_extract_state_schema_hints(state_schema).keys())


def _infer_schema_merge_rules(state_schema: Any) -> dict[str, str]:
    normalized: dict[str, str] = {}
    for field_name, annotation in _extract_state_schema_hints(state_schema).items():
        origin = get_origin(annotation)
        origin_name = getattr(origin, "__qualname__", getattr(origin, "__name__", ""))
        if origin_name != "Annotated":
            continue
        base, *metadata_items = get_args(annotation)
        for metadata in metadata_items:
            strategy = _schema_reducer_metadata_to_merge_strategy(base, metadata)
            if strategy is not None:
                normalized[field_name] = strategy
                break
    return normalized


def _apply_reducer_strategy_for_route(strategy: str, left: Any, right: Any) -> Any:
    if left is _MISSING or left is None:
        return right
    if strategy == "require_equal":
        if left != right:
            raise ValueError("state reducer require_equal saw conflicting values before routing")
        return right
    if strategy == "require_single_writer":
        raise ValueError("state reducer require_single_writer saw more than one writer before routing")
    if strategy == "last_writer_wins":
        return right
    if strategy == "first_writer_wins":
        return left
    if strategy == "sum_int64":
        return int(left) + int(right)
    if strategy == "max_int64":
        return max(int(left), int(right))
    if strategy == "min_int64":
        return min(int(left), int(right))
    if strategy == "logical_or":
        return bool(left) or bool(right)
    if strategy == "logical_and":
        return bool(left) and bool(right)
    if strategy == "concat_sequence":
        return list(left) + list(right)
    if strategy == "merge_messages":
        return add_messages(left, right)
    return right


def _apply_schema_reducer_route_overlay(
    schema_merge: dict[str, str],
    state: Mapping[str, Any],
    updates: dict[str, Any],
) -> dict[str, Any]:
    if not schema_merge or not updates:
        return updates
    reduced = dict(updates)
    for key, strategy in schema_merge.items():
        if key not in updates:
            continue
        reduced[key] = _apply_reducer_strategy_for_route(
            strategy,
            state.get(key, _MISSING),
            updates[key],
        )
    return reduced


def _infer_shared_subgraph_bindings(parent_schema: Any, graph: Any) -> dict[str, str]:
    child_schema = getattr(graph, "_state_schema", None)
    parent_fields = set(_extract_state_schema_fields(parent_schema))
    child_fields = set(_extract_state_schema_fields(child_schema))
    shared_fields = sorted(parent_fields & child_fields)
    if not shared_fields:
        raise NotImplementedError(
            "direct subgraph nodes require parent and child graphs to declare overlapping schema fields; "
            "switch to agentcore.graph and use add_subgraph(...) for explicit bindings otherwise"
        )
    return {field: field for field in shared_fields}


def _normalize_compile_name(name: Any) -> str | None:
    if name is None:
        return None
    if not isinstance(name, str) or not name:
        raise TypeError("compile name must be a non-empty string or None")
    return name


def _validate_compile_compat_options(
    *,
    checkpointer: Any,
    interrupt_before: Any,
    interrupt_after: Any,
    store: Any,
) -> None:
    unsupported: list[str] = []
    if checkpointer is not None:
        unsupported.append("checkpointer")
    if interrupt_before is not None:
        unsupported.append("interrupt_before")
    if interrupt_after is not None:
        unsupported.append("interrupt_after")
    if store is not None:
        unsupported.append("store")
    if unsupported:
        joined = ", ".join(unsupported)
        raise NotImplementedError(
            f"compile(...) does not currently support {joined}; "
            "switch to agentcore.graph for native wait/resume and persistent-session surfaces"
        )


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
        self._schema_merge = _infer_schema_merge_rules(state_schema)
        self._nodes: dict[str, Any] = {}
        self._edges: list[tuple[str, str]] = []
        self._conditional_edges: dict[str, tuple[Any, dict[str, str]]] = {}
        self._entry_point: str | None = None
        self._fanout_nodes: set[str] = set()
        self._join_scope_nodes: set[str] = set()
        self._join_targets: set[str] = set()
        self._synthetic_router_index = 0

    def add_node(self, name: str | Any, action: Any | None = None) -> "StateGraph":
        if action is None and isinstance(name, (StateGraph, CompiledStateGraph)):
            action = name
            name = _infer_subgraph_node_name(action)
        if action is None and not isinstance(name, str):
            action = name
            name = _infer_node_name(action)
        if not isinstance(name, str) or not name:
            raise TypeError("node name must be a non-empty string")
        if isinstance(action, (StateGraph, CompiledStateGraph)):
            self._nodes[name] = _CompatSubgraphSpec(
                graph=action,
                bindings=_infer_shared_subgraph_bindings(self._state_schema, action),
            )
        else:
            if action is not None and not callable(action):
                raise TypeError("node actions must be callable, subgraphs, or None")
            self._nodes[name] = action
        if self._entry_point is None:
            self._entry_point = name
        return self

    def add_fanout(
        self,
        name: str,
        action: Any | None = None,
        *,
        create_join_scope: bool = True,
    ) -> "StateGraph":
        self.add_node(name, action)
        self._fanout_nodes.add(name)
        if create_join_scope:
            self._join_scope_nodes.add(name)
        return self

    def add_join(self, name: str, action: Any | None = None) -> "StateGraph":
        self.add_node(name, action)
        self._join_targets.add(name)
        return self

    def add_edge(self, source: str | Sequence[str], target: str) -> "StateGraph":
        if _is_non_string_sequence(source):
            normalized_sources = [str(entry) for entry in source]
            if not normalized_sources:
                raise ValueError("source must contain at least one node name")
            self._join_targets.add(str(target))
            for entry in normalized_sources:
                self.add_edge(entry, target)
            return self
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
        if source == START:
            return self.set_conditional_entry_point(path, path_map)
        self._conditional_edges[source] = (path, dict(path_map or {}))
        return self

    def set_entry_point(self, name: str) -> "StateGraph":
        self._entry_point = name
        return self

    def set_finish_point(self, name: str) -> "StateGraph":
        return self.add_edge(name, END)

    def set_conditional_entry_point(
        self,
        path: Any,
        path_map: dict[str, str] | None = None,
    ) -> "StateGraph":
        if not callable(path):
            raise TypeError("conditional entry routing functions must be callable")
        router_name = self._allocate_synthetic_router_name("entry_router")
        self.add_node(router_name, lambda state, config: None)
        self._conditional_edges[router_name] = (path, dict(path_map or {}))
        self._entry_point = router_name
        return self

    def add_sequence(self, nodes: Sequence[Any]) -> "StateGraph":
        if not _is_non_string_sequence(nodes):
            raise TypeError("nodes must be a non-string sequence")
        ordered_names = [self._register_sequence_entry(entry) for entry in nodes]
        if not ordered_names:
            raise ValueError("nodes must contain at least one entry")
        for source, target in zip(ordered_names, ordered_names[1:]):
            self.add_edge(source, target)
        return self

    def compile(
        self,
        *,
        worker_count: int | None = None,
        name: str | None = None,
        debug: bool | None = None,
        checkpointer: Any | None = None,
        interrupt_before: Any | None = None,
        interrupt_after: Any | None = None,
        store: Any | None = None,
    ) -> "CompiledStateGraph":
        if debug is not None and not isinstance(debug, bool):
            raise TypeError("debug must be a bool or None")
        _validate_compile_compat_options(
            checkpointer=checkpointer,
            interrupt_before=interrupt_before,
            interrupt_after=interrupt_after,
            store=store,
        )
        native_graph = _native._create_graph(
            name=self._name if name is None else _normalize_compile_name(name),
            worker_count=self._worker_count if worker_count is None else max(1, int(worker_count)),
        )
        if self._schema_merge:
            _native._set_state_reducers(native_graph, self._schema_merge)
        owned_subgraphs: list[CompiledStateGraph] = []
        owned_subgraph_ids: set[int] = set()

        for node_name in self._nodes:
            node_value = self._nodes[node_name]
            if isinstance(node_value, _CompatSubgraphSpec):
                compiled_subgraph = (
                    node_value.graph
                    if isinstance(node_value.graph, CompiledStateGraph)
                    else node_value.graph.compile()
                )
                if id(compiled_subgraph) not in owned_subgraph_ids:
                    owned_subgraphs.append(compiled_subgraph)
                    owned_subgraph_ids.add(id(compiled_subgraph))
                _native._add_subgraph_node(
                    native_graph,
                    node_name,
                    compiled_subgraph._native_graph,
                    input_bindings=node_value.bindings,
                    output_bindings=node_value.bindings,
                )
            else:
                _native._add_node(
                    native_graph,
                    node_name,
                    self._build_callback(node_name),
                    stop_after=False,
                    allow_fan_out=node_name in self._fanout_nodes,
                    create_join_scope=node_name in self._join_scope_nodes,
                    join_incoming_branches=node_name in self._join_targets,
                    merge_rules=self._schema_merge if node_name in self._join_targets else None,
                )

        if self._entry_point is not None:
            _native._set_entry_point(native_graph, self._entry_point)

        for source, target in self._edges:
            _native._add_edge(native_graph, source, self._map_target_name(target))

        _native._finalize(native_graph)
        return CompiledStateGraph(
            native_graph,
            state_schema=self._state_schema,
            owned_subgraphs=owned_subgraphs,
        )

    def _map_target_name(self, target: str) -> str:
        if target == END:
            return _native._INTERNAL_END_NODE_NAME
        return target

    def _allocate_synthetic_router_name(self, label: str) -> str:
        while True:
            name = f"__agentcore_compat_{label}_{self._synthetic_router_index}"
            self._synthetic_router_index += 1
            if name not in self._nodes:
                return name

    def _register_sequence_entry(self, entry: Any) -> str:
        if isinstance(entry, str):
            if entry not in self._nodes:
                raise KeyError(
                    f"sequence entry {entry!r} is not a registered node; "
                    "pass a callable or a (name, action) pair to register it inline"
                )
            return entry
        if _is_non_string_sequence(entry):
            if len(entry) != 2:
                raise TypeError("sequence node tuples must contain exactly two elements")
            entry_name, entry_action = entry
            if not isinstance(entry_name, str) or not entry_name:
                raise TypeError("sequence node tuples must start with a non-empty string name")
            self.add_node(entry_name, entry_action)
            return entry_name
        if isinstance(entry, (StateGraph, CompiledStateGraph)):
            inferred_name = _infer_subgraph_node_name(entry)
        else:
            inferred_name = _infer_node_name(entry)
        self.add_node(inferred_name, entry)
        return inferred_name

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
            route_updates = (
                _apply_schema_reducer_route_overlay(self._schema_merge, state_view, updates)
                if updates
                else updates
            )
            merged_state = state_view if not route_updates else _OverlayMapping(state_view, route_updates)
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
    def __init__(
        self,
        native_graph: Any,
        *,
        state_schema: Any | None = None,
        owned_subgraphs: Sequence["CompiledStateGraph"] | None = None,
    ):
        self._native_graph = native_graph
        self._state_schema = state_schema
        self._owned_subgraphs = list(owned_subgraphs or [])

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
