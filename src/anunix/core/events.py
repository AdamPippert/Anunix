"""Internal event types for cross-component communication."""

from __future__ import annotations

from datetime import datetime
from enum import Enum
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import now_utc


class EventKind(str, Enum):
    STATE_OBJECT_CREATED = "state_object.created"
    STATE_OBJECT_UPDATED = "state_object.updated"
    STATE_OBJECT_DELETED = "state_object.deleted"
    CELL_CREATED = "cell.created"
    CELL_STATUS_CHANGED = "cell.status_changed"
    CELL_COMPLETED = "cell.completed"
    CELL_FAILED = "cell.failed"
    MEMORY_ADMITTED = "memory.admitted"
    MEMORY_PROMOTED = "memory.promoted"
    MEMORY_DEMOTED = "memory.demoted"
    MEMORY_FORGOTTEN = "memory.forgotten"
    ROUTE_PLANNED = "route.planned"
    ROUTE_SELECTED = "route.selected"
    VALIDATION_PASSED = "validation.passed"
    VALIDATION_FAILED = "validation.failed"
    PEER_CONNECTED = "network.peer_connected"
    PEER_DISCONNECTED = "network.peer_disconnected"


class Event(BaseModel):
    """A system event emitted by any component."""

    kind: EventKind
    timestamp: datetime = Field(default_factory=now_utc)
    source: str = ""
    payload: dict[str, Any] = Field(default_factory=dict)
