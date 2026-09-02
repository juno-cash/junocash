// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "crypto/randomx_msr.h"
#include "crypto/msr.h"
#include "util/system.h"

#include <algorithm>
#include <set>
#include <fstream>
#include <string>

// CPU vendor detection
enum CPUVendor {
    CPU_VENDOR_UNKNOWN = 0,
    CPU_VENDOR_AMD,
    CPU_VENDOR_INTEL
};

// AMD CPU family detection
enum AMDFamily {
    AMD_UNKNOWN = 0,
    AMD_RYZEN_17H,  // Zen/Zen+/Zen2
    AMD_RYZEN_19H,  // Zen3/Zen4
    AMD_RYZEN_ZEN4,
    AMD_RYZEN_ZEN5
};

// MSR mod type
enum MsrMod {
    MSR_MOD_NONE = 0,
    MSR_MOD_RYZEN_17H,
    MSR_MOD_RYZEN_19H,
    MSR_MOD_RYZEN_ZEN4,
    MSR_MOD_RYZEN_ZEN5,
    MSR_MOD_INTEL,
    MSR_MOD_CUSTOM,
    MSR_MOD_MAX
};

// MSR presets for different CPU architectures
static const std::vector<std::vector<MsrItem>> msrPresets = {
    // MSR_MOD_NONE
    {},

    // MSR_MOD_RYZEN_17H (Zen/Zen+/Zen2)
    {
        MsrItem(0xC0011020, 0ULL),
        MsrItem(0xC0011021, 0x40ULL, ~0x20ULL),
        MsrItem(0xC0011022, 0x1510000ULL),
        MsrItem(0xC001102b, 0x2000cc16ULL)
    },

    // MSR_MOD_RYZEN_19H (Zen3)
    {
        MsrItem(0xC0011020, 0x0004480000000000ULL),
        MsrItem(0xC0011021, 0x001c000200000040ULL, ~0x20ULL),
        MsrItem(0xC0011022, 0xc000000401570000ULL),
        MsrItem(0xC001102b, 0x2000cc10ULL)
    },

    // MSR_MOD_RYZEN_ZEN4
    {
        MsrItem(0xC0011020, 0x0004400000000000ULL),
        MsrItem(0xC0011021, 0x0004000000000040ULL, ~0x20ULL),
        MsrItem(0xC0011022, 0x8680000401570000ULL),
        MsrItem(0xC001102b, 0x2040cc10ULL)
    },

    // MSR_MOD_RYZEN_ZEN5
    {
        MsrItem(0xC0011020, 0x0004400000000000ULL),
        MsrItem(0xC0011021, 0x0004000000000040ULL, ~0x20ULL),
        MsrItem(0xC0011022, 0x8680000401570000ULL),
        MsrItem(0xC001102b, 0x2040cc10ULL)
    },

    // MSR_MOD_INTEL
    {
        MsrItem(0x1a4, 0xf)
    },

    // MSR_MOD_CUSTOM (empty, for future use)
    {}
};

static const char* msrModNames[] = {
    "none", "ryzen_17h", "ryzen_19h", "ryzen_zen4", "ryzen_zen5", "intel", "custom"
};

// Detect CPU vendor
static CPUVendor DetectCPUVendor() {
#ifdef __x86_64__
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];

    __asm__ __volatile__(
        "cpuid"
        : "=b"(ebx), "=d"(edx), "=c"(ecx)
        : "a"(0)
    );

    memcpy(vendor, &ebx, 4);
    memcpy(vendor + 4, &edx, 4);
    memcpy(vendor + 8, &ecx, 4);
    vendor[12] = '\0';

    if (strcmp(vendor, "AuthenticAMD") == 0) {
        return CPU_VENDOR_AMD;
    } else if (strcmp(vendor, "GenuineIntel") == 0) {
        return CPU_VENDOR_INTEL;
    }
#endif
    return CPU_VENDOR_UNKNOWN;
}

