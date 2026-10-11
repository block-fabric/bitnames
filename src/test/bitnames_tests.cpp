// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitnames/dns.h>
#include <bitnames/rpcutil.h>
#include <bitnames/state.h>
#include <key.h>
#include <netbase.h>
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
    bool CheckTx(const CTransaction& tx, int height, const Consensus::BitNamesParams& params, std::string& reason, std::optional<uint32_t> max_released = std::nullopt) const
    {
        return View().CheckTx(tx, height, params, reason, max_released);
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
    /** Write what an output carries straight into the table (0x20), as no transaction would: a state the rules never make. */
    void PutToken(const COutPoint& outpoint, const Token& token)
    {
        sidechain::Table<COutPoint, Token>{0x20}.Put(*m_store, outpoint, token);
        m_store->TakeUndo();
    }
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

/** A reservation of `name` under `nonce`, bound to the script of its output (Payee(salt)). */
CMutableTransaction Reservation(const NameHash& name, const uint256& nonce, uint8_t salt)
{
    return Tx({}, 0, 1, Reserve{ReservationCommitment(name, nonce, Payee(salt))}, salt);
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
    const CMutableTransaction reserve{Reservation(name, nonce, 1)};
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
    // that opens with the name and nonce, and then could not undo the block. (Here: two reservations
    // of the same maker, to two of its scripts.)
    Consensus::BitNamesParams params;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("satoshi")};
    const uint256 nonce{9};
    const CMutableTransaction first{Reservation(name, nonce, 1)};
    const CMutableTransaction copy{Reservation(name, nonce, 2)};
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
    const CMutableTransaction reserve{Reservation(name, nonce, 1)};
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
    const CMutableTransaction reserve{Reservation(name, nonce, 1)};
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
    const CMutableTransaction reserve{Reservation(name, nonce, 1)};
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
    // The holder of "com" makes the subname example.com: the text is still the ICANN name.
    {
        TState shadowed{state};
        TUndo u;
        const COutPoint com{Registered(shadowed, "com", {}, 40, params)};
        BOOST_REQUIRE_MESSAGE(shadowed.ApplyTx(CTransaction{Tx({com}, 2, 0, RegisterSubname{"example", {}}, 41)}, 4, params, u, reason), reason);
        BOOST_REQUIRE(shadowed.Names().contains(SubnameHash(HashName("com"), "example")));
        BOOST_CHECK(ParseNameArg(UniValue{"example.com"}, shadowed.View()) == name);
        // The subname, by its hash.
        BOOST_CHECK(ParseNameArg(UniValue{"0x" + SubnameHash(HashName("com"), "example").GetHex()}, shadowed.View()) == SubnameHash(HashName("com"), "example"));
    }
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
    const CMutableTransaction reserve{Reservation(name, nonce, salt)};
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
    const CMutableTransaction reserve{Reservation(www, uint256{9}, 90)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 3, params, undo, reason));
    NameData squat_data;
    squat_data.ipv4 = SocketV4{{6, 6, 6, 6}, 80};
    const CMutableTransaction squat{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{www, uint256{9}, squat_data}, 91)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{squat}, 4, params, undo, reason), reason);
    BOOST_REQUIRE(state.Names().contains(www));
    BOOST_CHECK_EQUAL(Resolve(state, "www.core.x", dns::A).rcode, dns::NXDOMAIN);
    RegisterSubname sub;
    sub.label = "www";
    sub.data.ipv4 = SocketV4{{198, 51, 100, 9}, 443};
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
    // Whoever makes a block sees the name and nonce of every registration waiting for it. Without the
    // rule, it could reserve the name under a nonce of its own and register it in that very block,
    // ahead of the one it saw.
    Consensus::BitNamesParams params;
    params.reveal_depth = 6;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("victim")};
    const uint256 thief_nonce{66};
    const CMutableTransaction reserve{Reservation(name, thief_nonce, 1)};
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
}

BOOST_AUTO_TEST_CASE(copies_in_the_same_block_do_not_stop_a_reveal)
{
    // A reservation's commitment is public in the mempool. Whoever makes a block (or pays a higher fee)
    // can put a copy of it in the same block: as deep as the original, a copy that revealed the name as
    // well as it would let whoever saw the registration first take the name through the copy. A
    // commitment binds the script of the reservation's output: a copy to any other script opens
    // nothing, and one to the maker's script is the maker's.
    Consensus::BitNamesParams params;
    params.reveal_depth = 2;
    TState state;
    TUndo undo;
    std::string reason;
    const NameHash name{HashName("prize")};
    const uint256 nonce{77};
    // Alice's reservation, to her script Payee(2); the copy, to the copier's Payee(1), ahead of it in the block.
    const uint256 commitment{ReservationCommitment(name, nonce, Payee(2))};
    const CMutableTransaction copy{Tx({}, 0, 1, Reserve{commitment}, 1)};
    const CMutableTransaction mine{Tx({}, 0, 1, Reserve{commitment}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{copy}, 5, params, undo, reason));
    BOOST_REQUIRE(state.ApplyTx(CTransaction{mine}, 5, params, undo, reason));
    BOOST_CHECK(state.View().GetReservationOrigin(mine.GetHash()) == Payee(2));
    BOOST_CHECK(state.View().GetReservationOrigin(copy.GetHash()) == Payee(1));
    // Alice reveals the name and nonce; the copier, seeing them, races her through the copy: it opens nothing.
    const CMutableTransaction steal{Tx({COutPoint{copy.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 3)};
    BOOST_CHECK(!state.CheckTx(CTransaction{steal}, 7, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-no-reservation");
    const CMutableTransaction reg{Tx({COutPoint{mine.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 4)};
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{reg}, 7, params, reason), reason);
    // A copy to Alice's own script, from an earlier block even, is hers: it stops nothing, and reveals the name for her.
    CMutableTransaction gift{Tx({}, 0, 1, Reserve{commitment}, 2)};
    gift.vin[0].prevout = COutPoint{Txid::FromUint256(uint256{99}), 7};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{gift}, 4, params, undo, reason));
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{reg}, 7, params, reason), reason);
    BOOST_CHECK_MESSAGE(state.CheckTx(CTransaction{Tx({COutPoint{gift.GetHash(), 0}}, 1, 0, Register{name, nonce, {}}, 10)}, 7, params, reason), reason);
    // Revealed, the reservation and where it was made are gone; undone, back.
    const TState before{state};
    TUndo block;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{reg}, 7, params, block, reason), reason);
    BOOST_CHECK(!state.View().GetReservationOrigin(mine.GetHash()));
    BOOST_CHECK(state.Names().contains(name));
    // The copier cannot take it after either.
    BOOST_CHECK(!state.CheckTx(CTransaction{steal}, 8, params, reason));
    BOOST_CHECK_EQUAL(reason, "bad-bn-name-taken");
    // Released, the copy's origin goes with it.
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{copy.GetHash(), 0}}, 0, 0, Release{}, 9)}, 8, params, block, reason), reason);
    BOOST_CHECK(!state.View().GetReservationOrigin(copy.GetHash()));
    state.Revert(block);
    BOOST_CHECK(state == before);
    TState reverted{state};
    reverted.Revert(undo);
    BOOST_CHECK(reverted == TState{});
}

BOOST_AUTO_TEST_CASE(subname_bounds)
{
    // Releasing a name releases everything under it: a name has at most
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

    // The mempool takes the release of a whole tree as large as allowed (policy: MAX_RELAYED_RELEASE),
    // and no larger: the walk stops once there are more.
    const CTransaction release_all{Tx({top}, 0, 0, Release{}, 9)};
    BOOST_CHECK_MESSAGE(state.CheckTx(release_all, 6, params, reason, MAX_RELAYED_RELEASE), reason);
    BOOST_CHECK(!state.CheckTx(release_all, 6, params, reason, MAX_RELAYED_RELEASE - 1));
    BOOST_CHECK_EQUAL(reason, "bn-release-too-large");
    // A block releases it whole.
    TUndo all;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(release_all, 7, params, all, reason), reason);
    BOOST_CHECK(!state.Names().contains(core));
    BOOST_CHECK(!state.Names().contains(chain.back()));
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
    const CMutableTransaction reserve{Reservation(look_alike, uint256{3}, 30)};
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

