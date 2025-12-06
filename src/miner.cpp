// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2016-2023 The Zcash developers
// Copyright (c) 2025 The Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "miner.h"
#ifdef ENABLE_MINING
// Juno Cash: Legacy Equihash - kept for reference
// #include "pow/tromp/equi_miner.h"
#include "crypto/randomx_wrapper.h"
#include "numa_helper.h"
#endif

#include "amount.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/funding.h"
#include "consensus/merkle.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#ifdef ENABLE_MINING
// Juno Cash: Legacy Equihash - kept for reference
// #include "crypto/equihash.h"
#endif
#include "hash.h"
#include "key_io.h"
#include "main.h"
#include "metrics.h"
#include "net.h"
#include "zcash/Note.hpp"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "random.h"
#include "timedata.h"
#include "transaction_builder.h"
#include "ui_interface.h"
#include "util/system.h"
#include "util/match.h"
#include "util/moneystr.h"
#include "validationinterface.h"
#include "zip317.h"

#include <librustzcash.h>
#include <rust/bridge.h>

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>
#ifdef ENABLE_MINING
#include <functional>
#endif
#include <mutex>
#include <queue>

using namespace std;

//////////////////////////////////////////////////////////////////////////////
//
// BitcoinMiner
//

//
// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest fee rate, so we might consider transactions
// that depend on transactions that aren't yet in the block.

std::optional<uint64_t> last_block_num_txs;
std::optional<uint64_t> last_block_size;

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    auto medianTimePast = pindexPrev->GetMedianTimePast();
    int64_t nNewTime = std::max(medianTimePast + 1, GetTime());
    // See the comment in ContextualCheckBlockHeader() for background.
    if (consensusParams.FutureTimestampSoftForkActive(pindexPrev->nHeight + 1)) {
        nNewTime = std::min(nNewTime, medianTimePast + MAX_FUTURE_BLOCK_TIME_MTP);
    }

    // The timestamp of a given block template should not go backwards.
    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.nPowAllowMinDifficultyBlocksAfterHeight != std::nullopt) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }

    return nNewTime - nOldTime;
}

bool IsShieldedMinerAddress(const MinerAddress& minerAddr) {
    return !std::holds_alternative<boost::shared_ptr<CReserveScript>>(minerAddr);
}

class AddOutputsToCoinbaseTxAndSign
{
private:
    CMutableTransaction &mtx;
    const CChainParams &chainparams;
    const int nHeight;
    const CAmount nFees;

public:
    AddOutputsToCoinbaseTxAndSign(
        CMutableTransaction &mtx,
        const CChainParams &chainparams,
        const int nHeight,
        const CAmount nFees) : mtx(mtx), chainparams(chainparams), nHeight(nHeight), nFees(nFees) {}

