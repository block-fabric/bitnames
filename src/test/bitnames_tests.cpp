// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitnames/dns.h>
#include <bitnames/rpcutil.h>
#include <bitnames/state.h>
#include <key.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace bitnames;

namespace {

/**
 * The names on a store of their own, with the shape the tests were written for: undo data as a list,
 * copies, comparison, and the tables as maps (read whole: for tests).
 */
struct TUndo {
    std::vector<sidechain::StoreUndo> parts;
    template <typename Stream>
    void Serialize(Stream& s) const { s << parts; }
    template <typename Stream>
    void Unserialize(Stream& s) { s >> parts; }
};

class TState
{
public:
    TState() : m_store{std::make_unique<sidechain::StoreOverlay>(m_empty, /*journal=*/true)} {}
    /** A copy: every entry, on a store of its own. */
    TState(const TState& other) : TState()
    {
        other.m_store->ForEach({}, [&](const sidechain::StoreBytes& k, const sidechain::StoreBytes& v) {
            m_store->Put(k, v);
            return true;
        });
        m_store->TakeUndo();
    }
    TState& operator=(const TState& other)
    {
        // A store of our own, over our own (empty) base, with the other's entries.
        auto store{std::make_unique<sidechain::StoreOverlay>(m_empty, /*journal=*/true)};
        other.m_store->ForEach({}, [&](const sidechain::StoreBytes& k, const sidechain::StoreBytes& v) {
            store->Put(k, v);
            return true;
        });
        store->TakeUndo();
        m_store = std::move(store);
        return *this;
    }
    bool ApplyTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, TUndo& undo, std::string& reason)
    {
        const bool ok{bitnames::State{*m_store}.ApplyTx(tx, height, params, reason)};
        undo.parts.push_back(m_store->TakeUndo());
        return ok;
    }
    bool CheckTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reason) const
    {
        return View().CheckTx(tx, height, params, reason);
    }
    void Revert(const TUndo& undo)
    {
        for (auto it{undo.parts.rbegin()}; it != undo.parts.rend(); ++it) m_store->Revert(*it);
        m_store->TakeUndo();
    }
    bitnames::State View() const { return bitnames::State{*m_store}; }
    operator bitnames::State() const { return View(); }
    std::map<COutPoint, Token> Tokens() const
    {
        std::map<COutPoint, Token> all;
        View().ForEachToken([&](const COutPoint& o, const Token& t) { all.emplace(o, t); return true; });
        return all;
    }
    std::map<NameHash, NameRecord> Names() const
    {
        std::map<NameHash, NameRecord> all;
        View().ForEachName([&](const NameHash& h, const NameRecord& r) { all.emplace(h, r); return true; });
        return all;
    }
    std::map<Txid, uint256> Reservations() const
    {
        // The table itself (0x22, by the reservation's transaction).
        std::map<Txid, uint256> all;
        sidechain::Table<uint256, ReservationRecord>{0x22}.ForEach(*m_store, [&](const uint256& id, const ReservationRecord& r) {
            all.emplace(Txid::FromUint256(id), r.commitment);
            return true;
        });
        return all;
    }
    std::optional<NameHash> NameOfSeq(uint32_t seq) const { return View().NameOfSeq(seq); }
    /** How many entries a table has. */
    size_t EntriesUnder(uint8_t table) const
    {
        size_t n{0};
        m_store->ForEach(sidechain::StoreBytes{table}, [&](const sidechain::StoreBytes&, const sidechain::StoreBytes&) { return ++n, true; });
        return n;
    }
    uint32_t NextSeq() const { return View().NextSeq(); }
    friend bool operator==(const TState& a, const TState& b) { return sidechain::StoreHash(*a.m_store) == sidechain::StoreHash(*b.m_store); }

private:
    sidechain::EmptyStore m_empty;
    std::unique_ptr<sidechain::StoreOverlay> m_store;
};
CScript Payee(uint8_t n)
{
    return CScript() << OP_0 << std::vector<unsigned char>(20, n);
}

