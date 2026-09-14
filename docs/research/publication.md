# Daily research publication and release gates

Forgejo stores each daily sequence while the daily research program proceeds.
GitHub publication waits until every day is complete, the release notes are ready, and final testing passes.
The user's publication instruction governs this sequence.
The user extended the scope to later briefings on September 13, 2026.
The current queue contains 81 days through September 14.
Final release preparation includes another source check and completion of any later days.

## Daily publication

Each `codex/ai-os-day-NNN` branch contains its implementation, comparison, and validation evidence.
An incomplete branch can retain committed work on Forgejo, but its record must identify pending checks.
The `codex/ai-os-integration` branch advances after host tests, conformance, and both Jekyll VM boots pass and the day merges locally.

Forgejo `main` currently contains five commits outside the research branch history.
Those commits include software RAID and ARM64 build fixes.
The research integration branch preserves the tested daily sequence until final integration testing reconciles the histories.

## Final gates

The final publication requires all of these results:

| Gate | Required evidence |
| --- | --- |
| Daily completion | Every scoped entry identifies its merged change and passing Jekyll evidence. |
| Repository integration | The final candidate preserves the current Forgejo and GitHub changes and resolves conflicts. |
| Host validation | The complete host suite and conformance checks pass for the final candidate. |
| Guest validation | The final research kernel and UEFI image pass all daily regressions on Jekyll. |
| Release build | The production image boots on Jekyll, and its kernel excludes research commands and fault controls. |
| Artifact identity | The record links the source revision, build commands, image hashes, and test results. |
| Release notes | The notes describe delivered behavior, validation, compatibility changes, and remaining limitations. |

Release notes distinguish implemented behavior from broader research proposals and unmeasured performance claims.
A failing gate keeps the candidate on Forgejo.
After every gate passes, the combined change can merge to GitHub.

## Initial publication checks

At initial publication, Forgejo `main` was observed at `d7c8ba6`; GitHub `main` was observed at `0ee146a`.
These observations describe the publication checkpoint, not permanent upstream versions.
Final integration must inspect both remotes again.

The Forgejo database reports zero push mirrors for this repository.
The committed release workflow publishes to GitHub on release tag pushes.
Daily publication therefore pushes named research branches without release tags.
The current workflow is [release.yml](../../.forgejo/workflows/release.yml).

At initial publication, Days 001 through 012 passed the Jekyll checks and merged locally.
Day 013 was pending because Jekyll was offline.
The [daily review](daily-review.md) and [Day 013 comparison](day-013.md) provide the detailed records.
