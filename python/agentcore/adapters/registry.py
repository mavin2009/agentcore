from __future__ import annotations

import asyncio
import concurrent.futures
import inspect
import json
from typing import Any

from .. import _agentcore_native as _native
from ..prompts.templates import _coerce_prompt_value_for_model_input


_VALID_DECODE_MODES = {"auto", "bytes", "text", "json"}
_TOOL_RESPONSE_KEYS = frozenset({"ok", "output", "flags", "attempts", "latency_ns"})
_TOOL_CONTROL_RESPONSE_KEYS = _TOOL_RESPONSE_KEYS - {"output"}
_MODEL_RESPONSE_KEYS = frozenset(
    {"ok", "output", "confidence", "token_usage", "flags", "attempts", "latency_ns"}
)
_MODEL_CONTROL_RESPONSE_KEYS = _MODEL_RESPONSE_KEYS - {"output"}


def encode_adapter_payload(value: Any) -> bytes:
    if value is None:
        return b""
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value)
    if isinstance(value, str):
        return value.encode("utf-8")
    try:
        return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
    except TypeError as exc:
        raise TypeError("adapter payloads must be bytes, strings, or JSON-serializable objects") from exc


def _normalize_decode_mode(value: Any, *, field_name: str) -> str:
    normalized = str(value).strip().lower()
    if normalized not in _VALID_DECODE_MODES:
        allowed = ", ".join(sorted(_VALID_DECODE_MODES))
        raise ValueError(f"{field_name} must be one of: {allowed}")
    return normalized


def decode_adapter_payload(payload: bytes, *, decode: str = "auto") -> Any:
    normalized_decode = _normalize_decode_mode(decode, field_name="decode")

    if normalized_decode == "bytes":
        return payload

    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        if normalized_decode in {"text", "json"}:
            raise ValueError("adapter response is not valid UTF-8 text")
        return payload

    if normalized_decode == "text":
        return text

    if normalized_decode == "json":
        return json.loads(text)

    if not text:
        return ""

    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def _decode_response_details(details: dict[str, Any], *, decode: str) -> dict[str, Any]:
    normalized = dict(details)
    raw_output = normalized.get("output", b"")
    if not isinstance(raw_output, (bytes, bytearray, memoryview)):
        raise TypeError("native adapter invocation returned a non-bytes payload")
    normalized["output"] = decode_adapter_payload(bytes(raw_output), decode=decode)
    return normalized


def _raise_adapter_error(kind: str, name: str, details: dict[str, Any]) -> None:
    category = str(details.get("error_category", "unknown"))
    flags = int(details.get("flags", 0))
    raise RuntimeError(f"{kind} {name!r} failed with category={category} flags={flags}")


def _normalize_adapter_name(value: Any, *, kind: str) -> str:
    name = str(value).strip()
    if not name:
        raise ValueError(f"{kind} name must be a non-empty string")
    return name


def _normalize_optional_mapping(value: Any, *, field_name: str) -> dict[str, Any] | None:
    if value is None:
        return None
    if not hasattr(value, "items"):
        raise TypeError(f"{field_name} must be a mapping or None")
    return dict(value.items())


def _merge_metadata(defaults: dict[str, Any], overrides: Any) -> dict[str, Any]:
    if overrides is None:
        return dict(defaults)
    if not hasattr(overrides, "items"):
        raise TypeError("metadata must be a mapping or None")
    merged = dict(defaults)
    for key, value in overrides.items():
        if not isinstance(key, str):
            raise TypeError("metadata keys must be strings")
        merged[key] = value
    return merged


def _positional_arity(function: Any) -> int | None:
    try:
        signature = inspect.signature(function)
    except (TypeError, ValueError):
        return None

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


def _compile_tool_handler_invoker(handler: Any) -> tuple[Any, bool]:
    positional_arity = _positional_arity(handler)
    if positional_arity == 0:
        def invoke(request: Any, metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler())

        return invoke, False

    if positional_arity == 1:
        def invoke(request: Any, metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler(request))

        return invoke, False

    def invoke(request: Any, metadata: dict[str, Any]) -> Any:
        return _resolve_maybe_awaitable(handler(request, metadata))

    return invoke, True


