#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "gtest/utils.h"
#include "main.h"
#include "random.h"
#include "test/test_util.h"
#include "transaction_builder.h"
#include "util/test.h"
#include "wallet/orchard.h"
#include "zcash/Address.hpp"

#include <ios>
#include <optional>

using namespace libzcash;

OrchardSpendingKey RandomOrchardSpendingKey() {
    auto coinType = Params().BIP44CoinType() ;

    auto seed = MnemonicSeed::Random(coinType);
    return OrchardSpendingKey::ForAccount(seed, coinType, 0);
}

CTransaction FakeOrchardTx(const OrchardSpendingKey& sk, libzcash::diversifier_index_t j) {
    CBasicKeyStore keystore;
    CKey tsk = AddTestCKeyToKeyStore(keystore);
    auto scriptPubKey = GetScriptForDestination(tsk.GetPubKey().GetID());

    auto fvk = sk.ToFullViewingKey();
    auto ivk = fvk.ToIncomingViewingKey();
    auto recipient = ivk.Address(j);
    auto orchardAnchor = uint256();

    // Create a shielding transaction from transparent to Orchard
    // 0.0005 t-ZEC in, 0.0004 z-ZEC out, 0.0001 fee
    auto builder = TransactionBuilder(Params(), 1, orchardAnchor, SaplingMerkleTree::empty_root(), &keystore);
    builder.SetFee(10000);
    builder.AddTransparentInput(COutPoint(uint256S("1234"), 0), scriptPubKey, 50000);
    builder.AddOrchardOutput(std::nullopt, recipient, 40000, std::nullopt);

    auto maybeTx = builder.Build();
    EXPECT_TRUE(maybeTx.IsTx());
    return maybeTx.GetTxOrThrow();
}

TEST(OrchardWalletTests, TxInvolvesMyNotes) {
    auto consensusParams = RegtestActivateNU5();
    OrchardWallet wallet;

    // Add a new spending key to the wallet
    auto sk = RandomOrchardSpendingKey();
    wallet.AddSpendingKey(sk);

    // Create a transaction sending to the default address for that
    // spending key and add it to the wallet.
    auto tx = FakeOrchardTx(sk, libzcash::diversifier_index_t(0));
    wallet.AddNotesIfInvolvingMe(tx);

    // Check that we detect the transaction as ours
    EXPECT_TRUE(wallet.TxInvolvesMyNotes(tx.GetHash()));

    // Create a transaction sending to a different diversified address
    auto tx1 = FakeOrchardTx(sk, libzcash::diversifier_index_t(0xffffffffffffffff));
    wallet.AddNotesIfInvolvingMe(tx1);

    // Check that we also detect this transaction as ours
    EXPECT_TRUE(wallet.TxInvolvesMyNotes(tx1.GetHash()));

    // Now generate a new key, and send a transaction to it without adding
    // the key to the wallet; it should not be detected as ours.
    auto skNotOurs = RandomOrchardSpendingKey();
    auto tx2 = FakeOrchardTx(skNotOurs, libzcash::diversifier_index_t(0));
    wallet.AddNotesIfInvolvingMe(tx2);
    EXPECT_FALSE(wallet.TxInvolvesMyNotes(tx2.GetHash()));

    RegtestDeactivateNU5();
}

