# RFC-0006: Network Plane and Federated Execution

| Field      | Value                                      |
|------------|--------------------------------------------|
| RFC        | 0006                                       |
| Title      | Network Plane and Federated Execution      |
| Author     | Adam Pippert                               |
| Status     | Draft                                      |
| Created    | 2026-04-13                                 |
| Updated    | 2026-04-13                                 |
| Depends On | RFC-0001, RFC-0002, RFC-0003, RFC-0004, RFC-0005 |

---

## Executive Summary

This RFC defines the **Network Plane**, the subsystem responsible for extending the operating environment across network boundaries so that remote compute, remote memory, and federated resources participate as normal parts of the system rather than ad-hoc external services.

RFC-0001 established that **network is not I/O — it is a distributed extension of compute and memory**. The preceding RFCs defined State Objects, Execution Cells, Memory Tiers, and Routing without assuming that all resources are local. This RFC makes those network-first assumptions concrete by specifying:

- the network architecture model and peer types
- trust zones and policy escalation across boundaries
- transport and serialization choices
- remote execution semantics
- federated memory access and consistency
- remote capability discovery
- replication contracts
- offline behavior and reconciliation
- partition tolerance and split-brain avoidance
- security model
- APIs and CLI
- reference implementation guidance

The design goal is to treat the network as a reliable extension of the local system while preserving local authority, graceful degradation, and explicit policy control at every boundary crossing.

---

## 1. Status

**Status:** Draft
**Author:** Adam Pippert / public collaborators
**Depends on:** RFC-0001, RFC-0002, RFC-0003, RFC-0004, RFC-0005
**Blocks:** RFC-0007, RFC-0008

---

## 2. Problem Statement

Classical operating systems treat the network as a transport layer. Applications open sockets, send bytes, and manage remote interactions entirely in userland. The kernel provides TCP/IP, DNS, and routing but has no awareness of what distributed computation or distributed memory mean at the semantic level.

This breaks down for AI-native workloads because:

- model inference may run on remote GPUs as a routine part of execution
- memory retrieval may span local indexes and remote replicas
- replication of State Objects must respect per-object policy
- offline operation must degrade gracefully, not silently fail
- trust boundaries differ between local, edge, and cloud resources
- federated teams may share memory selectively without merging everything

Without a first-class network plane, these concerns scatter across application code, resulting in:

- inconsistent trust boundaries
- uncontrolled data export
- fragile remote dependencies
- no standard for capability discovery
- ad-hoc replication with no policy enforcement
- poor offline behavior
- split-brain corruption risk

The system needs a dedicated layer that makes network participation explicit, policy-governed, and observable.

---

## 3. Goals

### 3.1 Primary Goals

1. **Network as extension**
   - Remote compute and remote memory must be accessible as normal system resources.

2. **Trust zone enforcement**
   - Every network boundary crossing must be policy-checked against explicit trust zones.

3. **Local authority by default**
   - The local node retains authority over its private state unless explicitly delegated.

4. **Graceful degradation**
   - Network unavailability must produce defined fallback behavior, not silent failure.

5. **Replication with policy**
   - State Object replication must respect per-object access, retention, and export policy.

6. **Remote capability discovery**
   - The routing plane must be able to discover and use remote engines through standard protocols.

7. **Offline reconciliation**
   - Partitioned nodes must reconcile safely when connectivity returns.

8. **Observable network behavior**
   - Every remote call, replication event, and trust zone crossing must be traceable.

### 3.2 Non-Goals

1. Building a general-purpose distributed database.
2. Requiring all nodes to run the same software version.
3. Guaranteeing strong consistency across all federated memory.
4. Replacing standard networking stacks.
5. Making every object globally available by default.

---

## 4. Core Definitions

### 4.1 Network Plane

The **Network Plane** is the subsystem that manages remote compute access, remote memory access, replication, capability discovery, and trust zone enforcement.

### 4.2 Node