def _compile_model_handler_invoker(handler: Any) -> tuple[Any, bool]:
    positional_arity = _positional_arity(handler)
    if positional_arity == 0:
        def invoke(prompt: Any, schema: Any, metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler())

        return invoke, False

    if positional_arity == 1:
        def invoke(prompt: Any, schema: Any, metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler(prompt))

        return invoke, False

    if positional_arity == 2:
        def invoke(prompt: Any, schema: Any, metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler(prompt, schema))

        return invoke, False

    def invoke(prompt: Any, schema: Any, metadata: dict[str, Any]) -> Any:
        return _resolve_maybe_awaitable(handler(prompt, schema, metadata))

    return invoke, True


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


def _tool_handler_meta(name: str, *, decode_input: str) -> dict[str, Any]:
    return {
        "name": name,
        "kind": "tool",
        "decode_input": decode_input,
    }


def _model_handler_meta(
    name: str,
    *,
    max_tokens: int,
    decode_prompt: str,
    decode_schema: str,
) -> dict[str, Any]:
    return {
        "name": name,
        "kind": "model",
        "max_tokens": int(max_tokens),
        "decode_prompt": decode_prompt,
        "decode_schema": decode_schema,
    }


def _looks_like_response_envelope(value: Any, *, control_keys: frozenset[str]) -> bool:
    if not hasattr(value, "keys"):
        return False
    return any(key in value for key in control_keys)


def _normalize_tool_handler_result(result: Any) -> dict[str, Any]:
    if _looks_like_response_envelope(result, control_keys=_TOOL_CONTROL_RESPONSE_KEYS):
        payload = dict(result.items())
        return {
            "ok": bool(payload.get("ok", True)),
            "output": encode_adapter_payload(payload.get("output")),
            "flags": int(payload.get("flags", 0)),
            "attempts": int(payload.get("attempts", 0)),
            "latency_ns": int(payload.get("latency_ns", 0)),
        }

    return {
        "ok": True,
        "output": encode_adapter_payload(result),
        "flags": 0,
        "attempts": 0,
        "latency_ns": 0,
    }


def _normalize_model_handler_result(result: Any) -> dict[str, Any]:
    if _looks_like_response_envelope(result, control_keys=_MODEL_CONTROL_RESPONSE_KEYS):
        payload = dict(result.items())
        return {
            "ok": bool(payload.get("ok", True)),
            "output": encode_adapter_payload(payload.get("output")),
            "confidence": float(payload.get("confidence", 0.0)),
            "token_usage": int(payload.get("token_usage", 0)),
            "flags": int(payload.get("flags", 0)),
            "attempts": int(payload.get("attempts", 0)),
            "latency_ns": int(payload.get("latency_ns", 0)),
        }

    return {
        "ok": True,
        "output": encode_adapter_payload(result),
        "confidence": 0.0,
        "token_usage": 0,
        "flags": 0,
        "attempts": 0,
        "latency_ns": 0,
    }


def _default_tool_metadata(name: str, *, decode_input: str) -> dict[str, Any]:
    return {
        "provider": "python",
        "implementation": "python_callable_tool",
        "display_name": name,
        "transport": "in_process",
        "auth": "none",
        "capabilities": ["sync", "async", "checkpoint_safe"],
        "request_format": f"python:{decode_input}",
        "response_format": "python:encoded",
    }


def _default_model_metadata(name: str, *, decode_prompt: str, decode_schema: str) -> dict[str, Any]:
    return {
        "provider": "python",
        "implementation": "python_callable_model",
        "display_name": name,
        "transport": "in_process",
        "auth": "none",
        "capabilities": ["sync", "async", "checkpoint_safe"],
        "request_format": f"python:prompt={decode_prompt};schema={decode_schema}",
        "response_format": "python:encoded",
    }


