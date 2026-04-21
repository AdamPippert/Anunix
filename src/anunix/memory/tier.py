"""Memory tier definitions per RFC-0004."""

from __future__ import annotations

from enum import Enum


class MemoryTier(str, Enum):
    L0 = "L0"  # Active execution-local working set
    L1 = "L1"  # Local transient cache
    L2 = "L2"  # Local durable object store
    L3 = "L3"  # Local semantic retrieval tier
    L4 = "L4"  # Long-term structured memory
    L5 = "L5"  # Remote or federated memory extension


class FreshnessClass(str, Enum):
    VOLATILE = "volatile"
    SHORT_HORIZON = "short_horizon"
    MEDIUM_HORIZON = "medium_horizon"
    LONG_HORIZON = "long_horizon"
    ARCHIVAL = "archival"


class AdmissionProfile(str, Enum):
    EPHEMERAL_ONLY = "ephemeral_only"
    CACHEABLE = "cacheable"
    RETRIEVAL_CANDIDATE = "retrieval_candidate"
    LONG_TERM_CANDIDATE = "long_term_candidate"
    GRAPH_CANDIDATE = "graph_candidate"
    QUARANTINED = "quarantined"