A **Node** is a system instance participating in the network plane. It may be a personal workstation, edge device, team server, or cloud instance.

### 4.3 Peer

A **Peer** is a remote node with which the local node has established a trust relationship and communication channel.

### 4.4 Trust Zone

A **Trust Zone** is a classification of network boundaries that determines default policy for data exchange, execution delegation, and memory sharing.

### 4.5 Replication Contract

A **Replication Contract** is a policy-bound agreement specifying what State Objects may be copied between nodes, under what conditions, and with what encryption and retention requirements.

### 4.6 Capability Advertisement

A **Capability Advertisement** is a message from a peer declaring which engines, memory surfaces, and services it makes available.

### 4.7 Partition

A **Partition** is a period during which one or more peers are unreachable due to network failure or degradation.

### 4.8 Reconciliation

**Reconciliation** is the process of merging or resolving divergent state after a partition ends.

---

## 5. Design Principles

### 5.1 Local Authority Is Default
The local node owns its state. Remote access requires explicit grants.

### 5.2 Policy Travels With Data
When State Objects cross network boundaries, their policy must travel with them.

### 5.3 Trust Is Zoned, Not Binary
Different peers get different levels of trust, and trust determines what is permitted.

### 5.4 Degradation Must Be Explicit
When the network is impaired, the system must announce reduced capability, not silently degrade quality.

### 5.5 Replication Is Policy-Governed
No object should leave the local node without satisfying its export and replication policy.

### 5.6 Discovery Must Be Authenticated
Capability advertisements from peers must be verified before influencing routing decisions.

### 5.7 Reconciliation Must Be Safe
After a partition, merging must not silently overwrite local authority or promote stale data.

---

## 6. Network Architecture Model

### 6.1 Peer Types

#### Personal Node
A single-user workstation or device. Primary owner of private state.

#### Edge Node
A low-latency compute resource on the local network or nearby infrastructure.

#### Team Server
A shared resource for a team or organization with its own memory and engines.

#### Cloud Node
A remote compute or storage resource with higher latency but greater capacity.

#### Inference Endpoint
A specialized remote service offering model inference without full node semantics.

### 6.2 Topology

The network plane does not assume a fixed topology. It supports:

- peer-to-peer connections between any two nodes
- hub-and-spoke with a team server as coordinator
- mesh among a small set of trusted peers
- client-to-cloud for inference and backup

### 6.3 Node Identity

Each node must have a stable identity:

```text
node_<ulid_or_uuidv7>
```

Node identity must be backed by a cryptographic key pair for authentication.

### 6.4 Node Registry

Each node maintains a local registry of known peers containing:

- node ID
- trust zone
- last seen timestamp
- advertised capabilities
- connection parameters
- replication contracts

---

## 7. Trust Zones

### 7.1 Canonical Trust Zones

The initial trust zone model defines four zones:

- **local**: the node itself
- **trusted-edge**: low-latency peers on the same network with explicit trust
- **trusted-remote**: authenticated remote peers with established trust
- **untrusted-remote**: peers with no established trust relationship

### 7.2 Zone Properties

| Zone | Default Export | Default Execute | Default Replicate | Encryption |
|------|--------------|----------------|-------------------|------------|
| local | allowed | allowed | n/a | optional |
| trusted-edge | policy-checked | policy-checked | policy-checked | required |
| trusted-remote | policy-checked | policy-checked | policy-checked | required |
| untrusted-remote | denied | denied | denied | required |

### 7.3 Policy Escalation

As trust zone risk increases, policy requirements escalate:

1. **local**: object policy applies directly
2. **trusted-edge**: object policy + edge export rules
3. **trusted-remote**: object policy + remote export rules + encryption verification
4. **untrusted-remote**: denied by default; requires explicit override

### 7.4 Trust Establishment

Trust between nodes should be established through:

- mutual TLS with verified certificates
- shared secret or key exchange
- administrator approval
- trust delegation from an already-trusted peer (with limits)

