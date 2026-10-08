#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the names: reservation, registration, data and its history, transfer, undo."""

from decimal import Decimal
import hashlib
import hmac
import socket
import struct

from feature_sidechain import ACTIVATION_PERIOD, SLOT, SidechainTest
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port

# How deep a reservation has to be to be revealed, on regtest.
REVEAL_DEPTH = 2


def ser_string(text):
    data = text.encode()
    assert len(data) < 253
    return bytes([len(data)]) + data


def subname_hash(parent_hex, label):
    """SubnameHash: of its parent (the hash as the RPCs show it) and its label, in its byte order."""
    return hashlib.sha256(ser_string("BitNames subname") + bytes.fromhex(parent_hex)[::-1] + ser_string(label)).digest()


def bound_commitment(name, nonce, script_hex):
    """ReservationCommitment(name, nonce, script), as from audit2_height."""
    return hmac.new(nonce, name + bytes.fromhex(script_hex), hashlib.sha256).digest()


def reserve_marker(commitment):
    # BNAM, version 0, Reserve: the commitment; no name outputs; reservation output 0; no memos.
    return b"BNAM\x00\x01" + commitment + b"\x00" + b"\x01" + struct.pack("<I", 0) + b"\x00"


def register_marker(name, nonce):
    # BNAM, version 0, Register: the name, the nonce, no data; name output 0; no reservation outputs; no memos.
    return b"BNAM\x00\x02" + name + nonce + b"\x00" * 6 + b"\x01" + struct.pack("<I", 0) + b"\x00" + b"\x00"


