// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_CPU_FEATURES_H
#define BITCOIN_CRYPTO_CPU_FEATURES_H

#include <cstdint>

/**
 * CPU feature detection for cryptographic optimizations.
 */
class CPUFeatures
{
public:
    // Detect CPU features once at startup
    static void Detect();

    // Check if AES-NI is supported
    static bool HasAES();

    // Check if AVX2 is supported
    static bool HasAVX2();

    // Check if AVX-512F is supported
    static bool HasAVX512F();

    // Check if BMI2 (Bit Manipulation Instruction Set 2) is supported
    static bool HasBMI2();

    // Get CPU brand string
    static const char* GetBrand();

private:
    static bool m_detected;
    static bool m_has_aes;
    static bool m_has_avx2;
    static bool m_has_avx512f;
    static bool m_has_bmi2;
    static char m_brand[64];
};

#endif // BITCOIN_CRYPTO_CPU_FEATURES_H