    const libzcash::Zip212Enabled GetZip212Flag() const {
        if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
            return libzcash::Zip212Enabled::AfterZip212;
        } else {
            return libzcash::Zip212Enabled::BeforeZip212;
        }
    }

    CAmount SetFoundersRewardAndGetMinerValue(sapling::Builder& saplingBuilder) const {
        const auto& consensus = chainparams.GetConsensus();
        const auto block_subsidy = consensus.GetBlockSubsidy(nHeight);
        auto miner_reward = block_subsidy; // founders' reward or funding stream amounts will be subtracted below

        if (nHeight > 0) {
            if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_CANOPY)) {
                LogPrint("pow", "%s: Constructing funding stream outputs for height %d", __func__, nHeight);
                for (const auto& [fsinfo, fs] : consensus.GetActiveFundingStreams(nHeight)) {
                    const auto amount = fsinfo.Value(block_subsidy);
                    miner_reward -= amount;

                    examine(fs.Recipient(consensus, nHeight), match {
                        [&](const libzcash::SaplingPaymentAddress& pa) {
                            LogPrint("pow", "%s: Adding Sapling funding stream output of value %d", __func__, amount);
                            saplingBuilder.add_recipient(
                                {},
                                pa.GetRawBytes(),
                                amount,
                                libzcash::Memo::ToBytes(std::nullopt));
                        },
                        [&](const CScript& scriptPubKey) {
                            LogPrint("pow", "%s: Adding transparent funding stream output of value %d", __func__, amount);
                            mtx.vout.emplace_back(amount, scriptPubKey);
                        },
                        [&](const Consensus::Lockbox& lockbox) {
                            LogPrint("pow", "%s: Noting lockbox output of value %d", __func__, amount);
                        }
                    });
                }
            } else if (nHeight <= chainparams.GetConsensus().GetLastFoundersRewardBlockHeight(nHeight)) {
                // Founders reward is 20% of the block subsidy
                const auto vFoundersReward = miner_reward / 5;
                // Take some reward away from us
                miner_reward -= vFoundersReward;
                // And give it to the founders
                mtx.vout.push_back(CTxOut(vFoundersReward, chainparams.GetFoundersRewardScriptAtHeight(nHeight)));
            } else {
                // Founders reward ends without replacement if Canopy is not activated by the
                // last Founders' Reward block height + 1.
            }

            if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU6_1)) {
                auto disbursements = consensus.GetLockboxDisbursementsForHeight(nHeight);
                if (!disbursements.empty()) {
                    LogPrint("pow", "%s: Constructing one-time lockbox disbursement outputs for height %d", __func__, nHeight);
                    for (const auto& disbursement : disbursements) {
                        LogPrint("pow", "%s: Adding transparent lockbox disbursement output of value %d",
                                 __func__, disbursement.GetAmount());
                        mtx.vout.emplace_back(disbursement.GetAmount(), disbursement.GetRecipient());
                    }
                }
            }
        }
        LogPrint("pow", "%s: Miner reward at height %d is %d", __func__, nHeight, miner_reward);

        return miner_reward + nFees;
    }

    void ComputeBindingSig(rust::Box<sapling::Builder> saplingBuilder, std::optional<orchard::UnauthorizedBundle> orchardBundle) const {
        auto consensusBranchId = CurrentEpochBranchId(nHeight, chainparams.GetConsensus());
        auto saplingBundle = sapling::build_bundle(std::move(saplingBuilder));

        // Empty output script.
        uint256 dataToBeSigned;
        try {
            if (mtx.fOverwintered) {
                // ProduceShieldedSignatureHash is only usable with v3+ transactions.
                dataToBeSigned = ProduceShieldedSignatureHash(
                    consensusBranchId,
                    mtx,
                    {},
                    *saplingBundle,
                    orchardBundle);
            } else {
                CScript scriptCode;
                PrecomputedTransactionData txdata(mtx, {});
                dataToBeSigned = SignatureHash(
                    scriptCode, mtx, NOT_AN_INPUT, SIGHASH_ALL, 0,
                    consensusBranchId,
                    txdata);
            }
        } catch (std::logic_error ex) {
            throw ex;
        }

        if (orchardBundle.has_value()) {
            auto authorizedBundle = orchardBundle.value().ProveAndSign({}, dataToBeSigned);
            if (authorizedBundle.has_value()) {
                mtx.orchardBundle = authorizedBundle.value();
            } else {
                throw new std::runtime_error("Failed to create Orchard proof or signatures");
            }
        }

        mtx.saplingBundle = sapling::apply_bundle_signatures(
            std::move(saplingBundle), dataToBeSigned.GetRawBytes());
    }

    // Create Orchard output
    void operator()(const libzcash::OrchardRawAddress &to) const {
        std::array<uint8_t, 32> saplingAnchor;
        auto saplingBuilder = sapling::new_builder(*chainparams.RustNetwork(), nHeight, saplingAnchor, true);

        // `enableSpends` must be set to `false` for coinbase transactions. This
        // means the Orchard anchor is unconstrained, so we set it to the empty
        // tree root via a null (all zeroes) uint256.
        uint256 orchardAnchor;
        auto builder = orchard::Builder(true, orchardAnchor);

        // Shielded coinbase outputs must be recoverable with an all-zeroes ovk.
        uint256 ovk;
        auto miner_reward = SetFoundersRewardAndGetMinerValue(*saplingBuilder);
        builder.AddOutput(ovk, to, miner_reward, std::nullopt);

        // orchard::Builder pads to two Actions, but does so using a "no OVK" policy for
        // dummy outputs, which violates coinbase rules requiring all shielded outputs to
        // be recoverable. We manually add a dummy output to sidestep this issue.
        // TODO: If/when we have funding streams going to Orchard recipients, this dummy
        // output can be removed.
        RawHDSeed rawSeed(32, 0);
        GetRandBytes(rawSeed.data(), 32);
        auto dummyTo = libzcash::OrchardSpendingKey::ForAccount(HDSeed(rawSeed), Params().BIP44CoinType(), 0)
            .ToFullViewingKey()
            .ToIncomingViewingKey()
            .Address(0);
        builder.AddOutput(ovk, dummyTo, 0, std::nullopt);

        auto bundle = builder.Build();
        if (!bundle.has_value()) {
            throw new std::runtime_error("Failed to create shielded output for miner");
        }

        ComputeBindingSig(std::move(saplingBuilder), std::move(bundle));
    }

    // Create Sapling output
    void operator()(const libzcash::SaplingPaymentAddress &pa) const {
        std::array<uint8_t, 32> saplingAnchor;
        auto saplingBuilder = sapling::new_builder(*chainparams.RustNetwork(), nHeight, saplingAnchor, true);

        auto miner_reward = SetFoundersRewardAndGetMinerValue(*saplingBuilder);

        saplingBuilder->add_recipient(
            {},
            pa.GetRawBytes(),
            miner_reward,
            libzcash::Memo::ToBytes(std::nullopt));

        ComputeBindingSig(std::move(saplingBuilder), std::nullopt);
    }

    // Create transparent output
    void operator()(const boost::shared_ptr<CReserveScript> &coinbaseScript) const {
        // Add the FR output and fetch the miner's output value.
        std::array<uint8_t, 32> saplingAnchor;
        auto saplingBuilder = sapling::new_builder(*chainparams.RustNetwork(), nHeight, saplingAnchor, true);

        // Miner output will be vout[0]; Founders' Reward & funding stream outputs
        // will follow.
        auto total_value = SetFoundersRewardAndGetMinerValue(*saplingBuilder);

        // Calculate donation if configured
        int donationPercent = GetArg("-donationpercentage", 0);
        std::string donationAddrStr = GetArg("-donationaddress", "");

        // Use default donation address if not overridden
        if (donationAddrStr.empty() && donationPercent > 0) {
            donationAddrStr = chainparams.GetDefaultDonationAddress();
        }

        CAmount donationAmount = 0;
        CAmount minerAmount = total_value;
        CScript donationScript;

        if (donationPercent > 0 && !donationAddrStr.empty()) {
            // Calculate donation using integer math
            donationAmount = (total_value * donationPercent) / 100;
            minerAmount = total_value - donationAmount;

            // Parse donation address to create script
            KeyIO keyIO(chainparams);
            auto donationAddr = keyIO.DecodePaymentAddress(donationAddrStr);
            if (donationAddr.has_value() && std::holds_alternative<CKeyID>(donationAddr.value())) {
                donationScript = GetScriptForDestination(std::get<CKeyID>(donationAddr.value()));
            } else {
                // Invalid donation address, log warning and skip donation
                LogPrintf("Warning: Invalid donation address '%s', skipping donation\n", donationAddrStr);
                minerAmount = total_value;
                donationAmount = 0;
            }
        }

        // Create outputs
        if (donationAmount > 0 && !donationScript.empty()) {
            mtx.vout.resize(2);
            mtx.vout[0] = CTxOut(minerAmount, coinbaseScript->reserveScript);
            mtx.vout[1] = CTxOut(donationAmount, donationScript);
            LogPrint("pow", "%s: Adding donation output of %s (%d%%) to %s\n",
                     __func__, FormatMoney(donationAmount), donationPercent, donationAddrStr);
        } else {
            mtx.vout.resize(1);
            mtx.vout[0] = CTxOut(minerAmount, coinbaseScript->reserveScript);
        }

        ComputeBindingSig(std::move(saplingBuilder), std::nullopt);
    }
};

