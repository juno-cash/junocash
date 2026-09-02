#!/bin/bash
# Setup DMI/SMBIOS read permissions for Junocash
# This script configures system permissions to allow non-root users to read
# hardware information (CPU, memory, motherboard) via DMI/SMBIOS tables.
#
# Usage: sudo ./setup-dmi-permissions.sh [username]
#
# If username is not provided, it will be detected automatically.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
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

echo -e "${GREEN}Setting up DMI/SMBIOS permissions for user: $TARGET_USER${NC}"
echo

# Step 1: Create 'dmi' group if it doesn't exist
if getent group dmi >/dev/null 2>&1; then
    echo -e "${YELLOW}✓ Group 'dmi' already exists${NC}"
else
    echo "Creating 'dmi' group..."
    groupadd dmi
    echo -e "${GREEN}✓ Created group 'dmi'${NC}"
fi

# Step 2: Add user to 'dmi' group
if id -nG "$TARGET_USER" | grep -qw dmi; then
    echo -e "${YELLOW}✓ User '$TARGET_USER' is already in group 'dmi'${NC}"
else
    echo "Adding user '$TARGET_USER' to group 'dmi'..."
    usermod -a -G dmi "$TARGET_USER"
    echo -e "${GREEN}✓ Added user '$TARGET_USER' to group 'dmi'${NC}"
fi

# Step 3: Create udev rule
UDEV_RULE_FILE="/etc/udev/rules.d/99-dmi.rules"
echo "Creating udev rule: $UDEV_RULE_FILE"

cat > "$UDEV_RULE_FILE" << 'EOF'
# Allow 'dmi' group to read DMI/SMBIOS tables
# This enables hardware detection (CPU, memory, motherboard) for mining software
SUBSYSTEM=="dmi", KERNEL=="DMI", GROUP="dmi", MODE="0440"
SUBSYSTEM=="dmi", KERNEL=="smbios_entry_point", GROUP="dmi", MODE="0440"
EOF

echo -e "${GREEN}✓ Created udev rule${NC}"

# Step 4: Reload udev rules
echo "Reloading udev rules..."
udevadm control --reload-rules
udevadm trigger --subsystem-match=dmi

# Wait a moment for udev to apply changes
sleep 1

echo -e "${GREEN}✓ Reloaded udev rules${NC}"

# Step 5: Verify permissions
echo
echo "Verifying DMI permissions..."

DMI_FILE="/sys/firmware/dmi/tables/DMI"
SMBIOS_FILE="/sys/firmware/dmi/tables/smbios_entry_point"

if [ -r "$DMI_FILE" ] && [ -r "$SMBIOS_FILE" ]; then
    echo -e "${GREEN}✓ DMI tables are now readable${NC}"
    ls -la "$DMI_FILE" "$SMBIOS_FILE" | sed 's/^/  /'
else
    echo -e "${YELLOW}⚠ DMI tables permissions updated, but files may not be immediately readable${NC}"
    echo "  This is normal - permissions will be applied after next reboot or udev event"
fi

# Final instructions
echo
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}Setup Complete!${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo
echo "IMPORTANT: For changes to take effect, user '$TARGET_USER' must:"
echo "  1. Log out and log back in (group membership)"
echo "  OR"
echo "  2. Run: newgrp dmi"
echo
echo "After logging back in, verify with: groups"
echo "You should see 'dmi' in your groups list."
echo
echo "Then Junocash will automatically detect hardware information!"
echo

exit 0
