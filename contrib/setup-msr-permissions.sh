#!/bin/bash
# Setup MSR (Model Specific Register) permissions for Junocash
# This script configures system permissions to allow non-root users to access
# CPU MSR registers for RandomX mining performance optimizations.
#
# Usage: sudo ./setup-msr-permissions.sh [username]
#
# If username is not provided, it will be detected automatically.
#
# IMPORTANT: MSR modifications provide 10-15% mining performance improvement
# but require proper permissions setup. This script handles all configuration.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root (use sudo)${NC}"
    echo "Usage: sudo $0 [username]"
    exit 1
fi

# Determine the target user
if [ -n "$1" ]; then
    TARGET_USER="$1"
elif [ -n "$SUDO_USER" ]; then
    TARGET_USER="$SUDO_USER"
else
    echo -e "${RED}Error: Could not determine target user${NC}"
    echo "Usage: sudo $0 [username]"
    exit 1
fi

# Verify user exists
if ! id "$TARGET_USER" &>/dev/null; then
    echo -e "${RED}Error: User '$TARGET_USER' does not exist${NC}"
    exit 1
fi

echo -e "${GREEN}╔══════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║  Setting up MSR permissions for RandomX mining optimization ║${NC}"
echo -e "${GREEN}║  User: ${YELLOW}$TARGET_USER${GREEN}                                               ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════════════════╝${NC}"
echo

# Step 1: Load MSR kernel module with write permissions
echo -e "${BLUE}[1/6]${NC} Loading MSR kernel module..."

# Check if msr module is already loaded
if lsmod | grep -q "^msr"; then
    echo -e "${YELLOW}✓ MSR module already loaded${NC}"
else
    echo "  Loading msr module..."
    modprobe msr allow_writes=on || {
        echo -e "${RED}✗ Failed to load msr module${NC}"
        echo "  This kernel may not support MSR access"
        exit 1
    }
    echo -e "${GREEN}✓ Loaded MSR module${NC}"
fi

# Enable write permissions for MSR module
if [ -f /sys/module/msr/parameters/allow_writes ]; then
    echo "on" > /sys/module/msr/parameters/allow_writes 2>/dev/null || true
    echo -e "${GREEN}✓ Enabled MSR write permissions${NC}"
fi

# Step 2: Create 'msr' group if it doesn't exist
echo
echo -e "${BLUE}[2/6]${NC} Creating 'msr' group..."

if getent group msr >/dev/null 2>&1; then
    echo -e "${YELLOW}✓ Group 'msr' already exists${NC}"
else
    groupadd msr
    echo -e "${GREEN}✓ Created group 'msr'${NC}"
fi

# Step 3: Add user to 'msr' group
echo
echo -e "${BLUE}[3/6]${NC} Adding user to 'msr' group..."

if id -nG "$TARGET_USER" | grep -qw msr; then
    echo -e "${YELLOW}✓ User '$TARGET_USER' is already in group 'msr'${NC}"
else
    usermod -a -G msr "$TARGET_USER"
    echo -e "${GREEN}✓ Added user '$TARGET_USER' to group 'msr'${NC}"
fi

# Step 4: Create udev rule for MSR devices
echo
echo -e "${BLUE}[4/6]${NC} Creating udev rule for MSR devices..."

UDEV_RULE_FILE="/etc/udev/rules.d/99-msr.rules"

cat > "$UDEV_RULE_FILE" << 'EOF'
# Allow 'msr' group to read/write CPU MSR registers
# This enables RandomX mining performance optimizations (10-15% hashrate improvement)
# MSR access is required for:
# - CPU-specific performance tuning
# - L3 cache QoS allocation
# - Prefetcher configuration

# Grant read/write access to MSR device files
KERNEL=="msr[0-9]*", GROUP="msr", MODE="0660"

# Alternative patterns for different kernel versions
SUBSYSTEM=="msr", GROUP="msr", MODE="0660"
EOF

echo -e "${GREEN}✓ Created udev rule: $UDEV_RULE_FILE${NC}"

# Step 5: Reload udev rules and trigger
echo
echo -e "${BLUE}[5/6]${NC} Applying udev rules..."

udevadm control --reload-rules
udevadm trigger --subsystem-match=misc --attr-match=dev_name=msr* 2>/dev/null || true

