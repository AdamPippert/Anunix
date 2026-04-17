# sysinfo(1) — System Information

## SYNOPSIS

```
sysinfo
```

## DESCRIPTION

Display a unified system overview combining CPU, memory, storage, network, and PCI device information. Consolidates data from ACPI tables, the page allocator, virtio drivers, and the PCI bus scanner.

## OUTPUT

```
=== Anunix System Information ===

CPU:       1 cores
LAPIC:     0xfee00000
IOAPICs:   1
Memory:    14336 KiB free / 16384 KiB total (12% used)
Disk:      256 MiB (virtio-blk)
Network:   52:54:0:12:34:56 (ip 10.0.2.15)
PCI:       7 devices
Scheduler: int=0 bg=0 lat=0 batch=0
```

## SEE ALSO

netinfo(1), pci(1), perf(1), hw-inventory(1)
