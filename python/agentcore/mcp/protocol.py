from __future__ import annotations

import base64
import json
import re
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any
from urllib.parse import quote, unquote


JSONRPC_VERSION = "2.0"
MCP_PROTOCOL_VERSION = "2025-06-18"
MCP_LOG_LEVELS = (
    "debug",
    "info",
    "notice",
    "warning",
    "error",
    "critical",
    "alert",
    "emergency",
)
_MCP_LOG_LEVEL_INDEX = {level: index for index, level in enumerate(MCP_LOG_LEVELS)}


def _coerce_text(value: Any) -> str:
    if isinstance(value, str):
        return value
    if isinstance(value, (bytes, bytearray, memoryview)):
        return bytes(value).decode("utf-8", errors="replace")
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False, sort_keys=True)


def _ensure_mapping(value: Any, *, field_name: str) -> dict[str, Any]:
    if not isinstance(value, Mapping):
        raise TypeError(f"{field_name} must be a mapping")
    return dict(value.items())


def _optional_mapping(value: Any) -> dict[str, Any] | None:
    if value is None:
        return None
    return _ensure_mapping(value, field_name="mapping")


def _optional_sequence_of_strings(value: Any) -> list[str] | None:
    if value is None:
        return None
    if not isinstance(value, Sequence) or isinstance(value, (str, bytes, bytearray, memoryview)):
        raise TypeError("value must be a sequence of strings")
    return [str(item) for item in value]


def _encode_blob(value: bytes | bytearray | memoryview | str) -> str:
    if isinstance(value, str):
        raw = value.encode("utf-8")
    else:
        raw = bytes(value)
    return base64.b64encode(raw).decode("ascii")


def _decode_blob(value: str) -> bytes:
    return base64.b64decode(value.encode("ascii"))


def _normalize_annotations(value: Any) -> dict[str, Any] | None:
    if value is None:
        return None
    return _ensure_mapping(value, field_name="annotations")


def _content_block_from_value(value: Any) -> dict[str, Any]:
    return {"type": "text", "text": _coerce_text(value)}


def _normalize_resource_payload(
    value: Any,
    *,
    default_uri: str = "",
    default_name: str = "",
    default_mime_type: str = "",
) -> dict[str, Any]:
    if isinstance(value, Mapping):
        payload = dict(value.items())
    elif isinstance(value, str):
        payload = {"text": value}
    elif isinstance(value, (bytes, bytearray, memoryview)):
        payload = {"blob": _encode_blob(value)}
    else:
        payload = {"text": _coerce_text(value)}

    resource: dict[str, Any] = {
        "uri": str(payload.get("uri", default_uri)),
    }
    name = payload.get("name", default_name)
    if name:
        resource["name"] = str(name)
    mime_type = payload.get("mimeType", payload.get("mime_type", default_mime_type))
    if mime_type:
        resource["mimeType"] = str(mime_type)
    size = payload.get("size")
    if size is not None:
        resource["size"] = int(size)
    annotations = _normalize_annotations(payload.get("annotations"))
    if annotations is not None:
        resource["annotations"] = annotations
    description = payload.get("description")
    if description:
        resource["description"] = str(description)

    if "text" in payload and payload.get("text") is not None:
        resource["text"] = _coerce_text(payload.get("text"))
    if "blob" in payload and payload.get("blob") is not None:
        blob_value = payload.get("blob")
        if isinstance(blob_value, str):
            resource["blob"] = blob_value
        else:
            resource["blob"] = _encode_blob(blob_value)

    if "text" not in resource and "blob" not in resource:
        resource["text"] = _coerce_text(payload)
    return resource


