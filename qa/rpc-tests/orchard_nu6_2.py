#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""
Integration test for Orchard across the NU6.2 activation.

NU6.2 changes the Orchard circuit, which changes the verifying key. A node
selects the verifying key for Orchard proof batch validation by whether NU6.2 is
active at the block being validated: the NU6.2-onward fixed key from NU6.2, and
the pre-NU6.2 key before it.
"""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.mininode import COIN
from test_framework.util import (
    BLOSSOM_BRANCH_ID,
    CANOPY_BRANCH_ID,
    HEARTWOOD_BRANCH_ID,
    NU5_BRANCH_ID,
    NU6_BRANCH_ID,
    NU6_1_BRANCH_ID,
    NU6_2_BRANCH_ID,
    assert_equal,
    connect_nodes_bi,
    nuparams,
    start_node,
    start_nodes,
    stop_node,
    wait_and_assert_operationid_status,
)
from test_framework.zip317 import conventional_fee


class OrchardNU6_2Test(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 3
        self.cache_behavior = 'clean'

    def network_upgrade_args(self):
        return [
            nuparams(BLOSSOM_BRANCH_ID, 1),
            nuparams(HEARTWOOD_BRANCH_ID, 1),
            nuparams(CANOPY_BRANCH_ID, 1),
            nuparams(NU5_BRANCH_ID, 2),
            nuparams(NU6_BRANCH_ID, 3),
            nuparams(NU6_1_BRANCH_ID, 4),
            nuparams(NU6_2_BRANCH_ID, 5),
        ]

    def setup_nodes(self):
        return start_nodes(
            self.num_nodes,
            self.options.tmpdir,
            extra_args=[self.network_upgrade_args()] * self.num_nodes)

    def run_test(self):
        acct1 = self.nodes[1].z_getnewaccount()['account']
        ua1 = self.nodes[1].z_getaddressforaccount(acct1, ['orchard'])['address']
        assert_equal(
            {'pools': {}, 'minimum_confirmations': 1},
            self.nodes[1].z_getbalanceforaccount(acct1))

        acct2 = self.nodes[2].z_getnewaccount()['account']
        ua2 = self.nodes[2].z_getaddressforaccount(acct2, ['orchard'])['address']

        stop_node(self.nodes[1], 1)
        self.nodes[1] = start_node(
            1,
            self.options.tmpdir,
            self.network_upgrade_args() + ["-mineraddress=%s" % ua1])
        connect_nodes_bi(self.nodes, 0, 1)
        connect_nodes_bi(self.nodes, 1, 2)

        self.nodes[0].generate(3)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), 3)

        self.nodes[1].generate(1)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), 4)

        acct1_balance = self.nodes[1].z_getbalanceforaccount(acct1)
        coinbase_zats = acct1_balance['pools']['orchard']['valueZat']
        assert coinbase_zats > 0
        assert_equal(
            {'pools': {'orchard': {'valueZat': coinbase_zats}}, 'minimum_confirmations': 1},
            acct1_balance)

        spend_fee = conventional_fee(2)
        spend_amount = Decimal('1')
        opid = self.nodes[1].z_sendmany(
            ua1, [{"address": ua2, "amount": spend_amount}], 1, spend_fee)
        wait_and_assert_operationid_status(self.nodes[1], opid)

        self.sync_all()
        assert_equal(len(self.nodes[1].getrawmempool()), 1)

        self.nodes[1].generate(1)
        self.sync_all()
        assert_equal(len(self.nodes[1].getrawmempool()), 0)

        assert_equal(
            {'pools': {'orchard': {'valueZat': spend_amount * COIN}}, 'minimum_confirmations': 1},
            self.nodes[2].z_getbalanceforaccount(acct2))
        assert_equal(
            {'pools': {'orchard': {'valueZat': coinbase_zats - ((spend_amount + spend_fee) * COIN)}}, 'minimum_confirmations': 1},
            self.nodes[1].z_getbalanceforaccount(acct1))
        assert_equal(self.nodes[0].getblockcount(), 5)


if __name__ == '__main__':
    OrchardNU6_2Test().main()
