# Staged effects: why Anunix objects can now stage, commit, or abort a mutation

*2026-08-22*

We've spent a chunk of this year tracking the AI-first-OS research
landscape — the wave of papers and experimental systems trying to
figure out what an operating system looks like when a meaningful
fraction of the code running on it is an agent, not a human at a
terminal. Most of that material doesn't translate cleanly to Anunix:
a lot of it assumes you're building a control plane that sits on top
of an existing Linux box, wiring agent policy into cgroups and eBPF
and sched_ext. Anunix doesn't have any of that underneath it — it's a
from-scratch kernel, so "wrap Linux's scheduler" isn't an option, and
usually isn't even the interesting part of the idea once you strip
the Linux-specific packaging away.

One thread was different. A few different groups, working
independently, kept arriving at some version of the same claim: when
an agent changes something on disk, that change shouldn't be
immediate and irreversible by default. It should be *staged* —
visible to the agent that made it, invisible to everything else,
inspectable, and undoable — right up until something decides it's
correct enough to commit.

The clearest version of this is a project called YoloFS, which builds
this directly into a Linux filesystem: three primitives —
introspect, undo, gate — instead of trying to predict which shell
commands are dangerous before they run. A related paper, on
"Agentic Transactions," made the case more abstractly: agent effects
should have the same four properties database transactions have —
atomicity, consistency, isolation, durability — even when the
reasoning that produced them isn't reproducible run to run. You don't
need the agent's thought process to replay identically. You need the
effect to either fully happen or not happen at all, and you need
concurrent agents not to see each other's half-finished work.

Neither of those is Anunix-shaped as written. But the claim underneath
them isn't tied to Linux, or to filesystems specifically. It's an
object-lifecycle claim, and Anunix already has an object lifecycle:
every unit of persistent state (RFC-0002's State Object) already
carries a version number and an append-only provenance log. So instead
of building a new staged filesystem, we extended the thing we already
had.

**What changed:** a State Object handle can now open a stage
(`anx_object_stage`), write against a private shadow copy of the
payload, and resolve it explicitly — `anx_object_commit`, which
atomically publishes the shadow as the live payload and bumps the
version exactly once, or `anx_object_abort`, which throws the shadow
away and leaves the live object completely untouched. Nothing else in
the system can see a staged write until it's committed.

The one detail we borrowed directly from YoloFS: an aborted stage
still gets recorded. The authors' reasoning is that rollback and
erasing evidence are different things — if an agent attempted
something and it got discarded, that's exactly the kind of event an
audit trail should show, not quietly omit. So an abort writes a new
provenance event type instead of just vanishing.

We also gave Execution Cells — Anunix's replacement for the process —
a way to *declare* how strict they want to be about this, before they
run: a `consistency` class (best-effort, semantic, or transactional)
and an `effect_mode` (direct, the old default, or staged). Declaring
nothing gets you exactly today's behavior. That was a hard constraint
going in — this needed to be additive, not a rewrite of how every
existing cell works.

One thing we deliberately didn't build yet: full wiring from a
staged-effect cell's commit phase into real output writes. The
runtime's commit step is still a stub in a few places pending the
Memory Control Plane work, so there's nothing to wire the staging API
into today. The primitive is real and tested on its own; the
end-to-end path lands when that piece does.

Next up, we're looking at a companion idea from a slightly later
research cycle — separating "can call this operation" from "can send
this data to that destination," which turned out to matter more than
we expected once we started reading about how agent frameworks handle
credentials and exfiltration risk.
