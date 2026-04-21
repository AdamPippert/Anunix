"""Filesystem-backed StateObjectStore — content-addressed local persistence."""

from __future__ import annotations

import json
from pathlib import Path

from anunix.core.types import ObjectID
from anunix.state.object import StateObject
from anunix.state.store import StateObjectStore


class FilesystemStore(StateObjectStore):
    """Persists State Objects as JSON files in a local directory.

    Layout:
        {root}/
            objects/
                {object_id}.json   — object envelope (metadata + inline payload)
            payloads/
                {object_id}.bin    — large binary payloads (if separated)
            index.json             — lightweight index for queries
    """

    def __init__(self, root: Path | str) -> None:
        self._root = Path(root).expanduser()
        self._objects_dir = self._root / "objects"
        self._payloads_dir = self._root / "payloads"
        self._objects_dir.mkdir(parents=True, exist_ok=True)
        self._payloads_dir.mkdir(parents=True, exist_ok=True)

    def _object_path(self, object_id: ObjectID) -> Path:
        return self._objects_dir / f"{object_id}.json"

    async def put(self, obj: StateObject) -> None:
        path = self._object_path(obj.id)
        data = obj.model_dump(mode="json")

        # Separate large binary payloads
        payload = data.get("payload")
        if isinstance(payload, (bytes, bytearray)):
            payload_path = self._payloads_dir / f"{obj.id}.bin"
            payload_path.write_bytes(payload)
            data["payload"] = None
            data["payload_ref"] = f"file://{payload_path}"

        path.write_text(json.dumps(data, indent=2, default=str))

    async def get(self, object_id: ObjectID) -> StateObject | None:
        path = self._object_path(object_id)
        if not path.exists():
            return None
        data = json.loads(path.read_text())

        # Rehydrate binary payload if stored separately
        payload_ref = data.get("payload_ref", "")
        if payload_ref.startswith("file://") and data.get("payload") is None:
            payload_path = Path(payload_ref.removeprefix("file://"))
            if payload_path.exists():
                data["payload"] = payload_path.read_bytes()

        return StateObject(**data)

    async def delete(self, object_id: ObjectID) -> bool:
        path = self._object_path(object_id)
        if not path.exists():
            return False
        path.unlink()
        payload_path = self._payloads_dir / f"{object_id}.bin"
        if payload_path.exists():
            payload_path.unlink()
        return True

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
        for path in sorted(self._objects_dir.glob("*.json"), reverse=True):
            try:
                data = json.loads(path.read_text())
            except (json.JSONDecodeError, OSError):
                continue

            if obj_type and data.get("type") != obj_type:
                continue
            if status and data.get("status") != status:
                continue
            if label and label not in data.get("labels", []):
                continue
            if taxonomy_path and taxonomy_path not in data.get("taxonomy_paths", []):
                continue

            results.append(StateObject(**data))
            if len(results) >= limit:
                break

        return results

    async def exists(self, object_id: ObjectID) -> bool:
        return self._object_path(object_id).exists()
