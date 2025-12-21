// Copyright (c) 2016-2023 The Zcash developers
// Copyright (c) 2025 Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "metrics.h"

#include "chainparams.h"
#include "init.h"
#include "checkpoints.h"
#include "main.h"
#include "miner.h"
#include "rpc/server.h"
#include "timedata.h"
#include "ui_interface.h"
#include "util/system.h"
#include "util/time.h"
#include "util/moneystr.h"
#include "util/strencodings.h"
#include "wallet/wallet.h"
#include "key_io.h"
#include "crypto/randomx_wrapper.h"
#include "hw/dmi/DmiReader.h"

#include <boost/range/irange.hpp>
#include <boost/thread.hpp>
#include <boost/thread/synchronized_value.hpp>

#include <optional>
#include <string>
#include <iostream>
#include <limits>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <cstdio>
#include <vector>
#include <set>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#ifdef WIN32
#include <io.h>
#include <wincon.h>
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#endif
#include <unistd.h>
#if defined(_M_X64) || defined(__x86_64__)
    #if defined(_MSC_VER)
        #include <intrin.h>
    #else
        #include <cpuid.h>
    #endif
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

// Box-drawing characters (UTF-8)
static const char* BOX_HORIZONTAL = "\xe2\x94\x80";      // ─ (U+2500)
static const char* BOX_VERTICAL = "\xe2\x94\x82";        // │ (U+2502)
static const char* BOX_TOP_LEFT = "\xe2\x94\x8c";        // ┌ (U+250C)
static const char* BOX_TOP_RIGHT = "\xe2\x94\x90";       // ┐ (U+2510)
static const char* BOX_BOTTOM_LEFT = "\xe2\x94\x94";     // └ (U+2514)
static const char* BOX_BOTTOM_RIGHT = "\xe2\x94\x98";    // ┘ (U+2518)
static const char* BOX_VERTICAL_RIGHT = "\xe2\x94\x9c";  // ├ (U+251C)
static const char* BOX_VERTICAL_LEFT = "\xe2\x94\xa4";   // ┤ (U+2524)
static const char* BOX_PROGRESS_FILLED = "\xe2\x96\x88"; // █ (U+2588)
static const char* BOX_PROGRESS_EMPTY = "\xe2\x96\x91";  // ░ (U+2591)
static const char* SYMBOL_CHECK = "\xe2\x9c\x93";        // ✓ (U+2713)
static const char* SYMBOL_CROSS = "\xe2\x9c\x97";        // ✗ (U+2717)

// Async mining stop - flag for non-blocking stop
static std::atomic<bool> miningStopInProgress{false};

// Shielding in progress - prevents repeated keypresses
static std::atomic<bool> shieldingInProgress{false};

// Track if there are shieldable coins (mature transparent balance > 0)
static std::atomic<bool> hasShieldableCoins{false};

void AtomicTimer::start()
{
    std::unique_lock<std::mutex> lock(mtx);
    if (threads < 1) {
        start_time = GetTime();
    }
    ++threads;
}

void AtomicTimer::stop()
{
    std::unique_lock<std::mutex> lock(mtx);
    // Ignore excess calls to stop()
    if (threads > 0) {
        --threads;
        if (threads < 1) {
            int64_t time_span = GetTime() - start_time;
            total_time += time_span;
        }
    }
}


void AtomicTimer::zeroize()
{
    std::unique_lock<std::mutex> lock(mtx);
    // only zeroize it if there's no more threads (same semantics as start())
    if (threads < 1) {
        start_time = 0;
        total_time = 0;
    }
}

bool AtomicTimer::running()
{
    std::unique_lock<std::mutex> lock(mtx);
    return threads > 0;
}

uint64_t AtomicTimer::threadCount()
{
    std::unique_lock<std::mutex> lock(mtx);
    return threads;
}

double AtomicTimer::rate(const AtomicCounter& count)
{
    std::unique_lock<std::mutex> lock(mtx);
    int64_t duration = total_time;
    if (threads > 0) {
        // Timer is running, so get the latest count
        duration += GetTime() - start_time;
    }
    return duration > 0 ? (double)count.get() / duration : 0;
}

static CCriticalSection cs_metrics;

static boost::synchronized_value<int64_t> nNodeStartTime;
static boost::synchronized_value<int64_t> nNextRefresh;
AtomicCounter transactionsValidated;
AtomicCounter ehSolverRuns;
AtomicCounter solutionTargetChecks;
static AtomicCounter minedBlocks;
AtomicTimer miningTimer;
std::atomic<size_t> nSizeReindexed(0);   // valid only during reindex
std::atomic<size_t> nFullSizeToReindex(1);   // valid only during reindex

static boost::synchronized_value<std::list<uint256>> trackedBlocks;

static boost::synchronized_value<std::list<std::string>> messageBox;
static boost::synchronized_value<std::string> initMessage;
static bool loaded = false;

// Benchmarking mode globals
std::atomic<bool> benchmarkMode(false);
std::atomic<int> benchmarkCurrentThreads(1);
std::atomic<int> benchmarkMaxThreads(0);
std::atomic<bool> benchmarkTestLight(true);
std::atomic<bool> benchmarkTestFast(true);
std::atomic<bool> benchmarkTestHugepages(true);
static std::atomic<int64_t> benchmarkStartTime(0);
static std::atomic<int> benchmarkStartHeight(0);  // Track starting block height for benchmark duration logging
static std::atomic<double> benchmarkAccumulatedHashrate(0.0);
static std::atomic<int> benchmarkSampleCount(0);
static std::atomic<bool> benchmarkWarmingUp(false);  // Track if currently in warmup phase
static const int BENCHMARK_DURATION_SECONDS = 20; // Mine for 20 seconds per thread count
static boost::thread* benchmarkThread = nullptr;
static std::atomic<bool> benchmarkAutoApply(false); // Auto-apply optimal threads to config

// Regular mining warmup tracking
static std::atomic<int64_t> miningStartTime(0);  // Track when mining started for warmup detection
static const int MINING_WARMUP_SECONDS = 10;  // Show warmup status for first 10 seconds

// Mining probability and luck tracking
static std::atomic<int64_t> lastBlockFoundTime(0);  // Timestamp when last block was found
static std::atomic<int> lastBlockFoundHeight(0);    // Height of last block we found
static std::atomic<double> lastBlockLuckPercent(0.0);  // Luck % for last found block

// Historical difficulty tracking for meter visualization
static std::atomic<double> difficultyHistoricalHigh(0.0);
static std::atomic<double> difficultyHistoricalLow(0.0);
static std::atomic<bool> difficultyHistoryInitialized(false);

// External function declarations
extern int64_t GetNetworkHashPS(int lookup, int height);

// DMI/SMBIOS hardware information reader
static xmrig::DmiReader* g_dmiReader = nullptr;
static std::once_flag g_dmiReaderInitFlag;

// Initialize DMI reader (called once)
static void InitDmiReader() {
    std::call_once(g_dmiReaderInitFlag, []() {
        LogPrintf("DMI/SMBIOS: Initializing reader...\n");
        g_dmiReader = new xmrig::DmiReader();
        if (!g_dmiReader->read()) {
            // Failed to read DMI, but don't delete - keep for API
            LogPrintf("DMI/SMBIOS: Failed to read hardware information\n");
        } else {
            LogPrintf("DMI/SMBIOS: Successfully read hardware information\n");
            LogPrintf("DMI/SMBIOS: Found %d memory modules\n", g_dmiReader->memory().size());

            // Log each memory module
            for (const auto& dimm : g_dmiReader->memory()) {
                if (dimm.isValid()) {
                    LogPrintf("DMI/SMBIOS: Memory module - slot=%s, size=%llu bytes, valid=%d\n",
                        dimm.slot().data(), dimm.size(), dimm.isValid() ? 1 : 0);
                }
            }
        }
    });
}

// Get motherboard model from DMI/SMBIOS
static std::string GetMotherboardModel() {
    InitDmiReader();

    if (g_dmiReader && g_dmiReader->board().isValid()) {
        const auto& board = g_dmiReader->board();
        std::string vendor = board.vendor().data();
        std::string product = board.product().data();

        if (!vendor.empty() && !product.empty()) {
            return vendor + " " + product;
        } else if (!product.empty()) {
            return product;
        } else if (!vendor.empty()) {
            return vendor;
        }
    }

    return "Unknown";
}

// Forward declaration
static std::string GetMemoryInfo();

// Get detailed memory information with DIMMs
static std::string GetDetailedMemoryInfo() {
    InitDmiReader();

    if (g_dmiReader && !g_dmiReader->memory().empty()) {
        const auto& memory = g_dmiReader->memory();
        std::stringstream ss;
        uint64_t totalMemory = 0;
        std::map<char, int> channelDimmCount;
        std::vector<std::string> dimmDetails;

        // Collect info from all populated DIMMs
        for (const auto& dimm : memory) {
            if (dimm.isValid() && dimm.size() > 0) {
                totalMemory += dimm.size();

                // Extract channel
                std::string slotId = dimm.id().data();
                size_t underscorePos = slotId.find('_');
                if (underscorePos != std::string::npos && underscorePos + 1 < slotId.length()) {
                    char channelChar = slotId[underscorePos + 1];
                    if (channelChar >= 'A' && channelChar <= 'Z') {
                        channelDimmCount[channelChar]++;
                    }
                }

                // Format DIMM details
                std::string dimmInfo = strprintf("  %s: %d GB %s @ %llu MHz",
                    dimm.id().data(),
                    (dimm.size() + 512*1024*1024) / (1024*1024*1024),
                    dimm.type(),
                    dimm.speed() / 1000000);

                if (dimm.product().isValid() && !dimm.product().isEmpty()) {
                    dimmInfo += std::string(" ") + dimm.product().data();
                }
                dimmDetails.push_back(dimmInfo);
            }
        }

        if (totalMemory > 0) {
            // Total with channel info and DPC
            ss << (totalMemory + 512*1024*1024) / (1024*1024*1024) << " GB";

            if (!channelDimmCount.empty()) {
                int numChannels = channelDimmCount.size();

                // Calculate DPC (DIMMs per channel)
                std::map<int, int> dpcFrequency;
                for (const auto& pair : channelDimmCount) {
                    dpcFrequency[pair.second]++;
                }

                int mostCommonDPC = 0;
                int maxFrequency = 0;
                for (const auto& pair : dpcFrequency) {
                    if (pair.second > maxFrequency) {
                        maxFrequency = pair.second;
                        mostCommonDPC = pair.first;
                    }
                }

                // Format channel description
                std::string channelDesc;
                if (numChannels == 1) channelDesc = "Single Channel";
                else if (numChannels == 2) channelDesc = "Dual Channel";
                else if (numChannels == 3) channelDesc = "Triple Channel";
                else if (numChannels == 4) channelDesc = "Quad Channel";
                else if (numChannels == 6) channelDesc = "Hexa Channel";
                else if (numChannels == 8) channelDesc = "Octa Channel";
                else if (numChannels == 12) channelDesc = "Dodeca Channel";
                else {
                    std::stringstream cs;
                    cs << numChannels << "-Channel";
                    channelDesc = cs.str();
                }

                // Add channel and DPC info
                if (mostCommonDPC > 0) {
                    ss << " (" << channelDesc << ", " << mostCommonDPC << "DPC)";
                } else {
                    ss << " (" << channelDesc << ")";
                }
            }

            // Add DIMM details
            if (!dimmDetails.empty()) {
                ss << "\n";
                for (const auto& detail : dimmDetails) {
                    ss << detail << "\n";
                }
            }

            return ss.str();
        }
    }

    // Fallback to simple info
    return GetMemoryInfo();
}

// Get memory information (DIMMs, speed, channels)
static std::string GetMemoryInfo() {
#if defined(__linux__)
    // Read total memory from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    uint64_t totalMemKB = 0;
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("MemTotal:") == 0) {
                std::istringstream iss(line.substr(9));
                iss >> totalMemKB;
                break;
            }
        }
    }

    if (totalMemKB > 0) {
        uint64_t totalGB = (totalMemKB + 524288) / 1048576;  // Round to nearest GB
        return strprintf("%d GB", totalGB);
    }
#elif defined(__APPLE__)
    // On macOS, use sysctl to get memory size
    int64_t memsize = 0;
    size_t len = sizeof(memsize);
    if (sysctlbyname("hw.memsize", &memsize, &len, nullptr, 0) == 0) {
        int totalGB = memsize / (1024 * 1024 * 1024);
        return strprintf("%d GB", totalGB);
    }
#elif defined(_WIN32)
    // On Windows, use GlobalMemoryStatusEx
    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        DWORDLONG totalMem = memInfo.ullTotalPhys;
        int totalGB = totalMem / (1024 * 1024 * 1024);
        return strprintf("%d GB", totalGB);
    }
#endif
    return "Unknown";
}

// Get CPU model name using CPUID
static std::string GetCPUModel() {
#if defined(_M_X64) || defined(__x86_64__)
    #if defined(_MSC_VER)
        int CPUInfo[4] = {-1};
        char CPUBrandString[0x40];
        __cpuid(CPUInfo, 0x80000000);
        unsigned int nExIds = CPUInfo[0];

        memset(CPUBrandString, 0, sizeof(CPUBrandString));

        if (nExIds >= 0x80000004) {
            __cpuid((int*)(CPUBrandString), 0x80000002);
            __cpuid((int*)(CPUBrandString + 16), 0x80000003);
            __cpuid((int*)(CPUBrandString + 32), 0x80000004);
        }

        return std::string(CPUBrandString);
    #elif defined(__MINGW32__) || defined(__MINGW64__)
        // MinGW uses Windows-style __cpuid(int[4], int)
        int CPUInfo[4] = {-1};
        char CPUBrandString[0x40];
        __cpuid(CPUInfo, 0x80000000);
        unsigned int nExIds = CPUInfo[0];

        memset(CPUBrandString, 0, sizeof(CPUBrandString));

        if (nExIds >= 0x80000004) {
            __cpuid((int*)(CPUBrandString), 0x80000002);
            __cpuid((int*)(CPUBrandString + 16), 0x80000003);
            __cpuid((int*)(CPUBrandString + 32), 0x80000004);
        }

        return std::string(CPUBrandString);
    #else
        char brand[0x40];
        unsigned int brand_data[12];

        // Get extended CPUID info
        if (__get_cpuid_max(0x80000000, nullptr) >= 0x80000004) {
            __cpuid(0x80000002, brand_data[0], brand_data[1], brand_data[2], brand_data[3]);
            __cpuid(0x80000003, brand_data[4], brand_data[5], brand_data[6], brand_data[7]);
            __cpuid(0x80000004, brand_data[8], brand_data[9], brand_data[10], brand_data[11]);

            memcpy(brand, brand_data, sizeof(brand_data));
            brand[sizeof(brand_data)] = '\0';

            // Trim leading spaces
            std::string result(brand);
            size_t start = result.find_first_not_of(" ");
            if (start != std::string::npos) {
                return result.substr(start);
            }
            return result;
        }
    #endif
#elif defined(__aarch64__) || defined(__arm__)
    // On ARM, try to read from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.find("model name") != std::string::npos ||
                line.find("Processor") != std::string::npos) {
                size_t pos = line.find(":");
                if (pos != std::string::npos) {
                    std::string model = line.substr(pos + 1);
                    // Trim leading spaces
                    size_t start = model.find_first_not_of(" \t");
                    if (start != std::string::npos) {
                        return model.substr(start);
                    }
                    return model;
                }
            }
        }
    }
