// Copyright (c) 2025 Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "randomx_wrapper.h"
#include "randomx/randomx.h"
#include "crypto/cpu_features.h"
#include "util/system.h"

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#ifdef HAVE_NUMA
#include <numa.h>
#endif

#include <mutex>
#include <memory>
#include <map>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <limits>

// Global fast mode and hugepages flags
static bool rx_fast_mode = false;
static bool rx_use_hugepages = false;
static std::atomic<bool> rx_initialized{false};
static std::atomic<bool> rx_building_dataset{false};

// Scratchpad prefetch mode (default to T0 - best for most CPUs)
static RandomX_ScratchpadPrefetchMode rx_prefetch_mode = RANDOMX_PREFETCH_T0;

// Multi-cache system to support concurrent access to multiple seeds
// This is essential for reindex and sync scenarios where background threads
// need to validate old blocks while the tip processes new blocks
struct CacheEntry {
    randomx_cache* cache;
    uint256 seedhash;
    uint64_t last_used;  // Timestamp for LRU cleanup

    CacheEntry() : cache(nullptr), last_used(0) {}
    ~CacheEntry() {
        if (cache) {
            randomx_release_cache(cache);
            cache = nullptr;
        }
    }
};

// Dataset entry for fast mode (one per seed, shared across all threads)
struct DatasetEntry {
    randomx_dataset* dataset;
    uint256 seedhash;
    uint64_t last_used;
    std::atomic<bool> initialized{false};

    DatasetEntry() : dataset(nullptr), last_used(0) {}
    ~DatasetEntry() {
        if (dataset) {
            randomx_release_dataset(dataset);
            dataset = nullptr;
        }
    }
};

// Map of seed hash -> cache entry
static std::map<uint256, std::shared_ptr<CacheEntry>> seed_caches;
static std::mutex cache_map_mutex;

// Map of seed hash -> node id -> dataset entry (for fast mode, per NUMA node)
// Using map<int, ...> allows supporting -1 (any/default) and specific node IDs
static std::map<uint256, std::map<int, std::shared_ptr<DatasetEntry>>> seed_datasets;
static std::mutex dataset_map_mutex;

// Thread-local NUMA node ID
static thread_local int rx_node_id = -1;

void RandomX_SetCurrentNode(int node) {
    rx_node_id = node;
}

static bool rx_shutting_down = false;

// Main seed for mining (set via RandomX_SetMainSeedHash)
static uint256 main_seed;
static bool main_seed_set = false;
static std::mutex main_seed_mutex;

// Track which VMs are using fast mode (per thread, per seed)
struct ThreadLocalVM {
    std::map<uint256, randomx_vm*> vms;
    std::map<uint256, bool> vm_is_fast;  // Track if VM was created in fast mode

    ~ThreadLocalVM() {
        for (auto& pair : vms) {
            if (pair.second) {
                randomx_destroy_vm(pair.second);
            }
        }
        vms.clear();
        vm_is_fast.clear();
    }
};

thread_local ThreadLocalVM rxVM_thread;

// Calculate seed height for a given block height
uint64_t RandomX_SeedHeight(uint64_t height)
{
    if (height <= RANDOMX_SEEDHASH_EPOCH_BLOCKS + RANDOMX_SEEDHASH_EPOCH_LAG) {
        return 0;
    }
    return (height - RANDOMX_SEEDHASH_EPOCH_LAG - 1) & ~(RANDOMX_SEEDHASH_EPOCH_BLOCKS - 1);
}

// Get or create a cache for a specific seed
static std::shared_ptr<CacheEntry> GetOrCreateCache(const uint256& seedhash)
{
    std::lock_guard<std::mutex> lock(cache_map_mutex);

    // Check if cache already exists
    auto it = seed_caches.find(seedhash);
    if (it != seed_caches.end()) {
        it->second->last_used = GetTime();
        return it->second;
    }

    // Create new cache
    LogPrintf("RandomX: Creating new cache for seed %s%s\n",
              seedhash.GetHex(),
              rx_use_hugepages ? " (with hugepages)" : "");

    randomx_flags flags = randomx_get_flags();
    flags |= RANDOMX_FLAG_JIT;

    if (rx_use_hugepages) {
        flags |= RANDOMX_FLAG_LARGE_PAGES;
    }

    auto entry = std::make_shared<CacheEntry>();
    entry->cache = randomx_alloc_cache(flags);
    if (!entry->cache && rx_use_hugepages) {
        // Hugepages allocation failed, try without
        LogPrintf("RandomX: WARNING - Failed to allocate cache with hugepages, retrying with normal memory\n");
        flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
        entry->cache = randomx_alloc_cache(flags);
    }

    if (!entry->cache) {
        LogPrintf("RandomX: ERROR - Failed to allocate cache\n");
        return nullptr;
    }

    randomx_init_cache(entry->cache, seedhash.begin(), 32);
    entry->seedhash = seedhash;
    entry->last_used = GetTime();

    seed_caches[seedhash] = entry;

    // Cleanup old caches (keep most recent 5, which covers +/-2 epochs around current)
    if (seed_caches.size() > 5) {
        // Find oldest cache
        auto oldest = seed_caches.begin();
        for (auto check_it = seed_caches.begin(); check_it != seed_caches.end(); ++check_it) {
            if (check_it->second->last_used < oldest->second->last_used) {
                oldest = check_it;
            }
        }
        LogPrintf("RandomX: Evicting old cache for seed %s\n", oldest->first.GetHex());
        seed_caches.erase(oldest);
    }

    return entry;
}