def _normalize_prompt_content_block(value: Any) -> dict[str, Any]:
    if isinstance(value, str):
        return {"type": "text", "text": value}
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {"type": "text", "text": bytes(value).decode("utf-8", errors="replace")}
    if not isinstance(value, Mapping):
        return {"type": "text", "text": _coerce_text(value)}

    block = dict(value.items())
    block_type = str(block.get("type", "text"))
    normalized: dict[str, Any] = {"type": block_type}
    if block_type == "text":
        normalized["text"] = _coerce_text(block.get("text", ""))
        return normalized
    if block_type == "resource":
        normalized["resource"] = _normalize_resource_payload(block.get("resource", {}))
        return normalized
    if block_type == "image":
        normalized["data"] = str(block.get("data", ""))
        mime_type = block.get("mimeType", block.get("mime_type"))
        if mime_type:
            normalized["mimeType"] = str(mime_type)
        return normalized
    if block_type == "audio":
        normalized["data"] = str(block.get("data", ""))
        mime_type = block.get("mimeType", block.get("mime_type"))
        if mime_type:
            normalized["mimeType"] = str(mime_type)
        return normalized
    return block


def normalize_prompt_message(value: Any) -> dict[str, Any]:
    if isinstance(value, Mapping):
        payload = dict(value.items())
    else:
        payload = {"role": "user", "content": value}
    role = str(payload.get("role", "user"))
    content = payload.get("content", "")
    return {
        "role": role,
        "content": _normalize_prompt_content_block(content),
    }


def coerce_mcp_prompt_result(
    value: Any,
    *,
    default_name: str | None = None,
    default_description: str | None = None,
) -> dict[str, Any]:
    if isinstance(value, Mapping) and ("messages" in value or "description" in value):
        payload = dict(value.items())
        messages = payload.get("messages", [])
        if not isinstance(messages, Sequence):
            raise TypeError("MCP prompt messages must be a sequence")
        result: dict[str, Any] = {
            "messages": [normalize_prompt_message(message) for message in messages],
        }
        description = payload.get("description", default_description)
        if description:
            result["description"] = str(description)
        if payload.get("name", default_name):
            result["name"] = str(payload.get("name", default_name))
        return result

    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray, memoryview)):
        return {
            "messages": [normalize_prompt_message(message) for message in value],
            **({"description": str(default_description)} if default_description else {}),
            **({"name": str(default_name)} if default_name else {}),
        }

    return {
        "messages": [normalize_prompt_message(value)],
        **({"description": str(default_description)} if default_description else {}),
        **({"name": str(default_name)} if default_name else {}),
    }


def normalize_prompt_result_to_text(
    value: Any,
    *,
    include_roles: bool = True,
    separator: str = "\n\n",
) -> str:
    result = coerce_mcp_prompt_result(value)
    parts: list[str] = []
    for message in result["messages"]:
        block = message["content"]
        rendered = _prompt_block_to_text(block)
        if include_roles:
            parts.append(f"{str(message['role']).upper()}:\n{rendered}")
        else:
            parts.append(rendered)
    return separator.join(parts)


def normalize_mcp_root(value: Any) -> dict[str, Any]:
    if isinstance(value, Mapping):
        payload = dict(value.items())
        uri = payload.get("uri")
        if uri is None:
            raise TypeError("MCP root mapping must contain uri")
        normalized = {"uri": _coerce_root_uri(uri)}
        name = payload.get("name")
        if name:
            normalized["name"] = str(name)
        return normalized

    return {"uri": _coerce_root_uri(value)}


def normalize_mcp_roots(value: Any) -> list[dict[str, Any]]:
    if value is None:
        return []
    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray, memoryview)):
        return [normalize_mcp_root(item) for item in value]
    return [normalize_mcp_root(value)]


def _coerce_root_uri(value: Any) -> str:
    if isinstance(value, Path):
        return value.expanduser().resolve().as_uri()
    text = str(value)
    if text.startswith("file://"):
        return text
    return Path(text).expanduser().resolve().as_uri()


