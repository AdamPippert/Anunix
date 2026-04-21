"""In-memory StateObjectStore backend — useful for testing."""

from __future__ import annotations

from anunix.core.types import ObjectID
from anunix.state.object import StateObject
from anunix.state.store import StateObjectStore


class InMemoryStore(StateObjectStore):
    """Non-persistent in-memory store for tests and ephemeral use."""

    def __init__(self) -> None:
        self._objects: dict[ObjectID, StateObject] = {}

    async def put(self, obj: StateObject) -> None:
        self._objects[obj.id] = obj

    async def get(self, object_id: ObjectID) -> StateObject | None:
        return self._objects.get(object_id)

    async def delete(self, object_id: ObjectID) -> bool:
        return self._objects.pop(object_id, None) is not None

    async def list_objects(
        self,
        *,
        obj_type: str | None = None,
        status: str | None = None,
        label: str | None = None,
        taxonomy_path: str | None = None,
        limit: int = 100,
    ) -> list[StateObject]:
        results: list[StateObject] = []
        for obj in self._objects.values():
            if obj_type and obj.type != obj_type:
                continue
            if status and obj.status.value != status:
                continue
            if label and label not in obj.labels:
                continue
            if taxonomy_path and taxonomy_path not in obj.taxonomy_paths:
                continue
            results.append(obj)
            if len(results) >= limit:
                break
        return results

    async def exists(self, object_id: ObjectID) -> bool:
        return object_id in self._objects
