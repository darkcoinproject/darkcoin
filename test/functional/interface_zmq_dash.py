#!/usr/bin/env python3
# Copyright (c) 2018-2020 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the dash specific ZMQ notification interfaces."""

import configparser
from enum import Enum
import io
import zmq

from test_framework.test_framework import DashTestFramework, SkipTest
from test_framework.util import *
from test_framework.messages import *


class ZMQPublisher(Enum):
    hash_chain_lock = "hashchainlock"
    hash_tx_lock = "hashtxlock"
    hash_governance_vote = "hashgovernancevote"
    hash_governance_object = "hashgovernanceobject"
    hash_instantsend_doublespend = "hashinstantsenddoublespend"
    hash_recovered_sig = "hashrecoveredsig"
    raw_chain_lock = "rawchainlock"
    raw_chain_lock_sig = "rawchainlocksig"
    raw_tx_lock = "rawtxlock"
    raw_tx_lock_sig = "rawtxlocksig"
    raw_governance_vote = "rawgovernancevote"
    raw_governance_object = "rawgovernanceobject"
    raw_instantsend_doublespend = "rawinstantsenddoublespend"
    raw_recovered_sig = "rawrecoveredsig"

class DashZMQTest (DashTestFramework):
    def set_test_params(self):
        # That's where the zmq publisher will listen for subscriber
        self.address = "tcp://127.0.0.1:28333"
        # node0 creates all available ZMQ publisher
        node0_extra_args = ["-zmqpub%s=%s" % (pub.value, self.address) for pub in ZMQPublisher]
        node0_extra_args.append("-whitelist=127.0.0.1")

        self.set_dash_test_params(4, 3, fast_dip3_enforcement=True, extra_args=[node0_extra_args, [], [], []])
        self.set_dash_dip8_activation(10)

    def run_test(self):
        # Check that dashd has been built with ZMQ enabled.
        config = configparser.ConfigParser()
        config.read_file(open(self.options.configfile))

        if not config["components"].getboolean("ENABLE_ZMQ"):
            raise SkipTest("dashd has not been built with zmq enabled.")

        try:
            # Setup the ZMQ subscriber socket
            self.zmq_context = zmq.Context()
            self.socket = self.zmq_context.socket(zmq.SUB)
            self.socket.connect(self.address)
            # Initialize the network
            self.log.info("Wait for dip0008 activation")
            while self.nodes[0].getblockchaininfo()["bip9_softforks"]["dip0008"]["status"] != "active":
                self.nodes[0].generate(10)
            self.sync_blocks()
            self.nodes[0].spork("SPORK_2_INSTANTSEND_ENABLED", 0)
            self.nodes[0].spork("SPORK_17_QUORUM_DKG_ENABLED", 0)
            self.nodes[0].spork("SPORK_19_CHAINLOCKS_ENABLED", 0)
            self.wait_for_sporks_same()
            force_finish_mnsync(self.nodes[0])
            # Create a LLMQ for testing
            self.quorum_type = 100  # llmq_test
            self.quorum_hash = self.mine_quorum()
            self.sync_blocks()
            self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())
            # Wait a moment to avoid subscribing to recovered sig in the test before the one from the chainlock
            # has been sent which leads to test failure.
            time.sleep(1)
            # Test all dash related ZMQ publisher
            self.test_recovered_signature_publisher()
            self.test_chainlock_publisher()
            self.test_instantsend_publisher()
            self.test_governance_publisher()
        finally:
            # Destroy the ZMQ context.
            self.log.debug("Destroying ZMQ context")
            self.zmq_context.destroy(linger=None)

    def subscribe(self, publisher):
        # Subscribe a list of ZMQPublisher
        for pub in publisher:
            self.socket.subscribe(pub.value)

    def unsubscribe(self, publisher):
        # Unsubscribe a list of ZMQPublisher
        for pub in publisher:
            self.socket.unsubscribe(pub.value)

    def receive(self, publisher):
        # Receive a ZMQ message and validate its sent from the correct ZMQPublisher
        topic, body, seq = self.socket.recv_multipart()
        # Topic should match the publisher value
        assert_equal(topic.decode(), publisher.value)
        return io.BytesIO(body)

    def test_recovered_signature_publisher(self):

        def validate_recovered_sig(request_id, msg_hash):
            # Make sure the recovered sig exists by RPC
            rpc_recovered_sig = self.get_recovered_sig(request_id, msg_hash)
            # Validate hashrecoveredsig
            zmq_recovered_sig_hash = bytes_to_hex_str(self.receive(ZMQPublisher.hash_recovered_sig).read(32))
            assert_equal(zmq_recovered_sig_hash, msg_hash)
            # Validate rawrecoveredsig
            zmq_recovered_sig_raw = CRecoveredSig()
            zmq_recovered_sig_raw.deserialize(self.receive(ZMQPublisher.raw_recovered_sig))
            assert_equal(zmq_recovered_sig_raw.llmqType, rpc_recovered_sig['llmqType'])
            assert_equal(uint256_to_string(zmq_recovered_sig_raw.quorumHash), rpc_recovered_sig['quorumHash'])
            assert_equal(uint256_to_string(zmq_recovered_sig_raw.id), rpc_recovered_sig['id'])
            assert_equal(uint256_to_string(zmq_recovered_sig_raw.msgHash), rpc_recovered_sig['msgHash'])
            assert_equal(bytes_to_hex_str(zmq_recovered_sig_raw.sig), rpc_recovered_sig['sig'])

        recovered_sig_publisher = [
            ZMQPublisher.hash_recovered_sig,
            ZMQPublisher.raw_recovered_sig
        ]
        self.log.info("Testing %d recovered signature publisher" % len(recovered_sig_publisher))
        # Subscribe recovered signature messages
        self.subscribe(recovered_sig_publisher)
        # Generate a ChainLock and make sure this leads to valid recovered sig ZMQ messages
        rpc_last_block_hash = self.nodes[0].generate(1)[0]
        self.wait_for_chainlocked_block_all_nodes(rpc_last_block_hash)
        height = self.nodes[0].getblockcount()
        rpc_request_id = hash256(ser_string(b"clsig") + struct.pack("<I", height))[::-1].hex()
        validate_recovered_sig(rpc_request_id, rpc_last_block_hash)
        # Sign an arbitrary and make sure this leads to valid recovered sig ZMQ messages
        sign_id = uint256_to_string(random.getrandbits(256))
        sign_msg_hash = uint256_to_string(random.getrandbits(256))
        for mn in self.get_quorum_masternodes(self.quorum_hash):
            mn.node.quorum("sign", self.quorum_type, sign_id, sign_msg_hash)
        validate_recovered_sig(sign_id, sign_msg_hash)
        # Unsubscribe recovered signature messages
        self.unsubscribe(recovered_sig_publisher)

    def test_chainlock_publisher(self):
        chain_lock_publisher = [
            ZMQPublisher.hash_chain_lock,
            ZMQPublisher.raw_chain_lock,
            ZMQPublisher.raw_chain_lock_sig
        ]
        self.log.info("Testing %d ChainLock publisher" % len(chain_lock_publisher))
        # Subscribe ChainLock messages
        self.subscribe(chain_lock_publisher)
        # Generate ChainLock
        generated_hash = self.nodes[0].generate(1)[0]
        self.wait_for_chainlocked_block_all_nodes(generated_hash)
        rpc_best_chain_lock = self.nodes[0].getbestchainlock()
        rpc_best_chain_lock_hash = rpc_best_chain_lock["blockhash"]
        rpc_best_chain_lock_sig = rpc_best_chain_lock["signature"]
        assert_equal(generated_hash, rpc_best_chain_lock_hash)
        rpc_chain_locked_block = self.nodes[0].getblock(rpc_best_chain_lock_hash)
        rpc_chain_lock_height = rpc_chain_locked_block["height"]
        rpc_chain_lock_hash = rpc_chain_locked_block["hash"]
        assert_equal(generated_hash, rpc_chain_lock_hash)
        # Validate hashchainlock
        zmq_chain_lock_hash = bytes_to_hex_str(self.receive(ZMQPublisher.hash_chain_lock).read(32))
        assert_equal(zmq_chain_lock_hash, rpc_best_chain_lock_hash)
        # Validate rawchainlock
        zmq_chain_locked_block = CBlock()
        zmq_chain_locked_block.deserialize(self.receive(ZMQPublisher.raw_chain_lock))
        assert(zmq_chain_locked_block.is_valid())
        assert_equal(zmq_chain_locked_block.hash, rpc_chain_lock_hash)
        # Validate rawchainlocksig
        zmq_chain_lock_sig_stream = self.receive(ZMQPublisher.raw_chain_lock_sig)
        zmq_chain_locked_block = CBlock()
        zmq_chain_locked_block.deserialize(zmq_chain_lock_sig_stream)
        assert(zmq_chain_locked_block.is_valid())
        zmq_chain_lock = msg_clsig()
        zmq_chain_lock.deserialize(zmq_chain_lock_sig_stream)
        assert_equal(zmq_chain_lock.height, rpc_chain_lock_height)
        assert_equal(uint256_to_string(zmq_chain_lock.blockHash), rpc_chain_lock_hash)
        assert_equal(zmq_chain_locked_block.hash, rpc_chain_lock_hash)
        assert_equal(bytes_to_hex_str(zmq_chain_lock.sig), rpc_best_chain_lock_sig)
        # Unsubscribe ChainLock messages
        self.unsubscribe(chain_lock_publisher)

    def test_instantsend_publisher(self):
        instantsend_publisher = [
            ZMQPublisher.hash_tx_lock,
            ZMQPublisher.raw_tx_lock,
            ZMQPublisher.raw_tx_lock_sig,
            ZMQPublisher.hash_instantsend_doublespend,
            ZMQPublisher.raw_instantsend_doublespend
        ]
        self.log.info("Testing %d InstantSend publisher" % len(instantsend_publisher))
        # Subscribe InstantSend messages
        self.subscribe(instantsend_publisher)
        # Make sure all nodes agree
        self.wait_for_chainlocked_block_all_nodes(self.nodes[0].getbestblockhash())
        # Create two raw TXs, they will conflict with each other
        rpc_raw_tx_1 = self.create_raw_tx(self.nodes[0], self.nodes[0], 1, 1, 100)
        rpc_raw_tx_2 = self.create_raw_tx(self.nodes[0], self.nodes[0], 1, 1, 100)
        # Send the first transaction and wait for the InstantLock
        rpc_raw_tx_1_hash = self.nodes[0].sendrawtransaction(rpc_raw_tx_1['hex'])
        self.wait_for_instantlock(rpc_raw_tx_1_hash, self.nodes[0])
        # Validate hashtxlock
        zmq_tx_lock_hash = bytes_to_hex_str(self.receive(ZMQPublisher.hash_tx_lock).read(32))
        assert_equal(zmq_tx_lock_hash, rpc_raw_tx_1['txid'])
        # Validate rawtxlock
        zmq_tx_lock_raw = CTransaction()
        zmq_tx_lock_raw.deserialize(self.receive(ZMQPublisher.raw_tx_lock))
        assert(zmq_tx_lock_raw.is_valid())
        assert_equal(zmq_tx_lock_raw.hash, rpc_raw_tx_1['txid'])
        # Validate rawtxlocksig
        zmq_tx_lock_sig_stream = self.receive(ZMQPublisher.raw_tx_lock_sig)
        zmq_tx_lock_tx = CTransaction()
        zmq_tx_lock_tx.deserialize(zmq_tx_lock_sig_stream)
        assert(zmq_tx_lock_tx.is_valid())
        assert_equal(zmq_tx_lock_tx.hash, rpc_raw_tx_1['txid'])
        zmq_tx_lock = msg_islock()
        zmq_tx_lock.deserialize(zmq_tx_lock_sig_stream)
        assert_equal(uint256_to_string(zmq_tx_lock.txid), rpc_raw_tx_1['txid'])
        # Try to send the second transaction. This must throw an RPC error because it conflicts with rpc_raw_tx_1
        # which already got the InstantSend lock.
        assert_raises_rpc_error(-26, "tx-txlock-conflict", self.nodes[0].sendrawtransaction, rpc_raw_tx_2['hex'])
        # Validate hashinstantsenddoublespend
        zmq_double_spend_hash2 = bytes_to_hex_str(self.receive(ZMQPublisher.hash_instantsend_doublespend).read(32))
        zmq_double_spend_hash1 = bytes_to_hex_str(self.receive(ZMQPublisher.hash_instantsend_doublespend).read(32))
        assert_equal(zmq_double_spend_hash2, rpc_raw_tx_2['txid'])
        assert_equal(zmq_double_spend_hash1, rpc_raw_tx_1['txid'])
        # Validate rawinstantsenddoublespend
        zmq_double_spend_tx_2 = CTransaction()
        zmq_double_spend_tx_2.deserialize(self.receive(ZMQPublisher.raw_instantsend_doublespend))
        assert (zmq_double_spend_tx_2.is_valid())
        assert_equal(zmq_double_spend_tx_2.hash, rpc_raw_tx_2['txid'])
        zmq_double_spend_tx_1 = CTransaction()
        zmq_double_spend_tx_1.deserialize(self.receive(ZMQPublisher.raw_instantsend_doublespend))
        assert(zmq_double_spend_tx_1.is_valid())
        assert_equal(zmq_double_spend_tx_1.hash, rpc_raw_tx_1['txid'])
        # Unsubscribe InstantSend messages
        self.unsubscribe(instantsend_publisher)

    def test_governance_publisher(self):
        governance_publisher = [
            ZMQPublisher.hash_governance_object,
            ZMQPublisher.raw_governance_object,
            ZMQPublisher.hash_governance_vote,
            ZMQPublisher.raw_governance_vote
        ]
        self.log.info("Testing %d governance publisher" % len(governance_publisher))
        # Subscribe governance messages
        self.subscribe(governance_publisher)
        # Create a proposal and submit it to the network
        proposal_rev = 1
        proposal_time = int(time.time())
        proposal_data = {
            "type": 1,  # GOVERNANCE_OBJECT_PROPOSAL
            "name": "Test",
            "start_epoch": proposal_time,
            "end_epoch": proposal_time + 60,
            "payment_amount": 5,
            "payment_address": self.nodes[0].getnewaddress(),
            "url": "https://dash.org"
        }
        proposal_hex = ''.join(format(x, '02x') for x in json.dumps(proposal_data).encode())
        collateral = self.nodes[0].gobject("prepare", "0", proposal_rev, proposal_time, proposal_hex)
        self.wait_for_instantlock(collateral, self.nodes[0])
        self.nodes[0].generate(6)
        self.sync_blocks()
        rpc_proposal_hash = self.nodes[0].gobject("submit", "0", proposal_rev, proposal_time, proposal_hex, collateral)
        # Validate hashgovernanceobject
        zmq_governance_object_hash = bytes_to_hex_str(self.receive(ZMQPublisher.hash_governance_object).read(32))
        assert_equal(zmq_governance_object_hash, rpc_proposal_hash)
        zmq_governance_object_raw = CGovernanceObject()
        zmq_governance_object_raw.deserialize(self.receive(ZMQPublisher.raw_governance_object))
        assert_equal(zmq_governance_object_raw.nHashParent, 0)
        assert_equal(zmq_governance_object_raw.nRevision, proposal_rev)
        assert_equal(zmq_governance_object_raw.nTime, proposal_time)
        assert_equal(json.loads(zmq_governance_object_raw.vchData.decode()), proposal_data)
        assert_equal(zmq_governance_object_raw.nObjectType, proposal_data["type"])
        assert_equal(zmq_governance_object_raw.masternodeOutpoint.hash, COutPoint().hash)
        assert_equal(zmq_governance_object_raw.masternodeOutpoint.n, COutPoint().n)
        # Vote for the proposal and validate the governance vote message
        map_vote_outcomes = {
            0: "none",
            1: "yes",
            2: "no",
            3: "abstain"
        }
        map_vote_signals = {
            0: "none",
            1: "funding",
            2: "valid",
            3: "delete",
            4: "endorsed"
        }
        self.nodes[0].gobject("vote-many", rpc_proposal_hash, map_vote_signals[1], map_vote_outcomes[1])
        rpc_proposal_votes = self.nodes[0].gobject('getcurrentvotes', rpc_proposal_hash)
        # Validate hashgovernancevote
        zmq_governance_vote_hash = bytes_to_hex_str(self.receive(ZMQPublisher.hash_governance_vote).read(32))
        assert(zmq_governance_vote_hash in rpc_proposal_votes)
        # Validate rawgovernancevote
        zmq_governance_vote_raw = CGovernanceVote()
        zmq_governance_vote_raw.deserialize(self.receive(ZMQPublisher.raw_governance_vote))
        assert_equal(uint256_to_string(zmq_governance_vote_raw.nParentHash), rpc_proposal_hash)
        rpc_vote_parts = rpc_proposal_votes[zmq_governance_vote_hash].split(':')
        rpc_outpoint_parts = rpc_vote_parts[0].split('-')
        assert_equal(uint256_to_string(zmq_governance_vote_raw.masternodeOutpoint.hash), rpc_outpoint_parts[0])
        assert_equal(zmq_governance_vote_raw.masternodeOutpoint.n, int(rpc_outpoint_parts[1]))
        assert_equal(zmq_governance_vote_raw.nTime, int(rpc_vote_parts[1]))
        assert_equal(map_vote_outcomes[zmq_governance_vote_raw.nVoteOutcome], rpc_vote_parts[2])
        assert_equal(map_vote_signals[zmq_governance_vote_raw.nVoteSignal], rpc_vote_parts[3])
        # Unsubscribe governance messages
        self.unsubscribe(governance_publisher)


if __name__ == '__main__':
    DashZMQTest().main()
