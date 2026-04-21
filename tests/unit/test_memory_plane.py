"""Tests for the Memory Control Plane."""

import pytest

from anunix.core.types import ObjectID
from anunix.memory.graph import MemoryGraph
from anunix.memory.plane import MemoryControlPlane
from anunix.memory.taxonomy import TaxonomyIndex
from anunix.memory.tier import AdmissionProfile, MemoryTier
from anunix.state.object import Relation, StateObject


# --- Taxonomy Tests ---

def test_taxonomy_assign_and_retrieve():
    idx = TaxonomyIndex()
    idx.assign(ObjectID("so_a"), "work/meetings")
    idx.assign(ObjectID("so_b"), "work/meetings")
    idx.assign(ObjectID("so_c"), "work/projects")

    assert idx.get_objects("work/meetings") == {ObjectID("so_a"), ObjectID("so_b")}
    assert idx.get_objects("work/projects") == {ObjectID("so_c")}


def test_taxonomy_recursive_retrieval():
    idx = TaxonomyIndex()
    idx.assign(ObjectID("so_a"), "work/customers/acme")
    idx.assign(ObjectID("so_b"), "work/customers/acme/meetings")
    idx.assign(ObjectID("so_c"), "work/internal")

    results = idx.get_objects("work/customers", recursive=True)
    assert ObjectID("so_a") in results
    assert ObjectID("so_b") in results
    assert ObjectID("so_c") not in results


def test_taxonomy_multiple_paths():
    idx = TaxonomyIndex()
    idx.assign(ObjectID("so_a"), "work/meetings")
    idx.assign(ObjectID("so_a"), "research/transcription")

    paths = idx.get_paths(ObjectID("so_a"))
    assert paths == {"work/meetings", "research/transcription"}


def test_taxonomy_remove_object():
    idx = TaxonomyIndex()
    idx.assign(ObjectID("so_a"), "work/meetings")
    idx.assign(ObjectID("so_a"), "work/projects")
    idx.remove_object(ObjectID("so_a"))

    assert idx.get_objects("work/meetings") == set()
    assert idx.get_paths(ObjectID("so_a")) == set()


# --- Graph Tests ---

def test_graph_add_and_query():
    g = MemoryGraph()
    g.add_relation(ObjectID("so_a"), ObjectID("so_b"), "derived_from")
    g.add_relation(ObjectID("so_a"), ObjectID("so_c"), "related_to", weight=0.8)

    neighbors = g.get_neighbors(ObjectID("so_a"), direction="forward")
    assert len(neighbors) == 2
    targets = [n[0] for n in neighbors]
    assert ObjectID("so_b") in targets
    assert ObjectID("so_c") in targets


def test_graph_reverse_lookup():
    g = MemoryGraph()
    g.add_relation(ObjectID("so_parent"), ObjectID("so_child"), "derived_from")

    reverse = g.get_neighbors(ObjectID("so_child"), direction="reverse")
    assert len(reverse) == 1
    assert reverse[0][0] == ObjectID("so_parent")


def test_graph_filter_by_type():
    g = MemoryGraph()
    g.add_relation(ObjectID("so_a"), ObjectID("so_b"), "derived_from")
    g.add_relation(ObjectID("so_a"), ObjectID("so_c"), "contradicts_claim")

    derived = g.get_neighbors(ObjectID("so_a"), relation_type="derived_from")
    assert len(derived) == 1
    assert derived[0][0] == ObjectID("so_b")


def test_graph_contradictions():
    g = MemoryGraph()
    g.add_relation(ObjectID("so_a"), ObjectID("so_b"), "contradicts_claim")
    g.add_relation(ObjectID("so_c"), ObjectID("so_a"), "contradicts_claim")

    contradictions = g.get_contradictions(ObjectID("so_a"))
    assert ObjectID("so_b") in contradictions
    assert ObjectID("so_c") in contradictions


# --- Memory Control Plane Tests ---

def test_plane_admit_object():
    plane = MemoryControlPlane()
    obj = StateObject(
        type="document.transcript",
        taxonomy_paths=["work/meetings"],
        labels=["customer"],
    )
    placement = plane.admit(obj, AdmissionProfile.RETRIEVAL_CANDIDATE)

    assert MemoryTier.L2 in placement.tiers
    assert placement.admission_profile == AdmissionProfile.RETRIEVAL_CANDIDATE
    assert plane.total_objects == 1


def test_plane_taxonomy_retrieval():
    plane = MemoryControlPlane()
    obj1 = StateObject(type="memory.summary", taxonomy_paths=["work/meetings"])
    obj2 = StateObject(type="memory.summary", taxonomy_paths=["work/meetings/acme"])
    obj3 = StateObject(type="file.text", taxonomy_paths=["personal/notes"])

    plane.admit(obj1)
    plane.admit(obj2)
    plane.admit(obj3)

    results = plane.retrieve(taxonomy_scope="work/meetings")
    assert obj1.id in results
    assert obj2.id in results
    assert obj3.id not in results


def test_plane_graph_linking_and_retrieval():
    plane = MemoryControlPlane()
    transcript = StateObject(type="document.transcript")
    summary = StateObject(type="memory.summary")
    plane.admit(transcript)
    plane.admit(summary)

    plane.link(summary.id, transcript.id, "derived_from")

    results = plane.retrieve(object_id=summary.id, relation_type="derived_from")
    assert transcript.id in results


def test_plane_promote_and_demote():
    plane = MemoryControlPlane()
    obj = StateObject(type="memory.fact_candidate")
    plane.admit(obj, AdmissionProfile.LONG_TERM_CANDIDATE)

    plane.promote(obj.id, MemoryTier.L4)
    placement = plane.get_placement(obj.id)
    assert placement is not None
    assert MemoryTier.L4 in placement.tiers

    plane.demote(obj.id, MemoryTier.L4)
    assert MemoryTier.L4 not in placement.tiers


def test_plane_forget():
    plane = MemoryControlPlane()
    obj = StateObject(type="memory.note", taxonomy_paths=["work/temp"])
    plane.admit(obj)
    assert plane.total_objects == 1

    plane.forget(obj.id)
    assert plane.total_objects == 0
    assert plane.taxonomy.get_objects("work/temp") == set()


def test_plane_admits_with_relations():
    plane = MemoryControlPlane()
    parent = StateObject(type="document.transcript")
    plane.admit(parent)

    child = StateObject(
        type="memory.summary",
        relations=[Relation(type="derived_from", target=parent.id)],
    )
    plane.admit(child)

    assert plane.graph.has_relation(child.id, parent.id)
