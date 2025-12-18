// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin Core developers
// Copyright (c) 2015-2025 The Zcash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "chainparams.h"
#include "consensus/merkle.h"
#include "key_io.h"
#include "main.h"
#include "crypto/equihash.h"

#include "tinyformat.h"
#include "util/system.h"
#include "util/strencodings.h"

#include <assert.h>
#include <optional>
#include <variant>

#include <boost/assign/list_of.hpp>

#include "chainparamsseeds.h"

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, const uint256& nNonce, const std::vector<unsigned char>& nSolution, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    // Juno Cash: Genesis uses Canopy-era transaction format (v4)
    // NU5 (Orchard) activates at block 1 to avoid genesis anchor initialization issues
    CMutableTransaction txNew;
    txNew.fOverwintered = true;
    txNew.nVersionGroupId = SAPLING_VERSION_GROUP_ID;
    txNew.nVersion = SAPLING_TX_VERSION;
    txNew.nConsensusBranchId = NetworkUpgradeInfo[Consensus::UPGRADE_CANOPY].nBranchId;
    txNew.nExpiryHeight = 0;  // Coinbase transactions never expire
    txNew.nLockTime = 0;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vin[0].scriptSig = CScript() << 520617983 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nSolution = nSolution;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(txNew);
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database (and is in any case of zero value).
 *
 * >>> from hashlib import blake2s
 * >>> 'Zcash' + blake2s(b'The Economist 2016-10-29 Known unknown: Another crypto-currency is born. BTC#436254 0000000000000000044f321997f336d2908cf8c8d6893e88dbf067e2d949487d ETH#2521903 483039a6b6bd8bd05f0584f9a078d075e454925eb71c1f13eaff59b405a721bb DJIA close on 27 Oct 2016: 18,169.68').hexdigest()
 *
 * CBlock(hash=00040fe8, ver=4, hashPrevBlock=00000000000000, hashMerkleRoot=c4eaa5, nTime=1477641360, nBits=1f07ffff, nNonce=4695, vtx=1)
 *   CTransaction(hash=c4eaa5, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff071f0104455a6361736830623963346565663862376363343137656535303031653335303039383462366665613335363833613763616331343161303433633432303634383335643334)
 *     CTxOut(nValue=0.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: c4eaa5
 */
static CBlock CreateGenesisBlock(uint32_t nTime, const uint256& nNonce, const std::vector<unsigned char>& nSolution, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "b7c923155000000000007ca8a49b1fc30e20bd9d5a3ca9a092af920f2f27b0c3";
    const CScript genesisOutputScript = CScript() << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nSolution, nBits, nVersion, genesisReward);
}

/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */

const arith_uint256 maxUint = UintToArith256(uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));