BOOST_AUTO_TEST_CASE(dns_delegate_to_a_subname)
{
    // A delegate with dots names a subname, found down from its last label, as commands read names:
    // never the top-level name of the hash of that text, which anyone can register.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    NameData alice;
    alice.ipv4 = SocketV4{{192, 0, 2, 1}, 80};
    const COutPoint alice_out{Registered(state, "alice", alice, 20, params)};
    RegisterSubname blog{"blog", {}};
    blog.data.ipv4 = SocketV4{{198, 51, 100, 5}, 80};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({alice_out}, 2, 0, blog, 40)}, 3, params, undo, reason), reason);
    const NameHash blog_hash{SubnameHash(HashName("alice"), "blog")};
    // The squat: a top-level name of the hash of "blog.alice".
    NameData squat;
    squat.ipv4 = SocketV4{{6, 6, 6, 6}, 80};
    const CMutableTransaction reserve{Reservation(HashName("blog.alice"), uint256{3}, 30)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 3, params, undo, reason));
    BOOST_REQUIRE(state.ApplyTx(CTransaction{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{HashName("blog.alice"), uint256{3}, squat}, 31)}, 4, params, undo, reason));
    NameData core;
    core.records = strprintf(R"({"map": {"shop": {"delegate": "blog.alice"}, "byhash": {"delegate": "0x%s"}, "up": {"delegate": "Alice"},
                                         "fake": {"delegate": "%s"}, "none": {"delegate": "nope.alice"}}})",
                             blog_hash.GetHex(), "\xd0\xb0lice");
    Registered(state, "core", core, 10, params);
    BOOST_CHECK((Texts(Resolve(state, "shop.core.x", dns::A)) == std::vector<std::string>{"A 198.51.100.5"}));
    BOOST_CHECK((Texts(Resolve(state, "byhash.core.x", dns::A)) == std::vector<std::string>{"A 198.51.100.5"}));
    BOOST_CHECK((Texts(Resolve(state, "up.core.x", dns::A)) == std::vector<std::string>{"A 192.0.2.1"}));
    // A look-alike of another script, or a subname there is not: no answer, never the squat's.
    BOOST_CHECK_EQUAL(Resolve(state, "fake.core.x", dns::A).rcode, dns::SERVFAIL);
    BOOST_CHECK_EQUAL(Resolve(state, "none.core.x", dns::A).rcode, dns::SERVFAIL);
    // The rows of setnamerecords: a delegate is a name, a subname or a hash.
    const auto row{[](const std::string& value) {
        UniValue rows(UniValue::VARR);
        UniValue r(UniValue::VOBJ);
        r.pushKV("host", "shop");
        r.pushKV("type", "DELEGATE");
        r.pushKV("value", value);
        rows.push_back(r);
        return rows;
    }};
    BOOST_CHECK_NO_THROW(RecordsFromRows(row("blog.alice")));
    BOOST_CHECK_NO_THROW(RecordsFromRows(row("alice")));
    BOOST_CHECK_NO_THROW(RecordsFromRows(row("0x" + blog_hash.GetHex())));
    BOOST_CHECK_THROW(RecordsFromRows(row("Alice")), UniValue);
    BOOST_CHECK_THROW(RecordsFromRows(row("\xd0\xb0lice")), UniValue);
    BOOST_CHECK_THROW(RecordsFromRows(row("a..b")), UniValue);
    BOOST_CHECK_THROW(RecordsFromRows(row("")), UniValue);
}

BOOST_AUTO_TEST_CASE(dns_bounded_work)
{
    // A question has at most MAX_DNS_LABELS labels, and an answer reads at most MAX_DNS_READS names.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    std::string long_name;
    for (size_t i{0}; i < MAX_DNS_LABELS; ++i) long_name += "a.";
    BOOST_CHECK_EQUAL(Resolve(state, long_name + "x", dns::A).rcode, dns::REFUSED);
    long_name.erase(0, 2);
    BOOST_CHECK_EQUAL(Resolve(state, long_name + "x", dns::A).rcode, dns::NXDOMAIN);
    // The deepest subname, through a delegation: answered.
    COutPoint at{Registered(state, "deep", {}, 10, params)};
    std::string path{"deep"};
    for (uint32_t depth{1}; depth <= MAX_SUBNAME_DEPTH; ++depth) {
        RegisterSubname sub{"l", {}};
        if (depth == MAX_SUBNAME_DEPTH) sub.data.ipv4 = SocketV4{{198, 51, 100, 8}, 0};
        const CMutableTransaction tx{Tx({at}, 2, 0, sub, static_cast<uint8_t>(40 + depth))};
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 3, params, undo, reason), reason);
        at = COutPoint{tx.GetHash(), 1};
        path = "l." + path;
    }
    NameData far;
    far.records = strprintf(R"({"delegate": "%s"})", path);
    Registered(state, "far", far, 60, params);
    BOOST_CHECK((Texts(Resolve(state, "far.x", dns::A)) == std::vector<std::string>{"A 198.51.100.8"}));
    BOOST_CHECK((Texts(Resolve(state, path + ".x", dns::A)) == std::vector<std::string>{"A 198.51.100.8"}));
    // Delegations that go round: no answer.
    NameData one, two;
    one.records = R"({"delegate": "two"})";
    two.records = R"({"delegate": "one"})";
    Registered(state, "one", one, 70, params);
    Registered(state, "two", two, 80, params);
    BOOST_CHECK_EQUAL(Resolve(state, "one.x", dns::A).rcode, dns::SERVFAIL);
}

BOOST_AUTO_TEST_CASE(dns_rate_limit_sources)
{
    // A source is an IPv4 address or an IPv6 /64.
    const auto key{[](const std::string& text) { return DnsSourceKey(*Assert(LookupHost(text, false))); }};
    BOOST_CHECK_EQUAL(key("2001:db8:1:2::1"), key("2001:db8:1:2:ffff::9"));
    BOOST_CHECK(key("2001:db8:1:2::1") != key("2001:db8:1:3::1"));
    BOOST_CHECK_EQUAL(key("203.0.113.7"), "203.0.113.7");
    BOOST_CHECK(key("203.0.113.7") != key("203.0.113.8"));
    // A flood of new sources while the table is full: one pass over it per interval, not one per question.
    DnsRateLimiter::Limits limits;
    limits.source_rate = 10;
    limits.source_burst = 20;
    limits.max_sources = 3;
    DnsRateLimiter limiter{limits};
    const auto t0{DnsRateLimiter::Clock::now()};
    // Three sources, each with its burst spent: their buckets are not full again for a while.
    for (const char* source : {"a", "b", "c"}) {
        for (int i{0}; i < 20; ++i) BOOST_CHECK(limiter.Allow(source, t0));
    }
    const auto t1{t0 + std::chrono::milliseconds{100}};
    for (int i{0}; i < 1000; ++i) BOOST_CHECK(!limiter.Allow(strprintf("n%d", i), t1));
    BOOST_CHECK_EQUAL(limiter.Sweeps(), 1U);
    BOOST_CHECK(limiter.Allow("a", t1));
    // Once the interval is over, and the buckets full again, a new source is let in.
    const auto t2{t0 + std::chrono::milliseconds{2500}};
    BOOST_CHECK(limiter.Allow("new", t2));
    BOOST_CHECK_EQUAL(limiter.Sweeps(), 2U);
    // The global rate is a local bridge's.
    BOOST_CHECK_LE(DnsRateLimiter::Limits{}.total_rate, 200);
}