CMutableTransaction CreateCoinbaseTransaction(const CChainParams& chainparams, CAmount nFees, const MinerAddress& minerAddress, int nHeight)
{
        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(
                chainparams.GetConsensus(), nHeight,
                !std::holds_alternative<libzcash::OrchardRawAddress>(minerAddress) && nPreferredTxVersion < ZIP225_MIN_TX_VERSION);
        mtx.vin.resize(1);
        mtx.vin[0].prevout.SetNull();
        if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5)) {
            // ZIP 203: From NU5 onwards, nExpiryHeight is set to the block height in
            // coinbase transactions.
            mtx.nExpiryHeight = nHeight;
        } else {
            // Set to 0 so expiry height does not apply to coinbase txs
            mtx.nExpiryHeight = 0;
        }

        // Add outputs and sign
        std::visit(
            AddOutputsToCoinbaseTxAndSign(mtx, chainparams, nHeight, nFees),
            minerAddress);

        mtx.vin[0].scriptSig = CScript() << nHeight << OP_0;
        return mtx;
}

BlockAssembler::BlockAssembler(const CChainParams& _chainparams)
    : chainparams(_chainparams)
{
    // Largest block you're willing to create:
    nBlockMaxSize = GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to between 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // Number of unpaid actions allowed in a block:
    nBlockUnpaidActionLimit = (size_t) GetArg("-blockunpaidactionlimit", DEFAULT_BLOCK_UNPAID_ACTION_LIMIT);
}

void BlockAssembler::resetBlock(const MinerAddress& minerAddress)
{
    inBlock.clear();

    // Reserve space for coinbase tx
    // nBlockMaxSize already includes 1000 bytes for transaction structure overhead.
    nBlockSize = examine(minerAddress, match {
        [](const libzcash::OrchardRawAddress&) { return 9000; },
        [](const libzcash::SaplingPaymentAddress&) { return 1000; },
        [](const boost::shared_ptr<CReserveScript> &) { return 1000; },
    });
    nBlockSigOps = 100;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;

    sproutValue = 0;
    saplingValue = 0;
    orchardValue = 0;
    monitoring_pool_balances = true;

    lastFewTxs = 0;
    blockFinished = false;
}