#elif defined(__APPLE__)
    // On macOS, use sysctl to get CPU brand string
    char brand[256];
    size_t len = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &len, nullptr, 0) == 0) {
        std::string result(brand);
        // Trim leading/trailing spaces
        size_t start = result.find_first_not_of(" \t");
        size_t end = result.find_last_not_of(" \t\n\r");
        if (start != std::string::npos && end != std::string::npos) {
            return result.substr(start, end - start + 1);
        }
        return result;
    }
#endif
    return "Unknown CPU";
}

// Load difficulty history from file, or calculate from blockchain if file doesn't exist
static void loadDifficultyHistory()
{
    boost::filesystem::path historyFile = GetDataDir() / "difficulty_history.dat";

    // Try to load from file first
    if (boost::filesystem::exists(historyFile)) {
        std::ifstream file(historyFile.string());
        if (file.is_open()) {
            double high, low;
            if (file >> high >> low) {
                difficultyHistoricalHigh = high;
                difficultyHistoricalLow = low;
                difficultyHistoryInitialized = true;
                LogPrintf("Loaded difficulty history: high=%.6f, low=%.6f\n", high, low);
                file.close();
                return;
            }
            file.close();
        }
    }

    // File doesn't exist or is invalid, calculate from blockchain
    // Use last ~2 months of blocks (assuming ~1 min block time: 60*24*60 = 86400 blocks)
    const int BLOCKS_TO_SCAN = 86400;

    LOCK(cs_main);
    if (!chainActive.Tip()) {
        LogPrintf("Cannot initialize difficulty history: no chain tip\n");
        return;
    }

    CBlockIndex* pindex = chainActive.Tip();
    double high = 0.0;
    double low = std::numeric_limits<double>::max();
    int scanned = 0;

    // Scan backwards through the chain
    while (pindex && scanned < BLOCKS_TO_SCAN) {
        double diff = GetDifficulty(pindex);
        if (diff > high) high = diff;
        if (diff < low) low = diff;
        pindex = pindex->pprev;
        scanned++;
    }

    if (scanned > 0) {
        difficultyHistoricalHigh = high;
        difficultyHistoricalLow = low;
        difficultyHistoryInitialized = true;
        LogPrintf("Initialized difficulty history from %d blocks: high=%.6f, low=%.6f\n",
                  scanned, high, low);

        // Save to file for future use
        std::ofstream outFile(historyFile.string());
        if (outFile.is_open()) {
            outFile << std::fixed << std::setprecision(6) << high << " " << low;
            outFile.close();
        }
    }
}

// Save difficulty history to file when updated
static void saveDifficultyHistory()
{
    boost::filesystem::path historyFile = GetDataDir() / "difficulty_history.dat";
    std::ofstream file(historyFile.string());
    if (file.is_open()) {
        double high = difficultyHistoricalHigh.load();
        double low = difficultyHistoricalLow.load();
        file << std::fixed << std::setprecision(6) << high << " " << low;
        file.close();
    }
}

// Calculate expected time to find a block (in seconds) based on solo hashrate
static double calculateExpectedBlockTime(double soloHashrate)
{
    if (soloHashrate <= 0) return 0;

    // Get network hashrate (average over last 120 blocks)
    int64_t networkHashrate = GetNetworkHashPS(120, -1);
    if (networkHashrate <= 0) return 0;

    // Expected time = (Network Hashrate / Solo Hashrate) × Target Block Time
    // Use POST_BLOSSOM_POW_TARGET_SPACING (60 seconds after Blossom activation)
    const Consensus::Params& params = Params().GetConsensus();
    double targetBlockTime = static_cast<double>(params.nPostBlossomPowTargetSpacing);

    double expectedTime = (static_cast<double>(networkHashrate) / soloHashrate) * targetBlockTime;
    return expectedTime;
}

// Record when a block is found for luck calculation
void RecordBlockFound(int64_t timeMining, double difficulty, double hashrate)
{
    lastBlockFoundTime = GetTime();
    lastBlockFoundHeight = chainActive.Height();

    // Calculate luck percentage
    // Luck = expected time / actual time * 100
    if (timeMining > 0 && hashrate > 0) {
        double expectedTime = calculateExpectedBlockTime(hashrate);
        double luckPercent = (expectedTime / timeMining) * 100.0;
        lastBlockLuckPercent = luckPercent;
        LogPrintf("Block found! Luck: %.1f%% (expected %s, actual %s)\n",
                  luckPercent,
                  DisplayDuration(expectedTime, DurationFormat::REDUCED).c_str(),
                  DisplayDuration(timeMining, DurationFormat::REDUCED).c_str());
    }
}

// Set mining start time (called when mining begins)
void SetMiningStartTime()
{
    miningStartTime = GetTime();
}

// Store benchmark results
struct BenchmarkResult {
    int threads;
    std::string mode;  // "Light", "Fast", or "Fast+Hugepages"
    double hashrate;
    int samples;
};
static std::vector<BenchmarkResult> benchmarkResults;

void TrackMinedBlock(uint256 hash)
{
    LOCK(cs_metrics);
    minedBlocks.increment();
    trackedBlocks->push_back(hash);
}

void MarkStartTime()
{
    *nNodeStartTime = GetTime();
}

int64_t GetUptime()
{
    return GetTime() - *nNodeStartTime;
}

// Check if hugepages are enabled and available
static bool IsHugepagesEnabled()
{
#ifndef WIN32
    // Check 1GB hugepages first
    std::ifstream hugepages_1g("/sys/kernel/mm/hugepages/hugepages-1048576kB/free_hugepages");
    if (hugepages_1g.is_open()) {
        int free_1g = 0;
        hugepages_1g >> free_1g;
        if (free_1g > 0) {
            return true;
        }
    }

    // Check 2MB hugepages
    std::ifstream hugepages_2m("/sys/kernel/mm/hugepages/hugepages-2048kB/free_hugepages");
    if (hugepages_2m.is_open()) {
        int free_2m = 0;
        hugepages_2m >> free_2m;
        if (free_2m > 0) {
            return true;
        }
    }
#endif
    return false;
}

double GetLocalSolPS()
{
    return miningTimer.rate(solutionTargetChecks);
}

std::string WhichNetwork()
{
    if (GetBoolArg("-regtest", false))
        return "regtest";
    if (GetBoolArg("-testnet", false))
        return "testnet";
    return "mainnet";
}

int EstimateNetHeight(const Consensus::Params& params, int currentHeadersHeight, int64_t currentHeadersTime)
{
    int64_t now = GetTime();
    if (currentHeadersTime >= now) {
        return currentHeadersHeight;
    }

    int estimatedHeight = currentHeadersHeight + (now - currentHeadersTime) / params.PoWTargetSpacing(currentHeadersHeight);

    int blossomActivationHeight = params.vUpgrades[Consensus::UPGRADE_NU6_1].nActivationHeight;
    if (currentHeadersHeight >= blossomActivationHeight || estimatedHeight <= blossomActivationHeight) {
        return ((estimatedHeight + 5) / 10) * 10;
    }

    int numPreBlossomBlocks = blossomActivationHeight - currentHeadersHeight;
    int64_t preBlossomTime = numPreBlossomBlocks * params.PoWTargetSpacing(blossomActivationHeight - 1);
    int64_t blossomActivationTime = currentHeadersTime + preBlossomTime;
    if (blossomActivationTime >= now) {
        return blossomActivationHeight;
    }

    int netheight =  blossomActivationHeight + (now - blossomActivationTime) / params.PoWTargetSpacing(blossomActivationHeight);
    return ((netheight + 5) / 10) * 10;
}

void TriggerRefresh()
{
    *nNextRefresh = GetTime();
    // Ensure that the refresh has started before we return
    MilliSleep(200);
}

static bool metrics_ThreadSafeMessageBox(const std::string& message,
                                      const std::string& caption,
                                      unsigned int style)
{
    // The SECURE flag has no effect in the metrics UI.
    style &= ~CClientUIInterface::SECURE;

    std::string strCaption;
    // Check for usage of predefined caption
    switch (style) {
    case CClientUIInterface::MSG_ERROR:
        strCaption += _("Error");
        break;
    case CClientUIInterface::MSG_WARNING:
        strCaption += _("Warning");
        break;
    case CClientUIInterface::MSG_INFORMATION:
        strCaption += _("Information");
        break;
    default:
        strCaption += caption; // Use supplied caption (can be empty)
    }

    boost::strict_lock_ptr<std::list<std::string>> u = messageBox.synchronize();
    u->push_back(strCaption + ": " + message);
    if (u->size() > 5) {
        u->pop_back();
    }

    TriggerRefresh();
    return false;
}

static bool metrics_ThreadSafeQuestion(const std::string& /* ignored interactive message */, const std::string& message, const std::string& caption, unsigned int style)
{
    return metrics_ThreadSafeMessageBox(message, caption, style);
}

static void metrics_InitMessage(const std::string& message)
{
    *initMessage = message;
}

void ConnectMetricsScreen()
{
    uiInterface.ThreadSafeMessageBox.disconnect_all_slots();
    uiInterface.ThreadSafeMessageBox.connect(metrics_ThreadSafeMessageBox);
    uiInterface.ThreadSafeQuestion.disconnect_all_slots();
    uiInterface.ThreadSafeQuestion.connect(metrics_ThreadSafeQuestion);
    uiInterface.InitMessage.disconnect_all_slots();
    uiInterface.InitMessage.connect(metrics_InitMessage);
}

std::string DisplayDuration(int64_t duration, DurationFormat format)
{
    int64_t days =  duration / (24 * 60 * 60);
    int64_t hours = (duration - (days * 24 * 60 * 60)) / (60 * 60);
    int64_t minutes = (duration - (((days * 24) + hours) * 60 * 60)) / 60;
    int64_t seconds = duration - (((((days * 24) + hours) * 60) + minutes) * 60);

    std::string strDuration;
    if (format == DurationFormat::REDUCED) {
        if (days > 0) {
            strDuration = strprintf(_("%d days"), days);
        } else if (hours > 0) {
            strDuration = strprintf(_("%d hours"), hours);
        } else if (minutes > 0) {
            strDuration = strprintf(_("%d minutes"), minutes);
        } else {
            strDuration = strprintf(_("%d seconds"), seconds);
        }
    } else {
        if (days > 0) {
            strDuration = strprintf(_("%d days, %d hours, %d minutes, %d seconds"), days, hours, minutes, seconds);
        } else if (hours > 0) {
            strDuration = strprintf(_("%d hours, %d minutes, %d seconds"), hours, minutes, seconds);
        } else if (minutes > 0) {
            strDuration = strprintf(_("%d minutes, %d seconds"), minutes, seconds);
        } else {
            strDuration = strprintf(_("%d seconds"), seconds);
        }
    }
    return strDuration;
}

std::string DisplaySize(size_t value)
{
    double coef = 1.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%d Bytes"), value);
    coef *= 1024.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%.2f KiB"), value / coef);
    coef *= 1024.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%.2f MiB"), value / coef);
    coef *= 1024.0;
    if (value < 1024.0 * coef)
        return strprintf(_("%.2f GiB"), value / coef);
    coef *= 1024.0;
    return strprintf(_("%.2f TiB"), value / coef);
}

std::string DisplayHashRate(double value)
{
    double coef = 1.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f H/s"), value);
    coef *= 1000.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f kH/s"), value / coef);
    coef *= 1000.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f MH/s"), value / coef);
    coef *= 1000.0;
    if (value < 1000.0 * coef)
        return strprintf(_("%.3f GH/s"), value / coef);
    coef *= 1000.0;
    return strprintf(_("%.3f TH/s"), value / coef);
}

std::optional<int64_t> SecondsLeftToNextEpoch(const Consensus::Params& params, int currentHeight)
{
    auto nextHeight = NextActivationHeight(currentHeight, params);
    if (nextHeight) {
        return (nextHeight.value() - currentHeight) * params.PoWTargetSpacing(nextHeight.value() - 1);
    } else {
        return std::nullopt;
    }
}

struct MetricsStats {
    int height;
    int64_t currentHeadersHeight;
    int64_t currentHeadersTime;
    size_t connections;
    int64_t netsolps;
};

MetricsStats loadStats()
{
    int height;
    int64_t currentHeadersHeight;
    int64_t currentHeadersTime;
    size_t connections;
    int64_t netsolps;

    {
        LOCK(cs_main);
        height = chainActive.Height();
        currentHeadersHeight = pindexBestHeader ? pindexBestHeader->nHeight: -1;
        currentHeadersTime = pindexBestHeader ? pindexBestHeader->nTime : 0;
        netsolps = GetNetworkHashPS(120, -1);
    }
    {
        LOCK(cs_vNodes);
        connections = vNodes.size();
    }

    return MetricsStats {
        height,
        currentHeadersHeight,
        currentHeadersTime,
        connections,
        netsolps
    };
}

// ============================================================================
// Beautiful UI Helper Functions
// ============================================================================

// Calculate visible length of string (excluding ANSI escape codes)
// Counts UTF-8 characters, not bytes
static size_t visibleLength(const std::string& str) {
    size_t len = 0;
    bool inEscape = false;
    for (size_t i = 0; i < str.length(); ) {
        unsigned char c = str[i];

        if (c == '\e') {
            inEscape = true;
            i++;
        } else if (inEscape && c == 'm') {
            inEscape = false;
            i++;
        } else if (!inEscape) {
            // Count this as one character and skip UTF-8 continuation bytes
            len++;
            // UTF-8: if byte starts with 11xxxxxx, count following 10xxxxxx bytes
            if ((c & 0x80) == 0) {
                i++; // ASCII (0xxxxxxx)
            } else if ((c & 0xE0) == 0xC0) {
                i += 2; // 2-byte UTF-8 (110xxxxx 10xxxxxx)
            } else if ((c & 0xF0) == 0xE0) {
                i += 3; // 3-byte UTF-8 (1110xxxx 10xxxxxx 10xxxxxx)
            } else if ((c & 0xF8) == 0xF0) {
                i += 4; // 4-byte UTF-8 (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            } else {
                i++; // Invalid UTF-8, just skip
            }
        } else {
            i++;
        }
    }
    return len;
}

