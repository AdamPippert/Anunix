# rm(1) — Delete a State Object

## SYNOPSIS

```
rm [-f] <namespace:path>
```

## DESCRIPTION

Transition a State Object to the `DELETED` lifecycle state and remove its namespace binding. The object's payload may be retained per its retention policy. Objects with active references (refcount > 0) cannot be deleted without `-f`.

## OPTIONS

- **-f** — Force delete. Transitions directly to `TOMBSTONE` state, bypassing retention policies.

## EXAMPLES

```
anx> write default:/temp "temporary data"
anx> rm default:/temp
deleted default:/temp

anx> rm -f default:/persistent
deleted default:/persistent
```

## LIFECYCLE

Normal deletion: `ACTIVE` → `DELETED` (payload retained per policy)
Force deletion: `ACTIVE` → `TOMBSTONE` (payload discarded, metadata retained for provenance)

## SEE ALSO

write(1), ls(1), inspect(1)