/** A transaction spending `inputs`, with `names` and `reservations` outputs, a marker, and `salt` to tell it apart. */
CMutableTransaction Tx(const std::vector<COutPoint>& inputs, size_t names, size_t reservations, std::optional<Operation> op, uint8_t salt)
{
    CMutableTransaction tx;
    for (const COutPoint& in : inputs) tx.vin.emplace_back(in);
    if (tx.vin.empty()) tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{salt}), 7});
    Marker marker;
    marker.operation = std::move(op);
    for (size_t i{0}; i < names; ++i) {
        marker.name_outputs.push_back(tx.vout.size());
        tx.vout.emplace_back(0, Payee(salt));
    }
    for (size_t i{0}; i < reservations; ++i) {
        marker.reservation_outputs.push_back(tx.vout.size());
        tx.vout.emplace_back(0, Payee(salt));
    }
    tx.vout.emplace_back(0, MarkerScript(marker));
    tx.vout.emplace_back(1000, Payee(salt));
    return tx;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(bitnames_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(seq_display)
{
    // As BitNames shows the numbers of names.
    const std::vector<std::pair<uint32_t, std::string>> vectors{
        {0, "1739-0029"}, {1, "1739-0129"}, {24, "2731-0420"}, {76928008, "9999-9899"},
        {76928009, "9999-9999"}, {76928013, "0000-0300"}, {100000000, "0001-1739-0029"}};
    for (const auto& [seq, text] : vectors) {
        BOOST_CHECK_EQUAL(FormatSeq(seq), text);
        BOOST_CHECK(ParseSeq(text) == seq);
    }
    BOOST_CHECK(!ParseSeq("1739-002"));
    BOOST_CHECK(!ParseSeq("0000-1739-0029"));
    BOOST_CHECK(!ParseSeq("x739-0029"));
    BOOST_CHECK(!ParseSeq("9999-9999-9999"));
}

BOOST_AUTO_TEST_CASE(marker_roundtrip)
{
    Marker marker;
    Register reg;
    reg.name = HashName("satoshi");
    reg.nonce = uint256{5};
    reg.data.ipv4 = SocketV4{{203, 0, 113, 7}, 443};
    reg.data.paymail_fee = 1000;
    marker.operation = reg;
    marker.name_outputs = {0, 2};
    marker.memos = {{3, {1, 2, 3}}};
    const CScript script{MarkerScript(marker)};
    BOOST_CHECK(IsMarkerScript(script));
    BOOST_CHECK(ParseMarkerScript(script) == marker);
    // One way to write it: anything appended is not a marker.
    CScript longer{script};
    longer << OP_TRUE;
    BOOST_CHECK(!ParseMarkerScript(longer));
}

BOOST_AUTO_TEST_CASE(names_move_and_cannot_be_destroyed)
{
    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("satoshi")};
    const uint256 nonce{9};

    // Reserve, then register.
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, nonce)}, 1)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason), reason);
    const COutPoint reservation{reserve.GetHash(), 0};
    BOOST_CHECK(state.Tokens().at(reservation).kind == Token::Kind::RESERVATION);
    // A wrong nonce opens nothing.
    Register wrong{name, uint256{8}, {}};
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({reservation}, 1, 0, wrong, 2)}, 2, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-no-reservation");
    const CMutableTransaction reg{Tx({reservation}, 1, 0, Register{name, nonce, {}}, 2)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{reg}, 2, params, undo, reason), reason);
    const COutPoint holder{reg.GetHash(), 0};
    BOOST_CHECK(state.Tokens().at(holder).id == name);
    BOOST_CHECK(state.Names().at(name).seq == 0);
    BOOST_CHECK(state.Reservations().empty());

    // Spending it without carrying it on is invalid; carrying it on moves it.
    CMutableTransaction plain;
    plain.vin.emplace_back(holder);
    plain.vout.emplace_back(1000, Payee(3));
    BOOST_CHECK(!state.CheckTx(CTransaction{plain}, 3, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-unbalanced");
    const CMutableTransaction move{Tx({holder}, 1, 0, std::nullopt, 3)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{move}, 3, params, undo, reason), reason);
    BOOST_CHECK(!state.Tokens().contains(holder));
    BOOST_CHECK(state.Tokens().at(COutPoint{move.GetHash(), 0}).id == name);
    // A name output with value is no name output.
    CMutableTransaction valued{Tx({COutPoint{move.GetHash(), 0}}, 1, 0, std::nullopt, 4)};
    valued.vout[0].nValue = 1;
    BOOST_CHECK(!CheckMarker(CTransaction{valued}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-token-output");
}

BOOST_AUTO_TEST_CASE(one_registration_of_a_name_per_block)
{
    // The original let two registrations of the same name into one block, each with a reservation
    // of the same commitment (whoever sees the first can copy it), and then could not undo the block.
    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("satoshi")};
    const uint256 nonce{9};
    const uint256 commitment{ReservationCommitment(name, nonce)};
    const CMutableTransaction first{Tx({}, 0, 1, Reserve{commitment}, 1)};
    const CMutableTransaction copy{Tx({}, 0, 1, Reserve{commitment}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{first}, 1, params, undo, reason));
    BOOST_REQUIRE(state.ApplyTx(CTransaction{copy}, 1, params, undo, reason));

    const TState before{state};
    TUndo block;
    BOOST_REQUIRE(state.ApplyTx(CTransaction{Tx({COutPoint{first.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 3)}, 2, params, block, reason));
    BOOST_CHECK(!state.ApplyTx(CTransaction{Tx({COutPoint{copy.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 4)}, 2, params, block, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-name-taken");
    // And the block undoes cleanly.
    state.Revert(block);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(updates_keep_their_history)
{
    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("satoshi")};
    const uint256 nonce{9};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, nonce)}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason));
    NameData data;
    data.paymail_fee = 500;
    const CMutableTransaction reg{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{name, nonce, data}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reg}, 2, params, undo, reason));

    const TState before{state};
    TUndo block;
    NameUpdates updates;
    updates.paymail_fee.kind = UpdateKind::DELETE;
    updates.commitment.kind = UpdateKind::SET;
    updates.commitment.value = uint256{3};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{reg.GetHash(), 0}}, 1, 0, UpdateName{updates}, 3)}, 3, params, block, reason), reason);
    const NameRecord record{state.Names().at(name)};
    BOOST_CHECK(!record.Current().paymail_fee);
    BOOST_CHECK(record.Current().commitment == uint256{3});
    BOOST_CHECK(state.View().DataAtHeight(record, 2)->paymail_fee == 500);
    BOOST_CHECK(!state.View().DataAtHeight(record, 1));

    // The undo survives serialization and restores the state.
    DataStream stream{};
    stream << block;
    TUndo read;
    stream >> read;
    state.Revert(read);
    BOOST_CHECK(state == before);
}

namespace {
COutPoint Registered(TState& state, const std::string& text, NameData data, uint8_t salt, const Consensus::BitNamesParams& params);
} // namespace