def _prompt_block_to_text(block: Mapping[str, Any]) -> str:
    block_type = str(block.get("type", "text"))
    if block_type == "text":
        return _coerce_text(block.get("text", ""))
    if block_type == "resource":
        resource = _ensure_mapping(block.get("resource", {}), field_name="resource")
        uri = str(resource.get("uri", ""))
        mime_type = str(resource.get("mimeType", resource.get("mime_type", "")))
        prefix = f"[RESOURCE uri={uri}"
        if mime_type:
            prefix += f" mimeType={mime_type}"
        prefix += "]"
        if resource.get("text") is not None:
            return f"{prefix}\n{_coerce_text(resource.get('text'))}"
        return prefix
    if block_type == "image":
        return f"[IMAGE mimeType={block.get('mimeType', '')}]"
    if block_type == "audio":
        return f"[AUDIO mimeType={block.get('mimeType', '')}]"
    return _coerce_text(block)


def normalize_sampling_message(value: Any) -> dict[str, Any]:
    return normalize_prompt_message(value)


def normalize_sampling_request(value: Mapping[str, Any]) -> dict[str, Any]:
    payload = _ensure_mapping(value, field_name="sampling request")
    messages = payload.get("messages")
    if not isinstance(messages, Sequence) or isinstance(messages, (str, bytes, bytearray, memoryview)):
        raise TypeError("sampling request messages must be a sequence")
    normalized: dict[str, Any] = {
        "messages": [normalize_sampling_message(message) for message in messages],
    }

    model_preferences = payload.get("modelPreferences", payload.get("model_preferences"))
    if model_preferences is not None:
        normalized["modelPreferences"] = _ensure_mapping(model_preferences, field_name="modelPreferences")

    system_prompt = payload.get("systemPrompt", payload.get("system_prompt"))
    if system_prompt is not None:
        normalized["systemPrompt"] = str(system_prompt)

    include_context = payload.get("includeContext", payload.get("include_context"))
    if include_context is not None:
        normalized["includeContext"] = str(include_context)

    temperature = payload.get("temperature")
    if temperature is not None:
        normalized["temperature"] = float(temperature)

    max_tokens = payload.get("maxTokens", payload.get("max_tokens"))
    if max_tokens is not None:
        normalized["maxTokens"] = int(max_tokens)

    stop_sequences = payload.get("stopSequences", payload.get("stop_sequences"))
    normalized_stop_sequences = _optional_sequence_of_strings(stop_sequences)
    if normalized_stop_sequences is not None:
        normalized["stopSequences"] = normalized_stop_sequences

    metadata = payload.get("metadata")
    if metadata is not None:
        normalized["metadata"] = _ensure_mapping(metadata, field_name="metadata")

    return normalized


def normalize_sampling_result(value: Any) -> dict[str, Any]:
    if isinstance(value, Mapping):
        payload = dict(value.items())
        if "content" in payload:
            content = _normalize_prompt_content_block(payload.get("content"))
        else:
            content = _normalize_prompt_content_block(payload.get("text", ""))
        normalized: dict[str, Any] = {
            "role": str(payload.get("role", "assistant")),
            "content": content,
        }
        if payload.get("model") is not None:
            normalized["model"] = str(payload.get("model"))
        if payload.get("stopReason", payload.get("stop_reason")) is not None:
            normalized["stopReason"] = str(payload.get("stopReason", payload.get("stop_reason")))
        return normalized

    return {
        "role": "assistant",
        "content": _normalize_prompt_content_block(value),
    }


def sampling_result_to_text(value: Any) -> str:
    result = normalize_sampling_result(value)
    return _prompt_block_to_text(_ensure_mapping(result.get("content", {}), field_name="content"))


def normalize_elicitation_result(value: Any) -> dict[str, Any]:
    if value is None:
        return {"action": "cancel"}

    if isinstance(value, str):
        action = str(value).strip().lower()
        if action not in {"accept", "decline", "cancel"}:
            raise TypeError("elicitation string results must be accept, decline, or cancel")
        return {"action": action}

    if not isinstance(value, Mapping):
        raise TypeError("elicitation result must be a mapping, string, or None")

    payload = dict(value.items())
    action = str(payload.get("action", "accept")).strip().lower()
    if action not in {"accept", "decline", "cancel"}:
        raise ValueError("elicitation action must be accept, decline, or cancel")
    normalized: dict[str, Any] = {"action": action}
    content = payload.get("content")
    if action == "accept":
        if content is None:
            content = {
                key: item
                for key, item in payload.items()
                if key != "action"
            }
        if content is not None:
            normalized["content"] = (
                _ensure_mapping(content, field_name="elicitation content")
                if isinstance(content, Mapping)
                else content
            )
    return normalized


