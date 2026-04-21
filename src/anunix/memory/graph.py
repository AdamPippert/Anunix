"""Graph memory — relationship tracking between State Objects using NetworkX."""

from __future__ import annotations

from typing import Any

import networkx as nx

from anunix.core.types import ObjectID


class MemoryGraph:
    """Directed graph for tracking relationships between State Objects.

    Backed by NetworkX DiGraph. Supports typed, weighted edges.
    """

    def __init__(self) -> None:
        self._graph: nx.DiGraph = nx.DiGraph()

    def add_relation(
        self,
        source: ObjectID,
        target: ObjectID,
        relation_type: str,
        weight: float = 1.0,
        **metadata: Any,
    ) -> None:
        """Add a typed relation between two objects."""
        self._graph.add_edge(
            source, target,
            relation_type=relation_type,
            weight=weight,
            **metadata,
        )

    def remove_relation(self, source: ObjectID, target: ObjectID) -> None:
        """Remove a relation between two objects."""
        if self._graph.has_edge(source, target):
            self._graph.remove_edge(source, target)

    def get_neighbors(
        self,
        object_id: ObjectID,
        *,
        relation_type: str | None = None,
        direction: str = "forward",
    ) -> list[tuple[ObjectID, dict[str, Any]]]:
        """Get adjacent objects with edge data.

        direction: "forward" (outgoing), "reverse" (incoming), or "both"
        """
        results: list[tuple[ObjectID, dict[str, Any]]] = []

        if direction in ("forward", "both"):
            for _, target, data in self._graph.out_edges(object_id, data=True):
                if relation_type and data.get("relation_type") != relation_type:
                    continue
                results.append((ObjectID(target), data))

        if direction in ("reverse", "both"):
            for source, _, data in self._graph.in_edges(object_id, data=True):
                if relation_type and data.get("relation_type") != relation_type:
                    continue
                results.append((ObjectID(source), data))

        return results

    def has_relation(self, source: ObjectID, target: ObjectID) -> bool:
        return self._graph.has_edge(source, target)

    def get_contradictions(self, object_id: ObjectID) -> list[ObjectID]:
        """Find objects that contradict the given object."""
        return [
            ObjectID(target)
            for _, target, data in self._graph.out_edges(object_id, data=True)
            if data.get("relation_type") == "contradicts_claim"
        ] + [
            ObjectID(source)
            for source, _, data in self._graph.in_edges(object_id, data=True)
            if data.get("relation_type") == "contradicts_claim"
        ]

    def remove_node(self, object_id: ObjectID) -> None:
        """Remove an object and all its relations from the graph."""
        if self._graph.has_node(object_id):
            self._graph.remove_node(object_id)

    @property
    def node_count(self) -> int:
        return self._graph.number_of_nodes()

    @property
    def edge_count(self) -> int:
        return self._graph.number_of_edges()
