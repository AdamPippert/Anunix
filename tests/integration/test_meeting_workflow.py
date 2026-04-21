"""Integration test: meeting recording workflow end-to-end."""

import pytest

from anunix.core.types import EngineID
from anunix.execution.cell import CellInput, ExecutionCell, Intent, RoutingPolicy
from anunix.execution.runtime import CellExecutor
from anunix.memory.plane import MemoryControlPlane
from anunix.memory.tier import AdmissionProfile, MemoryTier
from anunix.routing.registry import CapabilityRegistry, EngineClass, EngineEntry
from anunix.state.backends.memory import InMemoryStore
from anunix.state.object import (
    OriginKind,
    ProvenanceEntry,
    Relation,
    StateObject,
    ValidationRecord,
)
from anunix.validation.validator import ConfidenceThresholdRule, Validator


@pytest.mark.asyncio
async def test_meeting_workflow_end_to_end():
    """The canonical Anunix workflow: audio -> transcript -> summary -> actions -> memory."""
    # Setup
    store = InMemoryStore()
    memory = MemoryControlPlane()
    executor = CellExecutor()
    validator = Validator(rules=[ConfidenceThresholdRule(threshold=0.6)])

    executor.register_engine("transcriber", lambda cell, inputs: "Alice: Hello\nBob: Hi")
    executor.register_engine("summarizer", lambda cell, inputs: "Brief greeting exchanged")
    executor.register_engine("extractor", lambda cell, inputs: "1. [Alice] Follow up")

    # Step 1: Ingest audio
    audio = StateObject(
        type="media.audio",
        payload=b"fake_audio",
        provenance=ProvenanceEntry(origin_kind=OriginKind.INGESTED),
        taxonomy_paths=["work/meetings"],
    )
    await store.put(audio)

    # Step 2: Transcribe
    cell = ExecutionCell(
        intent=Intent(name="transcribe", requested_outputs=["document.transcript"]),
        inputs=[CellInput(name="audio", state_object_ref=audio.id)],
        routing_policy=RoutingPolicy(allowed_engines=["transcriber"]),
    )
    result = await executor.execute(cell, engine="transcriber", inputs={})
    assert result.success
    transcript = result.outputs[0]
    transcript.type = "document.transcript"
    transcript.taxonomy_paths = ["work/meetings"]
    transcript.validation = ValidationRecord(confidence=0.9)
    await store.put(transcript)

    # Step 3: Summarize
    cell = ExecutionCell(
        intent=Intent(name="summarize", requested_outputs=["memory.summary"]),
        inputs=[CellInput(name="transcript", state_object_ref=transcript.id)],
        routing_policy=RoutingPolicy(allowed_engines=["summarizer"]),
    )
    result = await executor.execute(cell, engine="summarizer", inputs={})
    assert result.success
    summary = result.outputs[0]
    summary.type = "memory.summary"
    summary.taxonomy_paths = ["work/meetings"]
    summary.relations = [Relation(type="derived_from", target=transcript.id)]
    summary.validation = ValidationRecord(confidence=0.85)
    await store.put(summary)

    # Step 4: Extract actions
    cell = ExecutionCell(
        intent=Intent(name="extract_actions", requested_outputs=["task.result"]),
        inputs=[CellInput(name="transcript", state_object_ref=transcript.id)],
        routing_policy=RoutingPolicy(allowed_engines=["extractor"]),
    )
    result = await executor.execute(cell, engine="extractor", inputs={})
    assert result.success
    actions = result.outputs[0]
    actions.type = "task.result"
    actions.taxonomy_paths = ["work/meetings", "work/actions"]
    actions.relations = [Relation(type="derived_from", target=transcript.id)]
    actions.validation = ValidationRecord(confidence=0.8)
    await store.put(actions)

    # Step 5: Validate
    for obj in [transcript, summary, actions]:
        vr = validator.validate(obj)
        assert vr.outcome.value in ("pass", "pass_with_warnings")

    # Step 6: Admit to memory
    memory.admit(audio, AdmissionProfile.RETRIEVAL_CANDIDATE)
    memory.admit(transcript, AdmissionProfile.RETRIEVAL_CANDIDATE)
    memory.admit(summary, AdmissionProfile.LONG_TERM_CANDIDATE)
    memory.admit(actions, AdmissionProfile.LONG_TERM_CANDIDATE)

    memory.promote(summary.id, MemoryTier.L4)
    memory.link(transcript.id, audio.id, "derived_from")
    memory.link(summary.id, transcript.id, "summarizes")
    memory.link(actions.id, transcript.id, "derived_from")

    # Step 7: Query
    meeting_objects = memory.retrieve(taxonomy_scope="work/meetings")
    assert len(meeting_objects) == 4

    derived = memory.retrieve(object_id=transcript.id, relation_type="derived_from")
    assert audio.id in derived

    action_objects = memory.retrieve(taxonomy_scope="work/actions")
    assert actions.id in action_objects

    # Verify persistence
    stored = await store.get(summary.id)
    assert stored is not None
    assert stored.payload == "Brief greeting exchanged"

    # Verify memory state
    assert memory.total_objects == 4
    assert memory.graph.edge_count == 3
    placement = memory.get_placement(summary.id)
    assert placement is not None
    assert MemoryTier.L4 in placement.tiers
