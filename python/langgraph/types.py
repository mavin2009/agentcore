from agentcore_langgraph_native.langgraph_compat import Command


def interrupt(*args, **kwargs):
    raise NotImplementedError(
        "the local AgentCore LangGraph compatibility shim does not implement langgraph.types.interrupt; "
        "switch to agentcore.graph for explicit wait/resume flows"
    )


__all__ = ["Command", "interrupt"]
