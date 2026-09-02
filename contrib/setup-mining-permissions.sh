#!/bin/bash
# Setup all mining-related permissions for Junocash
# This is a convenience script that runs both DMI and MSR permission setup.
#
# Usage: sudo ./setup-mining-permissions.sh [username]
#
# If username is not provided, it will be detected automatically.
#
# This script configures:
# - DMI/SMBIOS access for hardware detection
# - MSR access for RandomX mining optimizations (10-15% performance gain)

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo -e "${RED}Error: This script must be run as root (use sudo)${NC}"
    echo "Usage: sudo $0 [username]"
    exit 1
fi

# Get the directory where this script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

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

echo -e "${CYAN}╔═══════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║                                                                   ║${NC}"
echo -e "${CYAN}║         Junocash Mining Permissions Setup (All-in-One)         ║${NC}"
echo -e "${CYAN}║                                                                   ║${NC}"
echo -e "${CYAN}║  This script will configure your system for optimal mining:      ║${NC}"
echo -e "${CYAN}║    • DMI/SMBIOS access (hardware detection)                      ║${NC}"
echo -e "${CYAN}║    • MSR access (10-15% performance improvement)                 ║${NC}"
echo -e "${CYAN}║                                                                   ║${NC}"
echo -e "${CYAN}║  Target user: ${YELLOW}$TARGET_USER${CYAN}                                           ║${NC}"
echo -e "${CYAN}║                                                                   ║${NC}"
echo -e "${CYAN}╚═══════════════════════════════════════════════════════════════════╝${NC}"
echo
sleep 1

# Run DMI setup
echo -e "${GREEN}═══════════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}STEP 1: Setting up DMI/SMBIOS permissions${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════════════${NC}"
echo

if [ -f "$SCRIPT_DIR/setup-dmi-permissions.sh" ]; then
    bash "$SCRIPT_DIR/setup-dmi-permissions.sh" "$TARGET_USER"
    DMI_STATUS=$?
else
    echo -e "${RED}Error: setup-dmi-permissions.sh not found in $SCRIPT_DIR${NC}"
    DMI_STATUS=1
fi

echo
sleep 1

# Run MSR setup
echo -e "${GREEN}═══════════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}STEP 2: Setting up MSR permissions${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════════════${NC}"
echo

if [ -f "$SCRIPT_DIR/setup-msr-permissions.sh" ]; then
    bash "$SCRIPT_DIR/setup-msr-permissions.sh" "$TARGET_USER"
    MSR_STATUS=$?
else
    echo -e "${RED}Error: setup-msr-permissions.sh not found in $SCRIPT_DIR${NC}"
    MSR_STATUS=1
fi

# Summary
echo
echo -e "${CYAN}╔═══════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${CYAN}║                       SETUP SUMMARY                               ║${NC}"
echo -e "${CYAN}╚═══════════════════════════════════════════════════════════════════╝${NC}"
echo

if [ $DMI_STATUS -eq 0 ]; then
    echo -e "  DMI/SMBIOS setup: ${GREEN}✓ Success${NC}"
else
    echo -e "  DMI/SMBIOS setup: ${RED}✗ Failed${NC}"
fi

if [ $MSR_STATUS -eq 0 ]; then
    echo -e "  MSR setup:        ${GREEN}✓ Success${NC}"
else
    echo -e "  MSR setup:        ${RED}✗ Failed${NC}"
fi

echo
echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${YELLOW}NEXT STEPS FOR USER '$TARGET_USER':${NC}"
echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo
echo -e "${BLUE}Option 1 - Quick activation (no logout required):${NC}"
echo -e "  ${YELLOW}newgrp dmi${NC}"
echo -e "  ${YELLOW}newgrp msr${NC}"
echo
echo -e "${BLUE}Option 2 - Log out and log back in${NC}"
echo "  (This will activate both group memberships automatically)"
echo
echo -e "${BLUE}Verify setup:${NC}"
echo -e "  ${YELLOW}groups${NC}"
echo "  (Should show both 'dmi' and 'msr' in your groups)"
echo
echo -e "${BLUE}Test MSR access:${NC}"
echo -e "  ${YELLOW}ls -la /dev/cpu/0/msr${NC}"
echo "  (Should show: crw-rw---- ... msr)"
echo
echo -e "${BLUE}Start mining with all optimizations:${NC}"
echo -e "  ${YELLOW}./junocashd -gen -genproclimit=4 -randomxfastmode=1 -randomxmsr=1${NC}"
echo
echo -e "${GREEN}Expected performance improvement: +12-20% hashrate from MSR optimizations!${NC}"
echo

if [ $DMI_STATUS -ne 0 ] || [ $MSR_STATUS -ne 0 ]; then
    echo -e "${RED}Some setup steps failed. Please check the error messages above.${NC}"
    echo
    exit 1
fi

exit 0