CBlockTemplate* BlockAssembler::CreateNewBlock(
    const MinerAddress& minerAddress,
    const std::optional<CMutableTransaction>& next_cb_mtx)
{
    resetBlock(minerAddress);

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate.get())
        return NULL;
    pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.push_back(CTransaction());
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    // If we're given a coinbase tx, it's been precomputed, its fees are zero,
    // so we can't include any mempool transactions; this will be an empty block.
    blockFinished = blockFinished || next_cb_mtx;

    LOCK2(cs_main, mempool.cs);
    CBlockIndex* pindexPrev = chainActive.Tip();
    nHeight = pindexPrev->nHeight + 1;
    uint32_t consensusBranchId = CurrentEpochBranchId(nHeight, chainparams.GetConsensus());

    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand())
        pblock->nVersion = GetArg("-blockversion", pblock->nVersion);

    // Setting nTime to 0 and then calling UpdateTime ensures that it is set to the
    // nearest timestamp to the current time in the consensus-valid range (see #6960).
    pblock->nTime = 0;
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);

    const int64_t nMedianTimePast = pindexPrev->GetMedianTimePast();
    CCoinsViewCache view(pcoinsTip);

    SaplingMerkleTree sapling_tree;
    assert(view.GetSaplingAnchorAt(view.GetBestAnchor(SAPLING), sapling_tree));

    nLockTimeCutoff = (STANDARD_LOCKTIME_VERIFY_FLAGS & LOCKTIME_MEDIAN_TIME_PAST)
                       ? nMedianTimePast
                       : pblock->GetBlockTime();

    // We want to track the value pool, but if the miner gets
    // invoked on an old block before the hardcoded fallback
    // is active we don't want to trip up any assertions. So,
    // we only adhere to the turnstile (as a miner) if we
    // actually have all of the information necessary to do
    // so.
    if (chainparams.ZIP209Enabled()) {
        if (pindexPrev->nChainSproutValue) {
            sproutValue = *pindexPrev->nChainSproutValue;
        } else {
            monitoring_pool_balances = false;
        }
        if (pindexPrev->nChainSaplingValue) {
            saplingValue = *pindexPrev->nChainSaplingValue;
        } else {
            monitoring_pool_balances = false;
        }
        if (pindexPrev->nChainOrchardValue) {
            orchardValue = *pindexPrev->nChainOrchardValue;
        } else {
            monitoring_pool_balances = false;
        }
    }

    constructZIP317BlockTemplate();

    last_block_num_txs = nBlockTx;
    last_block_size = nBlockSize;
    LogPrintf("%s: total size %u (excluding coinbase) txs: %u fees: %ld sigops %d", __func__, nBlockSize, nBlockTx, nFees, nBlockSigOps);

    // Create coinbase tx
    if (next_cb_mtx) {
        pblock->vtx[0] = *next_cb_mtx;
    } else {
        pblock->vtx[0] = CreateCoinbaseTransaction(chainparams, nFees, minerAddress, nHeight);
    }
    pblocktemplate->vTxFees[0] = -nFees;

    // Update the Sapling commitment tree.
    for (const CTransaction& tx : pblock->vtx) {
        for (const auto& odesc : tx.GetSaplingOutputs()) {
            sapling_tree.append(uint256::FromRawBytes(odesc.cmu()));
        }
    }

    // Randomise nonce
    arith_uint256 nonce = UintToArith256(GetRandHash());
    // Clear the top and bottom 16 bits (for local use as thread flags and counters)
    nonce <<= 32;
    nonce >>= 16;
    pblock->nNonce = ArithToUint256(nonce);

    uint32_t prevConsensusBranchId = CurrentEpochBranchId(pindexPrev->nHeight, chainparams.GetConsensus());

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5)) {
        // hashBlockCommitments depends on the block transactions, so we have to
        // update it whenever the coinbase transaction changes.
        //
        // - For the internal miner (either directly or via the `generate` RPC), this
        //   will occur in `IncrementExtraNonce()`, like for `hashMerkleRoot`.
        // - For `getblocktemplate`, we have two sets of fields to handle:
        //   - The `defaultroots` fields, which contain both the default value (if
        //     nothing in the template is altered), and the roots that can be used to
        //     recalculate it (if some or all of the template is altered).
        //   - The legacy `finalsaplingroothash`, `lightclientroothash`, and
        //     `blockcommitmentshash` fields, which had the semantics of "place this
        //     value into the block header and things will work" (except for in
        //     v4.6.0 where they were accidentally set to always be the NU5 value).
        //
        // To accommodate all use cases, we calculate the `hashBlockCommitments`
        // default value here (unlike `hashMerkleRoot`), and additionally cache the
        // values necessary to recalculate it.
        pblocktemplate->hashChainHistoryRoot = view.GetHistoryRoot(prevConsensusBranchId);
        pblocktemplate->hashAuthDataRoot = pblock->BuildAuthDataMerkleTree();
        pblock->hashBlockCommitments = DeriveBlockCommitmentsHash(
                pblocktemplate->hashChainHistoryRoot,
                pblocktemplate->hashAuthDataRoot);
    } else if (IsActivationHeight(nHeight, chainparams.GetConsensus(), Consensus::UPGRADE_HEARTWOOD)) {
        pblocktemplate->hashChainHistoryRoot.SetNull();
        pblocktemplate->hashAuthDataRoot.SetNull();
        pblock->hashBlockCommitments.SetNull();
    } else if (chainparams.GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_HEARTWOOD)) {
        pblocktemplate->hashChainHistoryRoot = view.GetHistoryRoot(prevConsensusBranchId);
        pblocktemplate->hashAuthDataRoot.SetNull();
        pblock->hashBlockCommitments = pblocktemplate->hashChainHistoryRoot;
    } else {
        pblocktemplate->hashChainHistoryRoot.SetNull();
        pblocktemplate->hashAuthDataRoot.SetNull();
        pblock->hashBlockCommitments = sapling_tree.root();
    }
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nSolution.clear();
    pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(pblock->vtx[0]);

    CValidationState state;
    if (!TestBlockValidity(state, chainparams, *pblock, pindexPrev, true)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
    }

    return pblocktemplate.release();
}

