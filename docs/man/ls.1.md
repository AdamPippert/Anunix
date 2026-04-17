# ls(1) — List State Objects

## SYNOPSIS

```
ls [OPTIONS] [namespace:path]
```

## DESCRIPTION

List State Objects bound in a namespace. Without arguments, lists the root of the `default` namespace. Objects are displayed by their namespace binding name; use `-l` for detailed information including OID, type, lifecycle state, version, and payload size.

Unlike UNIX `ls` which lists file system entries, `ls` operates on Anunix namespaces — hierarchical mappings of names to State Object OIDs. Each entry is a first-class object with provenance, access control, and lifecycle management.

## OPTIONS

- **-l** — Long format. Shows OID, object type, lifecycle state, version, payload size, and name.
- **-a** — List all namespace names instead of entries.

## ADDRESSING

Paths are specified as `namespace:path`, where the namespace prefix is optional (defaults to `default`).

```
ls                       List root of 'default' namespace
ls default:/             Same as above
ls posix:/home           List entries under /home in posix namespace
ls system:/              List system namespace root
```

## OBJECT TYPES

| Type | Description |
|------|-------------|
| byte | Raw byte data |
| struct | Structured/JSON data |
| embed | Embedding vector |
| graph | Graph node |
| model | Model output |
| trace | Execution trace |
| cap | Capability object (RFC-0007) |
| cred | Credential object (RFC-0008) |

## EXAMPLES

```
anx> write default:/hello "Hello, world"
created 00000000-1f02-... (12 bytes) -> default:/hello

anx> ls
  hello

anx> ls -l
  00000000-1f02-...  byte  active  v1  12 bytes  hello
```

## SEE ALSO

cat(1), write(1), rm(1), inspect(1)