BOOST_AUTO_TEST_CASE(names_given_by_text_number_or_hash)
{
    // Text is of the characters names are made of: a look-alike of another script is no name, but an
    // error (it would be the hash of a text anyone can register). A number has a form of its own,
    // #1739-0029: 1739-0029 is a text like any other, which anyone can register.
    Consensus::BitNamesParams params;
    TState state;
    Registered(state, "shop", {}, 10, params);
    const State view{state.View()};
    const uint32_t seq{state.Names().at(HashName("shop")).seq};
    BOOST_CHECK(ParseNameArg(UniValue{"#" + FormatSeq(seq)}, view) == HashName("shop"));
    BOOST_CHECK(ParseNameArg(UniValue{FormatSeq(seq)}, view) == HashName(FormatSeq(seq)));
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"#" + FormatSeq(seq + 5)}, view), UniValue);
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"#1234"}, view), UniValue);
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"\xd1\x95hop"}, view), UniValue); // a Cyrillic letter
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"sh op"}, view), UniValue);
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"a..shop"}, view), UniValue);
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"shop."}, view), UniValue);
    BOOST_CHECK(ParseNameArg(UniValue{"0x" + HashName("\xd1\x95hop").GetHex()}, view) == HashName("\xd1\x95hop"));
    // Without numbers (a lookup only), a number is an error, not a text.
    const auto lookup{[&](const NameHash& h) { return view.GetName(h); }};
    BOOST_CHECK_THROW(ParseNameArg(UniValue{"#" + FormatSeq(seq)}, lookup), UniValue);
    BOOST_CHECK(ParseNameArg(UniValue{"#" + FormatSeq(seq)}, lookup, [&](uint32_t n) { return view.NameOfSeq(n); }) == HashName("shop"));
}

BOOST_AUTO_TEST_CASE(numbers_skip_released_names)
{
    // listnames walks the numbers names have, not every number given.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    Registered(state, "a", {}, 10, params);
    const COutPoint b{Registered(state, "b", {}, 20, params)};
    Registered(state, "c", {}, 30, params);
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({b}, 0, 0, Release{}, 40)}, 3, params, undo, reason), reason);
    const auto seqs{[&](uint32_t from) {
        std::vector<uint32_t> out;
        state.View().ForEachSeq(from, [&](uint32_t seq, const NameHash&) { out.push_back(seq); return true; });
        return out;
    }};
    BOOST_CHECK((seqs(0) == std::vector<uint32_t>{0, 2}));
    BOOST_CHECK((seqs(1) == std::vector<uint32_t>{2}));
    BOOST_CHECK(seqs(3).empty());
}

namespace {
/** The bytes a marker script pushes, after OP_RETURN. */
std::vector<unsigned char> PushedBytes(const CScript& script)
{
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(script.GetOp(pc, opcode, data));
    return data;
}

/** A marker script of the tag and then `body`, as it is: well formed or not. */
CScript RawMarker(const std::vector<unsigned char>& body)
{
    std::vector<unsigned char> data{std::begin(TAG), std::end(TAG)};
    data.insert(data.end(), body.begin(), body.end());
    return CScript() << OP_RETURN << data;
}

/** A transaction with `before` outputs of no value (to carry names), the marker, then `after` payments. */
CMutableTransaction WithMarker(const CScript& marker, size_t before, size_t after = 1)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{77}), 1});
    for (size_t i{0}; i < before; ++i) tx.vout.emplace_back(0, Payee(1));
    tx.vout.emplace_back(0, marker);
    for (size_t i{0}; i < after; ++i) tx.vout.emplace_back(1000, Payee(2));
    return tx;
}
CMutableTransaction WithMarker(const Marker& marker, size_t before, size_t after = 1) { return WithMarker(MarkerScript(marker), before, after); }

Marker WithOp(Operation op, std::vector<uint32_t> names = {}, std::vector<uint32_t> reservations = {})
{
    Marker marker;
    marker.operation = std::move(op);
    marker.name_outputs = std::move(names);
    marker.reservation_outputs = std::move(reservations);
    return marker;
}

EncryptionKey GoodEncryptionKey(const CKey& key)
{
    EncryptionKey out;
    const CPubKey pubkey{key.GetPubKey()};
    std::copy(pubkey.begin(), pubkey.end(), out.begin());
    return out;
}

struct RejectCase {
    std::string what;
    CMutableTransaction tx;
    std::string reason;
};
} // namespace

BOOST_AUTO_TEST_CASE(marker_roundtrip_every_kind)
{
    // Every operation, in both forms of the marker: written, read back the same, and written in the
    // extended form exactly when it carries records or the text of a name.
    CKey key;
    key.MakeNewKey(true);
    NameData full;
    full.commitment = uint256{1};
    full.ipv4 = SocketV4{{203, 0, 113, 7}, 443};
    full.ipv6 = SocketV6{{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7}, 8333};
    full.encryption_key = GoodEncryptionKey(key);
    full.signing_key = uint256{XOnlyPubKey{key.GetPubKey()}};
    full.paymail_fee = 5000;
    NameData with_records{full};
    with_records.records = R"({"ip":"203.0.113.7"})";
    Register reg_text{HashName("core"), uint256{2}, full};
    reg_text.text = "core";
    NameUpdates all_kinds;
    all_kinds.commitment = {UpdateKind::SET, uint256{3}};
    all_kinds.ipv4 = {UpdateKind::DELETE, std::nullopt};
    all_kinds.ipv6 = {UpdateKind::SET, full.ipv6};
    all_kinds.signing_key = {UpdateKind::DELETE, std::nullopt};
    all_kinds.paymail_fee = {UpdateKind::SET, 7};
    NameUpdates records_set{all_kinds};
    records_set.records = {UpdateKind::SET, std::string{R"({"txt":"hi"})"}};
    NameUpdates records_deleted;
    records_deleted.records = {UpdateKind::DELETE, std::nullopt};
    const std::vector<std::tuple<std::string, Marker, uint8_t>> cases{
        {"no operation", WithOp(Release{}, {}, {}), MARKER_VERSION},
        {"release", WithOp(Release{}), MARKER_VERSION},
        {"reserve", WithOp(Reserve{uint256{7}}, {}, {0}), MARKER_VERSION},
        {"register", WithOp(Register{HashName("core"), uint256{2}, full}, {0}), MARKER_VERSION},
        {"register with records", WithOp(Register{HashName("core"), uint256{2}, with_records}, {0}), MARKER_VERSION_EXTENDED},
        {"register with its text", WithOp(reg_text, {0}), MARKER_VERSION_EXTENDED},
        {"update", WithOp(UpdateName{all_kinds}, {0}), MARKER_VERSION},
        {"update of the records", WithOp(UpdateName{records_set}, {0}), MARKER_VERSION_EXTENDED},
        {"records deleted", WithOp(UpdateName{records_deleted}, {0}), MARKER_VERSION_EXTENDED},
        {"icann", WithOp(IcannNames{{"example.com", "bitcoin"}, std::vector<unsigned char>(64, 9)}), MARKER_VERSION},
        {"subname", WithOp(RegisterSubname{"blog", full}, {0, 1}), MARKER_VERSION},
        {"subname with records", WithOp(RegisterSubname{"blog", with_records}, {0, 1}), MARKER_VERSION_EXTENDED},
    };
    for (auto [what, marker, version] : cases) {
        if (what == "no operation") marker.operation.reset();
        marker.memos = {{5, {1, 2, 3}}};
        const CScript script{MarkerScript(marker)};
        BOOST_CHECK_MESSAGE(PushedBytes(script).at(sizeof(TAG)) == version, what);
        const auto read{ParseMarkerScript(script)};
        BOOST_REQUIRE_MESSAGE(read, what);
        BOOST_CHECK_MESSAGE(*read == marker, what);
        // Anything changed is another marker.
        Marker other{marker};
        other.memos.clear();
        BOOST_CHECK_MESSAGE(!(*read == other), what);
    }
    // The subname's records are read back as written.
    const auto sub{ParseMarkerScript(MarkerScript(WithOp(RegisterSubname{"blog", with_records}, {0, 1})))};
    BOOST_REQUIRE(sub);
    BOOST_CHECK(std::get<RegisterSubname>(*sub->operation).data.records == with_records.records);
    BOOST_CHECK(std::get<RegisterSubname>(*sub->operation).data == with_records);
    // Values compare by every field.
    BOOST_CHECK((SocketV4{{1, 2, 3, 4}, 5} == SocketV4{{1, 2, 3, 4}, 5}));
    BOOST_CHECK(!(SocketV6{{}, 1} == SocketV6{{}, 2}));
    BOOST_CHECK(!(Token{Token::Kind::NAME, uint256{1}, {}} == Token{Token::Kind::RESERVATION, uint256{1}, {}}));
    BOOST_CHECK((Stamped<CAmount>{5, Txid{}, 3} == Stamped<CAmount>{5, Txid{}, 3}));
    BOOST_CHECK(!(ReservationRecord{uint256{1}, 2} == ReservationRecord{uint256{1}, 3}));
    // A token of an unknown kind is no token.
    DataStream bad_token{};
    bad_token << uint8_t{2} << uint256{} << uint256{};
    Token token;
    BOOST_CHECK_THROW(bad_token >> token, std::ios_base::failure);
}