### 7.5 Trust Zone Rules

1. Trust zone assignment must be explicit, not inferred from network topology alone.
2. A peer may be demoted to a lower trust zone if trust conditions change.
3. Trust zone changes must be logged and traceable.

---

## 8. Transport Layer

### 8.1 Protocol Requirements

The transport layer must support:

- authenticated connections
- encrypted data in transit
- request/response patterns
- streaming patterns
- publish/subscribe for capability updates
- connection health monitoring

### 8.2 Protocol Choices

#### Phase 1 (Prototype)
- **gRPC** for structured request/response and streaming between nodes
- **ZeroMQ** for lightweight internal service communication (from RFC-0005)
- **TLS 1.3** for all inter-node connections

#### Phase 2 (Performance)
- **QUIC** for improved latency and multiplexing over unreliable links
- Connection migration for mobile or edge nodes

### 8.3 Serialization

- **Protocol Buffers** for inter-node messages (schema-enforced, efficient)
- **MessagePack** for internal service messages (self-describing, compact)
- **JSON** for debug and human-readable inspection only

### 8.4 Message Envelope

All inter-node messages must include:

```json
{
  "message_id": "msg_01JR...",
  "source_node": "node_01ABC...",
  "target_node": "node_01DEF...",
  "trust_zone": "trusted-remote",
  "timestamp": "2026-04-13T18:00:00Z",
  "message_type": "capability_advertisement",
  "correlation_id": "corr_01JR...",
  "signature": "...",
  "payload": {}
}
```

### 8.5 Transport Rules

1. All inter-node communication must be encrypted.
2. Message signatures must be verified before processing.
3. Connection health must be monitored with configurable heartbeat intervals.
4. Transport failures must produce explicit error events, not silent drops.

---

## 9. Remote Execution Model

### 9.1 Purpose

Remote execution allows an Execution Cell to delegate work to engines running on peer nodes.

### 9.2 Execution Flow

```text
local cell -> routing plane selects remote engine
  -> policy check (export, trust zone, execution policy)
  -> serialize inputs (respecting policy)
  -> transport to remote node
  -> remote node admits and executes
  -> remote node returns outputs
  -> local cell receives and validates
  -> commit locally
```

### 9.3 Trust Envelope

When inputs are sent to a remote node, they must be wrapped in a trust envelope:

```json
{
  "cell_ref": "cell_01JR...",
  "source_node": "node_01ABC...",
  "trust_zone": "trusted-remote",
  "input_refs": [
    {
      "object_id": "so_01XYZ...",
      "representation": "raw_text",
      "policy_summary": {
        "allow_remote_models": true,
        "allow_export": false,
        "retention_class": "ephemeral"
      }
    }
  ],
  "intent": {
    "name": "summarize_document",
    "objective": "Generate summary"
  },
  "constraints": {
    "max_latency_ms": 5000
  },
  "return_policy": {
    "persist_on_remote": false,
    "encrypt_in_transit": true
  }
}
```

### 9.4 Remote Execution Rules

1. The remote node must honor the trust envelope policy.
2. Inputs marked `allow_export: false` must not be persisted on the remote node after execution.
3. Remote outputs must include provenance identifying the remote engine and node.
4. The local node retains authority over commit decisions.
5. Remote execution failures must propagate error details back to the local cell.

### 9.5 Remote Output Handling

Remote outputs are received as serialized State Objects. The local node must:

1. verify signatures
2. attach remote provenance
3. apply local validation policy
4. commit locally if validation passes

---

## 10. Federated Memory Access

### 10.1 Purpose

Federated memory allows a node to query memory surfaces on peer nodes as part of normal retrieval operations.

### 10.2 Access Modes

#### Query-Only
The local node sends a retrieval query; the remote node returns matching results without replicating full objects.

#### Query-and-Fetch
The local node queries and selectively fetches full objects that pass policy checks.

