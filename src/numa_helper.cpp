// Copyright (c) 2025 The Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#include "numa_helper.h"
#include "util/system.h"

#ifdef HAVE_NUMA
#include <pthread.h>
#include <cstring>
#endif

NumaHelper& NumaHelper::GetInstance() {
    static NumaHelper instance;
    return instance;
}

void NumaHelper::Initialize() {
    if (initialized_) return;
    initialized_ = true;

#ifdef HAVE_NUMA
    if (numa_available() == -1) {
        LogPrintf("NUMA: Not available on this system\n");
        numa_available_ = false;
        num_numa_nodes_ = 1;
        return;
    }

    num_numa_nodes_ = numa_num_configured_nodes();
    if (num_numa_nodes_ <= 1) {
        LogPrintf("NUMA: Single node system, no optimization needed\n");
        numa_available_ = false;
        num_numa_nodes_ = 1;
        return;
    }

    numa_available_ = true;
    node_cpus_.resize(num_numa_nodes_);

    int total_cpus = numa_num_configured_cpus();
    for (int node = 0; node < num_numa_nodes_; node++) {
        struct bitmask* cpumask = numa_allocate_cpumask();
        if (numa_node_to_cpus(node, cpumask) == 0) {
            for (int cpu = 0; cpu < total_cpus; cpu++) {
                if (numa_bitmask_isbitset(cpumask, cpu)) {
                    node_cpus_[node].push_back(cpu);
                }
            }
        }
        numa_free_cpumask(cpumask);
        LogPrintf("NUMA: Node %d has %u CPUs\n", node, node_cpus_[node].size());
    }

    LogPrintf("NUMA: Detected %d nodes, thread affinity optimization enabled\n", num_numa_nodes_);
#else
    numa_available_ = false;
    num_numa_nodes_ = 1;
    LogPrint("numa", "NUMA: Support not compiled in\n");
#endif
}

int NumaHelper::GetNodeForThread(int thread_id, int total_threads) {
    if (!numa_available_ || num_numa_nodes_ <= 1) {
        return 0;
    }
    // Round-robin distribution across nodes
    return thread_id % num_numa_nodes_;
}

int NumaHelper::GetCPUForThread(int thread_id, int total_threads) {
    if (!numa_available_ || num_numa_nodes_ <= 1) {
        return -1; // No pinning on single-node systems
    }

    int node = GetNodeForThread(thread_id, total_threads);
    if (node < 0 || node >= (int)node_cpus_.size() || node_cpus_[node].empty()) {
        return -1;
    }

    // Count how many threads are on this node before this one
    int threads_on_node = 0;
    for (int t = 0; t < thread_id; t++) {
        if (GetNodeForThread(t, total_threads) == node) {
            threads_on_node++;
        }
    }

    // Assign CPU within node using round-robin
    size_t cpu_index = threads_on_node % node_cpus_[node].size();
    return node_cpus_[node][cpu_index];
}

bool NumaHelper::PinCurrentThread(int cpu_id) {
#ifdef HAVE_NUMA
    if (cpu_id < 0) return false;

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);

    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
        LogPrintf("NUMA: Failed to pin thread to CPU %d: %s\n", cpu_id, strerror(rc));
        return false;
    }
    return true;
#else
    return false;
#endif
}
