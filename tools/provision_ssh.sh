#!/bin/bash
# provision_ssh.sh — Bootstrap SSH key access to Framework Desktop via KVM HID.
#
# Run this when KVM HDMI + USB are connected but SSH key isn't provisioned.
# Uses glkvm.py type/key to type the public key into a terminal on the target.
#
# Prerequisite: Framework Desktop must be at a terminal prompt (not a lock screen).
# Usage: ./tools/provision_ssh.sh

set -euo pipefail
cd "$(dirname "$0")/.."

PUBKEY="ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIOJOuYrgXyolDi52yvbxX45wewKD1UzoM27kQcjhWmR hermes@adams-macbook-pro"
SUDO_PASS="${SUDO_PASS:-$(read -rsp "sudo password for Framework Desktop: " p; echo "$p")}"
KVM="python3 tools/kvm/glkvm.py"

log() { echo "[provision] $*"; }

# Open a terminal (try common hotkeys for Omarchy/i3/XFCE)
log "Opening terminal on target..."
$KVM key SuperLeft
sleep 0.5
$KVM type "kitty"
$KVM key Return
sleep 3

# Provision the key
log "Provisioning SSH key..."
$KVM type "mkdir -p ~/.ssh && chmod 700 ~/.ssh"
$KVM key Return
sleep 1

$KVM type "echo '$PUBKEY' >> ~/.ssh/authorized_keys"
$KVM key Return
sleep 1

$KVM type "chmod 600 ~/.ssh/authorized_keys"
$KVM key Return
sleep 1

$KVM type "echo 'SSH key installed'"
$KVM key Return
sleep 2

# Take screenshot to confirm
log "Taking screenshot to confirm..."
$KVM screenshot /tmp/provision_confirm.jpg
log "Screenshot saved to /tmp/provision_confirm.jpg"

log "Done. Test with: ssh adam@192.168.0.197 id"
