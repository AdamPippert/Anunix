"""Tests for the Execution Cell model and retry engine."""

import pytest

from anunix.core.errors import CellRetryExhaustedError
from anunix.execution.cell import (
    BackoffMode,
    CellRuntime,
    CellStatus,
    ExecutionCell,
    RetryCondition,
    RetryPolicy,
)
from anunix.execution.retry import compute_backoff_ms, prepare_retry, should_retry


def test_create_default_cell():
    cell = ExecutionCell()
    assert cell.id.startswith("cell_")
    assert cell.cell_type == "task.execution"
    assert cell.status == CellStatus.CREATED
    assert cell.runtime.attempt_count == 0
    assert cell.error is None


def test_retry_policy_defaults():
    policy = RetryPolicy()
    assert policy.max_attempts == 3
    assert policy.backoff_mode == BackoffMode.EXPONENTIAL
    assert policy.base_delay_ms == 1000
    assert policy.max_delay_ms == 30000
    assert RetryCondition.TIMEOUT in policy.retry_on
    assert RetryCondition.NETWORK_FAILURE in policy.retry_on
    assert RetryCondition.RETRYABLE_VALIDATION_FAIL in policy.retry_on


def test_compute_backoff_exponential():
    policy = RetryPolicy(backoff_mode=BackoffMode.EXPONENTIAL, base_delay_ms=1000)
    assert compute_backoff_ms(policy, 1) == 1000
    assert compute_backoff_ms(policy, 2) == 2000
    assert compute_backoff_ms(policy, 3) == 4000


def test_compute_backoff_linear():
    policy = RetryPolicy(backoff_mode=BackoffMode.LINEAR, base_delay_ms=1000)
    assert compute_backoff_ms(policy, 1) == 1000
    assert compute_backoff_ms(policy, 2) == 2000
    assert compute_backoff_ms(policy, 3) == 3000


def test_compute_backoff_constant():
    policy = RetryPolicy(backoff_mode=BackoffMode.CONSTANT, base_delay_ms=1000)
    assert compute_backoff_ms(policy, 1) == 1000
    assert compute_backoff_ms(policy, 2) == 1000
    assert compute_backoff_ms(policy, 5) == 1000


def test_compute_backoff_capped():
    policy = RetryPolicy(
        backoff_mode=BackoffMode.EXPONENTIAL,
        base_delay_ms=10000,
        max_delay_ms=30000,
    )
    # attempt 1: 10000, attempt 2: 20000, attempt 3: 40000 -> capped to 30000
    assert compute_backoff_ms(policy, 3) == 30000
    assert compute_backoff_ms(policy, 10) == 30000


def test_should_retry_true():
    cell = ExecutionCell(
        runtime=CellRuntime(attempt_count=1),
        retry_policy=RetryPolicy(max_attempts=3),
    )
    assert should_retry(cell, RetryCondition.TIMEOUT) is True


def test_should_retry_false_exhausted():
    cell = ExecutionCell(
        runtime=CellRuntime(attempt_count=3),
        retry_policy=RetryPolicy(max_attempts=3),
    )
    assert should_retry(cell, RetryCondition.TIMEOUT) is False


def test_should_retry_false_wrong_condition():
    cell = ExecutionCell(
        retry_policy=RetryPolicy(retry_on=[RetryCondition.TIMEOUT]),
    )
    assert should_retry(cell, RetryCondition.NETWORK_FAILURE) is False


def test_prepare_retry_transitions_to_waiting():
    cell = ExecutionCell(
        status=CellStatus.RUNNING,
        runtime=CellRuntime(attempt_count=0),
        retry_policy=RetryPolicy(max_attempts=3, base_delay_ms=500),
    )
    retried = prepare_retry(cell, RetryCondition.TIMEOUT)
    assert retried.status == CellStatus.WAITING
    assert retried.runtime.attempt_count == 1
    assert retried.runtime.last_failure_at is not None
    assert retried.runtime.next_retry_at is not None
    assert retried.runtime.next_retry_at > retried.runtime.last_failure_at
    # Original cell unchanged (copy-on-write)
    assert cell.status == CellStatus.RUNNING
    assert cell.runtime.attempt_count == 0


def test_prepare_retry_exhausted_raises():
    cell = ExecutionCell(
        runtime=CellRuntime(attempt_count=3),
        retry_policy=RetryPolicy(max_attempts=3),
    )
    with pytest.raises(CellRetryExhaustedError):
        prepare_retry(cell, RetryCondition.TIMEOUT)


def test_cell_unique_ids():
    a = ExecutionCell()
    b = ExecutionCell()
    assert a.id != b.id
