# search(1) — Search State Object Payloads

## SYNOPSIS

```
search [OPTIONS] <pattern>
```

## DESCRIPTION

Search across all State Objects in the store for payload content matching the pattern. Returns matching OIDs with payload sizes. Skips credential objects (RFC-0008) and non-active/sealed objects.

## OPTIONS

- **-i** — Case-insensitive matching.
- **-t <type>** — Filter by object type (`byte`, `structured`).

## EXAMPLES

```
anx> write default:/a "hello world"
anx> write default:/b "HELLO ANUNIX"
anx> write default:/c "goodbye"

anx> search hello
searching for 'hello'...
  00000000-1f02-...  11 bytes
1 matches

anx> search -i hello
searching for 'hello'...
  00000000-1f02-...  11 bytes
  00000000-2a03-...  12 bytes
2 matches
```

## SEE ALSO

cat(1), ls(1), inspect(1)
