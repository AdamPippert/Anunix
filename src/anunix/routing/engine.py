"""RoutingEngine — selects the best execution path for a cell.

Implements feasibility filtering then scoring per RFC-0005.
"""

from __future__ import annotations

from enum import Enum
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import EngineID, PlanID, new_plan_id
from anunix.execution.cell import ExecutionCell
from anunix.routing.registry import CapabilityRegistry, EngineEntry, EngineStatus


class RouteStrategy(str, Enum):
    LOCAL_FIRST = "local_first"
    COST_FIRST = "cost_first"
    LATENCY_FIRST = "latency_first"
    CONFIDENCE_FIRST = "confidence_first"
    PRIVACY_FIRST = "privacy_first"
    ADAPTIVE = "adaptive"


class RouteCandidate(BaseModel):
    engine_id: EngineID
    engine_class: str
    score: float = 0.0
    reason: str = ""


class RoutePlan(BaseModel):
    """The selected execution route for a cell."""

    id: PlanID = Field(default_factory=new_plan_id)
    cell_id: str
    strategy: RouteStrategy
    selected_engine: EngineID
    candidates_considered: list[RouteCandidate] = Field(default_factory=list)
    excluded: list[dict[str, str]] = Field(default_factory=list)
    fallback_engines: list[EngineID] = Field(default_factory=list)


class RoutingEngine:
    """Routes execution cells to appropriate engines using feasibility + scoring."""

    def __init__(self, registry: CapabilityRegistry) -> None:
        self._registry = registry

    def plan(
        self,
        cell: ExecutionCell,
        *,
        strategy: RouteStrategy | None = None,
    ) -> RoutePlan:
        """Generate a route plan for the given cell."""
        strat = strategy or RouteStrategy(cell.routing_policy.strategy)
        available = self._registry.all_available()

        # Phase 1: Feasibility filtering
        feasible, excluded = self._filter_feasible(cell, available)

        if not feasible:
            from anunix.core.errors import NoValidRouteError
            raise NoValidRouteError(
                f"No feasible route for cell {cell.id} "
                f"(intent={cell.intent.name}, excluded={len(excluded)})"
            )

        # Phase 2: Score candidates
        candidates = self._score_candidates(cell, feasible, strat)
        candidates.sort(key=lambda c: c.score, reverse=True)

        selected = candidates[0]
        fallbacks = [c.engine_id for c in candidates[1:3]]

        return RoutePlan(
            cell_id=cell.id,
            strategy=strat,
            selected_engine=selected.engine_id,
            candidates_considered=candidates,
            excluded=[{"engine": e, "reason": r} for e, r in excluded],
            fallback_engines=fallbacks,
        )

    def _filter_feasible(
        self, cell: ExecutionCell, engines: list[EngineEntry]
    ) -> tuple[list[EngineEntry], list[tuple[str, str]]]:
        """Remove engines that cannot satisfy hard constraints."""
        feasible: list[EngineEntry] = []
        excluded: list[tuple[str, str]] = []

        allowed = set(cell.routing_policy.allowed_engines) if cell.routing_policy.allowed_engines else None
        disallowed = set(cell.routing_policy.disallowed_engines)

        for engine in engines:
            # Check explicit allow/disallow lists
            if allowed and engine.engine_id not in allowed and engine.engine_class.value not in allowed:
                excluded.append((engine.engine_id, "not in allowed_engines"))
                continue
            if engine.engine_id in disallowed or engine.engine_class.value in disallowed:
                excluded.append((engine.engine_id, "in disallowed_engines"))
                continue

            # Check remote policy
            if not cell.constraints.allow_remote_execution and engine.locality == "remote":
                excluded.append((engine.engine_id, "remote execution not allowed"))
                continue
            if not cell.execution_policy.allow_remote_models and engine.engine_class.value == "remote_model":
                excluded.append((engine.engine_id, "remote models not allowed"))
                continue

            feasible.append(engine)

        return feasible, excluded

    def _score_candidates(
        self,
        cell: ExecutionCell,
        engines: list[EngineEntry],
        strategy: RouteStrategy,
    ) -> list[RouteCandidate]:
        """Score feasible engines based on strategy."""
        candidates: list[RouteCandidate] = []

        for engine in engines:
            score = 0.0
            reason = ""

            if strategy == RouteStrategy.LOCAL_FIRST:
                score += 10.0 if engine.locality == "local" else 0.0
                score += 5.0 if engine.engine_class.value == "deterministic_tool" else 0.0
                score += 3.0 if engine.engine_class.value == "local_model" else 0.0
                reason = "local_first scoring"

            elif strategy == RouteStrategy.COST_FIRST:
                cost = engine.cost_model.per_call + engine.cost_model.per_token_input * 1000
                score = 10.0 - min(cost * 100, 10.0)
                reason = f"cost={cost:.4f}"

            elif strategy == RouteStrategy.PRIVACY_FIRST:
                score += 10.0 if "private_safe" in engine.policy_tags else 0.0
                score += 5.0 if engine.locality == "local" else 0.0
                reason = "privacy_first scoring"

            else:
                # Adaptive / default: balanced
                score += 5.0 if engine.locality == "local" else 2.0
                score += 3.0 if engine.cost_model.per_call == 0 else 1.0
                reason = "adaptive scoring"

            candidates.append(RouteCandidate(
                engine_id=engine.engine_id,
                engine_class=engine.engine_class.value,
                score=score,
                reason=reason,
            ))

        return candidates