bool BlockAssembler::isStillDependent(CTxMemPool::txiter iter)
{
    for (CTxMemPool::txiter parent : mempool.GetMemPoolParents(iter))
    {
        if (!inBlock.count(parent)) {
            return true;
        }
    }
    return false;
}



bool BlockAssembler::TestForBlock(CTxMemPool::txiter iter)
{
    if (nBlockSize + iter->GetTxSize() >= nBlockMaxSize) {
        // If the block is so close to full that no more txs will fit
        // or if we've tried more than 50 times to fill remaining space
        // then flag that the block is finished
        if (nBlockSize >  nBlockMaxSize - 100 || lastFewTxs > 50) {
             blockFinished = true;
             return false;
        }
        // Once we're within 1000 bytes of a full block, only look at 50 more txs
        // to try to fill the remaining space.
        if (nBlockSize > nBlockMaxSize - 1000) {
            lastFewTxs++;
        }
        LogPrintf("%s: skipping tx %s: exceeded maximum block size %u.",
                  __func__,
                  iter->GetTx().GetHash().GetHex(),
                  nBlockMaxSize);
        return false;
    }

    if (nBlockSigOps + iter->GetSigOpCount() >= MAX_BLOCK_SIGOPS) {
        // If the block has room for no more sig ops then
        // flag that the block is finished
        if (nBlockSigOps > MAX_BLOCK_SIGOPS - 2) {
            blockFinished = true;
            return false;
        }
        // Otherwise attempt to find another tx with fewer sigops
        // to put in the block.
        LogPrintf("%s: skipping tx %s: exceeds legacy max sigops %u.",
                  __func__,
                  iter->GetTx().GetHash().GetHex(),
                  MAX_BLOCK_SIGOPS);
        return false;
    }

    // Must check that lock times are still valid
    // This can be removed once MTP is always enforced
    // as long as reorgs keep the mempool consistent.
    if (!IsFinalTx(iter->GetTx(), nHeight, nLockTimeCutoff))
        return false;

    // Must check that expiry heights are still valid.
    if (IsExpiredTx(iter->GetTx(), nHeight))
        return false;

    if (chainparams.ZIP209Enabled() && monitoring_pool_balances) {
        // Does this transaction lead to a turnstile violation?

        CAmount sproutValueDummy = sproutValue;
        CAmount saplingValueDummy = saplingValue;
        CAmount orchardValueDummy = orchardValue;

        saplingValueDummy += -iter->GetTx().GetValueBalanceSapling();
        orchardValueDummy += -iter->GetTx().GetOrchardBundle().GetValueBalance();

        for (auto js : iter->GetTx().vJoinSplit) {
            sproutValueDummy += js.vpub_old;
            sproutValueDummy -= js.vpub_new;
        }

        if (sproutValueDummy < 0) {
            LogPrintf("CreateNewBlock: tx %s appears to violate Sprout turnstile\n",
                      iter->GetTx().GetHash().GetHex());
            return false;
        }
        if (saplingValueDummy < 0) {
            LogPrintf("CreateNewBlock: tx %s appears to violate Sapling turnstile\n",
                      iter->GetTx().GetHash().GetHex());
            return false;
        }
        if (orchardValueDummy < 0) {
            LogPrintf("CreateNewBlock: tx %s appears to violate Orchard turnstile\n",
                      iter->GetTx().GetHash().GetHex());
            return false;
        }

        // We update this here instead of in AddToBlock to avoid recalculating
        // the deltas, because there are no more checks and we know that the
        // transaction will be added to the block.
        sproutValue = sproutValueDummy;
        saplingValue = saplingValueDummy;
        orchardValue = orchardValueDummy;
    }

    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.push_back(iter->GetTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOps.push_back(iter->GetSigOpCount());
    nBlockSize += iter->GetTxSize();
    ++nBlockTx;
    nBlockSigOps += iter->GetSigOpCount();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("%s: txid %s; modified fee %s; conventional fee %s; size %d bytes; logical actions %d; unpaid actions %d\n",
                  __func__,
                  iter->GetTx().GetHash().ToString(),
                  FormatMoney(iter->GetModifiedFee()),
                  FormatMoney(iter->GetTx().GetConventionalFee()),
                  iter->GetTxSize(),
                  iter->GetTx().GetLogicalActionCount(),
                  iter->GetUnpaidActionCount());
    }
}