// Draw a horizontal line with optional title
static void drawLine(const std::string& title, const char* left, const char* right, const char* fill, int width = 72) {
    std::cout << left;
    if (!title.empty()) {
        int titleLen = title.length() + 2; // +2 for spaces
        int leftPad = (width - titleLen) / 2;
        int rightPad = width - titleLen - leftPad;
        for (int i = 0; i < leftPad; i++) std::cout << fill;
        std::cout << " \e[1;37m" << title << "\e[0m ";
        for (int i = 0; i < rightPad; i++) std::cout << fill;
    } else {
        for (int i = 0; i < width; i++) std::cout << fill;
    }
    std::cout << right << "\e[K" << std::endl;
}

// Draw top border of box
static void drawBoxTop(const std::string& title = "", int width = 74) {
    drawLine(title, BOX_TOP_LEFT, BOX_TOP_RIGHT, BOX_HORIZONTAL, width);
}

// Draw bottom border of box
static void drawBoxBottom(int width = 74) {
    drawLine("", BOX_BOTTOM_LEFT, BOX_BOTTOM_RIGHT, BOX_HORIZONTAL, width);
}

// Draw a data row inside a box with label and value
static void drawRow(const std::string& label, const std::string& value, int width = 74) {
    int labelLen = visibleLength(label);
    int valueLen = visibleLength(value);
    int padding = width - labelLen - valueLen - 2; // -2 for the two spaces (after | and before |)

    std::cout << BOX_VERTICAL << " \e[1;36m" << label << "\e[0m";
    for (int i = 0; i < padding; i++) std::cout << " ";
    std::cout << "\e[1;33m" << value << "\e[0m " << BOX_VERTICAL << "\e[K" << std::endl;
}

// Draw a centered text line in a box
static void drawCentered(const std::string& text, const std::string& color = "", int width = 74) {
    int textLen = visibleLength(text);
    int padding = (width - textLen) / 2;
    int rightPad = width - textLen - padding;

    std::cout << BOX_VERTICAL;
    for (int i = 0; i < padding; i++) std::cout << " ";
    if (!color.empty()) std::cout << color;
    std::cout << text;
    if (!color.empty()) std::cout << "\e[0m";
    for (int i = 0; i < rightPad; i++) std::cout << " ";
    std::cout << BOX_VERTICAL << "\e[K" << std::endl;
}

// Draw a progress bar
static void drawProgressBar(int percent, int width = 72) {
    int filled = (percent * width) / 100;
    std::cout << BOX_VERTICAL << " \e[1;32m";
    for (int i = 0; i < filled; i++) std::cout << BOX_PROGRESS_FILLED;
    std::cout << "\e[0;32m";
    for (int i = filled; i < width; i++) std::cout << BOX_PROGRESS_EMPTY;
    std::cout << "\e[0m " << BOX_VERTICAL << "\e[K" << std::endl;
}

// Draw Progress row with inline progress bar
static void drawProgressRow(double progressPercent, int64_t timeMining, int rowWidth = 74) {
    // Format the progress value with elapsed time
    std::string valueStr = strprintf("%.1f%% (%s)",
        progressPercent,
        DisplayDuration(timeMining, DurationFormat::REDUCED).c_str());
    std::string label = "Progress";

    // Calculate available space for progress bar: total width - label - value - padding
    int labelLen = visibleLength(label);
    int valueLen = visibleLength(valueStr);
    // -2 for box borders, -2 for spaces after label, -2 for spaces before value = -6 total
    int barWidth = rowWidth - labelLen - valueLen - 6;
    if (barWidth < 10) barWidth = 10;  // Minimum bar width

    // Calculate filled portion of bar (cap at 100% for visual display)
    int displayPercent = static_cast<int>(progressPercent);
    if (displayPercent > 100) displayPercent = 100;
    if (displayPercent < 0) displayPercent = 0;

    int filled = (displayPercent * barWidth) / 100;

    // Draw the row: | Label  [filled progress bar]  value |
    std::cout << BOX_VERTICAL << " \e[1;36m" << label << "\e[0m  \e[1;32m";
    for (int i = 0; i < filled; i++) std::cout << BOX_PROGRESS_FILLED;
    std::cout << "\e[0;32m";
    for (int i = filled; i < barWidth; i++) std::cout << BOX_PROGRESS_EMPTY;
    std::cout << "\e[0m  \e[1;33m" << valueStr << "\e[0m " << BOX_VERTICAL << "\e[K" << std::endl;
}

// Draw Network Difficulty row with inline meter bar showing position between historical min/max
static void drawDifficultyRow(double currentDifficulty, int rowWidth = 74) {
    // Load difficulty history on first run (from file or blockchain)
    if (!difficultyHistoryInitialized.load()) {
        loadDifficultyHistory();
    }

    // Update historical values if we see new extremes
    bool historyUpdated = false;
    double currentHigh = difficultyHistoricalHigh.load();
    double currentLow = difficultyHistoricalLow.load();

    if (currentDifficulty > currentHigh) {
        difficultyHistoricalHigh = currentDifficulty;
        historyUpdated = true;
    }
    if (currentDifficulty < currentLow || currentLow == 0.0) {
        difficultyHistoricalLow = currentDifficulty;
        historyUpdated = true;
    }

    // Save to file if we updated the history
    if (historyUpdated) {
        saveDifficultyHistory();
    }

    // Calculate percentage for meter bar
    double high = difficultyHistoricalHigh.load();
    double low = difficultyHistoricalLow.load();
    int percent = 50;  // Default to middle if no range

    if (high > low) {
        double range = high - low;
        double position = currentDifficulty - low;
        percent = static_cast<int>((position / range) * 100);
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
    }

    // Format the difficulty value
    std::string valueStr = strprintf("%.6f", currentDifficulty);
    std::string label = "Network Difficulty";

    // Calculate available space for meter bar: total width - label - value - padding
    int labelLen = visibleLength(label);
    int valueLen = visibleLength(valueStr);
    // -2 for box borders, -2 for spaces after label, -2 for spaces before value = -6 total
    int meterWidth = rowWidth - labelLen - valueLen - 6;
    if (meterWidth < 10) meterWidth = 10;  // Minimum meter width

    // Calculate marker position within the bar
    int markerPos = (percent * meterWidth) / 100;
    if (markerPos >= meterWidth) markerPos = meterWidth - 1;

    // Determine color based on difficulty level
    // Green (low): 0-33%, Orange (average): 34-66%, Red (high): 67-100%
    const char* barColor;
    const char* markerColor;
    if (percent <= 33) {
        barColor = "\e[0;32m";     // Green for low difficulty
        markerColor = "\e[1;35m";  // Bright magenta marker (high contrast with green)
    } else if (percent <= 66) {
        barColor = "\e[0;33m";     // Orange for average difficulty
        markerColor = "\e[1;36m";  // Bright cyan marker (high contrast with orange)
    } else {
        barColor = "\e[0;31m";     // Red for high difficulty
        markerColor = "\e[1;36m";  // Bright cyan marker (high contrast with red)
    }

    // Draw the row: | Label  [bar with colored marker]  value |
    std::cout << BOX_VERTICAL << " \e[1;36m" << label << "\e[0m  " << barColor;
    for (int i = 0; i < meterWidth; i++) {
        if (i == markerPos) {
            std::cout << markerColor << BOX_PROGRESS_FILLED << barColor;  // High-contrast marker
        } else {
            std::cout << BOX_PROGRESS_EMPTY;
        }
    }
    std::cout << "\e[0m  \e[1;33m" << valueStr << "\e[0m " << BOX_VERTICAL << "\e[K" << std::endl;
}

int printStats(MetricsStats stats, bool isScreen, bool mining)
{
    int lines = 0;
    const Consensus::Params& params = Params().GetConsensus();
    auto localsolps = GetLocalSolPS();

    // Network Status Box
    drawBoxTop("NETWORK STATUS");
    lines++;

    // Syncing or synced status
    if (IsInitialBlockDownload(Params().GetConsensus())) {
        if (fReindex) {
            int downloadPercent = nSizeReindexed * 100 / nFullSizeToReindex;
            drawRow("Status", strprintf("Reindexing (%d%%)", downloadPercent));
            lines++;
            drawRow("Progress", strprintf("%s / %s",
                DisplaySize(nSizeReindexed), DisplaySize(nFullSizeToReindex)));
            lines++;
            drawRow("Blocks", strprintf("%d", stats.height));
            lines++;

            if (isScreen) {
                drawProgressBar(downloadPercent);
                lines++;
            }
        } else {
            int nHeaders = stats.currentHeadersHeight < 0 ? 0 : stats.currentHeadersHeight;
            int netheight = stats.currentHeadersHeight == -1 || stats.currentHeadersTime == 0 ?
                0 : EstimateNetHeight(params, stats.currentHeadersHeight, stats.currentHeadersTime);
            if (netheight < nHeaders) netheight = nHeaders;
            if (netheight <= 0) netheight = 1;
            int downloadPercent = stats.height * 100 / netheight;

            drawRow("Status", strprintf("\e[1;33mSYNCING\e[0m (%d%%)", downloadPercent));
            lines++;
            drawRow("Block Height", strprintf("%d / %d", stats.height, netheight));
            lines++;

            if (isScreen) {
                drawProgressBar(downloadPercent);
                lines++;
            }
        }
    } else {
        drawRow("Status", "\e[1;32m● SYNCHRONIZED\e[0m");
        lines++;
        drawRow("Block Height", strprintf("%d", stats.height));
        lines++;
    }

    // Network Difficulty with inline meter bar
    double difficulty = GetNetworkDifficulty(chainActive.Tip());
    if (isScreen) {
        drawDifficultyRow(difficulty);
    } else {
        drawRow("Network Difficulty", strprintf("%.6f", difficulty));
    }
    lines++;

    // Network info
    auto secondsLeft = SecondsLeftToNextEpoch(params, stats.height);
    if (secondsLeft) {
        auto nextHeight = NextActivationHeight(stats.height, params).value();
        auto nextBranch = NextEpoch(stats.height, params).value();
        drawRow("Next Upgrade", strprintf("%s at %d (~%s)",
            NetworkUpgradeInfo[nextBranch].strName, nextHeight,
            DisplayDuration(secondsLeft.value(), DurationFormat::REDUCED)));
    } else {
        drawRow("Next Upgrade", "None scheduled");
    }
    lines++;

    drawRow("Connections", strprintf("%d", stats.connections));
    lines++;
    drawRow("Network Hash", DisplayHashRate(stats.netsolps));
    lines++;

    if (mining && miningTimer.running()) {
        drawRow("Your Hash Rate", DisplayHashRate(localsolps));
        lines++;
    }

    drawBoxBottom();
    lines++;
    std::cout << std::endl;
    lines++;

    return lines;
}


// Forward declarations for donation helper functions
static int getCurrentDonationPercentage();
static std::string getCurrentDonationAddress();

// Forward declarations for address display
static std::vector<std::string> getShieldedAddresses();

// Address display state - controls whether j1 address is expanded or truncated
static bool expandJ1Address = false;

int printWalletStatus()
{
    int lines = 0;

    // Wallet Balance Box
    drawBoxTop("WALLET");
    lines++;

    if (pwalletMain) {
        // Get transparent (mined) balances
        CAmount immature = pwalletMain->GetImmatureBalance(std::nullopt);
        CAmount mature = pwalletMain->GetBalance(std::nullopt);

        // Update hasShieldableCoins flag for controls display
        hasShieldableCoins.store(mature > 0);

        // Get shielded balances from account 0
        // Orchard notes require 10 confirmations to be spendable
        CAmount shieldedSpendable = 0;    // minconf >= 10 (actually spendable)
        CAmount shieldedConfirming = 0;   // minconf 1-9 (confirmed but not yet spendable)
        CAmount shieldedUnconfirmed = 0;  // minconf 0 (pending in mempool)

        {
            LOCK2(cs_main, pwalletMain->cs_wallet);

            auto selector = pwalletMain->ZTXOSelectorForAccount(0, false, TransparentCoinbasePolicy::Allow);
            if (selector.has_value()) {
                // Helper to sum shielded inputs
                auto sumShieldedInputs = [](const SpendableInputs& inputs) {
                    CAmount total = 0;
                    for (const auto& t : inputs.saplingNoteEntries) {
                        total += t.note.value();
                    }
                    for (const auto& t : inputs.orchardNoteMetadata) {
                        total += t.GetNoteValue();
                    }
                    return total;
                };

                // Get spendable balance (minconf=10)
                auto spendableInputs = pwalletMain->FindSpendableInputs(selector.value(), 10, std::nullopt);
                shieldedSpendable = sumShieldedInputs(spendableInputs);

                // Get confirmed balance (minconf=1) to calculate confirming
                auto confirmedInputs = pwalletMain->FindSpendableInputs(selector.value(), 1, std::nullopt);
                CAmount totalConfirmed = sumShieldedInputs(confirmedInputs);
                shieldedConfirming = totalConfirmed - shieldedSpendable;

                // Get total balance (minconf=0) to calculate unconfirmed
                auto allInputs = pwalletMain->FindSpendableInputs(selector.value(), 0, std::nullopt);
                CAmount totalAll = sumShieldedInputs(allInputs);
                shieldedUnconfirmed = totalAll - totalConfirmed;

                // Clear shielding flag when unconfirmed balance becomes 0
                if (shieldedUnconfirmed == 0 && shieldingInProgress.load()) {
                    shieldingInProgress.store(false);
                }
            }
        }

        std::string units = Params().CurrencyUnits();

        drawRow("Mined Mature Balance", strprintf("%s %s", FormatMoney(mature), units.c_str()));
        lines++;
        drawRow("Mined Immature Balance", strprintf("%s %s", FormatMoney(immature), units.c_str()));
        lines++;
        drawRow("Shielded Balance", strprintf("%s %s", FormatMoney(shieldedSpendable), units.c_str()));
        lines++;
        if (shieldedConfirming > 0) {
            drawRow("Shielded Confirming", strprintf("%s %s", FormatMoney(shieldedConfirming), units.c_str()));
            lines++;
        }
        if (shieldedUnconfirmed > 0) {
            drawRow("Shielded Unconfirmed", strprintf("%s %s", FormatMoney(shieldedUnconfirmed), units.c_str()));
            lines++;
        }

        // Show blocks mined if any
        int blocksMined = minedBlocks.get();
        if (blocksMined > 0) {
            int orphaned = 0;
            {
                LOCK2(cs_main, cs_metrics);
                boost::strict_lock_ptr<std::list<uint256>> u = trackedBlocks.synchronize();

                // Update orphaned block count
                std::list<uint256>::iterator it = u->begin();
                while (it != u->end()) {
                    auto hash = *it;
                    if (mapBlockIndex.count(hash) > 0 &&
                            chainActive.Contains(mapBlockIndex[hash])) {
                        it++;
                    } else {
                        it = u->erase(it);
                    }
                }

                orphaned = blocksMined - u->size();
            }

            drawRow("Blocks Mined", strprintf("%d (orphaned: %d)", blocksMined, orphaned));
            lines++;
        }

        // Default Mining Address (t1...)
        if (pwalletMain->vchDefaultKey.IsValid()) {
            KeyIO keyIO(Params());
            std::string minerAddr = keyIO.EncodeDestination(pwalletMain->vchDefaultKey.GetID());
            drawRow("Miner Address", minerAddr);
            lines++;
        }

        // Default Unified Address (j1...)
        auto addresses = getShieldedAddresses();
        if (!addresses.empty()) {
            std::string j1Addr = addresses[0];  // First unified address (diversifier 0)
            if (expandJ1Address) {
                // Full address on single line for easy copy-paste
                std::cout << BOX_VERTICAL << " \e[1;36mShielded Address:\e[0m \e[1;33m" << j1Addr << "\e[0m\e[K" << std::endl;
                lines++;
            } else {
                // Truncate: first 12 + "..." + last 12
                std::string displayAddr;
                if (j1Addr.length() > 27) {
                    displayAddr = j1Addr.substr(0, 12) + "..." + j1Addr.substr(j1Addr.length() - 12);
                } else {
                    displayAddr = j1Addr;
                }
                drawRow("Shielded Address", displayAddr);
                lines++;
            }
        }
    } else {
        drawRow("Status", "Wallet not loaded");
        lines++;
    }

    drawBoxBottom();
    lines++;
    std::cout << std::endl;
    lines++;

    return lines;
}

