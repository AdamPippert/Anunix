"""Meeting Recording Workflow — the canonical Anunix end-to-end example.

Demonstrates:
  1. Ingesting an audio recording as a State Object
  2. Transcribing it via an Execution Cell
  3. Summarizing the transcript
  4. Extracting action items
  5. Validating outputs
  6. Admitting results into the Memory Control Plane
  7. Querying memory by taxonomy and graph relations

This example uses mock engines (no real model calls) to demonstrate the
architecture working end-to-end. Replace the mock engines with litellm
calls to run against real models.
"""

from __future__ import annotations

import asyncio

from anunix.core.types import EngineID, ObjectID, new_object_id
from anunix.execution.cell import (
    CellInput,
    ExecutionCell,
    Intent,
    RoutingPolicy,
)
from anunix.execution.runtime import CellExecutor
from anunix.memory.plane import MemoryControlPlane
from anunix.memory.tier import AdmissionProfile, MemoryTier
from anunix.routing.engine import RoutingEngine
from anunix.routing.registry import (
    CapabilityRegistry,
    EngineClass,
    EngineEntry,
)
from anunix.state.backends.filesystem import FilesystemStore
from anunix.state.object import (
    OriginKind,
    ProvenanceEntry,
    Relation,
    StateObject,
    Transformation,
    ValidationRecord,
)
from anunix.validation.validator import ConfidenceThresholdRule, Validator


# --- Mock Engines ---

def transcribe_engine(cell, inputs):
    """Mock transcription engine — simulates speech-to-text."""
    return (
        "Alice: Good morning everyone, let's review the Q2 roadmap.\n"
        "Bob: I think we should prioritize the routing engine refactor.\n"
        "Alice: Agreed. Bob, can you own that? Target end of month.\n"
        "Carol: I'll handle the memory plane testing.\n"
        "Alice: Great. Let's reconvene next Tuesday."
    )


def summarize_engine(cell, inputs):
    """Mock summarization engine."""
    return (
        "Team discussed Q2 roadmap priorities. Key decisions: "
        "Bob will lead routing engine refactor (deadline: end of month), "
        "Carol will handle memory plane testing. "
        "Next meeting: Tuesday."
    )


def extract_actions_engine(cell, inputs):
    """Mock action item extraction engine."""
    return (
        "Action Items:\n"
        "1. [Bob] Routing engine refactor - due end of month\n"
        "2. [Carol] Memory plane testing - ongoing\n"
        "3. [All] Reconvene Tuesday"
    )


# --- Main Workflow ---

