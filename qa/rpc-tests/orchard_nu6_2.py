#!/usr/bin/env python3
# Copyright (c) 2026 The Zcash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

"""RPC smoke test for NU6.2 activation wiring."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    BLOSSOM_BRANCH_ID,
    CANOPY_BRANCH_ID,
    HEARTWOOD_BRANCH_ID,
    NU5_BRANCH_ID,
    NU6_BRANCH_ID,
    NU6_1_BRANCH_ID,
    NU6_2_BRANCH_ID,
    assert_equal,
    nuparams,
    start_nodes,
)


def branch_id(branch_id_int):
    return '%08x' % branch_id_int


class OrchardNU6_2Test(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 2
        self.cache_behavior = 'clean'

    def network_upgrade_args(self, predecessor_height, nu6_2_height):
        return [
            nuparams(BLOSSOM_BRANCH_ID, predecessor_height),
            nuparams(HEARTWOOD_BRANCH_ID, predecessor_height),
            nuparams(CANOPY_BRANCH_ID, predecessor_height),
            nuparams(NU5_BRANCH_ID, predecessor_height),
            nuparams(NU6_BRANCH_ID, predecessor_height),
            nuparams(NU6_1_BRANCH_ID, predecessor_height),
            nuparams(NU6_2_BRANCH_ID, nu6_2_height),
        ]

    def setup_nodes(self):
        return start_nodes(
            self.num_nodes,
            self.options.tmpdir,
            extra_args=[
                self.network_upgrade_args(1, 2),
                self.network_upgrade_args(0, 0),
            ])

    def run_test(self):
        pending = self.nodes[0].getblockchaininfo()
        assert_equal(pending['blocks'], 0)
        assert_equal(pending['upgrades'][branch_id(NU6_2_BRANCH_ID)]['status'], 'pending')
        assert_equal(pending['consensus']['chaintip'], '00000000')
        assert_equal(pending['consensus']['nextblock'], branch_id(NU6_1_BRANCH_ID))

        active = self.nodes[1].getblockchaininfo()
        assert_equal(active['blocks'], 0)
        assert_equal(active['upgrades'][branch_id(NU6_2_BRANCH_ID)]['status'], 'active')
        assert_equal(active['consensus']['chaintip'], branch_id(NU6_2_BRANCH_ID))
        assert_equal(active['consensus']['nextblock'], branch_id(NU6_2_BRANCH_ID))


if __name__ == '__main__':
    OrchardNU6_2Test().main()
