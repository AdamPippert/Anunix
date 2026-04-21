"""Tests for State Object store backends."""

import pytest

from anunix.state.backends.filesystem import FilesystemStore
from anunix.state.backends.memory import InMemoryStore
from anunix.state.object import StateObject


@pytest.fixture
def memory_store():
    return InMemoryStore()


@pytest.fixture
def fs_store(tmp_path):
    return FilesystemStore(tmp_path / "anunix_test")


@pytest.mark.asyncio
async def test_memory_store_put_get(memory_store):
    obj = StateObject(type="document.markdown", payload="# Hello")
    await memory_store.put(obj)
    retrieved = await memory_store.get(obj.id)
    assert retrieved is not None
    assert retrieved.id == obj.id
    assert retrieved.payload == "# Hello"


@pytest.mark.asyncio
async def test_memory_store_delete(memory_store):
    obj = StateObject(type="file.text", payload="data")
    await memory_store.put(obj)
    assert await memory_store.exists(obj.id)
    assert await memory_store.delete(obj.id)
    assert not await memory_store.exists(obj.id)


@pytest.mark.asyncio
async def test_memory_store_list_by_type(memory_store):
    await memory_store.put(StateObject(type="document.markdown", payload="a"))
    await memory_store.put(StateObject(type="document.markdown", payload="b"))
    await memory_store.put(StateObject(type="media.audio", payload="c"))

    results = await memory_store.list_objects(obj_type="document.markdown")
    assert len(results) == 2
    assert all(r.type == "document.markdown" for r in results)


@pytest.mark.asyncio
async def test_memory_store_list_by_label(memory_store):
    obj = StateObject(type="file.text", labels=["meeting", "important"])
    await memory_store.put(obj)
    await memory_store.put(StateObject(type="file.text", labels=["unrelated"]))

    results = await memory_store.list_objects(label="meeting")
    assert len(results) == 1
    assert results[0].id == obj.id


@pytest.mark.asyncio
async def test_fs_store_put_get(fs_store):
    obj = StateObject(
        type="document.transcript",
        payload="Meeting transcript content here",
        metadata={"speaker_count": 3},
        labels=["meeting"],
        taxonomy_paths=["work/meetings"],
    )
    await fs_store.put(obj)
    retrieved = await fs_store.get(obj.id)
    assert retrieved is not None
    assert retrieved.id == obj.id
    assert retrieved.type == "document.transcript"
    assert retrieved.payload == "Meeting transcript content here"
    assert retrieved.metadata["speaker_count"] == 3
    assert "meeting" in retrieved.labels


@pytest.mark.asyncio
async def test_fs_store_delete(fs_store):
    obj = StateObject(type="file.text", payload="temp")
    await fs_store.put(obj)
    assert await fs_store.exists(obj.id)
    assert await fs_store.delete(obj.id)
    assert not await fs_store.exists(obj.id)


@pytest.mark.asyncio
async def test_fs_store_list_by_type(fs_store):
    await fs_store.put(StateObject(type="memory.summary", payload="sum1"))
    await fs_store.put(StateObject(type="memory.summary", payload="sum2"))
    await fs_store.put(StateObject(type="file.binary", payload="bin"))

    results = await fs_store.list_objects(obj_type="memory.summary")
    assert len(results) == 2


@pytest.mark.asyncio
async def test_fs_store_persistence(tmp_path):
    store_path = tmp_path / "persist_test"
    store1 = FilesystemStore(store_path)
    obj = StateObject(type="document.markdown", payload="persistent data")
    await store1.put(obj)

    # New store instance pointing at same path should see the object
    store2 = FilesystemStore(store_path)
    retrieved = await store2.get(obj.id)
    assert retrieved is not None
    assert retrieved.payload == "persistent data"


@pytest.mark.asyncio
async def test_fs_store_get_nonexistent(fs_store):
    from anunix.core.types import ObjectID
    result = await fs_store.get(ObjectID("so_nonexistent"))
    assert result is None
