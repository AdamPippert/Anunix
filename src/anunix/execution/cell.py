"""Execution Cell — policy-bound, provenance-producing unit of work.

Based on RFC-0003: Execution Cell Runtime.
"""

from __future__ import annotations

from datetime import datetime
from enum import Enum
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import CellID, new_cell_id, now_utc


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


class RetryCondition(str, Enum):
    TIMEOUT = "timeout"
    NETWORK_FAILURE = "network_failure"
    RETRYABLE_VALIDATION_FAIL = "retryable_validation_fail"


class BackoffMode(str, Enum):
    CONSTANT = "constant"
    LINEAR = "linear"
    EXPONENTIAL = "exponential"


class RetryPolicy(BaseModel):
    """Policy controlling how and when a cell retries after failure."""

    max_attempts: int = 3
    backoff_mode: BackoffMode = BackoffMode.EXPONENTIAL
    base_delay_ms: int = 1000
    max_delay_ms: int = 30000
    retry_on: list[RetryCondition] = Field(
        default_factory=lambda: [
            RetryCondition.TIMEOUT,
            RetryCondition.NETWORK_FAILURE,
            RetryCondition.RETRYABLE_VALIDATION_FAIL,
        ]
    )


class CellRuntime(BaseModel):
    """Mutable runtime state for an executing cell."""

    attempt_count: int = 0
    current_engine: str = ""
    last_failure_at: datetime | None = None
    next_retry_at: datetime | None = None


class ExecutionCell(BaseModel):
    """The canonical unit of execution in Anunix."""

    id: CellID = Field(default_factory=new_cell_id)
    cell_type: str = "task.execution"
    schema_version: str = "1.0"
    revision: int = 1
    created_at: datetime = Field(default_factory=now_utc)
    updated_at: datetime = Field(default_factory=now_utc)
    status: CellStatus = CellStatus.CREATED
    intent: dict[str, Any] = Field(default_factory=dict)
    inputs: list[dict[str, Any]] = Field(default_factory=list)
    constraints: dict[str, Any] = Field(default_factory=dict)
    routing_policy: dict[str, Any] = Field(default_factory=dict)
    validation_policy: dict[str, Any] = Field(default_factory=dict)
    commit_policy: dict[str, Any] = Field(default_factory=dict)
    execution_policy: dict[str, Any] = Field(default_factory=dict)
    retry_policy: RetryPolicy = Field(default_factory=RetryPolicy)
    runtime: CellRuntime = Field(default_factory=CellRuntime)
    parent_cell_ref: CellID | None = None
    child_cell_refs: list[CellID] = Field(default_factory=list)
    plan_ref: str | None = None
    trace_ref: str | None = None
    output_refs: list[str] = Field(default_factory=list)
    error: dict[str, Any] | None = None
    ext: dict[str, Any] = Field(default_factory=dict)