#### Shared Index
Both nodes maintain synchronized retrieval indexes over a shared subset of memory.

### 10.3 Consistency Model

Federated memory does **not** provide strong consistency by default. Instead:

- **local reads are authoritative** for local state
- **remote reads are advisory** unless the local node has no local copy
- **writes are local-first** with optional asynchronous replication
- **conflicts are resolved by policy** (last-writer-wins, manual merge, or authority-based)

### 10.4 Federated Query Schema

```json
{
  "query": "quarterly planning documents",
  "modes": ["semantic", "lexical"],
  "taxonomy_scope": ["work/projects"],
  "target_nodes": ["node_01DEF..."],
  "minimum_validation_state": "provisional",
  "max_results": 10,
  "include_remote_metadata": true,
  "fetch_full_objects": false
}
```

### 10.5 Federated Memory Rules

1. Remote queries must respect the queried node's access policy.
2. Results from untrusted zones must be marked with lower trust.
3. Fetched objects must carry their full policy and provenance.
4. Local authority is preserved — remote results do not override local state.
5. Federated queries should have configurable timeouts with graceful fallback to local-only.

---

## 11. Remote Capability Discovery

### 11.1 Purpose

The routing plane needs to know what engines and services are available on peer nodes.

### 11.2 Capability Advertisement

Peers periodically advertise their available capabilities:

```json
{
  "node_id": "node_01DEF...",
  "timestamp": "2026-04-13T18:00:00Z",
  "engines": [
    {
      "engine_id": "eng_remote_large_reasoning",
      "engine_class": "remote_model",
      "capabilities": ["long_context_reasoning", "summarization"],
      "status": "available",
      "cost_model": {
        "kind": "per_token",
        "input_cost_per_1k": 0.003,
        "output_cost_per_1k": 0.015
      },
      "constraints": {
        "max_context_tokens": 200000,
        "supports_streaming": true
      },
      "policy_tags": ["private_safe"]
    }
  ],
  "memory_surfaces": [
    {
      "surface_id": "mem_remote_semantic",
      "type": "semantic_retrieval",
      "taxonomy_scopes": ["work/shared"],
      "status": "available"
    }
  ]
}
```

### 11.3 Discovery Protocol

1. On connection establishment, peers exchange capability advertisements.
2. Advertisements are refreshed on configurable intervals.
3. Status changes (engine going offline) trigger immediate update.
4. The local routing plane merges remote capabilities into its registry with zone-appropriate trust markers.

### 11.4 Discovery Rules

1. Capability advertisements must be signed by the advertising node.
2. Remote capabilities must be flagged with their trust zone in the local registry.
3. The routing plane must not route to remote engines whose trust zone forbids the task's policy requirements.
4. Stale advertisements (no refresh within timeout) must mark the remote engine as `unknown` status.

---

## 12. Replication Contracts

### 12.1 Purpose

Replication contracts define what State Objects may be copied between nodes and under what conditions.

### 12.2 Contract Schema

```json
{
  "contract_id": "repl_01JR...",
  "source_node": "node_01ABC...",
  "target_node": "node_01DEF...",
  "scope": {
    "taxonomy_paths": ["work/shared/projects"],
    "object_types": ["document.markdown", "memory.summary"],
    "minimum_validation_state": "provisional"
  },
  "direction": "bidirectional",
  "encryption": {
    "in_transit": "tls_1.3",
    "at_rest_on_remote": "aes_256_gcm"
  },
  "retention_on_remote": {
    "class": "mirror_source",
    "max_days": 90
  },
  "frequency": "continuous",
  "conflict_resolution": "source_authority"
}
```

### 12.3 Replication Directions

- `push`: local node sends to remote
- `pull`: local node fetches from remote
- `bidirectional`: both directions with conflict resolution

### 12.4 Replication Scope Filters

Replication scope may filter by:

- taxonomy path
- object type
- validation state
- labels
- creation time range
- explicit include/exclude lists

### 12.5 Replication Rules

