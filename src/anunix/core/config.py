"""Configuration loading from TOML files."""

from __future__ import annotations

import tomllib
from pathlib import Path
from typing import Any

from pydantic import BaseModel, Field

_DEFAULT_CONFIG_PATH = Path(__file__).resolve().parents[3] / "config" / "default.toml"


class StateConfig(BaseModel):
    backend: str = "filesystem"
    content_addressed: bool = True
    default_retention_days: int = 30


class MemoryConfig(BaseModel):
    tiers: list[str] = Field(default_factory=lambda: ["L0", "L1", "L2", "L3", "L4", "L5"])
    hot_max_mb: int = 512
    embedding_model: str = "text-embedding-3-small"
    graph_backend: str = "networkx"
    chroma_persist_dir: str = "~/.anunix/chroma"


class ExecutionConfig(BaseModel):
    max_concurrent_cells: int = 16
    default_timeout_seconds: int = 300
    trace_enabled: bool = True


class RoutingConfig(BaseModel):
    default_strategy: str = "local_first"
    local_only: bool = False
    fallback_model: str = "gpt-4o-mini"


class SchedulerConfig(BaseModel):
    policy: str = "fifo"
    gpu_enabled: bool = False


class NetworkConfig(BaseModel):
    enabled: bool = False
    listen_address: str = "0.0.0.0:5550"
    heartbeat_interval_seconds: int = 30
    default_trust_zone: str = "untrusted-remote"


class AnunixConfig(BaseModel):
    version: str = "0.1.0"
    data_dir: str = "~/.anunix/data"
    log_level: str = "INFO"
    state: StateConfig = Field(default_factory=StateConfig)
    memory: MemoryConfig = Field(default_factory=MemoryConfig)
    execution: ExecutionConfig = Field(default_factory=ExecutionConfig)
    routing: RoutingConfig = Field(default_factory=RoutingConfig)
    scheduler: SchedulerConfig = Field(default_factory=SchedulerConfig)
    network: NetworkConfig = Field(default_factory=NetworkConfig)


def load_config(path: Path | None = None) -> AnunixConfig:
    """Load configuration from a TOML file, falling back to defaults."""
    if path is None:
        path = _DEFAULT_CONFIG_PATH

    if path.exists():
        with open(path, "rb") as f:
            raw = tomllib.load(f)
        top: dict[str, Any] = raw.get("anunix", {})
        top["state"] = raw.get("state", {})
        top["memory"] = raw.get("memory", {})
        top["execution"] = raw.get("execution", {})
        top["routing"] = raw.get("routing", {})
        top["scheduler"] = raw.get("scheduler", {})
        top["network"] = raw.get("network", {})
        return AnunixConfig(**top)

    return AnunixConfig()