// Initialize dataset in parallel using multiple threads
static void InitDatasetParallel(randomx_dataset* dataset, randomx_cache* cache, int numThreads)
{
    unsigned long itemCount = randomx_dataset_item_count();

    if (numThreads <= 1) {
        // Single-threaded initialization
        randomx_init_dataset(dataset, cache, 0, itemCount);
        return;
    }

    // Multi-threaded initialization
    std::vector<std::thread> threads;
    unsigned long itemsPerThread = itemCount / numThreads;
    unsigned long remainder = itemCount % numThreads;

    for (int t = 0; t < numThreads; t++) {
        unsigned long startItem = t * itemsPerThread + std::min((unsigned long)t, remainder);
        unsigned long endItem = startItem + itemsPerThread + (t < (int)remainder ? 1 : 0);
        unsigned long count = endItem - startItem;

        threads.emplace_back([dataset, cache, startItem, count]() {
            randomx_init_dataset(dataset, cache, startItem, count);
        });
    }

    for (auto& thread : threads) {
        thread.join();
    }
}

// Get or create a dataset for a specific seed (fast mode only)
static std::shared_ptr<DatasetEntry> GetOrCreateDataset(const uint256& seedhash, std::shared_ptr<CacheEntry> cache_entry)
{
    std::lock_guard<std::mutex> lock(dataset_map_mutex);

    // Use current thread's NUMA node (or -1 for default)
    int nodeId = rx_node_id;

    // Check if dataset already exists for this node
    if (seed_datasets.count(seedhash) > 0) {
        auto& nodeMap = seed_datasets[seedhash];
        auto it = nodeMap.find(nodeId);
        if (it != nodeMap.end() && it->second->initialized) {
            it->second->last_used = GetTime();
            return it->second;
        }
    }

    // Create new dataset
    LogPrintf("RandomX: Creating new dataset for seed %s (node %d, ~30s)%s...\n",
              seedhash.GetHex(), nodeId,
              rx_use_hugepages ? " (with hugepages)" : "");

#ifdef HAVE_NUMA
    if (nodeId >= 0) {
        numa_set_preferred(nodeId);
    }
#endif

    randomx_flags flags = randomx_get_flags();
    flags |= RANDOMX_FLAG_JIT;

    if (rx_use_hugepages) {
        flags |= RANDOMX_FLAG_LARGE_PAGES;
    }

    auto entry = std::make_shared<DatasetEntry>();
    entry->dataset = randomx_alloc_dataset(flags);
    if (!entry->dataset && rx_use_hugepages) {
        // Hugepages allocation failed, try without
        LogPrintf("RandomX: WARNING - Failed to allocate dataset with hugepages, retrying with normal memory\n");
        flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
        entry->dataset = randomx_alloc_dataset(flags);
    }

#ifdef HAVE_NUMA
    if (nodeId >= 0) {
        numa_set_preferred(-1); // Reset to default
    }
#endif

    if (!entry->dataset) {
        LogPrintf("RandomX: ERROR - Failed to allocate dataset (need ~2GB RAM)\n");
        return nullptr;
    }

    // Initialize dataset from cache using multiple threads
    int numThreads = std::thread::hardware_concurrency();
    if (numThreads < 1) numThreads = 4;  // Fallback
    if (numThreads > 16) numThreads = 16;  // Cap at 16 threads

    rx_building_dataset.store(true);
    auto startTime = std::chrono::steady_clock::now();
    InitDatasetParallel(entry->dataset, cache_entry->cache, numThreads);
    auto endTime = std::chrono::steady_clock::now();
    rx_building_dataset.store(false);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();

    entry->seedhash = seedhash;
    entry->last_used = GetTime();
    entry->initialized = true;

    seed_datasets[seedhash][nodeId] = entry;

    LogPrintf("RandomX: Dataset initialized in %d ms using %d threads\n", (int)elapsed, numThreads);

    // Cleanup old datasets (keep most recent 2 seeds)
    if (seed_datasets.size() > 2) {
        auto oldest = seed_datasets.begin();
        // Find oldest seed by checking any of its datasets (they share same seed/time roughly)
        uint64_t oldest_time = std::numeric_limits<uint64_t>::max();
        
        for (auto it = seed_datasets.begin(); it != seed_datasets.end(); ++it) {
            if (!it->second.empty()) {
                // Check first dataset in this seed's map
                if (it->second.begin()->second->last_used < oldest_time) {
                    oldest_time = it->second.begin()->second->last_used;
                    oldest = it;
                }
            }
        }
        
        LogPrintf("RandomX: Evicting old datasets for seed %s\n", oldest->first.GetHex());
        seed_datasets.erase(oldest);
    }

    return entry;
}