int printMiningStatus(bool mining)
{
#ifdef ENABLE_MINING
    int lines = 0;

    // Mining Status Box
    drawBoxTop("MINING");
    lines++;

    if (mining) {
        auto nThreads = miningTimer.threadCount();
        if (nThreads > 0) {
            // Check if in warmup phase (first 10 seconds after mining starts, or hashrate is 0)
            int64_t startTime = miningStartTime.load();
            bool isWarmingUp = false;
            if (startTime > 0) {
                int64_t elapsedTime = GetTime() - startTime;
                double currentHashrate = GetLocalSolPS();
                isWarmingUp = (elapsedTime < MINING_WARMUP_SECONDS && currentHashrate == 0);
            }

            if (isWarmingUp) {
                drawRow("Status", strprintf("\e[1;33m● WARMING UP\e[0m - %d threads", nThreads));
            } else {
                drawRow("Status", strprintf("\e[1;32m● ACTIVE\e[0m - %d threads", nThreads));
            }
            lines++;

            // Show CPU model
            static std::string cpuModel = GetCPUModel();
            if (cpuModel != "Unknown CPU") {
                drawRow("CPU", cpuModel);
                lines++;
            }

            // Show memory DIMMs (detailed info from DMI)
            InitDmiReader();
            if (g_dmiReader && !g_dmiReader->memory().empty()) {
                const auto& memory = g_dmiReader->memory();
                bool hasMemory = false;

                // Count valid/populated DIMMs
                for (const auto& dimm : memory) {
                    if (dimm.isValid() && dimm.size() > 0) {
                        hasMemory = true;
                        break;
                    }
                }

                if (hasMemory) {
                    // Show total first with channel info and DPC
                    uint64_t totalMemory = 0;
                    std::map<char, int> channelDimmCount;  // Track DIMMs per channel

                    for (const auto& dimm : memory) {
                        if (dimm.isValid() && dimm.size() > 0) {
                            totalMemory += dimm.size();

                            // Extract channel from slot ID (e.g., "DIMM_A1" -> 'A')
                            std::string slotId = dimm.id().data();
                            char channelChar = 0;

                            // Look for pattern: DIMM_X# where X is the channel letter
                            size_t underscorePos = slotId.find('_');
                            if (underscorePos != std::string::npos && underscorePos + 1 < slotId.length()) {
                                channelChar = slotId[underscorePos + 1];
                                if (channelChar >= 'A' && channelChar <= 'Z') {
                                    channelDimmCount[channelChar]++;
                                }
                            }
                            // Also try bank locator (e.g., "CHANNEL A")
                            else if (!dimm.bank().isEmpty()) {
                                std::string bank = dimm.bank().data();
                                for (char c : bank) {
                                    if (c >= 'A' && c <= 'Z') {
                                        channelDimmCount[c]++;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (totalMemory > 0) {
                        std::string memoryStr = strprintf("%d GB", (totalMemory + 512*1024*1024) / (1024*1024*1024));

                        // Add channel info and DPC
                        if (!channelDimmCount.empty()) {
                            int numChannels = channelDimmCount.size();

                            // Calculate DPC (DIMMs per channel) - use the most common value
                            std::map<int, int> dpcFrequency;
                            for (const auto& pair : channelDimmCount) {
                                dpcFrequency[pair.second]++;
                            }

                            int mostCommonDPC = 0;
                            int maxFrequency = 0;
                            for (const auto& pair : dpcFrequency) {
                                if (pair.second > maxFrequency) {
                                    maxFrequency = pair.second;
                                    mostCommonDPC = pair.first;
                                }
                            }

                            // Format channel description
                            std::string channelDesc;
                            if (numChannels == 1) channelDesc = "Single Channel";
                            else if (numChannels == 2) channelDesc = "Dual Channel";
                            else if (numChannels == 3) channelDesc = "Triple Channel";
                            else if (numChannels == 4) channelDesc = "Quad Channel";
                            else if (numChannels == 6) channelDesc = "Hexa Channel";
                            else if (numChannels == 8) channelDesc = "Octa Channel";
                            else if (numChannels == 12) channelDesc = "Dodeca Channel";
                            else channelDesc = strprintf("%d-Channel", numChannels);

                            // Add DPC info if valid
                            if (mostCommonDPC > 0) {
                                memoryStr += strprintf(" (%s, %dDPC)", channelDesc.c_str(), mostCommonDPC);
                            } else {
                                memoryStr += strprintf(" (%s)", channelDesc.c_str());
                            }
                        }

                        drawRow("Memory", memoryStr);
                        lines++;
                    }

                    // Show individual DIMMs (only populated ones)
                    for (const auto& dimm : memory) {
                        if (dimm.isValid() && dimm.size() > 0) {
                            // Format: DIMM_A1: 32 GB DDR4 Corsair CMK16GX4M2B3200C16 @ 3133 MHz
                            std::stringstream dimmSS;
                            dimmSS << dimm.id().data() << ": "
                                   << (dimm.size() + 512*1024*1024) / (1024*1024*1024) << " GB";

                            // Add type if available and not "Undefined"
                            std::string typeStr = dimm.type();
                            if (!typeStr.empty() && typeStr != "Undefined" && typeStr != "Unknown") {
                                dimmSS << " " << typeStr;
                            }

                            // Add vendor if available and not "Undefined"
                            if (dimm.vendor().isValid() && !dimm.vendor().isEmpty()) {
                                std::string vendorStr = dimm.vendor().data();
                                if (vendorStr != "Undefined" && vendorStr != "Unknown") {
                                    dimmSS << " " << vendorStr;
                                }
                            }

                            // Add product/model if available and not "Undefined"
                            if (dimm.product().isValid() && !dimm.product().isEmpty()) {
                                std::string productStr = dimm.product().data();
                                if (productStr != "Undefined" && productStr != "Unknown") {
                                    dimmSS << " " << productStr;
                                }
                            }

                            // Add speed at the end if available
                            if (dimm.speed() > 0) {
                                dimmSS << " @ " << dimm.speed() / 1000000 << " MHz";
                            }

                            drawRow("", dimmSS.str());
                            lines++;
                        }
                    }
                }
            } else {
                // Fallback to simple memory info
                static std::string memoryInfo = GetMemoryInfo();
                if (memoryInfo != "Unknown") {
                    drawRow("Memory", memoryInfo);
                    lines++;
                }
            }

            // Show motherboard model
            static std::string motherboard = GetMotherboardModel();
            if (motherboard != "Unknown") {
                drawRow("Motherboard", motherboard);
                lines++;
            }

            // Show block reward
            int nHeight = chainActive.Height() + 1; // Next block to be mined
            CAmount blockReward = Params().GetConsensus().GetBlockSubsidy(nHeight);
            drawRow("Block Reward", FormatMoney(blockReward));
            lines++;

            // Show RandomX mining mode
            bool isFastMode = RandomX_IsFastMode();
            bool hugepagesInUse = RandomX_IsUsingHugepages();
            std::string miningMode;

            if (isFastMode) {
                if (hugepagesInUse) {
                    miningMode = "\e[1;32mFast + Hugepages\e[0m";
                } else {
                    miningMode = "\e[1;36mFast\e[0m";
                }
            } else {
                miningMode = "\e[1;33mLight\e[0m";
            }

            drawRow("Mining Mode", miningMode);
            lines++;

            // Show mining probability and expected time to find block
            if (!benchmarkMode.load()) {
                double currentHashrate = GetLocalSolPS();
                double expectedTime = calculateExpectedBlockTime(currentHashrate);

                // Show expected time (or "Calculating..." if hashrate is still 0)
                if (currentHashrate > 0 && expectedTime > 0) {
                    std::string expectedTimeStr = DisplayDuration(expectedTime, DurationFormat::FULL);
                    drawRow("Expected Time", expectedTimeStr);
                } else {
                    drawRow("Expected Time", "\e[1;33mCalculating...\e[0m");
                }
                lines++;

                // Calculate time since mining started or last block found
                int64_t timeMining = 0;
                int64_t lastFound = lastBlockFoundTime.load();
                int64_t miningStart = miningStartTime.load();

                if (lastFound > 0 && lastFound > miningStart) {
                    timeMining = GetTime() - lastFound;
                } else if (miningStart > 0) {
                    timeMining = GetTime() - miningStart;
                }

                // Show luck for last block found (if any this session)
                if (lastBlockFoundTime.load() > 0 && lastBlockLuckPercent.load() > 0) {
                    double luck = lastBlockLuckPercent.load();
                    std::string luckColor;
                    if (luck >= 100) {
                        luckColor = "\e[1;32m";  // Green for lucky (less time than expected)
                    } else if (luck >= 50) {
                        luckColor = "\e[1;33m";  // Orange for average
                    } else {
                        luckColor = "\e[1;31m";  // Red for unlucky (more time than expected)
                    }
                    drawRow("Last Block Luck", strprintf("%s%.1f%%\e[0m", luckColor.c_str(), luck));
                    lines++;
                }

                // Always show progress toward finding next block (even at 0%)
                double progressPercent = (timeMining > 0 && expectedTime > 0) ? (timeMining / expectedTime) * 100.0 : 0.0;
                if (progressPercent > 999) progressPercent = 999;  // Cap display at 999%

                drawProgressRow(progressPercent, timeMining);
                lines++;
            }

            // Show benchmark status if active
            if (benchmarkMode.load()) {
                int currentThread = benchmarkCurrentThreads.load();
                int maxThread = benchmarkMaxThreads.load();

                std::string benchStatus;
                if (benchmarkWarmingUp.load()) {
                    benchStatus = strprintf("\e[1;33m● WARMING UP\e[0m - Thread %d/%d",
                        currentThread, maxThread);
                } else {
                    double currentHashrate = GetLocalSolPS();
                    benchStatus = strprintf("\e[1;36m● RUNNING\e[0m - Thread %d/%d (%.1f H/s)",
                        currentThread, maxThread, currentHashrate);
                }
                drawRow("Benchmark", benchStatus);
                lines++;
            }
        } else {
            bool fvNodesEmpty;
            {
                LOCK(cs_vNodes);
                fvNodesEmpty = vNodes.empty();
            }
            if (fvNodesEmpty) {
                drawRow("Status", "\e[1;33m○ PAUSED\e[0m - Waiting for connections");
            } else if (IsInitialBlockDownload(Params().GetConsensus())) {
                drawRow("Status", "\e[1;33m○ PAUSED\e[0m - Downloading blocks");
            } else {
                drawRow("Status", "\e[1;33m○ PAUSED\e[0m - Processing");
            }
            lines++;
        }

        // Show donation status if active
        int donationPct = getCurrentDonationPercentage();
        if (donationPct > 0) {
            std::string donationAddr = getCurrentDonationAddress();
            if (!donationAddr.empty()) {
                std::string shortAddr;
                if (donationAddr.length() > 20) {
                    shortAddr = donationAddr.substr(0, 10) + "..." + donationAddr.substr(donationAddr.length() - 6);
                } else {
                    shortAddr = donationAddr;
                }
                drawRow("Donations", strprintf("\e[1;35m%d%%\e[0m → %s", donationPct, shortAddr.c_str()));
            } else {
                drawRow("Donations", strprintf("\e[1;35m%d%%\e[0m → \e[1;31mNO ADDRESS SET\e[0m", donationPct));
            }
            lines++;
        }
    } else {
        drawRow("Status", "\e[1;31m○ INACTIVE\e[0m");
        lines++;
    }

    drawBoxBottom();
    lines++;
    std::cout << std::endl;
    lines++;

    // Controls Box
    drawBoxTop("CONTROLS");
    lines++;

    if (mining) {
        // Get current thread count (use benchmark current threads if in benchmark mode)
        int nThreads;
        if (benchmarkMode.load()) {
            nThreads = benchmarkCurrentThreads.load();
        } else {
            nThreads = GetArg("-genproclimit", 1);
        }

        // Line 1: Mining status, threads, donations, quit
        std::string miningStatus = miningStopInProgress.load() ? "\e[1;33mSTOPPING...\e[0m" : "\e[1;32mON\e[0m";
        std::string controls1 = strprintf("\e[1;37m[M]\e[0m Mining: %s  \e[1;37m[T]\e[0m Threads: %d", miningStatus.c_str(), nThreads);

        int donationPct = getCurrentDonationPercentage();
        if (donationPct > 0) {
            controls1 += strprintf("  \e[1;37m[D]\e[0m Donations: \e[1;35mON (%d%%)\e[0m  \e[1;37m[P]\e[0m Change %%", donationPct);
        } else {
            controls1 += "  \e[1;37m[D]\e[0m Donations: \e[1;31mOFF\e[0m";
        }

        controls1 += "  \e[1;37m[Q]\e[0m Quit";
        drawCentered(controls1);
        lines++;

        // Line 2: Mining mode toggles and benchmark
        bool isFastMode = RandomX_IsFastMode();
        bool hugepagesInUse = RandomX_IsUsingHugepages();
        bool buildingDataset = RandomX_IsBuildingDataset();

        std::string modeStatus;
        if (buildingDataset) {
            modeStatus = "\e[1;33mBUILDING...\e[0m";
        } else {
            modeStatus = isFastMode ? "\e[1;32mFAST\e[0m" : "\e[1;36mLIGHT\e[0m";
        }
        std::string hugepagesStatus = hugepagesInUse ? "\e[1;32mON\e[0m" : "\e[1;31mOFF\e[0m";

        std::string addrLabel = expandJ1Address ? "Collapse" : "Expand";
        bool showShield = hasShieldableCoins.load() || shieldingInProgress.load();
        std::string shieldPart = "";
        if (showShield) {
            std::string shieldLabel = shieldingInProgress.load() ? "\e[1;33mPROCESSING\e[0m" : "Shield";
            shieldPart = strprintf("  \e[1;37m[S]\e[0m %s", shieldLabel.c_str());
        }
        std::string controls2 = strprintf("\e[1;37m[R]\e[0m RandomX: %s  \e[1;37m[H]\e[0m Hugepages: %s  \e[1;37m[A]\e[0m %s%s  \e[1;37m[B]\e[0m Benchmark",
            modeStatus.c_str(), hugepagesStatus.c_str(), addrLabel.c_str(), shieldPart.c_str());
        drawCentered(controls2);
    } else {
        // Show STOPPING if mining is being stopped in background
        std::string offStatus = miningStopInProgress.load() ? "\e[1;33mSTOPPING...\e[0m" : "\e[1;31mOFF\e[0m";
        std::string addrLabel = expandJ1Address ? "Collapse" : "Expand";
        bool showShield = hasShieldableCoins.load() || shieldingInProgress.load();
        std::string shieldPart = "";
        if (showShield) {
            std::string shieldLabel = shieldingInProgress.load() ? "\e[1;33mPROCESSING\e[0m" : "Shield";
            shieldPart = strprintf("  \e[1;37m[S]\e[0m %s", shieldLabel.c_str());
        }
        std::string controls = strprintf("\e[1;37m[M]\e[0m Mining: %s  \e[1;37m[A]\e[0m %s%s  \e[1;37m[Q]\e[0m Quit", offStatus.c_str(), addrLabel.c_str(), shieldPart.c_str());
        drawCentered(controls);
    }
    lines++;

    drawBoxBottom();
    lines++;

    return lines;
#else // ENABLE_MINING
    return 0;
#endif // !ENABLE_MINING
}

// Force full screen clear on next frame (used when layout changes)
static bool forceFullClear = false;

static void toggleAddressExpansion()
{
    expandJ1Address = !expandJ1Address;
    forceFullClear = true;
}

// Shield coinbase UTXOs to the default j1 address
static void shieldCoinbase(int rows)
{
    // Prevent repeated keypresses while shielding
    if (shieldingInProgress.load()) {
        return;
    }

    if (!pwalletMain) {
        std::cout << "\r\e[K\e[1;31mError: Wallet not available\e[0m" << std::flush;
        MilliSleep(2000);
        return;
    }

    // Check for unconfirmed shielded balance (previous shield still processing)
    {
        LOCK2(cs_main, pwalletMain->cs_wallet);
        auto selector = pwalletMain->ZTXOSelectorForAccount(0, false, TransparentCoinbasePolicy::Allow);
        if (selector.has_value()) {
            auto allInputs = pwalletMain->FindSpendableInputs(selector.value(), 0, std::nullopt);
            auto confirmedInputs = pwalletMain->FindSpendableInputs(selector.value(), 1, std::nullopt);

            CAmount totalAll = 0, totalConfirmed = 0;
            for (const auto& t : allInputs.orchardNoteMetadata) totalAll += t.GetNoteValue();
            for (const auto& t : confirmedInputs.orchardNoteMetadata) totalConfirmed += t.GetNoteValue();

            if (totalAll > totalConfirmed) {
                std::cout << "\r\e[K\e[1;33mWaiting for previous shield to confirm...\e[0m" << std::flush;
                MilliSleep(2000);
                return;
            }
        }
    }

    // Get the default j1 address
    auto addresses = getShieldedAddresses();
    if (addresses.empty()) {
        std::cout << "\r\e[K\e[1;31mError: No shielded address available\e[0m" << std::flush;
        MilliSleep(2000);
        return;
    }

    std::string toAddress = addresses[0];

    // Set shielding flag
    shieldingInProgress.store(true);

    // Show shielding message
    std::cout << "\r\e[K\e[1;33mShielding coinbase to " << toAddress.substr(0, 20) << "...\e[0m" << std::flush;

    try {
        // Build RPC parameters: z_shieldcoinbase "*" "j1address..."
        UniValue params(UniValue::VARR);
        params.push_back("*");
        params.push_back(toAddress);

        // Call the RPC function via tableRPC
        const CRPCCommand* cmd = tableRPC["z_shieldcoinbase"];
        if (!cmd) {
            std::cout << "\r\e[K\e[1;31mError: RPC command not found\e[0m" << std::flush;
            shieldingInProgress.store(false);
            MilliSleep(2000);
            return;
        }

        UniValue result = cmd->actor(params, false);

        // Extract operation info from result
        int shieldingUTXOs = find_value(result, "shieldingUTXOs").get_int();
        double shieldingValue = find_value(result, "shieldingValue").get_real();
        int remainingUTXOs = find_value(result, "remainingUTXOs").get_int();
        std::string opid = find_value(result, "opid").get_str();

        if (shieldingUTXOs == 0) {
            std::cout << "\r\e[K\e[1;33mNo coinbase UTXOs to shield\e[0m" << std::flush;
            shieldingInProgress.store(false);
        } else {
            std::cout << "\r\e[K\e[1;32mShielding " << shieldingUTXOs << " UTXOs ("
                      << FormatMoney(shieldingValue * COIN) << " JUNO)"
                      << (remainingUTXOs > 0 ? strprintf(" - %d remaining", remainingUTXOs) : "")
                      << "\e[0m" << std::flush;
            // Keep shieldingInProgress true until unconfirmed balance clears
        }
        MilliSleep(2000);

    } catch (const UniValue& e) {
        std::string errMsg = find_value(e, "message").get_str();
        std::cout << "\r\e[K\e[1;31mError: " << errMsg << "\e[0m" << std::flush;
        shieldingInProgress.store(false);
        MilliSleep(3000);
    } catch (const std::exception& e) {
        std::cout << "\r\e[K\e[1;31mError: " << e.what() << "\e[0m" << std::flush;
        shieldingInProgress.store(false);
        MilliSleep(3000);
    }
}

// Get first 3 unified addresses for account 0 (indices 0, 1, 2)
static std::vector<std::string> getShieldedAddresses()
{
    std::vector<std::string> addresses;
    if (!pwalletMain) return addresses;

    LOCK(pwalletMain->cs_wallet);
    KeyIO keyIO(Params());

    // Find the account 0 UFVK
    libzcash::UFVKId ufvkId;
    bool foundAccount = false;
    for (const auto& [acctKey, id] : pwalletMain->mapUnifiedAccountKeys) {
        if (acctKey.second == libzcash::AccountId(0)) {
            ufvkId = id;
            foundAccount = true;
            break;
        }
    }

    if (!foundAccount) return addresses;

    // Get address metadata for this UFVK
    auto metaIt = pwalletMain->mapUfvkAddressMetadata.find(ufvkId);
    if (metaIt == pwalletMain->mapUfvkAddressMetadata.end()) return addresses;

    auto ufvkOpt = pwalletMain->GetUnifiedFullViewingKey(ufvkId);
    if (!ufvkOpt.has_value()) return addresses;
    auto ufvk = ufvkOpt.value();

    // Get known addresses sorted by diversifier index
    std::vector<std::pair<libzcash::diversifier_index_t, std::set<libzcash::ReceiverType>>> sortedAddrs;
    for (const auto& [j, receiverTypes] : metaIt->second.GetKnownReceiverSetsByDiversifierIndex()) {
        sortedAddrs.push_back({j, receiverTypes});
    }

    // Sort by diversifier index (numerically)
    std::sort(sortedAddrs.begin(), sortedAddrs.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    // Get first 3 addresses
    for (size_t i = 0; i < std::min(sortedAddrs.size(), size_t(3)); i++) {
        auto addrResult = ufvk.Address(sortedAddrs[i].first, sortedAddrs[i].second);
        auto addrPair = std::get_if<std::pair<libzcash::UnifiedAddress, libzcash::diversifier_index_t>>(&addrResult);
        if (addrPair) {
            addresses.push_back(keyIO.EncodePaymentAddress(addrPair->first));
        }
    }

    return addresses;
}



int printMetrics(size_t cols, bool mining)
{
    // Number of lines that are always displayed
    int lines = 2;

    // Calculate and display uptime
    std::string duration = DisplayDuration(GetUptime(), DurationFormat::FULL);

    std::string strDuration = strprintf(_("Uptime: %s"), duration);
    std::cout << strDuration << std::endl;
    lines += (strDuration.size() / cols);
    std::cout << std::endl;

    return lines;
}

int printMessageBox(size_t cols)
{
    boost::strict_lock_ptr<std::list<std::string>> u = messageBox.synchronize();

    if (u->size() == 0) {
        return 0;
    }

    int lines = 2 + u->size();
    std::cout << _("Messages:") << std::endl;
    for (auto it = u->cbegin(); it != u->cend(); ++it) {
        auto msg = FormatParagraph(*it, cols, 2);
        std::cout << "- " << msg << std::endl;
        // Handle newlines and wrapped lines
        size_t i = 0;
        size_t j = 0;
        while (j < msg.size()) {
            i = msg.find('\n', j);
            if (i == std::string::npos) {
                i = msg.size();
            } else {
                // Newline
                lines++;
            }
            j = i + 1;
        }
    }
    std::cout << std::endl;
    return lines;
}

int printInitMessage()
{
    if (loaded) {
        return 0;
    }

    std::string msg = *initMessage;
    std::cout << _("Node is starting up:") << " " << msg << std::endl;
    std::cout << std::endl;

    if (msg == _("Done loading")) {
        loaded = true;
    }

    return 2;
}

#ifdef WIN32
bool enableVTMode()
{
    // Set output mode to handle virtual terminal sequences
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) {
        return false;
    }

    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(hOut, dwMode)) {
        return false;
    }

    // Enable UTF-8 output for box-drawing characters (Windows 10 1903+)
    SetConsoleOutputCP(CP_UTF8);

    return true;
}
#endif

// Helper function to check for keyboard input without blocking
static int checkKeyPress()
{
#ifdef WIN32
    if (_kbhit()) {
        return _getch();
    }
    return 0;
#else
    struct pollfd fds;
    fds.fd = STDIN_FILENO;
    fds.events = POLLIN;

    int ret = poll(&fds, 1, 0);  // 0 timeout = non-blocking
    if (ret > 0 && (fds.revents & POLLIN)) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            return c;
        }
    }
    return 0;
#endif
}

// Terminal mode management for input prompts
#ifndef WIN32
static struct termios orig_termios;
static bool termios_saved = false;

static void disableRawMode()
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}

static void enableRawMode()
{
    if (!termios_saved) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        termios_saved = true;
        atexit(disableRawMode);
    }

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);  // Disable canonical mode and echo
    raw.c_cc[VMIN] = 0;   // Non-blocking read
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void enableCanonicalMode()
{
    if (termios_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    }
}
#endif

