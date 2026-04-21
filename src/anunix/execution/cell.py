"""ExecutionCell — the canonical unit of work in Anunix.

Based on RFC-0003: Execution Cell Runtime.
"""

from __future__ import annotations

from datetime import datetime
from enum import Enum
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import CellID, ObjectID, PlanID, TraceID, new_cell_id, now_utc


class CellStatus(str, Enum):
    CREATED = "created"
    ADMITTED = "admitted"
    PLANNING = "planning"
    PLANNED = "planned"
    QUEUED = "queued"
    RUNNING = "running"
    WAITING = "waiting"
    VALIDATING = "validating"
    COMMITTING = "committing"
    COMPLETED = "completed"
    FAILED = "failed"
    CANCELLED = "cancelled"
    COMPENSATING = "compensating"
    COMPENSATED = "compensated"


class CellType(str, Enum):
    EXECUTION = "task.execution"
    BATCH_EXECUTION = "task.batch_execution"
    STREAM_EXECUTION = "task.stream_execution"
    RETRIEVAL = "task.retrieval"
    VALIDATION = "task.validation"
    SIDE_EFFECT = "task.side_effect"
    COMPENSATION = "task.compensation"


class Intent(BaseModel):
    name: str
    objective: str = ""
    requested_outputs: list[str] = Field(default_factory=list)
    success_condition: dict[str, Any] = Field(default_factory=dict)


class CellInput(BaseModel):
    name: str
    state_object_ref: ObjectID | None = None
    value: Any = None
    required: bool = True
    access_mode: str = "read"


class Constraints(BaseModel):
    max_latency_ms: int | None = None
    max_cost_usd: float | None = None
    privacy_scope: str = "private"
    allow_remote_execution: bool = False
    minimum_confidence: float | None = None


class RoutingPolicy(BaseModel):
    strategy: str = "local_first"
    allowed_engines: list[str] = Field(default_factory=list)
    disallowed_engines: list[str] = Field(default_factory=list)
    max_child_cells: int = 8


class ValidationPolicy(BaseModel):
    required: bool = True
    minimum_validation_state: str = "provisional"
    block_commit_on_failure: bool = True


class CommitPolicy(BaseModel):
    persist_outputs: bool = True
    promote_to_memory: bool = False
    write_trace: bool = True


class ExecutionPolicy(BaseModel):
    allow_network_access: bool = False
    allow_remote_models: bool = False
    allow_recursive_cells: bool = True
    max_recursion_depth: int = 3
    sandbox_profile: str = "standard"


class CellRuntime(BaseModel):
    attempt_count: int = 0
    current_engine: str = ""
    started_at: datetime | None = None
    completed_at: datetime | None = None


class CellError(BaseModel):
    code: str
    message: str
    retryable: bool = False


class ExecutionCell(BaseModel):
    """The canonical unit of work in Anunix."""

    id: CellID = Field(default_factory=new_cell_id)
    cell_type: CellType = CellType.EXECUTION
    schema_version: str = "1.0"
    revision: int = 1
    created_at: datetime = Field(default_factory=now_utc)
    updated_at: datetime = Field(default_factory=now_utc)
    status: CellStatus = CellStatus.CREATED
    intent: Intent
    inputs: list[CellInput] = Field(default_factory=list)
    constraints: Constraints = Field(default_factory=Constraints)
    routing_policy: RoutingPolicy = Field(default_factory=RoutingPolicy)
    validation_policy: ValidationPolicy = Field(default_factory=ValidationPolicy)
    commit_policy: CommitPolicy = Field(default_factory=CommitPolicy)
    execution_policy: ExecutionPolicy = Field(default_factory=ExecutionPolicy)
    runtime: CellRuntime = Field(default_factory=CellRuntime)
    parent_cell_ref: CellID | None = None
    child_cell_refs: list[CellID] = Field(default_factory=list)
    plan_ref: PlanID | None = None
    trace_ref: TraceID | None = None
    output_refs: list[ObjectID] = Field(default_factory=list)
    error: CellError | None = None
    ext: dict[str, Any] = Field(default_factory=dict)
