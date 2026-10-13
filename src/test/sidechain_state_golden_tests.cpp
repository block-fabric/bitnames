// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Golden values of the sidechain rules: a fixed sequence of blocks, connected one after another to a
// state of their own, and the hash of the state and of what each coinbase pays out after each block.
// The values were recorded with the hooks of sidechain/hooks.h off, as they are unless a sidechain
// installs its own: any change in what a block does to the state, or in the order of what its coinbase
// pays, fails here. A change meant to make one has to say so, and record the values again.

#include <addresstype.h>
#include <arith_uint256.h>
#include <bitnames/state.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <sidechain/store.h>
#include <test/util/setup_common.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <functional>
#include <string>
#include <vector>
namespace {

/** A sidechain state, and the record of its mainchain, that blocks are connected to one after another. */
struct GoldenChain {
    Consensus::SidechainParams params;
    sidechain::EmptyStore empty;
    sidechain::StoreOverlay store{empty, /*journal=*/true};
    sidechain::State state{store};
    sidechain::Mainchain mainchain;
    uint256 prev{uint256{0xab}};
    int height{0};
    //! What the coinbase of the last block connected paid out, after its first output.
    std::vector<CTxOut> paid;

    GoldenChain() { Main(); }

    /** Append a block to the record of the mainchain. */
    void Main(const std::function<void(sidechain::MainBlock&)>& fill = {})
    {
        sidechain::MainBlock block;
        const int h{mainchain.Height() + 1};
        block.hash = ArithToUint256(arith_uint256{static_cast<uint64_t>(0x10000 + h)} << 128);
        block.prev_hash = mainchain.TipHash();
        block.time = 1'700'000'000 + h;
        if (fill) fill(block);
        BOOST_REQUIRE(mainchain.Append(block));
    }

    /**
     * A block of these transactions on the last one, after a mainchain block with these events, its
     * coinbase paying what the rules want (worked out on an overlay that is dropped, as a miner does);
     * then a mainchain block that commits to it. Connected if valid; why not, if not.
     */
    std::string Connect(const std::vector<CTransactionRef>& txs, const std::function<void(sidechain::MainBlock&)>& events = {})
    {
        Main(events);
        CBlock block;
        block.nVersion = 0x20000000;
        block.hashPrevBlock = prev;
        block.nTime = 1'700'000'000 + height + 1;
        block.nBits = 0x207fffff;
        CMutableTransaction coinbase;
        coinbase.vin.resize(1);
        coinbase.vin[0].prevout.SetNull();
        coinbase.vin[0].scriptSig = CScript() << (height + 1) << OP_0;
        coinbase.vout.emplace_back(0, CScript() << OP_TRUE);
        block.vtx.push_back(MakeTransactionRef(coinbase));
        block.vtx.insert(block.vtx.end(), txs.begin(), txs.end());
        {
            sidechain::StoreOverlay scratch{store, /*journal=*/false};
            sidechain::State dry{scratch};
            std::vector<CTxOut> payouts, tx_payouts;
            std::string reason;
            if (dry.ApplyMainEvents(mainchain.Height(), mainchain, height + 1, params, payouts, reason) &&
                dry.ApplyTxs(block, height + 1, params, dry.MainPending(mainchain, params), /*peg_step=*/true, tx_payouts, reason)) {
                for (const CTxOut& out : dry.TakePayouts(std::move(payouts), std::move(tx_payouts))) coinbase.vout.push_back(out);
            }
        }
        block.vtx[0] = MakeTransactionRef(coinbase);
        block.hashMerkleRoot = BlockMerkleRoot(block);
        Main([&](sidechain::MainBlock& b) { b.bmm = block.GetHash(); });
        sidechain::StoreOverlay block_store{store, /*journal=*/false};
        sidechain::State block_state{block_store};
        CAmount minted{0};
        std::string reason;
        if (!block_state.ConnectBlock(block, height + 1, params, mainchain, minted, reason)) return reason;
        block_store.MergeInto(store);
        store.TakeUndo();
        ++height;
        prev = block.GetHash();
        paid.assign(coinbase.vout.begin() + 1, coinbase.vout.end());
        return {};
    }

