// Copyright (c) 2019-2023 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "params.h"

#include <amount.h>
#include <key_io.h>
#include <script/standard.h>
#include "upgrades.h"
#include "util/system.h"
#include "util/match.h"

namespace Consensus {
    /**
     * General information about each funding stream.
     * Ordered by Consensus::FundingStreamIndex.
     */
    constexpr struct FSInfo FundingStreamInfo[Consensus::MAX_FUNDING_STREAMS] = {
        {
            .recipient = "Electric Coin Company",
            .specification = "https://zips.z.cash/zip-0214",
            .valueNumerator = 7,
            .valueDenominator = 100,
        },
        {
            .recipient = "Zcash Foundation",
            .specification = "https://zips.z.cash/zip-0214",
            .valueNumerator = 5,
            .valueDenominator = 100,
        },
        {
            .recipient = "Major Grants",
            .specification = "https://zips.z.cash/zip-0214",
            .valueNumerator = 8,
            .valueDenominator = 100,
        },
        {
            .recipient = "Zcash Community Grants NU6",
            .specification = "https://zips.z.cash/zip-0214",
            .valueNumerator = 8,
            .valueDenominator = 100,
        },
        {
            .recipient = "Lockbox NU6",
            .specification = "https://zips.z.cash/zip-0214",
            .valueNumerator = 12,
            .valueDenominator = 100,
        },
        {
            .recipient = "Zcash Community Grants to third halving",
            .specification = "https://zips.z.cash/zip-0214",
            .valueNumerator = 8,
            .valueDenominator = 100,
        },
        {
            .recipient = "Coinholder-Controlled Fund to third halving",
            .specification = "https://zips.z.cash/zip-0214",
            .valueNumerator = 12,
            .valueDenominator = 100,
        }
    };
    static constexpr bool validateFundingStreamInfo(uint32_t idx) {
        return (idx >= Consensus::MAX_FUNDING_STREAMS || (
            FundingStreamInfo[idx].valueNumerator < FundingStreamInfo[idx].valueDenominator &&
            FundingStreamInfo[idx].valueNumerator < (INT64_MAX / MAX_MONEY) &&
            validateFundingStreamInfo(idx + 1)));
    }
    static_assert(
        validateFundingStreamInfo(Consensus::FIRST_FUNDING_STREAM),
        "Invalid FundingStreamInfo");

    std::optional<int> Params::GetActivationHeight(Consensus::UpgradeIndex idx) const {
        auto nActivationHeight = vUpgrades[idx].nActivationHeight;
        if (nActivationHeight == Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT) {
            return std::nullopt;
        } else {
            return nActivationHeight;
        }
    }

    bool Params::NetworkUpgradeActive(int nHeight, Consensus::UpgradeIndex idx) const {
        return NetworkUpgradeState(nHeight, *this, idx) == UPGRADE_ACTIVE;
    }

    int Params::HeightOfLatestSettledUpgrade() const {
        for (auto idxInt = Consensus::MAX_NETWORK_UPGRADES - 1; idxInt > Consensus::BASE_SPROUT; idxInt--) {
            if (vUpgrades[idxInt].hashActivationBlock.has_value()) {
                return vUpgrades[idxInt].nActivationHeight;
            }
        }
        return 0;
    }

    bool Params::FeatureRequired(const Consensus::ConsensusFeature feature) const {
        return vRequiredFeatures.count(feature) > 0;
    }

    bool Params::FeatureActive(const int nHeight, const Consensus::ConsensusFeature feature) const {
        return Features.FeatureActive(*this, nHeight, feature);
    }

    bool Params::FutureTimestampSoftForkActive(int nHeight) const {
        return nHeight >= nFutureTimestampSoftForkHeight;
    }

