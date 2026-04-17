# mv(1) — Move/Rename a Namespace Binding

## SYNOPSIS

```
mv <source> <destination>
```

## DESCRIPTION

Rebind a State Object from one namespace path to another. The object itself does not move — its OID remains stable. Only the namespace mapping changes. This is fundamentally different from UNIX `mv` which moves bytes on disk.

## EXAMPLES

```
anx> write default:/old-name "some data"
anx> mv default:/old-name default:/new-name
moved default:/old-name -> default:/new-name

anx> ls
  new-name

anx> cat default:/new-name
some data
```

## SEE ALSO

cp(1), rm(1), ls(1)
