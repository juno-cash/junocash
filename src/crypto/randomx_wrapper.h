// Copyright (c) 2025 Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_RANDOMX_WRAPPER_H
#define BITCOIN_CRYPTO_RANDOMX_WRAPPER_H

#include "uint256.h"
#include <vector>
#include <cstddef>

// Forward declaration
struct randomx_vm;

/**
 * RandomX wrapper for Juno Cash
 *
 * RandomX is a proof-of-work algorithm optimized for general-purpose CPUs.
 * This wrapper provides a simple interface for using RandomX in Juno Cash.
 */

// RandomX epoch configuration
static const uint64_t RANDOMX_SEEDHASH_EPOCH_BLOCKS = 2048;  // Power of 2 for efficient bitmask operations
static const uint64_t RANDOMX_SEEDHASH_EPOCH_LAG = 96;

/**
 * Calculate the seed height for a given block height.
 * The seed changes every 2048 blocks with a 96-block lag.
 * First epoch transition occurs at block 2144 (2048 + 96).
 *
 * @param height Block height
 * @return Seed height to use for RandomX key
 */
uint64_t RandomX_SeedHeight(uint64_t height);

/**
 * Set the main seed hash for RandomX.
 * This should be called when mining or when the epoch changes.
 *
 * @param seedhash Pointer to seed hash (32 bytes, typically a block hash)
 * @param size Size of seed hash
 */
void RandomX_SetMainSeedHash(const void* seedhash, size_t size);

/**
 * Get thread-local VM for a specific seed.
 * This allows avoiding repeated lookups/locks in tight loops.
 *
 * @param seedhash Pointer to seed hash (32 bytes)
 * @param seedhashSize Size of seed hash
 * @return Pointer to initialized VM, or nullptr on error
 */
randomx_vm* RandomX_GetVM(const void* seedhash, size_t seedhashSize);

/**
 * Calculate a RandomX hash with a specific seed.
 *
 * @param seedhash Pointer to seed hash (32 bytes)
 * @param seedhashSize Size of seed hash
 * @param input Pointer to input data
 * @param inputSize Size of input data in bytes
 * @param output Pointer to output buffer (must be at least 32 bytes)
 * @return true if successful, false otherwise
 */
bool RandomX_Hash_WithSeed(const void* seedhash, size_t seedhashSize,
                           const void* input, size_t inputSize, void* output);

/**
 * Calculate a RandomX hash (uses current main seed).
 *
 * @param input Pointer to input data
 * @param inputSize Size of input data in bytes
 * @param output Pointer to output buffer (must be at least 32 bytes)
 * @return true if successful, false otherwise
 */
bool RandomX_Hash(const void* input, size_t inputSize, void* output);

/**
 * Calculate a RandomX hash from a block header (uses current main seed).
 * This is optimized for mining use case.
 *
 * @param input Pointer to input data
 * @param inputSize Size of input data in bytes
 * @param hash Output hash (32 bytes)
 * @return true if successful, false otherwise
 */
bool RandomX_Hash_Block(const void* input, size_t inputSize, uint256& hash);

/**
 * Verify a RandomX proof of work with a specific seed.
 *
 * @param seedhash Pointer to seed hash (32 bytes)
 * @param seedhashSize Size of seed hash
 * @param input Pointer to input data
 * @param inputSize Size of input data in bytes
 * @param expectedHash The expected hash value
 * @return true if the hash matches, false otherwise
 */
// Batch hashing API for pipelining (VM-direct version)
bool RandomX_HashFirst(randomx_vm* vm, const void* input, size_t inputSize);
bool RandomX_HashNext(randomx_vm* vm, const void* nextInput, size_t nextInputSize, void* output);
bool RandomX_HashLast(randomx_vm* vm, void* output);

bool RandomX_Verify_WithSeed(const void* seedhash, size_t seedhashSize,
                             const void* input, size_t inputSize, const uint256& expectedHash);

/**
 * Verify a RandomX proof of work (uses current main seed).
 *
 * @param input Pointer to input data
 * @param inputSize Size of input data in bytes
 * @param expectedHash The expected hash value
 * @return true if the hash matches, false otherwise
 */
bool RandomX_Verify(const void* input, size_t inputSize, const uint256& expectedHash);

/**
 * Initialize RandomX (call once at startup).
 * This prepares the RandomX cache and VM.
 *
 * @param fastMode If true, use full 2GB dataset for ~2x faster mining.
 *                 If false (default), use 256MB cache (light mode).
 * @param useHugePages If true, allocate memory using large pages (1GB/2MB hugepages)
 *                     for 5-10% additional performance. Requires system hugepages configured.
 *                     Falls back to normal memory if allocation fails.
 */
// Set the NUMA node for the current thread (optimization)
void RandomX_SetCurrentNode(int node);

void RandomX_Init(bool fastMode = false, bool useHugePages = false);

/**
 * Check if RandomX is running in fast mode.
 * @return true if using full dataset, false if using light mode.
 */
bool RandomX_IsFastMode();

/**
 * Check if RandomX is configured to use hugepages.
 * @return true if hugepages are enabled for RandomX, false otherwise.
 */
bool RandomX_IsUsingHugepages();

/**
 * Check if RandomX is currently building a dataset.
 * @return true if dataset initialization is in progress, false otherwise.
 */
bool RandomX_IsBuildingDataset();

/**
 * Change RandomX mode at runtime without full shutdown/reinit.
 * This is safe to call while mining - threads will automatically
 * recreate their VMs with the new mode settings.
 *
 * @param fastMode If true, use full 2GB dataset. If false, use 256MB cache.
 * @param useHugePages If true, use hugepages for memory allocation.
 */
void RandomX_ChangeMode(bool fastMode, bool useHugePages);

/**
 * Cleanup RandomX (call at shutdown).
 */
void RandomX_Shutdown();

/**
 * Scratchpad prefetch modes for performance tuning.
 */
enum RandomX_ScratchpadPrefetchMode {
    RANDOMX_PREFETCH_OFF = 0,  // No prefetch
    RANDOMX_PREFETCH_T0,       // prefetcht0 - Fetch to L1/L2/L3 cache (default, best for most CPUs)
    RANDOMX_PREFETCH_NTA,      // prefetchnta - Fetch to L3 cache only (non-temporal)
    RANDOMX_PREFETCH_MOV,      // Use MOV instruction for prefetch
    RANDOMX_PREFETCH_MAX
};

/**
 * Set scratchpad prefetch mode for RandomX.
 * Should be called before mining starts or after RandomX_Init().
 * Different modes work better on different CPUs.
 *
 * @param mode Prefetch mode to use (default: RANDOMX_PREFETCH_T0)
 */
void RandomX_SetScratchpadPrefetchMode(RandomX_ScratchpadPrefetchMode mode);

/**
 * Get current scratchpad prefetch mode.
 * @return Current prefetch mode
 */
RandomX_ScratchpadPrefetchMode RandomX_GetScratchpadPrefetchMode();

#endif // BITCOIN_CRYPTO_RANDOMX_WRAPPER_H