BOOST_AUTO_TEST_CASE(history_kept_apart)
{
    // The history of a name is kept apart from its record, one entry per value: an update reads and
    // writes the record as it is now and one entry per field it changes, however long the history.
    // The data at every height, and the history, are what keeping every value in the record gave.
    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("satoshi")};
    const uint256 nonce{9};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, nonce)}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason));
    NameData data;
    data.paymail_fee = 1;
    const CMutableTransaction reg{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{name, nonce, data}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reg}, 2, params, undo, reason));
    COutPoint holder{reg.GetHash(), 0};
    const size_t record_size{sidechain::EncodeValue(state.Names().at(name)).size()};

    // The model: every value of the fee, with its height, in order.
    std::vector<std::pair<int, std::optional<CAmount>>> model{{2, 1}};
    uint8_t salt{10};
    for (int height{3}; height <= 40; ++height) {
        // Two updates in some blocks: the second, spending the first's output, is the later.
        for (int i{0}; i < (height % 3 == 0 ? 2 : 1); ++i) {
            NameUpdates updates;
            const bool remove{height % 7 == 0 && i == 0};
            updates.paymail_fee.kind = remove ? UpdateKind::DELETE : UpdateKind::SET;
            if (!remove) updates.paymail_fee.value = height * 10 + i;
            const CMutableTransaction tx{Tx({holder}, 1, 0, UpdateName{updates}, ++salt)};
            BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, height, params, undo, reason), reason);
            holder = COutPoint{tx.GetHash(), 0};
            model.emplace_back(height, updates.paymail_fee.value);
        }
    }
    const NameRecord record{state.Names().at(name)};
    // The record is as large as it was after the registration.
    BOOST_CHECK_EQUAL(sidechain::EncodeValue(record).size(), record_size);
    BOOST_CHECK(record.Current().paymail_fee == model.back().second);
    BOOST_CHECK(!state.View().DataAtHeight(record, 1));
    for (int at{2}; at <= 45; ++at) {
        std::optional<CAmount> expected;
        for (const auto& [h, fee] : model) {
            if (h <= at) expected = fee;
        }
        BOOST_CHECK_MESSAGE(state.View().DataAtHeight(record, at)->paymail_fee == expected, at);
        BOOST_CHECK(!state.View().DataAtHeight(record, at)->ipv4);
    }
    const NameHistory history{state.View().History(record)};
    BOOST_REQUIRE_EQUAL(history.paymail_fee.size(), model.size());
    for (size_t i{0}; i < model.size(); ++i) {
        BOOST_CHECK_EQUAL(history.paymail_fee[i].height, model[i].first);
        BOOST_CHECK(history.paymail_fee[i].value == model[i].second);
    }
    // The fields never set have their registration's entry.
    BOOST_CHECK_EQUAL(history.ipv4.size(), 1U);
    BOOST_CHECK(!history.ipv4[0].value);

    // Released, the name goes with its history: registered again, it starts anew.
    TUndo released;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({holder}, 0, 0, Release{}, ++salt)}, 41, params, released, reason), reason);
    BOOST_CHECK(!state.Names().contains(name));
    BOOST_CHECK_EQUAL(state.EntriesUnder(0x2c), 0U);
    const COutPoint again{Registered(state, "satoshi", {}, 60, params)};
    (void)again;
    BOOST_CHECK_EQUAL(state.View().History(state.Names().at(name)).paymail_fee.size(), 1U);
    BOOST_CHECK_EQUAL(state.EntriesUnder(0x2c), 7U);
}

BOOST_AUTO_TEST_CASE(icann_names)
{
    BOOST_CHECK(IsIcannName("example.com"));
    BOOST_CHECK(IsIcannName("bitcoin"));
    BOOST_CHECK(!IsIcannName("a.b.com"));
    BOOST_CHECK(!IsIcannName("Example.com"));
    BOOST_CHECK(!IsIcannName("-bad.com"));
    BOOST_CHECK(!IsIcannName(""));

    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("example.com")};
    const uint256 nonce{9};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, nonce)}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason));
    BOOST_REQUIRE(state.ApplyTx(CTransaction{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 2)}, 2, params, undo, reason));

    // No authority: nobody can mark names.
    IcannNames icann;
    icann.names = {"example.com"};
    icann.signature.assign(64, 0);
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({}, 0, 0, icann, 5)}, 3, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-icann-disabled");

    // With one: its signature, of the names and the first input, marks them.
    CKey authority;
    authority.MakeNewKey(true);
    params.icann_authority = uint256{XOnlyPubKey{authority.GetPubKey()}};
    CMutableTransaction tx{Tx({}, 0, 0, icann, 5)};
    icann.signature.resize(64);
    BOOST_REQUIRE(authority.SignSchnorr(IcannMessage(icann.names, tx.vin[0].prevout), icann.signature, nullptr, uint256{}));
    tx = Tx({}, 0, 0, icann, 5);
    const TState before{state};
    TUndo block;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 3, params, block, reason), reason);
    BOOST_CHECK(state.Names().at(name).icann);
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({}, 0, 0, icann, 6)}, 4, params, reason));
    state.Revert(block);
    BOOST_CHECK(state == before);
}

namespace {
/** A registered name, the outpoint that holds it, in a state. */
COutPoint Registered(TState& state, const std::string& text, NameData data, uint8_t salt, const Consensus::BitNamesParams& params)
{
    TUndo undo;
    std::string reason;
    const NameHash name{HashName(text)};
    const uint256 nonce{salt};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, nonce)}, salt)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason), reason);
    Register reg{name, nonce, data};
    const CMutableTransaction registration{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, reg, static_cast<uint8_t>(salt + 100))};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{registration}, 2, params, undo, reason), reason);
    return COutPoint{registration.GetHash(), 0};
}

std::vector<std::string> Texts(const dns::Answer& answer)
{
    std::vector<std::string> texts;
    for (const auto& record : answer.records) texts.push_back(dns::TypeName(record.type) + " " + record.text);
    return texts;
}
} // namespace

