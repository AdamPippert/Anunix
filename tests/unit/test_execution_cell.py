"""Tests for the Execution Cell Runtime."""

import pytest

from anunix.execution.cell import (
    CellInput,
    CellStatus,
    CellType,
    ExecutionCell,
    Intent,
    RoutingPolicy,
)
from anunix.execution.runtime import CellExecutor
from anunix.execution.trace import ExecutionTrace


def test_create_execution_cell():
    cell = ExecutionCell(
        intent=Intent(
            name="summarize_document",
            objective="Generate a summary",
            requested_outputs=["memory.summary"],
        ),
    )
    assert cell.id.startswith("cell_")
    assert cell.status == CellStatus.CREATED
    assert cell.cell_type == CellType.EXECUTION
    assert cell.intent.name == "summarize_document"


def test_cell_with_inputs():
    from anunix.core.types import ObjectID

    cell = ExecutionCell(
        intent=Intent(name="process"),
        inputs=[
            CellInput(name="doc", state_object_ref=ObjectID("so_abc123")),
            CellInput(name="temperature", value=0.7, required=False),
        ],
    )
    assert len(cell.inputs) == 2
    assert cell.inputs[0].state_object_ref == "so_abc123"
    assert cell.inputs[1].value == 0.7


def test_execution_trace():
    from anunix.core.types import CellID

    trace = ExecutionTrace(cell_id=CellID("cell_test123"))
    trace.record("started", engine="local_tool")
    trace.record("completed", result="ok")
    assert len(trace.events) == 2
    assert trace.events[0].kind == "started"
    assert trace.events[1].detail["result"] == "ok"


@pytest.mark.asyncio
async def test_executor_with_callable_engine():
    executor = CellExecutor()

    def echo_engine(cell, inputs):
        return f"Echo: {inputs.get('text', 'empty')}"

    executor.register_engine("echo", echo_engine)

    cell = ExecutionCell(
        intent=Intent(
            name="echo_test",
            objective="Echo the input",
            requested_outputs=["model.output"],
        ),
        routing_policy=RoutingPolicy(allowed_engines=["echo"]),
    )

    result = await executor.execute(cell, inputs={"text": "hello world"})
    assert result.success
    assert result.cell.status == CellStatus.COMPLETED
    assert len(result.outputs) == 1
    assert result.outputs[0].payload == "Echo: hello world"
    assert result.outputs[0].type == "model.output"
    assert result.trace.engine_used == "echo"
    assert result.trace.status == "completed"


@pytest.mark.asyncio
async def test_executor_with_async_engine():
    executor = CellExecutor()

    async def async_engine(cell, inputs):
        return f"Async result: {inputs.get('value', 0) * 2}"

    executor.register_engine("async_doubler", async_engine)

    cell = ExecutionCell(
        intent=Intent(name="double", requested_outputs=["task.result"]),
        routing_policy=RoutingPolicy(allowed_engines=["async_doubler"]),
    )

    result = await executor.execute(cell, inputs={"value": 21})
    assert result.success
    assert result.outputs[0].payload == "Async result: 42"


@pytest.mark.asyncio
async def test_executor_failure_handling():
    executor = CellExecutor()

    def failing_engine(cell, inputs):
        raise ValueError("Something went wrong")

    executor.register_engine("failing", failing_engine)

    cell = ExecutionCell(
        intent=Intent(name="will_fail"),
        routing_policy=RoutingPolicy(allowed_engines=["failing"]),
    )

    result = await executor.execute(cell, inputs={})
    assert not result.success
    assert result.cell.status == CellStatus.FAILED
    assert result.cell.error is not None
    assert "Something went wrong" in result.cell.error.message
    assert result.trace.status == "failed"


@pytest.mark.asyncio
async def test_executor_provenance_tracking():
    executor = CellExecutor()

    def tool_engine(cell, inputs):
        return "processed"

    executor.register_engine("tool", tool_engine)

    from anunix.core.types import ObjectID

    cell = ExecutionCell(
        intent=Intent(name="process_doc", requested_outputs=["memory.chunk"]),
        inputs=[CellInput(name="source", state_object_ref=ObjectID("so_source123"))],
        routing_policy=RoutingPolicy(allowed_engines=["tool"]),
    )

    result = await executor.execute(cell, inputs={})
    assert result.success
    output = result.outputs[0]
    assert output.provenance.origin_kind.value == "derived"
    assert "so_source123" in output.provenance.source_refs
    assert output.provenance.transformation is not None
    assert output.provenance.transformation.engine == "tool"


@pytest.mark.asyncio
async def test_executor_attempt_count():
    executor = CellExecutor()
    executor.register_engine("ok", lambda cell, inputs: "done")

    cell = ExecutionCell(
        intent=Intent(name="test"),
        routing_policy=RoutingPolicy(allowed_engines=["ok"]),
    )
    assert cell.runtime.attempt_count == 0

    await executor.execute(cell, inputs={})
    assert cell.runtime.attempt_count == 1
    assert cell.runtime.started_at is not None
    assert cell.runtime.completed_at is not None
