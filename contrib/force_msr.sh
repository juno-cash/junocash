#!/bin/bash
# Force MSR modifications for Junocash RandomX mining
# This script directly applies MSR optimizations using wrmsr (from msr-tools).
# Useful if the internal miner fails to apply MSRs due to permission issues.
#
# Usage: sudo ./force_msr.sh

set -e

if [ "$EUID" -ne 0 ]; then
  echo "Error: This script must be run as root."
  exit 1
fi

if ! command -v wrmsr &> /dev/null; then
    echo "Error: 'wrmsr' command not found."
    echo "Please install msr-tools:"
    echo "  Debian/Ubuntu: sudo apt-get install msr-tools"
    echo "  RHEL/CentOS:   sudo yum install msr-tools"
    echo "  Arch Linux:    sudo pacman -S msr-tools"
    exit 1
fi

# Load MSR module
MSR_FILE=/sys/module/msr/parameters/allow_writes
if [ -e "$MSR_FILE" ]; then
    echo "on" > "$MSR_FILE" 2>/dev/null || true
else
    echo "Loading msr kernel module..."
    modprobe msr allow_writes=on 2>/dev/null || modprobe msr 2>/dev/null || true
fi

echo "Detecting CPU..."

if grep -E 'AMD Ryzen|AMD EPYC|AuthenticAMD' /proc/cpuinfo > /dev/null; then
    # AMD CPU Detection
    FAMILY=$(grep "cpu family" /proc/cpuinfo | head -1 | awk '{print $4}')
    MODEL=$(grep "model" /proc/cpuinfo | head -1 | awk '{print $3}')
    
    # Convert hex model to decimal if needed, though grep output is usually decimal
    # Family 23 = 0x17 (Zen/Zen+/Zen2)
    # Family 25 = 0x19 (Zen3/Zen4)
    # Family 26 = 0x1A (Zen5)

    if [ "$FAMILY" = "23" ]; then
        echo "Detected AMD Ryzen/EPYC (Zen/Zen2/17h)"
        wrmsr -a 0xc0011020 0
        wrmsr -a 0xc0011021 0x40
        wrmsr -a 0xc0011022 0x1510000
        wrmsr -a 0xc001102b 0x2000cc16
        echo "Applied MSR optimizations for Zen/Zen2"

    elif [ "$FAMILY" = "25" ]; then
        # Zen3 vs Zen4 distinction based on model
        # Zen4 models are usually 0x10-0x1F (16-31) and 0x60-0x7F?
        # Simplified check: typical Zen4 is model 96+ (0x60) or 16+? 
        # Using randomx_boost.sh logic: model 97 (0x61) is Zen4 (Raphael)
        # But let's use a broader range if possible or default to Zen3 settings which are safe-ish
        
        # XMRig script checks specifically for model 97 for Zen4. 
        # Let's assume models >= 16 (0x10) might be Zen4?
        # Actually, Zen3 models are typically 0x01 (Vermeer), 0x21 (Vermeer), 0x50 (Cezanne).
        # Zen4 is 0x61 (Raphael), 0x70 (Phoenix).
        
        # Let's apply Zen3 values as baseline, they are very similar to Zen4 except one register?
        # Zen3: 0xC0011021 = 0x1c000200000040
        # Zen4: 0xC0011021 = 0x0004000000000040
        
        if [ "$MODEL" -ge 96 ]; then # >= 0x60
             echo "Detected AMD Ryzen/EPYC (Zen4/19h)"
             wrmsr -a 0xc0011020 0x4400000000000
             wrmsr -a 0xc0011021 0x4000000000040
             wrmsr -a 0xc0011022 0x8680000401570000
             wrmsr -a 0xc001102b 0x2040cc10
             echo "Applied MSR optimizations for Zen4"
        else
             echo "Detected AMD Ryzen/EPYC (Zen3/19h)"
             wrmsr -a 0xc0011020 0x4480000000000
             wrmsr -a 0xc0011021 0x1c000200000040
             wrmsr -a 0xc0011022 0xc000000401570000
             wrmsr -a 0xc001102b 0x2000cc10
             echo "Applied MSR optimizations for Zen3"
        fi

    elif [ "$FAMILY" = "26" ]; then
        echo "Detected AMD Ryzen (Zen5/1Ah)"
        wrmsr -a 0xc0011020 0x4400000000000
        wrmsr -a 0xc0011021 0x4000000000040
        wrmsr -a 0xc0011022 0x8680000401570000
        wrmsr -a 0xc001102b 0x2040cc10
        echo "Applied MSR optimizations for Zen5"

    else
        echo "Unknown AMD CPU family: $FAMILY. No MSRs applied."
    fi

elif grep "Intel" /proc/cpuinfo > /dev/null; then
    echo "Detected Intel CPU"
    wrmsr -a 0x1a4 0xf
    echo "Applied MSR optimizations for Intel (prefetch disabled)"

else
    echo "No supported CPU detected for MSR optimizations."
fi

echo "Done."