// Get current donation percentage
static int getCurrentDonationPercentage()
{
    return GetArg("-donationpercentage", 0);
}

// Get current donation address
static std::string getCurrentDonationAddress()
{
    std::string donationAddr = GetArg("-donationaddress", "");

    // Use default address from chain params if not specified
    if (donationAddr.empty()) {
        donationAddr = Params().GetDefaultDonationAddress();
    }

    return donationAddr;
}

// Update donation percentage
static void updateDonationPercentage(int percentage)
{
    if (percentage < 0 || percentage > 100) {
        return;  // Invalid range
    }

    mapArgs["-donationpercentage"] = itostr(percentage);

    if (percentage > 0) {
        std::string devAddress = getCurrentDonationAddress();
        LogPrintf("User set donation to %d%% (address: %s)\n", percentage, devAddress);
    } else {
        LogPrintf("User disabled donations\n");
    }
}

// Toggle donation on/off
static void toggleDonation()
{
    int current = getCurrentDonationPercentage();
    if (current > 0) {
        // Turn off
        updateDonationPercentage(0);
    } else {
        // Check if donation address is available (will use defaults on mainnet/testnet)
        std::string donationAddr = getCurrentDonationAddress();
        if (donationAddr.empty()) {
            // This can only happen on regtest without explicit address
            LogPrintf("Cannot enable donations: no -donationaddress configured (regtest requires explicit address)\n");
            return;
        }
        // Turn on with default 5%
        updateDonationPercentage(5);
    }
}

// Prompt user for donation percentage (with fixed screen positioning)
static void promptForPercentage(int screenHeight)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen (3 rows from bottom)
    int inputRow = screenHeight - 2;

    // Clear input area
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Enter donation percentage (0-100): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    try {
        int percentage = std::stoi(input);
        if (percentage >= 0 && percentage <= 100) {
            updateDonationPercentage(percentage);
            std::cout << "\e[" << inputRow << ";1H\e[K";  // Clear and reposition
            if (percentage == 0) {
                std::cout << SYMBOL_CHECK << " Donations disabled" << std::flush;
            } else {
                std::cout << SYMBOL_CHECK << " Donation set to " << percentage << "%" << std::flush;
            }
        } else {
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CROSS << " Invalid percentage (must be 0-100)" << std::flush;
        }
    } catch (...) {
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CROSS << " Invalid input (not a number)" << std::flush;
    }

    // Brief pause to show confirmation, then clear
    MilliSleep(800);
    std::cout << "\e[" << inputRow << ";1H\e[K" << std::flush;

#ifndef WIN32
    enableRawMode();
#endif
}

static void stopMiningThreadFunc()
{
    // This runs in background thread - does the blocking join_all()
    GenerateBitcoins(false, 0, Params());
    miningStopInProgress.store(false);
    LogPrintf("Mining threads stopped\n");
}

// Toggle mining on/off (non-blocking when stopping)
static void toggleMining()
{
    bool currentlyMining = GetBoolArg("-gen", false);

    // If we're already stopping, don't do anything
    if (miningStopInProgress.load()) {
        return;
    }

    mapArgs["-gen"] = currentlyMining ? "0" : "1";

    if (!currentlyMining) {
        // Starting mining - this is fast (just spawns threads)
        int nThreads = GetArg("-genproclimit", 1);
        GenerateBitcoins(true, nThreads, Params());
        SetMiningStartTime();  // Track start time for warmup display
        LogPrintf("User enabled mining with %d threads\n", nThreads);
    } else {
        // Stopping mining - do it async to avoid blocking TUI
        miningStopInProgress.store(true);
        miningStartTime = 0;  // Clear start time when mining stops
        LogPrintf("User disabled mining (stopping threads in background)\n");
        std::thread stopThread(stopMiningThreadFunc);
        stopThread.detach();  // Let it run in background
    }
}

