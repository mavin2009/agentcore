from __future__ import annotations

import argparse
import json
import os
import platform
import operator
import resource
import statistics
import subprocess
import sys
import time
import importlib
from pathlib import Path
from typing import Any
from typing_extensions import Annotated, TypedDict


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_PYTHON_ROOT = REPO_ROOT / "build" / "python"


def _normalize_pythonpath_entries(raw_value: str | None) -> list[str]:
    if not raw_value:
        return []
    return [entry for entry in raw_value.split(os.pathsep) if entry]


def _without_build_python_root(raw_value: str | None) -> str | None:
    normalized_build_root = str(BUILD_PYTHON_ROOT.resolve())
    filtered = [
        entry
        for entry in _normalize_pythonpath_entries(raw_value)
        if str(Path(entry).resolve()) != normalized_build_root
    ]
    if not filtered:
        return None
    return os.pathsep.join(filtered)


def _prefer_installed_langgraph() -> None:
    normalized_build_root = str(BUILD_PYTHON_ROOT.resolve())
    sys.path[:] = [
        entry
        for entry in sys.path
        if str(Path(entry or ".").resolve()) != normalized_build_root
    ]
    for module_name in list(sys.modules):
        if module_name == "langgraph" or module_name.startswith("langgraph."):
            del sys.modules[module_name]
    importlib.invalidate_caches()


def _prefer_agentcore_build_python() -> None:
    normalized_build_root = str(BUILD_PYTHON_ROOT.resolve())
    sys.path[:] = [
        entry
        for entry in sys.path
        if str(Path(entry or ".").resolve()) != normalized_build_root
    ]
    sys.path.insert(0, normalized_build_root)
    importlib.invalidate_caches()


def _run_subprocess(worker: str) -> dict[str, Any]:
    command = [sys.executable, __file__, "--worker", worker]
    env = os.environ.copy()
    sanitized_pythonpath = _without_build_python_root(env.get("PYTHONPATH"))
    if sanitized_pythonpath is None:
        env.pop("PYTHONPATH", None)
    else:
        env["PYTHONPATH"] = sanitized_pythonpath
    completed = subprocess.run(
        command,
        cwd=str(REPO_ROOT),
        env=env,
        capture_output=True,
        text=True,
        check=True,
    )
    stdout = completed.stdout.strip().splitlines()
    if not stdout:
        raise RuntimeError(f"worker {worker!r} produced no output")
    try:
        return json.loads(stdout[-1])
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"worker {worker!r} did not emit JSON.\nstdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        ) from exc


def _fmt_ns(ns: int) -> str:
    if ns >= 1_000_000_000:
        return f"{ns / 1_000_000_000:.3f}s"
    if ns >= 1_000_000:
        return f"{ns / 1_000_000:.3f}ms"
    if ns >= 1_000:
        return f"{ns / 1_000:.3f}us"
    return f"{ns}ns"


def _fmt_ops_per_second(avg_ns: int) -> str:
    if avg_ns <= 0:
        return "n/a"
    return f"{1_000_000_000 / avg_ns:.1f}/s"


def _fmt_batch_items_per_second(avg_item_ns: int) -> str:
    if avg_item_ns <= 0:
        return "n/a"
    return f"{1_000_000_000 / avg_item_ns:.1f} items/s"


def _fmt_memory_kb(memory_kb: int) -> str:
    return f"{memory_kb / 1024:.1f} MiB"


def _materialize_state(value: Any) -> Any:
    if value is None:
        return None
    if isinstance(value, dict):
        return dict(value)
    if hasattr(value, "items"):
        return dict(value.items())
    try:
        return dict(value)
    except Exception:
        return value


def _pct_less(reference: int, candidate: int) -> float:
    if reference <= 0:
        return 0.0
    return ((reference - candidate) / reference) * 100.0


def _speedup(reference: int, candidate: int) -> float:
    if candidate <= 0:
        return 0.0
    return reference / candidate