// Check if fast mode is enabled
bool RandomX_IsFastMode()
{
    return rx_fast_mode;
}

bool RandomX_IsUsingHugepages()
{
    return rx_use_hugepages;
}

bool RandomX_IsBuildingDataset()
{
    return rx_building_dataset.load();
}

// Change mode at runtime without full shutdown/reinit
void RandomX_ChangeMode(bool fastMode, bool useHugePages)
{
    LogPrintf("RandomX: Changing mode to %s%s\n",
              fastMode ? "FAST (2GB dataset)" : "light (256MB cache)",
              useHugePages ? " with hugepages" : "");

    // Update global flags
    // Mining threads will automatically detect the mode change and recreate their VMs
    // (see RandomX_Hash_WithSeed lines 394-406 for VM mode mismatch detection)
    rx_fast_mode = fastMode;
    rx_use_hugepages = useHugePages;

    // Pre-create cache/dataset for current main seed with new settings
    uint256 seed;
    {
        std::lock_guard<std::mutex> lock(main_seed_mutex);
        if (!main_seed_set) {
            LogPrintf("RandomX: WARNING - No main seed set, skipping pre-cache\n");
            return;
        }
        seed = main_seed;
    }

    auto cache = GetOrCreateCache(seed);
    if (fastMode && cache) {
        GetOrCreateDataset(seed, cache);
    }

    LogPrintf("RandomX: Mode change complete\n");
}

// Initialize RandomX
void RandomX_Init(bool fastMode, bool useHugePages)
{
    rx_shutting_down = false;

    if (rx_initialized.exchange(true)) {
        // Already initialized - but update mode if different
        if (fastMode != rx_fast_mode) {
            LogPrintf("RandomX: Switching to %s mode\n",
                      fastMode ? "FAST (2GB dataset)" : "light (256MB cache)");
            rx_fast_mode = fastMode;
            rx_use_hugepages = useHugePages;
        }
        return;
    }

    // Detect CPU features
    CPUFeatures::Detect();

    // Check if hardware AES is available
    bool has_hw_aes = CPUFeatures::HasAES();
    LogPrintf("RandomX: Hardware AES-NI: %s\n", has_hw_aes ? "YES" : "NO (will use software AES)");

    rx_fast_mode = fastMode;
    rx_use_hugepages = useHugePages;
    LogPrintf("RandomX: Initializing %s mode%s\n",
              fastMode ? "FAST (2GB dataset)" : "light (256MB cache)",
              useHugePages ? " with hugepages" : "");

    // Set genesis seed as default main seed
    uint256 genesisSeed;
    genesisSeed.SetNull();
    *genesisSeed.begin() = 0x08;

    {
        std::lock_guard<std::mutex> lock(main_seed_mutex);
        main_seed = genesisSeed;
        main_seed_set = true;
    }

    // Pre-create genesis cache
    GetOrCreateCache(genesisSeed);

    // Note: We do NOT pre-create the dataset for the genesis seed here.
    // The miner will set the actual block seed shortly after starting, which will
    // trigger dataset allocation. Pre-allocating genesis dataset wastes 2GB of RAM/Hugepages
    // and can cause OOM when the miner tries to allocate the actual dataset.

    // Note: Scratchpad prefetch is handled internally by RandomX
    // The prefetch mode API is an xmrig-specific enhancement not in standard RandomX
    LogPrintf("RandomX: Scratchpad prefetch: Handled by RandomX library\n");

    LogPrintf("RandomX: Initialization complete\n");
}

