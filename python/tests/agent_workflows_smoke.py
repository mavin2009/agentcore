import asyncio

from agentcore.graph import Command, END, START, StateGraph


def build_single_agent_graph():
    async def agent(state, config):
        max_turns = int(dict(config.get("configurable", {})).get("max_turns", 3))
        turn = int(state.get("turn", 0)) + 1
        transcript = list(state.get("transcript", []))
        transcript.append(f"agent-turn-{turn}")
        updates = {
            "turn": turn,
            "transcript": transcript,
            "final_answer": transcript[-1],
        }
        if turn >= max_turns:
            return Command(update=updates, goto=END)
        return Command(update=updates, goto="agent")

    graph = StateGraph(dict, name="python_single_agent_workflow", worker_count=1)
    graph.add_node("agent", agent)
    graph.add_edge(START, "agent")
    graph.add_conditional_edges("agent", lambda state, config: END, {END: END})
    return graph.compile()


def build_multi_agent_graph():
    def coordinator(state, config):
        handoffs = list(state.get("handoffs", []))
        handoffs.append("coordinator")
        topic = str(dict(config.get("configurable", {})).get("topic", "unknown"))
        return Command(
            update={
                "topic": topic,
                "handoffs": handoffs,
            },
            goto="researcher",
        )

    async def researcher(state, config):
        await asyncio.sleep(0)
        handoffs = list(state.get("handoffs", []))
        handoffs.append("researcher")
        research_notes = list(state.get("research_notes", []))
        research_notes.append(f"research::{state['topic']}")
        research_notes.append("constraint::deterministic-state-patches")
        return Command(
            update={
                "handoffs": handoffs,
                "research_notes": research_notes,
            },
            goto="writer",
        )

    def writer(state, config):
        handoffs = list(state.get("handoffs", []))
        handoffs.append("writer")
        draft_version = int(state.get("draft_version", 0)) + 1
        research_notes = "|".join(state.get("research_notes", []))
        draft = f"{state['topic']}::draft-v{draft_version}::{research_notes}"
        return Command(
            update={
                "handoffs": handoffs,
                "draft_version": draft_version,
                "draft": draft,
            },
            goto="reviewer",
        )

    async def reviewer(state, config):
        await asyncio.sleep(0)
        handoffs = list(state.get("handoffs", []))
        handoffs.append("reviewer")
        approvals_needed = int(dict(config.get("configurable", {})).get("approvals_needed", 2))
        review_log = list(state.get("review_log", []))
        approved = int(state.get("draft_version", 0)) >= approvals_needed
        review_log.append("approved" if approved else "revise")
        return Command(
            update={
                "handoffs": handoffs,
                "review_log": review_log,
                "approved": approved,
            },
            goto=END if approved else "writer",
        )

    graph = StateGraph(dict, name="python_multi_agent_workflow", worker_count=4)
    graph.add_node("coordinator", coordinator)
    graph.add_node("researcher", researcher)
    graph.add_node("writer", writer)
    graph.add_node("reviewer", reviewer)
    graph.add_edge(START, "coordinator")
    graph.add_edge("coordinator", "researcher")
    graph.add_edge("researcher", "writer")
    graph.add_edge("writer", "reviewer")
    graph.add_edge("reviewer", "writer")
    graph.add_edge("reviewer", END)
    return graph.compile()


