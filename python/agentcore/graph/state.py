from __future__ import annotations

import asyncio
import concurrent.futures
import inspect
from collections.abc import Mapping
from dataclasses import dataclass, field
from typing import Any, Iterable, Sequence

from ..adapters.registry import (
    ModelRegistryView,
    ToolRegistryView,
    _decode_response_details,
    _raise_adapter_error,
    encode_adapter_payload,
)
from .. import _agentcore_native as _native

START = "__start__"
END = "__end__"
_RUNTIME_CONFIG_KEY = getattr(_native, "_INTERNAL_RUNTIME_CONFIG_KEY", "__agentcore_runtime__")

_VALID_NODE_KINDS = {
    "compute",
    "control",
    "tool",
    "model",
    "aggregate",
    "human",
}
_VALID_MERGE_STRATEGIES = {
    "require_equal",
    "require_single_writer",
    "last_writer_wins",
    "first_writer_wins",
    "sum_int64",
    "max_int64",
    "min_int64",
    "logical_or",
    "logical_and",
}

_MISSING = object()


@dataclass(frozen=True)
class Command:
    update: dict[str, Any] | None = None
    goto: str | None = None
    wait: bool = False


@dataclass
class _SubgraphSpec:
    graph: StateGraph | CompiledStateGraph
    namespace: str | None = None
    inputs: dict[str, str] = field(default_factory=dict)
    outputs: dict[str, str] = field(default_factory=dict)
    propagate_knowledge_graph: bool = False
    session_mode: str = "ephemeral"
    session_id_from: str | None = None


@dataclass
class _NodeSpec:
    action: Any | None = None
    kind: str = "compute"
    stop_after: bool = False
    allow_fan_out: bool = False
    create_join_scope: bool = False
    join_incoming_branches: bool = False
    deterministic: bool = False
    read_keys: tuple[str, ...] = ()
    cache_size: int = 16
    merge: dict[str, str] = field(default_factory=dict)
    subgraph: _SubgraphSpec | None = None


def _normalize_config(config: Any) -> dict[str, Any]:
    if config is None:
        return {}
    if isinstance(config, dict):
        return dict(config)
    if hasattr(config, "items"):
        return dict(config.items())
    raise TypeError("config must be a mapping or None")


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
        if len(result) != 2:
            raise TypeError("tuple/list node results must contain exactly two elements")
        return _normalize_update(result[0]), None if result[1] is None else str(result[1]), False
    return _normalize_update(result), None, False


def _positional_arity(function: Any) -> int | None:
    try:
        signature = inspect.signature(function)
    except (TypeError, ValueError):
        return 2

    positional_count = 0
    for parameter in signature.parameters.values():
        if parameter.kind == inspect.Parameter.VAR_POSITIONAL:
            return None
        if parameter.kind in (
            inspect.Parameter.POSITIONAL_ONLY,
            inspect.Parameter.POSITIONAL_OR_KEYWORD,
        ):
            positional_count += 1
    return positional_count


def _compile_callback_invoker(function: Any) -> Any:
    positional_arity = _positional_arity(function)
    if positional_arity is None or positional_arity >= 3:
        def invoke(state: Any, config: Any, runtime: RuntimeContext) -> Any:
            return function(state, config, runtime)

        return invoke

    if positional_arity >= 2:
        def invoke(state: Any, config: Any, runtime: RuntimeContext) -> Any:
            return function(state, config)

        return invoke

    def invoke(state: Any, config: Any, runtime: RuntimeContext) -> Any:
        return function(state)

    return invoke


def _run_awaitable_blocking(awaitable: Any) -> Any:
    try:
        asyncio.get_running_loop()
    except RuntimeError:
        return asyncio.run(awaitable)

    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
        return executor.submit(asyncio.run, awaitable).result()


def _resolve_maybe_awaitable(value: Any) -> Any:
    if inspect.isawaitable(value):
        return _run_awaitable_blocking(value)
    return value