1. No object may be replicated if its policy forbids export to the target trust zone.
2. Replicated objects must carry their full policy envelope.
3. Encryption at rest on the remote must meet the object's minimum encryption requirement.
4. Replication contracts must be versioned and auditable.
5. Contract changes require mutual acknowledgment.

---

## 13. Offline Behavior and Reconciliation

### 13.1 Offline Modes

When network connectivity is lost, the system may operate in:

- **full offline**: no remote access; local-only execution and memory
- **partial offline**: some peers reachable, others not
- **degraded**: connections available but with high latency or packet loss

### 13.2 Offline Behavior Rules

1. The local node must continue to function using local engines and local memory.
2. Remote capabilities must be removed from routing eligibility during partition.
3. Outputs produced during offline must be marked with provenance indicating offline context.
4. Queued replication must resume when connectivity returns.

### 13.3 Reconciliation Strategy

When a partitioned peer becomes reachable again:

1. **Exchange version vectors** — each node reports what it has changed since last sync
2. **Identify conflicts** — objects modified on both sides during partition
3. **Apply conflict resolution policy** from the replication contract:
   - `source_authority`: source node wins
   - `target_authority`: target node wins
   - `last_writer_wins`: most recent timestamp wins
   - `manual`: flag for human or agent review
   - `merge`: attempt automated merge where schema permits
4. **Replicate resolved state** — sync objects according to contract scope
5. **Log reconciliation events** — every conflict and resolution must be traceable

### 13.4 Version Tracking

Each node should maintain a logical version vector or similar mechanism per replication contract scope. This may use:

- Lamport timestamps for simple ordering
- vector clocks for multi-node causal tracking
- hybrid logical clocks for wall-clock approximation with causal ordering

### 13.5 Reconciliation Rules

1. Reconciliation must not silently discard local changes.
2. Conflict resolution must be traceable in the provenance chain.
3. Objects promoted to L4 (long-term memory) during partition must not be automatically overwritten by remote state without review.
4. Reconciliation should prefer safety over speed.

---

## 14. Partition Tolerance

### 14.1 Principle

The system must prioritize availability and partition tolerance over strong consistency, following a CP-leaning model for authoritative state and an AP-leaning model for replicated read surfaces.

### 14.2 Local Authority Preservation

During a partition:

- the local node remains authoritative over its own State Objects
- remote copies are considered stale until reconciliation
- local memory surfaces continue to operate
- routing falls back to local-only engines

### 14.3 Split-Brain Avoidance

Split-brain scenarios are mitigated through:

- **ownership semantics**: every object has a declared authority node
- **version vectors**: conflicting writes are detected, not silently merged
- **reconciliation policy**: contracts define how conflicts are resolved
- **no distributed locks**: the system does not depend on global consensus for normal operation

### 14.4 Partition Detection

The network plane should detect partitions through:

- heartbeat timeout
- failed RPC attempts
- transport-layer connection loss
- explicit peer status messages

### 14.5 Partition Rules

1. Partition must not block local execution.
2. Partition must trigger routing plane reconfiguration within bounded time.
3. Long partitions should trigger staleness marking on remote-sourced memory.
4. Reconnection should be handled automatically with explicit reconciliation.

---

## 15. Security Model

### 15.1 Authentication

- All inter-node connections must use mutual TLS (mTLS).
- Node identity is established through X.509 certificates or equivalent.
- Certificate rotation must be supported without service interruption.

### 15.2 Authorization

- Every remote operation must be checked against the requesting node's trust zone.
- Per-object policy must be evaluated before any data crosses a node boundary.
- Remote execution requests must be admitted through the same policy pipeline as local cells.

### 15.3 Data in Transit

- All inter-node data must be encrypted with TLS 1.3 or equivalent.
- Message integrity must be verified through signatures or authenticated encryption.

### 15.4 Data at Rest on Remote

