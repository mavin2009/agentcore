from __future__ import annotations

import json
import sys
import threading
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any

from ..adapters.registry import _positional_arity, _resolve_maybe_awaitable
from ..prompts.templates import (
    ChatPromptTemplate,
    PromptTemplate,
    RenderedChatPrompt,
    RenderedMCPPrompt,
    RenderedPrompt,
)
from .protocol import (
    JSONRPC_VERSION,
    MCP_PROTOCOL_VERSION,
    coerce_mcp_prompt_result,
    coerce_mcp_resource_result,
    coerce_mcp_tool_result,
    log_level_enabled,
    match_uri_template,
    normalize_completion_result,
    normalize_elicitation_result,
    normalize_log_level,
    normalize_mcp_roots,
    normalize_prompt_argument,
    normalize_prompt_message,
    normalize_prompt_result_to_text,
    normalize_sampling_request,
    normalize_sampling_result,
    parse_uri_template_variables,
    prompt_descriptor,
    render_uri_template,
    resource_descriptor,
    resource_template_descriptor,
    tool_descriptor,
)


@dataclass
class MCPTool:
    name: str
    handler: Any
    description: str = ""
    input_schema: dict[str, Any] | None = None
    title: str = ""
    annotations: dict[str, Any] | None = None


@dataclass
class MCPPrompt:
    name: str
    handler: Any
    description: str = ""
    arguments: list[dict[str, Any]] = field(default_factory=list)
    title: str = ""
    argument_completions: dict[str, Any] = field(default_factory=dict)


@dataclass
class MCPResource:
    uri: str
    handler: Any
    name: str = ""
    description: str = ""
    mime_type: str = ""
    size: int | None = None
    annotations: dict[str, Any] | None = None


@dataclass
class MCPResourceTemplate:
    uri_template: str
    handler: Any
    name: str = ""
    description: str = ""
    mime_type: str = ""
    annotations: dict[str, Any] | None = None
    argument_completions: dict[str, Any] = field(default_factory=dict)


@dataclass
class _PendingRequest:
    event: threading.Event
    result: Any = None
    error: Any = None


