from __future__ import annotations

import json
import os
import shlex
import subprocess
import threading
from collections import deque
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Any

from ..adapters.registry import _positional_arity, _resolve_maybe_awaitable
from ..prompts.templates import RenderedMCPPrompt
from .protocol import (
    JSONRPC_VERSION,
    MCP_PROTOCOL_VERSION,
    completion_values,
    normalize_elicitation_result,
    normalize_log_level,
    normalize_mcp_resource_result,
    normalize_mcp_roots,
    normalize_mcp_tool_result,
    normalize_sampling_request,
    normalize_sampling_result,
)


class MCPTransportError(RuntimeError):
    pass


class MCPProtocolError(RuntimeError):
    pass


@dataclass
class _PendingRequest:
    event: threading.Event
    result: Any = None
    error: Any = None


def _normalize_command(command: str | Sequence[str]) -> list[str]:
    if isinstance(command, str):
        parsed = shlex.split(command)
    else:
        parsed = [str(part) for part in command]
    if not parsed:
        raise ValueError("MCP command must not be empty")
    return parsed


def _normalize_arguments(arguments: Mapping[str, Any] | None) -> dict[str, Any]:
    if arguments is None:
        return {}
    if not isinstance(arguments, Mapping):
        raise TypeError("MCP tool arguments must be a mapping or None")
    return dict(arguments.items())


def _normalize_prompt_arguments(arguments: Mapping[str, Any] | None) -> dict[str, str]:
    if arguments is None:
        return {}
    if not isinstance(arguments, Mapping):
        raise TypeError("MCP prompt arguments must be a mapping or None")
    return {str(key): str(value) for key, value in arguments.items()}


def _normalize_list_result(
    result: Any,
    *,
    item_key: str,
    label: str,
) -> list[dict[str, Any]]:
    if not isinstance(result, Mapping):
        raise MCPProtocolError(f"{label} result must be a mapping")
    batch = result.get(item_key, [])
    if not isinstance(batch, Sequence):
        raise MCPProtocolError(f"{label} {item_key} must be a sequence")
    items: list[dict[str, Any]] = []
    for item in batch:
        if not isinstance(item, Mapping):
            raise MCPProtocolError(f"{label} entries must be mappings")
        items.append(dict(item.items()))
    return items


def _invoke_optional_handler(handler: Any, request: Any, owner: Any) -> Any:
    positional_arity = _positional_arity(handler)
    if positional_arity == 0:
        return _resolve_maybe_awaitable(handler())
    if positional_arity == 1:
        return _resolve_maybe_awaitable(handler(request))
    return _resolve_maybe_awaitable(handler(request, owner))


