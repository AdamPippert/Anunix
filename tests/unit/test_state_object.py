"""Tests for the State Object model."""

from anunix.state.object import (
    ObjectStatus,
    OriginKind,
    StateObject,
    Transformation,
    ValidationState,
)


def test_create_default_state_object():
    obj = StateObject()
    assert obj.id.startswith("so_")
    assert obj.type == "file.binary"
    assert obj.status == ObjectStatus.ACTIVE
    assert obj.revision == 1
    assert obj.validation.state == ValidationState.UNVALIDATED


def test_create_typed_state_object():
    obj = StateObject(
        type="document.transcript",
        payload="Hello world",
        metadata={"language": "en", "speaker_count": 2},
        labels=["meeting", "test"],
        taxonomy_paths=["work/test/meetings"],
    )
    assert obj.type == "document.transcript"
    assert obj.payload == "Hello world"
    assert obj.metadata["language"] == "en"
    assert "meeting" in obj.labels
    assert obj.taxonomy_paths == ["work/test/meetings"]


def test_derive_state_object():
    parent = StateObject(type="document.transcript", payload="full transcript text")
    child = parent.derive(
        new_type="memory.summary",
        payload="summary of the transcript",
        transformation=Transformation(
            name="summarize_v1",
            kind="model_pipeline",
            engine="local_qwen3_8b",
        ),
    )
    assert child.type == "memory.summary"
    assert child.provenance.origin_kind == OriginKind.DERIVED
    assert parent.id in child.provenance.source_refs
    assert child.provenance.transformation is not None
    assert child.provenance.transformation.name == "summarize_v1"
    assert child.id != parent.id


def test_state_object_unique_ids():
    a = StateObject()
    b = StateObject()
    assert a.id != b.id


def test_state_object_policy_defaults():
    obj = StateObject()
    assert obj.policy.access.scope == "private"
    assert obj.policy.execution.allow_local_models is True
    assert obj.policy.execution.allow_remote_models is False
    assert obj.policy.replication.mode == "local_only"
