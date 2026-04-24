from __future__ import annotations

import argparse
import importlib
import importlib.util
import json
import os
import sys
from pathlib import Path
from typing import Any

from .server import MCPServer


def parse_target_spec(target: str) -> tuple[str, str | None]:
    text = str(target).strip()
    if not text:
        raise ValueError("target must be a non-empty module-or-file reference")
    if ":" in text:
        source, attribute = text.split(":", 1)
        source = source.strip()
        attribute = attribute.strip() or None
        if not source:
            raise ValueError("target source must not be empty")
        return source, attribute
    return text, None


def load_target_object(target: str) -> Any:
    source, attribute = parse_target_spec(target)
    module = _load_module_source(source)
    if attribute is None:
        for candidate in ("server", "build_server", "create_server", "app"):
            if hasattr(module, candidate):
                return getattr(module, candidate)
        return module
    value: Any = module
    for part in attribute.split("."):
        part_name = part.strip()
        if not part_name:
            raise ValueError(f"invalid empty attribute segment in target {target!r}")
        value = getattr(value, part_name)
    return value


def resolve_server_target(
    target: str,
    *,
    name: str | None = None,
    version: str | None = None,
    instructions: str | None = None,
) -> MCPServer:
    value = load_target_object(target)
    if callable(value) and not isinstance(value, MCPServer):
        value = value()

    if isinstance(value, MCPServer):
        return value

    if _looks_like_compiled_graph(value):
        return MCPServer.from_compiled_graph(
            value,
            name="agentcore-mcp" if not name else str(name),
            version="0.1" if version is None else str(version),
        )

    if _looks_like_state_graph(value):
        compiled = value.compile()
        return MCPServer.from_compiled_graph(
            compiled,
            name="agentcore-mcp" if not name else str(name),
            version="0.1" if version is None else str(version),
        )

    if _looks_like_tool_registry(value):
        return MCPServer.from_tool_registry(
            value,
            name="agentcore-mcp" if not name else str(name),
            version="0.1" if version is None else str(version),
        )

    if hasattr(value, "run_stdio") and callable(value.run_stdio):
        if hasattr(value, "serve_stdio") and callable(value.serve_stdio):
            return value

    raise TypeError(
        "target did not resolve to an MCPServer, buildable server factory, StateGraph, "
        "CompiledStateGraph, or tool registry"
    )


def run_stdio_server(
    target: str,
    *,
    name: str | None = None,
    version: str | None = None,
    instructions: str | None = None,
) -> None:
    server = resolve_server_target(
        target,
        name=name,
        version=version,
        instructions=instructions,
    )
    if instructions and isinstance(server, MCPServer) and not getattr(server, "_instructions", ""):
        server._instructions = str(instructions)
    server.run_stdio()


