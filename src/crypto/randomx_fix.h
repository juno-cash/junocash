// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_RANDOMX_FIX_H
#define BITCOIN_CRYPTO_RANDOMX_FIX_H

/**
 * RandomX exception handler for Ryzen CPUs.
 *
 * Some Ryzen CPUs can experience rare crashes in the RandomX JIT main loop
 * due to hardware quirks. This module sets up signal handlers to catch
 * SIGSEGV and SIGILL and recover gracefully instead of crashing the miner.
 *
 * Based on xmrig's RxFix implementation.
 */
class RandomX_Fix
{
public:
    // Setup exception frame for RandomX main loop
    // Should be called once at miner startup
    static void SetupMainLoopExceptionFrame();

    // Remove exception handlers
    // Should be called at miner shutdown
    static void RemoveMainLoopExceptionFrame();

private:
    static bool m_initialized;
};

#endif // BITCOIN_CRYPTO_RANDOMX_FIX_H