class CMainParams : public CChainParams {
public:
    CMainParams() {
        keyConstants.strNetworkID = "main";
        strCurrencyUnits = "JUNO";
        keyConstants.bip44CoinType = 8133; // Juno Cash coin type
        consensus.fCoinbaseMustBeShielded = true;
        consensus.nSubsidySlowStartInterval = 20000;
        consensus.nPreBlossomSubsidyHalvingInterval = Consensus::PRE_BLOSSOM_HALVING_INTERVAL;
        consensus.nPostBlossomSubsidyHalvingInterval = POST_BLOSSOM_HALVING_INTERVAL(Consensus::PRE_BLOSSOM_HALVING_INTERVAL);
        consensus.nMajorityEnforceBlockUpgrade = 750;
        consensus.nMajorityRejectBlockOutdated = 950;
        consensus.nMajorityWindow = 4000;
        // Juno Cash: Legacy Equihash parameters removed (uses RandomX instead)
        // const size_t N = 200, K = 9;
        // static_assert(equihash_parameters_acceptable(N, K));
        // consensus.nEquihashN = N;
        // consensus.nEquihashK = K;
        consensus.powLimit = uint256S("0007ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowAveragingWindow = 100;
        assert(maxUint/UintToArith256(consensus.powLimit) >= consensus.nPowAveragingWindow);
        consensus.nPowMaxAdjustDown = 32; // 32% adjustment down
        consensus.nPowMaxAdjustUp = 16; // 16% adjustment up
        consensus.nPreBlossomPowTargetSpacing = Consensus::PRE_BLOSSOM_POW_TARGET_SPACING;
        consensus.nPostBlossomPowTargetSpacing = Consensus::POST_BLOSSOM_POW_TARGET_SPACING;
        consensus.nPowAllowMinDifficultyBlocksAfterHeight = std::nullopt;
        consensus.fPowNoRetargeting = false;
        // Juno Cash: All consensus upgrades active from genesis (Orchard-only chain)
        consensus.vUpgrades[Consensus::BASE_SPROUT].nProtocolVersion = 170002;
        consensus.vUpgrades[Consensus::BASE_SPROUT].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nProtocolVersion = 170002;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
            Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_OVERWINTER].nProtocolVersion = 170005;
        consensus.vUpgrades[Consensus::UPGRADE_OVERWINTER].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_SAPLING].nProtocolVersion = 170007;
        consensus.vUpgrades[Consensus::UPGRADE_SAPLING].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nProtocolVersion = 170009;
        consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_HEARTWOOD].nProtocolVersion = 170011;
        consensus.vUpgrades[Consensus::UPGRADE_HEARTWOOD].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_CANOPY].nProtocolVersion = 170013;
        consensus.vUpgrades[Consensus::UPGRADE_CANOPY].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        // Juno Cash: Delay NU5/NU6 activation to avoid genesis anchor issues
        // NU5 (Orchard) activates at block 1, not genesis
        consensus.vUpgrades[Consensus::UPGRADE_NU5].nProtocolVersion = 170100;
        consensus.vUpgrades[Consensus::UPGRADE_NU5].nActivationHeight = 1;
        consensus.vUpgrades[Consensus::UPGRADE_NU6].nProtocolVersion = 170120;
        consensus.vUpgrades[Consensus::UPGRADE_NU6].nActivationHeight = 2;
        consensus.vUpgrades[Consensus::UPGRADE_NU6_1].nProtocolVersion = 170140;
        consensus.vUpgrades[Consensus::UPGRADE_NU6_1].nActivationHeight = 3;
        consensus.vUpgrades[Consensus::UPGRADE_ZFUTURE].nProtocolVersion = 0x7FFFFFFF;
        consensus.vUpgrades[Consensus::UPGRADE_ZFUTURE].nActivationHeight =
            Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;

        consensus.nFundingPeriodLength = consensus.nPostBlossomSubsidyHalvingInterval / 48;

        // guarantees the first 2 characters, when base58 encoded, are "t1"
        keyConstants.base58Prefixes[PUBKEY_ADDRESS]     = {0x1C,0xB8};
        // guarantees the first 2 characters, when base58 encoded, are "t3"
        keyConstants.base58Prefixes[SCRIPT_ADDRESS]     = {0x1C,0xBD};
        // the first character, when base58 encoded, is "5" or "K" or "L" (as in Bitcoin)
        keyConstants.base58Prefixes[SECRET_KEY]         = {0x80};
        // do not rely on these BIP32 prefixes; they are not specified and may change
        keyConstants.base58Prefixes[EXT_PUBLIC_KEY]     = {0x04,0x88,0xB2,0x1E};
        keyConstants.base58Prefixes[EXT_SECRET_KEY]     = {0x04,0x88,0xAD,0xE4};
        // guarantees the first 2 characters, when base58 encoded, are "zc"
        keyConstants.base58Prefixes[ZCPAYMENT_ADDRESS]  = {0x16,0x9A};
        // guarantees the first 4 characters, when base58 encoded, are "ZiVK"
        keyConstants.base58Prefixes[ZCVIEWING_KEY]      = {0xA8,0xAB,0xD3};
        // guarantees the first 2 characters, when base58 encoded, are "SK"
        keyConstants.base58Prefixes[ZCSPENDING_KEY]     = {0xAB,0x36};

        keyConstants.bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "zs";
        keyConstants.bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "zviews";
        keyConstants.bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "zivks";
        keyConstants.bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "secret-extended-key-main";
        keyConstants.bech32HRPs[SAPLING_EXTENDED_FVK]         = "zxviews";

        keyConstants.bech32mHRPs[TEX_ADDRESS]                 = "tex";

        // Juno Cash: No funding streams (0% dev tax)

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x0000000000000000000000000000000000000000000000000000a2d4b9e4bdb1");

        /**
         * Network magic bytes for Juno Cash - derived from Bitcoin block 919123 hash
         */
        pchMessageStart[0] = 0xb5;
        pchMessageStart[1] = 0x0c;
        pchMessageStart[2] = 0x07;
        pchMessageStart[3] = 0x02;
        vAlertPubKey = ParseHex("042c66363e6bd947c72c64878822b3bb114f7b98da8d6e825ec5a575ebfd2a9bbe36e99af2fcbf340f65410da22d2e0e08ae685776b58bd1508b417ac9b61c3da9");
        nDefaultPort = 8234;
        nPruneAfterHeight = 100000;

        genesis = CreateGenesisBlock(
            1763197809,
            uint256S("0x0000000000000000000000000000000000000000000000000000000000001398"),
            ParseHex("bc701d355489e7960f94e828697ace16e45e6cf7374601eb244ab39225ff9100"),
            0x2000ffff, 4, 0);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x0091ff2592b34a24eb014637f76c5ee416ce7a6928e8940f96e78954351d70bc"));
        assert(genesis.hashMerkleRoot == uint256S("0xe53f8696814445c3b5668803e8f2475004dc9c00c05c68a67f92c731c1432c67"));

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.push_back(CDNSSeedData("junomoneta.io", "dnsseed.junomoneta.io"));
	vSeeds.push_back(CDNSSeedData("juno.cash", "dnsseed.juno.cash"));

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));

        fMiningRequiresPeers = true;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;
        fTestnetToBeDeprecatedFieldRPC = false;

        // Juno Cash: Checkpoint data
        checkpointData = (CCheckpointData) {
            boost::assign::map_list_of
            (0, consensus.hashGenesisBlock)
            (29453, uint256S("0x000000460b68ba29bc26af81f40d9ff798afbcac35ae3db80bc12cfaf78b9beb"))
            (49000, uint256S("0x00000000269763d45087dfb138ea4ed4bf33e9ab39a6aed2705aca72e4471e69")),
            1766015923,  // UNIX timestamp of last checkpoint block (49000)
            55276,       // total number of transactions between genesis and last checkpoint
            1000         // estimated number of transactions per day after checkpoint
        };

        // Juno Cash: Reset Sprout value pool checkpoint (no Sprout activity on new chain)
        nSproutValuePoolCheckpointHeight = 0;
        nSproutValuePoolCheckpointBalance = 0;
        fZIP209Enabled = true;
        hashSproutValuePoolCheckpointBlock = consensus.hashGenesisBlock;

        // Juno Cash: No founders reward (0% dev tax)
        vFoundersRewardAddress = {};
    }
};
static CMainParams mainParams;

