// Copyright (c) 2025 The Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NUMA_HELPER_H
#define BITCOIN_NUMA_HELPER_H

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include <vector>
#include <cstdint>

#ifdef HAVE_NUMA
#include <numa.h>
#include <sched.h>
#endif

/**
 * NUMA helper for mining thread optimization.
 * Provides topology detection and thread pinning for multi-socket NUMA systems.
 * On single-socket or non-NUMA systems, all functions return safe defaults.
 */
class NumaHelper {
public:
    // Get singleton instance
    static NumaHelper& GetInstance();

    // Initialize NUMA detection (call once at startup, before spawning mining threads)
    void Initialize();

    // Check if NUMA is available and multi-node
    bool IsNUMAAvailable() const { return numa_available_; }
    int GetNumNodes() const { return num_numa_nodes_; }

    // Get CPU assignment for a thread (round-robin across nodes)
    // Returns -1 if NUMA not available
    int GetCPUForThread(int thread_id, int total_threads);

    // Get NUMA node for a thread
    int GetNodeForThread(int thread_id, int total_threads);

    // Pin current thread to specified CPU
    // Returns true on success, false on failure (logs error)
    bool PinCurrentThread(int cpu_id);

private:
    NumaHelper() : numa_available_(false), num_numa_nodes_(1), initialized_(false) {}
    NumaHelper(const NumaHelper&) = delete;
    NumaHelper& operator=(const NumaHelper&) = delete;

    bool numa_available_;
    int num_numa_nodes_;
    bool initialized_;

    // Per-node CPU list
    std::vector<std::vector<int>> node_cpus_;
};

#endif // BITCOIN_NUMA_HELPER_H
