# Capabilities aren't enough: why Anunix separates "can call" from "can send"

*2026-08-22*

Last post, we wrote up staged effects — the change where an Execution
Cell can stage a State Object mutation and resolve it explicitly instead
of writing in place. This one's the thing we said we'd look at next:
separating "is this cell allowed to invoke this operation" from "is this
cell allowed to send this particular data through it."

Anunix already had an answer to the first question. Capability Objects
(RFC-0007) model installable competence — a capability gets validated,
installed, and becomes a routable engine. A cell's execution policy
(RFC-0003) separately gates whether it's allowed to perform side effects
or touch the network at all. What nothing in the kernel did was ask the
second question. A cell with a valid capability to make an external call,
and a capability to read a sensitive object, could combine them — read the
sensitive thing, send it through the call — and nothing would have
noticed that combination was happening, because each capability on its
own was perfectly legitimate.

This is exactly the gap a paper called Agent libOS closes in its August
revision, and it does it with a distinction that's easy to state and easy
to miss in practice: the tool definition a model sees (what it's *allowed
to ask for*) is not the same thing as authority over a protected resource,
which is not the same thing as where derived data is allowed to flow,
which is not the same thing as what proves any of it happened afterward.
Four separate properties. Most "agent OS" designs we've read collapse two
or three of them into a single permission bit and call it a capability
system. Agent libOS doesn't, and once you see the distinction it's hard
to unsee — a new Skill or generated tool can expand what a model can *ask*
for without automatically expanding what it's *authorized* to touch or
where the results of touching it can go.

**What changed:** State Objects now carry a sensitivity label — public,
internal, confidential, or restricted — defaulting to public, and
inherited as the *maximum* across a derived object's parents rather than
defaulting down to public just because someone forgot to set it
explicitly. Alongside that, we added Sinks: named destinations that
declare the sensitivity ceiling they're willing to receive. Checking
whether an object may flow to a Sink (`CAN_SEND`) is now a completely
separate function from checking whether a cell may invoke an operation at
all (`CAN_CALL`) — a cell with every capability it needs to make a call
can still be blocked from sending a `confidential` object through a Sink
whose ceiling is `internal`. Neither check implies the other, on purpose.

The other half of this is a state machine we're calling
prepare/dispatch/settle, for any effect that leaves the object store
entirely — network calls, external processes, anything credential-gated.
It's a small thing on paper: an effect moves from `PREPARED` to
`DISPATCHING`, and from there to exactly one of `COMMITTED`, `RESTORED`,
or `UNKNOWN`. The reason it exists is a failure mode that's obvious once
someone points it out and easy to get wrong otherwise: a timeout during
dispatch doesn't tell you whether the external thing happened. Treating
the timeout as failure and retrying risks doing it twice. Treating it as
success risks silently losing something that never actually happened. So
`UNKNOWN` is a real, permanent state — not a stand-in for "probably
failed" — and nothing can transition out of it. Whatever reads a
pending effect later and finds it unknown has to decide what to do about
that itself; the kernel isn't going to quietly resolve the ambiguity for
it.

One thing we didn't do: wire this into the live external-call dispatch
path by default. `anx_external_invoke` — the code that actually runs when
a cell makes an external call — already works end-to-end and already has
tests exercising it. Turning on mandatory `CAN_CALL`/`CAN_SEND` checks for
every existing external call, without first deciding which Sink each
scheme handler should check against, would have changed already-tested
behavior out from under it. So the primitives are real, and independently
tested on their own — a cell can prepare, dispatch, and settle an effect
today — but making that automatic for every `pg://` or `http://` call is
a deliberate follow-up, not something we backed into quietly here.