- Replicated objects must be encrypted at rest on the remote node according to the replication contract.
- Ephemeral execution inputs must not be persisted on the remote node after execution completes.

### 15.5 Credential Scoping

- Remote execution must use scoped credentials that limit what the remote node can access.
- Credentials must not grant broader access than the originating cell's execution policy permits.
- Credential lifetime should be bounded and non-renewable without re-authentication.

### 15.6 Security Rules

1. No plaintext data may traverse a node boundary.
2. Authentication failure must terminate the connection.
3. Authorization failure must be logged and returned as an explicit error.
4. Credential escalation across trust zones is forbidden without explicit policy override.

---

## 16. APIs

### 16.1 Peer Management

```http
POST /network/peers
GET /network/peers
GET /network/peers/{id}
DELETE /network/peers/{id}
```

### 16.2 Trust Zone Assignment

```http
PUT /network/peers/{id}/trust-zone
```

Request:

```json
{
  "trust_zone": "trusted-remote"
}
```

### 16.3 Replication Contracts

```http
POST /network/replication-contracts
GET /network/replication-contracts
GET /network/replication-contracts/{id}
PUT /network/replication-contracts/{id}
DELETE /network/replication-contracts/{id}
```

### 16.4 Remote Execution

```http
POST /network/execute
```

Request:

```json
{
  "target_node": "node_01DEF...",
  "cell_ref": "cell_01JR...",
  "trust_envelope": {}
}
```

### 16.5 Federated Query

```http
POST /network/query
```

### 16.6 Capability Discovery

```http
GET /network/capabilities
GET /network/capabilities/{node_id}
```

### 16.7 Reconciliation

```http
POST /network/reconcile/{peer_id}
GET /network/reconcile/{peer_id}/status
```

### 16.8 Network Status

```http
GET /network/status
```

---

## 17. CLI Surface

Suggested initial CLI:

```bash
network peers
network peers add --address 192.168.1.50:5550 --trust trusted-edge
network peers remove node_01DEF...
network trust node_01DEF... --zone trusted-remote
network capabilities
network capabilities node_01DEF...
network replicate create --target node_01DEF... --scope work/shared --direction push
network replicate list
network replicate status repl_01JR...
network execute --target node_01DEF... --cell cell_01JR...
network query --target node_01DEF... --query "project status" --scope work/projects
network reconcile node_01DEF...
network status
```

---

## 18. POSIX Compatibility

### 18.1 Compatibility Thesis

The Network Plane generalizes remote access without removing socket and mount-based networking.

### 18.2 Mapping

| Classical Concept | Network Plane Equivalent |
|---|---|
| NFS / SMB mount | Federated memory query + object fetch |
| SSH remote execution | Remote cell execution with trust envelope |
| Socket connection | Peer connection with trust zone |
| rsync | Replication contract |

### 18.3 Compatibility Mode

In the initial prototype, standard networking remains available for legacy tools. The network plane adds policy-aware, provenance-tracked alternatives for AI-native operations.

---

## 19. Reference Prototype Architecture

### 19.1 Initial Components

1. **Peer Manager**
   - connection lifecycle, trust zone assignment, heartbeat

2. **Transport Service**
   - gRPC server/client with mTLS

3. **Capability Exchange**
   - advertisement publication and remote capability ingestion

4. **Replication Engine**
   - contract management, scope filtering, conflict detection

5. **Reconciliation Service**
   - version vector exchange, conflict resolution, merge execution

6. **Remote Execution Proxy**
   - trust envelope construction, remote dispatch, output reception

7. **Federated Query Proxy**
   - remote memory query dispatch and result merging

### 19.2 Recommended Initial Stack

- Python for orchestration and service logic
- gRPC with Protocol Buffers for inter-node communication
- mTLS with self-signed CA for prototype trust
- PostgreSQL for peer registry, replication contracts, and reconciliation logs
- ZeroMQ for local service coordination (consistent with RFC-0005)

---

## 20. Observability

### 20.1 Required Metrics