def render_client_config(
    client: str,
    *,
    server_name: str,
    target: str,
    python_executable: str | None = None,
    env: dict[str, str] | None = None,
    cwd: str | None = None,
    gemini_scope: str = "project",
) -> str:
    normalized_client = str(client).strip().lower()
    if normalized_client not in {"claude", "claude-code", "claude-desktop", "codex", "gemini"}:
        raise ValueError("client must be one of: claude, claude-code, claude-desktop, codex, gemini")

    command = str(python_executable or sys.executable)
    args = ["-m", "agentcore.mcp", "serve", "--target", target]
    normalized_env = {} if env is None else {str(key): str(value) for key, value in env.items()}
    normalized_cwd = None if cwd is None else str(cwd)

    if normalized_client == "claude":
        return "\n\n".join([
            render_client_config(
                "claude-code",
                server_name=server_name,
                target=target,
                python_executable=command,
                env=normalized_env,
                cwd=normalized_cwd,
            ),
            render_client_config(
                "claude-desktop",
                server_name=server_name,
                target=target,
                python_executable=command,
                env=normalized_env,
                cwd=normalized_cwd,
            ),
        ])

    if normalized_client == "claude-code":
        payload = {
            "type": "stdio",
            "command": command,
            "args": args,
            "env": normalized_env,
        }
        sections = [
            "Claude Code CLI",
            _shell_block(
                _render_claude_code_cli_command(
                    server_name,
                    payload,
                )
            ),
            "Claude Code .mcp.json",
            _json_block(
                {
                    "mcpServers": {
                        server_name: _drop_empty({
                            "command": command,
                            "args": args,
                            "env": normalized_env,
                        })
                    }
                }
            ),
        ]
        return "\n".join(sections)

    if normalized_client == "claude-desktop":
        payload = {
            "mcpServers": {
                server_name: _drop_empty({
                    "type": "stdio",
                    "command": command,
                    "args": args,
                    "env": normalized_env,
                    "cwd": normalized_cwd,
                })
            }
        }
        return "\n".join([
            "Claude Desktop config",
            _json_block(payload),
        ])

    if normalized_client == "codex":
        sections = [
            "Codex CLI",
            _shell_block(
                _render_codex_cli_command(
                    server_name,
                    command,
                    args,
                    normalized_env,
                )
            ),
            "Codex config.toml",
            _toml_block(
                _render_codex_toml(
                    server_name,
                    command,
                    args,
                    normalized_env,
                    normalized_cwd,
                )
            ),
        ]
        return "\n".join(sections)

    sections = [
        "Gemini CLI",
        _shell_block(
            _render_gemini_cli_command(
                server_name,
                command,
                args,
                normalized_env,
                gemini_scope,
            )
        ),
        "Gemini settings.json",
        _json_block(
            {
                "mcpServers": {
                    server_name: _drop_empty({
                        "command": command,
                        "args": args,
                        "env": normalized_env,
                        "cwd": normalized_cwd,
                    })
                }
            }
        ),
    ]
    return "\n".join(sections)


def serve_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="agentcore-mcp-server",
        description="Launch an AgentCore MCP server from a module or Python file.",
    )
    parser.add_argument(
        "--target",
        required=True,
        help="Module/file target such as package.module:build_server or ./server.py:server",
    )
    parser.add_argument("--name", default=None, help="Override generated server name for graph/tool-registry targets")
    parser.add_argument("--version", default=None, help="Override generated server version for graph/tool-registry targets")
    parser.add_argument("--instructions", default=None, help="Optional instructions string for generated server wrappers")
    args = parser.parse_args(argv)
    run_stdio_server(
        args.target,
        name=args.name,
        version=args.version,
        instructions=args.instructions,
    )
    return 0


