# write(1) — Create a State Object

## SYNOPSIS

```
write [OPTIONS] <namespace:path> <content>
```

## DESCRIPTION

Create a new State Object with the given content as payload, and bind it to the specified namespace path. The object is assigned a new UUID v7 OID and enters the `ACTIVE` lifecycle state. Provenance is automatically recorded.

## OPTIONS

- **-t <type>** — Set the object type. Values: `byte` (default), `structured`.

## EXAMPLES

```
anx> write default:/greeting "Hello from Anunix"
created 00000000-2a03-... (17 bytes) -> default:/greeting

anx> write -t structured default:/config '{"key":"value"}'
created 00000000-2a04-... (15 bytes) -> default:/config
```

## SEE ALSO

cat(1), ls(1), rm(1), cp(1)