// Toggle Fast Mode on/off (turns off Light Mode)
static void toggleFastMode()
{
    bool currentlyMining = GetBoolArg("-gen", false);
    bool isFastMode = RandomX_IsFastMode();
    bool hugepagesInUse = RandomX_IsUsingHugepages();

    if (!currentlyMining) return;  // Only allow when mining

    if (isFastMode) {
        // Switching to Light Mode
        LogPrintf("User switching to Light Mode\n");

        // Record user's explicit choice so miner thread respects it
        mapArgs["-randomxfastmode"] = "0";

        // Stop mining
        GenerateBitcoins(false, 0, Params());
        MilliSleep(500);

        // Change mode
        RandomX_ChangeMode(false, false);

        // Reset counters and timer
        ehSolverRuns.value.store(0);
        solutionTargetChecks.value.store(0);
        miningTimer.zeroize();

        // Restart mining
        int nThreads = GetArg("-genproclimit", 1);
        GenerateBitcoins(true, nThreads, Params());
        SetMiningStartTime();
    } else {
        // Switching to Fast Mode
        LogPrintf("User switching to Fast Mode\n");

        // Record user's explicit choice so miner thread respects it
        mapArgs["-randomxfastmode"] = "1";

        // Stop mining
        GenerateBitcoins(false, 0, Params());
        MilliSleep(500);

        // Change mode (preserve hugepages setting)
        RandomX_ChangeMode(true, hugepagesInUse);

        // Reset counters and timer
        ehSolverRuns.value.store(0);
        solutionTargetChecks.value.store(0);
        miningTimer.zeroize();

        // Restart mining
        int nThreads = GetArg("-genproclimit", 1);
        GenerateBitcoins(true, nThreads, Params());
        SetMiningStartTime();
    }
}

// Toggle Light Mode on/off (turns off Fast Mode)
static void toggleLightMode()
{
    bool currentlyMining = GetBoolArg("-gen", false);
    bool isFastMode = RandomX_IsFastMode();

    if (!currentlyMining) return;  // Only allow when mining

    if (!isFastMode) {
        // Already in Light Mode, do nothing
        return;
    } else {
        // Switching to Light Mode
        LogPrintf("User switching to Light Mode\n");

        // Stop mining
        GenerateBitcoins(false, 0, Params());
        MilliSleep(500);

        // Change mode
        RandomX_ChangeMode(false, false);

        // Reset counters and timer
        ehSolverRuns.value.store(0);
        solutionTargetChecks.value.store(0);
        miningTimer.zeroize();

        // Restart mining
        int nThreads = GetArg("-genproclimit", 1);
        GenerateBitcoins(true, nThreads, Params());
        SetMiningStartTime();
    }
}

// Toggle Hugepages on/off (only works in Fast Mode)
static void toggleHugepages()
{
    bool currentlyMining = GetBoolArg("-gen", false);
    bool isFastMode = RandomX_IsFastMode();
    bool hugepagesInUse = RandomX_IsUsingHugepages();

    if (!currentlyMining) return;  // Only allow when mining

    if (!isFastMode) {
        // Hugepages only available in Fast Mode
        LogPrintf("Hugepages only available in Fast Mode\n");
        return;
    }

    LogPrintf("User toggling Hugepages: %s -> %s\n",
        hugepagesInUse ? "ON" : "OFF",
        !hugepagesInUse ? "ON" : "OFF");

    // Stop mining
    GenerateBitcoins(false, 0, Params());
    MilliSleep(500);

    // Change mode with toggled hugepages
    RandomX_ChangeMode(true, !hugepagesInUse);

    // Reset counters and timer
    ehSolverRuns.value.store(0);
    solutionTargetChecks.value.store(0);
    miningTimer.zeroize();

    // Restart mining
    int nThreads = GetArg("-genproclimit", 1);
    GenerateBitcoins(true, nThreads, Params());
    SetMiningStartTime();
}

// Store original thread count before benchmark
static std::atomic<int> benchmarkOriginalThreads(0);

// Write optimal thread count to config file
static bool writeOptimalThreadsToConfig(int threads, const std::string& mode = "")
{
    try {
        // Get config file path
        boost::filesystem::path configPath = GetConfigFile(GetArg("-conf", BITCOIN_CONF_FILENAME));

        // Read existing config file and filter out old benchmark settings
        std::vector<std::string> existingLines;
        std::ifstream configFileRead(configPath.string());
        
        // Helper to check if a line sets a specific config option (handles whitespace)
        auto isOption = [](const std::string& line, const std::string& option) -> bool {
            // Find start of key (skip whitespace)
            size_t keyStart = line.find_first_not_of(" \t");
            if (keyStart == std::string::npos) return false; // Empty or whitespace only

            // Check if comment
            if (line[keyStart] == '#') return false;

            // Check if key matches
            if (line.compare(keyStart, option.length(), option) != 0) return false;

            // Check what follows the key
            size_t afterKey = keyStart + option.length();
            if (afterKey >= line.length()) return false; // Key without value?

            char nextChar = line[afterKey];
            // Must be followed by '=' or whitespace then '='
            if (nextChar == '=') return true;
            if (nextChar == ' ' || nextChar == '\t') {
                size_t equalPos = line.find('=', afterKey);
                if (equalPos != std::string::npos) {
                    // Check if only whitespace between key and '='
                    for (size_t i = afterKey; i < equalPos; i++) {
                        if (line[i] != ' ' && line[i] != '\t') return false;
                    }
                    return true;
                }
            }
            return false;
        };

        if (configFileRead.is_open()) {
            std::string line;
            bool skipNextLines = false;
            while (std::getline(configFileRead, line)) {
                // Skip benchmark comment and subsequent settings
                if (line.find("# Optimal configuration determined by benchmark") != std::string::npos) {
                    skipNextLines = true;
                    continue;
                }

                // Skip old benchmark-related settings block
                if (skipNextLines) {
                    if (isOption(line, "randomxfastmode") ||
                        isOption(line, "randomxhugepages") ||
                        isOption(line, "randomxmsr") ||
                        isOption(line, "randomxcacheqos") ||
                        isOption(line, "randomxexceptionhandling") ||
                        isOption(line, "genproclimit") ||
                        isOption(line, "gen") ||
                        line.find("# Best mode:") != std::string::npos ||
                        line.empty()) {
                        continue;  // Skip these lines
                    }
                    skipNextLines = false;  // Resume keeping lines
                }

                // Also remove any standalone duplicates from earlier runs or manual edits
                if (isOption(line, "randomxfastmode") ||
                    isOption(line, "randomxhugepages") ||
                    isOption(line, "randomxmsr") ||
                    isOption(line, "randomxcacheqos") ||
                    isOption(line, "randomxexceptionhandling") ||
                    isOption(line, "genproclimit") ||
                    isOption(line, "gen")) {
                    continue;  // Skip duplicates
                }

                existingLines.push_back(line);
            }
            configFileRead.close();
        }

        // Write back the filtered config plus new benchmark settings
        std::ofstream configFile(configPath.string(), std::ios::trunc);
        if (!configFile.is_open()) {
            LogPrintf("ERROR: Could not open config file for writing: %s\n", configPath.string());
            return false;
        }

        // Write existing non-benchmark lines
        for (const auto& line : existingLines) {
            configFile << line << "\n";
        }

        // Write the optimal settings with timestamp
        configFile << "\n# Optimal configuration determined by benchmark on "
                   << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";

        if (!mode.empty()) {
            configFile << "# Best mode: " << mode << "\n";

            // Set RandomX mode flags based on best mode
            if (mode == "Fast" || mode == "Fast+Hugepages") {
                configFile << "randomxfastmode=1\n";
            } else {
                configFile << "randomxfastmode=0\n";
            }

            if (mode == "Fast+Hugepages") {
                configFile << "randomxhugepages=1\n";
            } else {
                configFile << "randomxhugepages=0\n";
            }

            // Add MSR and cache QoS optimizations (enabled by default)
            configFile << "randomxmsr=1\n";
            configFile << "randomxcacheqos=1\n";
            configFile << "randomxexceptionhandling=1\n";
        }

        configFile << "genproclimit=" << threads << "\n";
        configFile << "gen=1\n";  // Enable mining on startup
        configFile.close();

        LogPrintf("Wrote optimal config (mode=%s, threads=%d, gen=1) to: %s\n", mode, threads, configPath.string());
        return true;
    } catch (const std::exception& e) {
        LogPrintf("ERROR: Failed to write to config file: %s\n", e.what());
        return false;
    }
}

// Prompt user for which modes to benchmark
static bool promptBenchmarkModes(int screenHeight, bool& testLight, bool& testFast, bool& testHugepages)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen
    int inputRow = screenHeight - 2;

    // Clear input area and show prompt
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Benchmark modes - [A]ll [L]ight [F]ast [H]ugepages [FH]ast+Hugepages [C]ancel (default: A): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    // Convert to uppercase
    std::transform(input.begin(), input.end(), input.begin(), ::toupper);

    // Determine which modes to test
    testLight = false;
    testFast = false;
    testHugepages = false;

    if (input == "C") {
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CROSS << " Benchmark cancelled" << std::flush;
        MilliSleep(1000);
#ifndef WIN32
        enableRawMode();
#endif
        return false;
    }

    if (input.empty() || input == "A") {
        testLight = true;
        testFast = true;
        testHugepages = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test all modes: Light, Fast, Fast+Hugepages" << std::flush;
    } else if (input == "L") {
        testLight = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Light mode only" << std::flush;
    } else if (input == "F") {
        testFast = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Fast mode only" << std::flush;
    } else if (input == "H") {
        testHugepages = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Fast+Hugepages mode only" << std::flush;
    } else if (input == "FH") {
        testFast = true;
        testHugepages = true;
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Will test Fast and Fast+Hugepages modes" << std::flush;
    } else {
        // Invalid input, cancel
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CROSS << " Invalid choice, benchmark cancelled" << std::flush;
        MilliSleep(1000);
#ifndef WIN32
        enableRawMode();
#endif
        return false;
    }

    MilliSleep(1000);  // Brief pause to show the confirmation
    return true;
}

// Prompt user whether to auto-apply optimal threads after benchmark
static bool promptBenchmarkAutoApply(int screenHeight)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen (3 rows from bottom)
    int inputRow = screenHeight - 2;

    // Clear input area and show prompt
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Apply optimal thread count to junocashd.conf and start mining after benchmark? (Y/N): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    bool autoApply = false;
    if (!input.empty()) {
        char choice = std::toupper(input[0]);
        if (choice == 'Y') {
            autoApply = true;
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CHECK << " Will auto-apply optimal thread count after benchmark" << std::flush;
        } else {
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CHECK << " Benchmark results will be saved to file only" << std::flush;
        }
    } else {
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CHECK << " Benchmark results will be saved to file only" << std::flush;
    }

    // Brief pause to show confirmation
    MilliSleep(800);
    std::cout << "\e[" << inputRow << ";1H\e[K" << std::flush;

#ifndef WIN32
    enableRawMode();
#endif

    return autoApply;
}

// Toggle benchmark mode on/off
static void toggleBenchmark(int screenHeight)
{
    if (benchmarkMode.load()) {
        // Stop benchmark
        LogPrintf("Stopping benchmark mode\n");
        benchmarkMode = false;

        if (benchmarkThread != nullptr) {
            benchmarkThread->interrupt();
            benchmarkThread->join();
            delete benchmarkThread;
            benchmarkThread = nullptr;
        }

        // Restore original mining state (unless auto-apply is enabled)
        if (!benchmarkAutoApply.load()) {
            int originalThreads = benchmarkOriginalThreads.load();
            if (originalThreads > 0) {
                LogPrintf("Restoring mining with %d threads\n", originalThreads);
                mapArgs["-genproclimit"] = std::to_string(originalThreads);
                GenerateBitcoins(true, originalThreads, Params());
            }
        } else {
            LogPrintf("Auto-apply was enabled, keeping benchmark-determined thread count\n");
        }

        // Trigger screen refresh after stopping benchmark
        TriggerRefresh();
    } else {
        // Start benchmark
        bool mining = GetBoolArg("-gen", false);
        if (!mining) {
            LogPrintf("Benchmark requires mining to be enabled. Enable mining first with [M]\n");
            return;
        }

        // Ask user which modes to test
        bool testLight, testFast, testHugepages;
        if (!promptBenchmarkModes(screenHeight, testLight, testFast, testHugepages)) {
            return;
        }
        benchmarkTestLight = testLight;
        benchmarkTestFast = testFast;
        benchmarkTestHugepages = testHugepages;

        // Ask user if they want to auto-apply results
        bool autoApply = promptBenchmarkAutoApply(screenHeight);
        benchmarkAutoApply = autoApply;

        LogPrintf("Starting benchmark mode (auto-apply: %s)\n", autoApply ? "yes" : "no");

        // Save current thread count to restore later
        int currentThreads = GetArg("-genproclimit", boost::thread::hardware_concurrency());
        if (currentThreads == -1 || currentThreads == 0) {
            currentThreads = boost::thread::hardware_concurrency();
        }
        benchmarkOriginalThreads = currentThreads;

        // Stop current mining before benchmark starts
        LogPrintf("Stopping current mining to begin benchmark\n");
        GenerateBitcoins(false, 0, Params());

        // Determine max threads - always test all available threads
        // Do NOT use currentThreads here, as that's just what was configured
        int maxThreads = boost::thread::hardware_concurrency();
        LogPrintf("Benchmark will test 1 to %d threads (all available)\n", maxThreads);
        benchmarkMaxThreads = maxThreads;
        benchmarkMode = true;
        benchmarkResults.clear();

        // Start benchmark thread
        benchmarkThread = new boost::thread(&ThreadBenchmarkMining);
    }
}

// Prompt user for number of mining threads (with fixed screen positioning)
static void promptForThreads(int screenHeight)
{
#ifndef WIN32
    enableCanonicalMode();
#endif

    // Use dedicated input area at bottom of screen (3 rows from bottom)
    int inputRow = screenHeight - 2;

    // Clear input area
    std::cout << "\e[" << inputRow << ";1H\e[K";
    std::cout << "Enter number of mining threads (1-" << boost::thread::hardware_concurrency() << ", or -1 for all cores): " << std::flush;

    std::string input;
    std::getline(std::cin, input);

    try {
        int threads = std::stoi(input);
        int maxThreads = boost::thread::hardware_concurrency();

        if (threads == -1) {
            threads = maxThreads;
        }

        if (threads >= 1 && threads <= maxThreads) {
            mapArgs["-genproclimit"] = itostr(threads);

            // Restart mining with new thread count if currently mining
            bool currentlyMining = GetBoolArg("-gen", false);
            if (currentlyMining) {
                // Stop mining first
                GenerateBitcoins(false, 0, Params());
                MilliSleep(500);  // Wait for threads to stop

                // Reset counters and timer for accurate hashrate with new thread count
                ehSolverRuns.value.store(0);
                solutionTargetChecks.value.store(0);
                miningTimer.zeroize();

                // Restart mining with new thread count
                GenerateBitcoins(true, threads, Params());
                SetMiningStartTime();  // Track start time for warmup display
                LogPrintf("User set mining threads to %d (mining restarted, counters reset)\n", threads);
            } else {
                LogPrintf("User set mining threads to %d (will apply when mining starts)\n", threads);
            }
            std::cout << "\e[" << inputRow << ";1H\e[K";  // Clear and reposition
            std::cout << SYMBOL_CHECK << " Mining threads set to " << threads << std::flush;
        } else {
            std::cout << "\e[" << inputRow << ";1H\e[K";
            std::cout << SYMBOL_CROSS << " Invalid thread count (must be 1-" << maxThreads << ")" << std::flush;
        }
    } catch (...) {
        std::cout << "\e[" << inputRow << ";1H\e[K";
        std::cout << SYMBOL_CROSS << " Invalid input (not a number)" << std::flush;
    }

    // Brief pause to show confirmation, then clear
    MilliSleep(800);
    std::cout << "\e[" << inputRow << ";1H\e[K" << std::flush;

#ifndef WIN32
    enableRawMode();
#endif
}