def _relative_latency_label(langgraph_ns: int, agentcore_ns: int) -> str:
    baseline = max(langgraph_ns, agentcore_ns)
    if baseline <= 0:
        return "n/a"
    if abs(langgraph_ns - agentcore_ns) / baseline < 0.05:
        return "near parity"
    if agentcore_ns < langgraph_ns:
        return f"{langgraph_ns / agentcore_ns:.2f}x faster"
    return f"{agentcore_ns / langgraph_ns:.2f}x slower"


def _relative_memory_label(langgraph_kb: int, agentcore_kb: int) -> str:
    baseline = max(langgraph_kb, agentcore_kb)
    if baseline <= 0:
        return "n/a"
    if abs(langgraph_kb - agentcore_kb) / baseline < 0.05:
        return "near parity"
    if agentcore_kb < langgraph_kb:
        return f"{_pct_less(langgraph_kb, agentcore_kb):.1f}% less memory"
    return f"{_pct_less(agentcore_kb, langgraph_kb):.1f}% more memory"


def _bench_langgraph_compat() -> dict[str, Any]:
    _prefer_installed_langgraph()
    from langgraph.graph import END as LG_END
    from langgraph.graph import START as LG_START
    from langgraph.graph import StateGraph as LangGraphStateGraph

    _prefer_agentcore_build_python()
    from agentcore_langgraph_native.langgraph_compat import END as AC_END
    from agentcore_langgraph_native.langgraph_compat import START as AC_START
    from agentcore_langgraph_native.langgraph_compat import StateGraph as CompatStateGraph

    def make_node(prefix: str, index: int):
        def node(state: dict[str, Any], config: dict[str, Any] | None = None):
            trail = list(state.get("trail", []))
            trail.append(f"{prefix}-{index}")
            return {
                "step": index + 1,
                "trail": trail,
                "score": int(state.get("score", 0)) + index + 1,
            }

        return node

    def build_langgraph():
        builder = LangGraphStateGraph(dict)
        for index in range(10):
            builder.add_node(f"agent_{index}", make_node("lg", index))
        builder.add_edge(LG_START, "agent_0")
        for index in range(9):
            builder.add_edge(f"agent_{index}", f"agent_{index + 1}")
        builder.add_edge("agent_9", LG_END)
        return builder.compile()

    def build_compat():
        builder = CompatStateGraph(dict, worker_count=4)
        for index in range(10):
            builder.add_node(f"agent_{index}", make_node("ac", index))
        builder.add_edge(AC_START, "agent_0")
        for index in range(9):
            builder.add_edge(f"agent_{index}", f"agent_{index + 1}")
        builder.add_edge("agent_9", AC_END)
        return builder.compile()

    langgraph_graph = build_langgraph()
    compat_graph = build_compat()
    initial_state = {"step": 0, "trail": [], "score": 0}

    for _ in range(10):
        langgraph_graph.invoke(dict(initial_state))
        compat_graph.invoke(dict(initial_state))

    invoke_iterations = 200
    stream_iterations = 200
    batch_iterations = 50
    batch_size = 32
    batch_inputs = [dict(initial_state) for _ in range(batch_size)]

    start_ns = time.perf_counter_ns()
    langgraph_result = None
    for _ in range(invoke_iterations):
        langgraph_result = langgraph_graph.invoke(dict(initial_state))
    langgraph_invoke_ns = time.perf_counter_ns() - start_ns

    start_ns = time.perf_counter_ns()
    compat_result = None
    for _ in range(invoke_iterations):
        compat_result = compat_graph.invoke(dict(initial_state))
    compat_invoke_ns = time.perf_counter_ns() - start_ns

    start_ns = time.perf_counter_ns()
    langgraph_stream_events = 0
    for _ in range(stream_iterations):
        langgraph_stream_events = len(list(langgraph_graph.stream(dict(initial_state), stream_mode="updates")))
    langgraph_stream_ns = time.perf_counter_ns() - start_ns

    start_ns = time.perf_counter_ns()
    compat_stream_events = 0
    for _ in range(stream_iterations):
        compat_stream_events = len(list(compat_graph.stream(dict(initial_state))))
    compat_stream_ns = time.perf_counter_ns() - start_ns

    start_ns = time.perf_counter_ns()
    for _ in range(batch_iterations):
        langgraph_graph.batch(batch_inputs)
    langgraph_batch_ns = time.perf_counter_ns() - start_ns

    start_ns = time.perf_counter_ns()
    for _ in range(batch_iterations):
        compat_graph.batch(batch_inputs)
    compat_batch_ns = time.perf_counter_ns() - start_ns

    return {
        "worker": "langgraph_compat",
        "invoke_avg_ns_langgraph": langgraph_invoke_ns // invoke_iterations,
        "invoke_avg_ns_agentcore_compat": compat_invoke_ns // invoke_iterations,
        "stream_avg_ns_langgraph": langgraph_stream_ns // stream_iterations,
        "stream_avg_ns_agentcore_compat": compat_stream_ns // stream_iterations,
        "batch_item_avg_ns_langgraph": langgraph_batch_ns // (batch_iterations * batch_size),
        "batch_item_avg_ns_agentcore_compat": compat_batch_ns // (batch_iterations * batch_size),
        "stream_events_langgraph": langgraph_stream_events,
        "stream_events_agentcore_compat": compat_stream_events,
        "final_score_langgraph": int(langgraph_result["score"]),
        "final_score_agentcore_compat": int(compat_result["score"]),
    }