class ToolRegistryView:
    def __init__(self, native_graph: Any):
        self._native_graph = native_graph
        self._mcp_clients: list[Any] = []

    def list(self) -> list[dict[str, Any]]:
        return list(_native._list_tools(self._native_graph))

    def describe(self, name: str) -> dict[str, Any] | None:
        return _native._describe_tool(self._native_graph, str(name))

    def register(
        self,
        name: str,
        handler: Any,
        *,
        policy: dict[str, Any] | None = None,
        metadata: dict[str, Any] | None = None,
        decode_input: str = "auto",
    ) -> None:
        normalized_name = _normalize_adapter_name(name, kind="tool")
        if not callable(handler):
            raise TypeError("tool handler must be callable")
        normalized_decode = _normalize_decode_mode(decode_input, field_name="decode_input")
        normalized_policy = _normalize_optional_mapping(policy, field_name="policy")
        normalized_metadata = _merge_metadata(
            _default_tool_metadata(normalized_name, decode_input=normalized_decode),
            metadata,
        )
        handler_invoker, requires_metadata = _compile_tool_handler_invoker(handler)

        def native_handler(input_bytes: bytes) -> dict[str, Any]:
            request = decode_adapter_payload(input_bytes, decode=normalized_decode)
            handler_metadata = (
                _tool_handler_meta(normalized_name, decode_input=normalized_decode)
                if requires_metadata else
                {}
            )
            result = handler_invoker(request, handler_metadata)
            return _normalize_tool_handler_result(result)

        _native._register_python_tool_adapter(
            self._native_graph,
            normalized_name,
            native_handler,
            normalized_policy,
            normalized_metadata,
        )

    def register_http(
        self,
        name: str = "http_tool",
        *,
        policy: dict[str, Any] | None = None,
        enable_mock_scheme: bool = True,
        enable_file_scheme: bool = True,
    ) -> None:
        _native._register_http_tool_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
            enable_mock_scheme=bool(enable_mock_scheme),
            enable_file_scheme=bool(enable_file_scheme),
        )

    def register_sqlite(
        self,
        name: str = "sqlite_tool",
        *,
        policy: dict[str, Any] | None = None,
    ) -> None:
        _native._register_sqlite_tool_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
        )

    def register_http_json(
        self,
        name: str = "http_json_tool",
        *,
        policy: dict[str, Any] | None = None,
        transport: dict[str, Any] | None = None,
        default_method: str = "POST",
    ) -> None:
        _native._register_http_json_tool_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
            transport=transport,
            default_method=str(default_method),
        )

    def register_mcp_stdio(
        self,
        command: Any,
        *,
        prefix: str | None = None,
        include: Any = None,
        exclude: Any = None,
        env: Any = None,
        cwd: str | None = None,
        startup_timeout: float = 10.0,
        request_timeout: float = 30.0,
        tool_timeout: float | None = None,
        result_mode: str = "auto",
        argument_key: str | None = None,
    ) -> Any:
        from ..mcp import StdioMCPClient

        normalized_prefix = None if prefix is None else str(prefix).strip()
        normalized_result_mode = str(result_mode).strip().lower()
        if normalized_result_mode not in {"auto", "raw"}:
            raise ValueError("result_mode must be 'auto' or 'raw'")
        normalized_argument_key = None if argument_key is None else str(argument_key).strip()
        if normalized_argument_key == "":
            raise ValueError("argument_key must be None or a non-empty string")

        include_names = None if include is None else {str(value) for value in include}
        exclude_names = set() if exclude is None else {str(value) for value in exclude}
        normalized_env = None if env is None else {str(key): str(value) for key, value in dict(env).items()}

        client = StdioMCPClient(
            command,
            cwd=cwd,
            env=normalized_env,
            startup_timeout=float(startup_timeout),
            request_timeout=float(request_timeout),
        ).start()

        try:
            descriptors = client.list_tools()
            for descriptor in descriptors:
                remote_name = str(descriptor["name"])
                if include_names is not None and remote_name not in include_names:
                    continue
                if remote_name in exclude_names:
                    continue

                local_name = remote_name if normalized_prefix is None else f"{normalized_prefix}_{remote_name}"
                if self.describe(local_name) is not None:
                    raise ValueError(f"tool name collision while registering MCP tool {local_name!r}")

                display_name = str(descriptor.get("title") or remote_name)
                description = str(descriptor.get("description") or "")
                metadata = {
                    "provider": "mcp",
                    "implementation": "mcp_stdio_bridge",
                    "display_name": display_name,
                    "transport": "stdio",
                    "auth": "none",
                    "capabilities": [
                        "sync",
                        "async",
                        "structured_request",
                        "structured_response",
                    ],
                    "request_format": "json",
                    "response_format": f"mcp:{normalized_result_mode}",
                    "description": description,
                    "upstream_tool_name": remote_name,
                    "server_name": str(client.server_info.get("name", "")),
                    "server_version": str(client.server_info.get("version", "")),
                }

                def native_handler(
                    request: Any,
                    handler_metadata: dict[str, Any],
                    *,
                    bound_remote_name: str = remote_name,
                    bound_client: Any = client,
                    bound_timeout: float | None = tool_timeout,
                    bound_result_mode: str = normalized_result_mode,
                    bound_argument_key: str | None = normalized_argument_key,
                ) -> Any:
                    if request is None:
                        arguments: dict[str, Any] = {}
                    elif hasattr(request, "items"):
                        arguments = dict(request.items())
                    elif bound_argument_key is not None:
                        arguments = {bound_argument_key: request}
                    else:
                        raise TypeError(
                            "MCP tool requests must be mappings unless argument_key is provided"
                        )

                    if bound_result_mode == "raw":
                        return bound_client.call_tool_raw(
                            bound_remote_name,
                            arguments,
                            timeout=bound_timeout,
                        )
                    return bound_client.call_tool(
                        bound_remote_name,
                        arguments,
                        timeout=bound_timeout,
                        result_mode=bound_result_mode,
                    )

                self.register(
                    local_name,
                    native_handler,
                    decode_input="json",
                    metadata=_merge_metadata(metadata, None),
                )
        except Exception:
            client.close()
            raise

        self._mcp_clients.append(client)
        return client

    def invoke_with_metadata(
        self,
        name: str,
        request: Any,
        *,
        decode: str = "auto",
    ) -> dict[str, Any]:
        details = _native._invoke_tool_with_details(
            self._native_graph,
            str(name),
            encode_adapter_payload(request),
        )
        return _decode_response_details(details, decode=decode)

    def invoke(
        self,
        name: str,
        request: Any,
        *,
        decode: str = "auto",
    ) -> Any:
        details = self.invoke_with_metadata(name, request, decode=decode)
        if not details.get("ok", False):
            _raise_adapter_error("tool", str(name), details)
        return details["output"]

    def __del__(self) -> None:
        for client in self._mcp_clients:
            try:
                client.close()
            except Exception:
                pass