void RandomX_Shutdown()
{
    LogPrintf("RandomX: Starting shutdown...\n");

    rx_shutting_down = true;

    // Clean up thread-local VMs
    rxVM_thread.vms.clear();
    rxVM_thread.vm_is_fast.clear();

    // Give other threads time to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Clean up all datasets
    {
        std::lock_guard<std::mutex> lock(dataset_map_mutex);
        seed_datasets.clear();
    }

    // Clean up all caches
    {
        std::lock_guard<std::mutex> lock(cache_map_mutex);
        seed_caches.clear();
    }

    rx_initialized = false;
    LogPrintf("RandomX: Shutdown complete\n");
}

// Set main seed hash (for mining - pre-caches the seed)
void RandomX_SetMainSeedHash(const void* seedhash, size_t size)
{
    if (!seedhash || size != 32) {
        LogPrintf("RandomX: ERROR - Invalid seedhash\n");
        return;
    }

    uint256 seed;
    memcpy(seed.begin(), seedhash, 32);

    // Update main seed
    {
        std::lock_guard<std::mutex> lock(main_seed_mutex);
        main_seed = seed;
        main_seed_set = true;
    }

    // Pre-cache this seed
    auto cache = GetOrCreateCache(seed);

    // In fast mode, also pre-create the dataset
    if (rx_fast_mode && cache) {
        GetOrCreateDataset(seed, cache);
    }

    LogPrintf("RandomX: Main seed set to %s\n", seed.GetHex());
}

// Get or create thread-local VM for a specific seed
static randomx_vm* GetVM(const uint256& seed)
{
    // Get or create cache for this seed (always needed, even in fast mode for dataset init)
    auto cache_entry = GetOrCreateCache(seed);
    if (!cache_entry || !cache_entry->cache) {
        LogPrintf("RandomX: ERROR - Failed to get cache for seed\n");
        return nullptr;
    }

    // Get or create dataset if in fast mode
    std::shared_ptr<DatasetEntry> dataset_entry;
    if (rx_fast_mode) {
        dataset_entry = GetOrCreateDataset(seed, cache_entry);
        // If dataset allocation fails, we'll fall back to light mode below
    }

    // Determine if we should use fast mode for this hash
    bool use_fast = rx_fast_mode && dataset_entry && dataset_entry->dataset && dataset_entry->initialized;

    // Get or create thread-local VM for this seed
    auto vm_it = rxVM_thread.vms.find(seed);
    bool need_new_vm = (vm_it == rxVM_thread.vms.end());

    // Check if existing VM mode matches what we need
    if (!need_new_vm) {
        auto fast_it = rxVM_thread.vm_is_fast.find(seed);
        bool vm_is_fast = (fast_it != rxVM_thread.vm_is_fast.end()) && fast_it->second;

        // If we want fast mode but have light VM (or vice versa), recreate
        if (use_fast != vm_is_fast) {
            randomx_destroy_vm(vm_it->second);
            rxVM_thread.vms.erase(vm_it);
            rxVM_thread.vm_is_fast.erase(seed);
            need_new_vm = true;
        }
    }

    if (need_new_vm) {
        randomx_flags flags = randomx_get_flags();
        flags |= RANDOMX_FLAG_JIT;

        if (rx_use_hugepages) {
            flags |= RANDOMX_FLAG_LARGE_PAGES;
        }

        randomx_vm* vm = nullptr;
        bool tried_hugepages = false;

        if (use_fast) {
            // Fast mode: use dataset
            flags |= RANDOMX_FLAG_FULL_MEM;
            vm = randomx_create_vm(flags, nullptr, dataset_entry->dataset);
            if (!vm && rx_use_hugepages) {
                // Try without hugepages
                tried_hugepages = true;
                LogPrintf("RandomX: WARNING - Failed to create fast VM with hugepages, trying without\n");
                flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
                vm = randomx_create_vm(flags, nullptr, dataset_entry->dataset);
            }
            if (!vm) {
                LogPrintf("RandomX: WARNING - Failed to create fast VM, trying light mode\n");
                use_fast = false;
            }
        }

        if (!vm) {
            // Light mode: use cache
            flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_FULL_MEM));
            if (rx_use_hugepages && !tried_hugepages) {
                flags |= RANDOMX_FLAG_LARGE_PAGES;
            }
            vm = randomx_create_vm(flags, cache_entry->cache, nullptr);
            if (!vm && rx_use_hugepages) {
                // Try without hugepages
                LogPrintf("RandomX: WARNING - Failed to create VM with hugepages, trying without\n");
                flags = static_cast<randomx_flags>(static_cast<int>(flags) & ~static_cast<int>(RANDOMX_FLAG_LARGE_PAGES));
                vm = randomx_create_vm(flags, cache_entry->cache, nullptr);
            }
        }

        if (!vm) {
            LogPrintf("RandomX: ERROR - Failed to create VM\n");
            return nullptr;
        }

        rxVM_thread.vms[seed] = vm;
        rxVM_thread.vm_is_fast[seed] = use_fast;
        vm_it = rxVM_thread.vms.find(seed);
    }

    return vm_it->second;
}