BOOST_AUTO_TEST_CASE(old_markers_still_parse)
{
    // A marker without records or a public text is written as before the extension, so markers
    // already on the chain mean what they meant.
    Marker marker;
    marker.operation = Register{HashName("core"), uint256{1}, {}};
    marker.name_outputs = {0};
    const CScript script{MarkerScript(marker)};
    const auto data{script.begin() + 2};
    BOOST_CHECK_EQUAL(script[1] < OP_PUSHDATA1 ? script[2 + 4] : script[3 + 4], MARKER_VERSION);
    (void)data;
    BOOST_CHECK(ParseMarkerScript(script) == marker);
    Register with_records{HashName("core"), uint256{1}, {}};
    with_records.data.records = R"({"ip":"203.0.113.7"})";
    with_records.text = "core";
    marker.operation = with_records;
    BOOST_CHECK(ParseMarkerScript(MarkerScript(marker)) == marker);
    // The text has to be the name's.
    with_records.text = "other";
    marker.operation = with_records;
    CMutableTransaction tx{Tx({}, 1, 0, with_records, 9)};
    std::string reason;
    BOOST_CHECK(!CheckMarker(CTransaction{tx}, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-name-text");
}

BOOST_AUTO_TEST_CASE(release)
{
    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    // A reservation given up.
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{uint256{5}}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason));
    BOOST_CHECK_EQUAL(state.Reservations().size(), 1U);
    const TState before{state};
    TUndo block;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{reserve.GetHash(), 0}}, 0, 0, Release{}, 2)}, 2, params, block, reason), reason);
    BOOST_CHECK(state.Reservations().empty());
    BOOST_CHECK(state.Tokens().empty());
    state.Revert(block);
    BOOST_CHECK(state == before);

    // A name given up: free again, and taken by someone else.
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({core}, 1, 0, Release{}, 3)}, 3, params, reason));
    TUndo released;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({core}, 0, 0, Release{}, 4)}, 3, params, released, reason), reason);
    BOOST_CHECK(!state.Names().contains(HashName("core")));
    const TState after_release{state};
    Registered(state, "core", {}, 20, params);
    BOOST_CHECK(state.Names().contains(HashName("core")));
    state = after_release;
    state.Revert(released);
    BOOST_CHECK(state.Names().contains(HashName("core")));
    BOOST_CHECK(state.NameOfSeq(state.Names().at(HashName("core")).seq) == HashName("core"));
}

BOOST_AUTO_TEST_CASE(subnames)
{
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    RegisterSubname sub;
    sub.label = "blog";
    sub.data.ipv4 = SocketV4{{198, 51, 100, 7}, 443};
    // Without the parent there is no subname.
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({}, 1, 0, sub, 30)}, 3, params, reason));
    // The parent is spent and carried on; the subname comes after it.
    const TState before{state};
    TUndo block;
    const CMutableTransaction tx{Tx({core}, 2, 0, sub, 31)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 3, params, block, reason), reason);
    const NameHash blog{SubnameHash(HashName("core"), "blog")};
    BOOST_CHECK(state.Tokens().at(COutPoint{tx.GetHash(), 0}).id == HashName("core"));
    BOOST_CHECK(state.Tokens().at(COutPoint{tx.GetHash(), 1}).id == blog);
    BOOST_CHECK(state.Names().at(blog).parent == HashName("core"));
    // Once.
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({COutPoint{tx.GetHash(), 0}}, 2, 0, sub, 32)}, 4, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-name-taken");
    sub.label = "Blog";
    BOOST_CHECK(!CheckMarker(CTransaction{Tx({COutPoint{tx.GetHash(), 0}}, 2, 0, sub, 33)}, reason));
    state.Revert(block);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(release_takes_subnames)
{
    // Releasing a name releases its subnames, at any depth, whoever holds them: whoever registers the
    // name next starts with an empty namespace, and nobody keeps a part of it.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo setup;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    RegisterSubname sub;
    sub.label = "blog";
    const CMutableTransaction made_blog{Tx({core}, 2, 0, sub, 31)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{made_blog}, 3, params, setup, reason), reason);
    const NameHash blog{SubnameHash(HashName("core"), "blog")};
    // A subname of the subname, held apart (here: the output of its own transaction).
    sub.label = "post";
    const CMutableTransaction made_post{Tx({COutPoint{made_blog.GetHash(), 1}}, 2, 0, sub, 32)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{made_post}, 4, params, setup, reason), reason);
    const NameHash post{SubnameHash(blog, "post")};
    const COutPoint core_now{made_blog.GetHash(), 0};
    BOOST_REQUIRE(state.Names().contains(post));

    // Before the rule's height, the subnames stay (as on the test network until then).
    params.audit_rules_height = 100;
    {
        TState old_rules{state};
        TUndo undo;
        BOOST_REQUIRE_MESSAGE(old_rules.ApplyTx(CTransaction{Tx({core_now}, 0, 0, Release{}, 40)}, 5, params, undo, reason), reason);
        BOOST_CHECK(!old_rules.Names().contains(HashName("core")));
        BOOST_CHECK(old_rules.Names().contains(blog));
    }
    params.audit_rules_height = 0;

    // A release cannot keep one of the names it takes: carrying on the subname (spent first) and
    // releasing the name it is under.
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({COutPoint{made_post.GetHash(), 0}, core_now}, 1, 0, Release{}, 41)}, 5, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-release-subname-kept");
    // Carrying on the name and releasing its subname is fine.
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{Tx({core_now, COutPoint{made_post.GetHash(), 0}}, 1, 0, Release{}, 43)}, 5, params, reason), reason);

    const TState before{state};
    TUndo undo;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({core_now}, 0, 0, Release{}, 42)}, 5, params, undo, reason), reason);
    BOOST_CHECK(!state.Names().contains(HashName("core")));
    BOOST_CHECK(!state.Names().contains(blog));
    BOOST_CHECK(!state.Names().contains(post));
    // The holders keep their coins, which carry no name any more.
    for (const auto& [outpoint, token] : state.Tokens()) {
        BOOST_CHECK(token.id != blog && token.id != post && token.id != HashName("core"));
    }
    // The next holder of the name makes the same subname.
    const COutPoint again{Registered(state, "core", {}, 50, params)};
    sub.label = "blog";
    TUndo next;
    BOOST_CHECK_MESSAGE(state.ApplyTx(CTransaction{Tx({again}, 2, 0, sub, 51)}, 6, params, next, reason), reason);

    // Undone, all is back as it was.
    TState reverted{state};
    reverted.Revert(next);
    TState back{before};
    TUndo redo;
    BOOST_REQUIRE(back.ApplyTx(CTransaction{Tx({core_now}, 0, 0, Release{}, 42)}, 5, params, redo, reason));
    back.Revert(redo);
    BOOST_CHECK(back == before);
}

