"""Tests for the Routing Engine."""

import pytest

from anunix.core.types import EngineID
from anunix.execution.cell import (
    Constraints,
    ExecutionCell,
    ExecutionPolicy,
    Intent,
    RoutingPolicy,
)
from anunix.routing.engine import RoutePlan, RouteStrategy, RoutingEngine
from anunix.routing.registry import (
    CapabilityRegistry,
    CostModel,
    EngineClass,
    EngineEntry,
    EngineStatus,
)


@pytest.fixture
def registry():
    reg = CapabilityRegistry()
    reg.register(EngineEntry(
        engine_id=EngineID("eng_local_tool"),
        engine_class=EngineClass.DETERMINISTIC_TOOL,
        capabilities=["structured_extraction", "schema_validation"],
        policy_tags=["private_safe"],
        locality="local",
    ))
    reg.register(EngineEntry(
        engine_id=EngineID("eng_local_model"),
        engine_class=EngineClass.LOCAL_MODEL,
        capabilities=["summarization", "question_answering"],
        cost_model=CostModel(kind="local_estimate"),
        policy_tags=["private_safe"],
        locality="local",
    ))
    reg.register(EngineEntry(
        engine_id=EngineID("eng_remote_model"),
        engine_class=EngineClass.REMOTE_MODEL,
        capabilities=["summarization", "long_context_reasoning"],
        cost_model=CostModel(kind="per_token", per_token_input=0.003, per_token_output=0.015),
        locality="remote",
    ))
    return reg


def test_registry_find_by_capability(registry):
    results = registry.find_by_capability("summarization")
    assert len(results) == 2
    ids = [r.engine_id for r in results]
    assert "eng_local_model" in ids
    assert "eng_remote_model" in ids


def test_registry_find_by_class(registry):
    results = registry.find_by_class(EngineClass.LOCAL_MODEL)
    assert len(results) == 1
    assert results[0].engine_id == "eng_local_model"


def test_registry_status_change(registry):
    registry.set_status(EngineID("eng_remote_model"), EngineStatus.OFFLINE)
    results = registry.find_by_capability("summarization")
    assert len(results) == 1
    assert results[0].engine_id == "eng_local_model"


def test_routing_local_first(registry):
    router = RoutingEngine(registry)
    cell = ExecutionCell(
        intent=Intent(name="summarize"),
        routing_policy=RoutingPolicy(strategy="local_first"),
    )
    plan = router.plan(cell)
    assert plan.selected_engine == "eng_local_tool"
    assert plan.strategy == RouteStrategy.LOCAL_FIRST
    # Remote engine excluded by default policy, so only 2 local candidates
    assert len(plan.candidates_considered) == 2


def test_routing_excludes_remote_when_not_allowed(registry):
    router = RoutingEngine(registry)
    cell = ExecutionCell(
        intent=Intent(name="summarize"),
        constraints=Constraints(allow_remote_execution=False),
        execution_policy=ExecutionPolicy(allow_remote_models=False),
    )
    plan = router.plan(cell)
    excluded_engines = [e["engine"] for e in plan.excluded]
    assert "eng_remote_model" in excluded_engines
    assert plan.selected_engine != "eng_remote_model"


def test_routing_cost_first(registry):
    router = RoutingEngine(registry)
    cell = ExecutionCell(
        intent=Intent(name="summarize"),
        constraints=Constraints(allow_remote_execution=True),
        execution_policy=ExecutionPolicy(allow_remote_models=True),
        routing_policy=RoutingPolicy(strategy="cost_first"),
    )
    plan = router.plan(cell)
    # Local engines are free, so they should score higher
    assert plan.selected_engine in ("eng_local_tool", "eng_local_model")


def test_routing_with_allowed_engines(registry):
    router = RoutingEngine(registry)
    cell = ExecutionCell(
        intent=Intent(name="extract"),
        routing_policy=RoutingPolicy(
            strategy="local_first",
            allowed_engines=["eng_local_model"],
        ),
    )
    plan = router.plan(cell)
    assert plan.selected_engine == "eng_local_model"
    assert len(plan.excluded) == 2


def test_routing_no_valid_route(registry):
    from anunix.core.errors import NoValidRouteError

    router = RoutingEngine(registry)
    cell = ExecutionCell(
        intent=Intent(name="impossible"),
        routing_policy=RoutingPolicy(
            allowed_engines=["eng_nonexistent"],
        ),
    )
    with pytest.raises(NoValidRouteError):
        router.plan(cell)


def test_routing_plan_has_fallbacks(registry):
    router = RoutingEngine(registry)
    cell = ExecutionCell(
        intent=Intent(name="summarize"),
        constraints=Constraints(allow_remote_execution=True),
        execution_policy=ExecutionPolicy(allow_remote_models=True),
        routing_policy=RoutingPolicy(strategy="local_first"),
    )
    plan = router.plan(cell)
    assert len(plan.fallback_engines) >= 1
