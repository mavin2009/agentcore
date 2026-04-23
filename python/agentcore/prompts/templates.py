from __future__ import annotations

import json
import string
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from typing import Any


def _stringify_prompt_value(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    if isinstance(value, (bytes, bytearray, memoryview)):
        try:
            return bytes(value).decode("utf-8")
        except UnicodeDecodeError:
            return bytes(value).hex()
    if isinstance(value, (bool, int, float)):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, Mapping):
        try:
            return json.dumps(dict(value), indent=2, ensure_ascii=False, sort_keys=True)
        except TypeError:
            return str(dict(value))
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray, memoryview)):
        try:
            return json.dumps(list(value), indent=2, ensure_ascii=False)
        except TypeError:
            return str(list(value))
    return str(value)


class _PromptFormatter(string.Formatter):
    def format_field(self, value: Any, format_spec: str) -> str:
        if format_spec:
            return super().format_field(value, format_spec)
        return _stringify_prompt_value(value)


_PROMPT_FORMATTER = _PromptFormatter()


def _field_root(field_name: str) -> str:
    dot_index = field_name.find(".")
    bracket_index = field_name.find("[")
    split_candidates = [index for index in (dot_index, bracket_index) if index != -1]
    if not split_candidates:
        return field_name
    return field_name[: min(split_candidates)]


def _template_variables(template: str) -> tuple[str, ...]:
    variables: list[str] = []
    seen: set[str] = set()
    for _, field_name, _, _ in _PROMPT_FORMATTER.parse(template):
        if not field_name:
            continue
        root = _field_root(field_name)
        if root and root not in seen:
            seen.add(root)
            variables.append(root)
    return tuple(variables)


def _normalize_render_values(
    defaults: Mapping[str, Any],
    values: Mapping[str, Any] | None,
    kwargs: dict[str, Any],
) -> dict[str, Any]:
    merged = dict(defaults)
    if values is not None:
        if not hasattr(values, "items"):
            raise TypeError("prompt render values must be a mapping or None")
        merged.update(dict(values.items()))
    merged.update(kwargs)
    return merged


def _render_text_template(
    template: str,
    values: Mapping[str, Any],
) -> str:
    missing = [name for name in _template_variables(template) if name not in values]
    if missing:
        raise KeyError(
            "missing prompt template values for: " + ", ".join(sorted(missing))
        )
    try:
        return _PROMPT_FORMATTER.vformat(template, (), dict(values))
    except KeyError as exc:
        missing_name = exc.args[0] if exc.args else "<unknown>"
        raise KeyError(f"missing prompt template value for: {missing_name}") from exc


@dataclass(frozen=True)
class PromptMessage:
    role: str
    content: str
    name: str | None = None

    def as_dict(self) -> dict[str, str]:
        payload = {
            "role": str(self.role),
            "content": str(self.content),
        }
        if self.name:
            payload["name"] = str(self.name)
        return payload


@dataclass(frozen=True)
class RenderedPrompt:
    text: str
    name: str | None = None

    def __str__(self) -> str:
        return self.text

    def to_model_input(self) -> str:
        return self.text


@dataclass(frozen=True)
class RenderedChatPrompt:
    messages: tuple[PromptMessage, ...]
    name: str | None = None
    separator: str = "\n\n"

    def as_messages(self) -> list[dict[str, str]]:
        return [message.as_dict() for message in self.messages]

    def to_text(
        self,
        *,
        include_roles: bool = True,
        separator: str | None = None,
    ) -> str:
        normalized_separator = self.separator if separator is None else str(separator)
        if include_roles:
            parts = [
                f"{message.role.upper()}:\n{message.content}"
                for message in self.messages
            ]
        else:
            parts = [message.content for message in self.messages]
        return normalized_separator.join(parts)

    def __str__(self) -> str:
        return self.to_text()

    def to_model_input(self, *, mode: str = "text") -> Any:
        normalized_mode = str(mode).strip().lower()
        if normalized_mode == "text":
            return self.to_text()
        if normalized_mode == "messages":
            return self.as_messages()
        raise ValueError("mode must be 'text' or 'messages'")


@dataclass(frozen=True)
class PromptTemplate:
    template: str
    defaults: dict[str, Any] = field(default_factory=dict)
    name: str | None = None

    @property
    def variables(self) -> tuple[str, ...]:
        return _template_variables(self.template)

    def partial(self, /, **defaults: Any) -> "PromptTemplate":
        merged_defaults = dict(self.defaults)
        merged_defaults.update(defaults)
        return PromptTemplate(
            self.template,
            defaults=merged_defaults,
            name=self.name,
        )

    def render(
        self,
        values: Mapping[str, Any] | None = None,
        /,
        **kwargs: Any,
    ) -> RenderedPrompt:
        merged = _normalize_render_values(self.defaults, values, kwargs)
        return RenderedPrompt(
            _render_text_template(self.template, merged),
            name=self.name,
        )

    def format(
        self,
        values: Mapping[str, Any] | None = None,
        /,
        **kwargs: Any,
    ) -> str:
        return self.render(values, **kwargs).text

    def to_model_input(self) -> str:
        return self.render().to_model_input()