def _compile_tool_handler(handler: Any) -> tuple[Any, bool]:
    positional_arity = _positional_arity(handler)

    if positional_arity == 0:
        def invoke(arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler())

        return invoke, False

    if positional_arity == 1:
        def invoke(arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler(arguments))

        return invoke, False

    def invoke(arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
        return _resolve_maybe_awaitable(handler(arguments, metadata))

    return invoke, True


def _compile_prompt_handler(handler: Any) -> tuple[Any, bool]:
    positional_arity = _positional_arity(handler)

    if positional_arity == 0:
        def invoke(arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler())

        return invoke, False

    if positional_arity == 1:
        def invoke(arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler(arguments))

        return invoke, False

    def invoke(arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
        return _resolve_maybe_awaitable(handler(arguments, metadata))

    return invoke, True


def _compile_resource_handler(handler: Any, *, template: bool) -> tuple[Any, bool]:
    positional_arity = _positional_arity(handler)

    if positional_arity == 0:
        def invoke(uri: str, arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
            return _resolve_maybe_awaitable(handler())

        return invoke, False

    if positional_arity == 1:
        if template:
            def invoke(uri: str, arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
                return _resolve_maybe_awaitable(handler(arguments))
        else:
            def invoke(uri: str, arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
                return _resolve_maybe_awaitable(handler(uri))
        return invoke, False

    if positional_arity == 2:
        if template:
            def invoke(uri: str, arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
                return _resolve_maybe_awaitable(handler(arguments, metadata))
        else:
            def invoke(uri: str, arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
                return _resolve_maybe_awaitable(handler(uri, metadata))
        return invoke, True

    def invoke(uri: str, arguments: dict[str, Any], metadata: dict[str, Any]) -> Any:
        return _resolve_maybe_awaitable(handler(uri, arguments, metadata))

    return invoke, True


def _run_completion_provider(
    provider: Any,
    value: str,
    arguments: dict[str, Any],
    metadata: dict[str, Any],
) -> dict[str, Any]:
    if callable(provider):
        positional_arity = _positional_arity(provider)
        if positional_arity == 0:
            result = _resolve_maybe_awaitable(provider())
        elif positional_arity == 1:
            result = _resolve_maybe_awaitable(provider(value))
        elif positional_arity == 2:
            result = _resolve_maybe_awaitable(provider(value, arguments))
        else:
            result = _resolve_maybe_awaitable(provider(value, arguments, metadata))
    else:
        result = provider
    return normalize_completion_result(result)


def _normalize_prompt_arguments(arguments: Sequence[Mapping[str, Any]] | None) -> list[dict[str, Any]]:
    if arguments is None:
        return []
    return [normalize_prompt_argument(argument) for argument in arguments]


def _normalize_prompt_handler_result(
    value: Any,
    *,
    arguments: dict[str, Any],
    name: str,
    description: str,
) -> dict[str, Any]:
    if isinstance(value, PromptTemplate):
        rendered = value.render(arguments)
        return coerce_mcp_prompt_result(rendered.text, default_name=name, default_description=description)
    if isinstance(value, ChatPromptTemplate):
        rendered = value.render(arguments)
        return {
            "name": name,
            "description": description,
            "messages": [
                normalize_prompt_message(message.as_dict())
                for message in rendered.messages
            ],
        }
    if isinstance(value, RenderedPrompt):
        return coerce_mcp_prompt_result(value.text, default_name=name, default_description=description)
    if isinstance(value, RenderedChatPrompt):
        return {
            "name": name,
            "description": description,
            "messages": [
                normalize_prompt_message(message.as_dict())
                for message in value.messages
            ],
        }
    if isinstance(value, RenderedMCPPrompt):
        return {
            "name": value.name or name,
            "description": value.description or description,
            "messages": value.as_messages(),
        }
    return coerce_mcp_prompt_result(value, default_name=name, default_description=description)


class MCPServerSession:
    def __init__(
        self,
        server: "MCPServer",
        *,
        request_fn: Any,
        notify_fn: Any,
        client_capabilities: Mapping[str, Any] | None,
        client_info: Mapping[str, Any] | None,
        default_timeout: float,
    ):
        self._server = server
        self._request_fn = request_fn
        self._notify_fn = notify_fn
        self._client_capabilities = dict(client_capabilities.items()) if isinstance(client_capabilities, Mapping) else {}
        self._client_info = dict(client_info.items()) if isinstance(client_info, Mapping) else {}
        self._default_timeout = float(default_timeout)
        self._initialized = False
        self._logging_level: str | None = None
        self._resource_subscriptions: set[str] = set()
        self._subscription_lock = threading.Lock()
        self._roots_change_count = 0
        self._cancel_notifications: list[dict[str, Any]] = []

    @property
    def client_capabilities(self) -> dict[str, Any]:
        return dict(self._client_capabilities)

    @property
    def client_info(self) -> dict[str, Any]:
        return dict(self._client_info)

    @property
    def logging_level(self) -> str | None:
        return self._logging_level

    @property
    def initialized(self) -> bool:
        return self._initialized

    @property
    def roots_change_count(self) -> int:
        return self._roots_change_count

    @property
    def subscribed_resources(self) -> tuple[str, ...]:
        with self._subscription_lock:
            return tuple(sorted(self._resource_subscriptions))

    def list_roots(self, *, timeout: float | None = None) -> list[dict[str, Any]]:
        self._require_client_capability("roots")
        result = self._request("roots/list", {}, timeout=timeout)
        if not isinstance(result, Mapping):
            raise RuntimeError("roots/list result must be a mapping")
        return normalize_mcp_roots(result.get("roots", []))

    def sample(
        self,
        messages: Sequence[Mapping[str, Any]],
        *,
        model_preferences: Mapping[str, Any] | None = None,
        system_prompt: str | None = None,
        include_context: str | None = None,
        temperature: float | None = None,
        max_tokens: int | None = None,
        stop_sequences: Sequence[str] | None = None,
        metadata: Mapping[str, Any] | None = None,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        self._require_client_capability("sampling")
        request = normalize_sampling_request({
            "messages": list(messages),
            "modelPreferences": None if model_preferences is None else dict(model_preferences.items()),
            "systemPrompt": system_prompt,
            "includeContext": include_context,
            "temperature": temperature,
            "maxTokens": max_tokens,
            "stopSequences": None if stop_sequences is None else list(stop_sequences),
            "metadata": None if metadata is None else dict(metadata.items()),
        })
        return normalize_sampling_result(self._request("sampling/createMessage", request, timeout=timeout))

    def elicit(
        self,
        message: str,
        requested_schema: Mapping[str, Any],
        *,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        self._require_client_capability("elicitation")
        result = self._request(
            "elicitation/create",
            {
                "message": str(message),
                "requestedSchema": dict(requested_schema.items()),
            },
            timeout=timeout,
        )
        return normalize_elicitation_result(result)

    def send_log(
        self,
        level: str,
        data: Any,
        *,
        logger: str | None = None,
    ) -> bool:
        if not self._server._enable_logging:
            return False
        normalized_level = normalize_log_level(level)
        if not log_level_enabled(self._logging_level, normalized_level):
            return False
        payload: dict[str, Any] = {
            "level": normalized_level,
            "data": data,
        }
        if logger:
            payload["logger"] = str(logger)
        self._notify("notifications/message", payload)
        return True

    def notify_tools_changed(self) -> bool:
        return self._notify_if_enabled("notifications/tools/list_changed")

    def notify_prompts_changed(self) -> bool:
        return self._notify_if_enabled("notifications/prompts/list_changed")

    def notify_resources_changed(self) -> bool:
        return self._notify_if_enabled("notifications/resources/list_changed")

    def notify_resource_updated(self, uri: str) -> bool:
        if not self._server._resource_subscriptions_enabled:
            return False
        normalized_uri = str(uri)
        if not self.is_resource_subscribed(normalized_uri):
            return False
        self._notify("notifications/resources/updated", {"uri": normalized_uri})
        return True

    def is_resource_subscribed(self, uri: str) -> bool:
        normalized_uri = str(uri)
        with self._subscription_lock:
            return any(
                self._uri_matches_subscription(subscription, normalized_uri)
                for subscription in self._resource_subscriptions
            )

    def _request(self, method: str, params: Mapping[str, Any], *, timeout: float | None) -> Any:
        wait_timeout = self._default_timeout if timeout is None else float(timeout)
        return self._request_fn(str(method), dict(params.items()), wait_timeout)

    def _notify(self, method: str, params: Mapping[str, Any] | None = None) -> None:
        self._notify_fn(str(method), {} if params is None else dict(params.items()))

    def _notify_if_enabled(self, method: str) -> bool:
        if not self._server._list_changed_notifications_enabled:
            return False
        self._notify(method, {})
        return True

    def _require_client_capability(self, name: str) -> None:
        if name not in self._client_capabilities:
            raise RuntimeError(f"connected MCP client does not advertise {name!r}")

    def _mark_initialized(self) -> None:
        self._initialized = True

    def _record_roots_changed(self) -> None:
        self._roots_change_count += 1

    def _record_cancelled(self, payload: Mapping[str, Any]) -> None:
        self._cancel_notifications.append(dict(payload.items()))

    def _set_logging_level(self, level: str) -> None:
        self._logging_level = normalize_log_level(level)

    def _add_subscription(self, uri: str) -> None:
        with self._subscription_lock:
            self._resource_subscriptions.add(str(uri))

    def _remove_subscription(self, uri: str) -> None:
        with self._subscription_lock:
            self._resource_subscriptions.discard(str(uri))

    @staticmethod
    def _uri_matches_subscription(subscription: str, uri: str) -> bool:
        if subscription == uri:
            return True
        trimmed = subscription.rstrip("/")
        return bool(trimmed) and uri.startswith(trimmed + "/")


class MCPServer:
    def __init__(
        self,
        *,
        name: str = "agentcore-mcp",
        version: str = "0.1",
        instructions: str = "",
        request_timeout: float = 30.0,
        enable_logging: bool = True,
        resource_subscriptions_enabled: bool = True,
        list_changed_notifications_enabled: bool = True,
        auto_notify_catalog_changes: bool = True,
    ):
        self._name = str(name)
        self._version = str(version)
        self._instructions = str(instructions)
        self._request_timeout = float(request_timeout)
        self._enable_logging = bool(enable_logging)
        self._resource_subscriptions_enabled = bool(resource_subscriptions_enabled)
        self._list_changed_notifications_enabled = bool(list_changed_notifications_enabled)
        self._auto_notify_catalog_changes = bool(auto_notify_catalog_changes)
        self._tools: dict[str, MCPTool] = {}
        self._prompts: dict[str, MCPPrompt] = {}
        self._resources: dict[str, MCPResource] = {}
        self._resource_templates: dict[str, MCPResourceTemplate] = {}
        self._session_local = threading.local()
        self._active_session_lock = threading.Lock()
        self._active_session: MCPServerSession | None = None

    @property
    def name(self) -> str:
        return self._name

    @property
    def version(self) -> str:
        return self._version

    def current_session(self) -> MCPServerSession:
        session = getattr(self._session_local, "session", None)
        if isinstance(session, MCPServerSession):
            return session
        with self._active_session_lock:
            active = self._active_session
        if active is None:
            raise RuntimeError("no active MCP session is available")
        return active

    def list_client_roots(self, *, timeout: float | None = None) -> list[dict[str, Any]]:
        return self.current_session().list_roots(timeout=timeout)

    def sample(
        self,
        messages: Sequence[Mapping[str, Any]],
        *,
        model_preferences: Mapping[str, Any] | None = None,
        system_prompt: str | None = None,
        include_context: str | None = None,
        temperature: float | None = None,
        max_tokens: int | None = None,
        stop_sequences: Sequence[str] | None = None,
        metadata: Mapping[str, Any] | None = None,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        return self.current_session().sample(
            messages,
            model_preferences=model_preferences,
            system_prompt=system_prompt,
            include_context=include_context,
            temperature=temperature,
            max_tokens=max_tokens,
            stop_sequences=stop_sequences,
            metadata=metadata,
            timeout=timeout,
        )

    def elicit(
        self,
        message: str,
        requested_schema: Mapping[str, Any],
        *,
        timeout: float | None = None,
    ) -> dict[str, Any]:
        return self.current_session().elicit(message, requested_schema, timeout=timeout)

    def log(self, level: str, data: Any, *, logger: str | None = None) -> bool:
        return self.current_session().send_log(level, data, logger=logger)

    def notify_tools_changed(self) -> bool:
        return self.current_session().notify_tools_changed()

    def notify_prompts_changed(self) -> bool:
        return self.current_session().notify_prompts_changed()

    def notify_resources_changed(self) -> bool:
        return self.current_session().notify_resources_changed()

    def notify_resource_updated(self, uri: str) -> bool:
        return self.current_session().notify_resource_updated(uri)

    def add_tool(
        self,
        name: str,
        handler: Any,
        *,
        description: str = "",
        input_schema: Mapping[str, Any] | None = None,
        title: str = "",
        annotations: Mapping[str, Any] | None = None,
    ) -> None:
        tool_name = str(name).strip()
        if not tool_name:
            raise ValueError("tool name must be a non-empty string")
        if not callable(handler):
            raise TypeError("tool handler must be callable")
        self._tools[tool_name] = MCPTool(
            name=tool_name,
            handler=handler,
            description=str(description),
            input_schema=None if input_schema is None else dict(input_schema.items()),
            title=str(title),
            annotations=None if annotations is None else dict(annotations.items()),
        )
        self._auto_notify_changed("tools")

    def tool(
        self,
        name: str | None = None,
        *,
        description: str = "",
        input_schema: Mapping[str, Any] | None = None,
        title: str = "",
        annotations: Mapping[str, Any] | None = None,
    ):
        def decorator(function: Any) -> Any:
            resolved_name = function.__name__ if name is None else str(name)
            self.add_tool(
                resolved_name,
                function,
                description=description,
                input_schema=input_schema,
                title=title,
                annotations=annotations,
            )
            return function

        return decorator

    def list_tools(self) -> list[dict[str, Any]]:
        return [
            tool_descriptor(
                tool.name,
                description=tool.description or None,
                input_schema=tool.input_schema,
                title=tool.title or None,
                annotations=tool.annotations,
            )
            for tool in self._tools.values()
        ]

    def call_tool(
        self,
        name: str,
        arguments: Mapping[str, Any] | None = None,
        *,
        session: MCPServerSession | None = None,
    ) -> dict[str, Any]:
        tool_name = str(name)
        tool = self._tools.get(tool_name)
        if tool is None:
            return {
                "content": [{"type": "text", "text": f"unknown tool: {tool_name}"}],
                "isError": True,
            }

        resolved_session = self._resolve_session(session)
        invoker, uses_metadata = _compile_tool_handler(tool.handler)
        normalized_arguments = {} if arguments is None else dict(arguments.items())
        metadata = (
            {
                "tool_name": tool.name,
                "server_name": self._name,
                "server_version": self._version,
                "session": resolved_session,
                "mcp": resolved_session,
                "client_capabilities": None if resolved_session is None else resolved_session.client_capabilities,
            }
            if uses_metadata else {}
        )
        try:
            value = invoker(normalized_arguments, metadata)
        except Exception as exc:
            return {
                "content": [{"type": "text", "text": str(exc)}],
                "isError": True,
            }
        return coerce_mcp_tool_result(value)

    def add_prompt(
        self,
        name: str,
        handler: Any,
        *,
        description: str = "",
        arguments: Sequence[Mapping[str, Any]] | None = None,
        title: str = "",
        argument_completions: Mapping[str, Any] | None = None,
    ) -> None:
        prompt_name = str(name).strip()
        if not prompt_name:
            raise ValueError("prompt name must be a non-empty string")
        if not callable(handler):
            raise TypeError("prompt handler must be callable")
        self._prompts[prompt_name] = MCPPrompt(
            name=prompt_name,
            handler=handler,
            description=str(description),
            arguments=_normalize_prompt_arguments(arguments),
            title=str(title),
            argument_completions={} if argument_completions is None else {
                str(key): value for key, value in argument_completions.items()
            },
        )
        self._auto_notify_changed("prompts")

    def add_prompt_template(
        self,
        name: str,
        template: Any,
        *,
        description: str = "",
        arguments: Sequence[Mapping[str, Any]] | None = None,
        title: str = "",
        argument_completions: Mapping[str, Any] | None = None,
    ) -> None:
        def handler(values: dict[str, Any]) -> Any:
            return template.render(values) if hasattr(template, "render") else template

        self.add_prompt(
            name,
            handler,
            description=description,
            arguments=arguments,
            title=title,
            argument_completions=argument_completions,
        )

    def prompt(
        self,
        name: str | None = None,
        *,
        description: str = "",
        arguments: Sequence[Mapping[str, Any]] | None = None,
        title: str = "",
        argument_completions: Mapping[str, Any] | None = None,
    ):
        def decorator(function: Any) -> Any:
            resolved_name = function.__name__ if name is None else str(name)
            self.add_prompt(
                resolved_name,
                function,
                description=description,
                arguments=arguments,
                title=title,
                argument_completions=argument_completions,
            )
            return function

        return decorator

    def list_prompts(self) -> list[dict[str, Any]]:
        return [
            prompt_descriptor(
                prompt.name,
                description=prompt.description or None,
                arguments=prompt.arguments,
                title=prompt.title or None,
            )
            for prompt in self._prompts.values()
        ]

    def get_prompt(
        self,
        name: str,
        arguments: Mapping[str, Any] | None = None,
        *,
        session: MCPServerSession | None = None,
    ) -> dict[str, Any]:
        prompt_name = str(name)
        prompt = self._prompts.get(prompt_name)
        if prompt is None:
            raise KeyError(f"unknown prompt: {prompt_name}")
        resolved_session = self._resolve_session(session)
        invoker, uses_metadata = _compile_prompt_handler(prompt.handler)
        normalized_arguments = {} if arguments is None else dict(arguments.items())
        metadata = (
            {
                "prompt_name": prompt.name,
                "server_name": self._name,
                "server_version": self._version,
                "session": resolved_session,
                "mcp": resolved_session,
                "client_capabilities": None if resolved_session is None else resolved_session.client_capabilities,
            }
            if uses_metadata else {}
        )
        value = invoker(normalized_arguments, metadata)
        return _normalize_prompt_handler_result(
            value,
            arguments=normalized_arguments,
            name=prompt.name,
            description=prompt.description,
        )

    def add_resource(
        self,
        uri: str,
        handler: Any,
        *,
        name: str = "",
        description: str = "",
        mime_type: str = "",
        size: int | None = None,
        annotations: Mapping[str, Any] | None = None,
    ) -> None:
        resource_uri = str(uri).strip()
        if not resource_uri:
            raise ValueError("resource uri must be a non-empty string")
        stored_handler = handler
        if not callable(handler):
            static_value = handler

            def stored_handler() -> Any:
                return static_value

        self._resources[resource_uri] = MCPResource(
            uri=resource_uri,
            handler=stored_handler,
            name=str(name),
            description=str(description),
            mime_type=str(mime_type),
            size=None if size is None else int(size),
            annotations=None if annotations is None else dict(annotations.items()),
        )
        self._auto_notify_changed("resources")

    def list_resources(self) -> list[dict[str, Any]]:
        return [
            resource_descriptor(
                resource.uri,
                name=resource.name or None,
                description=resource.description or None,
                mime_type=resource.mime_type or None,
                size=resource.size,
                annotations=resource.annotations,
            )
            for resource in self._resources.values()
        ]

    def read_resource(
        self,
        uri: str,
        *,
        session: MCPServerSession | None = None,
    ) -> dict[str, Any]:
        normalized_uri = str(uri)
        resolved_session = self._resolve_session(session)
        resource = self._resources.get(normalized_uri)
        if resource is not None:
            invoker, uses_metadata = _compile_resource_handler(resource.handler, template=False)
            metadata = (
                {
                    "resource_uri": resource.uri,
                    "server_name": self._name,
                    "server_version": self._version,
                    "session": resolved_session,
                    "mcp": resolved_session,
                    "client_capabilities": None if resolved_session is None else resolved_session.client_capabilities,
                }
                if uses_metadata else {}
            )
            value = invoker(resource.uri, {}, metadata)
            return coerce_mcp_resource_result(
                value,
                default_uri=resource.uri,
                default_name=resource.name,
                default_mime_type=resource.mime_type,
            )

        for template in self._resource_templates.values():
            arguments = match_uri_template(template.uri_template, normalized_uri)
            if arguments is None:
                continue
            invoker, uses_metadata = _compile_resource_handler(template.handler, template=True)
            metadata = (
                {
                    "uri_template": template.uri_template,
                    "resource_uri": normalized_uri,
                    "server_name": self._name,
                    "server_version": self._version,
                    "arguments": arguments,
                    "session": resolved_session,
                    "mcp": resolved_session,
                    "client_capabilities": None if resolved_session is None else resolved_session.client_capabilities,
                }
                if uses_metadata else {}
            )
            value = invoker(normalized_uri, arguments, metadata)
            return coerce_mcp_resource_result(
                value,
                default_uri=normalized_uri,
                default_name=template.name,
                default_mime_type=template.mime_type,
            )

        raise KeyError(f"unknown resource: {normalized_uri}")

    def add_resource_template(
        self,
        uri_template: str,
        handler: Any,
        *,
        name: str = "",
        description: str = "",
        mime_type: str = "",
        annotations: Mapping[str, Any] | None = None,
        argument_completions: Mapping[str, Any] | None = None,
    ) -> None:
        normalized_template = str(uri_template).strip()
        if not normalized_template:
            raise ValueError("resource template must be a non-empty string")
        if not callable(handler):
            raise TypeError("resource template handler must be callable")
        self._resource_templates[normalized_template] = MCPResourceTemplate(
            uri_template=normalized_template,
            handler=handler,
            name=str(name),
            description=str(description),
            mime_type=str(mime_type),
            annotations=None if annotations is None else dict(annotations.items()),
            argument_completions={} if argument_completions is None else {
                str(key): value for key, value in argument_completions.items()
            },
        )
        self._auto_notify_changed("resources")

    def resource_template(
        self,
        uri_template: str | None = None,
        *,
        name: str = "",
        description: str = "",
        mime_type: str = "",
        annotations: Mapping[str, Any] | None = None,
        argument_completions: Mapping[str, Any] | None = None,
    ):
        def decorator(function: Any) -> Any:
            resolved_template = function.__name__ if uri_template is None else str(uri_template)
            self.add_resource_template(
                resolved_template,
                function,
                name=name,
                description=description,
                mime_type=mime_type,
                annotations=annotations,
                argument_completions=argument_completions,
            )
            return function

        return decorator

    def list_resource_templates(self) -> list[dict[str, Any]]:
        return [
            resource_template_descriptor(
                template.uri_template,
                name=template.name or None,
                description=template.description or None,
                mime_type=template.mime_type or None,
                annotations=template.annotations,
            )
            for template in self._resource_templates.values()
        ]

    def complete(
        self,
        ref: Mapping[str, Any],
        argument_name: str,
        value: str,
        *,
        arguments: Mapping[str, Any] | None = None,
        session: MCPServerSession | None = None,
    ) -> dict[str, Any]:
        normalized_ref = dict(ref.items())
        normalized_argument_name = str(argument_name)
        normalized_value = str(value)
        normalized_arguments = {} if arguments is None else dict(arguments.items())
        resolved_session = self._resolve_session(session)

        ref_type = str(normalized_ref.get("type", ""))
        if ref_type == "ref/prompt":
            prompt_name = str(normalized_ref.get("name", ""))
            prompt = self._prompts.get(prompt_name)
            if prompt is None:
                raise KeyError(f"unknown prompt: {prompt_name}")
            provider = prompt.argument_completions.get(normalized_argument_name)
            if provider is None:
                return normalize_completion_result([])
            return _run_completion_provider(
                provider,
                normalized_value,
                normalized_arguments,
                {
                    "type": ref_type,
                    "name": prompt_name,
                    "argument_name": normalized_argument_name,
                    "server_name": self._name,
                    "session": resolved_session,
                    "mcp": resolved_session,
                },
            )

        if ref_type == "ref/resource":
            uri_or_template = str(normalized_ref.get("uri", normalized_ref.get("uriTemplate", "")))
            template = self._resource_templates.get(uri_or_template)
            if template is None:
                for candidate in self._resource_templates.values():
                    if candidate.uri_template == uri_or_template:
                        template = candidate
                        break
            if template is None:
                raise KeyError(f"unknown resource template: {uri_or_template}")
            provider = template.argument_completions.get(normalized_argument_name)
            if provider is None:
                return normalize_completion_result([])
            return _run_completion_provider(
                provider,
                normalized_value,
                normalized_arguments,
                {
                    "type": ref_type,
                    "uri_template": template.uri_template,
                    "argument_name": normalized_argument_name,
                    "server_name": self._name,
                    "session": resolved_session,
                    "mcp": resolved_session,
                },
            )

        raise KeyError(f"unsupported completion ref type: {ref_type}")

    def serve_stdio(
        self,
        *,
        input_stream: Any = None,
        output_stream: Any = None,
        error_stream: Any = None,
    ) -> None:
        stream_in = sys.stdin if input_stream is None else input_stream
        stream_out = sys.stdout if output_stream is None else output_stream
        stream_err = sys.stderr if error_stream is None else error_stream

        send_lock = threading.Lock()
        pending_lock = threading.Lock()
        pending_requests: dict[int, _PendingRequest] = {}
        thread_lock = threading.Lock()
        active_threads: set[threading.Thread] = set()
        next_request_id = 1
        session_box: dict[str, MCPServerSession | None] = {"session": None}

        def write_message(payload: Mapping[str, Any]) -> None:
            with send_lock:
                self._write_message(stream_out, payload)

        def write_result(request_id: Any, result: Any) -> None:
            write_message({
                "jsonrpc": JSONRPC_VERSION,
                "id": request_id,
                "result": result,
            })

        def write_error(request_id: Any, *, code: int, message: str) -> None:
            write_message({
                "jsonrpc": JSONRPC_VERSION,
                "id": request_id,
                "error": {
                    "code": int(code),
                    "message": str(message),
                },
            })

        def notify(method: str, params: Mapping[str, Any] | None = None) -> None:
            write_message({
                "jsonrpc": JSONRPC_VERSION,
                "method": str(method),
                "params": {} if params is None else dict(params.items()),
            })

        def request(method: str, params: Mapping[str, Any], timeout: float) -> Any:
            nonlocal next_request_id
            pending = _PendingRequest(event=threading.Event())
            with pending_lock:
                request_id = next_request_id
                next_request_id += 1
                pending_requests[request_id] = pending

            write_message({
                "jsonrpc": JSONRPC_VERSION,
                "id": request_id,
                "method": str(method),
                "params": dict(params.items()),
            })

            if not pending.event.wait(timeout):
                with pending_lock:
                    pending_requests.pop(request_id, None)
                raise TimeoutError(f"MCP client request {method!r} timed out after {timeout:.2f}s")

            if pending.error is not None:
                if isinstance(pending.error, Mapping):
                    code = pending.error.get("code")
                    message = pending.error.get("message", "unknown MCP error")
                    raise RuntimeError(f"MCP client request {method!r} failed with code={code}: {message}")
                raise RuntimeError(str(pending.error))
            return pending.result

        def activate_session(session: MCPServerSession | None) -> None:
            self._session_local.session = session

        def deactivate_session() -> None:
            if hasattr(self._session_local, "session"):
                del self._session_local.session

        def dispatch_request(message: Mapping[str, Any]) -> None:
            request_id = message.get("id")
            method = str(message.get("method", ""))
            params = message.get("params", {})
            if not isinstance(params, Mapping):
                write_error(request_id, code=-32602, message="request params must be a mapping")
                return

            try:
                if method == "initialize":
                    client_capabilities = params.get("capabilities", {})
                    client_info = params.get("clientInfo", {})
                    if not isinstance(client_capabilities, Mapping):
                        client_capabilities = {}
                    if not isinstance(client_info, Mapping):
                        client_info = {}
                    session = MCPServerSession(
                        self,
                        request_fn=request,
                        notify_fn=notify,
                        client_capabilities=client_capabilities,
                        client_info=client_info,
                        default_timeout=self._request_timeout,
                    )
                    session_box["session"] = session
                    with self._active_session_lock:
                        self._active_session = session
                    activate_session(session)
                    write_result(request_id, {
                        "protocolVersion": MCP_PROTOCOL_VERSION,
                        "capabilities": self._capabilities(),
                        "serverInfo": {
                            "name": self._name,
                            "version": self._version,
                        },
                        **({"instructions": self._instructions} if self._instructions else {}),
                    })
                    return

                session = session_box["session"]
                activate_session(session)

                if method == "ping":
                    write_result(request_id, {})
                elif method == "tools/list":
                    write_result(request_id, self._paged_result(params, "tools", self.list_tools()))
                elif method == "tools/call":
                    name = params.get("name")
                    if name is None:
                        write_error(request_id, code=-32602, message="tools/call requires params.name")
                        return
                    arguments = params.get("arguments", {})
                    if arguments is None:
                        arguments = {}
                    if not isinstance(arguments, Mapping):
                        write_error(request_id, code=-32602, message="tools/call arguments must be a mapping")
                        return
                    write_result(request_id, self.call_tool(str(name), arguments, session=session))
                elif method == "prompts/list":
                    write_result(request_id, self._paged_result(params, "prompts", self.list_prompts()))
                elif method == "prompts/get":
                    name = params.get("name")
                    if name is None:
                        write_error(request_id, code=-32602, message="prompts/get requires params.name")
                        return
                    arguments = params.get("arguments", {})
                    if arguments is None:
                        arguments = {}
                    if not isinstance(arguments, Mapping):
                        write_error(request_id, code=-32602, message="prompts/get arguments must be a mapping")
                        return
                    write_result(request_id, self.get_prompt(str(name), arguments, session=session))
                elif method == "resources/list":
                    write_result(request_id, self._paged_result(params, "resources", self.list_resources()))
                elif method == "resources/templates/list":
                    write_result(
                        request_id,
                        self._paged_result(params, "resourceTemplates", self.list_resource_templates()),
                    )
                elif method == "resources/read":
                    uri = params.get("uri")
                    if uri is None:
                        write_error(request_id, code=-32602, message="resources/read requires params.uri")
                        return
                    write_result(request_id, self.read_resource(str(uri), session=session))
                elif method == "resources/subscribe":
                    if not self._resource_subscriptions_enabled:
                        write_error(request_id, code=-32601, message="resource subscriptions are not enabled")
                        return
                    uri = params.get("uri")
                    if uri is None:
                        write_error(request_id, code=-32602, message="resources/subscribe requires params.uri")
                        return
                    if session is not None:
                        session._add_subscription(str(uri))
                    write_result(request_id, {})
                elif method == "resources/unsubscribe":
                    if not self._resource_subscriptions_enabled:
                        write_error(request_id, code=-32601, message="resource subscriptions are not enabled")
                        return
                    uri = params.get("uri")
                    if uri is None:
                        write_error(request_id, code=-32602, message="resources/unsubscribe requires params.uri")
                        return
                    if session is not None:
                        session._remove_subscription(str(uri))
                    write_result(request_id, {})
                elif method == "logging/setLevel":
                    if not self._enable_logging:
                        write_error(request_id, code=-32601, message="logging is not enabled")
                        return
                    level = params.get("level")
                    if level is None:
                        write_error(request_id, code=-32602, message="logging/setLevel requires params.level")
                        return
                    if session is not None:
                        session._set_logging_level(str(level))
                    write_result(request_id, {})
                elif method == "completion/complete":
                    ref = params.get("ref")
                    argument = params.get("argument")
                    if not isinstance(ref, Mapping) or not isinstance(argument, Mapping):
                        write_error(
                            request_id,
                            code=-32602,
                            message="completion/complete requires params.ref and params.argument mappings",
                        )
                        return
                    argument_name = argument.get("name")
                    value = argument.get("value", "")
                    if argument_name is None:
                        write_error(request_id, code=-32602, message="completion/complete requires argument.name")
                        return
                    context = params.get("context", {})
                    context_arguments = (
                        context.get("arguments")
                        if isinstance(context, Mapping) and isinstance(context.get("arguments"), Mapping)
                        else None
                    )
                    write_result(
                        request_id,
                        self.complete(
                            ref,
                            str(argument_name),
                            str(value),
                            arguments=context_arguments,
                            session=session,
                        ),
                    )
                else:
                    write_error(request_id, code=-32601, message=f"unknown method: {method}")
            except KeyError as exc:
                write_error(request_id, code=-32002, message=str(exc))
            except Exception as exc:
                stream_err.write(f"agentcore MCP server error: {exc}\n")
                stream_err.flush()
                write_error(request_id, code=-32603, message=str(exc))
            finally:
                deactivate_session()

        def handle_notification(message: Mapping[str, Any]) -> None:
            method = str(message.get("method", ""))
            params = message.get("params", {})
            if not isinstance(params, Mapping):
                params = {}
            session = session_box["session"]
            if session is None:
                return
            if method == "notifications/initialized":
                session._mark_initialized()
            elif method == "notifications/roots/list_changed":
                session._record_roots_changed()
            elif method == "notifications/cancelled":
                session._record_cancelled(params)

        def handle_response(message: Mapping[str, Any]) -> None:
            try:
                response_id = int(message.get("id"))
            except (TypeError, ValueError):
                return
            with pending_lock:
                pending = pending_requests.pop(response_id, None)
            if pending is None:
                return
            if "error" in message:
                pending.error = message.get("error")
            else:
                pending.result = message.get("result")
            pending.event.set()

        def spawn_request_handler(message: Mapping[str, Any]) -> None:
            def run() -> None:
                try:
                    dispatch_request(message)
                finally:
                    with thread_lock:
                        active_threads.discard(threading.current_thread())

            thread = threading.Thread(
                target=run,
                name=f"agentcore-mcp-{message.get('method', 'request')}",
                daemon=True,
            )
            with thread_lock:
                active_threads.add(thread)
            thread.start()

        try:
            for raw_line in stream_in:
                line = raw_line.strip()
                if not line:
                    continue

                try:
                    message = json.loads(line)
                except json.JSONDecodeError:
                    continue

                if not isinstance(message, Mapping):
                    continue

                if "id" in message and ("result" in message or "error" in message):
                    handle_response(message)
                    continue

                if "method" not in message:
                    continue

                if "id" in message:
                    spawn_request_handler(message)
                else:
                    handle_notification(message)
        finally:
            with pending_lock:
                pending = list(pending_requests.values())
                pending_requests.clear()
            for item in pending:
                item.error = {"code": -1, "message": "MCP transport closed"}
                item.event.set()
            with thread_lock:
                threads = list(active_threads)
            for thread in threads:
                thread.join(timeout=1.0)
            with self._active_session_lock:
                self._active_session = None

    def run_stdio(self) -> None:
        self.serve_stdio()

    def _capabilities(self) -> dict[str, Any]:
        capabilities: dict[str, Any] = {
            "tools": {
                "listChanged": self._list_changed_notifications_enabled,
            },
            "prompts": {
                "listChanged": self._list_changed_notifications_enabled,
            },
            "resources": {
                "subscribe": self._resource_subscriptions_enabled,
                "listChanged": self._list_changed_notifications_enabled,
            },
        }
        if self._enable_logging:
            capabilities["logging"] = {}
        if any(prompt.argument_completions for prompt in self._prompts.values()) or any(
            template.argument_completions for template in self._resource_templates.values()
        ):
            capabilities["completions"] = {}
        return capabilities

    def _resolve_session(self, session: MCPServerSession | None) -> MCPServerSession | None:
        if session is not None:
            return session
        local_session = getattr(self._session_local, "session", None)
        if isinstance(local_session, MCPServerSession):
            return local_session
        return None

    def _auto_notify_changed(self, kind: str) -> None:
        if not self._auto_notify_catalog_changes:
            return
        session = self._resolve_session(None)
        if session is None:
            with self._active_session_lock:
                session = self._active_session
        if session is None:
            return
        if kind == "tools":
            session.notify_tools_changed()
        elif kind == "prompts":
            session.notify_prompts_changed()
        elif kind == "resources":
            session.notify_resources_changed()

    def _paged_result(self, params: Mapping[str, Any], key: str, items: list[dict[str, Any]]) -> dict[str, Any]:
        cursor = params.get("cursor")
        if cursor not in {None, ""}:
            return {key: []}
        return {key: items}

    @classmethod
    def from_tool_registry(
        cls,
        tool_registry: Any,
        *,
        name: str = "agentcore-tools",
        version: str = "0.1",
        descriptions: Mapping[str, str] | None = None,
        input_schemas: Mapping[str, Mapping[str, Any]] | None = None,
    ) -> "MCPServer":
        server = cls(name=name, version=version)
        description_map = {} if descriptions is None else {str(k): str(v) for k, v in descriptions.items()}
        schema_map = {} if input_schemas is None else {
            str(key): dict(value.items()) for key, value in input_schemas.items()
        }

        for tool in tool_registry.list():
            tool_name = str(tool["name"])
            metadata = dict(tool.get("metadata", {}) or {})
            display_name = str(metadata.get("display_name", tool_name))
            description = description_map.get(tool_name, f"Bridged AgentCore tool {display_name}.")

            def make_handler(bound_name: str):
                def handler(arguments: dict[str, Any]) -> Any:
                    details = tool_registry.invoke_with_metadata(bound_name, arguments, decode="json")
                    if not details.get("ok", False):
                        return {
                            "structuredContent": details,
                            "content": [{
                                "type": "text",
                                "text": json.dumps(details, separators=(",", ":"), ensure_ascii=False, sort_keys=True),
                            }],
                            "isError": True,
                        }
                    return details["output"]

                return handler

            server.add_tool(
                tool_name,
                make_handler(tool_name),
                description=description,
                input_schema=schema_map.get(tool_name),
                title=display_name,
            )

        return server

    @classmethod
    def from_compiled_graph(
        cls,
        compiled_graph: Any,
        *,
        name: str = "agentcore-tools",
        version: str = "0.1",
        descriptions: Mapping[str, str] | None = None,
        input_schemas: Mapping[str, Mapping[str, Any]] | None = None,
    ) -> "MCPServer":
        return cls.from_tool_registry(
            compiled_graph.tools,
            name=name,
            version=version,
            descriptions=descriptions,
            input_schemas=input_schemas,
        )

    def _write_result(self, output_stream: Any, request_id: Any, result: Any) -> None:
        self._write_message(output_stream, {
            "jsonrpc": JSONRPC_VERSION,
            "id": request_id,
            "result": result,
        })

    def _write_error(self, output_stream: Any, request_id: Any, *, code: int, message: str) -> None:
        self._write_message(output_stream, {
            "jsonrpc": JSONRPC_VERSION,
            "id": request_id,
            "error": {
                "code": int(code),
                "message": str(message),
            },
        })

    def _write_message(self, output_stream: Any, payload: Mapping[str, Any]) -> None:
        serialized = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
        if "\n" in serialized or "\r" in serialized:
            raise ValueError("serialized MCP payload unexpectedly contains a newline")
        output_stream.write(serialized)
        output_stream.write("\n")
        output_stream.flush()


class MCPToolServer(MCPServer):
    pass
