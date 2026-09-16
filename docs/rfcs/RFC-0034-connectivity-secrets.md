# RFC-0034: Connectivity Secrets — Key-Authenticated Credentials

| Field      | Value                                                            |
|------------|------------------------------------------------------------------|
| RFC        | 0034                                                             |
| Title      | Connectivity Secrets — Key-Authenticated Credentials             |
| Author     | Adam Pippert                                                     |
| Status     | Draft                                                            |
| Created    | 2026-09-15                                                       |
| Depends On | RFC-0008 (Credential Objects)                                    |
| Blocks     | Eigentunnel post-quantum transport                               |

This RFC uses the RFC 2119 requirement keywords `MUST`, `MUST NOT`, `SHOULD`,
and `MAY`, as RFC 8174 amends them.

---

## Executive Summary

`anx_credential_read()` takes a name and returns a plaintext secret. It does
not take a principal. Nothing in the call says who is asking, so nothing can
refuse.

Every path that reaches the credential store therefore reaches every secret in
it. The shell's `secret` command reads the same store as the kernel's network
bring-up. A password-authenticated SSH session reads it too, and so does the
HTTP API on port 8080.

One group of secrets deserves better. Wi-Fi credentials, SSH host and
authorized keys, and Eigentunnel post-quantum key material are what an attacker
wants most, because they do not merely unlock this machine — they grant reach
beyond it. A stolen Wi-Fi password is a foothold on the network. A stolen SSH
host key is an impersonation of the machine. Stolen tunnel key material is
every peer the tunnel reaches.

This RFC makes that group a class, and gives the class one rule: **a
connectivity secret can be read back or removed only by a principal that
authenticated with a public key.**

## Motivation

Three properties are missing today.

**No principal.** `anx_credential_read()` has no caller identity, so
enforcement has nowhere to live. An audit of the runtime against the Pluralea
adapter contracts recorded the same gap independently.

**No authentication method on a session.** `struct anx_session` records a
username and a scope set. It does not record how the session proved itself.
A password session and a key session are indistinguishable after the fact, so
a rule that names public keys cannot be written.

**SSH does not create a session at all.** `sshd.c` performs its own password
and public-key checks and never calls into `anx/auth.h`. The authentication
result is discarded as soon as the connection is admitted.

## Design

### 1. A credential class

Every credential carries a class. This RFC defines two.

| Class | Meaning |
|---|---|
| `ANX_CRED_CLASS_ORDINARY` | the existing behaviour, unchanged |
| `ANX_CRED_CLASS_CONNECTIVITY` | reach beyond this machine; key-authenticated access only |

Classification MUST be by name, not by a flag the creator chooses. A caller
that could pick its own class could store a connectivity secret as ordinary
and read it back freely, which defeats the rule.

The reserved names are:

| Name | Holds |
|---|---|
| `wifi-ssid` | the network the machine joins |
| `wifi-pass` | its pre-shared key |
| `ssh-host-key` | the identity this machine presents |
| `ssh-authorized-keys` | the keys permitted to log in |
| `ssh-password` | the fallback password for SSH |
| `eigentunnel-*` | reserved for post-quantum tunnel key material |

`eigentunnel-*` is a prefix reservation. No Eigentunnel code exists in the
kernel at the time of writing, and this RFC implements none. The prefix is
reserved now so that key material lands in the protected class on the day it
arrives, rather than being retrofitted after it has been stored in the clear.

### 2. Two accessors, failing closed

`anx_credential_read()` becomes principal-checked. Given a connectivity
credential, it MUST refuse unless the current session authenticated by public
key, returning `ANX_EPERM`.

`anx_credential_read_system()` is the kernel's own accessor. It performs no
principal check and MUST NOT be reachable from the shell, the HTTP API, or an
SSH session. It exists for bring-up paths that run before any principal can
exist — the boot-time Wi-Fi association, the SSH host key the server needs in
order to offer authentication at all, and the password check that authenticates
a session that does not yet exist.

The default is the checked one deliberately. A future caller that forgets this
RFC gets a refusal, not a leak.

### 3. Sessions record how they authenticated

`struct anx_session` gains an authentication method. `sshd` MUST establish a
session on successful authentication and record whether the proof was a public
key or a password.

A console session at the physical keyboard is not key-authenticated and
therefore MUST NOT read connectivity secrets back.

### 4. Writing is not reading

A connectivity secret MAY be created and rotated from the console.

This asymmetry is deliberate and is the crux of the design. Requiring a key
session to *write* Wi-Fi credentials is unsatisfiable: the machine needs Wi-Fi
to obtain a network, and a network to accept an SSH connection, and an SSH
connection to present a key. Bootstrap would be impossible.

Allowing the write costs nothing that the read protects. Someone at the
keyboard can already overwrite a secret, and overwriting destroys the value
rather than disclosing it. What the rule prevents is exfiltration: walking up
to a running machine and reading out the network's password, the host identity,
or tunnel key material.

So: **write at the console, read only with a key.**

### 5. Removal is gated and explicit

A connectivity secret MUST NOT be removed by the ordinary revoke path.

Removal happens through a single operation that MUST refuse unless the session
authenticated by public key. It reports what it removed. Because losing these
secrets costs the machine its network, the operation MUST name the class it is
clearing rather than deleting a credential the caller merely guessed the name
of.

## What this does not do

It does not encrypt the store. Credentials persist in plaintext, and anyone who
can read the object store's bytes — by removing the drive, for instance — reads
the secrets. This RFC raises the cost of a live machine and a shell; it is not
at-rest protection. At-rest encryption is separate work and MUST NOT be assumed
from this RFC.

It does not implement Eigentunnel, post-quantum key exchange, or any cipher. It
reserves a name prefix.

It does not make `anx_credential_read_system()` unreachable by a kernel bug. A
memory-safety failure in any driver still reaches the store. The boundary is
against paths, not against arbitrary code execution.

It does not add per-connection sessions. The session model remains one current
session; an SSH connection sets it. Concurrent sessions with different
authentication methods are out of scope and are recorded here as a known limit.

## Acceptance

1. Reading `wifi-pass` from the console shell returns a refusal, not a value.
2. Reading `wifi-pass` over a password-authenticated SSH session returns a
   refusal.
3. Reading `wifi-pass` over a public-key SSH session returns the value.
4. Boot-time Wi-Fi association still works with no session present.
5. The ordinary revoke path refuses a connectivity credential.
6. The removal operation refuses without a key session and succeeds with one.
7. An ordinary credential is unaffected in every one of the above.