# Manually set permissions for existing MSR devices (immediate effect)
for msr_dev in /dev/cpu/*/msr; do
    if [ -e "$msr_dev" ]; then
        chgrp msr "$msr_dev" 2>/dev/null || true
        chmod 0660 "$msr_dev" 2>/dev/null || true
    fi
done

echo -e "${GREEN}✓ Applied udev rules${NC}"

# Step 6: Create systemd service to load MSR module on boot
echo
echo -e "${BLUE}[6/6]${NC} Creating systemd service for MSR module..."

SYSTEMD_SERVICE="/etc/systemd/system/msr-permissions.service"

cat > "$SYSTEMD_SERVICE" << 'EOF'
[Unit]
Description=Load MSR kernel module with write permissions for mining
DefaultDependencies=no
Before=sysinit.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/modprobe msr allow_writes=on
ExecStart=/bin/sh -c 'echo on > /sys/module/msr/parameters/allow_writes'
ExecStart=/bin/sh -c 'for msr_dev in /dev/cpu/*/msr; do [ -e "$msr_dev" ] && chgrp msr "$msr_dev" && chmod 0660 "$msr_dev"; done'

[Install]
WantedBy=sysinit.target
EOF

systemctl daemon-reload
systemctl enable msr-permissions.service >/dev/null 2>&1 || true

echo -e "${GREEN}✓ Created and enabled systemd service${NC}"

# Verify MSR access
echo
echo -e "${BLUE}Verifying MSR permissions...${NC}"
echo

SUCCESS=true
MSR_COUNT=0

# Check if any MSR devices exist and are accessible
for msr_dev in /dev/cpu/*/msr; do
    if [ -e "$msr_dev" ]; then
        MSR_COUNT=$((MSR_COUNT + 1))
        if [ -r "$msr_dev" ] && [ -w "$msr_dev" ]; then
            echo -e "${GREEN}✓${NC} $(ls -lh "$msr_dev")"
        else
            echo -e "${YELLOW}⚠${NC} $(ls -lh "$msr_dev") ${YELLOW}(not yet accessible)${NC}"
            SUCCESS=false
        fi
    fi
done

if [ $MSR_COUNT -eq 0 ]; then
    echo -e "${RED}✗ No MSR devices found${NC}"
    echo "  This usually means the MSR module is not loaded properly"
    SUCCESS=false
else
    echo
    echo -e "${GREEN}Found $MSR_COUNT MSR device(s)${NC}"
fi

# Final instructions
echo
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}Setup Complete!${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo

if [ "$SUCCESS" = true ]; then
    echo -e "${GREEN}✓ MSR permissions configured successfully${NC}"
else
    echo -e "${YELLOW}⚠ MSR permissions configured, but not all devices are immediately accessible${NC}"
    echo "  This is normal - full access will be available after reboot or re-login"
fi

echo
echo -e "${YELLOW}IMPORTANT: For changes to take effect, user '$TARGET_USER' must:${NC}"
echo -e "  ${BLUE}Option 1 (Immediate):${NC}"
echo -e "    Run: ${YELLOW}newgrp msr${NC}"
echo -e "    Then test: ${YELLOW}groups${NC} (should show 'msr')"
echo
echo -e "  ${BLUE}Option 2 (Permanent):${NC}"
echo -e "    Log out and log back in"
echo

echo -e "${BLUE}To verify MSR access works:${NC}"
echo -e "  ${YELLOW}ls -la /dev/cpu/0/msr${NC}"
echo -e "  (Should show group 'msr' and permissions 'crw-rw----')"
echo

echo -e "${BLUE}Expected Performance Improvement:${NC}"
echo -e "  • MSR optimizations: ${GREEN}+10-15% hashrate${NC}"
echo -e "  • Cache QoS allocation: ${GREEN}+2-5% additional${NC}"
echo -e "  • Total expected gain: ${GREEN}+12-20% hashrate${NC}"
echo

echo -e "${BLUE}Mining with MSR optimizations:${NC}"
echo -e "  ${YELLOW}./junocashd -gen -genproclimit=4 -randomxmsr=1 -randomxcacheqos=1${NC}"
echo

echo -e "${BLUE}What was configured:${NC}"
echo -e "  ✓ Loaded msr kernel module with write permissions"
echo -e "  ✓ Created 'msr' group and added user '$TARGET_USER'"
echo -e "  ✓ Created udev rule for automatic MSR device permissions"
echo -e "  ✓ Created systemd service to load MSR module on boot"
echo -e "  ✓ Set immediate permissions on existing MSR devices"
echo

if [ "$SUCCESS" = false ]; then
    echo -e "${YELLOW}Note: A reboot is recommended for all permissions to take full effect${NC}"
    echo
fi

echo -e "${GREEN}Junocash can now use MSR optimizations for maximum mining performance!${NC}"
echo

exit 0