class BitNamesTest(SidechainTest):
    def set_test_params(self):
        super().set_test_params()
        # The first node answers DNS questions for .x.
        self.dns_port = p2p_port(self.num_nodes + 3)
        self.extra_args = [self.extra_args[0] + [f"-dnsbind=127.0.0.1:{self.dns_port}"], self.extra_args[1]]

    def dns_query(self, name, qtype=1):
        """Ask the DNS server of the first node; the answers' data, raw."""
        query = struct.pack(">HHHHHH", 0x4242, 0x0100, 1, 0, 0, 0)
        for label in name.split("."):
            query += bytes([len(label)]) + label.encode()
        query += b"\x00" + struct.pack(">HH", qtype, 1)
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(5)
            s.sendto(query, ("127.0.0.1", self.dns_port))
            response, _ = s.recvfrom(4096)
        rcode = response[3] & 0x0f
        answers = struct.unpack(">H", response[6:8])[0]
        # Skip the question, then read each answer: a name pointer, type, class, ttl, length, data.
        p = len(query)
        out = []
        for _ in range(answers):
            if response[p] & 0xc0 == 0xc0:
                p += 2
            else:
                while response[p] != 0:
                    p += response[p] + 1
                p += 1
            rtype, _, _, length = struct.unpack(">HHIH", response[p:p + 10])
            out.append((rtype, response[p + 10:p + 10 + length]))
            p += 10 + length
        return rcode, out

    def run_test(self):
        super().run_test()

    def mine_txs(self):
        self.sync_mempools()
        return self.nodes[0].getblock(self.bmm(), 2)

    def names(self, node):
        return {n.get("name", n.get("namehash")): n for n in node.listmynames()}

    def marker_of(self, node, txid):
        """The data of the BitNames marker of a transaction of the node's wallet."""
        decoded = node.decoderawtransaction(node.gettransaction(txid)["hex"])
        return [o for o in decoded["vout"] if o["scriptPubKey"]["type"] == "nulldata"][0]["scriptPubKey"]["asm"].split()[1]

    def send_marker(self, node, data, inputs=None, address=None):
        """A transaction of the node: its output 0 (no value) to `address`, the marker, then the change."""
        raw = node.createrawtransaction(inputs or [], [{address or node.getnewaddress(): 0}, {"data": data}])
        funded = node.fundrawtransaction(raw, {"changePosition": 2})["hex"]
        return node.signrawtransactionwithwallet(funded)["hex"]

    def run_sidechain_test(self):
        main = self.main
        side, other = self.nodes
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
        side.sendmany("", {side.getnewaddress(): 1 for _ in range(10)})
        other.sendmany("", {other.getnewaddress(): 1 for _ in range(10)})
        self.mine_txs()
        assert_equal({k: v for k, v in side.getbitnamesinfo().items() if k != "dns"}, {"names": 0, "reservations": 0, "icann": False, "revealdepth": REVEAL_DEPTH})
        assert_equal(side.getbitnamesinfo()["dns"], f"127.0.0.1:{self.dns_port}")

        self.log.info("A name is reserved, then registered")
        reserved = side.reservename("satoshi")
        assert_raises_rpc_error(-8, "no reservation of the name in a block", side.registername, "satoshi")
        self.mine_txs()
        assert_equal(side.getbitnamesinfo()["reservations"], 1)
        mine = self.names(side)
        assert_equal(mine["satoshi"]["kind"], "reservation")
        # Revealed only once the reservation is deep enough: whoever makes the next block sees the
        # name and could otherwise reserve and register it in that block, ahead of this one.
        assert_raises_rpc_error(-8, "can be revealed from block", side.registername, "satoshi")
        self.mine_txs()
        registered = side.registername("satoshi", {"ipv4": "203.0.113.7:443", "paymailfee": Decimal("0.001")})
        assert_equal(registered["namehash"], reserved["namehash"])
        block = self.mine_txs()
        registered_height = block["height"]
        info = side.getname("satoshi")
        assert_equal(info["seq"], "1739-0029")
        assert_equal(info["data"], {"ipv4": "203.0.113.7:443", "paymailfee": Decimal("0.001")})
        assert_equal(info["owner"]["address"], self.names(side)["satoshi"]["address"])
        # A number has a form of its own: without "#" it is a text like any other, which anyone can register.
        assert_equal(side.getname("#1739-0029")["namehash"], info["namehash"])
        assert_raises_rpc_error(-8, "No such name", side.getname, "1739-0029")
        assert_raises_rpc_error(-8, "No name has the number", side.getname, "#1739-0129")
        assert_equal(side.getname("0x" + info["namehash"])["seq"], "1739-0029")
        assert_equal(side.getbitnamesinfo()["names"], 1)
        self.check_in_sync()
        assert_equal(other.getname("satoshi"), info)

        self.log.info("A name is taken once")
        assert_raises_rpc_error(-8, "The name is taken", other.reservename, "satoshi")
        # A reservation of a name that is free, by the other node: its nonce is its own.
        other.reservename("nakamoto")
        self.mine_txs()
        assert_raises_rpc_error(-8, "no reservation", side.registername, "nakamoto")
        self.mine_txs()
        other.registername("nakamoto")
        self.mine_txs()
        assert_equal(other.getname("nakamoto")["seq"], "1739-0129")
        # The wallet reads numbers as the node does.
        assert_raises_rpc_error(-8, "does not hold this name", side.updatename, "#1739-0129", {"ipv4": None})

        self.log.info("A copy of a reservation, in the same block, reveals nothing")
        prize = side.reservename("prize")
        copy = other.sendrawtransaction(self.send_marker(other, self.marker_of(side, prize["txid"])))
        block = self.mine_txs()
        assert prize["txid"] in [tx["txid"] for tx in block["tx"]] and copy in [tx["txid"] for tx in block["tx"]]
        self.mine_txs()
        revealed = side.registername("prize")
        # The copier reads the name and the nonce in the registration, and spends its copy with them.
        steal = self.send_marker(other, self.marker_of(side, revealed["txid"]), inputs=[{"txid": copy, "vout": 0}])
        assert_raises_rpc_error(-26, "bad-bn-no-reservation", other.sendrawtransaction, steal)
        self.mine_txs()
        assert_equal(side.getname("prize")["owner"]["address"], self.names(side)["prize"]["address"])

        self.log.info("The data of a name changes, and its history is kept")
        side.updatename("satoshi", {"ipv4": None, "ipv6": "[2001:db8::7]:8333", "commitment": "11" * 32})
        block = self.mine_txs()
        assert_equal(side.getname("satoshi")["data"], {"ipv6": "[2001:db8::7]:8333", "commitment": "11" * 32, "paymailfee": Decimal("0.001")})
        assert_equal(side.getname("satoshi", registered_height)["data"], {"ipv4": "203.0.113.7:443", "paymailfee": Decimal("0.001")})
        assert "data" not in side.getname("satoshi", registered_height - 1)
        history = side.getnamehistory("satoshi")
        assert_equal([e["value"] for e in history["ipv4"]], ["203.0.113.7:443", None])
        assert_equal(len(history["paymailfee"]), 1)
        assert_equal(history["records"], [{"value": None, "txid": history["ipv4"][0]["txid"], "height": registered_height}])
        assert_raises_rpc_error(-8, "does not hold this name", other.updatename, "satoshi", {"ipv4": "198.51.100.1:80"})

        self.log.info("A name is given to someone else")
        new_owner = other.getnewaddress()
        side.transfername("satoshi", new_owner)
        self.mine_txs()
        assert_equal(side.getname("satoshi")["owner"]["address"], new_owner)
        assert "satoshi" not in self.names(side)
        assert info["namehash"] in self.names(other)
        other.updatename("0x" + info["namehash"], {"paymailfee": None})
        self.mine_txs()
        assert "paymailfee" not in side.getname("satoshi")["data"]

        self.log.info("Spending a name without carrying it on is refused")
        owned = [n for n in other.listmynames() if n["kind"] == "name"][0]
        fund = [u for u in other.listunspent() if u["amount"] >= Decimal("0.5")][0]
        raw = other.createrawtransaction([{"txid": owned["txid"], "vout": owned["vout"]}, {"txid": fund["txid"], "vout": fund["vout"]}],
                                         [{other.getnewaddress(): fund["amount"] - Decimal("0.001")}])
        signed = other.signrawtransactionwithwallet(raw)["hex"]
        assert_raises_rpc_error(-26, "bad-bn-unbalanced", other.sendrawtransaction, signed)

        self.log.info("Messages to and from the owner of a name")
        keys = other.getnewnamekeys()
        other.updatename("nakamoto", {"encryptionkey": keys["encryptionkey"], "signingkey": keys["signingkey"], "paymailfee": Decimal("0.01")})
        self.mine_txs()
        assert_equal(side.getname("nakamoto")["data"]["signingkey"], keys["signingkey"])
        secret = side.encryptnamemessage("nakamoto", "meet at noon")
        assert_equal(other.decryptnamemessage(secret), "meet at noon")
        assert_raises_rpc_error(-4, "not to a key of this wallet", side.decryptnamemessage, secret)
        tampered = secret[:-2] + ("00" if secret[-2:] != "00" else "01")
        assert_raises_rpc_error(-8, "does not decrypt", other.decryptnamemessage, tampered)
        signature = other.signasname("nakamoto", "I am nakamoto")
        assert side.verifynamesignature("nakamoto", "I am nakamoto", signature)
        assert not side.verifynamesignature("nakamoto", "I am satoshi", signature)
        assert_raises_rpc_error(-8, "paymail of 0.01 at least", side.sendpaymail, "nakamoto", Decimal("0.001"), "too cheap")
        side.sendpaymail("nakamoto", Decimal("0.02"), "a paid message")
        self.mine_txs()
        mail = other.listpaymail()
        assert_equal(len(mail), 1)
        assert_equal(mail[0]["message"], "a paid message")
        assert_equal(mail[0]["amount"], Decimal("0.02"))
        # A message that paid less than the fee, made by hand (the wallet does not send it), is not listed.
        memo = bytes.fromhex(side.encryptnamemessage("nakamoto", "cheap"))
        marker = b"BNAM" + bytes([0, 0, 0, 0, 1]) + struct.pack("<I", 0) + (bytes([len(memo)]) if len(memo) < 253 else bytes([0xfd]) + struct.pack("<H", len(memo))) + memo
        raw = side.createrawtransaction([], [{side.getname("nakamoto")["owner"]["address"]: Decimal("0.002")}, {"data": marker.hex()}])
        funded = side.fundrawtransaction(raw, {"changePosition": 2})["hex"]
        side.sendrawtransaction(side.signrawtransactionwithwallet(funded)["hex"])
        self.mine_txs()
        assert_equal([m.get("message") for m in other.listpaymail()], ["a paid message"])

        self.log.info("DNS records, and the DNS server for .x")
        records = {"ip6": "2001:db8::7", "txt": "satoshi was here", "map": {"www": {"alias": "@"}, "*": {"ip": "198.51.100.9"}, "shop": {"delegate": "nakamoto"}}}
        other.updatename("0x" + info["namehash"], {"records": records, "ipv4": "203.0.113.7:443"})
        other.updatename("nakamoto", {"ipv4": "192.0.2.1:80"})
        self.mine_txs()
        assert_equal(side.getname("satoshi")["data"]["records"], records)
        assert_equal(side.getnamehistory("satoshi")["records"][-1]["value"], records)
        assert_equal(side.resolvename("satoshi.x", "A"), {"status": "NOERROR", "records": [{"name": "satoshi.x", "type": "A", "data": "203.0.113.7"}]})
        assert_equal(side.resolvename("www.satoshi.x", "A")["records"][0]["data"], "satoshi.x.")
        assert_equal(side.resolvename("shop.satoshi.x", "A")["records"][0]["data"], "192.0.2.1")
        assert_equal(side.resolvename("anything.satoshi.x", "A")["records"][0]["data"], "198.51.100.9")
        assert_equal(side.resolvename("nobody.x")["status"], "NXDOMAIN")
        rcode, answers = self.dns_query("satoshi.x")
        assert_equal((rcode, answers), (0, [(1, bytes([203, 0, 113, 7]))]))
        rcode, answers = self.dns_query("satoshi.x", 28)
        assert_equal(answers, [(28, socket.inet_pton(socket.AF_INET6, "2001:db8::7"))])
        assert_equal(self.dns_query("nobody.x")[0], 3)
        assert_equal(self.dns_query("example.com")[0], 5)

        self.log.info("A subname, held by someone else")
        guest = side.getnewaddress()
        created = other.createsubname("nakamoto", "blog", guest, {"ipv4": "198.51.100.77:443"})
        self.mine_txs()
        blog = side.getname("0x" + created["namehash"])
        assert_equal(blog["label"], "blog")
        assert_equal(blog["owner"]["address"], guest)
        assert_equal(side.getname("blog.nakamoto")["namehash"], created["namehash"])
        assert_equal(side.resolvename("blog.nakamoto.x", "A")["records"][0]["data"], "198.51.100.77")
        # Its holder runs it, not the owner of the parent.
        side.updatename("blog.nakamoto", {"ipv4": "198.51.100.78:443"})
        self.mine_txs()
        assert_equal(side.resolvename("blog.nakamoto.x", "A")["records"][0]["data"], "198.51.100.78")
        assert_raises_rpc_error(-8, "does not hold this name", other.updatename, "blog.nakamoto", {"ipv4": None})
        assert_raises_rpc_error(-8, "The subname exists", other.createsubname, "nakamoto", "blog")
        # A delegate with dots names the subname, not a top-level name of the hash of that text.
        delegating = dict(records, map=dict(records["map"], blog={"delegate": "blog.nakamoto"}))
        other.updatename("satoshi", {"records": delegating})
        self.mine_txs()
        assert_equal(side.resolvename("blog.satoshi.x", "A")["records"][0]["data"], "198.51.100.78")
        assert_raises_rpc_error(-8, "DELEGATE: a name", other.setnamerecords, "satoshi", [{"host": "shop", "type": "DELEGATE", "value": "Nakamoto Shop"}])

        self.log.info("A host a subname holds is not in the parent's records")
        assert_raises_rpc_error(-8, "belong to the subname blog", other.setnamerecords, "nakamoto", [{"host": "blog", "type": "A", "value": "192.0.2.9"}])
        assert_raises_rpc_error(-8, "belong to the subname blog", other.setnamerecords, "nakamoto", [{"host": "www.blog", "type": "A", "value": "192.0.2.9"}])
        other.setnamerecords("nakamoto", [{"host": "shop", "type": "A", "value": "192.0.2.10"}])
        self.mine_txs()
        assert_raises_rpc_error(-8, "answer for shop already", other.createsubname, "nakamoto", "shop")

        self.log.info("Deeper subnames are made by the holder of the level above")
        assert_raises_rpc_error(-8, "does not hold this name", other.createsubname, "blog.nakamoto", "dev")
        dev = side.createsubname("blog.nakamoto", "dev")
        self.mine_txs()
        assert_equal(side.getname("dev.blog.nakamoto")["namehash"], dev["namehash"])
        assert_raises_rpc_error(-8, "A name has no dots", side.reservename, "x.nakamoto")

        self.log.info("Dotted text is a subname or nothing; names are lower case")
        # The hash of "nope.nakamoto" is not looked up for a subname that does not exist: anyone can
        # register a name by that hash, and payments and messages would go to it.
        assert_raises_rpc_error(-8, "No such subname", side.getname, "nope.nakamoto")
        assert_raises_rpc_error(-8, "No such subname", side.sendpaymail, "nope.nakamoto", Decimal("0.02"), "hello")
        assert_raises_rpc_error(-8, "lower case", side.getname, "Nakamoto")
        assert_raises_rpc_error(-8, "lower case", side.reservename, "Satoshi2")
        assert_raises_rpc_error(-8, "letters, digits", side.reservename, "<b>bold</b>")
        # A look-alike of another script is no name: only its hash would be.
        assert_raises_rpc_error(-8, "lower-case letters (a-z)", side.getname, "\u0455atoshi")
        assert_raises_rpc_error(-8, "lower-case letters (a-z)", side.sendpaymail, "n\u0430kamoto", Decimal("0.02"), "hello")

        self.log.info("A subname is made over a top-level name of its hash (a squat)")
        squat = subname_hash(side.getname("satoshi")["namehash"], "sq")
        nonce = bytes(range(32))
        squat_address = side.getnewaddress()
        squat_script = side.getaddressinfo(squat_address)["scriptPubKey"]
        reservation = side.sendrawtransaction(self.send_marker(side, reserve_marker(bound_commitment(squat, nonce, squat_script)).hex(), address=squat_address))
        self.mine_txs()
        self.mine_txs()
        side.sendrawtransaction(self.send_marker(side, register_marker(squat, nonce).hex(), inputs=[{"txid": reservation, "vout": 0}]))
        self.mine_txs()
        assert "parent" not in side.getname("0x" + squat[::-1].hex())
        made = other.createsubname("satoshi", "sq", "", {"ipv4": "198.51.100.90:80"})
        assert_equal(made["namehash"], squat[::-1].hex())
        self.mine_txs()
        assert_equal(side.getname("sq.satoshi")["label"], "sq")
        assert_equal(side.resolvename("sq.satoshi.x", "A")["records"][0]["data"], "198.51.100.90")
        assert_raises_rpc_error(-8, "The subname exists", other.createsubname, "satoshi", "sq")

        self.log.info("Standard DNS records, as rows")
        rows = [
            {"host": "@", "type": "A", "value": "198.51.100.80"},
            {"host": "@", "type": "AAAA", "value": "2001:db8::80"},
            {"host": "@", "type": "TXT", "value": "v=spf1 -all"},
            {"host": "@", "type": "MX", "value": "mail", "priority": 5},
            {"host": "www", "type": "CNAME", "value": "@"},
            {"host": "mail", "type": "A", "value": "198.51.100.81"},
            {"host": "_sip._tcp", "type": "SRV", "value": "sip", "priority": 10, "weight": 5, "port": 5060},
        ]
        side.setnamerecords("dev.blog.nakamoto", rows)
        self.mine_txs()
        assert_equal(sorted(map(str, side.getnamerecords("dev.blog.nakamoto"))), sorted(map(str, rows)))
        assert_equal(side.resolvename("dev.blog.nakamoto.x", "A")["records"][0]["data"], "198.51.100.80")
        assert_equal(side.resolvename("dev.blog.nakamoto.x", "MX")["records"][0]["data"], "5 mail.dev.blog.nakamoto.x.")
        assert_equal(side.resolvename("www.dev.blog.nakamoto.x", "A")["records"][0]["data"], "dev.blog.nakamoto.x.")
        assert_equal(side.resolvename("mail.dev.blog.nakamoto.x", "A")["records"][0]["data"], "198.51.100.81")
        assert_equal(side.resolvename("_sip._tcp.dev.blog.nakamoto.x", "SRV")["records"][0]["data"], "10 5 5060 sip.dev.blog.nakamoto.x.")
        assert_equal(self.dns_query("dev.blog.nakamoto.x")[1], [(1, bytes([198, 51, 100, 80]))])
        side.setnamerecords("dev.blog.nakamoto", [])
        self.mine_txs()
        assert_equal(side.getnamerecords("dev.blog.nakamoto"), [])

        self.log.info("Reservations and names are released")
        side.reservename("spare")
        self.mine_txs()
        side.releasereservation("spare")
        self.mine_txs()
        assert_equal([n for n in side.listmynames() if n.get("name") == "spare"], [])
        side.reservename("leaving")
        self.mine_txs()
        self.mine_txs()
        side.registername("leaving", {}, True)
        self.mine_txs()
        assert_equal(side.listnames(10, 0, "leav")[0]["name"], "leaving")
        assert_raises_rpc_error(-8, "pass true to confirm", side.releasename, "leaving", False)
        side.releasename("leaving", True)
        self.mine_txs()
        assert_raises_rpc_error(-8, "No such name", side.getname, "leaving")
        # Free again.
        other.reservename("leaving")
        self.mine_txs()
        self.mine_txs()
        other.registername("leaving")
        self.mine_txs()
        assert_equal(side.getname("leaving")["owner"]["address"], [n for n in other.listmynames() if n.get("name") == "leaving"][0]["address"])

        self.log.info("A name sent to the address of a reservation is not taken for the name reserved")
        side.reservename("pending")
        self.mine_txs()
        held = [n for n in side.listmynames() if n["kind"] == "reservation"]
        assert_equal([(n["name"], n["namehash"]) for n in held], [("pending", hashlib.sha256(b"pending").digest()[::-1].hex())])
        # The address is labelled with the name reserved; anyone can send a name to it.
        junk = other.createsubname("nakamoto", "junk", held[0]["address"])
        self.mine_txs()
        listed = [n for n in side.listmynames() if n.get("namehash") == junk["namehash"]]
        assert_equal(len(listed), 1)
        assert_equal(listed[0]["address"], held[0]["address"])
        assert "name" not in listed[0]
        self.mine_txs()
        side.registername("pending")
        self.mine_txs()
        assert_equal(self.names(side)["pending"]["kind"], "name")
        assert_equal(self.names(side)["pending"]["namehash"], held[0]["namehash"])

        self.log.info("Of copies of a reservation to the wallet's own script, the oldest deep enough is revealed")
        first = side.reservename("twice")
        self.mine_txs()
        address = [n for n in side.listmynames() if n.get("name") == "twice"][0]["address"]
        other.sendrawtransaction(self.send_marker(other, self.marker_of(side, first["txid"]), address=address))
        self.mine_txs()
        assert_equal(len([n for n in side.listmynames() if n.get("name") == "twice"]), 2)
        # The first is deep enough, the copy (a block younger) not yet.
        side.registername("twice")
        self.mine_txs()
        assert_equal(sorted(n["kind"] for n in side.listmynames() if n.get("name") == "twice"), ["name", "reservation"])

        self.log.info("A name is given on with its keys removed")
        keys = other.getnewnamekeys()
        other.updatename("leaving", {"encryptionkey": keys["encryptionkey"], "signingkey": keys["signingkey"], "paymailfee": Decimal("0.01"), "ipv4": "192.0.2.44:80"})
        self.mine_txs()
        receiver = side.getnewaddress()
        other.transfername("leaving", receiver, True)
        self.mine_txs()
        leaving = side.getname("leaving")
        assert_equal(leaving["owner"]["address"], receiver)
        assert_equal(leaving["data"], {"ipv4": "192.0.2.44:80"})

        self.log.info("Undoing blocks undoes the names")
        tip = side.getbestblockhash()
        state = side.getname("satoshi")
        count = side.getblockcount()
        registration_block = side.getblockhash(registered_height)
        side.invalidateblock(registration_block)
        assert_equal(side.getbitnamesinfo()["names"], 0)
        assert_raises_rpc_error(-8, "No such name", side.getname, "satoshi")
        side.reconsiderblock(registration_block)
        assert_equal(side.getbestblockhash(), tip)
        assert_equal(side.getblockcount(), count)
        assert_equal(side.getname("satoshi"), state)
        assert_equal(side.getbitnamesinfo()["names"], 10)
        self.check_in_sync()

        self.log.info("Releasing a name releases its subnames, whoever holds them")
        assert "Its subnames are released with it" in other.help("releasename")
        assert "released with it" in other.help("createsubname")
        other.releasename("nakamoto", True)
        self.mine_txs()
        assert_raises_rpc_error(-8, "No such name", side.getname, "0x" + created["namehash"])
        assert_raises_rpc_error(-8, "No such name", side.getname, "0x" + dev["namehash"])
        assert_raises_rpc_error(-8, "No such name", side.getname, "0x" + junk["namehash"])
        assert_equal(side.getbitnamesinfo()["names"], 6)
        self.check_in_sync()

        self.log.info("The names, rebuilt from the blocks, are the same (as on upgrade, when the store's format changed)")
        names = side.listnames()
        history = side.getnamehistory("satoshi")
        self.restart_node(0)
        assert_equal(side.listnames(), names)
        self.restart_node(0, extra_args=self.extra_args[0] + ["-reindex-chainstate"])
        assert_equal(side.listnames(), names)
        assert_equal(side.getnamehistory("satoshi"), history)
        assert_equal(side.getname("satoshi", registered_height)["data"], {"ipv4": "203.0.113.7:443", "paymailfee": Decimal("0.001")})

        self.log.info("sendall leaves the outputs of names alone")
        self.connect_nodes(0, 1)
        held = sorted(n["txid"] + str(n["vout"]) for n in side.listmynames())
        assert held
        side.sendall([other.getnewaddress()])
        self.mine_txs()
        assert_equal(sorted(n["txid"] + str(n["vout"]) for n in side.listmynames()), held)


if __name__ == "__main__":
    BitNamesTest(__file__).main()