class ModelRegistryView:
    def __init__(self, native_graph: Any):
        self._native_graph = native_graph

    def list(self) -> list[dict[str, Any]]:
        return list(_native._list_models(self._native_graph))

    def describe(self, name: str) -> dict[str, Any] | None:
        return _native._describe_model(self._native_graph, str(name))

    def register(
        self,
        name: str,
        handler: Any,
        *,
        policy: dict[str, Any] | None = None,
        metadata: dict[str, Any] | None = None,
        decode_prompt: str = "auto",
        decode_schema: str = "auto",
    ) -> None:
        normalized_name = _normalize_adapter_name(name, kind="model")
        if not callable(handler):
            raise TypeError("model handler must be callable")
        normalized_prompt_decode = _normalize_decode_mode(
            decode_prompt,
            field_name="decode_prompt",
        )
        normalized_schema_decode = _normalize_decode_mode(
            decode_schema,
            field_name="decode_schema",
        )
        normalized_policy = _normalize_optional_mapping(policy, field_name="policy")
        normalized_metadata = _merge_metadata(
            _default_model_metadata(
                normalized_name,
                decode_prompt=normalized_prompt_decode,
                decode_schema=normalized_schema_decode,
            ),
            metadata,
        )
        handler_invoker, requires_metadata = _compile_model_handler_invoker(handler)

        def native_handler(prompt_bytes: bytes, schema_bytes: bytes, max_tokens: int) -> dict[str, Any]:
            prompt = decode_adapter_payload(prompt_bytes, decode=normalized_prompt_decode)
            schema = decode_adapter_payload(schema_bytes, decode=normalized_schema_decode)
            handler_metadata = (
                _model_handler_meta(
                    normalized_name,
                    max_tokens=max_tokens,
                    decode_prompt=normalized_prompt_decode,
                    decode_schema=normalized_schema_decode,
                )
                if requires_metadata else
                {}
            )
            result = handler_invoker(prompt, schema, handler_metadata)
            return _normalize_model_handler_result(result)

        _native._register_python_model_adapter(
            self._native_graph,
            normalized_name,
            native_handler,
            normalized_policy,
            normalized_metadata,
        )

    def register_local(
        self,
        name: str = "local_model",
        *,
        policy: dict[str, Any] | None = None,
        default_max_tokens: int = 256,
    ) -> None:
        _native._register_local_model_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
            default_max_tokens=int(default_max_tokens),
        )

    def register_llm_http(
        self,
        name: str = "llm_http",
        *,
        policy: dict[str, Any] | None = None,
        enable_mock_transport: bool = True,
    ) -> None:
        _native._register_llm_http_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
            enable_mock_transport=bool(enable_mock_transport),
        )

    def register_openai_chat(
        self,
        name: str = "openai_chat",
        *,
        policy: dict[str, Any] | None = None,
        transport: dict[str, Any] | None = None,
        provider_model_name: str = "",
        endpoint_path: str = "/chat/completions",
        system_prompt: str = "",
        include_json_schema: bool = True,
    ) -> None:
        _native._register_openai_chat_model_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
            transport=transport,
            provider_model_name=str(provider_model_name),
            endpoint_path=str(endpoint_path),
            system_prompt=str(system_prompt),
            include_json_schema=bool(include_json_schema),
        )

    def register_grok_chat(
        self,
        name: str = "grok_chat",
        *,
        policy: dict[str, Any] | None = None,
        transport: dict[str, Any] | None = None,
        provider_model_name: str = "",
        endpoint_path: str = "/chat/completions",
        system_prompt: str = "",
        include_json_schema: bool = True,
    ) -> None:
        _native._register_grok_chat_model_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
            transport=transport,
            provider_model_name=str(provider_model_name),
            endpoint_path=str(endpoint_path),
            system_prompt=str(system_prompt),
            include_json_schema=bool(include_json_schema),
        )

    def register_gemini_generate_content(
        self,
        name: str = "gemini",
        *,
        policy: dict[str, Any] | None = None,
        transport: dict[str, Any] | None = None,
        provider_model_name: str = "",
        endpoint_path: str = "",
        system_prompt: str = "",
    ) -> None:
        _native._register_gemini_generate_content_model_adapter(
            self._native_graph,
            name=str(name),
            policy=policy,
            transport=transport,
            provider_model_name=str(provider_model_name),
            endpoint_path=str(endpoint_path),
            system_prompt=str(system_prompt),
        )

    def invoke_with_metadata(
        self,
        name: str,
        prompt: Any,
        *,
        schema: Any | None = None,
        max_tokens: int = 0,
        decode: str = "auto",
    ) -> dict[str, Any]:
        normalized_prompt = _coerce_prompt_value_for_model_input(prompt)
        normalized_schema = _coerce_prompt_value_for_model_input(schema)
        details = _native._invoke_model_with_details(
            self._native_graph,
            str(name),
            encode_adapter_payload(normalized_prompt),
            encode_adapter_payload(normalized_schema),
            int(max_tokens),
        )
        return _decode_response_details(details, decode=decode)

    def invoke(
        self,
        name: str,
        prompt: Any,
        *,
        schema: Any | None = None,
        max_tokens: int = 0,
        decode: str = "auto",
    ) -> Any:
        details = self.invoke_with_metadata(
            name,
            prompt,
            schema=schema,
            max_tokens=max_tokens,
            decode=decode,
        )
        if not details.get("ok", False):
            _raise_adapter_error("model", str(name), details)
        return details["output"]
