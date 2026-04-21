"""CellRuntime — executes cells against engines and produces traced outputs.

Supports two execution modes:
1. Python callable — runs a function with inputs, returns output
2. Model inference — calls litellm with a prompt, returns model output
"""

from __future__ import annotations

import asyncio
import traceback
from collections.abc import Callable
from datetime import datetime, timezone
from typing import Any

from anunix.core.errors import CellTimeoutError, ExecutionError
from anunix.core.types import ObjectID, new_object_id, now_utc
from anunix.execution.cell import (
    CellError,
    CellStatus,
    ExecutionCell,
)
from anunix.execution.trace import ExecutionTrace
from anunix.state.object import (
    OriginKind,
    ProvenanceEntry,
    StateObject,
    Transformation,
)


class CellExecutionResult:
    """Result of executing a cell."""

    def __init__(
        self,
        cell: ExecutionCell,
        outputs: list[StateObject],
        trace: ExecutionTrace,
    ) -> None:
        self.cell = cell
        self.outputs = outputs
        self.trace = trace

    @property
    def success(self) -> bool:
        return self.cell.status == CellStatus.COMPLETED


class CellExecutor:
    """Executes cells by dispatching to registered engines."""

    def __init__(self) -> None:
        self._engines: dict[str, Callable[..., Any]] = {}

    def register_engine(self, name: str, fn: Callable[..., Any]) -> None:
        """Register a callable as an execution engine."""
        self._engines[name] = fn

    async def execute(
        self,
        cell: ExecutionCell,
        *,
        engine: str | None = None,
        inputs: dict[str, Any] | None = None,
    ) -> CellExecutionResult:
        """Execute a cell and return results with trace."""
        trace = ExecutionTrace(
            cell_id=cell.id,
            parent_cell_id=cell.parent_cell_ref,
        )

        # Transition to running
        cell.status = CellStatus.RUNNING
        cell.runtime.attempt_count += 1
        cell.runtime.started_at = now_utc()
        trace.started_at = cell.runtime.started_at
        trace.record("status_change", status="running")

        # Resolve engine
        engine_name = engine or cell.runtime.current_engine
        if not engine_name:
            engine_name = self._select_engine(cell)
        cell.runtime.current_engine = engine_name
        trace.engine_used = engine_name
        trace.record("engine_selected", engine=engine_name)

        # Execute
        outputs: list[StateObject] = []
        try:
            if engine_name == "model" or engine_name.startswith("model:"):
                raw_output = await self._execute_model(cell, inputs or {})
            elif engine_name in self._engines:
                raw_output = await self._call_engine(engine_name, cell, inputs or {})
            else:
                raise ExecutionError(f"Unknown engine: {engine_name}")

            # Wrap output as StateObject
            output_obj = StateObject(
                id=new_object_id(),
                type=self._infer_output_type(cell),
                payload=raw_output,
                provenance=ProvenanceEntry(
                    origin_kind=OriginKind.DERIVED,
                    created_by={"kind": "execution_cell", "id": cell.id},
                    source_refs=[
                        ObjectID(inp.state_object_ref)
                        for inp in cell.inputs
                        if inp.state_object_ref
                    ],
                    transformation=Transformation(
                        name=cell.intent.name,
                        kind="cell_execution",
                        engine=engine_name,
                    ),
                ),
            )
            outputs.append(output_obj)
            cell.output_refs.append(output_obj.id)
            trace.output_refs.append(output_obj.id)
            trace.record("output_produced", object_id=output_obj.id)

            # Success
            cell.status = CellStatus.COMPLETED
            cell.runtime.completed_at = now_utc()
            trace.completed_at = cell.runtime.completed_at
            trace.status = "completed"
            trace.record("status_change", status="completed")

        except asyncio.TimeoutError:
            cell.status = CellStatus.FAILED
            cell.error = CellError(code="timeout", message="Execution timed out", retryable=True)
            trace.status = "failed"
            trace.error = "timeout"
            trace.record("error", code="timeout")
            raise CellTimeoutError(f"Cell {cell.id} timed out")

        except Exception as e:
            cell.status = CellStatus.FAILED
            cell.error = CellError(
                code="execution_error",
                message=str(e),
                retryable=False,
            )
            trace.status = "failed"
            trace.error = traceback.format_exc()
            trace.record("error", code="execution_error", message=str(e))

        return CellExecutionResult(cell=cell, outputs=outputs, trace=trace)

    def _select_engine(self, cell: ExecutionCell) -> str:
        """Select an engine based on routing policy. Defaults to first registered."""
        allowed = cell.routing_policy.allowed_engines
        for name in allowed:
            if name in self._engines:
                return name
        if self._engines:
            return next(iter(self._engines))
        return "model"

    async def _call_engine(
        self, engine_name: str, cell: ExecutionCell, inputs: dict[str, Any]
    ) -> Any:
        """Call a registered engine function."""
        fn = self._engines[engine_name]
        result = fn(cell=cell, inputs=inputs)
        if asyncio.iscoroutine(result):
            return await result
        return result

    async def _execute_model(self, cell: ExecutionCell, inputs: dict[str, Any]) -> str:
        """Execute via litellm model call."""
        try:
            from litellm import acompletion
        except ImportError:
            raise ExecutionError(
                "litellm is not installed. Install with: pip install anunix[models]"
            )

        prompt = inputs.get("prompt", "")
        if not prompt:
            prompt = f"{cell.intent.objective}\n\nInput: {inputs.get('text', '')}"

        model = inputs.get("model", "gpt-4o-mini")
        messages = [{"role": "user", "content": prompt}]

        timeout = cell.constraints.max_latency_ms
        timeout_s = (timeout / 1000.0) if timeout else 300.0

        response = await asyncio.wait_for(
            acompletion(model=model, messages=messages),
            timeout=timeout_s,
        )
        return response.choices[0].message.content

    def _infer_output_type(self, cell: ExecutionCell) -> str:
        """Infer the output State Object type from the cell's intent."""
        requested = cell.intent.requested_outputs
        if requested:
            return requested[0]
        return "model.output"