BOOST_AUTO_TEST_CASE(marker_rejects)
{
    // One transaction per reason a marker is refused, each with that very reason. These depend on the
    // transaction alone (CheckMarker), and the state's check starts with them.
    CKey key;
    key.MakeNewKey(true);
    const NameHash core{HashName("core")};
    const auto reg{[&](NameData data, std::optional<std::string> text = std::nullopt) {
        Register r{core, uint256{1}, std::move(data), std::nullopt};
        r.text = std::move(text);
        return WithMarker(WithOp(r, {0}), 1);
    }};
    const auto update{[&](NameUpdates updates) { return WithMarker(WithOp(UpdateName{std::move(updates)}, {0}), 1); }};
    const auto subname{[&](std::string label, NameData data = {}) { return WithMarker(WithOp(RegisterSubname{std::move(label), std::move(data)}, {0, 1}), 2); }};
    const auto icann{[&](std::vector<std::string> names, size_t signature = 64) {
        return WithMarker(WithOp(IcannNames{std::move(names), std::vector<unsigned char>(signature, 1)}), 0);
    }};
    const auto memos{[&](std::vector<std::pair<uint32_t, std::vector<unsigned char>>> list) {
        Marker marker;
        marker.memos = std::move(list);
        return WithMarker(marker, 0, 3); // the marker is output 0
    }};
    const auto data_with{[](auto set) {
        NameData data;
        set(data);
        return data;
    }};
    const auto updates_with{[](auto set) {
        NameUpdates updates;
        set(updates);
        return updates;
    }};
    const std::vector<unsigned char> empty_lists{0, 0, 0};
    const auto body{[](std::initializer_list<std::vector<unsigned char>> parts) {
        std::vector<unsigned char> out;
        for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
        return out;
    }};

    std::vector<RejectCase> cases;
    // Markers that are no markers: there is one way to write each.
    {
        CMutableTransaction two{Tx({}, 1, 0, std::nullopt, 1)};
        two.vout.emplace_back(0, MarkerScript(Marker{}));
        cases.push_back({"two markers", two, "bad-bn-markers"});
        // The same bytes, pushed with OP_PUSHDATA1.
        std::vector<unsigned char> data{std::begin(TAG), std::end(TAG)};
        data.insert(data.end(), {MARKER_VERSION, 0, 0, 0, 0});
        CScript padded;
        padded << OP_RETURN;
        padded.insert(padded.end(), OP_PUSHDATA1);
        padded.insert(padded.end(), static_cast<unsigned char>(data.size()));
        padded.insert(padded.end(), data.begin(), data.end());
        BOOST_CHECK(IsMarkerScript(padded));
        cases.push_back({"a push that is not the shortest", WithMarker(padded, 0), "bad-bn-marker"});
        CScript longer{MarkerScript(Marker{})};
        longer << OP_TRUE;
        cases.push_back({"more after the push", WithMarker(longer, 0), "bad-bn-marker"});
        cases.push_back({"unknown version", WithMarker(RawMarker(body({{2, 0}, empty_lists})), 0), "bad-bn-marker"});
        cases.push_back({"unknown operation", WithMarker(RawMarker(body({{MARKER_VERSION, 7}, empty_lists})), 0), "bad-bn-marker"});
        cases.push_back({"extended, without an operation", WithMarker(RawMarker(body({{MARKER_VERSION_EXTENDED, 0}, empty_lists})), 0), "bad-bn-marker"});
        std::vector<unsigned char> reserve{MARKER_VERSION_EXTENDED, Reserve::KIND};
        reserve.resize(2 + 32, 5);
        cases.push_back({"extended reservation", WithMarker(RawMarker(body({reserve, {0, 1, 0, 0, 0, 0, 0}})), 1), "bad-bn-marker"});
        cases.push_back({"extended release", WithMarker(RawMarker(body({{MARKER_VERSION_EXTENDED, Release::KIND}, empty_lists})), 0), "bad-bn-marker"});
        cases.push_back({"extended icann", WithMarker(RawMarker(body({{MARKER_VERSION_EXTENDED, IcannNames::KIND, 0, 0}, empty_lists})), 0), "bad-bn-marker"});
        cases.push_back({"cut short", WithMarker(RawMarker({MARKER_VERSION, Reserve::KIND, 1, 2, 3}), 0), "bad-bn-marker"});
        cases.push_back({"bytes left over", WithMarker(RawMarker(body({{MARKER_VERSION, 0}, empty_lists, {9}})), 0), "bad-bn-marker"});
        // An update of a field of a kind that is none (3: neither delete, retain nor set).
        cases.push_back({"unknown kind of update", WithMarker(RawMarker(body({{MARKER_VERSION, UpdateName::KIND, 3, 1, 1, 1, 1, 1}, empty_lists})), 0), "bad-bn-marker"});
        BOOST_CHECK(!ParseMarkerScript(RawMarker({MARKER_VERSION})));
        BOOST_CHECK(!IsMarkerScript(CScript() << OP_RETURN << std::vector<unsigned char>{TAG[0], TAG[1], TAG[2]}));
    }
    // Where it is, and what it lists.
    {
        CMutableTransaction coinbase;
        coinbase.vin.emplace_back();
        coinbase.vout.emplace_back(0, MarkerScript(Marker{}));
        BOOST_REQUIRE(CTransaction{coinbase}.IsCoinBase());
        cases.push_back({"in a coinbase", coinbase, "bad-bn-coinbase"});
        CMutableTransaction valued{WithMarker(Marker{}, 0)};
        valued.vout[0].nValue = 1;
        cases.push_back({"a marker with value", valued, "bad-bn-marker-value"});
        Marker marker;
        marker.name_outputs = {9};
        cases.push_back({"an output there is not", WithMarker(marker, 1), "bad-bn-outputs"});
        marker.name_outputs = {1, 0};
        cases.push_back({"outputs not in order", WithMarker(marker, 2), "bad-bn-outputs"});
        marker.name_outputs = {0, 0};
        cases.push_back({"an output twice", WithMarker(marker, 1), "bad-bn-outputs"});
        marker.name_outputs = {0};
        marker.reservation_outputs = {9};
        cases.push_back({"a reservation output there is not", WithMarker(marker, 1), "bad-bn-outputs"});
        marker.reservation_outputs = {0};
        cases.push_back({"a name and a reservation on one output", WithMarker(marker, 1), "bad-bn-outputs"});
        marker.name_outputs.clear();
        marker.reservation_outputs.clear();
        for (uint32_t i{0}; i <= MAX_TOKEN_OUTPUTS; ++i) marker.name_outputs.push_back(i);
        cases.push_back({"too many outputs", WithMarker(marker, MAX_TOKEN_OUTPUTS + 1), "bad-bn-outputs"});
        marker.name_outputs = {1};
        cases.push_back({"the marker carries a name", WithMarker(marker, 1), "bad-bn-outputs"});
        marker.name_outputs = {0};
        CMutableTransaction anyone{WithMarker(marker, 1)};
        anyone.vout[0].scriptPubKey = CScript() << OP_TRUE;
        cases.push_back({"a name to a script that is not standard", anyone, "bad-bn-token-output"});
        CMutableTransaction pubkey{WithMarker(marker, 1)};
        pubkey.vout[0].scriptPubKey = GetScriptForRawPubKey(key.GetPubKey());
        cases.push_back({"a name to a bare public key", pubkey, "bad-bn-token-output"});
        CMutableTransaction worth{WithMarker(marker, 1)};
        worth.vout[0].nValue = 546;
        cases.push_back({"a name output with value", worth, "bad-bn-token-output"});
    }
    // Memos: in order, by outputs that are not the marker, neither empty nor too large.
    cases.push_back({"a memo to an output there is not", memos({{4, {1}}}), "bad-bn-memo"});
    cases.push_back({"a memo to the marker", memos({{0, {1}}}), "bad-bn-memo"});
    cases.push_back({"memos not in order", memos({{2, {1}}, {1, {1}}}), "bad-bn-memo"});
    cases.push_back({"two memos to one output", memos({{1, {1}}, {1, {2}}}), "bad-bn-memo"});
    cases.push_back({"an empty memo", memos({{1, {}}}), "bad-bn-memo"});
    cases.push_back({"a memo too large", memos({{1, std::vector<unsigned char>(MAX_MEMO_SIZE + 1, 1)}}), "bad-bn-memo"});
    // The data of a name.
    cases.push_back({"records that are no JSON", reg(data_with([](NameData& d) { d.records = "{"; })), "bad-bn-data"});
    cases.push_back({"records that are no object", reg(data_with([](NameData& d) { d.records = "[1]"; })), "bad-bn-data"});
    cases.push_back({"records too large", reg(data_with([](NameData& d) { d.records = R"({"txt":")" + std::string(MAX_RECORDS_SIZE, 'a') + R"("})"; })), "bad-bn-data"});
    cases.push_back({"a negative paymail fee", reg(data_with([](NameData& d) { d.paymail_fee = -1; })), "bad-bn-data"});
    cases.push_back({"a paymail fee beyond all money", reg(data_with([](NameData& d) { d.paymail_fee = MAX_MONEY + 1; })), "bad-bn-data"});
    cases.push_back({"an encryption key that is no key", reg(data_with([](NameData& d) { d.encryption_key = EncryptionKey{0x05, 1}; })), "bad-bn-data"});
    cases.push_back({"a signing key that is no key", reg(data_with([](NameData& d) { d.signing_key = uint256::FromHex(std::string(64, 'f')).value(); })), "bad-bn-data"});
    cases.push_back({"an update to records that are no object", update(updates_with([](NameUpdates& u) { u.records = {UpdateKind::SET, std::string{"7"}}; })), "bad-bn-data"});
    cases.push_back({"an update to a negative fee", update(updates_with([](NameUpdates& u) { u.paymail_fee = {UpdateKind::SET, -5}; })), "bad-bn-data"});
    cases.push_back({"an update to a key that is no key", update(updates_with([](NameUpdates& u) { u.encryption_key = {UpdateKind::SET, EncryptionKey{}}; })), "bad-bn-data"});
    cases.push_back({"a subname with records that are no object", subname("blog", data_with([](NameData& d) { d.records = "\"x\""; })), "bad-bn-data"});
    // The text of a name, published: the name's, neither empty nor too long.
    cases.push_back({"an empty text", reg({}, ""), "bad-bn-name-text"});
    cases.push_back({"a text too long", reg({}, std::string(MAX_NAME_TEXT_SIZE + 1, 'a')), "bad-bn-name-text"});
    cases.push_back({"the text of another name", reg({}, "other"), "bad-bn-name-text"});
    // The label of a subname: a DNS label.
    cases.push_back({"a label in upper case", subname("Blog"), "bad-bn-label"});
    cases.push_back({"an empty label", subname(""), "bad-bn-label"});
    cases.push_back({"a label that starts with a hyphen", subname("-blog"), "bad-bn-label"});
    cases.push_back({"a label too long", subname(std::string(64, 'a')), "bad-bn-label"});
    cases.push_back({"a label with a dot", subname("a.b"), "bad-bn-label"});
    // ICANN statements.
    cases.push_back({"no ICANN names", icann({}), "bad-bn-icann-names"});
    cases.push_back({"too many ICANN names", icann(std::vector<std::string>(MAX_ICANN_NAMES + 1, "a")), "bad-bn-icann-names"});
    cases.push_back({"an ICANN name that is none", icann({"Example.com"}), "bad-bn-icann-names"});
    cases.push_back({"an ICANN name twice", icann({"example.com", "example.com"}), "bad-bn-icann-names"});
    cases.push_back({"an ICANN signature of the wrong size", icann({"example.com"}, 63), "bad-bn-icann-signature"});

    Consensus::BitNamesParams params;
    TState state;
    for (const RejectCase& c : cases) {
        std::string reason;
        BOOST_CHECK_MESSAGE(!CheckMarker(CTransaction{c.tx}, reason), c.what);
        BOOST_CHECK_MESSAGE(reason == c.reason, c.what + ": " + reason + ", not " + c.reason);
        // The state refuses it for the same reason, and changes nothing.
        reason.clear();
        TUndo undo;
        BOOST_CHECK_MESSAGE(!state.ApplyTx(CTransaction{c.tx}, 1, params, undo, reason), c.what);
        BOOST_CHECK_MESSAGE(reason == c.reason, c.what + ": " + reason + ", not " + c.reason);
        BOOST_CHECK_MESSAGE(state == TState{}, c.what);
        // What the outputs carry is known only of a marker that can be read.
        if (c.reason == "bad-bn-markers" || c.reason == "bad-bn-marker") BOOST_CHECK_MESSAGE(TokenOutputs(CTransaction{c.tx}).empty(), c.what);
    }
    // A well formed one of each kind passes the same check.
    std::string reason;
    BOOST_CHECK_MESSAGE(CheckMarker(CTransaction{memos({{1, {1}}, {2, std::vector<unsigned char>(MAX_MEMO_SIZE, 1)}})}, reason), reason);
    BOOST_CHECK_MESSAGE(CheckMarker(CTransaction{reg(data_with([](NameData& d) { d.records = R"({"txt":"hi"})"; d.paymail_fee = MAX_MONEY; }), "core")}, reason), reason);
    BOOST_CHECK_MESSAGE(CheckMarker(CTransaction{subname(std::string(63, 'a'))}, reason), reason);
    BOOST_CHECK_MESSAGE(CheckMarker(CTransaction{icann(std::vector<std::string>(1, "example.com"))}, reason), reason);
    BOOST_CHECK(TokenOutputs(CTransaction{subname("blog")}) == (std::set<uint32_t>{0, 1}));
}

