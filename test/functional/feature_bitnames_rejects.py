#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test what the names refuse, and what survives reorganisations, restarts and reindexes.

- The mempool: a transaction for every reason the names refuse one for that a transaction can be
  made for, by testmempoolaccept and sendrawtransaction, with the reason. (Two need a thousand
  names, bad-bn-too-many-subnames and bn-release-too-large: bitnames_tests has them.)
- Blocks that break the rules, which the mainchain committed to, through submitblock.
- The guards of the wallet: a name with a change waiting, a transaction the mempool refuses, a
  locked wallet, a reservation sent to an address labelled with another name.
- Records of every type, as rows and over the DNS server's wire; its errors at start.
- A reorganisation of the mainchain that takes a registration, then a release, with it.
- Restarts, -reindex and -reindex-chainstate.
"""

from decimal import Decimal
import hashlib
import re
import socket
import struct

from feature_bitnames import BitNamesTest, bound_commitment, subname_hash
from feature_sidechain import ACTIVATION_PERIOD, SLOT
from test_framework.blocktools import add_witness_commitment, create_block, create_coinbase
from test_framework.messages import COIN, COutPoint, CTransaction, CTxIn, CTxOut, ser_compact_size
from test_framework.script import CScript, OP_RETURN
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_raises_rpc_error

# The kinds of the operations of a marker.
NONE, RESERVE, REGISTER, UPDATE, ICANN, RELEASE, SUBNAME = range(7)
# Name data with no field set; an update that leaves every field.
NO_DATA = b"\x00" * 6
RETAIN_ALL = b"\x01" * 6


def u32s(values):
    return ser_compact_size(len(values)) + b"".join(struct.pack("<I", v) for v in values)


def bstr(data):
    return ser_compact_size(len(data)) + data


def opt(data):
    return b"\x00" if data is None else b"\x01" + data


def marker(kind, op=b"", names=(), reservations=(), memos=(), extension=None, version=None):
    """The bytes of a marker (see bitnames::MarkerScript), well formed or not."""
    if version is None:
        version = 0 if extension is None else 1
    out = b"BNAM" + bytes([version, kind]) + op + u32s(names) + u32s(reservations)
    out += ser_compact_size(len(memos)) + b"".join(struct.pack("<I", n) + bstr(m) for n, m in memos)
    return out + (extension or b"")


def register_op(name, nonce, data=NO_DATA):
    return name + nonce + data


def icann_op(names, signature):
    return ser_compact_size(len(names)) + b"".join(bstr(n.encode()) for n in names) + bstr(signature)


def subname_op(label, data=NO_DATA):
    return bstr(label.encode()) + data


def name_hash(text):
    return hashlib.sha256(text.encode()).digest()


class BitNamesRejectsTest(BitNamesTest):
    def set_test_params(self):
        super().set_test_params()
        # The second node takes transactions that are not standard: there, what the names refuse is
        # seen whatever the policy would say first.
        self.extra_args[1] = self.extra_args[1] + ["-acceptnonstdtxn=1"]

    def run_test(self):
        super().run_test()

    def script_of(self, node, address=None):
        return bytes.fromhex(node.getaddressinfo(address or node.getnewaddress())["scriptPubKey"])

    def coin(self, node):
        """A coin of the node's wallet to pay the fee with, not used by this test before."""
        for u in sorted(node.listunspent(), key=lambda u: -u["amount"]):
            if u["amount"] >= Decimal("0.01") and (u["txid"], u["vout"]) not in self.used:
                self.used.add((u["txid"], u["vout"]))
                return u
        raise AssertionError("no coin left")

    def make_tx(self, node, outputs, inputs=(), fee=Decimal("0.001"), prevtxs=None):
        """A transaction of the node: `inputs`, then a coin for the fee; `outputs`, then the change. Signed, not sent.
        `prevtxs`: the outputs it spends that are in no block nor mempool, for signrawtransactionwithwallet."""
        tx = CTransaction()
        for txid, vout in inputs:
            tx.vin.append(CTxIn(COutPoint(int(txid, 16), vout)))
        fee_coin = self.coin(node)
        tx.vin.append(CTxIn(COutPoint(int(fee_coin["txid"], 16), fee_coin["vout"])))
        tx.vout.extend(outputs)
        tx.vout.append(CTxOut(int((fee_coin["amount"] - fee) * COIN), self.script_of(node)))
        signed = node.signrawtransactionwithwallet(tx.serialize().hex(), prevtxs or [])
        assert signed["complete"], signed
        return signed["hex"]

    def unused(self, node, *txs):
        """Transactions made by make_tx that were not sent: their fee coins are for what follows."""
        for tx in txs:
            fee_in = node.decoderawtransaction(tx)["vin"][-1]
            self.used.discard((fee_in["txid"], fee_in["vout"]))

    def child_of(self, node, parent_hex, vout):
        """A transaction of the node that spends output `vout` of `parent_hex`, which is not sent."""
        parent = node.decoderawtransaction(parent_hex)
        out = parent["vout"][vout]
        # The value of the parent's output is the child's too: the change takes it, less a fee.
        return self.make_tx(node, [], [(parent["txid"], vout)], fee=Decimal("0.001") - out["value"],
                            prevtxs=[{"txid": parent["txid"], "vout": vout, "scriptPubKey": out["scriptPubKey"]["hex"], "amount": out["value"]}])

    def zero(self, node):
        """An output of no value to the node's wallet: one to carry a name."""
        return CTxOut(0, self.script_of(node))

    @staticmethod
    def mark(body, value=0):
        return CTxOut(value, CScript([OP_RETURN, body]))

    def owner(self, node, name):
        owner = node.getname(name)["owner"]
        return owner["txid"], owner["vout"]

    def refused(self, node, tx_hex, reason):
        result = node.testmempoolaccept([tx_hex])[0]
        assert_equal((result["allowed"], result.get("reject-reason")), (False, reason))

    def reservation(self, node, name, nonce):
        """A reservation made by hand, bound to a new address of the node: its txid."""
        script = self.script_of(node)
        commitment = bound_commitment(name, nonce, script.hex())
        return node.sendrawtransaction(self.make_tx(node, [CTxOut(0, script), self.mark(marker(RESERVE, commitment, reservations=[0]))]))

    def registration(self, node, reservation, name, nonce):
        return self.make_tx(node, [self.zero(node), self.mark(marker(REGISTER, register_op(name, nonce), names=[0]))], inputs=[(reservation, 0)])

    def submit_committed(self, txs):
        """A block of the first node's tip with `txs`, committed to by the mainchain, then submitted: what submitblock says."""
        side = self.nodes[0]
        tip = side.getbestblockhash()
        block = create_block(int(tip, 16), create_coinbase(side.getblockcount() + 1, script_pubkey=self.script_of(side), nValue=0),
                             ntime=side.getblock(tip)["time"] + 1, txlist=txs)
        add_witness_commitment(block)
        block.solve()
        self.main.createbmmrequest(SLOT, block.hash_hex, Decimal("0.001"))
        self.mine_main()
        result = side.submitblock(block.serialize().hex())
        assert_equal(side.getbestblockhash(), tip)
        assert {"hash": block.hash_hex, "status": "invalid"}.items() <= {t["hash"]: t for t in side.getchaintips()}.get(block.hash_hex, {}).items()
        return result

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
        self.used = set()
        self.main_address = main.getnewaddress()
        self.side_address = side.getnewaddress()

        self.log.info("Set up the sidechain and fund both wallets")
        self.mine_main(110)
        main.createsidechainproposal(SLOT, "BitNames", "Names")
        self.mine_main(ACTIVATION_PERIOD + 1)
        main.createsidechaindeposit(SLOT, side.getnewaddress(), 20)
        main.createsidechaindeposit(SLOT, other.getnewaddress(), 20)
        self.mine_main()
        self.bmm()
        side.sendmany("", {side.getnewaddress(): Decimal("0.3") for _ in range(40)})
        other.sendmany("", {other.getnewaddress(): Decimal("0.3") for _ in range(20)})
        self.mine_txs()

        self.log.info("Names to work with")
        side.reservename("alpha")
        self.mine_txs()
        self.mine_txs()
        side.registername("alpha", {"ipv6": "[2001:db8::5]:80"})
        self.mine_txs()
        side.createsubname("alpha", "sub")
        self.mine_txs()
        alpha_out = self.owner(side, "alpha")
        sub_out = self.owner(side, "sub.alpha")
        free = name_hash("free")

        self.log.info("The mempool refuses a transaction for every reason the names have")
        cases = [
            ("two markers", [self.mark(marker(NONE)), self.mark(marker(NONE))], [], "bad-bn-markers"),
            ("an operation that is none", [self.mark(marker(7))], [], "bad-bn-marker"),
            ("a marker of the extended form without an operation", [self.mark(marker(NONE, version=1))], [], "bad-bn-marker"),
            ("a marker of a version that is none", [self.mark(marker(NONE, version=2))], [], "bad-bn-marker"),
            ("a marker cut short", [self.mark(b"BNAM\x00\x01" + bytes(10))], [], "bad-bn-marker"),
            ("a marker with value", [self.mark(marker(NONE), value=1000)], [], "bad-bn-marker-value"),
            ("a name output there is not", [self.mark(marker(NONE, names=[5]))], [], "bad-bn-outputs"),
            ("a name and a reservation on one output", [self.zero(side), self.mark(marker(NONE, names=[0], reservations=[0]))], [alpha_out], "bad-bn-outputs"),
            ("a name output with value", [CTxOut(1000, self.script_of(side)), self.mark(marker(NONE, names=[0]))], [alpha_out], "bad-bn-token-output"),
            ("a memo to the marker", [self.mark(marker(NONE, memos=[(0, b"hi")]))], [], "bad-bn-memo"),
            ("an empty memo", [CTxOut(1000, self.script_of(side)), self.mark(marker(NONE, memos=[(0, b"")]))], [], "bad-bn-memo"),
            ("records that are no object", [self.zero(side), self.mark(marker(REGISTER, register_op(free, bytes(32)), names=[0], extension=opt(bstr(b"[1]")) + opt(None)))], [], "bad-bn-data"),
            ("a negative paymail fee", [self.zero(side), self.mark(marker(REGISTER, register_op(free, bytes(32), b"\x00" * 5 + b"\x01" + struct.pack("<q", -1)), names=[0]))], [], "bad-bn-data"),
            ("the text of another name", [self.zero(side), self.mark(marker(REGISTER, register_op(free, bytes(32)), names=[0], extension=opt(None) + opt(bstr(b"other"))))], [], "bad-bn-name-text"),
            ("a label that is none", [self.zero(side), self.zero(side), self.mark(marker(SUBNAME, subname_op("Bad"), names=[0, 1]))], [alpha_out], "bad-bn-label"),
            ("no ICANN names", [self.mark(marker(ICANN, icann_op([], bytes(64))))], [], "bad-bn-icann-names"),
            ("an ICANN name that is none", [self.mark(marker(ICANN, icann_op(["Alpha"], bytes(64))))], [], "bad-bn-icann-names"),
            ("an ICANN signature of the wrong size", [self.mark(marker(ICANN, icann_op(["alpha"], bytes(10))))], [], "bad-bn-icann-signature"),
            ("an ICANN statement where there is no authority", [self.mark(marker(ICANN, icann_op(["alpha"], bytes(64))))], [], "bad-bn-icann-disabled"),
            ("a reservation without its output", [self.mark(marker(RESERVE, bytes(32)))], [], "bad-bn-unbalanced"),
            ("a name spent and not carried on", [], [alpha_out], "bad-bn-unbalanced"),
            ("a release of nothing", [self.zero(side), self.mark(marker(RELEASE, names=[0]))], [alpha_out], "bad-bn-unbalanced"),
            ("an update without a name", [self.zero(side), self.mark(marker(UPDATE, RETAIN_ALL, names=[0]))], [], "bad-bn-no-name"),
            ("a subname without a name", [self.zero(side), self.mark(marker(SUBNAME, subname_op("x"), names=[0]))], [], "bad-bn-no-name"),
            ("a registration without a reservation", [self.zero(side), self.mark(marker(REGISTER, register_op(free, bytes(32)), names=[0]))], [], "bad-bn-no-reservation"),
            ("a release that keeps a subname of the name it releases", [self.zero(side), self.mark(marker(RELEASE, names=[0]))], [sub_out, alpha_out], "bad-bn-release-subname-kept"),
            ("a subname that exists", [self.zero(side), self.zero(side), self.mark(marker(SUBNAME, subname_op("sub"), names=[0, 1]))], [alpha_out], "bad-bn-name-taken"),
        ]
        for what, outputs, inputs, reason in cases:
            self.log.debug(what)
            tx = self.make_tx(side, outputs, inputs)
            for node in self.nodes:
                self.refused(node, tx, reason)
        # The first node's policy lets them through to the rules, and sendrawtransaction says the same.
        assert_raises_rpc_error(-26, "bad-bn-marker-value", side.sendrawtransaction, self.make_tx(side, [self.mark(marker(NONE), value=1000)]), 0, 1)
        # A name output to a script nobody can spend: the second node, which takes what is not standard.
        self.refused(other, self.make_tx(side, [CTxOut(0, CScript([1])), self.mark(marker(NONE, names=[0]))], [alpha_out]), "bad-bn-token-output")

        self.log.info("Reservations made by hand: too young, of a taken name, and two of a name")
        young_nonce, first_nonce, second_nonce = bytes([1] * 32), bytes([2] * 32), bytes([3] * 32)
        young = self.reservation(side, name_hash("young"), young_nonce)
        dup = name_hash("dup")
        first = self.reservation(side, dup, first_nonce)
        second = self.reservation(side, dup, second_nonce)
        taken = self.reservation(side, name_hash("alpha"), young_nonce)
        self.mine_txs()
        early = self.registration(side, young, name_hash("young"), young_nonce)
        self.refused(side, early, "bad-bn-reservation-too-young")
        self.refused(side, self.registration(side, taken, name_hash("alpha"), young_nonce), "bad-bn-name-taken")
        # Wrong nonce: the reservation opens to nothing.
        self.refused(side, self.registration(side, first, dup, second_nonce), "bad-bn-no-reservation")

        self.log.info("Blocks that break the rules are refused, whoever committed to them")
        assert_equal(self.submit_committed([early]), "bad-bn-reservation-too-young")
        self.mine_txs()
        registered = side.sendrawtransaction(self.registration(side, first, dup, first_nonce))
        rival = self.registration(side, second, dup, second_nonce)
        # One registration of a name at a time in the mempool: the second could never be mined.
        self.refused(side, rival, "bn-registration-in-mempool")
        # A block with both: the second finds the name taken.
        assert_equal(self.submit_committed([side.getrawtransaction(registered), rival]), "bad-bn-name-taken")
        spent_plainly = self.make_tx(side, [], [alpha_out])
        assert_equal(self.submit_committed([spent_plainly]), "bad-bn-unbalanced")
        assert_equal(self.submit_committed([self.make_tx(side, [self.mark(marker(7))])]), "bad-bn-marker")
        self.check_in_sync()
        block = self.mine_txs()
        assert registered in [tx["txid"] for tx in block["tx"]]
        assert_equal(side.getname("0x" + dup[::-1].hex())["owner"]["txid"], registered)

        self.log.info("A name output waiting for a block is not spent; the wallet says why")
        update = side.updatename("alpha", {"ipv4": "192.0.2.1"})
        self.refused(side, self.make_tx(side, [self.zero(side), self.mark(marker(NONE, names=[0]))], [(update, 0)]), "bn-token-unconfirmed")
        assert_raises_rpc_error(-4, f"The last change to this name (transaction {update}) is waiting for the next block", side.updatename, "alpha", {"ipv4": None})
        self.mine_txs()
        side.updatename("alpha", {"ipv4": None})
        self.mine_txs()
        alpha_out = self.owner(side, "alpha")

        self.log.info("Names and refunds come one at a time, not in packages: the checks of the mempool see only what is in it")
        # A move of alpha, which pays no fee, and a child that spends its name output as a plain coin
        # and pays for both: no block could hold the child, as bn-token-unconfirmed says of it alone.
        script = self.script_of(side)
        mover = self.make_tx(side, [CTxOut(0, script), self.mark(marker(NONE, names=[0]))], [alpha_out], fee=Decimal(0))
        mover_txid = side.decoderawtransaction(mover)["txid"]
        child = self.make_tx(side, [], [(mover_txid, 0)], fee=Decimal("0.01"),
                             prevtxs=[{"txid": mover_txid, "vout": 0, "scriptPubKey": script.hex(), "amount": 0}])
        assert_equal([r.get("package-error") for r in side.testmempoolaccept([mover, child])], ["package-drivechain-tx"] * 2)
        assert_equal(side.submitpackage([mover, child])["package_msg"], "package-drivechain-tx")
        assert mover_txid not in side.getrawmempool()
        self.unused(side, mover, child)
        # A refund request (whatever it refunds: the package is refused first) with a child.
        refund = self.make_tx(side, [CTxOut(0, CScript([OP_RETURN, bytes.fromhex("d952464e") + bytes(36 + 65)]))])
        refund_child = self.child_of(side, refund, 1)
        assert_equal([r.get("package-error") for r in side.testmempoolaccept([refund, refund_child])], ["package-drivechain-tx"] * 2)
        self.unused(side, refund, refund_child)
        # Plain transactions still go as a package.
        plain = self.make_tx(side, [])
        plain_child = self.child_of(side, plain, 0)
        result = side.testmempoolaccept([plain, plain_child])
        assert all(r["allowed"] for r in result), result
        self.unused(side, plain, plain_child)

        self.log.info("A squat of a subname cannot be kept by whoever makes the subname")
        squat = subname_hash(side.getname("alpha")["namehash"], "sq")
        squat_nonce = bytes([4] * 32)
        squat_reservation = self.reservation(side, squat, squat_nonce)
        self.mine_txs()
        self.mine_txs()
        side.sendrawtransaction(self.registration(side, squat_reservation, squat, squat_nonce))
        self.mine_txs()
        squat_out = self.owner(side, "0x" + squat[::-1].hex())
        self.refused(side, self.make_tx(side, [self.zero(side), self.zero(side), self.zero(side), self.mark(marker(SUBNAME, subname_op("sq"), names=[0, 1, 2]))],
                                        [squat_out, alpha_out]), "bad-bn-released-name-kept")

        self.log.info("Subnames go 8 levels deep; a transaction the mempool refuses is abandoned by the wallet")
        path = "sub.alpha"
        for _ in range(7):
            side.createsubname(path, "l")
            self.mine_txs()
            path = "l." + path
        # sub is 1 level under alpha, and each l one more: 8.
        assert_equal(path.count("."), 8)
        assert_equal(side.getname(path)["label"], "l")
        balance = side.getbalances()["mine"]["trusted"]
        assert_raises_rpc_error(-4, "The transaction was not accepted into the mempool", side.createsubname, path, "l")
        assert_equal(side.getbalances()["mine"]["trusted"], balance)
        deepest = self.owner(side, path)
        self.refused(side, self.make_tx(side, [self.zero(side), self.zero(side), self.mark(marker(SUBNAME, subname_op("l"), names=[0, 1]))], [deepest]),
                     "bad-bn-subname-too-deep")
        # Its name output was given back: the name is used again at once.
        side.updatename(path, {"ipv4": "192.0.2.8"})
        self.mine_txs()
        assert_equal(side.getname(path)["data"], {"ipv4": "192.0.2.8"})

        self.log.info("Records of every type, as rows and over the DNS server's wire")
        rows = [
            {"host": "@", "type": "TXT", "value": "hello"},
            {"host": "@", "type": "NS", "value": "ns1"},
            {"host": "_443._tcp", "type": "TLSA", "value": "3 1 1 aabb"},
            {"host": "www", "type": "A", "value": "192.0.2.80"},
        ]
        assert_raises_rpc_error(-8, "Record types: A, AAAA, CNAME, TXT, MX, SRV, NS, TLSA, DELEGATE", side.setnamerecords, "alpha", [{"type": "SPF", "value": "v=spf1"}])
        assert_raises_rpc_error(-8, "TLSA: usage selector type data", side.setnamerecords, "alpha", [{"type": "TLSA", "value": "3 1 aabb"}])
        side.setnamerecords("alpha", rows)
        self.mine_txs()
        assert_equal(sorted(map(str, side.getnamerecords("alpha"))), sorted(map(str, rows)))
        assert_equal(side.resolvename("_443._tcp.alpha.x", "TLSA")["records"], [{"name": "_443._tcp.alpha.x", "type": "TLSA", "data": "3 1 1 aabb"}])
        assert_equal(self.dns_query("alpha.x", 16), (0, [(16, b"\x05hello")]))
        assert_equal(self.dns_query("alpha.x", 2), (0, [(2, b"\x03ns1\x05alpha\x01x\x00")]))
        # Its records have no IPv6 address: that of its data answers.
        assert_equal(self.dns_query("alpha.x", 28), (0, [(28, socket.inet_pton(socket.AF_INET6, "2001:db8::5"))]))
        assert_equal(self.dns_query("www.alpha.x"), (0, [(1, bytes([192, 0, 2, 80]))]))
        # A host of the map given as an address alone is that host's, as the rows show it.
        side.updatename("alpha", {"records": {"map": {"www": "192.0.2.81"}}})
        self.mine_txs()
        assert_equal(side.getnamerecords("alpha"), [{"host": "www", "type": "A", "value": "192.0.2.81"}])
        assert_equal(self.dns_query("www.alpha.x"), (0, [(1, bytes([192, 0, 2, 81]))]))
        assert_equal(self.dns_query("alpha.x", 1), (0, []))

        self.log.info("A reservation is listed by its name only when the wallet can open it")
        side.createwallet(wallet_name="locked", passphrase="secret")
        locked = side.get_wallet_rpc("locked")
        default = side.get_wallet_rpc(self.default_wallet_name)
        default.sendtoaddress(locked.getnewaddress(), 2)
        self.mine_txs()
        locked.walletpassphrase("secret", 600)
        locked.reservename("hidden")
        self.mine_txs()
        assert_equal([(n["kind"], n.get("name")) for n in locked.listmynames()], [("reservation", "hidden")])
        locked.walletlock()
        assert_equal([(n["kind"], n.get("name")) for n in locked.listmynames()], [("reservation", None)])
        side.unloadwallet("locked")
        # A reservation of someone else sent to an address labelled with a name: not that name's.
        side.reservename("labelled")
        self.mine_txs()
        address = [n for n in side.listmynames() if n.get("name") == "labelled"][0]["address"]
        other.sendrawtransaction(self.send_marker(other, marker(RESERVE, bytes([7] * 32), reservations=[0]).hex(), address=address))
        self.mine_txs()
        assert_equal(sorted(str(n.get("name")) for n in side.listmynames() if n["address"] == address), ["None", "labelled"])

        self.log.info("A reorganisation of the mainchain takes a registration with it, and gives it back")
        side.reservename("reorged")
        self.mine_txs()
        self.mine_txs()
        reg = side.registername("reorged", {"ipv4": "192.0.2.9"})
        tip = side.getbestblockhash()
        names_before = side.getbitnamesinfo()["names"]
        block = self.mine_txs()
        info = side.getname("reorged")
        main_tip = main.getbestblockhash()
        main.invalidateblock(main_tip)
        replacement = main.generateblock(self.main_address, [])["hash"]
        for node in self.nodes:
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), tip)
            assert_raises_rpc_error(-8, "No such name", node.getname, "reorged")
            assert_equal(node.getbitnamesinfo()["names"], names_before)
        assert reg["txid"] in side.getrawmempool()
        self.check_in_sync()
        main.invalidateblock(replacement)
        main.reconsiderblock(main_tip)
        for node in self.nodes:
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), block["hash"])
            assert_equal(node.getname("reorged"), info)
        self.check_in_sync()

        self.log.info("... and a release, with the subnames it took")
        side.createsubname("reorged", "kid", "", {"ipv4": "192.0.2.10"})
        self.mine_txs()
        side.updatename("kid.reorged", {"ipv4": "192.0.2.11"})
        self.mine_txs()
        counts = lambda node: {k: v for k, v in node.getbitnamesinfo().items() if k != "dns"}
        before = (side.getname("reorged"), side.getname("kid.reorged"), side.getnamehistory("kid.reorged"), counts(side))
        tip = side.getbestblockhash()
        side.releasename("reorged", True)
        block = self.mine_txs()
        assert_raises_rpc_error(-8, "No such subname", side.getname, "kid.reorged")
        main_tip = main.getbestblockhash()
        main.invalidateblock(main_tip)
        replacement = main.generateblock(self.main_address, [])["hash"]
        for node in self.nodes:
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), tip)
            assert_equal((node.getname("reorged"), node.getname("kid.reorged"), node.getnamehistory("kid.reorged"), counts(node)), before)
        assert_equal(self.dns_query("kid.reorged.x"), (0, [(1, bytes([192, 0, 2, 11]))]))
        self.check_in_sync()
        main.invalidateblock(replacement)
        main.reconsiderblock(main_tip)
        for node in self.nodes:
            node.syncmainchain()
            assert_equal(node.getbestblockhash(), block["hash"])
            assert_raises_rpc_error(-8, "No such name", node.getname, "reorged")
        assert_equal(self.dns_query("kid.reorged.x")[0], 3)
        self.check_in_sync()

        self.log.info("The names survive a restart, and are rebuilt by -reindex and -reindex-chainstate")
        expected = (side.listnames(), side.getbitnamesinfo(), side.getnamehistory("alpha"), side.getnamerecords("alpha"), side.getsidechainstate())
        tip = side.getbestblockhash()

        def check():
            self.wait_until(lambda: side.getbestblockhash() == tip)
            assert_equal((side.listnames(), side.getbitnamesinfo(), side.getnamehistory("alpha"), side.getnamerecords("alpha"), side.getsidechainstate()), expected)
            assert_equal(self.dns_query("www.alpha.x"), (0, [(1, bytes([192, 0, 2, 81]))]))

        self.restart_node(0)
        check()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex"])
        check()
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex-chainstate"])
        check()
        self.connect_nodes(0, 1)
        self.mine_txs()
        self.check_in_sync()

        self.log.info("The DNS server does not start on an address that is none, or on a port in use")
        self.stop_node(1)
        self.nodes[1].assert_start_raises_init_error(self.extra_args[1] + ["-dnsbind=not an address"], "Error: Invalid -dnsbind address: not an address",
                                                     match=ErrorMatch.PARTIAL_REGEX)
        in_use = f"127.0.0.1:{self.dns_port}"
        self.nodes[1].assert_start_raises_init_error(self.extra_args[1] + [f"-dnsbind={in_use}"],
                                                     re.escape(f"Error: The BitNames DNS server (-dnsbind={in_use}) did not start: cannot listen on {in_use} (is it in use?)"),
                                                     match=ErrorMatch.PARTIAL_REGEX)
        self.start_node(1)
        assert "dns" not in other.getbitnamesinfo()
        self.connect_nodes(0, 1)
        self.check_in_sync()


if __name__ == "__main__":
    BitNamesRejectsTest(__file__).main()
