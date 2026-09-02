// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_RANDOMX_MSR_H
#define BITCOIN_CRYPTO_RANDOMX_MSR_H

#include <vector>

/**
 * RandomX MSR optimization module.
 * Applies CPU-specific MSR tweaks for improved RandomX mining performance.
 *
 * Key optimizations:
 * - Cache QoS (Quality of Service) allocation for mining threads
 * - CPU-specific performance tuning for AMD Ryzen and Intel CPUs
 * - Can provide 10-15% hashrate improvement
 *
 * Requires root/admin privileges to modify MSR registers.
 */
class RandomX_Msr
{
public:
    // Initialize MSR optimizations
    // thread_affinities: List of CPU core IDs where mining threads run
    // enable_cache_qos: Enable L3 cache allocation (requires proper thread affinity)
    static bool Init(const std::vector<int>& thread_affinities, bool enable_cache_qos = true);

    // Restore original MSR values (call at shutdown)
    static void Destroy();

    // Check if MSR optimizations are enabled
    static bool IsEnabled();

private:
    static bool m_enabled;
    static bool m_initialized;
    static bool m_cache_qos;
};

#endif // BITCOIN_CRYPTO_RANDOMX_MSR_H