def config_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="agentcore-mcp-config",
        description="Render Claude, Codex, or Gemini MCP configuration for an AgentCore server target.",
    )
    parser.add_argument("client", choices=["claude", "claude-code", "claude-desktop", "codex", "gemini"])
    parser.add_argument("--name", required=True, help="Configured MCP server name")
    parser.add_argument(
        "--target",
        required=True,
        help="Module/file target such as package.module:build_server or ./server.py:server",
    )
    parser.add_argument("--python", default=sys.executable, help="Python executable to embed in the generated config")
    parser.add_argument("--cwd", default=None, help="Optional working directory to include in generated config where supported")
    parser.add_argument(
        "--env",
        action="append",
        default=[],
        metavar="KEY=VALUE",
        help="Environment variables to embed in the generated config",
    )
    parser.add_argument(
        "--gemini-scope",
        choices=["project", "user"],
        default="project",
        help="Gemini CLI scope used in the generated add command",
    )
    args = parser.parse_args(argv)
    env = _parse_env_assignments(args.env)
    print(
        render_client_config(
            args.client,
            server_name=args.name,
            target=args.target,
            python_executable=args.python,
            env=env,
            cwd=args.cwd,
            gemini_scope=args.gemini_scope,
        )
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    arguments = list(sys.argv[1:] if argv is None else argv)
    if not arguments:
        return serve_main([])
    command = arguments[0]
    if command == "serve":
        return serve_main(arguments[1:])
    if command == "config":
        return config_main(arguments[1:])
    return serve_main(arguments)


def _load_module_source(source: str) -> Any:
    if source.endswith(".py") or os.path.sep in source or (os.path.altsep and os.path.altsep in source):
        path = Path(source).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(f"target file not found: {path}")
        module_name = f"agentcore_user_mcp_{abs(hash(str(path)))}"
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            raise ImportError(f"could not load Python module from {path}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)
        return module
    return importlib.import_module(source)


def _looks_like_tool_registry(value: Any) -> bool:
    return (
        hasattr(value, "list") and callable(value.list) and
        hasattr(value, "invoke_with_metadata") and callable(value.invoke_with_metadata)
    )


def _looks_like_compiled_graph(value: Any) -> bool:
    return hasattr(value, "tools") and _looks_like_tool_registry(getattr(value, "tools", None))


def _looks_like_state_graph(value: Any) -> bool:
    return hasattr(value, "compile") and callable(value.compile)


def _parse_env_assignments(values: list[str]) -> dict[str, str]:
    env: dict[str, str] = {}
    for item in values:
        if "=" not in item:
            raise ValueError(f"environment assignment must be KEY=VALUE, got {item!r}")
        key, raw_value = item.split("=", 1)
        key = key.strip()
        if not key:
            raise ValueError(f"environment variable name must not be empty: {item!r}")
        env[key] = raw_value
    return env


def _render_claude_code_cli_command(
    server_name: str,
    payload: dict[str, Any],
) -> str:
    json_payload = json.dumps(payload, separators=(",", ":"), ensure_ascii=False)
    return f"claude mcp add-json {server_name} '{json_payload}'"


def _render_codex_cli_command(
    server_name: str,
    command: str,
    args: list[str],
    env: dict[str, str],
) -> str:
    parts = ["codex", "mcp", "add", server_name]
    for key, value in env.items():
        parts.extend(["--env", f"{key}={value}"])
    parts.append("--")
    parts.append(command)
    parts.extend(args)
    return " ".join(_shell_quote(part) for part in parts)


def _render_gemini_cli_command(
    server_name: str,
    command: str,
    args: list[str],
    env: dict[str, str],
    scope: str,
) -> str:
    parts = ["gemini", "mcp", "add", "--scope", scope]
    for key, value in env.items():
        parts.extend(["-e", f"{key}={value}"])
    parts.extend([server_name, command])
    parts.extend(args)
    return " ".join(_shell_quote(part) for part in parts)


def _render_codex_toml(
    server_name: str,
    command: str,
    args: list[str],
    env: dict[str, str],
    cwd: str | None,
) -> str:
    server_key = json.dumps(server_name)
    lines = [f"[mcp_servers.{server_key}]"]
    lines.append(f'command = {json.dumps(command)}')
    if args:
        lines.append(f"args = {json.dumps(args)}")
    if cwd:
        lines.append(f"cwd = {json.dumps(cwd)}")
    if env:
        lines.append("")
        lines.append(f"[mcp_servers.{server_key}.env]")
        for key, value in env.items():
            lines.append(f"{key} = {json.dumps(value)}")
    return "\n".join(lines)


def _drop_empty(payload: dict[str, Any]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in payload.items():
        if value in (None, "", {}, []):
            continue
        result[key] = value
    return result


def _json_block(payload: dict[str, Any]) -> str:
    return "```json\n" + json.dumps(payload, indent=2, ensure_ascii=False) + "\n```"


def _toml_block(text: str) -> str:
    return "```toml\n" + text + "\n```"


def _shell_block(text: str) -> str:
    return "```bash\n" + text + "\n```"


def _shell_quote(value: str) -> str:
    if not value:
        return "''"
    if all(character.isalnum() or character in "@%_+=:,./-" for character in value):
        return value
    return "'" + value.replace("'", "'\"'\"'") + "'"