// Hash with a specific seed
bool RandomX_Hash_WithSeed(const void* seedhash, size_t seedhashSize,
                           const void* input, size_t inputSize, void* output)
{
    if (!seedhash || seedhashSize != 32 || !input || !output) {
        return false;
    }

    if (rx_shutting_down) {
        return false;
    }

    uint256 seed;
    memcpy(seed.begin(), seedhash, 32);

    randomx_vm* vm = GetVM(seed);
    if (!vm) return false;

    // Calculate hash
    randomx_calculate_hash(vm, input, inputSize, output);
    return true;
}

// Get thread-local VM for a specific seed
randomx_vm* RandomX_GetVM(const void* seedhash, size_t seedhashSize)
{
    if (!seedhash || seedhashSize != 32) return nullptr;
    if (rx_shutting_down) return nullptr;

    uint256 seed;
    memcpy(seed.begin(), seedhash, 32);
    return GetVM(seed);
}

bool RandomX_HashFirst(randomx_vm* vm, const void* input, size_t inputSize)
{
    if (!vm || !input) return false;
    randomx_calculate_hash_first(vm, input, inputSize);
    return true;
}

bool RandomX_HashNext(randomx_vm* vm, const void* nextInput, size_t nextInputSize, void* output)
{
    if (!vm || !nextInput || !output) return false;
    randomx_calculate_hash_next(vm, nextInput, nextInputSize, output);
    return true;
}

bool RandomX_HashLast(randomx_vm* vm, void* output)
{
    if (!vm || !output) return false;
    randomx_calculate_hash_last(vm, output);
    return true;
}

// Hash with current main seed
bool RandomX_Hash(const void* input, size_t inputSize, void* output)
{
    if (!input || !output) {
        return false;
    }

    if (rx_shutting_down) {
        return false;
    }

    // Get current main seed (and auto-initialize if needed)
    uint256 seed;
    {
        std::unique_lock<std::mutex> lock(main_seed_mutex);

        // Auto-initialize if needed (check inside lock to avoid race)
        if (!main_seed_set) {
            // Release lock before calling Init() to avoid deadlock
            lock.unlock();
            RandomX_Init(false);  // Default to light mode for auto-init
            lock.lock();
        }

        seed = main_seed;
    }

    return RandomX_Hash_WithSeed(seed.begin(), 32, input, inputSize, output);
}

bool RandomX_Hash_Block(const void* input, size_t inputSize, uint256& hash)
{
    return RandomX_Hash(input, inputSize, hash.begin());
}

bool RandomX_Verify_WithSeed(const void* seedhash, size_t seedhashSize,
                             const void* input, size_t inputSize, const uint256& expectedHash)
{
    uint256 computedHash;
    if (!RandomX_Hash_WithSeed(seedhash, seedhashSize, input, inputSize, computedHash.begin())) {
        return false;
    }

    return computedHash == expectedHash;
}

bool RandomX_Verify(const void* input, size_t inputSize, const uint256& expectedHash)
{
    uint256 computedHash;
    if (!RandomX_Hash_Block(input, inputSize, computedHash)) {
        return false;
    }

    return computedHash == expectedHash;
}

void RandomX_SetScratchpadPrefetchMode(RandomX_ScratchpadPrefetchMode mode)
{
    if (mode >= RANDOMX_PREFETCH_MAX) {
        LogPrintf("RandomX: WARNING - Invalid prefetch mode %d, using default\n", mode);
        return;
    }

    // Note: Standard RandomX library doesn't expose scratchpad prefetch mode API
    // Prefetching is handled internally by the RandomX implementation
    // This function is kept for future compatibility if we integrate xmrig's RandomX fork
    rx_prefetch_mode = mode;

    const char* mode_names[] = { "off", "t0", "nta", "mov" };
    LogPrintf("RandomX: Scratchpad prefetch mode (API not available): %s\n", mode_names[mode]);
    LogPrintf("RandomX: Note - Prefetching is handled internally by RandomX library\n");
}

RandomX_ScratchpadPrefetchMode RandomX_GetScratchpadPrefetchMode()
{
    return rx_prefetch_mode;
}
