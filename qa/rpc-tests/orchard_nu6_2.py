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
    get_coinbase_address,
    nuparams,
    start_nodes,
    wait_and_assert_operationid_status,
)
from test_framework.zip317 import conventional_fee


class OrchardNU6_2Test(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 3
        self.cache_behavior = 'clean'

    def setup_nodes(self):
        return start_nodes(self.num_nodes, self.options.tmpdir, extra_args=[[
            nuparams(BLOSSOM_BRANCH_ID, 1),
            nuparams(HEARTWOOD_BRANCH_ID, 5),
            nuparams(CANOPY_BRANCH_ID, 5),
            nuparams(NU5_BRANCH_ID, 10),
            nuparams(NU6_BRANCH_ID, 20),
            nuparams(NU6_1_BRANCH_ID, 30),
            nuparams(NU6_2_BRANCH_ID, 105),
        ]] * self.num_nodes)

    def run_test(self):
        self.nodes[0].generate(103)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), 103)

        acct1 = self.nodes[1].z_getnewaccount()['account']
        ua1 = self.nodes[1].z_getaddressforaccount(acct1, ['orchard'])['address']
        assert_equal(
            {'pools': {}, 'minimum_confirmations': 1},
            self.nodes[1].z_getbalanceforaccount(acct1))

        acct2 = self.nodes[2].z_getnewaccount()['account']
        ua2 = self.nodes[2].z_getaddressforaccount(acct2, ['orchard'])['address']

        coinbase_fee = conventional_fee(3)
        coinbase_amount = Decimal('10') - coinbase_fee
        opid = self.nodes[0].z_sendmany(
            get_coinbase_address(self.nodes[0]),
            [{"address": ua1, "amount": coinbase_amount}],
            1, coinbase_fee, 'AllowRevealedSenders')
        wait_and_assert_operationid_status(self.nodes[0], opid)

        self.sync_all()
        self.nodes[0].generate(1)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), 104)

        assert_equal(
            {'pools': {'orchard': {'valueZat': coinbase_amount * COIN}}, 'minimum_confirmations': 1},
            self.nodes[1].z_getbalanceforaccount(acct1))

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
            {'pools': {'orchard': {'valueZat': (coinbase_amount - spend_amount - spend_fee) * COIN}}, 'minimum_confirmations': 1},
            self.nodes[1].z_getbalanceforaccount(acct1))
        assert_equal(self.nodes[0].getblockcount(), 105)


if __name__ == '__main__':
    OrchardNU6_2Test().main()
