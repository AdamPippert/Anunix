# inspect(1) — State Object Inspector

## SYNOPSIS

```
inspect [OPTIONS] <oid-or-path>
```

## DESCRIPTION

Display complete State Object internals: OID, content hash, type, lifecycle state, version, payload size, refcount, parent count, and a hex dump of the payload with ASCII sidebar.

This is the diagnostic tool for understanding exactly what a State Object contains and how it relates to other objects in the system.

## OPTIONS

- **-x** — Hex dump only (skip metadata).
- **-m** — Metadata only (skip hex dump).

## OUTPUT FIELDS

| Field | Description |
|-------|-------------|
| OID | Globally unique UUID v7 identifier |
| Type | Object type (byte_data, structured_data, etc.) |
| State | Lifecycle state (creating, active, sealed, etc.) |
| Version | Monotonically increasing version number |
| Payload | Payload size in bytes |
| Refcount | Number of active references |
| Parents | Number of derivation parents (provenance) |

## EXAMPLES

```
anx> inspect default:/hello

=== State Object Inspection ===

  OID:       00000000-1f02-7f3d-bb7c-236ca129e063
  Type:      byte_data
  State:     active
  Version:   1
  Payload:   12 bytes
  Refcount:  0
  Parents:   0

  Hex dump (12 of 12 bytes):
  0: 48 65 6c 6c 6f 2c 20 77 6f 72 6c 64  Hello, world
```

## SEE ALSO

cat(1), ls(1), cp(1)