BOOST_AUTO_TEST_CASE(squatted_subname_gives_way)
{
    // A name registered by the hash of a subname (no text could hash to it) is a squat: it does not
    // resolve as the subname, and gives way when the parent's holder makes the subname.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    const NameHash www{SubnameHash(HashName("core"), "www")};
    // The squat: reserve and register the raw hash.
    TUndo undo;
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(www, uint256{9})}, 90)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 3, params, undo, reason));
    NameData squat_data;
    squat_data.ipv4 = SocketV4{{6, 6, 6, 6}, 80};
    const CMutableTransaction squat{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{www, uint256{9}, squat_data}, 91)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{squat}, 4, params, undo, reason), reason);
    BOOST_REQUIRE(state.Names().contains(www));
    BOOST_CHECK_EQUAL(Resolve(state, "www.core.x", dns::A).rcode, dns::NXDOMAIN);
    // Before the rule's height it holds the label.
    RegisterSubname sub;
    sub.label = "www";
    sub.data.ipv4 = SocketV4{{198, 51, 100, 9}, 443};
    params.audit_rules_height = 100;
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({core}, 2, 0, sub, 92)}, 5, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-name-taken");
    params.audit_rules_height = 0;
    // The squatter, holding the parent too, cannot keep the squat's coin: the subname would be on two
    // outputs, one of which could be sold while the other still updates or releases it.
    const COutPoint squat_coin{squat.GetHash(), 0};
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({squat_coin, core}, 3, 0, sub, 93)}, 5, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-released-name-kept");
    const TState before{state};
    TUndo made;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({core}, 2, 0, sub, 92)}, 5, params, made, reason), reason);
    BOOST_CHECK(state.Names().at(www).parent == HashName("core"));
    BOOST_CHECK((Texts(Resolve(state, "www.core.x", dns::A)) == std::vector<std::string>{"A 198.51.100.9"}));
    // The squatter's coin carries no name any more.
    BOOST_CHECK(!state.Tokens().contains(COutPoint{squat.GetHash(), 0}));
    state.Revert(made);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(orphans_swept_are_not_kept)
{
    // Subnames left by a release before the rules sweep away when the name is registered anew; the
    // registration cannot carry one of them on, or its coin would control the subname again once the
    // new holder makes it.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    RegisterSubname sub;
    sub.label = "blog";
    const CMutableTransaction made_blog{Tx({core}, 2, 0, sub, 20)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{made_blog}, 3, params, undo, reason), reason);
    const COutPoint blog_coin{made_blog.GetHash(), 1};
    params.audit_rules_height = 100;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{made_blog.GetHash(), 0}}, 0, 0, Release{}, 21)}, 4, params, undo, reason), reason);
    BOOST_REQUIRE(state.Names().contains(SubnameHash(HashName("core"), "blog")));
    params.audit_rules_height = 0;
    // Registered again, with the orphan's coin among the inputs and carried on.
    const NameHash name{HashName("core")};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, uint256{5})}, 22)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 5, params, undo, reason));
    const COutPoint reservation{reserve.GetHash(), 0};
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({reservation, blog_coin}, 2, 0, Register{name, uint256{5}, {}}, 23)}, 6, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-released-name-kept");
    // Without it, the registration sweeps the orphan, and its coin carries no name.
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({reservation}, 1, 0, Register{name, uint256{5}, {}}, 24)}, 6, params, undo, reason), reason);
    BOOST_CHECK(!state.Names().contains(SubnameHash(HashName("core"), "blog")));
    BOOST_CHECK(!state.Tokens().contains(blog_coin));
}

BOOST_AUTO_TEST_CASE(oldest_reservation_reveals)
{
    // A copy of someone's reservation, made to register their name first once they reveal it, is younger
    // than theirs and cannot reveal it.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    const NameHash name{HashName("prize")};
    const uint256 nonce{77};
    const uint256 commitment{ReservationCommitment(name, nonce)};
    const CMutableTransaction mine{Tx({}, 0, 1, Reserve{commitment}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{mine}, 1, params, undo, reason));
    const CMutableTransaction copy{Tx({}, 0, 1, Reserve{commitment}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{copy}, 2, params, undo, reason));
    // The copier sees the name and nonce in the owner's registration, and races it.
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({COutPoint{copy.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 3)}, 3, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-reservation-not-first");
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{Tx({COutPoint{mine.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 4)}, 3, params, reason), reason);
    // The order is in the store's entries: a copy of them (as a restarted node reads them) keeps it.
    const TState read{state};
    BOOST_CHECK(!read.CheckTx(CTransaction{Tx({COutPoint{copy.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 3)}, 3, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-reservation-not-first");
    // And undo.
    TState reverted{state};
    reverted.Revert(undo);
    BOOST_CHECK(reverted == TState{});
}

BOOST_AUTO_TEST_CASE(numbers_after_register_and_release_in_one_block)
{
    // A name registered and released in the same block: undone, its number does not come back.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    const TState before{state};
    TUndo block;
    RegisterSubname sub;
    sub.label = "tmp";
    const CMutableTransaction made{Tx({core}, 2, 0, sub, 50)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{made}, 5, params, block, reason));
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{made.GetHash(), 0}}, 0, 0, Release{}, 51)}, 5, params, block, reason), reason);
    state.Revert(block);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(dns_numbers_that_are_not_whole)
{
    // Records are anyone's JSON. A number that is not a plain integer (1.5, 1e3, too large) threw in
    // the DNS server's thread, which stopped the node, for anyone who published it and asked.
    Consensus::BitNamesParams params;
    TState state;
    NameData evil;
    evil.records = R"({"mx": [[1.5, "m"], [20, "good"]], "srv": [[1e3, 0, 0, "t"], [1, 2, 3, "ok"]],
                       "tls": [[1.5, 0, 0, "ab"], [99999999999999999999, 0, 0, "ab"]]})";
    Registered(state, "evil", evil, 10, params);
    BOOST_CHECK((Texts(Resolve(state, "evil.x", dns::MX)) == std::vector<std::string>{"MX 20 good.evil.x."}));
    BOOST_CHECK_EQUAL(Resolve(state, "evil.x", dns::SRV).records.size(), 1U);
    BOOST_CHECK(Resolve(state, "evil.x", dns::TLSA).records.empty());
    BOOST_CHECK_NO_THROW(Resolve(state, "evil.x", dns::ANY));
    // The rows the RPC shows: the same records left out, no error.
    UniValue rows;
    BOOST_CHECK_NO_THROW(rows = RowsFromRecords(*evil.records));
    BOOST_CHECK_EQUAL(rows.size(), 2U);
}