class StdioMCPClient:
    def __init__(
        self,
        command: str | Sequence[str],
        *,
        cwd: str | None = None,
        env: Mapping[str, str] | None = None,
        startup_timeout: float = 10.0,
        request_timeout: float = 30.0,
        client_name: str = "agentcore",
        client_version: str = "0.1",
        roots: Sequence[Mapping[str, Any] | str] | Mapping[str, Any] | str | Any | None = None,
        roots_list_changed: bool = True,
        sampling_handler: Any | None = None,
        elicitation_handler: Any | None = None,
        log_handler: Any | None = None,
        notification_handler: Any | None = None,
    ):
        self._command = _normalize_command(command)
        self._cwd = None if cwd is None else str(cwd)
        self._env = None if env is None else {str(key): str(value) for key, value in env.items()}
        self._startup_timeout = float(startup_timeout)
        self._request_timeout = float(request_timeout)
        self._client_name = str(client_name)
        self._client_version = str(client_version)
        self._roots_source = roots
        self._roots_list_changed = bool(roots_list_changed)
        self._sampling_handler = sampling_handler
        self._elicitation_handler = elicitation_handler
        self._log_handler = log_handler
        self._notification_handler = notification_handler

        self._lock = threading.Lock()
        self._send_lock = threading.Lock()
        self._pending: dict[int, _PendingRequest] = {}
        self._stderr_lines: deque[str] = deque(maxlen=200)
        self._notifications: deque[dict[str, Any]] = deque(maxlen=200)
        self._logs: deque[dict[str, Any]] = deque(maxlen=200)
        self._resource_updates: deque[str] = deque(maxlen=200)
        self._list_change_counts = {
            "tools": 0,
            "prompts": 0,
            "resources": 0,
            "roots": 0,
        }
        self._resource_subscriptions: set[str] = set()
        self._next_request_id = 1
        self._started = False
        self._closed = False

        self._process: subprocess.Popen[str] | None = None
        self._reader_thread: threading.Thread | None = None
        self._stderr_thread: threading.Thread | None = None

        self._server_info: dict[str, Any] = {}
        self._server_capabilities: dict[str, Any] = {}
        self._protocol_version = ""

    @property
    def command(self) -> tuple[str, ...]:
        return tuple(self._command)

    @property
    def server_info(self) -> dict[str, Any]:
        return dict(self._server_info)

    @property
    def server_capabilities(self) -> dict[str, Any]:
        return dict(self._server_capabilities)

    @property
    def protocol_version(self) -> str:
        return self._protocol_version

    @property
    def list_change_counts(self) -> dict[str, int]:
        return dict(self._list_change_counts)

    @property
    def resource_subscriptions(self) -> tuple[str, ...]:
        return tuple(sorted(self._resource_subscriptions))

    def start(self) -> "StdioMCPClient":
        if self._closed:
            raise RuntimeError("MCP client has already been closed")
        if self._started:
            return self

        inherited_env = None
        if self._env is not None:
            inherited_env = dict(os.environ)
            inherited_env.update(self._env)

        try:
            process = subprocess.Popen(
                self._command,
                cwd=self._cwd,
                env=inherited_env,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                encoding="utf-8",
                bufsize=1,
            )
            self._process = process

            self._reader_thread = threading.Thread(
                target=self._reader_loop,
                name="agentcore-mcp-stdout",
                daemon=True,
            )
            self._reader_thread.start()
            self._stderr_thread = threading.Thread(
                target=self._stderr_loop,
                name="agentcore-mcp-stderr",
                daemon=True,
            )
            self._stderr_thread.start()

            initialize_result = self._request(
                "initialize",
                {
                    "protocolVersion": MCP_PROTOCOL_VERSION,
                    "capabilities": self._client_capabilities(),
                    "clientInfo": {
                        "name": self._client_name,
                        "version": self._client_version,
                    },
                },
                timeout=self._startup_timeout,
            )
            if not isinstance(initialize_result, Mapping):
                raise MCPProtocolError("MCP initialize result must be a mapping")
            self._protocol_version = str(initialize_result.get("protocolVersion", ""))
            self._server_info = dict(initialize_result.get("serverInfo", {}) or {})
            self._server_capabilities = dict(initialize_result.get("capabilities", {}) or {})
            self._notify("notifications/initialized", {})
            self._started = True
            return self
        except Exception:
            self.close()
            raise

    def list_tools(self) -> list[dict[str, Any]]:
        self.start()
        tools: list[dict[str, Any]] = []
        cursor: str | None = None
        while True:
            params: dict[str, Any] = {}
            if cursor is not None:
                params["cursor"] = cursor
            result = self._request("tools/list", params, timeout=self._request_timeout)
            tools.extend(_normalize_list_result(result, item_key="tools", label="tools/list"))
            next_cursor = result.get("nextCursor")
            if next_cursor is None:
                break
            cursor = str(next_cursor)
        return tools

    def list_prompts(self) -> list[dict[str, Any]]:
        self.start()
        prompts: list[dict[str, Any]] = []
        cursor: str | None = None
        while True:
            params: dict[str, Any] = {}
            if cursor is not None:
                params["cursor"] = cursor
            result = self._request("prompts/list", params, timeout=self._request_timeout)
            prompts.extend(_normalize_list_result(result, item_key="prompts", label="prompts/list"))
            next_cursor = result.get("nextCursor")
            if next_cursor is None:
                break
            cursor = str(next_cursor)
        return prompts

    def get_prompt_raw(
        self,
        name: str,
        arguments: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        self.start()
        result = self._request(
            "prompts/get",
            {
                "name": str(name),
                "arguments": _normalize_prompt_arguments(arguments),
            },
            timeout=self._request_timeout if timeout is None else float(timeout),
        )
        if not isinstance(result, Mapping):
            raise MCPProtocolError("prompts/get result must be a mapping")
        messages = result.get("messages", [])
        if not isinstance(messages, Sequence):
            raise MCPProtocolError("prompts/get messages must be a sequence")
        normalized_messages: list[dict[str, Any]] = []
        for item in messages:
            if not isinstance(item, Mapping):
                raise MCPProtocolError("prompt messages must be mappings")
            normalized_messages.append(dict(item.items()))
        payload = dict(result.items())
        payload["messages"] = normalized_messages
        return payload

    def get_prompt(
        self,
        name: str,
        arguments: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> RenderedMCPPrompt:
        result = self.get_prompt_raw(name, arguments, timeout=timeout)
        return RenderedMCPPrompt(
            tuple(result.get("messages", [])),
            name=None if result.get("name") is None else str(result.get("name")),
            description=None if result.get("description") is None else str(result.get("description")),
        )

    def list_resources(self) -> list[dict[str, Any]]:
        self.start()
        resources: list[dict[str, Any]] = []
        cursor: str | None = None
        while True:
            params: dict[str, Any] = {}
            if cursor is not None:
                params["cursor"] = cursor
            result = self._request("resources/list", params, timeout=self._request_timeout)
            resources.extend(_normalize_list_result(result, item_key="resources", label="resources/list"))
            next_cursor = result.get("nextCursor")
            if next_cursor is None:
                break
            cursor = str(next_cursor)
        return resources

    def list_resource_templates(self) -> list[dict[str, Any]]:
        self.start()
        templates: list[dict[str, Any]] = []
        cursor: str | None = None
        while True:
            params: dict[str, Any] = {}
            if cursor is not None:
                params["cursor"] = cursor
            result = self._request("resources/templates/list", params, timeout=self._request_timeout)
            templates.extend(
                _normalize_list_result(result, item_key="resourceTemplates", label="resources/templates/list")
            )
            next_cursor = result.get("nextCursor")
            if next_cursor is None:
                break
            cursor = str(next_cursor)
        return templates

    def read_resource_raw(
        self,
        uri: str,
        *,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        self.start()
        result = self._request(
            "resources/read",
            {"uri": str(uri)},
            timeout=self._request_timeout if timeout is None else float(timeout),
        )
        if not isinstance(result, Mapping):
            raise MCPProtocolError("resources/read result must be a mapping")
        payload = dict(result.items())
        contents = payload.get("contents", [])
        if not isinstance(contents, Sequence):
            raise MCPProtocolError("resources/read contents must be a sequence")
        payload["contents"] = [dict(item.items()) if isinstance(item, Mapping) else item for item in contents]
        return payload

    def read_resource(
        self,
        uri: str,
        *,
        timeout: float | None = None,
        decode: str = "auto",
    ) -> Any:
        return normalize_mcp_resource_result(
            self.read_resource_raw(uri, timeout=timeout),
            decode=decode,
        )

    def complete(
        self,
        ref: Mapping[str, Any],
        argument_name: str,
        value: str,
        *,
        arguments: Mapping[str, Any] | None = None,
        timeout: float | None = None,
    ) -> list[str]:
        self.start()
        if not isinstance(ref, Mapping):
            raise TypeError("completion ref must be a mapping")
        result = self._request(
            "completion/complete",
            {
                "ref": dict(ref.items()),
                "argument": {
                    "name": str(argument_name),
                    "value": str(value),
                },
                "context": {
                    "arguments": _normalize_prompt_arguments(arguments),
                },
            },
            timeout=self._request_timeout if timeout is None else float(timeout),
        )
        return completion_values(result)

    def complete_prompt_argument(
        self,
        prompt_name: str,
        argument_name: str,
        value: str,
        *,
        arguments: Mapping[str, Any] | None = None,
        timeout: float | None = None,
    ) -> list[str]:
        return self.complete(
            {"type": "ref/prompt", "name": str(prompt_name)},
            argument_name,
            value,
            arguments=arguments,
            timeout=timeout,
        )

    def complete_resource_argument(
        self,
        uri_template: str,
        argument_name: str,
        value: str,
        *,
        arguments: Mapping[str, Any] | None = None,
        timeout: float | None = None,
    ) -> list[str]:
        return self.complete(
            {"type": "ref/resource", "uri": str(uri_template)},
            argument_name,
            value,
            arguments=arguments,
            timeout=timeout,
        )

    def call_tool_raw(
        self,
        name: str,
        arguments: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        self.start()
        result = self._request(
            "tools/call",
            {
                "name": str(name),
                "arguments": _normalize_arguments(arguments),
            },
            timeout=self._request_timeout if timeout is None else float(timeout),
        )
        if not isinstance(result, Mapping):
            raise MCPProtocolError("tools/call result must be a mapping")
        return dict(result.items())

    def call_tool(
        self,
        name: str,
        arguments: Mapping[str, Any] | None = None,
        *,
        timeout: float | None = None,
        result_mode: str = "auto",
    ) -> Any:
        result = self.call_tool_raw(name, arguments, timeout=timeout)
        return normalize_mcp_tool_result(result, mode=result_mode)

    def list_roots(self) -> list[dict[str, Any]]:
        source = self._roots_source
        if callable(source):
            value = _invoke_optional_handler(source, {}, self)
        else:
            value = source
        return normalize_mcp_roots(value)

    def set_sampling_handler(self, handler: Any | None) -> None:
        self._sampling_handler = handler

    def set_elicitation_handler(self, handler: Any | None) -> None:
        self._elicitation_handler = handler

    def set_log_handler(self, handler: Any | None) -> None:
        self._log_handler = handler

    def set_notification_handler(self, handler: Any | None) -> None:
        self._notification_handler = handler

    def set_roots(
        self,
        roots: Sequence[Mapping[str, Any] | str] | Mapping[str, Any] | str | Any | None,
        *,
        notify: bool = True,
    ) -> None:
        self._roots_source = roots
        if notify and self._started and self._roots_list_changed:
            self._notify("notifications/roots/list_changed", {})

    def subscribe_resource(
        self,
        uri: str,
        *,
        timeout: float | None = None,
    ) -> None:
        self.start()
        self._request(
            "resources/subscribe",
            {"uri": str(uri)},
            timeout=self._request_timeout if timeout is None else float(timeout),
        )
        self._resource_subscriptions.add(str(uri))

    def unsubscribe_resource(
        self,
        uri: str,
        *,
        timeout: float | None = None,
    ) -> None:
        self.start()
        self._request(
            "resources/unsubscribe",
            {"uri": str(uri)},
            timeout=self._request_timeout if timeout is None else float(timeout),
        )
        self._resource_subscriptions.discard(str(uri))

    def set_logging_level(
        self,
        level: str,
        *,
        timeout: float | None = None,
    ) -> None:
        self.start()
        self._request(
            "logging/setLevel",
            {"level": normalize_log_level(level)},
            timeout=self._request_timeout if timeout is None else float(timeout),
        )

    def drain_notifications(self) -> list[dict[str, Any]]:
        notifications = list(self._notifications)
        self._notifications.clear()
        return notifications

    def drain_logs(self) -> list[dict[str, Any]]:
        logs = list(self._logs)
        self._logs.clear()
        return logs

    def drain_resource_updates(self) -> list[str]:
        updates = list(self._resource_updates)
        self._resource_updates.clear()
        return updates

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True

        process = self._process
        self._process = None
        if process is None:
            return

        try:
            if process.stdin is not None and not process.stdin.closed:
                process.stdin.close()
        except OSError:
            pass

        try:
            process.wait(timeout=1.0)
        except subprocess.TimeoutExpired:
            process.terminate()
            try:
                process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2.0)

        error = MCPTransportError("MCP client closed")
        with self._lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for request in pending:
            request.error = error
            request.event.set()

    def __enter__(self) -> "StdioMCPClient":
        return self.start()

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _client_capabilities(self) -> dict[str, Any]:
        capabilities: dict[str, Any] = {}
        if self._roots_source is not None:
            capabilities["roots"] = {"listChanged": self._roots_list_changed}
        if self._sampling_handler is not None:
            capabilities["sampling"] = {}
        if self._elicitation_handler is not None:
            capabilities["elicitation"] = {}
        return capabilities

    def _notify(self, method: str, params: Mapping[str, Any] | None) -> None:
        self._send_message({
            "jsonrpc": JSONRPC_VERSION,
            "method": str(method),
            "params": {} if params is None else dict(params.items()),
        })

    def _request(self, method: str, params: Mapping[str, Any] | None, *, timeout: float) -> Any:
        process = self._require_process()
        if process.poll() is not None:
            raise MCPTransportError(self._dead_process_message())

        pending = _PendingRequest(event=threading.Event())
        with self._lock:
            request_id = self._next_request_id
            self._next_request_id += 1
            self._pending[request_id] = pending

        self._send_message({
            "jsonrpc": JSONRPC_VERSION,
            "id": request_id,
            "method": str(method),
            "params": {} if params is None else dict(params.items()),
        })

        if not pending.event.wait(timeout):
            with self._lock:
                self._pending.pop(request_id, None)
            raise TimeoutError(f"MCP request {method!r} timed out after {timeout:.2f}s")

        if pending.error is not None:
            error = pending.error
            if isinstance(error, Mapping):
                code = error.get("code")
                message = error.get("message", "unknown MCP error")
                raise MCPProtocolError(f"MCP request {method!r} failed with code={code}: {message}")
            raise error
        return pending.result

    def _send_message(self, payload: Mapping[str, Any]) -> None:
        process = self._require_process()
        if process.stdin is None:
            raise MCPTransportError("MCP client stdin is not available")
        serialized = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
        if "\n" in serialized or "\r" in serialized:
            raise ValueError("serialized MCP payload unexpectedly contains a newline")
        with self._send_lock:
            try:
                process.stdin.write(serialized)
                process.stdin.write("\n")
                process.stdin.flush()
            except OSError as exc:
                raise MCPTransportError(self._dead_process_message()) from exc

    def _reader_loop(self) -> None:
        process = self._process
        if process is None or process.stdout is None:
            return

        try:
            for raw_line in process.stdout:
                line = raw_line.strip()
                if not line:
                    continue
                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    continue
                self._handle_message(message)
        finally:
            self._fail_pending(MCPTransportError(self._dead_process_message()))

    def _stderr_loop(self) -> None:
        process = self._process
        if process is None or process.stderr is None:
            return
        for raw_line in process.stderr:
            line = raw_line.rstrip()
            if line:
                self._stderr_lines.append(line)

    def _handle_message(self, message: Any) -> None:
        if not isinstance(message, Mapping):
            return

        if "id" in message and ("result" in message or "error" in message):
            try:
                request_id = int(message["id"])
            except (TypeError, ValueError):
                return
            with self._lock:
                pending = self._pending.pop(request_id, None)
            if pending is None:
                return
            if "error" in message:
                pending.error = message.get("error")
            else:
                pending.result = message.get("result")
            pending.event.set()
            return

        if "method" in message and "id" in message:
            self._handle_request(message)
            return

        if "method" in message:
            self._handle_notification(message)

    def _handle_request(self, message: Mapping[str, Any]) -> None:
        method = str(message.get("method", ""))
        request_id = message.get("id")
        params = message.get("params", {})
        if not isinstance(params, Mapping):
            self._send_message({
                "jsonrpc": JSONRPC_VERSION,
                "id": request_id,
                "error": {
                    "code": -32602,
                    "message": "request params must be a mapping",
                },
            })
            return

        try:
            if method == "roots/list":
                if self._roots_source is None:
                    raise MCPProtocolError("roots capability is not configured")
                result = {"roots": self.list_roots()}
            elif method == "sampling/createMessage":
                if self._sampling_handler is None:
                    raise MCPProtocolError("sampling capability is not configured")
                request = normalize_sampling_request(params)
                result = normalize_sampling_result(_invoke_optional_handler(self._sampling_handler, request, self))
            elif method == "elicitation/create":
                if self._elicitation_handler is None:
                    raise MCPProtocolError("elicitation capability is not configured")
                request = dict(params.items())
                result = normalize_elicitation_result(_invoke_optional_handler(self._elicitation_handler, request, self))
            else:
                raise KeyError(method)
            self._send_message({
                "jsonrpc": JSONRPC_VERSION,
                "id": request_id,
                "result": result,
            })
        except KeyError:
            self._send_message({
                "jsonrpc": JSONRPC_VERSION,
                "id": request_id,
                "error": {
                    "code": -32601,
                    "message": f"unknown method: {method}",
                },
            })
        except Exception as exc:
            self._send_message({
                "jsonrpc": JSONRPC_VERSION,
                "id": request_id,
                "error": {
                    "code": -32603,
                    "message": str(exc),
                },
            })

    def _handle_notification(self, message: Mapping[str, Any]) -> None:
        payload = dict(message.items())
        self._notifications.append(payload)
        method = str(payload.get("method", ""))
        params = payload.get("params", {})
        if not isinstance(params, Mapping):
            params = {}

        if method == "notifications/message":
            entry = {
                "level": normalize_log_level(params.get("level", "info")),
                "data": params.get("data"),
            }
            logger = params.get("logger")
            if logger is not None:
                entry["logger"] = str(logger)
            self._logs.append(entry)
            if self._log_handler is not None:
                _invoke_optional_handler(self._log_handler, entry, self)
        elif method == "notifications/resources/updated":
            uri = params.get("uri")
            if uri is not None:
                self._resource_updates.append(str(uri))
        elif method == "notifications/tools/list_changed":
            self._list_change_counts["tools"] += 1
        elif method == "notifications/prompts/list_changed":
            self._list_change_counts["prompts"] += 1
        elif method == "notifications/resources/list_changed":
            self._list_change_counts["resources"] += 1
        elif method == "notifications/roots/list_changed":
            self._list_change_counts["roots"] += 1

        if self._notification_handler is not None:
            _invoke_optional_handler(self._notification_handler, payload, self)

    def _fail_pending(self, error: Exception) -> None:
        with self._lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for request in pending:
            request.error = error
            request.event.set()

    def _require_process(self) -> subprocess.Popen[str]:
        if self._process is None:
            raise RuntimeError("MCP client has not been started")
        return self._process

    def _dead_process_message(self) -> str:
        stderr_tail = " | ".join(self._stderr_lines)
        if stderr_tail:
            return f"MCP server exited or closed the transport: {stderr_tail}"
        return "MCP server exited or closed the transport"