/**
 * Testnet (v3)
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams() {
        keyConstants.strNetworkID = "test";
        strCurrencyUnits = "TJUNO";
        keyConstants.bip44CoinType = 8134;
        consensus.fCoinbaseMustBeShielded = true;
        consensus.nSubsidySlowStartInterval = 20000;
        consensus.nPreBlossomSubsidyHalvingInterval = Consensus::PRE_BLOSSOM_HALVING_INTERVAL;
        consensus.nPostBlossomSubsidyHalvingInterval = POST_BLOSSOM_HALVING_INTERVAL(Consensus::PRE_BLOSSOM_HALVING_INTERVAL);
        consensus.nMajorityEnforceBlockUpgrade = 51;
        consensus.nMajorityRejectBlockOutdated = 75;
        consensus.nMajorityWindow = 400;
        // Juno Cash: Legacy Equihash parameters removed (uses RandomX instead)
        // const size_t N = 200, K = 9;
        // static_assert(equihash_parameters_acceptable(N, K));
        // consensus.nEquihashN = N;
        // consensus.nEquihashK = K;
        consensus.powLimit = uint256S("07ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowAveragingWindow = 17;
        assert(maxUint/UintToArith256(consensus.powLimit) >= consensus.nPowAveragingWindow);
        consensus.nPowMaxAdjustDown = 32; // 32% adjustment down
        consensus.nPowMaxAdjustUp = 16; // 16% adjustment up
        consensus.nPreBlossomPowTargetSpacing = Consensus::PRE_BLOSSOM_POW_TARGET_SPACING;
        consensus.nPostBlossomPowTargetSpacing = Consensus::POST_BLOSSOM_POW_TARGET_SPACING;
        consensus.nPowAllowMinDifficultyBlocksAfterHeight = 299187;
        consensus.fPowNoRetargeting = false;
        consensus.vUpgrades[Consensus::BASE_SPROUT].nProtocolVersion = 170002;
        consensus.vUpgrades[Consensus::BASE_SPROUT].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nProtocolVersion = 170002;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
            Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        // Juno Cash: All consensus upgrades active from genesis (Orchard-only chain)
        consensus.vUpgrades[Consensus::UPGRADE_OVERWINTER].nProtocolVersion = 170003;
        consensus.vUpgrades[Consensus::UPGRADE_OVERWINTER].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_SAPLING].nProtocolVersion = 170007;
        consensus.vUpgrades[Consensus::UPGRADE_SAPLING].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nProtocolVersion = 170008;
        consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_HEARTWOOD].nProtocolVersion = 170010;
        consensus.vUpgrades[Consensus::UPGRADE_HEARTWOOD].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_CANOPY].nProtocolVersion = 170012;
        consensus.vUpgrades[Consensus::UPGRADE_CANOPY].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_NU5].nProtocolVersion = 170050;
        consensus.vUpgrades[Consensus::UPGRADE_NU5].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_NU6].nProtocolVersion = 170110;
        consensus.vUpgrades[Consensus::UPGRADE_NU6].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_NU6_1].nProtocolVersion = 170130;
        consensus.vUpgrades[Consensus::UPGRADE_NU6_1].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_ZFUTURE].nProtocolVersion = 0x7FFFFFFF;
        consensus.vUpgrades[Consensus::UPGRADE_ZFUTURE].nActivationHeight =
            Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;

        consensus.nFundingPeriodLength = consensus.nPostBlossomSubsidyHalvingInterval / 48;

        // guarantees the first 2 characters, when base58 encoded, are "tm"
        keyConstants.base58Prefixes[PUBKEY_ADDRESS]     = {0x1D,0x25};
        // guarantees the first 2 characters, when base58 encoded, are "t2"
        keyConstants.base58Prefixes[SCRIPT_ADDRESS]     = {0x1C,0xBA};
        // the first character, when base58 encoded, is "9" or "c" (as in Bitcoin)
        keyConstants.base58Prefixes[SECRET_KEY]         = {0xEF};
        // do not rely on these BIP32 prefixes; they are not specified and may change
        keyConstants.base58Prefixes[EXT_PUBLIC_KEY]     = {0x04,0x35,0x87,0xCF};
        keyConstants.base58Prefixes[EXT_SECRET_KEY]     = {0x04,0x35,0x83,0x94};
        // guarantees the first 2 characters, when base58 encoded, are "zt"
        keyConstants.base58Prefixes[ZCPAYMENT_ADDRESS]  = {0x16,0xB6};
        // guarantees the first 4 characters, when base58 encoded, are "ZiVt"
        keyConstants.base58Prefixes[ZCVIEWING_KEY]      = {0xA8,0xAC,0x0C};
        // guarantees the first 2 characters, when base58 encoded, are "ST"
        keyConstants.base58Prefixes[ZCSPENDING_KEY]     = {0xAC,0x08};

        keyConstants.bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "ztestsapling";
        keyConstants.bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "zviewtestsapling";
        keyConstants.bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "zivktestsapling";
        keyConstants.bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "secret-extended-key-test";
        keyConstants.bech32HRPs[SAPLING_EXTENDED_FVK]         = "zxviewtestsapling";

        keyConstants.bech32mHRPs[TEX_ADDRESS]                 = "textest";

        // Juno Cash: No funding streams (0% dev tax)

        // On testnet we activate this rule 6 blocks after Blossom activation. From block 299188 and
        // prior to Blossom activation, the testnet minimum-difficulty threshold was 15 minutes (i.e.
        // a minimum difficulty block can be mined if no block is mined normally within 15 minutes):
        // <https://zips.z.cash/zip-0205#change-to-difficulty-adjustment-on-testnet>
        // However the median-time-past is 6 blocks behind, and the worst-case time for 7 blocks at a
        // 15-minute spacing is ~105 minutes, which exceeds the limit imposed by the soft fork of
        // 90 minutes.
        //
        // After Blossom, the minimum difficulty threshold time is changed to 6 times the block target
        // spacing, which is 7.5 minutes:
        // <https://zips.z.cash/zip-0208#minimum-difficulty-blocks-on-the-test-network>
        // 7 times that is 52.5 minutes which is well within the limit imposed by the soft fork.

        static_assert(6 * Consensus::POST_BLOSSOM_POW_TARGET_SPACING * 7 < MAX_FUTURE_BLOCK_TIME_MTP - 60,
                      "MAX_FUTURE_BLOCK_TIME_MTP is too low given block target spacing");
        consensus.nFutureTimestampSoftForkHeight = consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nActivationHeight + 6;

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x24cad43");

        pchMessageStart[0] = 0xa7;
        pchMessageStart[1] = 0x23;
        pchMessageStart[2] = 0xe1;
        pchMessageStart[3] = 0x6c;
        vAlertPubKey = ParseHex("048abb146dca9b466d2b5cf82c7582590d4dc376693613804a85dc252ab64e649b3df9b8386bce20f73a6a1de706ce26c815558d1fe8fae1c851bfc7f5453e4d22");
        nDefaultPort = 18234;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(
            1763197808,
            uint256S("0x00000000000000000000000000000000000000000000000000000000000000c0"),
            ParseHex("2f841edd2b5c75d7c2583fa36c459c5e785c55f6c5e48f54f0d195bdc6839a00"),
            0x2000ffff, 4, 0);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x009a83c6bd95d1f0548fe4c5f6555c785e9c456ca33f58c2d7755c2bdd1e842f"));
        assert(genesis.hashMerkleRoot == uint256S("0xe53f8696814445c3b5668803e8f2475004dc9c00c05c68a67f92c731c1432c67"));

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.push_back(CDNSSeedData("junomoneta.io", "dnsseed.testnet.junomoneta.io"));
	vSeeds.push_back(CDNSSeedData("juno.cash", "dnsseed.testnet.juno.cash"));

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_test, pnSeed6_test + ARRAYLEN(pnSeed6_test));

        fMiningRequiresPeers = true;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fMineBlocksOnDemand = false;
        fTestnetToBeDeprecatedFieldRPC = true;


        // Juno Cash: Reset checkpoint data for new chain
        checkpointData = (CCheckpointData) {
            boost::assign::map_list_of
            (0, consensus.hashGenesisBlock),
            0,  // UNIX timestamp of last checkpoint block
            0,  // total number of transactions between genesis and last checkpoint
            0   // estimated number of transactions per day after checkpoint
        };

        // Juno Cash: Reset Sprout value pool checkpoint (no Sprout activity on new chain)
        nSproutValuePoolCheckpointHeight = 0;
        nSproutValuePoolCheckpointBalance = 0;
        fZIP209Enabled = true;
        hashSproutValuePoolCheckpointBlock = consensus.hashGenesisBlock;

        // Juno Cash: No founders reward (0% dev tax)
        vFoundersRewardAddress = {};
    }
};
static CTestNetParams testNetParams;

/**
 * Regression test
 */