BOOST_AUTO_TEST_CASE(state_rejects)
{
    // One transaction per reason the state refuses a well formed one, each with that very reason.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    const COutPoint other{Registered(state, "other", {}, 20, params)};
    const CMutableTransaction reserve{Reservation(HashName("free"), uint256{3}, 30)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 3, params, undo, reason));
    const COutPoint reservation{reserve.GetHash(), 0};
    const Register free{HashName("free"), uint256{3}, {}};
    // A name output the rules never made: a token of a name that has no record.
    const COutPoint ghost{Txid::FromUint256(uint256{66}), 0};
    TState corrupt{state};
    corrupt.PutToken(ghost, Token{Token::Kind::NAME, HashName("ghost"), {}});

    const std::vector<std::tuple<std::string, const TState*, CMutableTransaction, std::string>> cases{
        {"a reservation that also makes a name output", &state, Tx({}, 1, 1, Reserve{uint256{1}}, 40), "bad-bn-unbalanced"},
        {"a reservation without its output", &state, Tx({}, 0, 0, Reserve{uint256{1}}, 41), "bad-bn-unbalanced"},
        {"a registration that keeps the reservation", &state, Tx({reservation}, 1, 1, free, 42), "bad-bn-unbalanced"},
        {"a registration without the name output", &state, Tx({reservation}, 0, 0, free, 43), "bad-bn-unbalanced"},
        {"a registration of a taken name", &state, Tx({reservation}, 1, 0, Register{HashName("core"), uint256{3}, {}}, 44), "bad-bn-name-taken"},
        {"an update without a name", &state, Tx({}, 1, 0, UpdateName{}, 45), "bad-bn-no-name"},
        {"an update that makes a name output more", &state, Tx({core}, 2, 0, UpdateName{}, 46), "bad-bn-unbalanced"},
        {"an update that drops a reservation", &state, Tx({core, reservation}, 1, 0, UpdateName{}, 47), "bad-bn-unbalanced"},
        {"a subname without a name", &state, Tx({}, 1, 0, RegisterSubname{"blog", {}}, 48), "bad-bn-no-name"},
        {"a subname without its output", &state, Tx({core}, 1, 0, RegisterSubname{"blog", {}}, 49), "bad-bn-unbalanced"},
        {"a subname that drops a reservation", &state, Tx({core, reservation}, 2, 0, RegisterSubname{"blog", {}}, 50), "bad-bn-unbalanced"},
        {"a release of nothing", &state, Tx({core}, 1, 0, Release{}, 51), "bad-bn-unbalanced"},
        {"a release that makes a name output", &state, Tx({}, 1, 0, Release{}, 52), "bad-bn-unbalanced"},
        {"a release that makes a reservation output", &state, Tx({core}, 0, 1, Release{}, 53), "bad-bn-unbalanced"},
        {"moving a name to two outputs", &state, Tx({core}, 2, 0, std::nullopt, 54), "bad-bn-unbalanced"},
        {"moving a reservation and dropping a name", &state, Tx({core, reservation}, 0, 1, std::nullopt, 55), "bad-bn-unbalanced"},
        {"an icann statement that makes an output", &state, Tx({}, 1, 0, IcannNames{{"core"}, std::vector<unsigned char>(64, 1)}, 56), "bad-bn-unbalanced"},
        {"an icann statement without an authority", &state, Tx({}, 0, 0, IcannNames{{"core"}, std::vector<unsigned char>(64, 1)}, 57), "bad-bn-icann-disabled"},
        // Defensive: a name output whose name has no record cannot come of transactions (a released
        // name's outputs are spent with it), only of a store written otherwise.
        {"an update of a name with no record", &corrupt, Tx({ghost}, 1, 0, UpdateName{}, 60), "bad-bn-name-unknown"},
        {"a release of a name with no record", &corrupt, Tx({other, ghost}, 1, 0, Release{}, 61), "bad-bn-name-unknown"},
        {"a subname of a name with no record", &corrupt, Tx({ghost}, 2, 0, RegisterSubname{"blog", {}}, 62), "bad-bn-name-unknown"},
    };
    for (const auto& [what, at, tx, expected] : cases) {
        BOOST_CHECK_MESSAGE(CheckMarker(CTransaction{tx}, reason), what + ": " + reason);
        BOOST_CHECK_MESSAGE(!at->CheckTx(CTransaction{tx}, 4, params, reason), what);
        BOOST_CHECK_MESSAGE(reason == expected, what + ": " + reason + ", not " + expected);
        // Applied, it changes nothing.
        TState copy{*at};
        TUndo none;
        BOOST_CHECK_MESSAGE(!copy.ApplyTx(CTransaction{tx}, 4, params, none, reason), what);
        BOOST_CHECK_MESSAGE(reason == expected, what);
        BOOST_CHECK_MESSAGE(copy == *at, what);
    }
    // Those spend the name that has no record last; spent first and carried on, it only moves.
    BOOST_CHECK_MESSAGE(corrupt.CheckTx(CTransaction{Tx({ghost}, 1, 0, std::nullopt, 63)}, 4, params, reason), reason);
}