BOOST_AUTO_TEST_CASE(dns_resolution)
{
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    NameData core;
    core.ipv4 = SocketV4{{203, 0, 113, 7}, 443};
    core.records = R"({"ip6": "2001:db8::7", "txt": "hello", "mx": [[10, "mail"]],
                       "map": {"www": {"alias": "@"}, "*": {"ip": "203.0.113.99"}, "shop": {"delegate": "alice"}, "": {"tls": [[3, 1, 1, "aabb"]]}}})";
    const COutPoint core_out{Registered(state, "core", core, 10, params)};
    NameData alice;
    alice.ipv4 = SocketV4{{192, 0, 2, 1}, 80};
    Registered(state, "alice", alice, 20, params);

    // The name: its address from the field, the rest from its records.
    BOOST_CHECK((Texts(Resolve(state, "core.x", dns::A)) == std::vector<std::string>{"A 203.0.113.7"}));
    BOOST_CHECK((Texts(Resolve(state, "CORE.x.", dns::AAAA)) == std::vector<std::string>{"AAAA 2001:db8::7"}));
    BOOST_CHECK((Texts(Resolve(state, "core.x", dns::MX)) == std::vector<std::string>{"MX 10 mail.core.x."}));
    BOOST_CHECK((Texts(Resolve(state, "core.x", dns::TLSA)) == std::vector<std::string>{"TLSA 3 1 1 aabb"}));
    BOOST_CHECK((Texts(Resolve(state, "www.core.x", dns::A)) == std::vector<std::string>{"CNAME core.x."}));
    BOOST_CHECK((Texts(Resolve(state, "anything.core.x", dns::A)) == std::vector<std::string>{"A 203.0.113.99"}));
    BOOST_CHECK((Texts(Resolve(state, "shop.core.x", dns::A)) == std::vector<std::string>{"A 192.0.2.1"}));
    BOOST_CHECK_EQUAL(Resolve(state, "nobody.x", dns::A).rcode, dns::NXDOMAIN);
    BOOST_CHECK_EQUAL(Resolve(state, "core.com", dns::A).rcode, dns::REFUSED);

    // A subname on the chain comes before the map.
    RegisterSubname sub;
    sub.label = "anything";
    sub.data.ipv4 = SocketV4{{198, 51, 100, 1}, 80};
    TUndo undo;
    BOOST_REQUIRE(state.ApplyTx(CTransaction{Tx({core_out}, 2, 0, sub, 40)}, 3, params, undo, reason));
    BOOST_CHECK((Texts(Resolve(state, "anything.core.x", dns::A)) == std::vector<std::string>{"A 198.51.100.1"}));
    BOOST_CHECK((Texts(Resolve(state, "other.core.x", dns::A)) == std::vector<std::string>{"A 203.0.113.99"}));

    // On the wire: a query for core.x A, and its answer.
    const std::vector<unsigned char> query{0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 4, 'c', 'o', 'r', 'e', 1, 'x', 0, 0, 1, 0, 1};
    const auto response{DnsRespond(query, [&](const std::string& q, uint16_t t) { return Resolve(state, q, t); })};
    BOOST_REQUIRE(response.size() > query.size());
    BOOST_CHECK(response[0] == 0x12 && response[1] == 0x34);
    BOOST_CHECK(response[2] & 0x80);           // a response
    BOOST_CHECK_EQUAL(response[3] & 0x0f, 0);  // NOERROR
    BOOST_CHECK_EQUAL(response[7], 1);         // one answer
    BOOST_CHECK((std::vector<unsigned char>(response.end() - 4, response.end()) == std::vector<unsigned char>{203, 0, 113, 7}));
}

BOOST_AUTO_TEST_CASE(reveal_only_when_deep)
{
    // Whoever makes a block sees the name and nonce of every registration waiting for it. Before the
    // rule, it could reserve the name under a nonce of its own and register it in that very block,
    // ahead of the one it saw: only the mempool kept a reservation from being revealed in its block.
    Consensus::BitNamesParams params;
    params.reveal_depth = 6;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("victim")};
    const uint256 thief_nonce{66};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, thief_nonce)}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 10, params, undo, reason));
    const CMutableTransaction reg{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{name, thief_nonce, {}}, 2)};
    // In the same block, and in any block until it is reveal_depth deep: refused.
    for (int height{10}; height < 16; ++height) {
        BOOST_CHECK(!state.CheckTx(CTransaction{reg}, height, params, reason));
        BOOST_CHECK_EQUAL(reason, "bad-bn-reservation-too-young");
    }
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{reg}, 16, params, reason), reason);
    // Moved to another output, a reservation keeps its age.
    const CMutableTransaction moved{Tx({COutPoint{reserve.GetHash(), 0}}, 0, 1, std::nullopt, 3)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{moved}, 12, params, undo, reason), reason);
    const CMutableTransaction reg_moved{Tx({COutPoint{moved.GetHash(), 0}}, 1, 0, Register{name, thief_nonce, {}}, 4)};
    BOOST_CHECK(!state.CheckTx(CTransaction{reg_moved}, 15, params, reason));
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{reg_moved}, 16, params, reason), reason);
    // Before the rule's height, in the same block.
    params.audit2_height = 100;
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{reg_moved}, 12, params, reason), reason);
}