class CRegTestParams : public CChainParams {
public:
    CRegTestParams() {
        keyConstants.strNetworkID = "regtest";
        strCurrencyUnits = "RJUNO";
        keyConstants.bip44CoinType = 8135;
        consensus.fCoinbaseMustBeShielded = true;
        consensus.nSubsidySlowStartInterval = 0;
        consensus.nPreBlossomSubsidyHalvingInterval = Consensus::PRE_BLOSSOM_REGTEST_HALVING_INTERVAL;
        consensus.nPostBlossomSubsidyHalvingInterval = POST_BLOSSOM_HALVING_INTERVAL(Consensus::PRE_BLOSSOM_REGTEST_HALVING_INTERVAL);
        consensus.nMajorityEnforceBlockUpgrade = 750;
        consensus.nMajorityRejectBlockOutdated = 950;
        consensus.nMajorityWindow = 1000;
        // Juno Cash: Legacy Equihash parameters removed (uses RandomX instead)
        // const size_t N = 48, K = 5;
        // static_assert(equihash_parameters_acceptable(N, K));
        // consensus.nEquihashN = N;
        // consensus.nEquihashK = K;
        consensus.powLimit = uint256S("0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f0f"); // if this is any larger, the for loop in GetNextWorkRequired can overflow bnTot
        consensus.nPowAveragingWindow = 17;
        assert(maxUint/UintToArith256(consensus.powLimit) >= consensus.nPowAveragingWindow);
        consensus.nPowMaxAdjustDown = 0; // Turn off adjustment down
        consensus.nPowMaxAdjustUp = 0; // Turn off adjustment up
        consensus.nPreBlossomPowTargetSpacing = Consensus::PRE_BLOSSOM_POW_TARGET_SPACING;
        consensus.nPostBlossomPowTargetSpacing = Consensus::POST_BLOSSOM_POW_TARGET_SPACING;
        consensus.nPowAllowMinDifficultyBlocksAfterHeight = 0;
        consensus.fPowNoRetargeting = true;
        consensus.vUpgrades[Consensus::BASE_SPROUT].nProtocolVersion = 170002;
        consensus.vUpgrades[Consensus::BASE_SPROUT].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nProtocolVersion = 170002;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
            Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        // Juno Cash: All consensus upgrades active from genesis (Orchard-only chain)
        consensus.vUpgrades[Consensus::UPGRADE_OVERWINTER].nProtocolVersion = 170003;
        consensus.vUpgrades[Consensus::UPGRADE_OVERWINTER].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_SAPLING].nProtocolVersion = 170006;
        consensus.vUpgrades[Consensus::UPGRADE_SAPLING].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nProtocolVersion = 170008;
        consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_HEARTWOOD].nProtocolVersion = 170010;
        consensus.vUpgrades[Consensus::UPGRADE_HEARTWOOD].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_CANOPY].nProtocolVersion = 170012;
        consensus.vUpgrades[Consensus::UPGRADE_CANOPY].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_NU5].nProtocolVersion = 170050;
        consensus.vUpgrades[Consensus::UPGRADE_NU5].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_NU6].nProtocolVersion = 170110;
        consensus.vUpgrades[Consensus::UPGRADE_NU6].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_NU6_1].nProtocolVersion = 170130;
        consensus.vUpgrades[Consensus::UPGRADE_NU6_1].nActivationHeight =
            Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_ZFUTURE].nProtocolVersion = 0x7FFFFFFF;
        consensus.vUpgrades[Consensus::UPGRADE_ZFUTURE].nActivationHeight =
            Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;

        consensus.nFundingPeriodLength = consensus.nPostBlossomSubsidyHalvingInterval / 48;
        // Defined funding streams can be enabled with node config flags.

        // These prefixes are the same as the testnet prefixes
        keyConstants.base58Prefixes[PUBKEY_ADDRESS]     = {0x1D,0x25};
        keyConstants.base58Prefixes[SCRIPT_ADDRESS]     = {0x1C,0xBA};
        keyConstants.base58Prefixes[SECRET_KEY]         = {0xEF};
        // do not rely on these BIP32 prefixes; they are not specified and may change
        keyConstants.base58Prefixes[EXT_PUBLIC_KEY]     = {0x04,0x35,0x87,0xCF};
        keyConstants.base58Prefixes[EXT_SECRET_KEY]     = {0x04,0x35,0x83,0x94};
        keyConstants.base58Prefixes[ZCPAYMENT_ADDRESS]  = {0x16,0xB6};
        keyConstants.base58Prefixes[ZCVIEWING_KEY]      = {0xA8,0xAC,0x0C};
        keyConstants.base58Prefixes[ZCSPENDING_KEY]     = {0xAC,0x08};

        keyConstants.bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "zregtestsapling";
        keyConstants.bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "zviewregtestsapling";
        keyConstants.bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "zivkregtestsapling";
        keyConstants.bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "secret-extended-key-regtest";
        keyConstants.bech32HRPs[SAPLING_EXTENDED_FVK]         = "zxviewregtestsapling";

        keyConstants.bech32mHRPs[TEX_ADDRESS]                 = "texregtest";

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = uint256S("0x00");

        pchMessageStart[0] = 0x81;
        pchMessageStart[1] = 0x1d;
        pchMessageStart[2] = 0x21;
        pchMessageStart[3] = 0xf6;
        nDefaultPort = 18345;
        nPruneAfterHeight = 1000;

        genesis = CreateGenesisBlock(
            1763197807,
            uint256S("0x0000000000000000000000000000000000000000000000000000000000000036"),
            ParseHex("db25196d5e054d96aca793c3acf2e0264da7547fcf0176dc41825eff2895a102"),
            0x200f0f0f, 4, 0);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x02a19528ff5e8241dc7601cf7f54a74d26e0f2acc393a7ac964d055e6d1925db"));
        assert(genesis.hashMerkleRoot == uint256S("0xe53f8696814445c3b5668803e8f2475004dc9c00c05c68a67f92c731c1432c67"));

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //!< Regtest mode doesn't have any DNS seeds.

        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fMineBlocksOnDemand = true;
        fTestnetToBeDeprecatedFieldRPC = false;

        // Juno Cash: Use genesis hash for regtest checkpoint
        checkpointData = (CCheckpointData){
            boost::assign::map_list_of
            (0, consensus.hashGenesisBlock),
            0,
            0,
            0
        };

        // Juno Cash: No founders reward (0% dev tax)
        vFoundersRewardAddress = {};
    }

    void UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex idx, int nActivationHeight)
    {
        assert(idx > Consensus::BASE_SPROUT && idx < Consensus::MAX_NETWORK_UPGRADES);
        consensus.vUpgrades[idx].nActivationHeight = nActivationHeight;
    }

    void UpdateFundingStreamParameters(Consensus::FundingStreamIndex idx, Consensus::FundingStream fs)
    {
        assert(idx >= Consensus::FIRST_FUNDING_STREAM && idx < Consensus::MAX_FUNDING_STREAMS);
        consensus.vFundingStreams[idx] = fs;
    }

    void UpdateOnetimeLockboxDisbursementParameters(
        Consensus::OnetimeLockboxDisbursementIndex idx,
        Consensus::OnetimeLockboxDisbursement ld)
    {
        assert(idx >= Consensus::FIRST_ONETIME_LOCKBOX_DISBURSEMENT && idx < Consensus::MAX_ONETIME_LOCKBOX_DISBURSEMENTS);
        consensus.vOnetimeLockboxDisbursements[idx] = ld;
    }

    void UpdateRegtestPow(
        int64_t nPowMaxAdjustDown,
        int64_t nPowMaxAdjustUp,
        uint256 powLimit,
        bool noRetargeting)
    {
        consensus.nPowMaxAdjustDown = nPowMaxAdjustDown;
        consensus.nPowMaxAdjustUp = nPowMaxAdjustUp;
        consensus.powLimit = powLimit;
        consensus.fPowNoRetargeting = noRetargeting;
    }

    void SetRegTestZIP209Enabled() {
        fZIP209Enabled = true;
    }
};
static CRegTestParams regTestParams;

