// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin Core developers
// Copyright (c) 2016-2025 The Zcash developers
// Copyright (c) 2025 Junocash developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#ifndef BITCOIN_ALERT_H
#define BITCOIN_ALERT_H

#include <string>

// Local alerting via -alertnotify. The P2P network alert system has been removed.
class CAlert
{
public:
    static void Notify(const std::string& strMessage, bool fThread);
};

#endif // BITCOIN_ALERT_H