    /** The hash of what the last block paid out, in order. */
    uint256 PaidHash() const
    {
        HashWriter hasher{};
        hasher << paid;
        return hasher.GetHash();
    }

    /** The state and what the last block paid out are those recorded. */
    void Check(const std::string& state_hash, const std::string& paid_hash, size_t paid_count)
    {
        BOOST_CHECK_EQUAL(state.Hash().GetHex(), state_hash);
        BOOST_CHECK_EQUAL(PaidHash().GetHex(), paid_hash);
        BOOST_CHECK_EQUAL(paid.size(), paid_count);
    }
};

/** The key of the tests: always the same, so that its signatures are too. */
CKey FixedKey(uint8_t n)
{
    std::vector<unsigned char> secret(32, 0);
    secret[0] = 0x5e;
    secret[31] = n;
    CKey key;
    key.Set(secret.begin(), secret.end(), /*fCompressedIn=*/true);
    return key;
}

/** A transaction with these outputs, spending a coin of its own so that every one is different. */
CTransactionRef Tx(std::vector<CTxOut> outputs, uint32_t& nonce)
{
    CMutableTransaction tx;
    ++nonce;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(ArithToUint256(arith_uint256{nonce} << 64)), nonce});
    tx.vout = std::move(outputs);
    return MakeTransactionRef(std::move(tx));
}

CTxOut RefundOut(const CKey& key, const COutPoint& withdrawal)
{
    sidechain::RefundRequest request;
    request.withdrawal = withdrawal;
    BOOST_REQUIRE(key.SignCompact(MessageHash(sidechain::RefundMessage(withdrawal)), request.signature));
    return CTxOut{0, sidechain::RefundScript(request)};
}

/** A transaction that reserves `name` under `nonce`, to `holder`. */
CTransactionRef Reservation(const bitnames::NameHash& name, const uint256& nonce, const CScript& holder, uint32_t& n)
{
    bitnames::Marker marker;
    marker.operation = bitnames::Reserve{bitnames::ReservationCommitment(name, nonce, holder)};
    marker.reservation_outputs = {0};
    return Tx({CTxOut{0, holder}, CTxOut{0, bitnames::MarkerScript(marker)}}, n);
}

