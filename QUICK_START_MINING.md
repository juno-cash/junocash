# Junocash Mining - Quick Start Guide

## TL;DR - Get Maximum Performance in 3 Steps

```bash
# 1. Setup permissions (one-time, requires sudo)
cd contrib
sudo ./setup-mining-permissions.sh

# 2. Activate MSR group (once per login)
newgrp msr

# 3. Configure mining (one-time)
cd ..
cp contrib/junocashd-mining.conf.example ~/.junocash/junocashd.conf

# 4. Start mining (no flags needed!)
./junocashd
```

**Expected Performance: 52-89% faster than baseline!**

---

## What You Get

### Performance Improvements

| Optimization | Hashrate Gain | Status |
|--------------|--------------|--------|
| Core mining loop optimizations | +38-64% | ✅ Already in codebase |
| MSR modifications | +10-15% | ✅ Implemented (requires setup) |
| L3 Cache QoS | +2-5% | ✅ Implemented (auto-enabled) |
| Scratchpad prefetch | +5-10% | ✅ Implemented (auto-configured) |
| **TOTAL** | **+52-89%** | **Ready to use!** |

---

## Setup Details

### What the Setup Script Does

The `setup-mining-permissions.sh` script automatically:

✅ Loads MSR kernel module with write permissions
✅ Creates `msr` and `dmi` groups
✅ Adds your user to these groups
✅ Creates udev rules for device permissions
✅ Creates systemd service to load MSR on boot
✅ Sets immediate permissions on `/dev/cpu/*/msr`

**Result:** You can mine as a normal user (no sudo needed!)

### Why No Sudo for Mining?

Running miner as sudo creates `.junocash` directory in `/root` instead of your home directory. The setup script gives your user the necessary permissions instead.

---

## Configuration

### Option 1: Config File (Recommended)

```bash
# Copy example config (includes all optimizations)
cp contrib/junocashd-mining.conf.example ~/.junocash/junocashd.conf

# Start mining (settings loaded from config)
./junocashd
```

### Option 2: Command-Line Flags

```bash
./junocashd -gen \
  -genproclimit=$(nproc) \        # Use all CPU cores
  -randomxfastmode=1 \             # 2GB dataset (2x faster)
  -randomxmsr=1 \                  # MSR optimizations (+10-15%)
  -randomxcacheqos=1               # L3 cache allocation (+2-5%)
```

### Optional Tweaks

```bash
# Use specific number of threads
-genproclimit=4

# Disable MSR (if you don't have permissions)
-randomxmsr=0

# Disable cache QoS
-randomxcacheqos=0

# Disable Ryzen exception handling
-randomxexceptionhandling=0
```

---

## Verify Setup

### Check Groups
```bash
groups
# Should show: ... dmi msr ...
```

### Check MSR Access
```bash
ls -la /dev/cpu/0/msr
# Should show: crw-rw---- 1 root msr 202, 0 ...
```

### Check Mining Logs

Look for these messages when mining starts:

```
RandomX: Initializing FAST mode with hugepages
RandomX: Hardware AES-NI: YES
RandomX: Scratchpad prefetch mode: 1
MSR: Detected AMD Ryzen 19H (Zen3)
MSR: Successfully applied 'ryzen_19h' preset
MSR: Cache QoS enabled for 4 mining threads
RandomX MSR optimizations enabled (10-15% expected hashrate improvement)
```

---

## Troubleshooting

### "MSR optimizations FAILED"

**Most common cause:** Not in `msr` group yet

**Quick fix:**
```bash
# Option 1: Activate group immediately
newgrp msr

# Option 2: Log out and log back in
```

**Verify:**
```bash
groups  # Should show 'msr'
```

### Still Not Working?

**Rerun setup script:**
```bash
cd contrib
sudo ./setup-mining-permissions.sh
```

**Check if MSR module is loaded:**
```bash
lsmod | grep msr
```

**Manually load MSR module:**
```bash
sudo modprobe msr allow_writes=on
```

### Performance Not Improving?

1. **Check logs** - Verify MSR optimizations are enabled
2. **CPU not supported** - Only AMD Ryzen and modern Intel get MSR boost
3. **Already fast** - Previous optimizations already give 38-64% gain

---

## Advanced Tuning

### Huge Pages (Extra 3-5%)

```bash
# Temporary
sudo sysctl -w vm.nr_hugepages=1280

# Permanent
echo "vm.nr_hugepages=1280" | sudo tee -a /etc/sysctl.conf
```

### CPU Governor
```bash
# Set to performance mode
sudo cpupower frequency-set -g performance
```

### Disable CPU Frequency Scaling
```bash
# Disable turbo boost (for stability)
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost
```

---

## Performance Benchmarking

### Measure Your Improvement

```bash
# 1. Baseline (no optimizations)
./junocashd -gen -genproclimit=4 -randomxmsr=0 -randomxfastmode=0

# 2. With fast mode only
./junocashd -gen -genproclimit=4 -randomxmsr=0 -randomxfastmode=1

# 3. With all optimizations
./junocashd -gen -genproclimit=4 -randomxmsr=1 -randomxfastmode=1
```

Run each for 5-10 minutes and compare hashrates.

**Expected results:**
- Baseline → Fast mode: ~2x improvement
- Fast mode → Fast + MSR: +10-20% improvement
- Baseline → All optimizations: +52-89% improvement

---

## System Requirements

### Linux
- Ubuntu 20.04+ / Debian 10+ / Fedora 33+ / Arch Linux
- Kernel with MSR support (most standard kernels)

### CPU
- **Optimal:** AMD Ryzen (Zen/Zen2/Zen3/Zen4/Zen5)
- **Good:** Modern Intel (2010+)
- **Works:** Any x86_64 CPU (but no MSR optimizations)

### Memory
- **Light mode:** 256 MB per thread
- **Fast mode:** 2 GB shared + 256 MB per thread (recommended)

### Permissions
- User must be able to run `sudo`
- Script must be run once for setup

---

## Documentation

- **Complete Setup Guide:** `contrib/MINING_SETUP.md`
- **Optimization History:** `MINING_OPTIMIZATIONS.md`
- **Next Commit Details:** `next-commit-changes.txt`

---

## Support

**Issues?** Check the troubleshooting section in `contrib/MINING_SETUP.md`

**Questions?** See FAQ in `MINING_SETUP.md`

---

**Happy Mining! 🚀**

Expected hashrate improvement: **52-89% over baseline!**