// Detect AMD CPU family
static AMDFamily DetectAMDFamily() {
#ifdef __x86_64__
    uint32_t eax, ebx, ecx, edx;

    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1)
    );

    uint32_t family = ((eax >> 8) & 0xF) + ((eax >> 20) & 0xFF);
    uint32_t model = ((eax >> 4) & 0xF) | ((eax >> 12) & 0xF0);

    LogPrint("msr", "AMD CPU Family: 0x%x, Model: 0x%x\n", family, model);

    if (family == 0x17) {
        return AMD_RYZEN_17H;
    } else if (family == 0x19) {
        // Zen3 and Zen4 are both family 19h
        // Zen4 typically has model >= 0x10
        if (model >= 0x10 && model < 0x70) {
            return AMD_RYZEN_ZEN4;
        } else if (model >= 0x70) {
            return AMD_RYZEN_ZEN5;
        }
        return AMD_RYZEN_19H;
    }
#endif
    return AMD_UNKNOWN;
}

// Check if CPU supports L3 cache QoS
static bool HasCatL3() {
#ifdef __x86_64__
    uint32_t eax, ebx, ecx, edx;

    // Check CPUID leaf 0x10 (Resource Director Technology)
    __asm__ __volatile__(
        "cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x10), "c"(0)
    );

    // Bit 1 indicates L3 cache QoS support
    return (ebx & 0x2) != 0;
#endif
    return false;
}

// Auto-detect appropriate MSR preset
static MsrMod DetectMsrMod() {
    CPUVendor vendor = DetectCPUVendor();

    if (vendor == CPU_VENDOR_AMD) {
        AMDFamily family = DetectAMDFamily();
        switch (family) {
            case AMD_RYZEN_17H:
                LogPrintf("MSR: Detected AMD Ryzen 17H (Zen/Zen+/Zen2)\n");
                return MSR_MOD_RYZEN_17H;
            case AMD_RYZEN_19H:
                LogPrintf("MSR: Detected AMD Ryzen 19H (Zen3)\n");
                return MSR_MOD_RYZEN_19H;
            case AMD_RYZEN_ZEN4:
                LogPrintf("MSR: Detected AMD Ryzen Zen4\n");
                return MSR_MOD_RYZEN_ZEN4;
            case AMD_RYZEN_ZEN5:
                LogPrintf("MSR: Detected AMD Ryzen Zen5\n");
                return MSR_MOD_RYZEN_ZEN5;
            default:
                LogPrintf("MSR: Unknown AMD CPU family, no MSR preset available\n");
                return MSR_MOD_NONE;
        }
    } else if (vendor == CPU_VENDOR_INTEL) {
        LogPrintf("MSR: Detected Intel CPU\n");
        return MSR_MOD_INTEL;
    }

    LogPrintf("MSR: Unknown CPU vendor, no MSR preset available\n");
    return MSR_MOD_NONE;
}

// Static members
bool RandomX_Msr::m_enabled = false;
bool RandomX_Msr::m_initialized = false;
bool RandomX_Msr::m_cache_qos = false;

// Store original MSR values for restoration
static MsrItems original_msrs;