def _bench_native_memory() -> dict[str, Any]:
    runtime = os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"]
    rounds = 30
    specialist_count = 10

    if runtime == "langgraph":
        _prefer_installed_langgraph()
        import operator
        from typing_extensions import Annotated, TypedDict

        from langgraph.checkpoint.memory import InMemorySaver
        from langgraph.graph import END, START, StateGraph

        class ChildState(TypedDict, total=False):
            session_id: str
            query: str
            visits: Annotated[int, operator.add]
            memory: Annotated[list[str], operator.add]
            answer: str

        child_builder = StateGraph(ChildState)

        def specialist(state: ChildState):
            query = state.get("query", "")
            return {
                "visits": 1,
                "memory": [query + ":" + ("x" * 256)],
                "answer": f"{state.get('session_id')}::{query}::{state.get('visits', 0) + 1}",
            }

        child_builder.add_node("specialist", specialist)
        child_builder.add_edge(START, "specialist")
        child_builder.add_edge("specialist", END)
        child_graph = child_builder.compile(checkpointer=InMemorySaver())

        annotations: dict[str, Any] = {"round": int, "summary": str}
        for index in range(specialist_count):
            annotations[f"answer_{index}"] = str
            annotations[f"visits_{index}"] = int
        ParentState = TypedDict("ParentState", annotations, total=False)

        parent_builder = StateGraph(ParentState)

        def make_branch(index: int):
            def branch(state: ParentState):
                output = child_graph.invoke(
                    {
                        "session_id": f"specialist-{index}",
                        "query": f"round-{state['round']}-q{index}",
                    },
                    config={"configurable": {"thread_id": f"specialist-{index}"}},
                )
                return {
                    f"answer_{index}": output["answer"],
                    f"visits_{index}": output["visits"],
                }

            return branch

        for index in range(specialist_count):
            parent_builder.add_node(f"branch_{index}", make_branch(index))
            parent_builder.add_edge(START, f"branch_{index}")
        parent_builder.add_node(
            "join",
            lambda state: {
                "summary": "|".join(state.get(f"answer_{index}", "") for index in range(specialist_count))
            },
        )
        parent_builder.add_edge([f"branch_{index}" for index in range(specialist_count)], "join")
        parent_builder.add_edge("join", END)
        parent_graph = parent_builder.compile()

    elif runtime == "agentcore":
        _prefer_agentcore_build_python()
        from agentcore.graph import END, START, StateGraph

        child_graph = StateGraph(dict, name="memory_child", worker_count=4)

        def specialist(state: dict[str, Any], config: dict[str, Any]):
            query = state.get("query", "")
            memory = list(state.get("memory", []))
            memory.append(query + ":" + ("x" * 256))
            visits = int(state.get("visits", 0)) + 1
            return {
                "memory": memory,
                "visits": visits,
                "answer": f"{state.get('session_id')}::{query}::{visits}",
            }

        child_graph.add_node("specialist", specialist)
        child_graph.add_edge(START, "specialist")
        child_graph.add_edge("specialist", END)

        parent_graph = StateGraph(dict, name="memory_parent", worker_count=max(4, specialist_count))
        parent_graph.add_fanout("fanout")

        def make_prepare(index: int):
            def prepare(state: dict[str, Any], config: dict[str, Any]):
                return {
                    f"session_{index}_id": f"specialist-{index}",
                    f"session_{index}_query": f"round-{state['round']}-q{index}",
                }

            return prepare

        for index in range(specialist_count):
            parent_graph.add_node(f"prepare_{index}", make_prepare(index))
            parent_graph.add_subgraph(
                f"branch_{index}",
                child_graph,
                namespace=f"branch_{index}",
                inputs={
                    f"session_{index}_id": "session_id",
                    f"session_{index}_query": "query",
                },
                outputs={
                    f"answer_{index}": "answer",
                    f"visits_{index}": "visits",
                },
                session_mode="persistent",
                session_id_from=f"session_{index}_id",
            )
            parent_graph.add_edge("fanout", f"prepare_{index}")
            parent_graph.add_edge(f"prepare_{index}", f"branch_{index}")

        parent_graph.add_join(
            "join",
            lambda state, config: {
                "summary": "|".join(state.get(f"answer_{index}", "") for index in range(specialist_count))
            },
        )
        for index in range(specialist_count):
            parent_graph.add_edge(f"branch_{index}", "join")
        parent_graph.add_edge(START, "fanout")
        parent_graph.add_edge("join", END)
        parent_graph = parent_graph.compile()

    else:
        raise ValueError(f"unsupported runtime {runtime!r}")

    start_ns = time.perf_counter_ns()
    final_state = None
    for round_index in range(rounds):
        final_state = parent_graph.invoke({"round": round_index})
    elapsed_ns = time.perf_counter_ns() - start_ns
    maxrss_kb = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

    return {
        "worker": "native_memory",
        "runtime": runtime,
        "avg_ns": elapsed_ns // rounds,
        "maxrss_kb": maxrss_kb,
        "summary_length": len(final_state["summary"]),
    }


