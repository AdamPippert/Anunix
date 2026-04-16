# Anunix RFC Index

This directory contains the formal Request for Comments (RFC) documents for the Anunix AI-Native Operating System.

## RFC Status Legend

| Status | Meaning |
|--------|---------|
| Draft | Under active development |
| Review | Open for community review |
| Accepted | Approved for implementation |
| Superseded | Replaced by a newer RFC |

## RFCs

| RFC | Title | Status | Author | Depends On |
|-----|-------|--------|--------|------------|
| [0001](RFC-0001-architecture-thesis.md) | Architecture Thesis | Draft | Adam Pippert | — |
| [0002](RFC-0002-state-object-model.md) | State Object Model | Draft | Adam Pippert | 0001 |
| [0003](RFC-0003-execution-cell-runtime.md) | Execution Cell Runtime | Draft | Adam Pippert | 0001, 0002 |
| [0004](RFC-0004-memory-control-plane.md) | Memory Control Plane | Draft | Adam Pippert | 0001, 0002, 0003 |
| [0005](RFC-0005-routing-and-scheduler.md) | Routing Plane and Unified Scheduler | Draft | Adam Pippert | 0001, 0002, 0003, 0004 |
| [0006](RFC-0006-network-plane.md) | Network Plane and Federated Execution | Draft | Adam Pippert | 0001, 0002, 0003, 0004, 0005 |
| [0007](RFC-0007-capability-objects.md) | Capability Objects and Runtime Installation | Draft | Adam Pippert | 0001, 0002, 0003, 0004, 0005, 0006 |
| [0008](RFC-0008-credential-objects.md) | Credential Objects | Draft | Adam Pippert | 0001, 0002, 0003, 0007 |
| [0009](RFC-0009-agent-memory.md) | Agent Memory System | Draft | Adam Pippert | 0001, 0002, 0003, 0004, 0005 |
| [0010](RFC-0010-userland-utilities.md) | Userland Utility Layer — POSIX Port and Anunix Adaptation | Draft | Adam Pippert | 0001–0008 |
| [0011](RFC-0011-agent-native-utilities.md) | Agent-Native Utilities and Hardware Discovery | Draft | Adam Pippert | 0001–0010 |

## Dependency Graph

```
RFC-0001 (Architecture Thesis)
  └── RFC-0002 (State Object Model)
       └── RFC-0003 (Execution Cell Runtime)
            └── RFC-0004 (Memory Control Plane)
                 └── RFC-0005 (Routing Plane and Unified Scheduler)
                      └── RFC-0006 (Network Plane and Federated Execution)
                           └── RFC-0007 (Capability Objects and Runtime Installation)
                                └── RFC-0008 (Credential Objects)
                                     └── RFC-0009 (Agent Memory System)
                                          └── RFC-0010 (Userland Utility Layer)
                                               └── RFC-0011 (Agent-Native Utilities and Hardware Discovery)
```