class RuntimeContext:
    def __init__(self, native_runtime: Any | None):
        self._native_runtime = native_runtime

    @property
    def available(self) -> bool:
        return self._native_runtime is not None

    def record_once_with_metadata(self, key: str, request: Any, producer: Any) -> dict[str, Any]:
        if not self.available:
            raise RuntimeError("runtime context is not available for this callback")
        if not isinstance(key, str) or not key:
            raise ValueError("recorded effect key must be a non-empty string")
        if not callable(producer):
            raise TypeError("producer must be callable")
        details = _native._runtime_record_once(
            self._native_runtime,
            key,
            request,
            producer,
        )
        if not isinstance(details, dict):
            raise TypeError("native runtime returned an invalid recorded-effect payload")
        return details

    def record_once(self, key: str, request: Any, producer: Any) -> Any:
        return self.record_once_with_metadata(key, request, producer)["value"]

    def invoke_tool_with_metadata(
        self,
        name: str,
        request: Any,
        *,
        decode: str = "auto",
    ) -> dict[str, Any]:
        if not self.available:
            raise RuntimeError("runtime context is not available for this callback")
        details = _native._runtime_invoke_tool_with_details(
            self._native_runtime,
            str(name),
            encode_adapter_payload(request),
        )
        return _decode_response_details(details, decode=decode)

    def invoke_tool(
        self,
        name: str,
        request: Any,
        *,
        decode: str = "auto",
    ) -> Any:
        details = self.invoke_tool_with_metadata(name, request, decode=decode)
        if not details.get("ok", False):
            _raise_adapter_error("tool", str(name), details)
        return details["output"]

    def invoke_model_with_metadata(
        self,
        name: str,
        prompt: Any,
        *,
        schema: Any | None = None,
        max_tokens: int = 0,
        decode: str = "auto",
    ) -> dict[str, Any]:
        if not self.available:
            raise RuntimeError("runtime context is not available for this callback")
        details = _native._runtime_invoke_model_with_details(
            self._native_runtime,
            str(name),
            encode_adapter_payload(prompt),
            encode_adapter_payload(schema),
            int(max_tokens),
        )
        return _decode_response_details(details, decode=decode)

    def invoke_model(
        self,
        name: str,
        prompt: Any,
        *,
        schema: Any | None = None,
        max_tokens: int = 0,
        decode: str = "auto",
    ) -> Any:
        details = self.invoke_model_with_metadata(
            name,
            prompt,
            schema=schema,
            max_tokens=max_tokens,
            decode=decode,
        )
        if not details.get("ok", False):
            _raise_adapter_error("model", str(name), details)
        return details["output"]


def _extract_runtime_and_config(config: Any) -> tuple[Any, RuntimeContext]:
    runtime_accessor = getattr(config, "_agentcore_runtime_capsule", None)
    if callable(runtime_accessor):
        return config, RuntimeContext(runtime_accessor())
    normalized = _normalize_config(config)
    return normalized, RuntimeContext(normalized.pop(_RUNTIME_CONFIG_KEY, None))


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


def _normalize_batch_configs(config: Any, size: int) -> list[dict[str, Any]]:
    if isinstance(config, Sequence) and not isinstance(config, (str, bytes, bytearray, dict)):
        if len(config) != size:
            raise ValueError("batch config sequences must match the number of inputs")
        return [_normalize_config(entry) for entry in config]
    normalized = _normalize_config(config)
    return [dict(normalized) for _ in range(size)]


def _normalize_node_kind(kind: str, *, allow_subgraph: bool = False) -> str:
    normalized = str(kind).strip().lower()
    valid_kinds = _VALID_NODE_KINDS | ({"subgraph"} if allow_subgraph else set())
    if normalized not in valid_kinds:
        allowed = ", ".join(sorted(valid_kinds))
        raise ValueError(f"node kind must be one of: {allowed}")
    return normalized


def _normalize_string_mapping(value: Any, *, field_name: str) -> dict[str, str]:
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


def _normalize_merge_rules(merge: Any) -> dict[str, str]:
    normalized = _normalize_string_mapping(merge, field_name="merge")
    for field_name, strategy in normalized.items():
        normalized_strategy = strategy.strip().lower()
        if normalized_strategy not in _VALID_MERGE_STRATEGIES:
            allowed = ", ".join(sorted(_VALID_MERGE_STRATEGIES))
            raise ValueError(
                f"merge strategy for {field_name!r} must be one of: {allowed}"
            )
        normalized[field_name] = normalized_strategy
    return normalized