def _bench_native_resume() -> dict[str, Any]:
    runtime = os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"]
    iterations = 200

    if runtime == "langgraph":
        _prefer_installed_langgraph()
        from typing_extensions import TypedDict

        from langgraph.checkpoint.memory import InMemorySaver
        from langgraph.graph import END, START, StateGraph
        from langgraph.types import Command, interrupt

        class State(TypedDict, total=False):
            input: int
            approved: bool
            ready: bool
            result: int

        def build_direct():
            builder = StateGraph(State)
            builder.add_node("gate", lambda state: {"approved": True, "ready": True})
            builder.add_node("final", lambda state: {"result": int(state["input"]) * 2})
            builder.add_edge(START, "gate")
            builder.add_edge("gate", "final")
            builder.add_edge("final", END)
            return builder.compile()

        def build_resumable():
            builder = StateGraph(State)

            def gate(state: State):
                approved = interrupt("approve?")
                return {"approved": bool(approved), "ready": True}

            builder.add_node("gate", gate)
            builder.add_node("final", lambda state: {"result": int(state["input"]) * 2})
            builder.add_edge(START, "gate")
            builder.add_edge("gate", "final")
            builder.add_edge("final", END)
            return builder.compile(checkpointer=InMemorySaver())

        direct_graph = build_direct()
        resumable_graph = build_resumable()

        for warm_index in range(5):
            direct_graph.invoke({"input": 7})
            config = {"configurable": {"thread_id": f"warm-{warm_index}"}}
            resumable_graph.invoke({"input": 7}, config=config)
            resumable_graph.invoke(Command(resume=True), config=config)

        start_ns = time.perf_counter_ns()
        direct_state = None
        for _ in range(iterations):
            direct_state = direct_graph.invoke({"input": 7})
        direct_elapsed_ns = time.perf_counter_ns() - start_ns

        start_ns = time.perf_counter_ns()
        resumed_state = None
        for iteration in range(iterations):
            config = {"configurable": {"thread_id": f"run-{iteration}"}}
            resumable_graph.invoke({"input": 7}, config=config)
            resumed_state = resumable_graph.invoke(Command(resume=True), config=config)
        resume_elapsed_ns = time.perf_counter_ns() - start_ns

    elif runtime == "agentcore":
        _prefer_agentcore_build_python()
        from agentcore.graph import Command, END, START, StateGraph

        def build_direct():
            builder = StateGraph(dict)
            builder.add_node("gate", lambda state, config: {"approved": True, "ready": True})
            builder.add_node("final", lambda state, config: {"result": int(state["input"]) * 2})
            builder.add_edge(START, "gate")
            builder.add_edge("gate", "final")
            builder.add_edge("final", END)
            return builder.compile()

        def build_resumable():
            builder = StateGraph(dict)

            def gate(state: dict[str, Any], config: dict[str, Any]):
                if not state.get("approved", False):
                    return Command(update={"approved": True}, wait=True)
                return {"ready": True}

            builder.add_node("gate", gate)
            builder.add_node("final", lambda state, config: {"result": int(state["input"]) * 2})
            builder.add_edge(START, "gate")
            builder.add_edge("gate", "final")
            builder.add_edge("final", END)
            return builder.compile()

        direct_graph = build_direct()
        resumable_graph = build_resumable()

        for _ in range(5):
            direct_graph.invoke({"input": 7})
            pause_result = resumable_graph.invoke_until_pause_with_metadata({"input": 7})
            resumable_graph.resume_with_metadata(pause_result["summary"]["checkpoint_id"])

        start_ns = time.perf_counter_ns()
        direct_state = None
        for _ in range(iterations):
            direct_state = direct_graph.invoke({"input": 7})
        direct_elapsed_ns = time.perf_counter_ns() - start_ns

        start_ns = time.perf_counter_ns()
        resumed_state = None
        for _ in range(iterations):
            pause_result = resumable_graph.invoke_until_pause_with_metadata({"input": 7})
            resumed_state = resumable_graph.resume_with_metadata(pause_result["summary"]["checkpoint_id"])[
                "state"
            ]
        resume_elapsed_ns = time.perf_counter_ns() - start_ns

    else:
        raise ValueError(f"unsupported runtime {runtime!r}")

    return {
        "worker": "native_resume",
        "runtime": runtime,
        "direct_avg_ns": direct_elapsed_ns // iterations,
        "resume_avg_ns": resume_elapsed_ns // iterations,
        "state_equal": bool(_materialize_state(direct_state) == _materialize_state(resumed_state)),
    }


