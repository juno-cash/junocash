// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_MSR_H
#define BITCOIN_CRYPTO_MSR_H

#include "crypto/msr_item.h"
#include <functional>
#include <memory>

/**
 * MSR (Model Specific Register) interface for CPU performance tuning.
 * Provides low-level access to CPU MSR registers for RandomX optimization.
 *
 * Based on xmrig's implementation. Requires root/admin privileges.
 */
class Msr
{
public:
    using Callback = std::function<bool(int32_t cpu)>;

    Msr();
    ~Msr();

    static const char *tag();
    static std::shared_ptr<Msr> get();

    bool isAvailable() const;

    // Write MSR item to specific CPU (or all CPUs if cpu == -1)
    bool write(const MsrItem &item, int32_t cpu = -1, bool verbose = true);
    bool write(uint32_t reg, uint64_t value, int32_t cpu = -1, uint64_t mask = MsrItem::kNoMask, bool verbose = true);

    // Execute callback for each CPU
    bool write(Callback &&callback);

    // Read MSR register
    MsrItem read(uint32_t reg, int32_t cpu = -1, bool verbose = true) const;

private:
    bool rdmsr(uint32_t reg, int32_t cpu, uint64_t &value) const;
    bool wrmsr(uint32_t reg, uint64_t value, int32_t cpu);

    class MsrPrivate;
    MsrPrivate *d_ptr = nullptr;
};

#endif // BITCOIN_CRYPTO_MSR_H
