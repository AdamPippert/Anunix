#!/bin/bash
# deploy_framework.sh — Deploy Anunix kernel to Framework Desktop and set up boot partition.
#
# Usage:
#   ./tools/deploy_framework.sh [TARGET_IP]
#
# Requires:
#   - SSH access to adam@<TARGET_IP> (password or key)
#   - sudo rights on target (set SUDO_PASS env var or script will prompt)
#   - build/x86_64/anunix.elf already built
#
# What this does:
#   1. Shows current disk layout on the target
#   2. Creates a 50 GiB GPT partition for Anunix (if not already present)
#   3. Formats it ext2 (simple, GRUB-readable)
#   4. Mounts at /boot/anunix
#   5. Copies anunix.elf to /boot/anunix/anunix.elf
#   6. Adds a custom GRUB menu entry
#   7. Updates GRUB

set -euo pipefail

TARGET="${1:-192.168.0.197}"
USER="adam"
SUDO_PASS="${SUDO_PASS:-$(read -rsp "sudo password for $USER@$TARGET: " p; echo "$p")}"
ELF="build/x86_64/anunix.elf"
REMOTE_TMP="/tmp/anunix_deploy"
HTTP_PORT=19871

# Color output
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[deploy]${NC} $*"; }
warn() { echo -e "${YELLOW}[warn]${NC} $*"; }
die()  { echo -e "${RED}[error]${NC} $*" >&2; exit 1; }

# Check prerequisites
[[ -f "$ELF" ]] || die "Kernel not built: $ELF missing. Run: make kernel ARCH=x86_64"
command -v ssh >/dev/null || die "ssh not found"

SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o BatchMode=yes"
SSH="ssh $SSH_OPTS $USER@$TARGET"
SCP="scp $SSH_OPTS"

# Test connectivity
log "Testing SSH to $USER@$TARGET..."
$SSH "echo connected" 2>/dev/null || {
    # Try password auth
    SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10"
    SSH="ssh $SSH_OPTS $USER@$TARGET"
    SCP="scp $SSH_OPTS"
    log "Key auth failed — falling back to password. You may be prompted."
}

SUDO="echo '$SUDO_PASS' | sudo -S"

# 1. Show disk layout
log "Current disk layout on $TARGET:"
$SSH "$SUDO lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT 2>/dev/null || lsblk"

# 2. Find the target disk (usually nvme0n1 on Framework Desktop)
DISK=$($SSH "lsblk -dpno NAME | grep -E 'nvme|sda' | head -1")
log "Target disk: $DISK"

# 3. Check if Anunix partition already exists
ANUNIX_PART=$($SSH "$SUDO blkid | grep -i 'LABEL=\"anunix\"' | cut -d: -f1" 2>/dev/null || true)

if [[ -n "$ANUNIX_PART" ]]; then
    warn "Anunix partition already exists: $ANUNIX_PART"
else
    log "Creating 50 GiB Anunix partition on $DISK..."
    # Use sgdisk to add a new partition at the end of the disk
    $SSH "$SUDO sgdisk --new=0:0:+50G --change-name=0:anunix --typecode=0:8300 $DISK" || {
        # Fallback: try parted
        $SSH "$SUDO parted -s $DISK mkpart anunix ext2 -- -50GiB 100%"
    }
    # Re-read partition table
    $SSH "$SUDO partprobe $DISK 2>/dev/null || true"
    sleep 2
    ANUNIX_PART=$($SSH "$SUDO fdisk -l $DISK | grep -i anunix | awk '{print \$1}'" 2>/dev/null || true)
    [[ -n "$ANUNIX_PART" ]] || ANUNIX_PART="${DISK}p$(($($SSH "$SUDO fdisk -l $DISK | grep -c '^/dev'" 2>/dev/null || echo 2)))"
    log "Created partition: $ANUNIX_PART"

    log "Formatting $ANUNIX_PART as ext2..."
    $SSH "$SUDO mkfs.ext2 -L anunix $ANUNIX_PART"
fi

# 4. Mount the partition
log "Mounting Anunix partition..."
$SSH "$SUDO mkdir -p /boot/anunix"
$SSH "$SUDO mount $ANUNIX_PART /boot/anunix 2>/dev/null || true"

# 5. Transfer kernel via HTTP (avoids scp auth complexity)
log "Starting upload server on Mac Studio..."
ELF_DIR=$(dirname "$(realpath "$ELF")")
ELF_NAME=$(basename "$ELF")

# Start Python HTTP server on Mac Studio serving the ELF
(cd "$ELF_DIR" && python3 -m http.server $HTTP_PORT --bind 0.0.0.0 > /tmp/elf_serve.log 2>&1) &
SERVER_PID=$!
sleep 1

MAC_IP=$(ifconfig utun3 2>/dev/null | grep 'inet ' | awk '{print $2}' || \
         ifconfig | grep '100\.' | grep 'inet ' | awk '{print $2}' | head -1)
[[ -n "$MAC_IP" ]] || MAC_IP=$(ifconfig | grep 'inet ' | grep -v '127\.' | awk '{print $2}' | head -1)
log "Mac Studio Tailscale/local IP: $MAC_IP"

log "Downloading kernel to target..."
$SSH "$SUDO curl -sf http://$MAC_IP:$HTTP_PORT/$ELF_NAME -o /boot/anunix/anunix.elf" || {
    kill $SERVER_PID 2>/dev/null || true
    die "Kernel download failed. Check that $MAC_IP:$HTTP_PORT is reachable from $TARGET."
}

kill $SERVER_PID 2>/dev/null || true
log "Kernel installed: /boot/anunix/anunix.elf"

# 6. Install GRUB custom config
log "Installing GRUB custom entry..."
ANUNIX_PART_UUID=$($SSH "$SUDO blkid -s UUID -o value $ANUNIX_PART")
log "Anunix partition UUID: $ANUNIX_PART_UUID"

$SSH "$SUDO tee /etc/grub.d/40_anunix > /dev/null" << 'GRUBSCRIPT'
#!/bin/sh
exec tail -n +4 $0
# Anunix custom GRUB entry

menuentry "Anunix (x86_64)" --class anunix {
    insmod part_gpt
    insmod ext2
    search --no-floppy --label --set=root anunix
    multiboot2 /anunix.elf
    boot
}
GRUBSCRIPT

$SSH "$SUDO chmod +x /etc/grub.d/40_anunix"
$SSH "$SUDO grub-mkconfig -o /boot/grub/grub.cfg 2>&1 | tail -5"

log "GRUB updated."

# 7. Summary
log ""
log "=== Deployment complete ==="
log "  Kernel:     /boot/anunix/anunix.elf"
log "  Partition:  $ANUNIX_PART (UUID $ANUNIX_PART_UUID)"
log "  GRUB entry: 'Anunix (x86_64)'"
log ""
log "Reboot target to test:"
log "  ssh $USER@$TARGET 'echo $SUDO_PASS | sudo -S shutdown -r now'"
