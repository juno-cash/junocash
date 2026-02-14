// Copyright (c) 2019-2023 The Zcash developers
// Copyright (c) 2025 Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include <gtest/gtest.h>

#include "arith_uint256.h"
#include "mempool_limit.h"
#include "util/time.h"
#include "zip317.h"


const uint256 TX_ID1 = ArithToUint256(1);
const uint256 TX_ID2 = ArithToUint256(2);
const uint256 TX_ID3 = ArithToUint256(3);

TEST(MempoolLimitTests, RecentlyEvictedListAddWrapsAfterMaxSize)
{
    FixedClock clock(std::chrono::seconds(1));
    RecentlyEvictedList recentlyEvicted(&clock, 2, 100);
    recentlyEvicted.add(TX_ID1);
    recentlyEvicted.add(TX_ID2);
    recentlyEvicted.add(TX_ID3);
    // tx 1 should be overwritten by tx 3 due to maxSize 2
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID1));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID2));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID3));
}

TEST(MempoolLimitTests, RecentlyEvictedListDoesNotContainAfterExpiry)
{
    FixedClock clock(std::chrono::seconds(1));
    // maxSize=3, timeToKeep=1
    RecentlyEvictedList recentlyEvicted(&clock, 3, 1);
    recentlyEvicted.add(TX_ID1);
    clock.Set(std::chrono::seconds(2));
    recentlyEvicted.add(TX_ID2);
    recentlyEvicted.add(TX_ID3);
    // After 1 second the txId will still be there
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID1));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID2));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID3));
    clock.Set(std::chrono::seconds(3));
    // After 2 seconds it is gone
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID1));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID2));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID3));
    clock.Set(std::chrono::seconds(4));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID1));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID2));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID3));
}

TEST(MempoolLimitTests, RecentlyEvictedDropOneAtATime)
{
    FixedClock clock(std::chrono::seconds(1));
    RecentlyEvictedList recentlyEvicted(&clock, 3, 2);
    recentlyEvicted.add(TX_ID1);
    clock.Set(std::chrono::seconds(2));
    recentlyEvicted.add(TX_ID2);
    clock.Set(std::chrono::seconds(3));
    recentlyEvicted.add(TX_ID3);
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID1));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID2));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID3));
    clock.Set(std::chrono::seconds(4));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID1));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID2));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID3));
    clock.Set(std::chrono::seconds(5));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID1));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID2));
    EXPECT_TRUE(recentlyEvicted.contains(TX_ID3));
    clock.Set(std::chrono::seconds(6));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID1));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID2));
    EXPECT_FALSE(recentlyEvicted.contains(TX_ID3));
}

TEST(MempoolLimitTests, MempoolLimitTxSetCheckSizeAfterDropping)
{
    std::set<uint256> testedDropping;
    // Run the test until we have tested dropping each of the elements
    int trialNum = 0;
    while (testedDropping.size() < 3) {
        MempoolLimitTxSet limitSet(MIN_TX_COST * 2);
        EXPECT_EQ(0, limitSet.getTotalWeight());
        limitSet.add(TX_ID1, MIN_TX_COST, MIN_TX_COST);
        EXPECT_EQ(10000, limitSet.getTotalWeight());
        limitSet.add(TX_ID2, MIN_TX_COST, MIN_TX_COST);
        EXPECT_EQ(20000, limitSet.getTotalWeight());
        EXPECT_FALSE(limitSet.maybeDropRandom().has_value());
        limitSet.add(TX_ID3, MIN_TX_COST, MIN_TX_COST + LOW_FEE_PENALTY);
        EXPECT_EQ(30000 + LOW_FEE_PENALTY, limitSet.getTotalWeight());
        std::optional<uint256> drop = limitSet.maybeDropRandom();
        ASSERT_TRUE(drop.has_value());
        uint256 txid = drop.value();
        testedDropping.insert(txid);
        // Do not continue to test if a particular trial fails
        ASSERT_EQ(txid == TX_ID3 ? 20000 : 20000 + LOW_FEE_PENALTY, limitSet.getTotalWeight());
    }
}

TEST(MempoolLimitTests, CalculateConventionalFeeShielding)
{
    // Standard conventional fee uses MARGINAL_FEE
    EXPECT_EQ(MARGINAL_FEE * GRACE_ACTIONS, CalculateConventionalFee(2));
    EXPECT_EQ(MARGINAL_FEE * 11, CalculateConventionalFee(11));
    EXPECT_EQ(MARGINAL_FEE * GRACE_ACTIONS, CalculateConventionalFee(2, false));

    // Shielding conventional fee uses SHIELDING_MARGINAL_FEE
    EXPECT_EQ(SHIELDING_MARGINAL_FEE * GRACE_ACTIONS, CalculateConventionalFee(2, true));
    EXPECT_EQ(SHIELDING_MARGINAL_FEE * 11, CalculateConventionalFee(11, true));

    // Shielding fee is always less than standard fee
    EXPECT_LT(CalculateConventionalFee(2, true), CalculateConventionalFee(2, false));
    EXPECT_LT(CalculateConventionalFee(11, true), CalculateConventionalFee(11, false));
}