def normalize_log_level(value: Any) -> str:
    level = str(value).strip().lower()
    if level not in _MCP_LOG_LEVEL_INDEX:
        raise ValueError(f"unsupported MCP log level: {value!r}")
    return level


def log_level_enabled(minimum_level: str | None, message_level: str) -> bool:
    normalized_message = normalize_log_level(message_level)
    if minimum_level is None:
        return True
    normalized_minimum = normalize_log_level(minimum_level)
    return _MCP_LOG_LEVEL_INDEX[normalized_message] >= _MCP_LOG_LEVEL_INDEX[normalized_minimum]


def _coerce_content_blocks(content: Any) -> list[dict[str, Any]]:
    if content is None:
        return []
    if isinstance(content, str):
        return [{"type": "text", "text": content}]
    if isinstance(content, (bytes, bytearray, memoryview)):
        return [{"type": "text", "text": bytes(content).decode("utf-8", errors="replace")}]
    if not isinstance(content, Sequence):
        raise TypeError("MCP tool result content must be a sequence of content blocks")

    blocks: list[dict[str, Any]] = []
    for block in content:
        if not isinstance(block, Mapping):
            raise TypeError("MCP tool result content blocks must be mappings")
        normalized = dict(block.items())
        block_type = str(normalized.get("type", "text"))
        normalized["type"] = block_type
        if block_type == "text":
            normalized["text"] = _coerce_text(normalized.get("text", ""))
        elif block_type == "resource":
            normalized["resource"] = _normalize_resource_payload(normalized.get("resource", {}))
        blocks.append(normalized)
    return blocks


def looks_like_mcp_tool_result(value: Any) -> bool:
    if not isinstance(value, Mapping):
        return False
    return (
        "content" in value or
        "structuredContent" in value or
        "structured_content" in value or
        "isError" in value or
        "is_error" in value
    )


def coerce_mcp_tool_result(value: Any) -> dict[str, Any]:
    if looks_like_mcp_tool_result(value):
        payload = dict(value.items())
        structured_key = "structuredContent" if "structuredContent" in payload else "structured_content"
        result: dict[str, Any] = {
            "isError": bool(payload.get("isError", payload.get("is_error", False))),
        }
        if structured_key in payload:
            result["structuredContent"] = payload.get(structured_key)
        content = payload.get("content")
        if content is None:
            if "structuredContent" in result:
                content = [_content_block_from_value(result["structuredContent"])]
            elif "text" in payload:
                content = [{"type": "text", "text": _coerce_text(payload.get("text", ""))}]
            else:
                content = []
        result["content"] = _coerce_content_blocks(content)
        return result

    if isinstance(value, str):
        return {
            "content": [{"type": "text", "text": value}],
            "isError": False,
        }

    if isinstance(value, (bytes, bytearray, memoryview)):
        return {
            "content": [{"type": "text", "text": bytes(value).decode("utf-8", errors="replace")}],
            "isError": False,
        }

    result = {
        "content": [_content_block_from_value(value)],
        "isError": False,
    }
    if value is not None:
        result["structuredContent"] = value
    return result


def normalize_mcp_tool_result(value: Any, *, mode: str = "auto") -> Any:
    normalized_mode = str(mode).strip().lower()
    if normalized_mode not in {"auto", "raw"}:
        raise ValueError("mode must be 'auto' or 'raw'")

    result = coerce_mcp_tool_result(value)
    if normalized_mode == "raw":
        return result

    if result.get("isError", False):
        return result

    structured = result.get("structuredContent")
    if structured is not None:
        return structured

    content = result.get("content", [])
    if isinstance(content, Sequence):
        text_blocks: list[str] = []
        for block in content:
            if not isinstance(block, Mapping) or str(block.get("type", "")) != "text":
                text_blocks = []
                break
            text_blocks.append(str(block.get("text", "")))
        if text_blocks:
            return "\n".join(text_blocks)

    return result