BOOST_AUTO_TEST_CASE(icann_rejects)
{
    Consensus::BitNamesParams params;
    CKey authority, stranger;
    authority.MakeNewKey(true);
    stranger.MakeNewKey(true);
    params.icann_authority = uint256{XOnlyPubKey{authority.GetPubKey()}};
    TState state;
    std::string reason;
    Registered(state, "example.com", {}, 10, params);
    Registered(state, "bitcoin", {}, 20, params);
    // A statement of `names`, signed by `key` for the first input of the transaction.
    const auto statement{[&](const std::vector<std::string>& names, const CKey& key, uint8_t salt, size_t name_outputs = 0) {
        IcannNames op{names, std::vector<unsigned char>(64)};
        const CMutableTransaction unsigned_tx{Tx({}, name_outputs, 0, op, salt)};
        BOOST_REQUIRE(key.SignSchnorr(IcannMessage(names, unsigned_tx.vin[0].prevout), op.signature, nullptr, uint256{}));
        return Tx({}, name_outputs, 0, op, salt);
    }};
    const auto refused{[&](const CMutableTransaction& tx, const std::string& expected) {
        BOOST_CHECK(!state.CheckTx(CTransaction{tx}, 3, params, reason));
        BOOST_CHECK_EQUAL(reason, expected);
    }};
    refused(statement({"example.com"}, stranger, 1), "bad-bn-icann-signature");
    IcannNames zero{{"example.com"}, std::vector<unsigned char>(64, 0)};
    refused(Tx({}, 0, 0, zero, 2), "bad-bn-icann-signature");
    // Signed for another first input: not to be replayed.
    CMutableTransaction replayed{statement({"example.com"}, authority, 3)};
    replayed.vin[0].prevout.n = 8;
    refused(replayed, "bad-bn-icann-signature");
    refused(statement({"example.com", "nobody.com"}, authority, 4), "bad-bn-icann-unknown");
    refused(statement({"example.com"}, authority, 5, 1), "bad-bn-unbalanced");
    // Marked once; once only.
    const TState before{state};
    TUndo block;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{statement({"example.com", "bitcoin"}, authority, 6)}, 3, params, block, reason), reason);
    BOOST_CHECK(state.Names().at(HashName("bitcoin")).icann);
    BOOST_CHECK_EQUAL(state.Names().at(HashName("example.com")).icann_name, "example.com");
    refused(statement({"example.com"}, authority, 7), "bad-bn-icann-already");
    state.Revert(block);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(release_limits)
{
    // The mempool's bound on what one transaction releases (policy) counts the names it releases
    // itself, and a squat a subname replaces with whatever is under it.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    const COutPoint a{Registered(state, "a", {}, 10, params)};
    const COutPoint b{Registered(state, "b", {}, 20, params)};
    const CTransaction both{Tx({a, b}, 0, 0, Release{}, 30)};
    BOOST_CHECK(!state.CheckTx(both, 3, params, reason, 1));
    BOOST_CHECK_EQUAL(reason, "bn-release-too-large");
    BOOST_CHECK_MESSAGE(state.CheckTx(both, 3, params, reason, 2), reason);

    // A squat of core's subname www, with a subname of its own.
    const COutPoint core{Registered(state, "core", {}, 40, params)};
    const NameHash www{SubnameHash(HashName("core"), "www")};
    const CMutableTransaction reserve{Reservation(www, uint256{9}, 50)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 3, params, undo, reason));
    const CMutableTransaction squat{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{www, uint256{9}, {}}, 51)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{squat}, 4, params, undo, reason), reason);
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{squat.GetHash(), 0}}, 2, 0, RegisterSubname{"under", {}}, 52)}, 4, params, undo, reason), reason);
    const CTransaction made{Tx({core}, 2, 0, RegisterSubname{"www", {}}, 53)};
    BOOST_CHECK(!state.CheckTx(made, 5, params, reason, 1));
    BOOST_CHECK_EQUAL(reason, "bn-release-too-large");
    BOOST_CHECK_MESSAGE(state.CheckTx(made, 5, params, reason, 2), reason);
    // A block has no such bound.
    TState applied{state};
    BOOST_REQUIRE_MESSAGE(applied.ApplyTx(made, 5, params, undo, reason), reason);
    BOOST_CHECK(applied.Names().at(www).parent == HashName("core"));
    BOOST_CHECK(!applied.Names().contains(SubnameHash(www, "under")));
    // The index of names by parent.
    BOOST_CHECK(applied.View().ChildrenOf(HashName("core")) == std::vector<NameHash>{www});
    BOOST_CHECK(applied.View().ChildrenOf(www).empty());
    BOOST_CHECK_EQUAL(state.View().ChildrenOf(www).size(), 1U);
}