// Get system information for benchmarking
static std::string GetCPUInfo()
{
#ifndef WIN32
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    std::string model_name = "Unknown";
    std::string vendor_id = "Unknown";

    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                model_name = line.substr(pos + 2);
            }
        } else if (line.find("vendor_id") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                vendor_id = line.substr(pos + 2);
            }
        }
        if (model_name != "Unknown" && vendor_id != "Unknown") {
            break;
        }
    }
    return vendor_id + " - " + model_name;
#else
    return "Windows CPU";
#endif
}

static std::string GetCPUVendor()
{
#ifndef WIN32
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;

    while (std::getline(cpuinfo, line)) {
        if (line.find("vendor_id") != std::string::npos) {
            size_t pos = line.find(":");
            if (pos != std::string::npos) {
                std::string vendor = line.substr(pos + 2);
                // Clean up vendor string for filename
                vendor.erase(std::remove_if(vendor.begin(), vendor.end(), ::isspace), vendor.end());
                return vendor;
            }
        }
    }
#endif
    return "Unknown";
}

static int GetCPUCores()
{
#ifndef WIN32
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    int cores = 0;

    while (std::getline(cpuinfo, line)) {
        if (line.find("processor") != std::string::npos && line.find(":") != std::string::npos) {
            cores++;
        }
    }

    return cores > 0 ? cores : 1;
#else
    return 1;
#endif
}

static std::string GetMotherboardName()
{
#ifndef WIN32
    // Try to read motherboard name from DMI
    std::string boardName = "Unknown";
    std::string boardVendor = "Unknown";

    // Try board name
    std::ifstream boardNameFile("/sys/devices/virtual/dmi/id/board_name");
    if (boardNameFile.is_open()) {
        std::getline(boardNameFile, boardName);
        // Remove trailing whitespace
        boardName.erase(boardName.find_last_not_of(" \n\r\t") + 1);
    }

    // Try board vendor
    std::ifstream boardVendorFile("/sys/devices/virtual/dmi/id/board_vendor");
    if (boardVendorFile.is_open()) {
        std::getline(boardVendorFile, boardVendor);
        // Remove trailing whitespace
        boardVendor.erase(boardVendor.find_last_not_of(" \n\r\t") + 1);
    }

    // Combine vendor and name if both available
    if (boardVendor != "Unknown" && boardName != "Unknown") {
        return boardVendor + " " + boardName;
    } else if (boardName != "Unknown") {
        return boardName;
    } else if (boardVendor != "Unknown") {
        return boardVendor;
    }

    return "Unknown";
#else
    return "Windows";
#endif
}

static std::string GetMotherboardNameForFilename()
{
#ifndef WIN32
    std::string mbName = GetMotherboardName();

    // Clean up for filename - remove special characters, keep only alphanumeric and dash
    mbName.erase(std::remove_if(mbName.begin(), mbName.end(),
        [](char c) { return !std::isalnum(c) && c != '-'; }), mbName.end());

    // Limit length
    if (mbName.length() > 30) {
        mbName = mbName.substr(0, 30);
    }

    return mbName;
#else
    return "Windows";
#endif
}

// Benchmark thread - automatically tests different thread counts
void ThreadBenchmarkMining()
{
    LogPrintf("Starting mining benchmark mode\n");

    // Get mode selection from global atomics (set by toggleBenchmark)
    bool testLight = benchmarkTestLight.load();
    bool testFast = benchmarkTestFast.load();
    bool testHugepages = benchmarkTestHugepages.load();

    // Get system information
    std::string cpuVendor = GetCPUVendor();
    std::string cpuModel = GetCPUModel();
    std::string cpuInfo = GetCPUInfo();
    std::string memInfo = GetDetailedMemoryInfo();  // Use detailed memory info for benchmarks
    std::string motherboardName = GetMotherboardName();
    std::string motherboardNameClean = GetMotherboardNameForFilename();
    int cpuCores = GetCPUCores();

    // Create filename with motherboard, CPU info and date
    std::string dateStr = DateTimeStrFormat("%Y%m%d-%H%M%S", GetTime());
    std::string filename = strprintf("benchmark-%s-%s-%s-%s.txt", motherboardNameClean, cpuVendor, cpuModel, dateStr);

    // Open benchmark log file
    boost::filesystem::path benchmarkLogPath = GetDataDir() / filename;
    std::ofstream benchmarkLog(benchmarkLogPath.string(), std::ios::out);

    if (!benchmarkLog.is_open()) {
        LogPrintf("ERROR: Could not open %s for writing\n", filename);
        return;
    }

    LogPrintf("Benchmark results will be saved to: %s\n", benchmarkLogPath.string());

    // Capture starting block height for benchmark duration tracking
    {
        LOCK(cs_main);
        if (chainActive.Tip()) {
            benchmarkStartHeight = chainActive.Tip()->nHeight;
        } else {
            benchmarkStartHeight = 0;
        }
    }

    // Get mining mode information
    bool fastMode = GetBoolArg("-randomxfastmode", false);
    bool hugepagesEnabled = IsHugepagesEnabled();
    std::string randomxMode = fastMode ? "Fast (2GB dataset)" : "Light (256MB dataset)";
    std::string hugepagesStatus = hugepagesEnabled ? "ENABLED" : "DISABLED";

    // Print configuration to console
    LogPrintf("===========================================\n");
    LogPrintf("Benchmark Configuration:\n");
    LogPrintf("  Motherboard: %s\n", motherboardName);
    LogPrintf("  CPU: %s\n", cpuInfo);
    LogPrintf("  CPU Cores: %d\n", cpuCores);
    LogPrintf("  Thread Range: 1 to %d\n", benchmarkMaxThreads.load());
    LogPrintf("  Duration per test: %d seconds (+ 2s warmup)\n", BENCHMARK_DURATION_SECONDS);
    LogPrintf("  RandomX Mode: %s\n", randomxMode);
    LogPrintf("  Hugepages: %s\n", hugepagesStatus);
    if (!hugepagesEnabled) {
        LogPrintf("  WARNING: Hugepages disabled - performance may be reduced by 5-10%%\n");
    }
    if (!fastMode) {
        LogPrintf("  INFO: Using Light mode - consider -randomxfastmode for ~30%% better performance\n");
    }
    LogPrintf("===========================================\n");

    // Reset hash counters for accurate benchmarking
    LogPrintf("Resetting hash counters for accurate measurements\n");
    ehSolverRuns.value.store(0);
    solutionTargetChecks.value.store(0);
    miningTimer.zeroize();

    // Write header with system information
    benchmarkLog << "========================================\n";
    benchmarkLog << "JunoCash Mining Benchmark\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Date: " << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";
    benchmarkLog << "Version: " << FormatFullVersion() << "\n";
    benchmarkLog << "Network: " << Params().NetworkIDString() << "\n";
    benchmarkLog << "\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "System Configuration\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Motherboard: " << motherboardName << "\n";
    benchmarkLog << "CPU: " << cpuInfo << "\n";
    benchmarkLog << "CPU Cores: " << cpuCores << "\n";
    benchmarkLog << "Memory: " << memInfo << "\n";
    benchmarkLog << "\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Benchmark Parameters\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Thread Count Range: 1 to " << benchmarkMaxThreads.load() << "\n";
    benchmarkLog << "Duration per test: " << BENCHMARK_DURATION_SECONDS << " seconds (+ 2s warmup per test)\n";
    benchmarkLog << "RandomX Mode: " << randomxMode << "\n";
    benchmarkLog << "Hugepages: " << hugepagesStatus << "\n";
    if (!hugepagesEnabled) {
        benchmarkLog << "  Note: Hugepages disabled - hashrate may be 5-10% lower than with hugepages\n";
    }
    if (!fastMode) {
        benchmarkLog << "  Note: Light mode - Fast mode (-randomxfastmode) provides ~30% better performance\n";
    }
    benchmarkLog << "\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << "Results\n";
    benchmarkLog << "========================================\n";
    benchmarkLog << std::flush;

    // Define the 3 modes to test
    struct TestMode {
        std::string name;
        bool fastMode;
        bool hugepages;
    };

    std::vector<TestMode> modes;
    if (testLight) {
        modes.push_back({"Light", false, false});
    }
    if (testFast) {
        modes.push_back({"Fast", true, false});
    }
    if (testHugepages) {
        modes.push_back({"Fast+Hugepages", true, true});
    }

    try {
        // Test each mode
        for (const auto& mode : modes) {
            LogPrintf("\n========================================\n");
            LogPrintf("Testing Mode: %s\n", mode.name);
            LogPrintf("========================================\n");

            benchmarkLog << "\n========================================\n";
            benchmarkLog << "Mode: " << mode.name << "\n";
            benchmarkLog << "========================================\n";
            benchmarkLog << std::flush;

            // Change RandomX mode without shutting down
            // Mining threads will automatically recreate their VMs with new settings
            LogPrintf("Switching to %s mode...\n", mode.name);
            RandomX_ChangeMode(mode.fastMode, mode.hugepages);

            // Test each thread count in this mode
            for (int threadCount = 1; threadCount <= benchmarkMaxThreads.load(); threadCount++) {
                benchmarkCurrentThreads = threadCount;

                LogPrintf("Benchmark: Testing %s mode with %d thread(s)...\n", mode.name, threadCount);
                benchmarkLog << "\n  Thread Count: " << threadCount << "\n";
                benchmarkLog << std::flush;

                // Stop mining before changing thread count
                GenerateBitcoins(false, 0, Params());
                MilliSleep(500);  // Wait for threads to stop

                // Reset hash counters and timer for this thread count test
                ehSolverRuns.value.store(0);
                solutionTargetChecks.value.store(0);
                miningTimer.zeroize();

                // Start mining with new thread count
                GenerateBitcoins(true, threadCount, Params());

                // Reset benchmark stats for this test
                benchmarkAccumulatedHashrate = 0.0;
                benchmarkSampleCount = 0;
                benchmarkStartTime = GetTime();

                // Wait for mining to produce first hashes (RandomX can be slow to start)
                benchmarkWarmingUp = true;
                int64_t warmupTimeout = GetTime() + 10;  // Reduced from 30s to 10s
                while (GetTime() < warmupTimeout) {
                    MilliSleep(1000);
                    double warmupHashrate = GetLocalSolPS();

                    if (warmupHashrate > 0) {
                        LogPrintf("Mining active, hashrate detected: %.2f H/s\n", warmupHashrate);
                        break;
                    }
                    boost::this_thread::interruption_point();
                }
                benchmarkWarmingUp = false;

                if (GetLocalSolPS() == 0) {
                    LogPrintf("WARNING: No hashrate detected after warmup period\n");
                }

                // Mine for the specified duration and collect samples
                int64_t endTime = GetTime() + BENCHMARK_DURATION_SECONDS;
                while (GetTime() < endTime) {
                    boost::this_thread::interruption_point();

                    // Sample hashrate every second
                    MilliSleep(1000);

                    double currentHashrate = GetLocalSolPS();
                    if (currentHashrate > 0) {
                        benchmarkAccumulatedHashrate = benchmarkAccumulatedHashrate.load() + currentHashrate;
                        benchmarkSampleCount++;
                    }
                }

                // Calculate average
                double avgHashrate = 0.0;
                if (benchmarkSampleCount > 0) {
                    avgHashrate = benchmarkAccumulatedHashrate.load() / benchmarkSampleCount.load();
                }

                // Log results
                LogPrintf("Benchmark: %s mode, %d thread(s) = %.2f H/s (avg over %d samples)\n",
                         mode.name, threadCount, avgHashrate, benchmarkSampleCount.load());

                benchmarkLog << "    Average Hashrate: " << std::fixed << std::setprecision(2) << avgHashrate << " H/s\n";
                benchmarkLog << "    Samples: " << benchmarkSampleCount.load() << "\n";
                benchmarkLog << std::flush;

                // Store result
                BenchmarkResult result;
                result.threads = threadCount;
                result.mode = mode.name;
                result.hashrate = avgHashrate;
                result.samples = benchmarkSampleCount.load();
                benchmarkResults.push_back(result);
            }
        }

        // Find best configuration overall
        int bestThreads = 1;
        std::string bestMode = "Light";
        double bestHashrate = 0.0;
        for (const auto& result : benchmarkResults) {
            if (result.hashrate > bestHashrate) {
                bestHashrate = result.hashrate;
                bestThreads = result.threads;
                bestMode = result.mode;
            }
        }

        // Write summary
        benchmarkLog << "\n========================================\n";
        benchmarkLog << "Summary\n";
        benchmarkLog << "========================================\n";
        benchmarkLog << "Best Configuration: " << bestMode << " mode, " << bestThreads << " threads @ " << std::fixed << std::setprecision(2) << bestHashrate << " H/s\n";

        // Group results by mode
        benchmarkLog << "\nHashrate by mode and thread count:\n";
        for (const auto& testMode : modes) {
            benchmarkLog << "\n  " << testMode.name << " Mode:\n";
            for (const auto& result : benchmarkResults) {
                if (result.mode == testMode.name) {
                    benchmarkLog << "    " << result.threads << " threads: " << std::fixed << std::setprecision(2) << result.hashrate << " H/s\n";
                }
            }
        }

        // Write footer
        benchmarkLog << "\n========================================\n";
        benchmarkLog << "Benchmark Complete\n";
        benchmarkLog << "Finished: " << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";
        benchmarkLog << "========================================\n";
        benchmarkLog << std::flush;

        // Get ending block height
        int endHeight = 0;
        {
            LOCK(cs_main);
            if (chainActive.Tip()) {
                endHeight = chainActive.Tip()->nHeight;
            }
        }

        LogPrintf("Benchmark complete! Best: %s mode, %d threads @ %.2f H/s\n", bestMode, bestThreads, bestHashrate);
        LogPrintf("Benchmark ran from block height %d to %d\n", benchmarkStartHeight, endHeight);
        LogPrintf("Results saved to %s\n", benchmarkLogPath.string());

        benchmarkLog << "Starting block height: " << benchmarkStartHeight << "\n";
        benchmarkLog << "Ending block height: " << endHeight << "\n";

        // Apply optimal configuration if user requested
        if (benchmarkAutoApply.load() && bestThreads > 0) {
            LogPrintf("Auto-applying optimal configuration: %s mode, %d threads\n", bestMode, bestThreads);

            // Write to config file
            if (writeOptimalThreadsToConfig(bestThreads, bestMode)) {
                // Update runtime settings
                mapArgs["-genproclimit"] = std::to_string(bestThreads);

                // Set RandomX mode flags
                if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxfastmode"] = "1";
                } else {
                    mapArgs["-randomxfastmode"] = "0";
                }

                if (bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxhugepages"] = "1";
                } else {
                    mapArgs["-randomxhugepages"] = "0";
                }

                // Reinitialize RandomX with best mode
                GenerateBitcoins(false, 0, Params());  // Stop mining first
                RandomX_Shutdown();
                RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                            bestMode == "Fast+Hugepages");

                // Exit benchmark mode BEFORE starting normal mining
                benchmarkMode = false;

                // Enable mining flag
                mapArgs["-gen"] = "1";

                // Start mining with optimal configuration
                LogPrintf("Starting mining with optimal configuration\n");
                GenerateBitcoins(true, bestThreads, Params());
                SetMiningStartTime();  // Track start time for warmup display

                benchmarkLog << "\nAuto-applied optimal settings:\n";
                benchmarkLog << "  - Updated junocashd.conf with best mode: " << bestMode << "\n";
                benchmarkLog << "  - Updated junocashd.conf with genproclimit=" << bestThreads << "\n";
                benchmarkLog << "  - Started mining with " << bestMode << " mode, " << bestThreads << " threads\n";
                benchmarkLog << std::flush;
            } else {
                LogPrintf("WARNING: Failed to write to config file, but starting mining anyway\n");

                // Update runtime settings
                mapArgs["-genproclimit"] = std::to_string(bestThreads);
                if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxfastmode"] = "1";
                } else {
                    mapArgs["-randomxfastmode"] = "0";
                }
                if (bestMode == "Fast+Hugepages") {
                    mapArgs["-randomxhugepages"] = "1";
                } else {
                    mapArgs["-randomxhugepages"] = "0";
                }

                // Reinitialize and start mining
                GenerateBitcoins(false, 0, Params());
                RandomX_Shutdown();
                RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                            bestMode == "Fast+Hugepages");

                // Exit benchmark mode BEFORE starting normal mining
                benchmarkMode = false;

                // Enable mining flag
                mapArgs["-gen"] = "1";

                GenerateBitcoins(true, bestThreads, Params());
                SetMiningStartTime();  // Track start time for warmup display
            }
        }

    } catch (boost::thread_interrupted&) {
        // User stopped benchmark early - still write summary
        benchmarkLog << "\n========================================\n";
        benchmarkLog << "Benchmark Interrupted by User\n";
        benchmarkLog << "========================================\n";

        if (!benchmarkResults.empty()) {
            // Find best from partial results
            int bestThreads = 1;
            std::string bestMode = "Light";
            double bestHashrate = 0.0;
            for (const auto& result : benchmarkResults) {
                if (result.hashrate > bestHashrate) {
                    bestHashrate = result.hashrate;
                    bestThreads = result.threads;
                    bestMode = result.mode;
                }
            }

            benchmarkLog << "Partial Results (tested " << benchmarkResults.size() << " configurations):\n";
            benchmarkLog << "Best so far: " << bestMode << " mode, " << bestThreads << " threads @ " << std::fixed << std::setprecision(2) << bestHashrate << " H/s\n\n";

            // Group partial results by mode
            benchmarkLog << "Results by mode:\n";
            std::set<std::string> testedModes;
            for (const auto& result : benchmarkResults) {
                testedModes.insert(result.mode);
            }
            for (const auto& modeName : testedModes) {
                benchmarkLog << "  " << modeName << " Mode:\n";
                for (const auto& result : benchmarkResults) {
                    if (result.mode == modeName) {
                        benchmarkLog << "    " << result.threads << " threads: " << std::fixed << std::setprecision(2) << result.hashrate << " H/s\n";
                    }
                }
            }

            // Get ending block height
            int endHeight = 0;
            {
                LOCK(cs_main);
                if (chainActive.Tip()) {
                    endHeight = chainActive.Tip()->nHeight;
                }
            }

            LogPrintf("Benchmark interrupted. Partial results: Best %s mode, %d threads @ %.2f H/s\n", bestMode, bestThreads, bestHashrate);
            LogPrintf("Benchmark ran from block height %d to %d (interrupted)\n", benchmarkStartHeight, endHeight);

            benchmarkLog << "Starting block height: " << benchmarkStartHeight << "\n";
            benchmarkLog << "Ending block height: " << endHeight << " (interrupted)\n";

            // Apply optimal configuration if user requested (even with partial results)
            if (benchmarkAutoApply.load() && bestThreads > 0) {
                LogPrintf("Auto-applying best partial result: %s mode, %d threads\n", bestMode, bestThreads);

                // Write to config file
                if (writeOptimalThreadsToConfig(bestThreads, bestMode)) {
                    // Update runtime settings
                    mapArgs["-genproclimit"] = std::to_string(bestThreads);
                    if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxfastmode"] = "1";
                    } else {
                        mapArgs["-randomxfastmode"] = "0";
                    }
                    if (bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxhugepages"] = "1";
                    } else {
                        mapArgs["-randomxhugepages"] = "0";
                    }

                    // Reinitialize RandomX and start mining
                    GenerateBitcoins(false, 0, Params());
                    RandomX_Shutdown();
                    RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                                bestMode == "Fast+Hugepages");

                    // Exit benchmark mode BEFORE starting normal mining
                    benchmarkMode = false;

                    // Enable mining flag
                    mapArgs["-gen"] = "1";

                    LogPrintf("Starting mining with %s mode, %d threads (from partial results)\n", bestMode, bestThreads);
                    GenerateBitcoins(true, bestThreads, Params());
                    SetMiningStartTime();  // Track start time for warmup display

                    benchmarkLog << "\nAuto-applied partial results:\n";
                    benchmarkLog << "  - Updated junocashd.conf with best mode: " << bestMode << "\n";
                    benchmarkLog << "  - Updated junocashd.conf with genproclimit=" << bestThreads << "\n";
                    benchmarkLog << "  - Started mining with " << bestMode << " mode, " << bestThreads << " threads\n";
                    benchmarkLog << std::flush;
                } else {
                    LogPrintf("WARNING: Failed to write to config file, but starting mining anyway\n");
                    mapArgs["-genproclimit"] = std::to_string(bestThreads);
                    if (bestMode == "Fast" || bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxfastmode"] = "1";
                    } else {
                        mapArgs["-randomxfastmode"] = "0";
                    }
                    if (bestMode == "Fast+Hugepages") {
                        mapArgs["-randomxhugepages"] = "1";
                    } else {
                        mapArgs["-randomxhugepages"] = "0";
                    }
                    GenerateBitcoins(false, 0, Params());
                    RandomX_Shutdown();
                    RandomX_Init(bestMode == "Fast" || bestMode == "Fast+Hugepages",
                                bestMode == "Fast+Hugepages");

                    // Exit benchmark mode BEFORE starting normal mining
                    benchmarkMode = false;

                    // Enable mining flag
                    mapArgs["-gen"] = "1";

                    GenerateBitcoins(true, bestThreads, Params());
                    SetMiningStartTime();  // Track start time for warmup display
                }
            }
        } else {
            benchmarkLog << "No results collected yet\n";
            LogPrintf("Benchmark interrupted with no results\n");
        }

        benchmarkLog << "\nFinished: " << DateTimeStrFormat("%Y-%m-%d %H:%M:%S", GetTime()) << "\n";
        benchmarkLog << "========================================\n";
        benchmarkLog << std::flush;
    }

    // Stop mining before exiting benchmark (unless auto-apply started mining)
    if (!benchmarkAutoApply.load()) {
        LogPrintf("Benchmark ending, stopping mining\n");
        GenerateBitcoins(false, 0, Params());
    } else {
        LogPrintf("Benchmark ending, mining continues with optimal threads\n");

        // Reset counters and timer for accurate hashrate display after benchmark
        LogPrintf("Resetting hashrate counters for post-benchmark mining\n");
        MilliSleep(500);  // Wait for mining to stabilize
        ehSolverRuns.value.store(0);
        solutionTargetChecks.value.store(0);
        miningTimer.zeroize();
    }

    benchmarkLog.close();
    benchmarkMode = false;

    // Trigger screen refresh to clear benchmark status
    TriggerRefresh();
}