def _normalize_string_sequence(value: Any, *, field_name: str) -> tuple[str, ...]:
    if value is None:
        return ()
    if isinstance(value, str):
        raise TypeError(f"{field_name} must be a sequence of strings, not a single string")

    normalized: list[str] = []
    for entry in value:
        if not isinstance(entry, str):
            raise TypeError(f"{field_name} must contain only strings")
        normalized.append(entry)
    return tuple(normalized)


class StateGraph:
    def __init__(self, state_schema: Any = None, *, name: str | None = None, worker_count: int = 1):
        self._state_schema = state_schema
        self._name = name or getattr(state_schema, "__name__", "agentcore_state_graph")
        self._worker_count = max(1, int(worker_count))
        self._nodes: dict[str, _NodeSpec] = {}
        self._edges: list[tuple[str, str]] = []
        self._conditional_edges: dict[str, tuple[Any, dict[Any, str]]] = {}
        self._entry_point: str | None = None

    def add_node(
        self,
        name: str,
        action: Any | None = None,
        *,
        kind: str = "compute",
        stop_after: bool = False,
        allow_fan_out: bool = False,
        create_join_scope: bool = False,
        join_incoming_branches: bool = False,
        deterministic: bool = False,
        read_keys: Sequence[str] | None = None,
        cache_size: int = 16,
        merge: dict[str, Any] | None = None,
    ) -> "StateGraph":
        if action is not None and not callable(action):
            raise TypeError("node actions must be callable or None")

        normalized_kind = _normalize_node_kind(kind)
        if normalized_kind == "subgraph":
            raise ValueError("use add_subgraph(...) to register subgraph nodes")

        normalized_read_keys = _normalize_string_sequence(read_keys, field_name="read_keys")
        if not deterministic and normalized_read_keys:
            raise ValueError("read_keys require deterministic=True")
        normalized_cache_size = max(1, int(cache_size))

        self._nodes[name] = _NodeSpec(
            action=action,
            kind=normalized_kind,
            stop_after=bool(stop_after),
            allow_fan_out=bool(allow_fan_out),
            create_join_scope=bool(create_join_scope),
            join_incoming_branches=bool(join_incoming_branches),
            deterministic=bool(deterministic),
            read_keys=normalized_read_keys,
            cache_size=normalized_cache_size,
            merge=_normalize_merge_rules(merge),
        )
        if self._entry_point is None:
            self._entry_point = name
        return self

    def add_fanout(
        self,
        name: str,
        action: Any | None = None,
        *,
        kind: str = "control",
        create_join_scope: bool = True,
    ) -> "StateGraph":
        return self.add_node(
            name,
            action,
            kind=kind,
            allow_fan_out=True,
            create_join_scope=create_join_scope,
        )

    def add_join(
        self,
        name: str,
        action: Any | None = None,
        *,
        merge: dict[str, Any] | None = None,
        kind: str = "aggregate",
    ) -> "StateGraph":
        return self.add_node(
            name,
            action,
            kind=kind,
            join_incoming_branches=True,
            merge=merge,
        )

    def add_subgraph(
        self,
        name: str,
        graph: StateGraph | CompiledStateGraph,
        *,
        inputs: dict[str, str] | None = None,
        outputs: dict[str, str] | None = None,
        namespace: str | None = None,
        propagate_knowledge_graph: bool = False,
        session_mode: str = "ephemeral",
        session_id_from: str | None = None,
    ) -> "StateGraph":
        if not isinstance(graph, (StateGraph, CompiledStateGraph)):
            raise TypeError("subgraph must be a StateGraph or CompiledStateGraph instance")
        if graph is self:
            raise ValueError("a graph cannot include itself as a subgraph")
        normalized_session_mode = str(session_mode).strip().lower()
        if normalized_session_mode not in {"ephemeral", "persistent"}:
            raise ValueError("session_mode must be either 'ephemeral' or 'persistent'")
        if normalized_session_mode == "persistent":
            if not isinstance(session_id_from, str) or not session_id_from:
                raise ValueError("persistent subgraphs require session_id_from")
        elif session_id_from is not None:
            raise ValueError("ephemeral subgraphs must not declare session_id_from")

        self._nodes[name] = _NodeSpec(
            action=None,
            kind="subgraph",
            subgraph=_SubgraphSpec(
                graph=graph,
                namespace=namespace,
                inputs=_normalize_string_mapping(inputs, field_name="inputs"),
                outputs=_normalize_string_mapping(outputs, field_name="outputs"),
                propagate_knowledge_graph=bool(propagate_knowledge_graph),
                session_mode=normalized_session_mode,
                session_id_from=session_id_from,
            ),
        )
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
        path_map: dict[Any, str] | None = None,
    ) -> "StateGraph":
        if not callable(path):
            raise TypeError("conditional routing functions must be callable")
        self._conditional_edges[source] = (path, dict(path_map or {}))
        return self

    def set_entry_point(self, name: str) -> "StateGraph":
        self._entry_point = name
        return self

    def compile(self, *, worker_count: int | None = None) -> "CompiledStateGraph":
        return self._compile_internal(
            worker_count=self._worker_count if worker_count is None else max(1, int(worker_count)),
            subgraph_cache={},
            compile_stack=set(),
        )

    def _compile_internal(
        self,
        *,
        worker_count: int,
        subgraph_cache: dict[int, "CompiledStateGraph"],
        compile_stack: set[int],
    ) -> "CompiledStateGraph":
        graph_identity = id(self)
        if graph_identity in compile_stack:
            raise ValueError(f"cyclical subgraph composition detected while compiling {self._name!r}")

        compile_stack.add(graph_identity)
        try:
            native_graph = _native._create_graph(
                name=self._name,
                worker_count=worker_count,
            )
            owned_subgraphs: list[CompiledStateGraph] = []
            owned_subgraph_ids: set[int] = set()

            for node_name, node_spec in self._nodes.items():
                if node_spec.subgraph is not None:
                    if node_name in self._conditional_edges:
                        raise ValueError(
                            "conditional edges are not supported directly on subgraph nodes; "
                            f"add a router node after {node_name!r}"
                        )

                    compiled_subgraph = self._compile_subgraph_reference(
                        node_spec.subgraph.graph,
                        subgraph_cache=subgraph_cache,
                        compile_stack=compile_stack,
                    )
                    if id(compiled_subgraph) not in owned_subgraph_ids:
                        owned_subgraphs.append(compiled_subgraph)
                        owned_subgraph_ids.add(id(compiled_subgraph))

                    _native._add_subgraph_node(
                        native_graph,
                        node_name,
                        compiled_subgraph._native_graph,
                        namespace_name=node_spec.subgraph.namespace,
                        input_bindings=node_spec.subgraph.inputs,
                        output_bindings=node_spec.subgraph.outputs,
                        propagate_knowledge_graph=node_spec.subgraph.propagate_knowledge_graph,
                        session_mode=node_spec.subgraph.session_mode,
                        session_id_from=node_spec.subgraph.session_id_from,
                    )
                    continue

                callback = self._build_callback(node_name) if self._needs_python_callback(node_name) else None
                _native._add_node(
                    native_graph,
                    node_name,
                    callback,
                    kind=node_spec.kind,
                    stop_after=node_spec.stop_after,
                    allow_fan_out=node_spec.allow_fan_out,
                    create_join_scope=node_spec.create_join_scope,
                    join_incoming_branches=node_spec.join_incoming_branches,
                    deterministic=node_spec.deterministic,
                    read_keys=node_spec.read_keys,
                    cache_size=node_spec.cache_size,
                    merge_rules=node_spec.merge,
                )

            if self._entry_point is not None:
                _native._set_entry_point(native_graph, self._entry_point)

            for source, target in self._edges:
                _native._add_edge(native_graph, source, self._map_target_name(target))

            _native._finalize(native_graph)
            return CompiledStateGraph(native_graph, self._name, owned_subgraphs=owned_subgraphs)
        finally:
            compile_stack.remove(graph_identity)

    def _compile_subgraph_reference(
        self,
        graph: StateGraph | "CompiledStateGraph",
        *,
        subgraph_cache: dict[int, "CompiledStateGraph"],
        compile_stack: set[int],
    ) -> "CompiledStateGraph":
        if isinstance(graph, CompiledStateGraph):
            return graph

        cache_key = id(graph)
        cached = subgraph_cache.get(cache_key)
        if cached is not None:
            return cached

        compiled = graph._compile_internal(
            worker_count=graph._worker_count,
            subgraph_cache=subgraph_cache,
            compile_stack=compile_stack,
        )
        subgraph_cache[cache_key] = compiled
        return compiled

    def _needs_python_callback(self, node_name: str) -> bool:
        node_spec = self._nodes[node_name]
        return node_spec.subgraph is None and (
            node_spec.action is not None or node_name in self._conditional_edges
        )

    def _map_target_name(self, target: str) -> str:
        if target == END:
            return _native._INTERNAL_END_NODE_NAME
        return target

    def _build_callback(self, node_name: str):
        node_spec = self._nodes[node_name]
        node_action = node_spec.action
        routing_spec = self._conditional_edges.get(node_name)
        action_invoker = _compile_callback_invoker(node_action) if node_action is not None else None
        route_invoker = None
        route_map = None
        if routing_spec is not None:
            route_fn, route_map = routing_spec
            route_invoker = _compile_callback_invoker(route_fn)

        def wrapped(state: Any, config: Any = None) -> Any:
            normalized_config, runtime = _extract_runtime_and_config(config)
            state_view = _StateView(state)
            updates: dict[str, Any] = {}
            explicit_goto: str | None = None

            if action_invoker is not None:
                node_result = action_invoker(
                    state_view,
                    normalized_config,
                    runtime,
                )
                updates, explicit_goto, should_wait = _normalize_result(
                    _resolve_maybe_awaitable(node_result)
                )
                if should_wait:
                    return updates, None, True
                if explicit_goto is not None:
                    return updates, self._map_target_name(explicit_goto)

            if routing_spec is None:
                return updates

            merged_state = state_view if not updates else _OverlayMapping(state_view, updates)
            route_result = route_invoker(
                merged_state,
                normalized_config,
                runtime,
            )
            route_value = _resolve_maybe_awaitable(route_result)
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
        name: str,
        *,
        owned_subgraphs: Sequence["CompiledStateGraph"] | None = None,
    ):
        self._native_graph = native_graph
        self._name = name
        self._owned_subgraphs = list(owned_subgraphs or [])
        self._tool_registry_view = ToolRegistryView(native_graph)
        self._model_registry_view = ModelRegistryView(native_graph)

    @property
    def name(self) -> str:
        return self._name

    @property
    def tools(self) -> ToolRegistryView:
        return self._tool_registry_view

    @property
    def models(self) -> ModelRegistryView:
        return self._model_registry_view

    def invoke(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        initial_state = {} if input_state is None else dict(input_state)
        return _native._invoke(self._native_graph, initial_state, _normalize_config(config))

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

    def invoke_until_pause_with_metadata(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
    ) -> dict[str, Any]:
        initial_state = {} if input_state is None else dict(input_state)
        return _native._invoke_until_pause_with_details(
            self._native_graph,
            initial_state,
            _normalize_config(config),
            include_subgraphs=include_subgraphs,
        )

    def resume_with_metadata(
        self,
        checkpoint_id: int,
        *,
        include_subgraphs: bool = True,
    ) -> dict[str, Any]:
        return _native._resume_with_details(
            self._native_graph,
            int(checkpoint_id),
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
        if stream_mode != "events":
            raise NotImplementedError("the native state graph layer currently supports only stream_mode='events'")
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

    async def ainvoke_with_metadata(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
    ) -> dict[str, Any]:
        return await asyncio.to_thread(
            self.invoke_with_metadata,
            input_state,
            config=config,
            include_subgraphs=include_subgraphs,
        )

    async def astream(
        self,
        input_state: dict[str, Any] | None = None,
        *,
        config: dict[str, Any] | None = None,
        include_subgraphs: bool = True,
        stream_mode: str = "events",
    ):
        events = await asyncio.to_thread(
            lambda: list(
                self.stream(
                    input_state,
                    config=config,
                    include_subgraphs=include_subgraphs,
                    stream_mode=stream_mode,
                )
            )
        )
        for event in events:
            yield event

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
