"""StateObjectStore — abstract interface for persisting State Objects."""

from __future__ import annotations

from abc import ABC, abstractmethod

from anunix.core.types import ObjectID
from anunix.state.object import StateObject


class StateObjectStore(ABC):
    """Abstract base for State Object persistence backends."""

    @abstractmethod
    async def put(self, obj: StateObject) -> None:
        """Persist a State Object."""

    @abstractmethod
    async def get(self, object_id: ObjectID) -> StateObject | None:
        """Retrieve a State Object by ID. Returns None if not found."""

    @abstractmethod
    async def delete(self, object_id: ObjectID) -> bool:
        """Delete a State Object. Returns True if it existed."""

    @abstractmethod
    async def list_objects(
        self,
        *,
        obj_type: str | None = None,
        status: str | None = None,
        label: str | None = None,
        taxonomy_path: str | None = None,
        limit: int = 100,
    ) -> list[StateObject]:
        """Query State Objects by filters."""

    @abstractmethod
    async def exists(self, object_id: ObjectID) -> bool:
        """Check if a State Object exists."""
