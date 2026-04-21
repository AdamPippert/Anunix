"""Capability Registry — inventory of available engines and their properties."""

from __future__ import annotations

from enum import Enum
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import EngineID


class EngineStatus(str, Enum):
    AVAILABLE = "available"
    DEGRADED = "degraded"
    OFFLINE = "offline"
    MAINTENANCE = "maintenance"


class EngineClass(str, Enum):
    DETERMINISTIC_TOOL = "deterministic_tool"
    LOCAL_MODEL = "local_model"
    REMOTE_MODEL = "remote_model"
    RETRIEVAL_SERVICE = "retrieval_service"
    GRAPH_SERVICE = "graph_service"
    VALIDATION_SERVICE = "validation_service"


class CostModel(BaseModel):
    kind: str = "free"
    per_call: float = 0.0
    per_token_input: float = 0.0
    per_token_output: float = 0.0


class EngineEntry(BaseModel):
    """A registered engine with its capabilities and constraints."""

    engine_id: EngineID
    engine_class: EngineClass
    status: EngineStatus = EngineStatus.AVAILABLE
    capabilities: list[str] = Field(default_factory=list)
    constraints: dict[str, Any] = Field(default_factory=dict)
    cost_model: CostModel = Field(default_factory=CostModel)
    policy_tags: list[str] = Field(default_factory=list)
    locality: str = "local"


class CapabilityRegistry:
    """Registry of available engines and their capabilities."""

    def __init__(self) -> None:
        self._engines: dict[EngineID, EngineEntry] = {}

    def register(self, entry: EngineEntry) -> None:
        self._engines[entry.engine_id] = entry

    def unregister(self, engine_id: EngineID) -> None:
        self._engines.pop(engine_id, None)

    def get(self, engine_id: EngineID) -> EngineEntry | None:
        return self._engines.get(engine_id)

    def find_by_capability(self, capability: str) -> list[EngineEntry]:
        """Find all engines that advertise a given capability."""
        return [
            e for e in self._engines.values()
            if capability in e.capabilities and e.status == EngineStatus.AVAILABLE
        ]

    def find_by_class(self, engine_class: EngineClass) -> list[EngineEntry]:
        """Find all available engines of a given class."""
        return [
            e for e in self._engines.values()
            if e.engine_class == engine_class and e.status == EngineStatus.AVAILABLE
        ]

    def all_available(self) -> list[EngineEntry]:
        """Return all currently available engines."""
        return [e for e in self._engines.values() if e.status == EngineStatus.AVAILABLE]

    def set_status(self, engine_id: EngineID, status: EngineStatus) -> None:
        entry = self._engines.get(engine_id)
        if entry:
            entry.status = status
