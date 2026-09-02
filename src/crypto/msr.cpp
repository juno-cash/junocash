// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "crypto/msr.h"
#include "util/system.h"

#include <array>
#include <cstdio>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

// Get number of CPU cores
static int GetNumCPUs() {
    return std::thread::hardware_concurrency();
}

// Get list of all CPU IDs
static std::vector<int32_t> GetCPUList() {
    std::vector<int32_t> cpus;
    int num = GetNumCPUs();
    for (int i = 0; i < num; i++) {
        cpus.push_back(i);
    }
    return cpus;
}

#ifdef __linux__

class Msr::MsrPrivate
{
public:
    MsrPrivate() : m_available(msr_allow_writes() || msr_modprobe()) {}

    bool isAvailable() const { return m_available; }

private:
    bool msr_allow_writes() {
        std::ofstream file("/sys/module/msr/parameters/allow_writes", std::ios::out | std::ios::binary | std::ios::trunc);
        if (file.is_open()) {
            file << "on";
        }
        return file.good();
    }

    bool msr_modprobe() {
        return system("/sbin/modprobe msr allow_writes=on > /dev/null 2>&1") == 0;
    }

    const bool m_available;
};

static int msr_open(int32_t cpu, int flags) {
    if (cpu < 0) {
        auto cpus = GetCPUList();
        cpu = cpus.empty() ? 0 : cpus.front();
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", cpu);
    return open(path, flags);
}

#else // Non-Linux platforms

class Msr::MsrPrivate
{
public:
    MsrPrivate() : m_available(false) {}
    bool isAvailable() const { return m_available; }

private:
    const bool m_available;
};

static int msr_open(int32_t, int) { return -1; }

#endif

// Static instance
static std::shared_ptr<Msr> msr_instance;

const char *Msr::tag() {
    return "msr";
}

std::shared_ptr<Msr> Msr::get() {
    if (!msr_instance) {
        msr_instance = std::make_shared<Msr>();
    }
    return msr_instance;
}

Msr::Msr() : d_ptr(new MsrPrivate()) {
    if (!isAvailable()) {
        LogPrintf("MSR: WARNING - msr kernel module is not available\n");
    }
}

Msr::~Msr() {
    delete d_ptr;
}

bool Msr::isAvailable() const {
    return d_ptr->isAvailable();
}

bool Msr::write(const MsrItem &item, int32_t cpu, bool verbose) {
    return write(item.reg(), item.value(), cpu, item.mask(), verbose);
}

bool Msr::write(uint32_t reg, uint64_t value, int32_t cpu, uint64_t mask, bool verbose) {
    if (!isAvailable()) {
        return false;
    }

    // Apply mask if needed
    if (mask != MsrItem::kNoMask) {
        uint64_t old_value;
        if (!rdmsr(reg, cpu, old_value)) {
            if (verbose) {
                LogPrintf("MSR: Failed to read register 0x%08x for masking\n", reg);
            }
            return false;
        }
        value = MsrItem::maskedValue(old_value, value, mask);
        if (verbose) {
            LogPrintf("MSR: 0x%08x: 0x%016llx -> 0x%016llx\n", reg, old_value, value);
        }
    }

    return wrmsr(reg, value, cpu);
}

bool Msr::write(Callback &&callback) {
    const auto cpus = GetCPUList();

    for (int32_t cpu : cpus) {
        if (!callback(cpu)) {
            return false;
        }
    }

    return true;
}

MsrItem Msr::read(uint32_t reg, int32_t cpu, bool verbose) const {
    uint64_t value;
    if (rdmsr(reg, cpu, value)) {
        if (verbose) {
            LogPrintf("MSR: Read 0x%08x = 0x%016llx\n", reg, value);
        }
        return MsrItem(reg, value);
    }
    return MsrItem();
}

bool Msr::rdmsr(uint32_t reg, int32_t cpu, uint64_t &value) const {
#ifdef __linux__
    const int fd = msr_open(cpu, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    const bool success = pread(fd, &value, sizeof(value), reg) == sizeof(value);
    close(fd);
    return success;
#else
    return false;
#endif
}

bool Msr::wrmsr(uint32_t reg, uint64_t value, int32_t cpu) {
#ifdef __linux__
    const int fd = msr_open(cpu, O_WRONLY);
    if (fd < 0) {
        return false;
    }

    const bool success = pwrite(fd, &value, sizeof(value), reg) == sizeof(value);
    close(fd);
    return success;
#else
    return false;
#endif
}