BOOST_AUTO_TEST_CASE(dns_record_types)
{
    BOOST_CHECK_EQUAL(dns::TypeName(99), "TYPE99");
    BOOST_CHECK(dns::ParseType("aaaa") == dns::AAAA);
    BOOST_CHECK(dns::ParseType("tlsa") == dns::TLSA);
    BOOST_CHECK(!dns::ParseType("SPF"));

    Consensus::BitNamesParams params;
    TState state;
    NameData core;
    core.ipv6 = SocketV6{{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9}, 443};
    const std::string long_text(300, 't');
    core.records = strprintf(R"({"ns": ["ns1", "ns.other.org."], "txt": ["", "%s", 5], "mx": "mail.example.com.",
                                 "map": {"plain": "198.51.100.3", "odd": 5, "v6": {"ip6": ["2001:db8::1", "198.51.100.1", 7]}}})", long_text);
    Registered(state, "core", core, 10, params);
    // The address of the name's data, where its records have none.
    BOOST_CHECK((Texts(Resolve(state, "core.x", dns::AAAA)) == std::vector<std::string>{"AAAA 2001:db8::9"}));
    BOOST_CHECK((Texts(Resolve(state, "core.x", dns::NS)) == std::vector<std::string>{"NS ns1.core.x.", "NS ns.other.org."}));
    BOOST_CHECK((Texts(Resolve(state, "core.x", dns::MX)) == std::vector<std::string>{"MX 10 mail.example.com."}));
    // Text: in strings of at most 255 bytes, each after its length.
    const dns::Answer txt{Resolve(state, "core.x", dns::TXT)};
    BOOST_REQUIRE_EQUAL(txt.records.size(), 2U);
    BOOST_CHECK(txt.records[0].rdata == std::vector<unsigned char>{0});
    BOOST_CHECK_EQUAL(txt.records[0].text, "\"\"");
    BOOST_REQUIRE_EQUAL(txt.records[1].rdata.size(), 1 + 255 + 1 + 45U);
    BOOST_CHECK_EQUAL(txt.records[1].rdata[0], 255);
    BOOST_CHECK_EQUAL(txt.records[1].rdata[256], 45);
    BOOST_CHECK_EQUAL(txt.records[1].text, "\"" + long_text + "\"");
    // A host of the map given as an address alone, one given as something else, and only what is of the type asked for.
    BOOST_CHECK((Texts(Resolve(state, "plain.core.x", dns::A)) == std::vector<std::string>{"A 198.51.100.3"}));
    const dns::Answer odd{Resolve(state, "odd.core.x", dns::ANY)};
    BOOST_CHECK_EQUAL(odd.rcode, dns::NOERROR);
    BOOST_CHECK(odd.records.empty());
    BOOST_CHECK((Texts(Resolve(state, "v6.core.x", dns::AAAA)) == std::vector<std::string>{"AAAA 2001:db8::1"}));
    BOOST_CHECK_EQUAL(Resolve(state, "x", dns::A).rcode, dns::NOERROR);
    // Every type at once.
    BOOST_CHECK_EQUAL(Resolve(state, "core.x", dns::ANY).records.size(), 1U + 2U + 2U + 1U);

    // On the wire, an answer for a name other than the question's is written out in full.
    const std::vector<unsigned char> query{0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0, 4, 'c', 'o', 'r', 'e', 1, 'x', 0, 0, 1, 0, 1};
    const auto response{DnsRespond(query, [](const std::string&, uint16_t) {
        dns::Answer answer;
        answer.records.push_back({"ab.x", dns::A, {1, 2, 3, 4}, ""});
        return answer;
    })};
    BOOST_CHECK_EQUAL(response[7], 1);
    BOOST_CHECK((std::vector<unsigned char>(response.begin() + query.size(), response.begin() + query.size() + 6) == std::vector<unsigned char>{2, 'a', 'b', 1, 'x', 0}));
    // What is no question: too short, a response, another opcode, two questions, a name cut short, another class.
    BOOST_CHECK(DnsRespond({1, 2, 3}, {}).empty());
    auto response_flag{query};
    response_flag[2] |= 0x80;
    BOOST_CHECK(DnsRespond(response_flag, {}).empty());
    auto notify{query};
    notify[2] = 0x20; // opcode 4
    BOOST_CHECK_EQUAL(DnsRespond(notify, {})[3] & 0x0f, dns::NOTIMP);
    auto two{query};
    two[5] = 2;
    BOOST_CHECK_EQUAL(DnsRespond(two, {})[3] & 0x0f, dns::FORMERR);
    const std::vector<unsigned char> cut(query.begin(), query.begin() + 16);
    BOOST_CHECK_EQUAL(DnsRespond(cut, {})[3] & 0x0f, dns::FORMERR);
    auto chaos{query};
    chaos.back() = 3;
    BOOST_CHECK_EQUAL(DnsRespond(chaos, {})[3] & 0x0f, dns::REFUSED);
}

BOOST_AUTO_TEST_CASE(dns_reads_bounded_on_the_way_down)
{
    // The reads of an answer are counted on the way down the labels too: a host of the map that
    // delegates, at each level, to a deep subname costs a dozen reads a label, and the answer stops
    // at MAX_DNS_READS with SERVFAIL, before the bound on delegations is reached.
    Consensus::BitNamesParams params;
    TState state;
    std::string reason;
    TUndo undo;
    COutPoint at{Registered(state, "deep", {}, 10, params)};
    std::string path{"deep"};
    for (uint32_t depth{1}; depth <= MAX_SUBNAME_DEPTH; ++depth) {
        RegisterSubname sub{"l", {}};
        const CMutableTransaction tx{Tx({at}, 2, 0, sub, static_cast<uint8_t>(40 + depth))};
        BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 3, params, undo, reason), reason);
        at = COutPoint{tx.GetHash(), 1};
        path = "l." + path;
    }
    // The deepest subname: any host under it delegates back to it.
    NameUpdates updates;
    updates.records = {UpdateKind::SET, strprintf(R"({"map": {"*": {"delegate": "%s"}}})", path)};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({at}, 1, 0, UpdateName{updates}, 60)}, 4, params, undo, reason), reason);
    NameData start;
    start.records = strprintf(R"({"delegate": "%s"})", path);
    Registered(state, "start", start, 70, params);
    // 1 read for the name, 11 for the delegation (a read per label of the path, one for an ICANN name
    // of that text, one for the deepest subname), then 12 a label: 48 after three labels.
    BOOST_CHECK_EQUAL(Resolve(state, "q.q.q.start.x", dns::A).rcode, dns::NOERROR);
    BOOST_CHECK_EQUAL(Resolve(state, "q.q.q.q.start.x", dns::A).rcode, dns::SERVFAIL);
}

