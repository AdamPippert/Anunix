"""StateObject — the atomic unit of structured state in Anunix.

Based on RFC-0002: State Object Model.
"""

from __future__ import annotations

from datetime import datetime
from enum import Enum
from typing import Any

from pydantic import BaseModel, Field

from anunix.core.types import ObjectID, new_object_id, now_utc


class ObjectStatus(str, Enum):
    ACTIVE = "active"
    ARCHIVED = "archived"
    DELETED = "deleted"
    QUARANTINED = "quarantined"
    STAGED = "staged"
    EPHEMERAL = "ephemeral"


class ValidationState(str, Enum):
    UNVALIDATED = "unvalidated"
    PROVISIONAL = "provisional"
    VALIDATED = "validated"
    CONTESTED = "contested"
    REJECTED = "rejected"
    STALE = "stale"


class OriginKind(str, Enum):
    INGESTED = "ingested"
    CREATED = "created"
    DERIVED = "derived"
    IMPORTED = "imported"
    REPLICATED = "replicated"
    RECOVERED = "recovered"


class Transformation(BaseModel):
    name: str
    kind: str = ""
    engine: str = ""
    version: str = ""


class ProvenanceEntry(BaseModel):
    origin_kind: OriginKind = OriginKind.CREATED
    created_by: dict[str, str] = Field(default_factory=dict)
    source_refs: list[ObjectID] = Field(default_factory=list)
    transformation: Transformation | None = None


class AccessPolicy(BaseModel):
    scope: str = "private"
    principals: list[str] = Field(default_factory=list)


class RetentionPolicy(BaseModel):
    retention_class: str = "long_term"
    expires_at: datetime | None = None


class ExecutionPolicy(BaseModel):
    allow_local_models: bool = True
    allow_remote_models: bool = False
    allow_export: bool = False


class ReplicationPolicy(BaseModel):
    mode: str = "local_only"


class PromotionPolicy(BaseModel):
    eligible_tiers: list[str] = Field(default_factory=lambda: ["L2", "L3", "L4"])


class Policy(BaseModel):
    access: AccessPolicy = Field(default_factory=AccessPolicy)
    retention: RetentionPolicy = Field(default_factory=RetentionPolicy)
    execution: ExecutionPolicy = Field(default_factory=ExecutionPolicy)
    replication: ReplicationPolicy = Field(default_factory=ReplicationPolicy)
    promotion: PromotionPolicy = Field(default_factory=PromotionPolicy)


class ValidationRecord(BaseModel):
    state: ValidationState = ValidationState.UNVALIDATED
    confidence: float | None = None
    methods: list[dict[str, Any]] = Field(default_factory=list)
    issues: list[dict[str, Any]] = Field(default_factory=list)


class Relation(BaseModel):
    type: str
    target: str
    weight: float = 1.0


class Representation(BaseModel):
    name: str
    content_type: str = ""
    ref: str = ""
    created_at: datetime = Field(default_factory=now_utc)


class StateObject(BaseModel):
    """The canonical unit of structured state in Anunix."""

    id: ObjectID = Field(default_factory=new_object_id)
    type: str = "file.binary"
    schema_version: str = "1.0"
    revision: int = 1
    created_at: datetime = Field(default_factory=now_utc)
    updated_at: datetime = Field(default_factory=now_utc)
    status: ObjectStatus = ObjectStatus.ACTIVE
    payload_ref: str = ""
    payload: Any = None
    metadata: dict[str, Any] = Field(default_factory=dict)
    provenance: ProvenanceEntry = Field(default_factory=ProvenanceEntry)
    policy: Policy = Field(default_factory=Policy)
    validation: ValidationRecord = Field(default_factory=ValidationRecord)
    relations: list[Relation] = Field(default_factory=list)
    labels: list[str] = Field(default_factory=list)
    taxonomy_paths: list[str] = Field(default_factory=list)
    representations: list[Representation] = Field(default_factory=list)
    ext: dict[str, Any] = Field(default_factory=dict)

    def derive(
        self,
        new_type: str,
        payload: Any,
        transformation: Transformation,
        **kwargs: Any,
    ) -> StateObject:
        """Create a new StateObject derived from this one."""
        return StateObject(
            type=new_type,
            payload=payload,
            provenance=ProvenanceEntry(
                origin_kind=OriginKind.DERIVED,
                source_refs=[self.id],
                transformation=transformation,
            ),
            **kwargs,
        )
