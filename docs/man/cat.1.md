# cat(1) — Read State Object Payload

## SYNOPSIS

```
cat [OPTIONS] <oid-or-path>
```

## DESCRIPTION

Read and display the payload of a State Object. The object can be addressed by namespace path (`default:/hello`) or by OID prefix.

For credential objects (`ANX_OBJ_CREDENTIAL`), the payload is always `[REDACTED]` per RFC-0008 — use `anx_credential_read()` through a bound cell instead.

## OPTIONS

- **-x** — Hex dump with ASCII sidebar instead of text output.
- **-p** — Show provenance information (OID, version, size, parent count).

## EXAMPLES

```
anx> cat default:/hello
Hello, world

anx> cat -p default:/hello
oid:     00000000-1f02-7f3d-bb7c-236ca129e063
version: 1
size:    12 bytes
parents: 0
Hello, world

anx> cat -x default:/hello
  0: 48 65 6c 6c 6f 2c 20 77 6f 72 6c 64  Hello, world
```

## SEE ALSO

write(1), inspect(1), ls(1)
