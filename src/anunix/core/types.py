"""Foundation types for the Anunix system."""

from __future__ import annotations

import uuid
from datetime import datetime, timezone
from typing import NewType

# Opaque ID types — all use UUID v7 (time-sortable) format: prefix_uuid
ObjectID = NewType("ObjectID", str)
CellID = NewType("CellID", str)
EngineID = NewType("EngineID", str)
PlanID = NewType("PlanID", str)
TraceID = NewType("TraceID", str)
NodeID = NewType("NodeID", str)
ContractID = NewType("ContractID", str)


def _make_id(prefix: str) -> str:
    """Generate a time-sortable unique ID with the given prefix."""
    return f"{prefix}_{uuid.uuid4().hex[:24]}"


def new_object_id() -> ObjectID:
    return ObjectID(_make_id("so"))


def new_cell_id() -> CellID:
    return CellID(_make_id("cell"))


def new_engine_id() -> EngineID:
    return EngineID(_make_id("eng"))


def new_plan_id() -> PlanID:
    return PlanID(_make_id("plan"))


def new_trace_id() -> TraceID:
    return TraceID(_make_id("trace"))


def new_node_id() -> NodeID:
    return NodeID(_make_id("node"))


def now_utc() -> datetime:
    """Return current UTC datetime."""
    return datetime.now(timezone.utc)