- peer connection count and status
- messages sent/received per peer
- remote execution count and latency
- replication throughput and lag
- reconciliation frequency and conflict count
- partition events and duration
- trust zone distribution
- failed authentication/authorization attempts

### 20.2 Debug Questions the System Must Answer

- Why did this task execute remotely instead of locally?
- Why was this object not replicated?
- Why did reconciliation choose remote state over local?
- How long was this peer partitioned?
- What capabilities are available from which peers?
- Why was this remote query denied?

---

## 21. Failure Modes

### 21.1 Trust Zone Misconfiguration
A peer is assigned an overly permissive trust zone.

**Mitigation:** default to most restrictive zone; require explicit elevation.

### 21.2 Replication Policy Violation
Objects are replicated to nodes that should not hold them.

**Mitigation:** per-object policy check before every replication event.

### 21.3 Reconciliation Data Loss
Remote state overwrites local authoritative changes.

**Mitigation:** local authority preservation, conflict flagging, manual review for contested merges.

### 21.4 Stale Capability Advertisements
Routing plane uses remote engines that are no longer available.

**Mitigation:** advertisement TTL, heartbeat-based status, graceful fallback.

### 21.5 Network Dependency Creep
System becomes unusable without remote resources.

**Mitigation:** local-first design, offline capability testing, degradation monitoring.

### 21.6 Credential Leakage
Remote execution credentials are broader than intended.

**Mitigation:** scoped credentials, short lifetimes, audit logging.

---

## 22. Implementation Plan

### Phase 1: Peer Discovery and Local Communication
- peer registry
- trust zone model
- gRPC transport with mTLS
- capability advertisement exchange
- basic network status

### Phase 2: Remote Execution
- trust envelope construction
- remote cell dispatch
- output reception and local validation
- remote provenance tracking

### Phase 3: Replication
- replication contracts
- push/pull replication with policy enforcement
- encryption at rest on remote

### Phase 4: Federated Memory
- remote memory queries
- result merging
- taxonomy-scoped federation

### Phase 5: Offline and Reconciliation
- version vectors
- conflict detection and resolution
- partition-aware routing
- reconciliation logging

---

## 23. Open Questions

1. Should replication contracts be symmetric by default or require separate inbound/outbound definitions?
2. What is the right heartbeat interval for peer health detection without excessive overhead?
3. How should the system handle trust zone changes for objects already replicated under the old zone?
4. Should federated queries support join-like operations across nodes, or only independent queries?
5. What is the best conflict resolution default: source-authority, last-writer-wins, or manual?
6. How should the system handle version skew between nodes running different schema versions?
7. Should the network plane support anonymous or pseudonymous peers for privacy-sensitive use cases?

---

## 24. Decision Summary

This RFC makes the following decisions:

1. The Network Plane is a first-class subsystem, not application-level glue.
2. Trust is organized into four zones: local, trusted-edge, trusted-remote, untrusted-remote.
3. Local authority is preserved by default; remote state is advisory until reconciled.
4. All inter-node communication is encrypted and authenticated via mTLS.
5. Replication is governed by explicit contracts with per-object policy enforcement.
6. Capability discovery uses signed advertisements with configurable TTL.
7. Offline operation is supported with defined degradation and reconciliation behavior.
8. Conflict resolution is policy-driven and traceable, not hidden.
9. Initial implementation uses gRPC with Protocol Buffers for inter-node transport.

---

## 25. Conclusion

The Network Plane makes the system's network-first thesis concrete.

It gives the operating environment a principled way to:

- extend compute across node boundaries
- share memory selectively and safely
- discover remote capabilities
- replicate state with policy enforcement
- operate during network partitions
- reconcile divergent state after reconnection
- maintain local authority and trust boundaries

Without this layer, network participation remains fragile, ad-hoc, and policy-blind. With it, the network becomes a governed extension of the local operating environment — which is what RFC-0001 promised from the beginning.
