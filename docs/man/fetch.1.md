# fetch(1) — HTTP Client with State Object Storage

## SYNOPSIS

```
fetch <host> <port> [path] [namespace:name]
```

## DESCRIPTION

Perform an HTTP GET request and store the response body as a State Object. Optionally bind the result to a namespace path. Provenance records the source URL.

Unlike UNIX `wget` which writes to a file, `fetch` creates a first-class State Object with OID, versioning, and provenance tracking.

## EXAMPLES

```
anx> fetch example.com 80 /
fetch: example.com:80/
fetch: HTTP 200, 540 bytes
stored: 00000000-3c07-...
<!doctype html>...

anx> fetch example.com 80 /index.html default:/webpage
fetch: example.com:80/index.html
fetch: HTTP 200, 540 bytes
stored: 00000000-3c08-... -> default:/webpage

anx> cat default:/webpage
<!doctype html>...
```

## SEE ALSO

http-get(1), api(1), cat(1)