void BlockAssembler::constructZIP317BlockTemplate()
{
    CTxMemPool::weightedCandidates candidatesPayingConventionalFee;
    CTxMemPool::weightedCandidates candidatesNotPayingConventionalFee;

    for (auto mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
    {
        int128_t weightRatio = mi->GetWeightRatio();
        if (weightRatio >= WEIGHT_RATIO_SCALE) {
            candidatesPayingConventionalFee.add(mi->GetTx().GetHash(), mi, weightRatio);
        } else {
            candidatesNotPayingConventionalFee.add(mi->GetTx().GetHash(), mi, weightRatio);
        }
    }

    CTxMemPool::queueEntries waiting;
    CTxMemPool::queueEntries cleared;
    addTransactions(candidatesPayingConventionalFee, waiting, cleared);
    addTransactions(candidatesNotPayingConventionalFee, waiting, cleared);
}

void BlockAssembler::addTransactions(
    CTxMemPool::weightedCandidates& candidates,
    CTxMemPool::queueEntries& waiting,
    CTxMemPool::queueEntries& cleared)
{
    size_t nBlockUnpaidActions = 0;

    while (!blockFinished && !(candidates.empty() && cleared.empty()))
    {
        CTxMemPool::txiter iter;
        if (cleared.empty()) {
            // If no txs that were previously postponed are available to try
            // again, then select the next transaction randomly by weight ratio
            // from the candidate set.
            assert(!candidates.empty());
            iter = std::get<1>(candidates.takeRandom().value());
        } else {
            // If a previously postponed tx is available to try again, then it
            // has already been randomly sampled, so just take it in order.
            iter = cleared.front();
            cleared.pop_front();
        }

        // The tx should never already be in the block for ZIP 317.
        assert(inBlock.count(iter) == 0);

        // If the tx would cause the block to exceed the unpaid action limit, skip it.
        // A tx that pays at least the conventional fee will have no unpaid actions.
        size_t txUnpaidActions = iter->GetUnpaidActionCount();
        if (nBlockUnpaidActions + txUnpaidActions > nBlockUnpaidActionLimit) {
            continue;
        }

        // If tx is dependent on other mempool transactions that haven't yet been
        // included then put it in the waiting queue.
        if (isStillDependent(iter)) {
            waiting.push_back(iter);
            continue;
        }

        // If this tx fits in the block add it, otherwise keep looping.
        if (TestForBlock(iter)) {
            AddToBlock(iter);
            nBlockUnpaidActions += txUnpaidActions;

            // This tx was successfully added, so add waiting transactions that
            // depend on this one to the cleared queue to try again.
            //
            // TODO: This makes the overall algorithm O(n^2 log n) in the worst case
            // of a linear dependency chain. (children is a std::map; its count method
            // is O(log n) given the maximum number of children at each step, and is
            // called O(n^2) times.) Daira conjectures that O(n log n) is possible.
            auto children = mempool.GetMemPoolChildren(iter);
            CTxMemPool::queueEntries stillWaiting;
            for (CTxMemPool::txiter maybeChild : waiting)
            {
                if (children.count(maybeChild) > 0) {
                    cleared.push_back(maybeChild);
                } else {
                    stillWaiting.push_back(maybeChild);
                }
            }
            waiting.swap(stillWaiting);
        }
    }
}


//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//

#ifdef ENABLE_MINING

class MinerAddressScript : public CReserveScript
{
    // CReserveScript requires implementing this function, so that if an
    // internal (not-visible) wallet address is used, the wallet can mark it as
    // important when a block is mined (so it then appears to the user).
    // If -mineraddress is set, the user already knows about and is managing the
    // address, so we don't need to do anything here.
    void KeepScript() {}
};

std::optional<MinerAddress> ExtractMinerAddress::operator()(const CKeyID &keyID) const {
    boost::shared_ptr<MinerAddressScript> mAddr(new MinerAddressScript());
    mAddr->reserveScript = CScript() << OP_DUP << OP_HASH160 << ToByteVector(keyID) << OP_EQUALVERIFY << OP_CHECKSIG;
    return mAddr;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const CScriptID &addr) const {
    return std::nullopt;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const libzcash::SproutPaymentAddress &addr) const {
    return std::nullopt;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const libzcash::SaplingPaymentAddress &addr) const {
    return addr;
}
std::optional<MinerAddress> ExtractMinerAddress::operator()(const libzcash::UnifiedAddress &addr) const {
    auto preferred = addr.GetPreferredRecipientAddress(consensus, height);
    if (preferred.has_value()) {
        return examine(preferred.value(), match {
            [&](const libzcash::OrchardRawAddress addr) -> std::optional<MinerAddress> { return MinerAddress(addr); },
            [&](const libzcash::SaplingPaymentAddress addr) -> std::optional<MinerAddress> { return MinerAddress(addr); },
            [&](const CKeyID keyID) -> std::optional<MinerAddress> { return operator()(keyID); },
            [&](const auto other) -> std::optional<MinerAddress> { return std::nullopt; }
        });
    } else {
        return std::nullopt;
    }
}


void GetMinerAddress(std::optional<MinerAddress> &minerAddress)
{
    KeyIO keyIO(Params());

    // If the user sets a UA miner address with an Orchard component, we want to ensure we
    // start using it once we reach that height.
    int height;
    {
        LOCK(cs_main);
        height = chainActive.Height() + 1;
    }

    auto mAddrArg = GetArg("-mineraddress", "");
    auto zaddr0 = keyIO.DecodePaymentAddress(mAddrArg);
    if (zaddr0.has_value()) {
        auto zaddr = std::visit(ExtractMinerAddress(Params().GetConsensus(), height), zaddr0.value());
        if (zaddr.has_value()) {
            minerAddress = zaddr.value();
        }
    }
}

void IncrementExtraNonce(
    CBlockTemplate* pblocktemplate,
    const CBlockIndex* pindexPrev,
    unsigned int& nExtraNonce,
    const Consensus::Params& consensusParams)
{
    CBlock *pblock = &pblocktemplate->block;
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    CMutableTransaction txCoinbase(pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = txCoinbase;
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
    if (consensusParams.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_NU5)) {
        pblocktemplate->hashAuthDataRoot = pblock->BuildAuthDataMerkleTree();
        pblock->hashBlockCommitments = DeriveBlockCommitmentsHash(
            pblocktemplate->hashChainHistoryRoot,
            pblocktemplate->hashAuthDataRoot);
    }
}

static bool ProcessBlockFound(const CBlock* pblock, const CChainParams& chainparams)
{
    LogPrintf("%s\n", pblock->ToString());

    // Log coinbase outputs
    CAmount totalMinerReward = 0;
    for (size_t i = 0; i < pblock->vtx[0].vout.size(); i++) {
        totalMinerReward += pblock->vtx[0].vout[i].nValue;
    }

    if (pblock->vtx[0].vout.size() > 1) {
        // Has donation output
        LogPrintf("generated %s to miner, %s donation (%d%% of %s total)\n",
                  FormatMoney(pblock->vtx[0].vout[0].nValue),
                  FormatMoney(pblock->vtx[0].vout[1].nValue),
                  GetArg("-donationpercentage", 0),
                  FormatMoney(totalMinerReward));
    } else {
        LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0].vout[0].nValue));
    }

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != chainActive.Tip()->GetBlockHash())
            return error("JunoCashMiner: generated block is stale");
    }

    // Process this block the same as if we had received it from another node
    CValidationState state;
    if (!ProcessNewBlock(state, chainparams, NULL, pblock, true, NULL))
        return error("JunoCashMiner: ProcessNewBlock, block not accepted");

    TrackMinedBlock(pblock->GetHash());

    return true;
}