BOOST_AUTO_TEST_CASE(copies_in_the_same_block_do_not_stop_a_reveal)
{
    // A reservation's commitment is public in the mempool. Whoever made a block could put a copy of
    // it ahead of it in the block: first in the chain's order, the copy stopped the original for
    // good. From audit2_height only a copy from an earlier block stops it.
    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("prize")};
    const uint256 nonce{77};
    const uint256 commitment{ReservationCommitment(name, nonce)};
    const CMutableTransaction copy{Tx({}, 0, 1, Reserve{commitment}, 1)};
    const CMutableTransaction mine{Tx({}, 0, 1, Reserve{commitment}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{copy}, 5, params, undo, reason));
    BOOST_REQUIRE(state.ApplyTx(CTransaction{mine}, 5, params, undo, reason));
    const CMutableTransaction reg{Tx({COutPoint{mine.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 3)};
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{reg}, 6, params, reason), reason);
    // Before the rule: the copy, first in the block, stops it.
    params.audit2_height = 100;
    BOOST_CHECK(!state.CheckTx(CTransaction{reg}, 6, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-reservation-not-first");
    params.audit2_height = 0;
    // A copy from an earlier block still stops a later reservation (a copy made after seeing it
    // revealed is always later).
    const CMutableTransaction late{Tx({}, 0, 1, Reserve{commitment}, 4)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{late}, 7, params, undo, reason));
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({COutPoint{late.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 5)}, 9, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-reservation-not-first");
    TState reverted{state};
    reverted.Revert(undo);
    BOOST_CHECK(reverted == TState{});
}

BOOST_AUTO_TEST_CASE(subname_bounds)
{
    // Releasing a name releases everything under it: from audit2_height a name has at most
    // MAX_SUBNAMES names under it, MAX_SUBNAME_DEPTH levels deep, so that this is bounded work.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    COutPoint top{Registered(state, "core", {}, 10, params)};
    const NameHash core{HashName("core")};
    // A chain of subnames, each under the last, as deep as allowed.
    COutPoint at{top};
    NameHash parent{core};
    uint8_t salt{20};
    std::vector<NameHash> chain;
    for (uint32_t depth{1}; depth <= MAX_SUBNAME_DEPTH; ++depth) {
        const CMutableTransaction tx{Tx({at}, 2, 0, RegisterSubname{"l", {}}, ++salt)};
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 3, params, undo, reason), reason);
        if (depth == 1) top = COutPoint{tx.GetHash(), 0};
        parent = SubnameHash(parent, "l");
        chain.push_back(parent);
        at = COutPoint{tx.GetHash(), 1};
        BOOST_CHECK_EQUAL(state.Names().at(parent).depth, depth);
    }
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({at}, 2, 0, RegisterSubname{"l", {}}, ++salt)}, 3, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-subname-too-deep");
    params.audit2_height = 100;
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{Tx({at}, 2, 0, RegisterSubname{"l", {}}, ++salt)}, 3, params, reason), reason);
    params.audit2_height = 0;
    // Each name counts the names under it.
    BOOST_CHECK_EQUAL(state.Names().at(core).descendants, MAX_SUBNAME_DEPTH);
    BOOST_CHECK_EQUAL(state.Names().at(chain[2]).descendants, MAX_SUBNAME_DEPTH - 3);

    // Many subnames right under the top-level name, up to the bound.
    uint32_t made{MAX_SUBNAME_DEPTH};
    for (uint32_t i{0}; made < MAX_SUBNAMES; ++i, ++made) {
        const CMutableTransaction tx{Tx({top}, 2, 0, RegisterSubname{strprintf("n%u", i), {}}, static_cast<uint8_t>(i))};
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 4, params, undo, reason), reason);
        top = COutPoint{tx.GetHash(), 0};
    }
    BOOST_CHECK_EQUAL(state.Names().at(core).descendants, MAX_SUBNAMES);
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({top}, 2, 0, RegisterSubname{"over", {}}, 1)}, 4, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-too-many-subnames");
    // Under a subname too: the top-level name counts the whole tree.
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({at}, 2, 0, RegisterSubname{"m", {}}, 2)}, 4, params, reason));
    // Released, a part of the tree is counted out of every name above it, and there is room again.
    const NameHash third_level{chain[1]};
    TUndo released;
    // The holder of chain[1] (at depth 2) releases it: it and the 6 under it go.
    const NameRecord before_release{state.Names().at(core)};
    COutPoint third_coin;
    for (const auto& [outpoint, token] : state.Tokens()) {
        if (token.id == third_level) third_coin = outpoint;
    }
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({third_coin}, 0, 0, Release{}, 3)}, 5, params, released, reason), reason);
    BOOST_CHECK_EQUAL(state.Names().at(core).descendants, MAX_SUBNAMES - (MAX_SUBNAME_DEPTH - 1));
    BOOST_CHECK_EQUAL(state.Names().at(chain[0]).descendants, 0U);
    BOOST_CHECK(!state.Names().contains(chain.back()));
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{Tx({top}, 2, 0, RegisterSubname{"over", {}}, 1)}, 6, params, reason), reason);
    state.Revert(released);
    BOOST_CHECK(state.Names().at(core) == before_release);
}