/** A transaction that registers `name`, revealing `reservation`, to `holder`; `extra` outputs after the marker. */
CTransactionRef Registration(const COutPoint& reservation, const bitnames::NameHash& name, const uint256& nonce, const CScript& holder, const std::vector<CTxOut>& extra, uint32_t& n)
{
    bitnames::Marker marker;
    bitnames::Register reg;
    reg.name = name;
    reg.nonce = nonce;
    marker.operation = reg;
    marker.name_outputs = {0};
    std::vector<CTxOut> outputs{CTxOut{0, holder}, CTxOut{0, bitnames::MarkerScript(marker)}};
    outputs.insert(outputs.end(), extra.begin(), extra.end());
    CMutableTransaction tx{*Tx(outputs, n)};
    tx.vin.emplace_back(reservation);
    return MakeTransactionRef(std::move(tx));
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_state_golden_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(refunds_in_name_registrations)
{
    // Refunds of withdrawals asked for in the transactions that register names: the names go first,
    // the refunds after, paid in the order of the transactions; a registration that breaks the rules
    // takes its refund with it.
    GoldenChain chain;
    uint32_t nonce{0};
    const CKey key{FixedKey(1)};
    const CScript pay{GetScriptForDestination(WitnessV0KeyHash{key.GetPubKey()})};
    const CScript holder{GetScriptForDestination(WitnessV0KeyHash{FixedKey(2).GetPubKey()})};

    // Block 1: withdrawals, refundable by the key; reservations of two names; a deposit.
    std::vector<CTxOut> withdrawal_outs;
    for (int i{0}; i < 4; ++i) withdrawal_outs.emplace_back(COIN + i, sidechain::WithdrawalScript(1000, key.GetPubKey().GetID(), pay));
    const CTransactionRef withdrawals{Tx(withdrawal_outs, nonce)};
    const bitnames::NameHash alice{bitnames::HashName("alice")}, bob{bitnames::HashName("bob")}, carol{bitnames::HashName("carol")};
    const CTransactionRef reserve_alice{Reservation(alice, uint256{0xa1}, holder, nonce)};
    const CTransactionRef reserve_bob{Reservation(bob, uint256{0xb0}, holder, nonce)};
    const auto deposit{[](sidechain::MainBlock& b) {
        sidechain::MainDeposit d;
        d.destination = "deposit";
        d.amount = 5 * COIN;
        d.txid = uint256{0xd1};
        b.deposits.push_back(d);
    }};
    BOOST_REQUIRE_EQUAL(chain.Connect({withdrawals, reserve_alice, reserve_bob}, deposit), "");
    chain.Check("c00f34024109fdf9545234ec54345d622e7aa2b42b37bc368fe40a22153766a1", "7382c6d27934e5fa2e1da7140e784cbf08a8cf755594c3448a27ec4cbf3b2184", 1);
    const auto refund{[&](uint32_t n) { return RefundOut(key, COutPoint{withdrawals->GetHash(), n}); }};
    const auto refunded{[&](uint32_t n) { return CTxOut{COIN + n, pay}; }};

    // A registration of a name nobody reserved, with a refund: not valid, the refund with it.
    const uint256 before{chain.state.Hash()};
    const CTransactionRef unreserved{Registration(COutPoint{reserve_bob->GetHash(), 0}, carol, uint256{0xc0}, holder, {refund(3)}, nonce)};
    BOOST_CHECK_EQUAL(chain.Connect({unreserved}), "bad-bn-no-reservation");
    BOOST_CHECK(chain.state.Hash() == before);

    // Block 2: alice registered by a transaction that asks for a refund too, then a refund on its own.
    const CTransactionRef registered_alice{Registration(COutPoint{reserve_alice->GetHash(), 0}, alice, uint256{0xa1}, holder, {refund(1)}, nonce)};
    const CTransactionRef refund_alone{Tx({refund(0)}, nonce)};
    BOOST_REQUIRE_EQUAL(chain.Connect({registered_alice, refund_alone}), "");
    chain.Check("98c3064a2e32c5c8e0d719e99cad19fd54e53873b74ee1af763f38ca668b95de", "c961078d7ba68fc1bfe1f030ae80df8b66934baecd015bb157c7db1de33d43b3", 2);
    BOOST_CHECK(chain.paid[0] == refunded(1));
    BOOST_CHECK(chain.paid[1] == refunded(0));
    BOOST_CHECK(chain.state.BitNames().GetName(alice));

    // Block 3: bob registered by a transaction that asks for two refunds, before and after its marker.
    const CTransactionRef registered_bob{[&] {
        CMutableTransaction tx{*Registration(COutPoint{reserve_bob->GetHash(), 0}, bob, uint256{0xb0}, holder, {refund(3)}, nonce)};
        tx.vout.insert(tx.vout.begin() + 1, refund(2));
        return MakeTransactionRef(std::move(tx));
    }()};
    BOOST_REQUIRE_EQUAL(chain.Connect({registered_bob}), "");
    chain.Check("4323593959dcb23577db08e96a3609dc899d313a6e144d39b30b721ca298f542", "4460ec6ba5928d8ffbfb202204f3c40cdb1168016dc87f8f1f5619ba690eec85", 2);
    BOOST_CHECK(chain.paid[0] == refunded(2));
    BOOST_CHECK(chain.paid[1] == refunded(3));
    BOOST_CHECK(chain.state.BitNames().GetName(bob));
}

BOOST_AUTO_TEST_SUITE_END()
