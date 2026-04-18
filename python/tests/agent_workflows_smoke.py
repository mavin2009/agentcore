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


def build_persistent_specialist_child_graph(*, wait_on_first_visit: bool = False):
    def specialist_step(state, config, runtime):
        session_id = str(state.get("session_id", "unknown"))
        query = str(state.get("query", ""))
        resume_attempt = int(state.get("resume_attempt", 0))
        if wait_on_first_visit and resume_attempt == 0:
            return Command(update={"resume_attempt": 1}, wait=True)

        visits = int(state.get("visits", 0)) + 1
        memory = list(state.get("memory", []))
        prior_memory = len(memory)
        memory.append(query)
        memoized = runtime.record_once(
            f"python-specialist::{session_id}::memo",
            {"session_id": session_id},
            lambda: {"memo": f"memo::{session_id}"},
        )
        return {
            "visits": visits,
            "prior_memory": prior_memory,
            "memory": memory,
            "memo": memoized["memo"],
            "answer": f"{session_id}::{query}::visit-{visits}",
        }

    child_graph = StateGraph(
        dict,
        name="python_persistent_specialist_child",
        worker_count=2,
    )
    child_graph.add_node("specialist_step", specialist_step)
    child_graph.add_edge(START, "specialist_step")
    child_graph.add_edge("specialist_step", END)
    return child_graph


def build_persistent_specialist_parent(*, wait_on_first_visit: bool = False):
    child_graph = build_persistent_specialist_child_graph(wait_on_first_visit=wait_on_first_visit)

    def revisit_or_finish(state, config):
        round_index = int(state.get("round", 0))
        if round_index == 0:
            return Command(
                update={
                    "round": 1,
                    "query": "followup-brief",
                },
                goto="specialist_session",
            )
        return Command(update={"complete": True}, goto=END)

    parent_graph = StateGraph(
        dict,
        name="python_persistent_specialist_parent",
        worker_count=4,
    )
    parent_graph.add_subgraph(
        "specialist_session",
        child_graph,
        namespace="specialist_session",
        inputs={
            "session_id": "session_id",
            "query": "query",
        },
        outputs={
            "answer": "answer",
            "visits": "visits",
            "prior_memory": "prior_memory",
            "memory": "memory",
            "memo": "memo",
            "resume_attempt": "resume_attempt",
        },
        session_mode="persistent",
        session_id_from="session_id",
    )
    parent_graph.add_node("revisit_or_finish", revisit_or_finish)
    parent_graph.add_edge(START, "specialist_session")
    parent_graph.add_edge("specialist_session", "revisit_or_finish")
    parent_graph.add_edge("revisit_or_finish", END)
    return parent_graph.compile()


def build_parallel_specialist_graph():
    child_graph = build_persistent_specialist_child_graph()

    def left_prepare(state, config):
        return {
            "left_session_id": "planner",
            "left_query": "analyze-system",
        }

    def right_prepare(state, config):
        return {
            "right_session_id": "reviewer",
            "right_query": "review-plan",
        }

    def join_branches(state, config):
        return {
            "combined": f"{state['left_answer']}|{state['right_answer']}",
            "joined": True,
        }

    graph = StateGraph(dict, name="python_parallel_specialists", worker_count=4)
    graph.add_fanout("fanout")
    graph.add_node("left_prepare", left_prepare)
    graph.add_node("right_prepare", right_prepare)
    graph.add_subgraph(
        "left_specialist",
        child_graph,
        namespace="left_specialist",
        inputs={
            "left_session_id": "session_id",
            "left_query": "query",
        },
        outputs={
            "left_answer": "answer",
            "left_visits": "visits",
        },
        session_mode="persistent",
        session_id_from="left_session_id",
    )
    graph.add_subgraph(
        "right_specialist",
        child_graph,
        namespace="right_specialist",
        inputs={
            "right_session_id": "session_id",
            "right_query": "query",
        },
        outputs={
            "right_answer": "answer",
            "right_visits": "visits",
        },
        session_mode="persistent",
        session_id_from="right_session_id",
    )
    graph.add_join("join", join_branches)
    graph.add_edge(START, "fanout")
    graph.add_edge("fanout", "left_prepare")
    graph.add_edge("fanout", "right_prepare")
    graph.add_edge("left_prepare", "left_specialist")
    graph.add_edge("right_prepare", "right_specialist")
    graph.add_edge("left_specialist", "join")
    graph.add_edge("right_specialist", "join")
    graph.add_edge("join", END)
    return graph.compile()


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


