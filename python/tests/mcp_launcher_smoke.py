from __future__ import annotations

import os
import subprocess
import sys

from agentcore.mcp import StdioMCPClient, load_target_object, render_client_config, resolve_server_target


FIXTURE = os.path.join(
    os.path.dirname(__file__),
    "fixtures",
    "mcp_installable_server.py",
)
TARGET = FIXTURE + ":build_server"


loaded = load_target_object(TARGET)
assert callable(loaded)

server = resolve_server_target(TARGET)
assert server.name == "agentcore-installable-fixture"

claude_config = render_client_config(
    "claude",
    server_name="agentcore-fixture",
    target=TARGET,
    python_executable=sys.executable,
)
assert "claude mcp add-json agentcore-fixture" in claude_config
assert '"mcpServers"' in claude_config
assert "agentcore.mcp" in claude_config

codex_config = render_client_config(
    "codex",
    server_name="agentcore-fixture",
    target=TARGET,
    python_executable=sys.executable,
    env={"AGENTCORE_MODE": "test"},
)
assert "codex mcp add agentcore-fixture" in codex_config
assert '[mcp_servers."agentcore-fixture"]' in codex_config
assert "AGENTCORE_MODE" in codex_config

gemini_config = render_client_config(
    "gemini",
    server_name="agentcore-fixture",
    target=TARGET,
    python_executable=sys.executable,
)
assert "gemini mcp add --scope project agentcore-fixture" in gemini_config
assert '"mcpServers"' in gemini_config

launcher_command = [
    sys.executable,
    "-m",
    "agentcore.mcp",
    "serve",
    "--target",
    TARGET,
]

with StdioMCPClient(launcher_command, startup_timeout=10.0, request_timeout=10.0) as client:
    assert client.server_info["name"] == "agentcore-installable-fixture"
    tools = client.list_tools()
    assert [tool["name"] for tool in tools] == ["echo_text"]
    assert client.call_tool("echo_text", {"text": "installed"}) == {"echo": "installed"}


config_process = subprocess.run(
    [
        sys.executable,
        "-m",
        "agentcore.mcp",
        "config",
        "codex",
        "--name",
        "agentcore-fixture",
        "--target",
        TARGET,
    ],
    check=True,
    text=True,
    capture_output=True,
)
assert "codex mcp add agentcore-fixture" in config_process.stdout
assert "agentcore.mcp" in config_process.stdout
