// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "crypto/cpu_features.h"
#include "util/system.h"

#include <cstring>

// Static members
bool CPUFeatures::m_detected = false;
bool CPUFeatures::m_has_aes = false;
bool CPUFeatures::m_has_avx2 = false;
bool CPUFeatures::m_has_avx512f = false;
bool CPUFeatures::m_has_bmi2 = false;
char CPUFeatures::m_brand[64] = {0};

#ifdef __x86_64__

// CPUID function wrapper
static void cpuid(uint32_t level, uint32_t sublevel, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(level), "c"(sublevel)
    );
}

#else

// Stub for non-x86_64 platforms
static void cpuid(uint32_t, uint32_t, uint32_t* eax, uint32_t* ebx, uint32_t* ecx, uint32_t* edx) {
    *eax = *ebx = *ecx = *edx = 0;
}

#endif

void CPUFeatures::Detect() {
    if (m_detected) {
        return;
    }

    m_detected = true;

#ifdef __x86_64__
    uint32_t eax, ebx, ecx, edx;

    // Get CPU brand string
    cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_ext_level = eax;

    if (max_ext_level >= 0x80000004) {
        uint32_t* brand_ptr = reinterpret_cast<uint32_t*>(m_brand);

        cpuid(0x80000002, 0, &brand_ptr[0], &brand_ptr[1], &brand_ptr[2], &brand_ptr[3]);
        cpuid(0x80000003, 0, &brand_ptr[4], &brand_ptr[5], &brand_ptr[6], &brand_ptr[7]);
        cpuid(0x80000004, 0, &brand_ptr[8], &brand_ptr[9], &brand_ptr[10], &brand_ptr[11]);
        m_brand[sizeof(m_brand) - 1] = '\0';

        // Trim leading spaces
        char* p = m_brand;
        while (*p == ' ') p++;
        if (p != m_brand) {
            memmove(m_brand, p, strlen(p) + 1);
        }
    } else {
        strcpy(m_brand, "Unknown CPU");
    }

    // Check feature flags
    cpuid(1, 0, &eax, &ebx, &ecx, &edx);

    // ECX register (leaf 1)
    m_has_aes = (ecx & (1 << 25)) != 0;  // AES-NI

    // Check for AVX2, AVX-512, and BMI2
    cpuid(7, 0, &eax, &ebx, &ecx, &edx);

    // EBX register (leaf 7, sublevel 0)
    m_has_avx2 = (ebx & (1 << 5)) != 0;      // AVX2
    m_has_avx512f = (ebx & (1 << 16)) != 0;  // AVX-512F
    m_has_bmi2 = (ebx & (1 << 8)) != 0;      // BMI2

    LogPrintf("CPU: %s\n", m_brand);
    LogPrintf("CPU Features: AES=%d, AVX2=%d, AVX512F=%d, BMI2=%d\n",
              m_has_aes, m_has_avx2, m_has_avx512f, m_has_bmi2);

#else
    strcpy(m_brand, "Non-x86_64 CPU");
    LogPrintf("CPU: %s (feature detection not available)\n", m_brand);
#endif
}

bool CPUFeatures::HasAES() {
    if (!m_detected) Detect();
    return m_has_aes;
}

bool CPUFeatures::HasAVX2() {
    if (!m_detected) Detect();
    return m_has_avx2;
}

bool CPUFeatures::HasAVX512F() {
    if (!m_detected) Detect();
    return m_has_avx512f;
}

bool CPUFeatures::HasBMI2() {
    if (!m_detected) Detect();
    return m_has_bmi2;
}

const char* CPUFeatures::GetBrand() {
    if (!m_detected) Detect();
    return m_brand;
}
