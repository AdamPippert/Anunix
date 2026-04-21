"""MemoryControlPlane — orchestrates taxonomy, graph, and retrieval.

Based on RFC-0004: Memory Control Plane.
"""

from __future__ import annotations

from typing import Any

from anunix.core.types import ObjectID
from anunix.memory.graph import MemoryGraph
from anunix.memory.taxonomy import TaxonomyIndex
from anunix.memory.tier import AdmissionProfile, MemoryTier
from anunix.state.object import StateObject, ValidationState


class MemoryPlacement:
    """Tracks which tiers an object resides in."""

    def __init__(self, object_id: ObjectID) -> None:
        self.object_id = object_id
        self.tiers: set[MemoryTier] = set()
        self.admission_profile: AdmissionProfile = AdmissionProfile.CACHEABLE
        self.access_count: int = 0


class MemoryControlPlane:
    """Orchestrates admission, taxonomy, graph, and retrieval for memory objects."""

    def __init__(self) -> None:
        self.taxonomy = TaxonomyIndex()
        self.graph = MemoryGraph()
        self._placements: dict[ObjectID, MemoryPlacement] = {}

    def admit(
        self,
        obj: StateObject,
        profile: AdmissionProfile = AdmissionProfile.RETRIEVAL_CANDIDATE,
    ) -> MemoryPlacement:
        """Admit a State Object into the memory system."""
        placement = MemoryPlacement(obj.id)
        placement.admission_profile = profile

        # Assign initial tier based on profile
        if profile == AdmissionProfile.EPHEMERAL_ONLY:
            placement.tiers.add(MemoryTier.L0)
        elif profile == AdmissionProfile.CACHEABLE:
            placement.tiers.add(MemoryTier.L1)
        elif profile in (AdmissionProfile.RETRIEVAL_CANDIDATE, AdmissionProfile.GRAPH_CANDIDATE):
            placement.tiers.add(MemoryTier.L2)
        elif profile == AdmissionProfile.LONG_TERM_CANDIDATE:
            placement.tiers.add(MemoryTier.L2)

        # Index taxonomy paths
        for path in obj.taxonomy_paths:
            self.taxonomy.assign(obj.id, path)

        # Index relations
        for rel in obj.relations:
            self.graph.add_relation(obj.id, ObjectID(rel.target), rel.type, rel.weight)

        self._placements[obj.id] = placement
        return placement

    def promote(self, object_id: ObjectID, target_tier: MemoryTier) -> bool:
        """Promote an object to a higher-value tier."""
        placement = self._placements.get(object_id)
        if not placement:
            return False
        placement.tiers.add(target_tier)
        return True

    def demote(self, object_id: ObjectID, tier: MemoryTier) -> bool:
        """Remove an object from a tier."""
        placement = self._placements.get(object_id)
        if not placement:
            return False
        placement.tiers.discard(tier)
        return True

    def link(
        self,
        source: ObjectID,
        target: ObjectID,
        relation_type: str,
        weight: float = 1.0,
    ) -> None:
        """Create a graph relation between two memory objects."""
        self.graph.add_relation(source, target, relation_type, weight)

    def retrieve(
        self,
        *,
        taxonomy_scope: str | None = None,
        relation_type: str | None = None,
        object_id: ObjectID | None = None,
    ) -> list[ObjectID]:
        """Simple retrieval across taxonomy and graph surfaces."""
        results: set[ObjectID] = set()

        if taxonomy_scope:
            results.update(self.taxonomy.get_objects(taxonomy_scope, recursive=True))

        if object_id and relation_type:
            neighbors = self.graph.get_neighbors(
                object_id, relation_type=relation_type, direction="both"
            )
            results.update(obj_id for obj_id, _ in neighbors)

        if object_id and not relation_type and not taxonomy_scope:
            neighbors = self.graph.get_neighbors(object_id, direction="both")
            results.update(obj_id for obj_id, _ in neighbors)

        return list(results)

    def forget(self, object_id: ObjectID) -> None:
        """Remove an object from all memory structures."""
        self._placements.pop(object_id, None)
        self.taxonomy.remove_object(object_id)
        self.graph.remove_node(object_id)

    def get_placement(self, object_id: ObjectID) -> MemoryPlacement | None:
        return self._placements.get(object_id)

    @property
    def total_objects(self) -> int:
        return len(self._placements)
