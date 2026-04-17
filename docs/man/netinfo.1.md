# netinfo(1) — Network Configuration

## SYNOPSIS

```
netinfo
```

## DESCRIPTION

Display network interface configuration including MAC address, IPv4 address, and DNS server. Information is sourced from the virtio-net driver and IP stack configuration (DHCP or static).

## OUTPUT

```
=== Network Configuration ===

  Interface:  virtio-net0
  MAC:        52:54:0:12:34:56
  IPv4:       10.0.2.15
  DNS:        10.0.2.3
  Status:     up
```

## SEE ALSO

sysinfo(1), ping(1), dns(1)
