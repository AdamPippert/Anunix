# netinfo(1) — Network Configuration

## SYNOPSIS

```
netinfo
```

## DESCRIPTION

`netinfo` shows the active network interface and its IPv4 configuration.
It reads the interface that the Ethernet layer drives: `virtio-net0` or
`e1000` in a VM, and `wlan0 (mt7925)` on Wi-Fi hardware.

The `Source` line names where the address came from:

| Source                   | Meaning                                         |
|--------------------------|-------------------------------------------------|
| `DHCP lease`             | A DHCP server assigned the address.             |
| `static (config)`        | The network configuration set the address.      |
| `QEMU user-net fallback` | DHCP failed at boot in a VM. The kernel used `10.0.2.15`. |

## OUTPUT

A VM with a DHCP lease:

```
=== Network Configuration ===

  Interface:  virtio-net0
  MAC:        52:54:00:12:34:56
  IPv4:       10.0.2.15
  Netmask:    255.255.255.0
  Gateway:    10.0.2.2
  DNS:        10.0.2.3
  Source:     DHCP lease
  Status:     up
```

A Wi-Fi link without a lease:

```
=== Network Configuration ===

  Interface:  wlan0 (mt7925)
  MAC:        0a:0b:0c:0d:0e:0f
  IPv4:       none
  Status:     no address; run 'net dhcp'
```

## TROUBLESHOOTING

IF `netinfo` prints `No network interface is up` on Wi-Fi hardware,
THEN run `wifi connect <ssid> [<psk>]`.

IF the interface is up and `IPv4` is `none`,
THEN run `net dhcp`.

## SEE ALSO

sysinfo(1)
