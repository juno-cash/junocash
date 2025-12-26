// Copyright (c) 2022-2023 The Zcash developers
// Copyright (c) 2025 Juno Cash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "wallet/orchard.h"
#include "util/system.h"

std::optional<libzcash::OrchardSpendingKey> OrchardWallet::GetSpendingKeyForAddress(
    const libzcash::OrchardRawAddress& addr) const
{
    auto skPtr = orchard_wallet_get_spending_key_for_address(inner.get(), addr.inner.get());
    if (skPtr == nullptr) return std::nullopt;
    return libzcash::OrchardSpendingKey(skPtr);
}

std::vector<std::pair<libzcash::OrchardSpendingKey, orchard::SpendInfo>> OrchardWallet::GetSpendInfo(
    const std::vector<OrchardNoteMetadata>& noteMetadata,
    unsigned int anchorConfirmations,
    const uint256& anchor) const
{
    std::vector<std::pair<libzcash::OrchardSpendingKey, orchard::SpendInfo>> result;
    auto walletAnchor = GetAnchorWithConfirmations(anchorConfirmations);

    // Debug logging for anchor mismatch diagnosis
    if (!walletAnchor.has_value()) {
        LogPrintf("ERROR: Orchard anchor not found at %d confirmations. "
                  "Last checkpoint height: %s\n",
                  anchorConfirmations,
                  GetLastCheckpointHeight().has_value()
                      ? std::to_string(GetLastCheckpointHeight().value())
                      : "none");
    } else if (walletAnchor.value() != anchor) {
        LogPrintf("ERROR: Orchard anchor mismatch at %d confirmations:\n"
                  "  Wallet anchor: %s\n"
                  "  Chain anchor:  %s\n"
                  "  Last checkpoint height: %s\n",
                  anchorConfirmations,
                  walletAnchor.value().GetHex(),
                  anchor.GetHex(),
                  GetLastCheckpointHeight().has_value()
                      ? std::to_string(GetLastCheckpointHeight().value())
                      : "none");
    }

    assert(walletAnchor.has_value() && walletAnchor.value() == anchor);

    for (const auto& note : noteMetadata) {
        auto pSpendInfo = orchard_wallet_get_spend_info(
            inner.get(),
            note.GetOutPoint().hash.begin(),
            note.GetOutPoint().n,
            anchorConfirmations - 1);
        if (pSpendInfo == nullptr) {
            throw std::logic_error("Called OrchardWallet::GetSpendInfo with unknown outpoint");
        } else {
            auto spendInfo = orchard::SpendInfo(
                pSpendInfo,
                note.GetAddress(),
                note.GetNoteValue());

            auto sk = GetSpendingKeyForAddress(note.GetAddress());
            if (sk.has_value()) {
                result.push_back(std::pair(std::move(sk.value()), std::move(spendInfo)));
            } else {
                throw std::logic_error("Unknown spending key for given outpoint");
            }
        }
    }
    return result;
}