def build_subgraph_graph():
    def child_step(state, config):
        seed = int(state.get("child_input", 0))
        tag_count = len(config.get("tags", []))
        return {
            "child_total": seed + 40,
            "child_trace": f"child::{seed}::{tag_count}",
        }

    child_graph = StateGraph(dict, name="python_embedded_child_graph", worker_count=2)
    child_graph.add_node("child_step", child_step)
    child_graph.add_edge(START, "child_step")
    child_graph.add_edge("child_step", END)

    def seed_parent(state, config):
        seed = int(dict(config.get("configurable", {})).get("seed", 6))
        return {"input_value": seed}

    def summarize_parent(state, config):
        return {
            "summary": f"subgraph={state['subgraph_total']}::{state['subgraph_trace']}",
            "validated": True,
        }

    parent_graph = StateGraph(dict, name="python_subgraph_parent_graph", worker_count=4)
    parent_graph.add_node("seed", seed_parent)
    parent_graph.add_subgraph(
        "planner_subgraph",
        child_graph,
        namespace="planner_subgraph",
        inputs={"input_value": "child_input"},
        outputs={
            "subgraph_total": "child_total",
            "subgraph_trace": "child_trace",
        },
    )
    parent_graph.add_node("summarize", summarize_parent)
    parent_graph.add_edge(START, "seed")
    parent_graph.add_edge("seed", "planner_subgraph")
    parent_graph.add_edge("planner_subgraph", "summarize")
    parent_graph.add_edge("summarize", END)
    return parent_graph.compile()


def build_fanout_join_graph():
    def left_branch(state, config):
        return {
            "total": 1,
            "left_seen": True,
        }

    def right_branch(state, config):
        return {
            "total": 2,
            "right_seen": True,
        }

    def join_branches(state, config):
        completed = [
            branch_name
            for branch_name in ("left", "right")
            if state.get(f"{branch_name}_seen") is True
        ]
        return {
            "joined": True,
            "summary": f"branches={','.join(completed)} total={state['total']}",
        }

    graph = StateGraph(dict, name="python_fanout_join_workflow", worker_count=4)
    graph.add_fanout("fanout")
    graph.add_node("left", left_branch)
    graph.add_node("right", right_branch)
    graph.add_join(
        "join",
        join_branches,
        merge={
            "total": "sum_int64",
            "left_seen": "logical_or",
            "right_seen": "logical_or",
        },
    )
    graph.add_edge(START, "fanout")
    graph.add_edge("fanout", "left")
    graph.add_edge("fanout", "right")
    graph.add_edge("left", "join")
    graph.add_edge("right", "join")
    graph.add_edge("join", END)
    return graph.compile()


def exercise_single_agent():
    compiled = build_single_agent_graph()
    config = {"configurable": {"max_turns": 3}, "tags": ["single-agent"]}

    final_state = compiled.invoke({"turn": 0, "transcript": []}, config=config)
    assert final_state["turn"] == 3
    assert final_state["transcript"] == ["agent-turn-1", "agent-turn-2", "agent-turn-3"]
    assert final_state["final_answer"] == "agent-turn-3"

    details = compiled.invoke_with_metadata({"turn": 0, "transcript": []}, config=config)
    assert details["summary"]["status"] == "completed"
    assert [event["node_name"] for event in details["trace"]] == ["agent", "agent", "agent"]
    assert details["proof"]["combined_digest"] != 0

    events = list(compiled.stream({"turn": 0, "transcript": []}, config=config))
    assert [event["node_name"] for event in events] == ["agent", "agent", "agent"]
    assert all(event["graph_name"] == "python_single_agent_workflow" for event in events)


def exercise_subgraph_composition():
    compiled = build_subgraph_graph()
    config = {"configurable": {"seed": 6}, "tags": ["subgraph", "stream"]}

    final_state = compiled.invoke({}, config=config)
    assert final_state["subgraph_total"] == 46
    assert final_state["subgraph_trace"] == "child::6::2"
    assert final_state["summary"] == "subgraph=46::child::6::2"
    assert final_state["validated"] is True

    details = compiled.invoke_with_metadata({}, config=config)
    assert details["summary"]["status"] == "completed"
    subgraph_events = [event for event in details["trace"] if event["namespaces"]]
    assert len(subgraph_events) == 1
    assert subgraph_events[0]["graph_name"] == "python_embedded_child_graph"
    assert subgraph_events[0]["node_name"] == "child_step"
    assert subgraph_events[0]["namespaces"][0]["graph_name"] == "python_subgraph_parent_graph"
    assert subgraph_events[0]["namespaces"][0]["node_name"] == "planner_subgraph"

    root_only = compiled.invoke_with_metadata({}, config=config, include_subgraphs=False)
    assert [event["node_name"] for event in root_only["trace"]] == [
        "seed",
        "planner_subgraph",
        "summarize",
    ]

    streamed_events = list(compiled.stream({}, config=config))
    streamed_subgraph_events = [event for event in streamed_events if event["namespaces"]]
    assert len(streamed_subgraph_events) == 1
    assert streamed_subgraph_events[0]["graph_name"] == "python_embedded_child_graph"
    assert streamed_subgraph_events[0]["node_name"] == "child_step"
    assert streamed_subgraph_events[0]["namespaces"][0]["node_name"] == "planner_subgraph"


