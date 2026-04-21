"""Taxonomic memory — hierarchical organization of State Objects."""

from __future__ import annotations

from anunix.core.types import ObjectID


class TaxonomyIndex:
    """Hierarchical index mapping taxonomy paths to object IDs.

    Paths are symbolic strings like "work/customers/acme/meetings".
    Objects may belong to multiple paths.
    """

    def __init__(self) -> None:
        # path -> set of object IDs
        self._index: dict[str, set[ObjectID]] = {}
        # object_id -> set of paths
        self._reverse: dict[ObjectID, set[str]] = {}

    def assign(self, object_id: ObjectID, path: str) -> None:
        """Assign an object to a taxonomy path."""
        self._index.setdefault(path, set()).add(object_id)
        self._reverse.setdefault(object_id, set()).add(path)

    def unassign(self, object_id: ObjectID, path: str) -> None:
        """Remove an object from a taxonomy path."""
        if path in self._index:
            self._index[path].discard(object_id)
        if object_id in self._reverse:
            self._reverse[object_id].discard(path)

    def get_objects(self, path: str, *, recursive: bool = False) -> set[ObjectID]:
        """Get all objects at a given path, optionally including sub-paths."""
        if not recursive:
            return set(self._index.get(path, set()))

        results: set[ObjectID] = set()
        for stored_path, ids in self._index.items():
            if stored_path == path or stored_path.startswith(path + "/"):
                results.update(ids)
        return results

    def get_paths(self, object_id: ObjectID) -> set[str]:
        """Get all taxonomy paths for an object."""
        return set(self._reverse.get(object_id, set()))

    def list_branches(self, prefix: str = "") -> list[str]:
        """List all known taxonomy paths under a prefix."""
        if not prefix:
            return sorted(self._index.keys())
        return sorted(p for p in self._index.keys() if p.startswith(prefix))

    def remove_object(self, object_id: ObjectID) -> None:
        """Remove an object from all taxonomy paths."""
        paths = self._reverse.pop(object_id, set())
        for path in paths:
            if path in self._index:
                self._index[path].discard(object_id)