void BuildOrchardSpend(CTransaction& outTx, int nHeight = 2, std::optional<bool> useFixedCircuitForProving = std::nullopt) {
    OrchardWallet wallet;
    auto sk = RandomOrchardSpendingKey();
    wallet.AddSpendingKey(sk);

    libzcash::diversifier_index_t j(0);
    auto txRecv = FakeOrchardTx(sk, j);
    wallet.AddNotesIfInvolvingMe(txRecv);

    auto recipient = RandomOrchardSpendingKey()
        .ToFullViewingKey()
        .ToIncomingViewingKey()
        .Address(j);

    std::vector<OrchardNoteMetadata> notes;
    wallet.GetFilteredNotes(
        notes, sk.ToFullViewingKey().ToIncomingViewingKey(), true, true);
    ASSERT_EQ(notes.size(), 1);
    ASSERT_THROW(wallet.GetSpendInfo(notes, 1, wallet.GetLatestAnchor()), std::logic_error);

    CBlock fakeBlock;
    fakeBlock.vtx.resize(2);
    fakeBlock.vtx[1] = txRecv;
    ASSERT_TRUE(wallet.AppendNoteCommitments(2, fakeBlock));

    auto spendInfo = wallet.GetSpendInfo(notes, 1, wallet.GetLatestAnchor());
    ASSERT_EQ(spendInfo[0].second.Value(), 40000);

    OrchardMerkleFrontier tree;
    tree.AppendBundle(txRecv.GetOrchardBundle());

    CTransaction tx;
    if (useFixedCircuitForProving.has_value()) {
        auto orchardBuilder = orchard::Builder(false, tree.root(), useFixedCircuitForProving.value());
        ASSERT_TRUE(orchardBuilder.AddSpend(std::move(spendInfo[0].second)));
        orchardBuilder.AddOutput(std::nullopt, recipient, 25000, std::nullopt);
        auto orchardBundle = orchardBuilder.Build();
        ASSERT_TRUE(orchardBundle.has_value());

        CMutableTransaction mtx = CreateNewContextualCMutableTransaction(
            Params().GetConsensus(), nHeight, false);

        auto saplingBuilder = sapling::new_builder(
            *Params().RustNetwork(),
            nHeight,
            SaplingMerkleTree::empty_root().ToRawBytes(),
            false);
        auto maybeSaplingBundle = sapling::build_bundle(std::move(saplingBuilder));
        ASSERT_TRUE(maybeSaplingBundle.has_value());
        auto saplingBundle = std::move(maybeSaplingBundle.value());

        auto dataToBeSigned = ProduceShieldedSignatureHash(
            CurrentEpochBranchId(nHeight, Params().GetConsensus()),
            mtx,
            {},
            *saplingBundle,
            orchardBundle);
        auto authorizedBundle = orchardBundle.value().ProveAndSign({sk}, dataToBeSigned);
        ASSERT_TRUE(authorizedBundle.has_value());
        mtx.orchardBundle = authorizedBundle.value();
        tx = CTransaction(mtx);
    } else {
        auto builder = TransactionBuilder(Params(), nHeight, tree.root(), SaplingMerkleTree::empty_root());
        ASSERT_TRUE(builder.AddOrchardSpend(sk, std::move(spendInfo[0].second)));
        builder.AddOrchardOutput(std::nullopt, recipient, 25000, std::nullopt);
        auto maybeTx = builder.Build();
        ASSERT_TRUE(maybeTx.IsTx());
        tx = maybeTx.GetTxOrThrow();
    }

    ASSERT_EQ(tx.vin.size(), 0);
    ASSERT_EQ(tx.vout.size(), 0);
    ASSERT_EQ(tx.vJoinSplit.size(), 0);
    ASSERT_EQ(tx.GetSaplingSpendsCount(), 0);
    ASSERT_EQ(tx.GetSaplingOutputsCount(), 0);
    ASSERT_TRUE(tx.GetOrchardBundle().IsPresent());
    ASSERT_EQ(tx.GetOrchardBundle().GetValueBalance(), 1000);
    outTx = tx;
}

void MakeOrchardProofNonCanonicalBytes(const CTransaction& tx, std::vector<unsigned char>& outBytes) {
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << tx;
    std::vector<unsigned char> bytes(ss.begin(), ss.end());

    size_t pos = 4 + 4 + 4 + 4 + 4;
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(bytes[pos], 0);
        pos += 1;
    }

    size_t nActions = bytes[pos];
    ASSERT_GT(nActions, 0u);
    ASSERT_LT(nActions, 253u);
    pos += 1 + nActions * 820;
    pos += 1 + 8 + 32;

    ASSERT_EQ(bytes[pos], 0xfd);
    size_t proofLen = bytes[pos + 1] | (bytes[pos + 2] << 8);
    size_t newProofLen = proofLen + 1;
    ASSERT_LT(newProofLen, 0x10000u);
    bytes[pos + 1] = newProofLen & 0xff;
    bytes[pos + 2] = (newProofLen >> 8) & 0xff;
    bytes.insert(bytes.begin() + pos + 3, 0);

    outBytes = bytes;
}