static const CChainParams* pCurrentParams = nullptr;

const CChainParams& Params() {
    assert(pCurrentParams);
    return *pCurrentParams;
}

const CChainParams& Params(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
            return mainParams;
    else if (chain == CBaseChainParams::TESTNET)
            return testNetParams;
    else if (chain == CBaseChainParams::REGTEST)
            return regTestParams;
    else
        throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    pCurrentParams = &Params(network);

    // Some python qa rpc tests need to enforce the coinbase consensus rule
    if (network == CBaseChainParams::REGTEST && mapArgs.count("-regtestshieldcoinbase")) {
        regTestParams.SetRegTestCoinbaseMustBeShielded();
    }

    // When a developer is debugging turnstile violations in regtest mode, enable ZIP209
    if (network == CBaseChainParams::REGTEST && mapArgs.count("-developersetpoolsizezero")) {
        regTestParams.SetRegTestZIP209Enabled();
    }
}


// Block height must be >0 and <=last founders reward block height
// Index variable i ranges from 0 - (vFoundersRewardAddress.size()-1)
std::string CChainParams::GetFoundersRewardAddressAtHeight(int nHeight) const {
    int preBlossomMaxHeight = consensus.GetLastFoundersRewardBlockHeight(0);
    // zip208
    // FounderAddressAdjustedHeight(height) :=
    // height, if not IsBlossomActivated(height)
    // BlossomActivationHeight + floor((height - BlossomActivationHeight) / BlossomPoWTargetSpacingRatio), otherwise
    bool blossomActive = consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_BLOSSOM);
    if (blossomActive) {
        int blossomActivationHeight = consensus.vUpgrades[Consensus::UPGRADE_BLOSSOM].nActivationHeight;
        nHeight = blossomActivationHeight + ((nHeight - blossomActivationHeight) / Consensus::BLOSSOM_POW_TARGET_SPACING_RATIO);
    }
    assert(nHeight > 0 && nHeight <= preBlossomMaxHeight);
    size_t addressChangeInterval = (preBlossomMaxHeight + vFoundersRewardAddress.size()) / vFoundersRewardAddress.size();
    size_t i = nHeight / addressChangeInterval;
    return vFoundersRewardAddress[i];
}

