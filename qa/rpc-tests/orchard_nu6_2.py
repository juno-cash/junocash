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
        self.num_nodes = 1
        self.cache_behavior = 'clean'

    def network_upgrade_args(self):
        return [
            nuparams(BLOSSOM_BRANCH_ID, 1),
            nuparams(HEARTWOOD_BRANCH_ID, 1),
            nuparams(CANOPY_BRANCH_ID, 1),
            nuparams(NU5_BRANCH_ID, 1),
            nuparams(NU6_BRANCH_ID, 1),
            nuparams(NU6_1_BRANCH_ID, 1),
            nuparams(NU6_2_BRANCH_ID, 2),
        ]

    def setup_nodes(self):
        return start_nodes(
            self.num_nodes,
            self.options.tmpdir,
            extra_args=[self.network_upgrade_args()] * self.num_nodes)

    def run_test(self):
        node = self.nodes[0]

        info = node.getblockchaininfo()
        assert_equal(info['blocks'], 0)
        assert_equal(info['upgrades'][branch_id(NU6_2_BRANCH_ID)]['status'], 'pending')
        assert_equal(info['consensus']['chaintip'], '00000000')
        assert_equal(info['consensus']['nextblock'], branch_id(NU6_1_BRANCH_ID))

        node.generate(1)
        info = node.getblockchaininfo()
        assert_equal(info['blocks'], 1)
        assert_equal(info['upgrades'][branch_id(NU6_2_BRANCH_ID)]['status'], 'pending')
        assert_equal(info['consensus']['chaintip'], branch_id(NU6_1_BRANCH_ID))
        assert_equal(info['consensus']['nextblock'], branch_id(NU6_2_BRANCH_ID))

        print("Activating NU6.2")
        node.generate(1)
        info = node.getblockchaininfo()
        assert_equal(info['blocks'], 2)
        assert_equal(info['upgrades'][branch_id(NU6_2_BRANCH_ID)]['status'], 'active')
        assert_equal(info['consensus']['chaintip'], branch_id(NU6_2_BRANCH_ID))
        assert_equal(info['consensus']['nextblock'], branch_id(NU6_2_BRANCH_ID))


if __name__ == '__main__':
    OrchardNU6_2Test().main()