def tool_descriptor(
    name: str,
    *,
    description: str | None = None,
    input_schema: Mapping[str, Any] | None = None,
    title: str | None = None,
    annotations: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    descriptor: dict[str, Any] = {
        "name": str(name),
        "inputSchema": dict(input_schema.items()) if input_schema is not None else {
            "type": "object",
            "additionalProperties": True,
        },
    }
    if description:
        descriptor["description"] = str(description)
    if title:
        descriptor["title"] = str(title)
    normalized_annotations = _normalize_annotations(annotations)
    if normalized_annotations is not None:
        descriptor["annotations"] = normalized_annotations
    return descriptor


def prompt_descriptor(
    name: str,
    *,
    description: str | None = None,
    arguments: Sequence[Mapping[str, Any]] | None = None,
    title: str | None = None,
) -> dict[str, Any]:
    descriptor: dict[str, Any] = {
        "name": str(name),
    }
    if description:
        descriptor["description"] = str(description)
    if title:
        descriptor["title"] = str(title)
    if arguments is not None:
        descriptor["arguments"] = [normalize_prompt_argument(argument) for argument in arguments]
    return descriptor


def normalize_prompt_argument(value: Mapping[str, Any]) -> dict[str, Any]:
    payload = _ensure_mapping(value, field_name="prompt argument")
    normalized = {
        "name": str(payload["name"]),
        "required": bool(payload.get("required", False)),
    }
    description = payload.get("description")
    if description:
        normalized["description"] = str(description)
    title = payload.get("title")
    if title:
        normalized["title"] = str(title)
    return normalized


def resource_descriptor(
    uri: str,
    *,
    name: str | None = None,
    description: str | None = None,
    mime_type: str | None = None,
    size: int | None = None,
    annotations: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    descriptor: dict[str, Any] = {
        "uri": str(uri),
    }
    if name:
        descriptor["name"] = str(name)
    if description:
        descriptor["description"] = str(description)
    if mime_type:
        descriptor["mimeType"] = str(mime_type)
    if size is not None:
        descriptor["size"] = int(size)
    normalized_annotations = _normalize_annotations(annotations)
    if normalized_annotations is not None:
        descriptor["annotations"] = normalized_annotations
    return descriptor


def resource_template_descriptor(
    uri_template: str,
    *,
    name: str | None = None,
    description: str | None = None,
    mime_type: str | None = None,
    annotations: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    descriptor: dict[str, Any] = {
        "uriTemplate": str(uri_template),
    }
    if name:
        descriptor["name"] = str(name)
    if description:
        descriptor["description"] = str(description)
    if mime_type:
        descriptor["mimeType"] = str(mime_type)
    normalized_annotations = _normalize_annotations(annotations)
    if normalized_annotations is not None:
        descriptor["annotations"] = normalized_annotations
    return descriptor


def coerce_mcp_resource_result(
    value: Any,
    *,
    default_uri: str = "",
    default_name: str = "",
    default_mime_type: str = "",
) -> dict[str, Any]:
    if isinstance(value, Mapping) and "contents" in value:
        payload = dict(value.items())
        contents = payload.get("contents", [])
    else:
        contents = value

    if isinstance(contents, Sequence) and not isinstance(contents, (str, bytes, bytearray, memoryview)):
        normalized_contents = [
            _normalize_resource_payload(
                item,
                default_uri=default_uri,
                default_name=default_name,
                default_mime_type=default_mime_type,
            )
            for item in contents
        ]
    else:
        normalized_contents = [
            _normalize_resource_payload(
                contents,
                default_uri=default_uri,
                default_name=default_name,
                default_mime_type=default_mime_type,
            )
        ]
    return {"contents": normalized_contents}


def normalize_mcp_resource_result(value: Any, *, decode: str = "auto") -> Any:
    normalized_decode = str(decode).strip().lower()
    if normalized_decode not in {"auto", "text", "bytes", "json", "entries", "raw"}:
        raise ValueError("decode must be 'auto', 'text', 'bytes', 'json', 'entries', or 'raw'")

    result = coerce_mcp_resource_result(value)
    if normalized_decode in {"entries", "raw"}:
        return result if normalized_decode == "raw" else list(result["contents"])

    entries = list(result["contents"])
    if len(entries) != 1:
        return entries

    entry = entries[0]
    if "text" in entry:
        text = str(entry["text"])
        if normalized_decode == "text":
            return text
        if normalized_decode == "json":
            return json.loads(text)
        if normalized_decode == "bytes":
            return text.encode("utf-8")
        if normalized_decode == "auto":
            try:
                return json.loads(text)
            except json.JSONDecodeError:
                return text
        return entry

    blob = _decode_blob(str(entry.get("blob", "")))
    if normalized_decode == "text":
        return blob.decode("utf-8")
    if normalized_decode == "json":
        return json.loads(blob.decode("utf-8"))
    if normalized_decode in {"bytes", "auto"}:
        return blob
    return entry


def normalize_completion_result(value: Any) -> dict[str, Any]:
    if isinstance(value, Mapping) and "completion" in value:
        payload = dict(value.items())
        completion = _ensure_mapping(payload["completion"], field_name="completion")
        values = completion.get("values", [])
        if not isinstance(values, Sequence):
            raise TypeError("completion values must be a sequence")
        normalized = {
            "completion": {
                "values": [str(item) for item in values],
                "total": int(completion.get("total", len(values))),
                "hasMore": bool(completion.get("hasMore", False)),
            }
        }
        return normalized

    if isinstance(value, Sequence) and not isinstance(value, (str, bytes, bytearray, memoryview)):
        values = [str(item) for item in value]
        return {
            "completion": {
                "values": values,
                "total": len(values),
                "hasMore": False,
            }
        }

    text = str(value)
    return {
        "completion": {
            "values": [text],
            "total": 1,
            "hasMore": False,
        }
    }


def completion_values(value: Any) -> list[str]:
    return list(normalize_completion_result(value)["completion"]["values"])


def parse_uri_template_variables(uri_template: str) -> tuple[str, ...]:
    variables: list[str] = []
    seen: set[str] = set()
    for match in re.finditer(r"\{([A-Za-z0-9_]+)\}", str(uri_template)):
        name = match.group(1)
        if name not in seen:
            seen.add(name)
            variables.append(name)
    return tuple(variables)


def render_uri_template(uri_template: str, arguments: Mapping[str, Any] | None = None) -> str:
    rendered = str(uri_template)
    values = {} if arguments is None else {str(key): value for key, value in arguments.items()}
    for variable in parse_uri_template_variables(uri_template):
        if variable not in values:
            raise KeyError(f"missing URI template value for: {variable}")
        rendered = rendered.replace("{" + variable + "}", quote(str(values[variable]), safe=""))
    return rendered


def match_uri_template(uri_template: str, uri: str) -> dict[str, str] | None:
    template = str(uri_template)
    pattern_parts: list[str] = []
    cursor = 0
    variables = parse_uri_template_variables(template)
    for variable in variables:
        token = "{" + variable + "}"
        index = template.find(token, cursor)
        if index < 0:
            return None
        pattern_parts.append(re.escape(template[cursor:index]))
        pattern_parts.append(f"(?P<{variable}>[^/?#]+)")
        cursor = index + len(token)
    pattern_parts.append(re.escape(template[cursor:]))
    pattern = "^" + "".join(pattern_parts) + "$"
    match = re.match(pattern, str(uri))
    if match is None:
        return None
    return {key: unquote(value) for key, value in match.groupdict().items()}