@dataclass(frozen=True)
class MessageTemplate:
    role: str
    template: str
    defaults: dict[str, Any] = field(default_factory=dict)
    name: str | None = None

    @property
    def variables(self) -> tuple[str, ...]:
        return _template_variables(self.template)

    def partial(self, /, **defaults: Any) -> "MessageTemplate":
        merged_defaults = dict(self.defaults)
        merged_defaults.update(defaults)
        return MessageTemplate(
            role=self.role,
            template=self.template,
            defaults=merged_defaults,
            name=self.name,
        )

    def render(
        self,
        values: Mapping[str, Any] | None = None,
        /,
        **kwargs: Any,
    ) -> PromptMessage:
        merged = _normalize_render_values(self.defaults, values, kwargs)
        return PromptMessage(
            role=str(self.role),
            content=_render_text_template(self.template, merged),
            name=self.name,
        )

    def format(
        self,
        values: Mapping[str, Any] | None = None,
        /,
        **kwargs: Any,
    ) -> str:
        return self.render(values, **kwargs).content


def _coerce_message_template(value: Any) -> MessageTemplate:
    if isinstance(value, MessageTemplate):
        return value
    if isinstance(value, PromptMessage):
        return MessageTemplate(
            role=value.role,
            template=value.content,
            name=value.name,
        )
    if isinstance(value, (tuple, list)) and len(value) == 2:
        role, template = value
        return MessageTemplate(role=str(role), template=str(template))
    if hasattr(value, "items"):
        mapping = dict(value.items())
        role = mapping.get("role")
        template = mapping.get("template", mapping.get("content"))
        if role is None or template is None:
            raise ValueError("message mapping must include role and template/content")
        defaults = mapping.get("defaults")
        if defaults is not None and not hasattr(defaults, "items"):
            raise TypeError("message defaults must be a mapping or None")
        return MessageTemplate(
            role=str(role),
            template=str(template),
            defaults={} if defaults is None else dict(defaults.items()),
            name=None if mapping.get("name") is None else str(mapping["name"]),
        )
    raise TypeError(
        "chat messages must be MessageTemplate, PromptMessage, (role, template) pairs, or mappings"
    )


@dataclass(frozen=True)
class ChatPromptTemplate:
    messages: tuple[MessageTemplate, ...]
    name: str | None = None
    separator: str = "\n\n"

    @classmethod
    def from_messages(
        cls,
        messages: Sequence[Any],
        *,
        name: str | None = None,
        separator: str = "\n\n",
    ) -> "ChatPromptTemplate":
        return cls(
            tuple(_coerce_message_template(message) for message in messages),
            name=name,
            separator=separator,
        )

    @property
    def variables(self) -> tuple[str, ...]:
        seen: set[str] = set()
        ordered: list[str] = []
        for message in self.messages:
            for variable in message.variables:
                if variable not in seen:
                    seen.add(variable)
                    ordered.append(variable)
        return tuple(ordered)

    def partial(self, /, **defaults: Any) -> "ChatPromptTemplate":
        return ChatPromptTemplate(
            tuple(message.partial(**defaults) for message in self.messages),
            name=self.name,
            separator=self.separator,
        )

    def render(
        self,
        values: Mapping[str, Any] | None = None,
        /,
        **kwargs: Any,
    ) -> RenderedChatPrompt:
        rendered_messages = tuple(
            message.render(values, **kwargs)
            for message in self.messages
        )
        return RenderedChatPrompt(
            rendered_messages,
            name=self.name,
            separator=self.separator,
        )

    def format(
        self,
        values: Mapping[str, Any] | None = None,
        /,
        **kwargs: Any,
    ) -> str:
        return self.render(values, **kwargs).to_text()

    def as_messages(
        self,
        values: Mapping[str, Any] | None = None,
        /,
        **kwargs: Any,
    ) -> list[dict[str, str]]:
        return self.render(values, **kwargs).as_messages()

    def to_model_input(self, *, mode: str = "text") -> Any:
        return self.render().to_model_input(mode=mode)


def _coerce_prompt_value_for_model_input(value: Any) -> Any:
    if isinstance(value, (PromptTemplate, ChatPromptTemplate, RenderedPrompt, RenderedChatPrompt)):
        return value.to_model_input()
    return value

