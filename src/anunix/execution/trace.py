"""Execution tracing — append-only record of cell execution."""

from __future__ import annotations

from datetime import datetime
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import CellID, ObjectID, TraceID, new_trace_id, now_utc


class TraceEvent(BaseModel):
    timestamp: datetime = Field(default_factory=now_utc)
    kind: str
    detail: dict[str, Any] = Field(default_factory=dict)


class ExecutionTrace(BaseModel):
    """Append-only execution record for a cell."""

    id: TraceID = Field(default_factory=new_trace_id)
    cell_id: CellID
    parent_cell_id: CellID | None = None
    engine_used: str = ""
    started_at: datetime | None = None
    completed_at: datetime | None = None
    status: str = "running"
    events: list[TraceEvent] = Field(default_factory=list)
    input_refs: list[ObjectID] = Field(default_factory=list)
    output_refs: list[ObjectID] = Field(default_factory=list)
    error: str | None = None

    def record(self, kind: str, **detail: Any) -> None:
        self.events.append(TraceEvent(kind=kind, detail=detail))