    bool Params::TemporaryOrchardDisablingSoftForkActive(int nHeight) const {
        return
            nTemporaryOrchardDisablingSoftForkHeight != Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT &&
            nHeight >= nTemporaryOrchardDisablingSoftForkHeight &&
            !NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU6_2);
    }

    int Params::Halving(int nHeight) const {
        // Simplified halving calculation for new emission curve
        // - Blocks 0-120,000: Pre-halving phase (slow start + plateau)
        // - Blocks 120,001-1,171,201: Initial halving epoch (6.25 COIN) - counts as halving 0
        // - Blocks 1,171,202+: Standard halving epochs (every 2,102,400 blocks)

        if (nHeight <= 120000) {
            return 0;  // Pre-halving phase
        }

        if (nHeight <= 1171201) {
            return 1;  // Initial halving epoch (6.25 COIN)
        }

        // Standard halvings: starting from halving index 2
        return 2 + ((nHeight - 1171201) / 2102400);
    }

    /**
     * This method determines the block height of the `halvingIndex`th
     * halving, as known at the specified `nHeight` block height.
     *
     * Simplified for new emission curve.
     */
    int Params::HalvingHeight(int nHeight, int halvingIndex) const {
        assert(nHeight >= 0);
        assert(halvingIndex > 0);

        // Emission curve halving heights:
        // Halving 1: Block 120,001 (first halving to 6.25 COIN)
        // Halving 2: Block 1,171,202 (to 3.125 COIN)
        // Halving 3+: Every 2,102,400 blocks after that

        if (halvingIndex == 1) {
            return 120001;
        }

        // Standard halvings starting from index 2
        return 1171202 + ((halvingIndex - 2) * 2102400);
    }

    int Params::GetLastFoundersRewardBlockHeight(int nHeight) const {
        // No founders reward (0% dev tax)
        return 0;
    }

    int Params::FundingPeriodIndex(int fundingStreamStartHeight, int nHeight) const {
        // Juno Cash: With all upgrades active from genesis, this assertion can fail during
        // early chain initialization. Return 0 if we're before the funding stream starts.
        if (fundingStreamStartHeight > nHeight) {
            return 0;
        }

        int firstHalvingHeight = HalvingHeight(fundingStreamStartHeight, 1);

        // If the start height of the funding period is not aligned to a multiple of the
        // funding period length, the first funding period will be shorter than the
        // funding period length.
        auto startPeriodOffset = (fundingStreamStartHeight - firstHalvingHeight) % nFundingPeriodLength;
        if (startPeriodOffset < 0) startPeriodOffset += nFundingPeriodLength; // C++ '%' is remainder, not modulus!

        return (nHeight - fundingStreamStartHeight + startPeriodOffset) / nFundingPeriodLength;
    }

    std::variant<FundingStream, FundingStreamError> FundingStream::ValidateFundingStream(
        const Consensus::Params& params,
        const int startHeight,
        const int endHeight,
        const std::vector<FundingStreamRecipient>& recipients
    ) {
        if (!params.NetworkUpgradeActive(startHeight, Consensus::UPGRADE_CANOPY)) {
            return FundingStreamError::CANOPY_NOT_ACTIVE;
        }

        if (endHeight < startHeight) {
            return FundingStreamError::ILLEGAL_RANGE;
        }

        const auto expectedRecipients = params.FundingPeriodIndex(startHeight, endHeight - 1) + 1;
        if (expectedRecipients > recipients.size()) {
            return FundingStreamError::INSUFFICIENT_RECIPIENTS;
        }

        // Lockbox output periods must not start before NU6
        if (!params.NetworkUpgradeActive(startHeight, Consensus::UPGRADE_NU6)) {
            for (auto recipient : recipients) {
                if (std::holds_alternative<Consensus::Lockbox>(recipient)) {
                    return FundingStreamError::NU6_NOT_ACTIVE;
                }
            }
        }

        return FundingStream(startHeight, endHeight, recipients);
    };

    class GetFundingStreamOrThrow {
    public:
        FundingStream operator()(const FundingStream& fs) const {
            return fs;
        }

        FundingStream operator()(const FundingStreamError& e) const {
            switch (e) {
                case FundingStreamError::CANOPY_NOT_ACTIVE:
                    throw std::runtime_error("Canopy network upgrade not active at funding stream start height.");
                case FundingStreamError::ILLEGAL_RANGE:
                    throw std::runtime_error("Illegal start/end height combination for funding stream.");
                case FundingStreamError::INSUFFICIENT_RECIPIENTS:
                    throw std::runtime_error("Insufficient recipient identifiers to fully exhaust funding stream.");
                case FundingStreamError::NU6_NOT_ACTIVE:
                    throw std::runtime_error("NU6 network upgrade not active at lockbox period start height.");
                default:
                    throw std::runtime_error("Unrecognized error validating funding stream.");
            };
        }
    };

    FundingStream FundingStream::ParseFundingStream(
        const Consensus::Params& params,
        const KeyConstants& keyConstants,
        const int startHeight,
        const int endHeight,
        const std::vector<std::string>& strAddresses,
        const bool allowDeferredPool)
    {
        KeyIO keyIO(keyConstants);

        // Parse the address strings into concrete types.
        std::vector<FundingStreamRecipient> recipients;
        for (const auto& strAddr : strAddresses) {
            if (allowDeferredPool && strAddr == "DEFERRED_POOL") {
                recipients.push_back(Lockbox());
                continue;
            }

            auto addr = keyIO.DecodePaymentAddress(strAddr);
            if (!addr.has_value()) {
                throw std::runtime_error("Funding stream address was not a valid " PACKAGE_NAME " address.");
            }

            examine(addr.value(), match {
                [&](const CKeyID& keyId) {
                    recipients.push_back(GetScriptForDestination(keyId));
                },
                [&](const CScriptID& scriptId) {
                    recipients.push_back(GetScriptForDestination(scriptId));
                },
                [&](const libzcash::SaplingPaymentAddress& zaddr) {
                    recipients.push_back(zaddr);
                },
                [&](const auto& zaddr) {
                    throw std::runtime_error("Funding stream address was not a valid transparent P2SH or Sapling address.");
                }
            });
        }

        auto validationResult = FundingStream::ValidateFundingStream(params, startHeight, endHeight, recipients);
        return std::visit(GetFundingStreamOrThrow(), validationResult);
    };

    OnetimeLockboxDisbursement OnetimeLockboxDisbursement::Parse(
        const Consensus::Params& params,
        const KeyConstants& keyConstants,
        const UpgradeIndex upgrade,
        const CAmount zatoshis,
        const std::string& strAddress)
    {
        KeyIO keyIO(keyConstants);

        if (upgrade < Consensus::UPGRADE_NU6_1) {
            throw std::runtime_error("Cannot define one-time lockbox disbursements prior to NU6.1.");
        }

        // Parse the address string into concrete types.
        auto addr = keyIO.DecodePaymentAddress(strAddress);
        if (!addr.has_value()) {
            throw std::runtime_error("One-time lockbox disbursement address was not a valid " PACKAGE_NAME " address.");
        }

        CScript recipient;
        examine(addr.value(), match {
            [&](const CScriptID& scriptId) {
                recipient = GetScriptForDestination(scriptId);
            },
            [&](const auto& zaddr) {
                throw std::runtime_error("One-time lockbox disbursement address was not a valid transparent P2SH address.");
            }
        });

        // TODO: Consider verifying that the set of (recipient, amount) tuples
        // are distinct from all possible funding stream tuples.
        return OnetimeLockboxDisbursement(upgrade, zatoshis, recipient);
    };

    void Params::AddZIP207FundingStream(
        const KeyConstants& keyConstants,
        FundingStreamIndex idx,
        int startHeight,
        int endHeight,
        const std::vector<std::string>& strAddresses)
    {
        vFundingStreams[idx] = FundingStream::ParseFundingStream(
                *this, keyConstants,
                startHeight, endHeight, strAddresses,
                false);
    };

    void Params::AddZIP207LockboxStream(
        const KeyConstants& keyConstants,
        FundingStreamIndex idx,
        int startHeight,
        int endHeight)
    {
        auto intervalCount = FundingPeriodIndex(startHeight, endHeight - 1) + 1;
        std::vector<FundingStreamRecipient> recipients(intervalCount, Lockbox());
        auto validationResult = FundingStream::ValidateFundingStream(
                *this,
                startHeight,
                endHeight,
                recipients);
        vFundingStreams[idx] = std::visit(GetFundingStreamOrThrow(), validationResult);
    };

    void Params::AddZIP271LockboxDisbursement(
        const KeyConstants& keyConstants,
        OnetimeLockboxDisbursementIndex idx,
        UpgradeIndex upgrade,
        CAmount zatoshis,
        const std::string& strAddress)
    {
        vOnetimeLockboxDisbursements[idx] = OnetimeLockboxDisbursement::Parse(
                *this, keyConstants,
                upgrade, zatoshis, strAddress);
    };

    CAmount Params::GetBlockSubsidy(int nHeight) const
    {
        // Emission schedule with ~21,000,000 JUNO maximum supply
        // Phase breakdown:
        // - Block 0: 0 coins (genesis block)
        // - Blocks 1-20,000: Slow start (0.25 -> 12.5 coins linear) = 127,500 JUNO
        // - Blocks 20,001-120,000: Plateau (12.5 coins constant) = 1,250,000 JUNO
        // - Blocks 120,001-1,171,200: Initial halving (6.25 coins) = 6,570,000 JUNO
        // - Blocks 1,171,201+: Standard halvings every 2,102,400 blocks
        //   - Epoch 0 (1,171,201-3,273,600): 3.125 coins (ends early at 3273599 due to integer division)
        //   - Epoch 1 (3,273,600-5,375,999): 1.5625 coins
        //   - Epoch 2 (5,376,002-7,478,401): 0.78125 coins
        //   - Epoch 3 (7,478,402-9,580,801): 0.390625 coins
        //   - Epoch 4 (9,580,802-11,683,201): 0.1953125 coins
        //   - Epoch 5 (11,683,202-13,785,601): 0.09765625 coins
        //   - Epoch 6 (13,785,602-15,888,001): 0.048828125 coins
        //   - Epoch 7 (15,888,002-16,508,927): 0.024414063 coins (partial, 620,926 blocks)
        // - After block 16,508,927: 0 coins (21M cap reached)
        //
        // Total supply: 20,999,999.98783572 JUNO (1,216,428 monetas short of 21M due to hard cutoff)

        static const int SLOW_START_INTERVAL = 20000;
        static const int PLATEAU_END = 120000;
        static const int INITIAL_HALVING_END = 1171200;
        static const int STANDARD_HALVING_INTERVAL = 2102400;
        static const int MAX_HEIGHT = 16508927;

        // Maximum supply enforcement - hard cap at 21M
        if (nHeight > MAX_HEIGHT) {
            return 0;
        }

        // Genesis block
        if (nHeight == 0) {
            return 0;
        }

        // Slow start: linear ramp from 0.25 to 12.5 COIN over 20,000 blocks
        // Formula: subsidy = 0.25 + (height - 1) * (12.25 / 19999)
        // In monetas: subsidy = 25000000 + ((height - 1) * 1225000000) / 19999
        if (nHeight <= SLOW_START_INTERVAL) {
            CAmount nSubsidy = 25000000LL + (((nHeight - 1) * 1225000000LL) / 19999);
            return nSubsidy;
        }

        // Plateau: constant 12.5 COIN for 100,000 blocks
        if (nHeight <= PLATEAU_END) {
            return 1250000000LL;  // 12.5 * COIN
        }

        // Initial halving epoch: 6.25 COIN for 1,051,200 blocks (120,001-1,171,200)
        if (nHeight <= INITIAL_HALVING_END) {
            return 625000000LL;  // 6.25 * COIN
        }

        // Standard halvings: starting from 3.125 COIN, halving every 2,102,400 blocks
        // Calculate which halving epoch we're in
        int halvings = (nHeight - INITIAL_HALVING_END) / STANDARD_HALVING_INTERVAL;
        CAmount nSubsidy = 312500000LL;  // 3.125 * COIN

        // Force block reward to zero when right shift is undefined
        if (halvings >= 64) {
            return 0;
        }

        nSubsidy >>= halvings;  // Right shift = divide by 2^halvings
        return nSubsidy;
    }

    std::vector<std::pair<FSInfo, FundingStream>> Params::GetActiveFundingStreams(int nHeight) const
    {
        std::vector<std::pair<FSInfo, FundingStream>> activeStreams;

        // Funding streams are disabled if Canopy is not active.
        if (NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
            for (uint32_t idx = Consensus::FIRST_FUNDING_STREAM; idx < Consensus::MAX_FUNDING_STREAMS; idx++) {
                // The following indexed access is safe as Consensus::MAX_FUNDING_STREAMS is used
                // in the definition of vFundingStreams.
                auto fs = vFundingStreams[idx];

                // Funding period is [startHeight, endHeight).
                if (fs && nHeight >= fs.value().GetStartHeight() && nHeight < fs.value().GetEndHeight()) {
                    activeStreams.push_back(std::make_pair(FundingStreamInfo[idx], fs.value()));
                }
            }
        }

        return activeStreams;
    };

    std::set<FundingStreamElement> Params::GetActiveFundingStreamElements(int nHeight) const
    {
        return GetActiveFundingStreamElements(nHeight, GetBlockSubsidy(nHeight));
    }

    std::set<FundingStreamElement> Params::GetActiveFundingStreamElements(
        int nHeight,
        CAmount blockSubsidy) const
    {
        std::set<std::pair<FundingStreamRecipient, CAmount>> requiredElements;

        // Funding streams are disabled if Canopy is not active.
        if (NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
            for (const auto& [fsinfo, fs] : GetActiveFundingStreams(nHeight)) {
                requiredElements.insert(std::make_pair(
                    fs.Recipient(*this, nHeight),
                    fsinfo.Value(blockSubsidy)));
            }
        }

        return requiredElements;
    };

    std::vector<OnetimeLockboxDisbursement> Params::GetLockboxDisbursementsForHeight(int nHeight) const
    {
        std::vector<OnetimeLockboxDisbursement> disbursements;

        // Disbursements are disabled if NU6.1 is not active.
        if (NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU6_1)) {
            for (uint32_t idx = Consensus::FIRST_ONETIME_LOCKBOX_DISBURSEMENT; idx < Consensus::MAX_ONETIME_LOCKBOX_DISBURSEMENTS; idx++) {
                // The following indexed access is safe as
                // Consensus::MAX_ONETIME_LOCKBOX_DISBURSEMENTS is used
                // in the definition of vOnetimeLockboxDisbursements.
                auto ld = vOnetimeLockboxDisbursements[idx];

                if (ld && GetActivationHeight(ld.value().GetUpgrade()) == nHeight) {
                    disbursements.push_back(ld.value());
                }
            }
        }

        return disbursements;
    };

    FundingStreamRecipient FundingStream::Recipient(const Consensus::Params& params, int nHeight) const
    {
        auto addressIndex = params.FundingPeriodIndex(startHeight, nHeight);

        assert(addressIndex >= 0 && addressIndex < recipients.size());
        return recipients[addressIndex];
    };

    int64_t Params::PoWTargetSpacing(int nHeight) const {
        // zip208
        // PoWTargetSpacing(height) :=
        // PreBlossomPoWTargetSpacing, if not IsBlossomActivated(height)
        // PostBlossomPoWTargetSpacing, otherwise.
        bool blossomActive = NetworkUpgradeActive(nHeight, Consensus::UPGRADE_BLOSSOM);
        return blossomActive ? nPostBlossomPowTargetSpacing : nPreBlossomPowTargetSpacing;
    }

    int64_t Params::AveragingWindowTimespan(int nHeight) const {
        return nPowAveragingWindow * PoWTargetSpacing(nHeight);
    }

    int64_t Params::MinActualTimespan(int nHeight) const {
        return (AveragingWindowTimespan(nHeight) * (100 - nPowMaxAdjustUp)) / 100;
    }

    int64_t Params::MaxActualTimespan(int nHeight) const {
        return (AveragingWindowTimespan(nHeight) * (100 + nPowMaxAdjustDown)) / 100;
    }
}