BOOST_AUTO_TEST_CASE(record_rows)
{
    const auto rows{[](std::vector<std::tuple<std::string, std::string, std::string>> list) {
        UniValue out(UniValue::VARR);
        for (const auto& [host, type, value] : list) {
            UniValue row(UniValue::VOBJ);
            row.pushKV("host", host);
            row.pushKV("type", type);
            row.pushKV("value", value);
            out.push_back(row);
        }
        return out;
    }};
    const auto message{[](const auto& f) {
        try {
            f();
        } catch (const UniValue& e) {
            return e.find_value("message").get_str();
        }
        return std::string{};
    }};
    // TLSA, as written in zone files; NS; and the rows read back.
    const std::string records{RecordsFromRows(rows({{"_443._tcp", "tlsa", "3 1 1 aabb"}, {"@", "NS", "ns1"}, {"@", "NS", "ns2"}}))};
    BOOST_CHECK_EQUAL(records, R"({"ns":["ns1","ns2"],"map":{"_tcp":{"map":{"_443":{"tls":[[3,1,1,"aabb"]]}}}}})");
    const UniValue back{RowsFromRecords(records)};
    BOOST_REQUIRE_EQUAL(back.size(), 3U);
    BOOST_CHECK_EQUAL(back[0]["type"].get_str(), "NS");
    BOOST_CHECK_EQUAL(back[2]["host"].get_str(), "_443._tcp");
    BOOST_CHECK_EQUAL(back[2]["value"].get_str(), "3 1 1 aabb");
    BOOST_CHECK_EQUAL(message([&] { RecordsFromRows(rows({{"@", "TLSA", "3 1 aabb"}})); }), "TLSA: usage selector type data, such as 3 1 1 ab12...");
    BOOST_CHECK_EQUAL(message([&] { RecordsFromRows(rows({{"@", "SPF", "v=spf1"}})); }), "Record types: A, AAAA, CNAME, TXT, MX, SRV, NS, TLSA, DELEGATE");
    BOOST_CHECK_EQUAL(message([&] { RecordsFromRows(UniValue{"rows"}); }), "records: a list of rows");
    BOOST_CHECK_EQUAL(message([&] { RecordsFromRows(rows({{"@", "CNAME", "a"}, {"@", "CNAME", "b"}})); }), "One CNAME per host");
    BOOST_CHECK(message([&] { RecordsFromRows(rows({{"a b", "A", "192.0.2.1"}})); }).starts_with("host: labels"));
    // A host of the map that is an address alone is an A row.
    const UniValue plain{RowsFromRecords(R"({"map": {"www": "192.0.2.5"}})")};
    BOOST_REQUIRE_EQUAL(plain.size(), 1U);
    // Of that host, not of the name: the rows are what the wallet writes back (the page saves what it shows).
    BOOST_CHECK_EQUAL(plain[0].write(), R"({"host":"www","type":"A","value":"192.0.2.5"})");
    BOOST_CHECK_EQUAL(RecordsFromRows(plain), R"({"map":{"www":{"ip":"192.0.2.5"}}})");
    BOOST_CHECK(RowsFromRecords("not json").empty());

    // Records the rows give back, which an editor of rows may save, and records they change, which it may not.
    for (const char* kept : {"", "{}", R"({"ip": "192.0.2.1"})", R"({"ip": ["192.0.2.1", "192.0.2.2"], "txt": [], "map": {}})",
                             R"({"map": {"www": "192.0.2.5", "_tcp": {"map": {"_sip": {"srv": [[10, 5, 5060, "sip."]]}}}}, "alias": "x."})",
                             R"({"mx": [[10, "mail."]], "tls": [[3, 1, 1, "aabb"]], "ns": "ns1", "ip6": "2001:db8::7", "delegate": "core"})"}) {
        BOOST_CHECK_MESSAGE(RowsKeepRecords(kept), kept);
    }
    for (const char* changed : {"not json", "[1]",
                                R"({"map": {"a.b": {"ip": "192.0.2.1"}}})",       // nested by a save: DNS would then reach it
                                R"({"map": {"WWW": {"ip": "192.0.2.1"}}})",       // lower-cased by a save
                                R"({"txt": "a", "map": {"": {"txt": "b"}}})",     // merged into @ by a save
                                R"({"ip": "192.0.2.1", "colour": "red"})",        // a key rows have no type for
                                R"({"ip": "2001:db8::7"})",                        // an IPv6 address as an A row: no save at all
                                R"({"srv": [[10, 5, 5060]]})",                     // a record rows leave out
                                R"({"mx": "mail."})",                              // a priority a save adds
                                R"({"map": {"www": {}}})"}) {
        BOOST_CHECK_MESSAGE(!RowsKeepRecords(changed), changed);
    }

    // An update deletes any field given as null, and checks every value.
    UniValue all_null(UniValue::VOBJ);
    for (const char* field : {"commitment", "ipv4", "ipv6", "encryptionkey", "signingkey", "paymailfee", "records"}) all_null.pushKV(field, UniValue{});
    const NameUpdates deleted{ParseUpdates(all_null)};
    bool every{true};
    ForEachField(deleted, [&](Field, const auto& update) { every &= update.kind == UpdateKind::DELETE; });
    BOOST_CHECK(every);
    const auto field{[](const std::string& key, const UniValue& value) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV(key, value);
        return obj;
    }};
    BOOST_CHECK_EQUAL(message([&] { ParseData(UniValue{"x"}); }), "The data of a name is an object");
    BOOST_CHECK_EQUAL(message([&] { ParseData(field("colour", "red")); }), "Unknown field of name data: colour");
    BOOST_CHECK_EQUAL(message([&] { ParseData(field("commitment", "11")); }), "commitment: 32 bytes in hex");
    BOOST_CHECK(message([&] { ParseData(field("ipv4", "2001:db8::1")); }).starts_with("ipv4: an IPv4 address"));
    BOOST_CHECK(message([&] { ParseData(field("ipv6", "192.0.2.1")); }).starts_with("ipv6: an IPv6 address"));
    BOOST_CHECK_EQUAL(message([&] { ParseData(field("encryptionkey", std::string(66, '0'))); }), "encryptionkey: a compressed public key, 33 bytes in hex");
    BOOST_CHECK_EQUAL(message([&] { ParseData(field("signingkey", std::string(64, 'f'))); }), "signingkey: an x-only public key, 32 bytes in hex");
    BOOST_CHECK_EQUAL(message([&] { ParseData(field("records", "[1]")); }), "records: a JSON object");
    BOOST_CHECK_EQUAL(message([&] { ParseData(field("records", R"({"txt":")" + std::string(MAX_RECORDS_SIZE, 'a') + R"("})")); }),
                      strprintf("records: at most %u bytes of JSON", MAX_RECORDS_SIZE));
    // Records given as an object, not as text, are the same.
    UniValue object(UniValue::VOBJ);
    object.pushKV("ip", "192.0.2.1");
    BOOST_CHECK(ParseData(field("records", object)).records == std::string{R"({"ip":"192.0.2.1"})"});
    // Shown back: addresses with their ports, or alone.
    NameData data;
    data.ipv4 = SocketV4{{192, 0, 2, 1}, 0};
    data.ipv6 = SocketV6{{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, 0};
    data.records = "{}";
    const UniValue shown{DataToJSON(data)};
    BOOST_CHECK_EQUAL(shown["ipv4"].get_str(), "192.0.2.1");
    BOOST_CHECK_EQUAL(shown["ipv6"].get_str(), "2001:db8::1");
    BOOST_CHECK(shown["records"].isObject());
}

BOOST_AUTO_TEST_SUITE_END()