void ThreadShowMetricsScreen()
{
    // Determine whether we should render a persistent UI or rolling metrics
    bool isTTY = isatty(STDOUT_FILENO);
    bool isScreen = GetBoolArg("-metricsui", isTTY);
    int64_t nRefresh = GetArg("-metricsrefreshtime", isTTY ? 1 : 600);

    // Track when we last did a full screen clear to prevent artifact accumulation
    // Start at -INTERVAL to force full clear on first frame
    const int64_t FULL_CLEAR_INTERVAL = 60; // Full clear every 60 seconds
    int64_t nLastFullClear = GetTime() - FULL_CLEAR_INTERVAL;

    // Header is 6 lines: box top + 3 centered lines + box bottom + blank line
    if (isScreen) {
#ifdef WIN32
        enableVTMode();
#else
        // Enable raw mode for non-blocking keyboard input
        if (isTTY) {
            enableRawMode();
        }
#endif
    }

    while (true) {
        // Number of lines that are always displayed
        int lines = 0;
        int cols = 80;
        int rows = 24;  // Default terminal height

        // Get current window size
        if (isTTY) {
#ifdef WIN32
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi) != 0) {
                cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
                rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            }
#else
            struct winsize w;
            w.ws_col = 0;
            w.ws_row = 0;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1) {
                if (w.ws_col != 0) cols = w.ws_col;
                if (w.ws_row != 0) rows = w.ws_row;
            }
#endif
        }

        // Lock and fetch stats before erasing the screen, in case we block.
        std::optional<MetricsStats> metricsStats;
        if (loaded) {
            metricsStats = loadStats();
        }

        if (isScreen) {
            // Periodically do a full screen clear to prevent artifact accumulation
            int64_t nNow = GetTime();
            bool doFullClear = forceFullClear || (nNow - nLastFullClear >= FULL_CLEAR_INTERVAL);
            if (doFullClear) {
                nLastFullClear = nNow;
                forceFullClear = false;
                // Full clear: hide cursor, clear screen, move to home
                std::cout << "\e[?25l\e[2J\e[H" << std::flush;
            } else {
                // Normal update: hide cursor, move to home (no clear - reduces flicker)
                std::cout << "\e[?25l\e[H" << std::flush;
            }

            // Draw header every frame
            drawBoxTop("");
            drawCentered("Juno Cash", "\e[1;33m");
            drawCentered("Private Money", "\e[1;36m");
            drawCentered(FormatFullVersion() + " - " + WhichNetwork() + " - RandomX", "\e[0;37m");
            drawBoxBottom();
            std::cout << std::endl;
        }

        // Miner status
#ifdef ENABLE_MINING
        bool mining = GetBoolArg("-gen", false);
#else
        bool mining = false;
#endif

        if (loaded) {
            lines += printStats(metricsStats.value(), isScreen, mining);
            lines += printWalletStatus();
            lines += printMiningStatus(mining);
        }
        lines += printMetrics(cols, mining);
        lines += printMessageBox(cols);
        lines += printInitMessage();

        if (isScreen) {
            // Footer hint (no newline - avoid scrolling)
            std::cout << "[" << _("Set 'showmetrics=0' to hide") << "]";
            // Clear from cursor to end of screen (removes old content) and show cursor
            std::cout << "\e[J\e[?25h" << std::flush;
            lines++; // Count the footer line
        } else {
            // Print delineator
            std::cout << "----------------------------------------" << std::endl;
        }

        *nNextRefresh = GetTime() + nRefresh;
        while (GetTime() < *nNextRefresh) {
            boost::this_thread::interruption_point();

            // Check for keyboard input
            if (isScreen && isTTY) {
                int key = checkKeyPress();
                if (key == 'Q' || key == 'q') {
                    // Wait for mining stop to complete before shutdown
                    if (miningStopInProgress.load()) {
                        std::cout << std::endl << "Waiting for mining threads to stop..." << std::flush;
                        while (miningStopInProgress.load()) {
                            MilliSleep(100);
                        }
                        std::cout << " done." << std::endl;
                    }
                    // Quit the daemon gracefully
                    std::cout << std::endl << "Shutting down, please wait..." << std::endl << std::endl;
                    StartShutdown();
                    return;
                } else if (key == 'M' || key == 'm') {
                    toggleMining();
                    forceFullClear = true;
                    break;
                } else if (key == 'T' || key == 't') {
                    // Only allow changing threads if mining or on non-main network
                    if (mining || Params().NetworkIDString() != "main") {
                        promptForThreads(rows);
                        forceFullClear = true;
                        break;
                    }
                } else if (key == 'B' || key == 'b') {
                    // Toggle benchmark mode
                    if (mining) {
                        toggleBenchmark(rows);
                        forceFullClear = true;
                        break;
                    }
                } else if (key == 'A' || key == 'a') {
                    toggleAddressExpansion();
                    break;
                } else if (key == 'S' || key == 's') {
                    // Only process if there are shieldable coins or shielding in progress
                    if (hasShieldableCoins.load() || shieldingInProgress.load()) {
                        shieldCoinbase(rows);
                        forceFullClear = true;
                        break;
                    }
                } else if (key == ' ') {
                    forceFullClear = true;
                    break;
                } else if (mining) {
                    // Mining mode controls only available when mining
                    if (key == 'R' || key == 'r') {
                        toggleFastMode();  // Toggles between LIGHT and FAST
                        forceFullClear = true;
                        break;
                    } else if (key == 'H' || key == 'h') {
                        toggleHugepages();
                        forceFullClear = true;
                        break;
                    } else if (key == 'D' || key == 'd') {
                        toggleDonation();
                        forceFullClear = true;
                        break;
                    } else if (key == 'P' || key == 'p') {
                        // Only allow changing percentage if donations are active
                        int currentPct = getCurrentDonationPercentage();
                        if (currentPct > 0) {
                            promptForPercentage(rows);
                            forceFullClear = true;
                            break;
                        }
                    }
                }
            }

            MilliSleep(100);
        }

        // Screen will be redrawn from home position at start of next loop
        // No need to reposition cursor
    }
}