def exercise_persistent_specialist_session():
    compiled = build_persistent_specialist_parent()
    details = compiled.invoke_with_metadata(
        {
            "session_id": "specialist-alpha",
            "query": "initial-brief",
            "round": 0,
        },
        config={"tags": ["persistent-session"]},
    )

    assert details["summary"]["status"] == "completed"
    assert details["state"]["visits"] == 2
    assert details["state"]["prior_memory"] == 1
    assert details["state"]["memory"] == ["initial-brief", "followup-brief"]
    assert details["state"]["memo"] == "memo::specialist-alpha"
    assert details["state"]["answer"] == "specialist-alpha::followup-brief::visit-2"

    session_events = [event for event in details["trace"] if event["session_id"] == "specialist-alpha"]
    namespaced_session_events = [event for event in session_events if event["namespaces"]]
    assert len(session_events) >= 2
    assert {event["session_revision"] for event in session_events} == {1, 2}
    assert namespaced_session_events
    assert all(
        event["namespaces"][0]["node_name"] == "specialist_session"
        for event in namespaced_session_events
    )

    streamed_events = list(
        compiled.stream(
            {
                "session_id": "specialist-alpha",
                "query": "initial-brief",
                "round": 0,
            },
            config={"tags": ["persistent-session", "stream"]},
        )
    )
    streamed_session_events = [
        event for event in streamed_events if event["session_id"] == "specialist-alpha"
    ]
    streamed_namespaced_session_events = [
        event for event in streamed_session_events if event["namespaces"]
    ]
    assert {event["session_revision"] for event in streamed_session_events} == {1, 2}
    assert streamed_namespaced_session_events
    assert all(
        event["namespaces"][0]["session_id"] == "specialist-alpha"
        for event in streamed_namespaced_session_events
    )


def exercise_parallel_specialists():
    compiled = build_parallel_specialist_graph()
    details = compiled.invoke_with_metadata({}, config={"tags": ["parallel-specialists"]})

    assert details["summary"]["status"] == "completed"
    assert details["state"]["left_answer"] == "planner::analyze-system::visit-1"
    assert details["state"]["right_answer"] == "reviewer::review-plan::visit-1"
    assert details["state"]["combined"] == (
        "planner::analyze-system::visit-1|reviewer::review-plan::visit-1"
    )
    assert details["state"]["joined"] is True

    subgraph_events = [event for event in details["trace"] if event["namespaces"]]
    seen_sessions = {event["session_id"] for event in subgraph_events}
    assert seen_sessions == {"planner", "reviewer"}
    assert {
        event["namespaces"][0]["node_name"]
        for event in subgraph_events
    } == {"left_specialist", "right_specialist"}


def exercise_persistent_specialist_resume():
    compiled = build_persistent_specialist_parent(wait_on_first_visit=True)
    paused = compiled.invoke_until_pause_with_metadata(
        {
            "session_id": "resume-specialist",
            "query": "initial-brief",
            "round": 0,
        },
        config={"tags": ["persistent-session", "pause"]},
    )

    assert paused["summary"]["status"] == "paused"
    assert paused["summary"]["checkpoint_id"] > 0

    resumed = compiled.resume_with_metadata(paused["summary"]["checkpoint_id"])
    assert resumed["summary"]["status"] == "completed"
    assert resumed["state"]["visits"] == 2
    assert resumed["state"]["prior_memory"] == 1
    assert resumed["state"]["memory"] == ["initial-brief", "followup-brief"]
    assert resumed["state"]["resume_attempt"] == 1
    assert resumed["state"]["memo"] == "memo::resume-specialist"

    resumed_session_events = [
        event for event in resumed["trace"] if event["session_id"] == "resume-specialist"
    ]
    assert resumed_session_events
    assert max(event["session_revision"] for event in resumed_session_events) == 2


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
exercise_persistent_specialist_session()
exercise_parallel_specialists()
exercise_persistent_specialist_resume()
exercise_fanout_join()
asyncio.run(exercise_subgraph_parallel())
asyncio.run(exercise_multi_agent())

print("python agent and multi-agent workflows smoke passed")