void SetConsensusBranchId(std::vector<unsigned char>& bytes, uint32_t branchId) {
    for (int i = 0; i < 4; i++) {
        bytes[8 + i] = (branchId >> (8 * i)) & 0xff;
    }
}

bool TryDeserializeTx(const std::vector<unsigned char>& bytes) {
    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    try {
        CTransaction tx;
        ss >> tx;
        return true;
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

bool OrchardAuthorizationValidWithKey(const CTransaction& tx, uint32_t consensusBranchId, bool nu6point2Active) {
    const PrecomputedTransactionData txdata(tx, {});
    CValidationState state;
    AssumeShieldedInputsExistAndAreSpendable baseView;
    CCoinsViewCache view(&baseView);
    std::optional<rust::Box<sapling::BatchValidator>> saplingAuth = sapling::init_batch_validator(false);
    std::optional<rust::Box<orchard::BatchValidator>> orchardAuth =
        orchard::init_batch_validator(false, nu6point2Active);

    EXPECT_TRUE(ContextualCheckShieldedInputs(
        tx,
        txdata,
        state,
        view,
        saplingAuth,
        orchardAuth,
        Params().GetConsensus(),
        consensusBranchId,
        true,
        true));
    EXPECT_EQ(state.GetRejectReason(), "");
    return orchardAuth.value()->validate();
}

// This test is here instead of test_transaction_builder.cpp because it depends
// on OrchardWallet, which only exists if the wallet is compiled in.
TEST(TransactionBuilder, OrchardToOrchard) {
    LoadProofParameters();

    auto consensusParams = RegtestActivateNU5();
    OrchardWallet wallet;

    CBasicKeyStore keystore;
    CKey tsk = AddTestCKeyToKeyStore(keystore);
    auto scriptPubKey = GetScriptForDestination(tsk.GetPubKey().GetID());

    auto sk = RandomOrchardSpendingKey();
    wallet.AddSpendingKey(sk);

    // Create a transaction sending to the default address for that
    // spending key and add it to the wallet.
    libzcash::diversifier_index_t j(0);
    auto txRecv = FakeOrchardTx(sk, j);
    wallet.AddNotesIfInvolvingMe(txRecv);

    // Generate a recipient.
    auto recipient = RandomOrchardSpendingKey()
        .ToFullViewingKey()
        .ToIncomingViewingKey()
        .Address(j);

    // Select the one note in the wallet for spending.
    std::vector<OrchardNoteMetadata> notes;
    wallet.GetFilteredNotes(
        notes, sk.ToFullViewingKey().ToIncomingViewingKey(), true, true);
    ASSERT_EQ(notes.size(), 1);

    // If we attempt to get spend info now, it will fail because the note hasn't
    // been witnessed in the Orchard commitment tree.
    EXPECT_THROW(wallet.GetSpendInfo(notes, 1, wallet.GetLatestAnchor()), std::logic_error);

    // Append the bundle to the wallet's commitment tree.
    CBlock fakeBlock;
    fakeBlock.vtx.resize(2);
    fakeBlock.vtx[1] = txRecv;
    ASSERT_TRUE(wallet.AppendNoteCommitments(2, fakeBlock));

    // Now we can get spend info for the note.
    auto spendInfo = wallet.GetSpendInfo(notes, 1, wallet.GetLatestAnchor());
    EXPECT_EQ(spendInfo[0].second.Value(), 40000);

    // Get the root of the commitment tree.
    OrchardMerkleFrontier tree;
    tree.AppendBundle(txRecv.GetOrchardBundle());
    auto orchardAnchor = tree.root();

    // Create an Orchard-only transaction
    // 0.0004 z-ZEC in, 0.00025 z-ZEC out, default fee, 0.00014 z-ZEC change
    auto builder = TransactionBuilder(Params(), 2, orchardAnchor, SaplingMerkleTree::empty_root());
    EXPECT_TRUE(builder.AddOrchardSpend(sk, std::move(spendInfo[0].second)));
    builder.AddOrchardOutput(std::nullopt, recipient, 25000, std::nullopt);
    auto maybeTx = builder.Build();
    EXPECT_TRUE(maybeTx.IsTx());
    if (maybeTx.IsError()) {
        std::cerr << "Failed to build transaction: " << maybeTx.GetError() << std::endl;
        GTEST_FAIL();
    }
    auto tx = maybeTx.GetTxOrThrow();

    EXPECT_EQ(tx.vin.size(), 0);
    EXPECT_EQ(tx.vout.size(), 0);
    EXPECT_EQ(tx.vJoinSplit.size(), 0);
    EXPECT_EQ(tx.GetSaplingSpendsCount(), 0);
    EXPECT_EQ(tx.GetSaplingOutputsCount(), 0);
    EXPECT_TRUE(tx.GetOrchardBundle().IsPresent());
    EXPECT_EQ(tx.GetOrchardBundle().GetValueBalance(), 1000);

    CValidationState state;
    EXPECT_TRUE(ContextualCheckTransaction(tx, state, Params(), 3, true));
    EXPECT_EQ(state.GetRejectReason(), "");

    // Revert to default
    RegtestDeactivateNU5();
}

TEST(TransactionBuilder, OrchardNonCanonicalProofSizeRejectedFromNU6point2) {
    LoadProofParameters();

    auto consensusParams = RegtestActivateNU6point2(false, 1);
    CTransaction tx;
    BuildOrchardSpend(tx);

    {
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << tx;
        std::vector<unsigned char> bytes(ss.begin(), ss.end());
        EXPECT_TRUE(TryDeserializeTx(bytes));
    }

    std::vector<unsigned char> tampered;
    ASSERT_NO_FATAL_FAILURE(MakeOrchardProofNonCanonicalBytes(tx, tampered));
    EXPECT_FALSE(TryDeserializeTx(tampered));

    RegtestDeactivateNU6point2();
}

TEST(TransactionBuilder, OrchardNonCanonicalProofSizeAllowedBeforeNU6point2) {
    LoadProofParameters();

    auto consensusParams = RegtestActivateNU6point2(false, 1);
    CTransaction tx;
    BuildOrchardSpend(tx);

    std::vector<unsigned char> tampered;
    ASSERT_NO_FATAL_FAILURE(MakeOrchardProofNonCanonicalBytes(tx, tampered));
    ASSERT_FALSE(TryDeserializeTx(tampered));

    SetConsensusBranchId(tampered, NetworkUpgradeInfo[Consensus::UPGRADE_NU6_1].nBranchId);
    EXPECT_TRUE(TryDeserializeTx(tampered));

    RegtestDeactivateNU6point2();
}

TEST(TransactionBuilder, OrchardPreNu6point2CircuitRejectedFromNU6point2) {
    LoadProofParameters();

    const int hardForkHeight = 3;
    auto consensusParams = RegtestActivateNU6point2(false, hardForkHeight);
    auto nu6point2BranchId = CurrentEpochBranchId(hardForkHeight, Params().GetConsensus());

    CTransaction tx;
    BuildOrchardSpend(tx, hardForkHeight, false);

    EXPECT_TRUE(OrchardAuthorizationValidWithKey(tx, nu6point2BranchId, false));
    EXPECT_FALSE(OrchardAuthorizationValidWithKey(tx, nu6point2BranchId, true));

    RegtestDeactivateNU6point2();
}
