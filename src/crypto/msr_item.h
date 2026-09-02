// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_MSR_ITEM_H
#define BITCOIN_CRYPTO_MSR_ITEM_H

#include <cstdint>
#include <vector>
#include <limits>

/**
 * Represents a Model Specific Register (MSR) item for CPU performance tuning.
 * Based on xmrig's MSR implementation for RandomX mining optimization.
 */
class MsrItem
{
public:
    constexpr static uint64_t kNoMask = std::numeric_limits<uint64_t>::max();

    MsrItem() = default;
    MsrItem(uint32_t reg, uint64_t value, uint64_t mask = kNoMask)
        : m_reg(reg), m_value(value), m_mask(mask) {}

    bool isValid() const { return m_reg > 0; }
    uint32_t reg() const { return m_reg; }
    uint64_t value() const { return m_value; }
    uint64_t mask() const { return m_mask; }

    // Apply mask to combine old and new values
    static uint64_t maskedValue(uint64_t old_value, uint64_t new_value, uint64_t mask) {
        return (new_value & mask) | (old_value & ~mask);
    }

private:
    uint32_t m_reg = 0;
    uint64_t m_value = 0;
    uint64_t m_mask = kNoMask;
};

using MsrItems = std::vector<MsrItem>;

#endif // BITCOIN_CRYPTO_MSR_ITEM_H