// Block height must be >0 and <=last founders reward block height
// The founders reward address is expected to be a multisig (P2SH) address
CScript CChainParams::GetFoundersRewardScriptAtHeight(int nHeight) const {
    assert(nHeight > 0 && nHeight <= consensus.GetLastFoundersRewardBlockHeight(nHeight));

    KeyIO keyIO(*this);
    auto address = keyIO.DecodePaymentAddress(GetFoundersRewardAddressAtHeight(nHeight).c_str());
    assert(address.has_value());
    assert(std::holds_alternative<CScriptID>(address.value()));
    CScriptID scriptID = std::get<CScriptID>(address.value());
    CScript script = CScript() << OP_HASH160 << ToByteVector(scriptID) << OP_EQUAL;
    return script;
}

std::string CChainParams::GetFoundersRewardAddressAtIndex(int i) const {
    assert(i >= 0 && i < vFoundersRewardAddress.size());
    return vFoundersRewardAddress[i];
}

std::string CChainParams::GetDefaultDonationAddress() const {
    if (NetworkIDString() == "main") {
        return "t1HuKnfjJdtkMA2dMYpPeFgkMeX3pnLFppA";
    } else if (NetworkIDString() == "test") {
        return "tmJV5QYQZa5wuCQUBd5pTKuWnKvQYhriiHw";
    }
    // Regtest has no default - must be explicitly configured
    return "";
}

void UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex idx, int nActivationHeight)
{
    regTestParams.UpdateNetworkUpgradeParameters(idx, nActivationHeight);
}

void UpdateFundingStreamParameters(Consensus::FundingStreamIndex idx, Consensus::FundingStream fs)
{
    regTestParams.UpdateFundingStreamParameters(idx, fs);
}

void UpdateOnetimeLockboxDisbursementParameters(
    Consensus::OnetimeLockboxDisbursementIndex idx,
    Consensus::OnetimeLockboxDisbursement ld)
{
    regTestParams.UpdateOnetimeLockboxDisbursementParameters(idx, ld);
}

void UpdateRegtestPow(
    int64_t nPowMaxAdjustDown,
    int64_t nPowMaxAdjustUp,
    uint256 powLimit,
    bool noRetargeting)
{
    regTestParams.UpdateRegtestPow(nPowMaxAdjustDown, nPowMaxAdjustUp, powLimit, noRetargeting);
}