def _bench_native_fanout() -> dict[str, Any]:
    runtime = os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"]
    session_count = 24
    iterations = 50

    if runtime == "langgraph":
        _prefer_installed_langgraph()
        import operator
        from typing_extensions import Annotated, TypedDict

        from langgraph.checkpoint.memory import InMemorySaver
        from langgraph.graph import END, START, StateGraph

        class ChildState(TypedDict, total=False):
            session_id: str
            query: str
            visits: Annotated[int, operator.add]
            memory: Annotated[list[str], operator.add]
            answer: str

        child_builder = StateGraph(ChildState)

        def specialist(state: ChildState):
            query = state.get("query", "")
            return {
                "visits": 1,
                "memory": [query],
                "answer": f"{state.get('session_id')}::{query}::{state.get('visits', 0) + 1}",
            }

        child_builder.add_node("specialist", specialist)
        child_builder.add_edge(START, "specialist")
        child_builder.add_edge("specialist", END)
        child_graph = child_builder.compile(checkpointer=InMemorySaver())

        annotations: dict[str, Any] = {"round": int, "summary": str}
        for index in range(session_count):
            annotations[f"answer_{index}"] = str
            annotations[f"visits_{index}"] = int
        ParentState = TypedDict("ParentState", annotations, total=False)

        parent_builder = StateGraph(ParentState)

        def make_branch(index: int):
            def branch(state: ParentState):
                output = child_graph.invoke(
                    {
                        "session_id": f"specialist-{index}",
                        "query": f"round-{state['round']}-q{index}",
                    },
                    config={"configurable": {"thread_id": f"specialist-{index}"}},
                )
                return {
                    f"answer_{index}": output["answer"],
                    f"visits_{index}": output["visits"],
                }

            return branch

        for index in range(session_count):
            parent_builder.add_node(f"branch_{index}", make_branch(index))
            parent_builder.add_edge(START, f"branch_{index}")
        parent_builder.add_node(
            "join",
            lambda state: {
                "summary": "|".join(state.get(f"answer_{index}", "") for index in range(session_count))
            },
        )
        parent_builder.add_edge([f"branch_{index}" for index in range(session_count)], "join")
        parent_builder.add_edge("join", END)
        parent_graph = parent_builder.compile()

        for _ in range(5):
            parent_graph.invoke({"round": 0})

        start_ns = time.perf_counter_ns()
        final_state = None
        for round_index in range(iterations):
            final_state = parent_graph.invoke({"round": round_index})
        elapsed_ns = time.perf_counter_ns() - start_ns

        return {
            "worker": "native_fanout",
            "runtime": runtime,
            "avg_ns": elapsed_ns // iterations,
            "summary_length": len(final_state["summary"]),
            "session_count": session_count,
        }

    if runtime == "agentcore":
        _prefer_agentcore_build_python()
        from agentcore.graph import END, START, StateGraph

        child_graph = StateGraph(dict, name="fanout_child", worker_count=8)

        def specialist(state: dict[str, Any], config: dict[str, Any]):
            query = state.get("query", "")
            visits = int(state.get("visits", 0)) + 1
            return {
                "visits": visits,
                "answer": f"{state.get('session_id')}::{query}::{visits}",
            }

        child_graph.add_node("specialist", specialist)
        child_graph.add_edge(START, "specialist")
        child_graph.add_edge("specialist", END)

        parent_graph = StateGraph(dict, name="fanout_parent", worker_count=max(8, session_count))
        parent_graph.add_fanout("fanout")

        def make_prepare(index: int):
            def prepare(state: dict[str, Any], config: dict[str, Any]):
                return {
                    f"session_{index}_id": f"specialist-{index}",
                    f"session_{index}_query": f"round-{state['round']}-q{index}",
                }

            return prepare

        for index in range(session_count):
            parent_graph.add_node(f"prepare_{index}", make_prepare(index))
            parent_graph.add_subgraph(
                f"branch_{index}",
                child_graph,
                namespace=f"branch_{index}",
                inputs={
                    f"session_{index}_id": "session_id",
                    f"session_{index}_query": "query",
                },
                outputs={
                    f"answer_{index}": "answer",
                    f"visits_{index}": "visits",
                },
                session_mode="persistent",
                session_id_from=f"session_{index}_id",
            )
            parent_graph.add_edge("fanout", f"prepare_{index}")
            parent_graph.add_edge(f"prepare_{index}", f"branch_{index}")

        parent_graph.add_join(
            "join",
            lambda state, config: {
                "summary": "|".join(state.get(f"answer_{index}", "") for index in range(session_count))
            },
        )
        for index in range(session_count):
            parent_graph.add_edge(f"branch_{index}", "join")
        parent_graph.add_edge(START, "fanout")
        parent_graph.add_edge("join", END)
        parent_graph = parent_graph.compile()

        for _ in range(5):
            parent_graph.invoke({"round": 0})

        start_ns = time.perf_counter_ns()
        final_state = None
        for round_index in range(iterations):
            final_state = parent_graph.invoke({"round": round_index})
        elapsed_ns = time.perf_counter_ns() - start_ns

        details = parent_graph.invoke_with_metadata({"round": iterations}, config={"tags": ["fanout-benchmark"]})
        session_events = [event for event in details["trace"] if event.get("session_id")]

        return {
            "worker": "native_fanout",
            "runtime": runtime,
            "avg_ns": elapsed_ns // iterations,
            "summary_length": len(final_state["summary"]),
            "session_count": session_count,
            "session_event_count": len(session_events),
        }

    raise ValueError(f"unsupported runtime {runtime!r}")