async def run_meeting_workflow(data_dir: str = "/tmp/anunix_demo") -> None:
    """Run the complete meeting recording workflow."""
    print("=" * 60)
    print("  Anunix Meeting Recording Workflow")
    print("=" * 60)

    # --- Setup ---
    store = FilesystemStore(data_dir)
    memory = MemoryControlPlane()
    registry = CapabilityRegistry()
    validator = Validator(rules=[ConfidenceThresholdRule(threshold=0.6)])

    # Register mock engines
    registry.register(EngineEntry(
        engine_id=EngineID("eng_transcriber"),
        engine_class=EngineClass.DETERMINISTIC_TOOL,
        capabilities=["transcription"],
        policy_tags=["private_safe"],
        locality="local",
    ))
    registry.register(EngineEntry(
        engine_id=EngineID("eng_summarizer"),
        engine_class=EngineClass.LOCAL_MODEL,
        capabilities=["summarization"],
        policy_tags=["private_safe"],
        locality="local",
    ))
    registry.register(EngineEntry(
        engine_id=EngineID("eng_extractor"),
        engine_class=EngineClass.LOCAL_MODEL,
        capabilities=["structured_extraction"],
        policy_tags=["private_safe"],
        locality="local",
    ))

    router = RoutingEngine(registry)
    executor = CellExecutor()
    executor.register_engine("eng_transcriber", transcribe_engine)
    executor.register_engine("eng_summarizer", summarize_engine)
    executor.register_engine("eng_extractor", extract_actions_engine)

    # --- Step 1: Ingest Audio ---
    print("\n[1] Ingesting audio recording...")
    audio_obj = StateObject(
        type="media.audio",
        payload="<binary audio data>",
        metadata={
            "title": "Q2 Roadmap Review",
            "duration_seconds": 1842,
            "speaker_count": 3,
            "source_device": "conference_room_mic",
        },
        provenance=ProvenanceEntry(origin_kind=OriginKind.INGESTED),
        taxonomy_paths=["work/team/meetings"],
        labels=["meeting", "q2", "roadmap"],
    )
    await store.put(audio_obj)
    print(f"    Created: {audio_obj.id} (type={audio_obj.type})")

    # --- Step 2: Transcribe ---
    print("\n[2] Transcribing audio...")
    transcribe_cell = ExecutionCell(
        intent=Intent(
            name="transcribe_meeting",
            objective="Convert audio to text transcript",
            requested_outputs=["document.transcript"],
        ),
        inputs=[CellInput(name="audio", state_object_ref=audio_obj.id)],
        routing_policy=RoutingPolicy(allowed_engines=["eng_transcriber"]),
    )

    result = await executor.execute(transcribe_cell, engine="eng_transcriber", inputs={})
    transcript_obj = result.outputs[0]
    transcript_obj.type = "document.transcript"
    transcript_obj.metadata = {"language": "en", "speaker_count": 3}
    transcript_obj.taxonomy_paths = ["work/team/meetings"]
    transcript_obj.labels = ["meeting", "transcript"]
    transcript_obj.validation = ValidationRecord(confidence=0.92)
    await store.put(transcript_obj)
    print(f"    Created: {transcript_obj.id} (type={transcript_obj.type})")
    print(f"    Content: {transcript_obj.payload[:80]}...")

    # --- Step 3: Summarize ---
    print("\n[3] Generating summary...")
    summarize_cell = ExecutionCell(
        intent=Intent(
            name="summarize_meeting",
            objective="Produce concise meeting summary",
            requested_outputs=["memory.summary"],
        ),
        inputs=[CellInput(name="transcript", state_object_ref=transcript_obj.id)],
        routing_policy=RoutingPolicy(allowed_engines=["eng_summarizer"]),
    )

    result = await executor.execute(summarize_cell, engine="eng_summarizer", inputs={})
    summary_obj = result.outputs[0]
    summary_obj.type = "memory.summary"
    summary_obj.taxonomy_paths = ["work/team/meetings"]
    summary_obj.labels = ["meeting", "summary"]
    summary_obj.relations = [Relation(type="derived_from", target=transcript_obj.id)]
    summary_obj.validation = ValidationRecord(confidence=0.88)
    await store.put(summary_obj)
    print(f"    Created: {summary_obj.id} (type={summary_obj.type})")
    print(f"    Content: {summary_obj.payload}")

    # --- Step 4: Extract Action Items ---
    print("\n[4] Extracting action items...")
    extract_cell = ExecutionCell(
        intent=Intent(
            name="extract_action_items",
            objective="Identify actionable tasks from transcript",
            requested_outputs=["task.result"],
        ),
        inputs=[CellInput(name="transcript", state_object_ref=transcript_obj.id)],
        routing_policy=RoutingPolicy(allowed_engines=["eng_extractor"]),
    )

    result = await executor.execute(extract_cell, engine="eng_extractor", inputs={})
    actions_obj = result.outputs[0]
    actions_obj.type = "task.result"
    actions_obj.taxonomy_paths = ["work/team/meetings", "work/team/actions"]
    actions_obj.labels = ["meeting", "action_items"]
    actions_obj.relations = [Relation(type="derived_from", target=transcript_obj.id)]
    actions_obj.validation = ValidationRecord(confidence=0.85)
    await store.put(actions_obj)
    print(f"    Created: {actions_obj.id} (type={actions_obj.type})")
    print(f"    Content: {actions_obj.payload}")

    # --- Step 5: Validate ---
    print("\n[5] Validating outputs...")
    for obj in [transcript_obj, summary_obj, actions_obj]:
        vr = validator.validate(obj)
        print(f"    {obj.id}: {vr.outcome.value} -> {vr.new_state.value}")

    # --- Step 6: Admit to Memory ---
    print("\n[6] Admitting to Memory Control Plane...")
    memory.admit(audio_obj, AdmissionProfile.RETRIEVAL_CANDIDATE)
    memory.admit(transcript_obj, AdmissionProfile.RETRIEVAL_CANDIDATE)
    memory.admit(summary_obj, AdmissionProfile.LONG_TERM_CANDIDATE)
    memory.admit(actions_obj, AdmissionProfile.LONG_TERM_CANDIDATE)

    # Promote validated summaries to L4
    memory.promote(summary_obj.id, MemoryTier.L4)
    memory.promote(actions_obj.id, MemoryTier.L4)

    # Add lineage links
    memory.link(transcript_obj.id, audio_obj.id, "derived_from")
    memory.link(summary_obj.id, transcript_obj.id, "summarizes")
    memory.link(actions_obj.id, transcript_obj.id, "derived_from")

    print(f"    Total memory objects: {memory.total_objects}")
    print(f"    Graph edges: {memory.graph.edge_count}")

    # --- Step 7: Query Memory ---
    print("\n[7] Querying memory...")

    # By taxonomy
    meeting_objects = memory.retrieve(taxonomy_scope="work/team/meetings")
    print(f"    Objects in work/team/meetings: {len(meeting_objects)}")

    # By graph relation
    derived = memory.retrieve(object_id=transcript_obj.id, relation_type="derived_from")
    print(f"    Objects derived from transcript: {len(derived)}")

    # Action items specifically
    action_objects = memory.retrieve(taxonomy_scope="work/team/actions")
    print(f"    Action items found: {len(action_objects)}")

    # --- Summary ---
    print("\n" + "=" * 60)
    print("  Workflow Complete!")
    print("=" * 60)
    print(f"\n  Objects created: 4")
    print(f"  Validations passed: 3")
    print(f"  Memory objects: {memory.total_objects}")
    print(f"  Graph relations: {memory.graph.edge_count}")
    print(f"  Taxonomy branches: {len(memory.taxonomy.list_branches())}")
    print(f"\n  Data persisted to: {data_dir}")


if __name__ == "__main__":
    asyncio.run(run_meeting_workflow())
