"""Retry engine — backoff computation and retry lifecycle for Execution Cells.

Implements RFC-0003 Section 22: Retry Policy.
"""

from __future__ import annotations

from datetime import timedelta

from anunix.core.errors import CellRetryExhaustedError
from anunix.core.types import now_utc
from anunix.execution.cell import (
    BackoffMode,
    CellStatus,
    ExecutionCell,
    RetryCondition,
    RetryPolicy,
)


def compute_backoff_ms(policy: RetryPolicy, attempt: int) -> int:
    """Return the backoff delay in milliseconds for the given attempt number.

    Attempt numbering starts at 1 (first retry).
    """
    if policy.backoff_mode == BackoffMode.CONSTANT:
        delay = policy.base_delay_ms
    elif policy.backoff_mode == BackoffMode.LINEAR:
        delay = policy.base_delay_ms * attempt
    elif policy.backoff_mode == BackoffMode.EXPONENTIAL:
        delay = policy.base_delay_ms * (2 ** (attempt - 1))
    else:
        delay = policy.base_delay_ms

    return min(delay, policy.max_delay_ms)


def should_retry(cell: ExecutionCell, failure_kind: RetryCondition) -> bool:
    """Determine whether a cell is eligible for retry given a failure kind."""
    if failure_kind not in cell.retry_policy.retry_on:
        return False
    return cell.runtime.attempt_count < cell.retry_policy.max_attempts


def prepare_retry(cell: ExecutionCell, failure_kind: RetryCondition) -> ExecutionCell:
    """Prepare a cell for its next retry attempt.

    Increments the attempt counter, computes the backoff delay, and
    transitions the cell to ``waiting`` status.  Returns a new cell
    instance (copy-on-write).

    Raises CellRetryExhaustedError if the cell is not eligible for retry.
    """
    if not should_retry(cell, failure_kind):
        raise CellRetryExhaustedError(
            f"Cell {cell.id} exhausted retries "
            f"(attempt {cell.runtime.attempt_count}/{cell.retry_policy.max_attempts})"
        )

    new_attempt = cell.runtime.attempt_count + 1
    delay_ms = compute_backoff_ms(cell.retry_policy, new_attempt)
    current = now_utc()

    new_runtime = cell.runtime.model_copy(
        update={
            "attempt_count": new_attempt,
            "last_failure_at": current,
            "next_retry_at": current + timedelta(milliseconds=delay_ms),
        }
    )

    return cell.model_copy(
        update={
            "status": CellStatus.WAITING,
            "runtime": new_runtime,
            "updated_at": current,
        }
    )
