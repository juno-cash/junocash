// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "crypto/randomx_fix.h"
#include "util/system.h"

#include <csignal>
#include <csetjmp>

#ifdef __linux__
#include <ucontext.h>
#endif

// Static members
bool RandomX_Fix::m_initialized = false;

#ifdef __linux__

// Exception context for recovery
static thread_local sigjmp_buf exception_env;
static thread_local bool exception_frame_active = false;

// Signal handler for SIGSEGV and SIGILL
static void MainLoopHandler(int sig, siginfo_t* info, void* ucontext) {
    if (!exception_frame_active) {
        // Not in a protected region, re-raise signal
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }

    LogPrint("randomx", "RandomX: Caught signal %d in main loop, recovering...\n", sig);

    // Jump back to the safe point
    siglongjmp(exception_env, 1);
}

#endif

void RandomX_Fix::SetupMainLoopExceptionFrame() {
    if (m_initialized) {
        return;
    }

#ifdef __linux__
    struct sigaction act = {};
    act.sa_sigaction = MainLoopHandler;
    act.sa_flags = SA_RESTART | SA_SIGINFO;

    if (sigaction(SIGSEGV, &act, nullptr) == 0 &&
        sigaction(SIGILL, &act, nullptr) == 0) {
        LogPrintf("RandomX: Exception handlers installed for Ryzen stability\n");
        m_initialized = true;
    } else {
        LogPrintf("RandomX: WARNING - Failed to install exception handlers\n");
    }
#else
    LogPrintf("RandomX: Exception handling not available on this platform\n");
#endif
}

void RandomX_Fix::RemoveMainLoopExceptionFrame() {
    if (!m_initialized) {
        return;
    }

#ifdef __linux__
    signal(SIGSEGV, SIG_DFL);
    signal(SIGILL, SIG_DFL);
    LogPrintf("RandomX: Exception handlers removed\n");
    m_initialized = false;
#endif
}