void static BitcoinMiner(const CChainParams& chainparams, int thread_id, int total_threads)
{
    LogPrintf("JunoMonetaMiner started (thread %d/%d)\n", thread_id + 1, total_threads);
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    RenameThread("juno-miner");

    // NUMA: Pin thread to CPU if available for optimal memory access on multi-socket systems
    NumaHelper& numa = NumaHelper::GetInstance();
    if (numa.IsNUMAAvailable()) {
        int cpu_id = numa.GetCPUForThread(thread_id, total_threads);
        if (cpu_id >= 0 && numa.PinCurrentThread(cpu_id)) {
            LogPrint("numa", "NUMA: Thread %d pinned to CPU %d (node %d)\n",
                     thread_id, cpu_id, numa.GetNodeForThread(thread_id, total_threads));
        }
    }

    // Initialize RandomX (if not already done by init.cpp)
    bool randomxFastMode = GetBoolArg("-randomxfastmode", false);
    RandomX_Init(randomxFastMode);

    // Each thread has its own counter
    unsigned int nExtraNonce = 0;

    // Juno Cash: Legacy Equihash parameters removed
    // unsigned int n = chainparams.GetConsensus().nEquihashN;
    // unsigned int k = chainparams.GetConsensus().nEquihashK;
    // std::string solver = GetArg("-equihashsolver", "default");
    // assert(solver == "tromp" || solver == "default");
    // LogPrint("pow", "Using Equihash solver \"%s\" with n = %u, k = %u\n", solver, n, k);

    LogPrint("pow", "Using RandomX proof-of-work algorithm\n");

    std::mutex m_cs;
    bool cancelSolver = false;
    boost::signals2::connection c = uiInterface.NotifyBlockTip.connect(
        [&m_cs, &cancelSolver](bool, const CBlockIndex *) mutable {
            std::lock_guard<std::mutex> lock{m_cs};
            cancelSolver = true;
        }
    );
    miningTimer.start();

    try {
        while (true) {
            // Get a fresh address for each block
            std::optional<MinerAddress> maybeMinerAddress;
            GetMainSignals().AddressForMining(maybeMinerAddress);

            // Throw an error if no address valid for mining was provided.
            if (!(maybeMinerAddress.has_value() && std::visit(IsValidMinerAddress(), maybeMinerAddress.value()))) {
                throw std::runtime_error("No miner address available (mining requires a wallet or -mineraddress)");
            }
            auto minerAddress = maybeMinerAddress.value();

            if (chainparams.MiningRequiresPeers()) {
                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                miningTimer.stop();
                do {
                    bool fvNodesEmpty;
                    {
                        LOCK(cs_vNodes);
                        fvNodesEmpty = vNodes.empty();
                    }
                    if (!fvNodesEmpty && !IsInitialBlockDownload(chainparams.GetConsensus()))
                        break;
                    MilliSleep(1000);
                } while (true);
                miningTimer.start();
            }

            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
            CBlockIndex* pindexPrev;
            {
                LOCK(cs_main);
                pindexPrev = chainActive.Tip();
            }

            // If we don't have a valid chain tip to work from, wait and try again.
            if (pindexPrev == nullptr) {
                MilliSleep(1000);
                continue;
            }

            unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(chainparams).CreateNewBlock(minerAddress));
            if (!pblocktemplate.get())
            {
                if (GetArg("-mineraddress", "").empty()) {
                    LogPrintf("Error in JunoCashMiner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                } else {
                    // Should never reach here, because -mineraddress validity is checked in init.cpp
                    LogPrintf("Error in JunoCashMiner: Invalid -mineraddress\n");
                }
                return;
            }
            CBlock *pblock = &pblocktemplate->block;
            IncrementExtraNonce(pblocktemplate.get(), pindexPrev, nExtraNonce, chainparams.GetConsensus());

            LogPrintf("Running JunoMonetaMiner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
                ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));

            // Calculate RandomX seed for this block height
            uint64_t blockHeight = pindexPrev->nHeight + 1;
            uint64_t seedHeight = RandomX_SeedHeight(blockHeight);
            uint256 seedHash;

            if (seedHeight == 0) {
                // Genesis epoch - use genesis seed
                seedHash.SetNull();
                *seedHash.begin() = 0x08;
                LogPrint("pow", "Mining block %u in genesis epoch (seed height 0)\n", blockHeight);
            } else {
                // Get seed block hash from chain
                CBlockIndex* pindexSeed = chainActive[seedHeight];
                if (!pindexSeed) {
                    LogPrintf("Error: Could not find seed block at height %u\n", seedHeight);
                    continue;
                }
                seedHash = pindexSeed->GetBlockHash();
                LogPrint("pow", "Mining block %u with seed from height %u: %s\n",
                         blockHeight, seedHeight, seedHash.GetHex());
            }

            // Update RandomX cache for this seed
            RandomX_SetMainSeedHash(seedHash.begin(), 32);

            //
            // Search
            //
            int64_t nStart = GetTime();
            arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

            while (true) {
                // I = the block header minus nonce and solution
                CEquihashInput I{*pblock};
                CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                ss << I;
                ss << pblock->nNonce;

                // Calculate RandomX hash
                uint256 hash;
                if (!RandomX_Hash_Block(ss.data(), ss.size(), hash)) {
                    LogPrintf("RandomX hashing failed\n");
                    break;
                }

                // Increment hash counter for metrics
                ehSolverRuns.increment();

                // Store the hash in nSolution (32 bytes)
                pblock->nSolution.resize(32);
                memcpy(pblock->nSolution.data(), hash.begin(), 32);

                // Check if hash meets target
                solutionTargetChecks.increment();
                if (UintToArith256(hash) <= hashTarget) {
                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    LogPrintf("JunoMonetaMiner:\n");
                    LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());

                    if (ProcessBlockFound(pblock, chainparams)) {
                        // Ignore chain updates caused by us
                        std::lock_guard<std::mutex> lock{m_cs};
                        cancelSolver = false;
                    }
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    std::visit(KeepMinerAddress(), minerAddress);

                    // In regression test mode, stop mining after a block is found
                    if (chainparams.MineBlocksOnDemand()) {
                        throw boost::thread_interrupted();
                    }

                    break;
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();
                // Regtest mode doesn't require peers
                if (vNodes.empty() && chainparams.MiningRequiresPeers())
                    break;
                if ((UintToArith256(pblock->nNonce) & 0xffff) == 0xffff)
                    break;
                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                    break;
                if (pindexPrev != chainActive.Tip())
                    break;

                // Update nNonce and nTime
                pblock->nNonce = ArithToUint256(UintToArith256(pblock->nNonce) + 1);
                if (UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev) < 0)
                    break; // Recreate the block if the clock has run backwards,
                           // so that we can use the correct time.
                if (chainparams.GetConsensus().nPowAllowMinDifficultyBlocksAfterHeight != std::nullopt)
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
            }
        }
    }
    catch (const boost::thread_interrupted&)
    {
        miningTimer.stop();
        c.disconnect();
        LogPrintf("JunoCashMiner terminated\n");
        throw;
    }
    catch (const std::runtime_error &e)
    {
        miningTimer.stop();
        c.disconnect();
        LogPrintf("JunoCashMiner runtime error: %s\n", e.what());
        return;
    }
    miningTimer.stop();
    c.disconnect();
}

void GenerateBitcoins(bool fGenerate, int nThreads, const CChainParams& chainparams)
{
    static boost::thread_group* minerThreads = NULL;

    if (nThreads < 0)
        nThreads = GetNumCores();

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();
        minerThreads->join_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    // Initialize NUMA before spawning threads for optimal thread-to-CPU pinning
    NumaHelper::GetInstance().Initialize();

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++) {
        minerThreads->create_thread(boost::bind(&BitcoinMiner, boost::cref(chainparams), i, nThreads));
    }
}

#endif // ENABLE_MINING
