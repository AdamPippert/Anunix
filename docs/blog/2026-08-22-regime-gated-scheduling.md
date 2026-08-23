# Anunix's scheduler doesn't trust AI by default: the regime-gate design

*2026-08-22*

Third post in this run — after staged effects and the capabilities/Sinks
split, this one's about the routing plane itself, and it's the piece
we'd been building toward the whole time: what happens when something
*proposes* a change to how Anunix routes work, instead of just executing
inside the boundaries that already exist.

The honest starting point is that nothing stopped this before. RFC-0005's
route planner scores engines against a cell's requirements and picks a
winner, and that scoring function has hardcoded constants — locality
bonus, cost penalties, a topology-affinity bonus, that kind of thing. If
something clever proposed different constants and they scored better on
one run, there was no gate anywhere that would ask "better than what,
compared how, and is that actually real or did you just get lucky." We
kept finding, across this whole research pass, that the interesting
design question was never "should an AI be allowed to change policy" —
it's "what has to be true before that's even worth asking."

So this pass is four pieces, and we built them as one thing rather than
four:

**A Resource Twin** — a frozen, value-copy snapshot of the engine
registry and queue depths, cheap enough to take before simulating
anything. It replays the exact same scoring dimensions the live planner
uses, just as tunable weights instead of hardcoded numbers, so you can
ask "who would win under this candidate policy" without touching
anything real. We proved this actually mirrors live routing, not just a
plausible parallel implementation of it: simulating with the *current*
weights against a snapshot taken right before a real routing decision
reproduces the same winner. If that stopped being true, the Twin would
be lying to you about what it's simulating, which is worse than not
having one.

**A Regime Detector** — two exponentially-weighted moving averages, one
slow and one fast, watching whatever metric you feed it. When they
diverge past a threshold, that's an escalation: something left the
normal operating envelope. When they reconverge, it drops back to
stable. That's it. It's deliberately dumb — no learning, no model, just
arithmetic you can trace by hand — because its whole job is to answer one
narrow question before anything smarter gets involved: is this even a
moment worth reconsidering policy, or is this just Tuesday.

**A Cognitive Envelope** — a token/reasoning budget a cell can declare at
admission time, before it's routed anywhere. Small, and honestly the
least finished piece of this pass: it's a real, tested primitive, but we
didn't wire it into the actual inference dispatch path, because that path
doesn't currently carry cell context through to where the budget would
apply. Same story as last post's external-call integration — build the
real thing, say clearly what it doesn't do yet, don't fake the rest.

**A measured-null promotion gate**, which is the one we think matters
most beyond scheduling specifically. Anunix already had a capability
promotion lifecycle — validate, then install — and a field on every
capability (`supersedes_oid`) for "this one's meant to replace an
existing installed one." Nothing checked that field before this. Now a
capability declaring an incumbent to replace has to clear a real trial
first: paired runs against the incumbent, and it has to win *every* pair
by a margin, not just win on average. The margin required goes up with
however many candidates you tried in the same round — so trying five
half-decent ideas and cherry-picking the best one doesn't get an easier
bar than trying one. We were tempted to reach for a real paired
significance test here and didn't: this kernel restricts floating point
to a handful of directories that accept a real constraint (never running
in interrupt context), and a promotion decision has no business asking
for that exception just to compute a standard deviation. So it's
deliberately a worse, stricter test than what the statistics literature
would recommend — takes the single worst paired result, not an average —
traded for being exact, deterministic, and safe to run anywhere in the
kernel without special casing. We wrote that tradeoff down rather than
letting it hide.

None of these four pieces are AI systems themselves. That's on purpose.
The regime gate decides whether to even ask; the Twin lets you ask
without touching anything real; the promotion gate decides whether a
proposed answer earned the right to matter. What runs in between —
whatever eventually generates a candidate policy — is a problem for
later. This pass was about making sure that, whenever it shows up, it
walks into a system that already knows how to say no.