async def exercise_subgraph_parallel():
    compiled = build_subgraph_graph()
    batch_results = await compiled.abatch(
        [{}, {}],
        config=[
            {"configurable": {"seed": 8}, "tags": ["subgraph", "alpha"]},
            {"configurable": {"seed": 5}, "tags": ["subgraph", "beta", "gamma"]},
        ],
    )
    assert [result["subgraph_total"] for result in batch_results] == [48, 45]
    assert [result["subgraph_trace"] for result in batch_results] == [
        "child::8::2",
        "child::5::3",
    ]


def exercise_fanout_join():
    compiled = build_fanout_join_graph()

    final_state = compiled.invoke(
        {
            "total": 0,
            "left_seen": False,
            "right_seen": False,
        }
    )
    assert final_state["total"] == 3
    assert final_state["left_seen"] is True
    assert final_state["right_seen"] is True
    assert final_state["joined"] is True
    assert final_state["summary"] == "branches=left,right total=3"

    details = compiled.invoke_with_metadata(
        {
            "total": 0,
            "left_seen": False,
            "right_seen": False,
        }
    )
    assert details["summary"]["status"] == "completed"
    node_names = {event["node_name"] for event in details["trace"]}
    assert {"fanout", "left", "right", "join"}.issubset(node_names)
    assert sum(1 for event in details["trace"] if event["node_name"] == "join") >= 2


async def exercise_multi_agent():
    compiled = build_multi_agent_graph()
    config = {
        "configurable": {
            "topic": "native-runtime",
            "approvals_needed": 2,
        },
        "tags": ["multi-agent"],
    }

    final_state = await compiled.ainvoke({}, config=config)
    assert final_state["approved"] is True
    assert final_state["draft_version"] == 2
    assert final_state["handoffs"] == [
        "coordinator",
        "researcher",
        "writer",
        "reviewer",
        "writer",
        "reviewer",
    ]
    assert final_state["review_log"] == ["revise", "approved"]
    assert final_state["draft"].startswith("native-runtime::draft-v2::")

    details = await compiled.ainvoke_with_metadata({}, config=config)
    assert details["summary"]["status"] == "completed"
    assert [event["node_name"] for event in details["trace"]] == [
        "coordinator",
        "researcher",
        "writer",
        "reviewer",
        "writer",
        "reviewer",
    ]
    assert details["proof"]["combined_digest"] != 0

    async_events = [event async for event in compiled.astream({}, config=config)]
    assert [event["node_name"] for event in async_events] == [
        "coordinator",
        "researcher",
        "writer",
        "reviewer",
        "writer",
        "reviewer",
    ]
    assert all(event["graph_name"] == "python_multi_agent_workflow" for event in async_events)

    batch_results = await compiled.abatch(
        [{}, {}],
        config=[
            {
                "configurable": {"topic": "runtime-a", "approvals_needed": 2},
                "tags": ["multi-agent", "batch-a"],
            },
            {
                "configurable": {"topic": "runtime-b", "approvals_needed": 3},
                "tags": ["multi-agent", "batch-b"],
            },
        ],
    )
    assert [result["draft_version"] for result in batch_results] == [2, 3]
    assert [result["approved"] for result in batch_results] == [True, True]
    assert [result["handoffs"][-1] for result in batch_results] == ["reviewer", "reviewer"]


exercise_single_agent()
exercise_subgraph_composition()
exercise_fanout_join()
asyncio.run(exercise_subgraph_parallel())
asyncio.run(exercise_multi_agent())

print("python agent and multi-agent workflows smoke passed")