BOOST_AUTO_TEST_CASE(names_given_to_commands)
{
    // A dotted text is a subname or an error: never the name of the hash of that text, which anyone
    // can register (a squat of a look-alike of pay.shop). Names are lower case.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    const COutPoint shop{Registered(state, "shop", {}, 10, params)};
    const NameHash look_alike{HashName("pay.shop")};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(look_alike, uint256{3})}, 30)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 3, params, undo, reason));
    BOOST_REQUIRE(state.ApplyTx(CTransaction{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{look_alike, uint256{3}, {}}, 31)}, 4, params, undo, reason));
    const State view{state.View()};
    try {
        ParseNameArg(UniValue{"pay.shop"}, view);
        BOOST_ERROR("pay.shop is no subname yet");
    } catch (const UniValue& error) {
        BOOST_CHECK(error.find_value("message").get_str().starts_with("No such subname: pay.shop"));
    }
    // By its hash, it is the look-alike.
    BOOST_CHECK(ParseNameArg(UniValue{"0x" + look_alike.GetHex()}, view) == look_alike);
    // Once the subname is made, the text is it.
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({shop}, 2, 0, RegisterSubname{"pay", {}}, 32)}, 5, params, undo, reason), reason);
    BOOST_CHECK(ParseNameArg(UniValue{"pay.shop"}, state.View()) == SubnameHash(HashName("shop"), "pay"));
    // Through a lookup only (as the wallet has it), the same.
    const auto lookup{[&](const NameHash& h) { return state.View().GetName(h); }};
    BOOST_CHECK(ParseNameArg(UniValue{"pay.shop"}, lookup) == SubnameHash(HashName("shop"), "pay"));
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"nope.shop"}, lookup), UniValue);
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"Shop"}, lookup), UniValue);
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"pay.Shop"}, lookup), UniValue);
    BOOST_CHECK(ParseNameArg(UniValue{"shop"}, lookup) == HashName("shop"));
    BOOST_CHECK(ParseNameArg(UniValue{"unregistered"}, lookup) == HashName("unregistered"));

    // What is shown of a name: its text if of the characters names are made of; nothing otherwise.
    BOOST_CHECK(IsGoodNameText("core_2-x"));
    BOOST_CHECK(!IsGoodNameText("Core"));
    BOOST_CHECK(!IsGoodNameText("<b>x</b>"));
    BOOST_CHECK(!IsGoodNameText(""));
    BOOST_CHECK(!IsGoodNameText(std::string(256, 'a')));
    NameRecord shown;
    shown.text = "<img src=x>";
    BOOST_CHECK_EQUAL(FullText(state.View(), shown), "");
    shown.text = "fine";
    BOOST_CHECK_EQUAL(FullText(state.View(), shown), "fine");
}

BOOST_AUTO_TEST_CASE(full_text_is_whole_or_nothing)
{
    // Deeper than FullText follows (possible before audit2_height): no text, rather than the text of
    // the last 64 levels, which is the text of another name.
    Consensus::BitNamesParams params;
    params.audit2_height = 1000;
    TState state;
    std::string reason;
    TUndo undo;
    // A public name, registered with its text.
    const NameHash pub{HashName("open")};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(pub, uint256{4})}, 40)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason));
    Register reg{pub, uint256{4}, {}};
    reg.text = "open";
    const CMutableTransaction registration{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, reg, 41)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{registration}, 2, params, undo, reason), reason);
    COutPoint at{registration.GetHash(), 0};
    NameHash name{pub};
    for (int depth{1}; depth <= 66; ++depth) {
        CMutableTransaction tx{Tx({at}, 2, 0, RegisterSubname{"a", {}}, 50)};
        tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(depth)}), 9});
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 3, params, undo, reason), reason);
        name = SubnameHash(name, "a");
        at = COutPoint{tx.GetHash(), 1};
        const std::string text{FullText(state.View(), state.Names().at(name))};
        if (depth <= 64) {
            BOOST_CHECK_EQUAL(text.size(), static_cast<size_t>(depth) * 2 + 4);
        } else {
            BOOST_CHECK_EQUAL(text, "");
        }
    }
}

BOOST_AUTO_TEST_CASE(dns_answer_count)
{
    // A record whose name cannot be written is left out of the answer, and out of its count: the
    // count was written first, and the answer was malformed.
    const std::vector<unsigned char> query{0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 4, 'c', 'o', 'r', 'e', 1, 'x', 0, 0, 1, 0, 1};
    const auto response{DnsRespond(query, [](const std::string&, uint16_t) {
        dns::Answer answer;
        answer.records.push_back({std::string(70, 'a') + ".x", dns::A, {1, 2, 3, 4}, ""});
        answer.records.push_back({"core.x", dns::A, {5, 6, 7, 8}, ""});
        return answer;
    })};
    BOOST_CHECK_EQUAL(response[7], 1);
    BOOST_CHECK_EQUAL(response.size(), query.size() + 2 + 10 + 4);
    // Too large for UDP: TC set, no answers.
    const auto large{DnsRespond(query, [](const std::string&, uint16_t) {
        dns::Answer answer;
        for (int i{0}; i < 40; ++i) answer.records.push_back({"core.x", dns::A, {5, 6, 7, 8}, ""});
        return answer;
    })};
    BOOST_CHECK(large[2] & 0x02);
    BOOST_CHECK_EQUAL(large[7], 0);
    BOOST_CHECK_EQUAL(large.size(), query.size());
}

BOOST_AUTO_TEST_CASE(dns_rate_limit)
{
    DnsRateLimiter::Limits limits;
    limits.source_rate = 10;
    limits.source_burst = 20;
    limits.total_rate = 100;
    limits.total_burst = 50;
    limits.max_sources = 3;
    DnsRateLimiter limiter{limits};
    const auto t0{DnsRateLimiter::Clock::now()};
    int allowed{0};
    for (int i{0}; i < 100; ++i) allowed += limiter.Allow("a", t0);
    BOOST_CHECK_EQUAL(allowed, 20);
    // Another source has its own bucket; all of them share one.
    allowed = 0;
    for (int i{0}; i < 100; ++i) allowed += limiter.Allow("b", t0);
    BOOST_CHECK_EQUAL(allowed, 20);
    allowed = 0;
    for (int i{0}; i < 100; ++i) allowed += limiter.Allow("c", t0);
    BOOST_CHECK_EQUAL(allowed, 10);
    // A source refills at its rate.
    const auto t1{t0 + std::chrono::milliseconds{500}};
    allowed = 0;
    for (int i{0}; i < 100; ++i) allowed += limiter.Allow("a", t1);
    BOOST_CHECK_EQUAL(allowed, 5);
    // Beyond max_sources, new ones wait until one is full again (forgotten).
    BOOST_CHECK(!limiter.Allow("d", t1));
    const auto t2{t0 + std::chrono::seconds{10}};
    BOOST_CHECK(limiter.Allow("d", t2));
    BOOST_CHECK_EQUAL(limiter.Sources(), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