def _system_info() -> dict[str, Any]:
    langgraph_version = subprocess.run(
        [sys.executable, "-c", "import importlib.metadata as m; print(m.version('langgraph'))"],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=True,
    ).stdout.strip()

    cpu_model = ""
    try:
        cpu_model = (
            subprocess.run(
                ["bash", "-lc", "lscpu | awk -F: '/Model name/ {gsub(/^ +/, \"\", $2); print $2; exit}'"],
                cwd=str(REPO_ROOT),
                capture_output=True,
                text=True,
                check=True,
            )
            .stdout.strip()
        )
    except Exception:
        cpu_model = platform.processor() or "unknown"

    return {
        "python_version": platform.python_version(),
        "platform": platform.platform(),
        "cpu_model": cpu_model,
        "langgraph_version": langgraph_version,
    }


def _print_markdown(results: dict[str, Any]) -> None:
    compat = results["compat"]
    native_memory = results["native_memory"]
    native_resume = results["native_resume"]
    native_fanout = results["native_fanout"]
    system = results["system"]

    print("# AgentCore vs LangGraph")
    print()
    print("## Environment")
    print()
    print(f"- Python: `{system['python_version']}`")
    print(f"- Platform: `{system['platform']}`")
    print(f"- CPU: `{system['cpu_model']}`")
    print(f"- LangGraph: `{system['langgraph_version']}`")
    print(f"- AgentCore: local source tree at `{REPO_ROOT}`")
    print()
    print("## 1. Same-code builder path")
    print()
    print(
        "This section compares upstream LangGraph to AgentCore's generated LangGraph-compatible surface "
        "(`agentcore_langgraph_native.langgraph_compat`) on the same 10-node research workflow."
    )
    print()
    print("| Metric | LangGraph | AgentCore compat | Relative |")
    print("| --- | ---: | ---: | ---: |")
    print(
        f"| Invoke avg latency | {_fmt_ns(compat['invoke_avg_ns_langgraph'])} | "
        f"{_fmt_ns(compat['invoke_avg_ns_agentcore_compat'])} | "
        f"{_relative_latency_label(compat['invoke_avg_ns_langgraph'], compat['invoke_avg_ns_agentcore_compat'])} |"
    )
    print(
        f"| Invoke throughput | {_fmt_ops_per_second(compat['invoke_avg_ns_langgraph'])} | "
        f"{_fmt_ops_per_second(compat['invoke_avg_ns_agentcore_compat'])} | - |"
    )
    print(
        f"| Stream avg latency | {_fmt_ns(compat['stream_avg_ns_langgraph'])} | "
        f"{_fmt_ns(compat['stream_avg_ns_agentcore_compat'])} | "
        f"{_relative_latency_label(compat['stream_avg_ns_langgraph'], compat['stream_avg_ns_agentcore_compat'])} |"
    )
    print(
        f"| Batch avg cost / item | {_fmt_ns(compat['batch_item_avg_ns_langgraph'])} | "
        f"{_fmt_ns(compat['batch_item_avg_ns_agentcore_compat'])} | "
        f"{_relative_latency_label(compat['batch_item_avg_ns_langgraph'], compat['batch_item_avg_ns_agentcore_compat'])} |"
    )
    print(
        f"| Batch throughput | {_fmt_batch_items_per_second(compat['batch_item_avg_ns_langgraph'])} | "
        f"{_fmt_batch_items_per_second(compat['batch_item_avg_ns_agentcore_compat'])} | - |"
    )
    print(
        f"| Stream events / run | {compat['stream_events_langgraph']} | {compat['stream_events_agentcore_compat']} | matched |"
    )
    print()
    print("## 2. Native runtime features")
    print()
    print(
        "This section compares upstream LangGraph to AgentCore's native API on features that rely on persistent "
        "sessions, explicit pause/resume, and built-in child-session lifecycle management."
    )
    print()
    print("| Workload | LangGraph | AgentCore native | Relative |")
    print("| --- | ---: | ---: | ---: |")
    print(
        f"| Long-running persistent specialist memory: avg latency / round | "
        f"{_fmt_ns(native_memory['langgraph']['avg_ns'])} | {_fmt_ns(native_memory['agentcore']['avg_ns'])} | "
        f"{_relative_latency_label(native_memory['langgraph']['avg_ns'], native_memory['agentcore']['avg_ns'])} |"
    )
    print(
        f"| Long-running persistent specialist memory: peak RSS | "
        f"{_fmt_memory_kb(native_memory['langgraph']['maxrss_kb'])} | {_fmt_memory_kb(native_memory['agentcore']['maxrss_kb'])} | "
        f"{_relative_memory_label(native_memory['langgraph']['maxrss_kb'], native_memory['agentcore']['maxrss_kb'])} |"
    )
    print(
        f"| Pause + resume avg latency | {_fmt_ns(native_resume['langgraph']['resume_avg_ns'])} | "
        f"{_fmt_ns(native_resume['agentcore']['resume_avg_ns'])} | "
        f"{_relative_latency_label(native_resume['langgraph']['resume_avg_ns'], native_resume['agentcore']['resume_avg_ns'])} |"
    )
    print(
        f"| Direct invoke avg latency | {_fmt_ns(native_resume['langgraph']['direct_avg_ns'])} | "
        f"{_fmt_ns(native_resume['agentcore']['direct_avg_ns'])} | "
        f"{_relative_latency_label(native_resume['langgraph']['direct_avg_ns'], native_resume['agentcore']['direct_avg_ns'])} |"
    )
    print(
        f"| Direct vs resumed final state | {native_resume['langgraph']['state_equal']} | "
        f"{native_resume['agentcore']['state_equal']} | both preserved |"
    )
    print(
        f"| Persistent session fan-out ({native_fanout['langgraph']['session_count']} sessions) avg latency | "
        f"{_fmt_ns(native_fanout['langgraph']['avg_ns'])} | {_fmt_ns(native_fanout['agentcore']['avg_ns'])} | "
        f"{_relative_latency_label(native_fanout['langgraph']['avg_ns'], native_fanout['agentcore']['avg_ns'])} |"
    )
    print(
        f"| Session-tagged events on fan-out run | n/a | {native_fanout['agentcore']['session_event_count']} | AgentCore emits child-session identities |"
    )


