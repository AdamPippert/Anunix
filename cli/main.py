"""Anunix CLI entry point."""

from __future__ import annotations

import click

from anunix import __version__


@click.group()
@click.version_option(version=__version__, prog_name="anunix")
def cli() -> None:
    """Anunix — AI-native operating system framework."""


@cli.group()
def state() -> None:
    """State Object operations."""


@state.command("create")
@click.option("--type", "obj_type", default="file.text", help="Object type")
@click.option("--file", "file_path", type=click.Path(exists=True), help="Payload file")
def state_create(obj_type: str, file_path: str | None) -> None:
    """Create a new State Object."""
    from anunix.state.object import StateObject

    obj = StateObject(type=obj_type)
    if file_path:
        with open(file_path) as f:
            obj.payload = f.read()
    click.echo(f"Created: {obj.id} (type={obj.type})")


@cli.group()
def system() -> None:
    """System operations."""


@system.command("status")
def system_status() -> None:
    """Show system status."""
    from anunix.core.config import load_config

    config = load_config()
    click.echo(f"Anunix v{config.version}")
    click.echo(f"  State backend: {config.state.backend}")
    click.echo(f"  Routing strategy: {config.routing.default_strategy}")
    click.echo(f"  Network: {'enabled' if config.network.enabled else 'disabled'}")
    click.echo(f"  Scheduler: {config.scheduler.policy}")


if __name__ == "__main__":
    cli()