// Apply MSR preset and optionally configure cache QoS
static bool ApplyMsrPreset(const MsrItems& preset, const std::vector<int>& thread_affinities, bool cache_qos) {
    auto msr = Msr::get();
    if (!msr || !msr->isAvailable()) {
        return false;
    }

    // Save original values
    original_msrs.clear();
    original_msrs.reserve(preset.size());

    for (const auto& item : preset) {
        auto orig = msr->read(item.reg());
        if (!orig.isValid()) {
            LogPrintf("MSR: Failed to read register 0x%08x for backup\n", item.reg());
            original_msrs.clear();
            return false;
        }
        original_msrs.push_back(orig);
    }

    // Determine which CPU cores will have access to full L3 cache
    std::set<int32_t> cache_enabled(thread_affinities.begin(), thread_affinities.end());
    bool cache_qos_disabled = thread_affinities.empty();

    if (cache_qos && !cache_qos_disabled && !HasCatL3()) {
        LogPrintf("MSR: WARNING - This CPU doesn't support CAT L3, cache QoS is unavailable\n");
        cache_qos = false;
    }

    // Apply MSR preset to all CPUs
    bool success = msr->write([&](int32_t cpu) {
        // Apply preset items
        for (const auto& item : preset) {
            if (!msr->write(item, cpu, false)) {
                return false;
            }
        }

        // Apply cache QoS if enabled
        if (cache_qos && !cache_qos_disabled) {
            // Assign Class Of Service 0 to mining cores (full L3 cache)
            if (cache_enabled.count(cpu)) {
                if (!msr->write(0xC8F, 0, cpu, MsrItem::kNoMask, false)) {
                    return false;
                }
            } else {
                // Disable L3 cache for Class Of Service 1
                if (!msr->write(0xC91, 0, cpu, MsrItem::kNoMask, false)) {
                    // Some CPUs don't let set it to all zeros
                    if (!msr->write(0xC91, 1, cpu, MsrItem::kNoMask, false)) {
                        return false;
                    }
                }
                // Assign Class Of Service 1 to non-mining cores
                if (!msr->write(0xC8F, 1ULL << 32, cpu, MsrItem::kNoMask, false)) {
                    return false;
                }
            }
        }

        return true;
    });

    return success;
}

bool RandomX_Msr::Init(const std::vector<int>& thread_affinities, bool enable_cache_qos) {
    if (m_initialized) {
        return m_enabled;
    }

    m_initialized = true;
    m_enabled = false;
    m_cache_qos = enable_cache_qos;

    // Auto-detect MSR preset
    MsrMod mod = DetectMsrMod();
    if (mod == MSR_MOD_NONE || mod >= MSR_MOD_MAX) {
        LogPrintf("MSR: No MSR preset available for this CPU\n");
        return false;
    }

    const auto& preset = msrPresets[mod];
    if (preset.empty()) {
        LogPrintf("MSR: MSR preset is empty\n");
        return false;
    }

    LogPrintf("MSR: Applying '%s' preset with %d MSR modifications%s\n",
              msrModNames[mod], preset.size(),
              m_cache_qos ? " and cache QoS" : "");

    if (m_cache_qos && thread_affinities.empty()) {
        LogPrintf("MSR: WARNING - Cache QoS requires thread affinity to be set\n");
    }

    if ((m_enabled = ApplyMsrPreset(preset, thread_affinities, m_cache_qos))) {
        LogPrintf("MSR: Successfully applied '%s' preset\n", msrModNames[mod]);
        if (m_cache_qos && !thread_affinities.empty()) {
            LogPrintf("MSR: Cache QoS enabled for %d mining threads\n", thread_affinities.size());
        }
    } else {
        LogPrintf("MSR: FAILED TO APPLY MSR MODIFICATIONS - HASHRATE WILL BE LOWER\n");
        LogPrintf("MSR: Make sure you have root privileges and the msr kernel module is loaded\n");
        LogPrintf("MSR: Run: sudo modprobe msr\n");
    }

    return m_enabled;
}

void RandomX_Msr::Destroy() {
    if (!m_initialized || original_msrs.empty()) {
        return;
    }

    LogPrintf("MSR: Restoring original MSR values...\n");

    auto msr = Msr::get();
    if (!msr || !msr->isAvailable()) {
        LogPrintf("MSR: Cannot restore MSR values - MSR not available\n");
        return;
    }

    bool success = msr->write([&](int32_t cpu) {
        for (const auto& item : original_msrs) {
            if (!msr->write(item, cpu, false)) {
                return false;
            }
        }
        return true;
    });

    if (success) {
        LogPrintf("MSR: Successfully restored original MSR values\n");
    } else {
        LogPrintf("MSR: WARNING - Failed to restore some MSR values\n");
    }

    original_msrs.clear();
    m_initialized = false;
    m_enabled = false;
}

bool RandomX_Msr::IsEnabled() {
    return m_enabled;
}