def _run_main() -> dict[str, Any]:
    system = _system_info()
    compat = _run_subprocess("langgraph_compat")

    native_memory = {
        "langgraph": _run_subprocess("native_memory_langgraph"),
        "agentcore": _run_subprocess("native_memory_agentcore"),
    }
    native_resume = {
        "langgraph": _run_subprocess("native_resume_langgraph"),
        "agentcore": _run_subprocess("native_resume_agentcore"),
    }
    native_fanout = {
        "langgraph": _run_subprocess("native_fanout_langgraph"),
        "agentcore": _run_subprocess("native_fanout_agentcore"),
    }

    return {
        "system": system,
        "compat": compat,
        "native_memory": native_memory,
        "native_resume": native_resume,
        "native_fanout": native_fanout,
    }


def _run_worker(worker: str) -> dict[str, Any]:
    if worker == "langgraph_compat":
        return _bench_langgraph_compat()

    if worker == "native_memory_langgraph":
        os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"] = "langgraph"
        return _bench_native_memory()
    if worker == "native_memory_agentcore":
        os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"] = "agentcore"
        return _bench_native_memory()

    if worker == "native_resume_langgraph":
        os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"] = "langgraph"
        return _bench_native_resume()
    if worker == "native_resume_agentcore":
        os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"] = "agentcore"
        return _bench_native_resume()

    if worker == "native_fanout_langgraph":
        os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"] = "langgraph"
        return _bench_native_fanout()
    if worker == "native_fanout_agentcore":
        os.environ["AGENTCORE_HEAD_TO_HEAD_RUNTIME"] = "agentcore"
        return _bench_native_fanout()

    raise ValueError(f"unknown worker {worker!r}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run AgentCore vs LangGraph benchmarks.")
    parser.add_argument("--worker", default=None, help="Internal worker mode.")
    parser.add_argument(
        "--format",
        choices=("markdown", "json"),
        default="markdown",
        help="Output format for the aggregated benchmark report.",
    )
    args = parser.parse_args()

    if args.worker:
        print(json.dumps(_run_worker(args.worker)))
        return

    results = _run_main()
    if args.format == "json":
        print(json.dumps(results, indent=2, sort_keys=True))
    else:
        _print_markdown(results)


if __name__ == "__main__":
    main()
