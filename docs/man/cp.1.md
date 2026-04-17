# cp(1) — Copy a State Object

## SYNOPSIS

```
cp <source> <destination>
```

## DESCRIPTION

Create a new State Object that is a copy of the source. The new object gets a new OID, but its `parent_oids` field links back to the source, recording derivation provenance. Content-addressed deduplication may occur automatically via `content_hash`.

The source is resolved by namespace path. The destination is bound as a new namespace entry.

## EXAMPLES

```
anx> write default:/original "important data"
created 00000000-1f02-... (14 bytes) -> default:/original

anx> cp default:/original default:/backup
copied -> 00000000-2b05-... (default:/backup)

anx> cat -p default:/backup
oid:     00000000-2b05-...
version: 1
size:    14 bytes
parents: 1
important data
```

## PROVENANCE

The copy records its source in `parent_oids`, creating a traceable lineage. Use `inspect -p` to view the full derivation chain.

## SEE ALSO

mv(1), rm(1), inspect(1)
