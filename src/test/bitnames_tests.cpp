// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitnames/dns.h>
#include <bitnames/state.h>
#include <key.h>
#include <script/script.h>
#include <streams.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

using namespace bitnames;

namespace {
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
    State state;
    StateUndo undo;
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
    State state;
    StateUndo undo;
    std::string reason;
    const NameHash name{HashName("satoshi")};
    const uint256 nonce{9};
    const uint256 commitment{ReservationCommitment(name, nonce)};
    const CMutableTransaction first{Tx({}, 0, 1, Reserve{commitment}, 1)};
    const CMutableTransaction copy{Tx({}, 0, 1, Reserve{commitment}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{first}, 1, params, undo, reason));
    BOOST_REQUIRE(state.ApplyTx(CTransaction{copy}, 1, params, undo, reason));

    const State before{state};
    StateUndo block;
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
    State state;
    StateUndo undo;
    std::string reason;
    const NameHash name{HashName("satoshi")};
    const uint256 nonce{9};
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{ReservationCommitment(name, nonce)}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason));
    NameData data;
    data.paymail_fee = 500;
    const CMutableTransaction reg{Tx({COutPoint{reserve.GetHash(), 0}}, 1, 0, Register{name, nonce, data}, 2)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reg}, 2, params, undo, reason));

    const State before{state};
    StateUndo block;
    NameUpdates updates;
    updates.paymail_fee.kind = UpdateKind::DELETE;
    updates.commitment.kind = UpdateKind::SET;
    updates.commitment.value = uint256{3};
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{reg.GetHash(), 0}}, 1, 0, UpdateName{updates}, 3)}, 3, params, block, reason), reason);
    const NameRecord& record{state.Names().at(name)};
    BOOST_CHECK(!record.Current().paymail_fee);
    BOOST_CHECK(record.Current().commitment == uint256{3});
    BOOST_CHECK(record.AtHeight(2)->paymail_fee == 500);
    BOOST_CHECK(!record.AtHeight(1));

    // The undo survives serialization and restores the state.
    DataStream stream{};
    stream << block;
    StateUndo read;
    stream >> read;
    state.Revert(read);
    BOOST_CHECK(state == before);
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
    State state;
    StateUndo undo;
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
    const State before{state};
    StateUndo block;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{tx}, 3, params, block, reason), reason);
    BOOST_CHECK(state.Names().at(name).icann);
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({}, 0, 0, icann, 6)}, 4, params, reason));
    state.Revert(block);
    BOOST_CHECK(state == before);
}

namespace {
/** A registered name, the outpoint that holds it, in a state. */
COutPoint Registered(State& state, const std::string& text, NameData data, uint8_t salt, const Consensus::BitNamesParams& params)
{
    StateUndo undo;
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
    State state;
    StateUndo undo;
    std::string reason;
    // A reservation given up.
    const CMutableTransaction reserve{Tx({}, 0, 1, Reserve{uint256{5}}, 1)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{reserve}, 1, params, undo, reason));
    BOOST_CHECK_EQUAL(state.Reservations().size(), 1U);
    const State before{state};
    StateUndo block;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{reserve.GetHash(), 0}}, 0, 0, Release{}, 2)}, 2, params, block, reason), reason);
    BOOST_CHECK(state.Reservations().empty());
    BOOST_CHECK(state.Tokens().empty());
    state.Revert(block);
    BOOST_CHECK(state == before);

    // A name given up: free again, and taken by someone else.
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({core}, 1, 0, Release{}, 3)}, 3, params, reason));
    StateUndo released;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({core}, 0, 0, Release{}, 4)}, 3, params, released, reason), reason);
    BOOST_CHECK(!state.Names().contains(HashName("core")));
    const State after_release{state};
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
    State state;
    std::string reason;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    RegisterSubname sub;
    sub.label = "blog";
    sub.data.ipv4 = SocketV4{{198, 51, 100, 7}, 443};
    // Without the parent there is no subname.
    BOOST_CHECK(!state.CheckTx(CTransaction{Tx({}, 1, 0, sub, 30)}, 3, params, reason));
    // The parent is spent and carried on; the subname comes after it.
    const State before{state};
    StateUndo block;
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
    State state;
    std::string reason;
    StateUndo setup;
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
        State old_rules{state};
        StateUndo undo;
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

    const State before{state};
    StateUndo undo;
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
    StateUndo next;
    BOOST_CHECK_MESSAGE(state.ApplyTx(CTransaction{Tx({again}, 2, 0, sub, 51)}, 6, params, next, reason), reason);

    // Undone, all is back as it was.
    State reverted{state};
    reverted.Revert(next);
    State back{before};
    StateUndo redo;
    BOOST_REQUIRE(back.ApplyTx(CTransaction{Tx({core_now}, 0, 0, Release{}, 42)}, 5, params, redo, reason));
    back.Revert(redo);
    BOOST_CHECK(back == before);
}

BOOST_AUTO_TEST_CASE(squatted_subname_gives_way)
{
    // A name registered by the hash of a subname (no text could hash to it) is a squat: it does not
    // resolve as the subname, and gives way when the parent's holder makes the subname.
    Consensus::BitNamesParams params;
    State state;
    std::string reason;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    const NameHash www{SubnameHash(HashName("core"), "www")};
    // The squat: reserve and register the raw hash.
    StateUndo undo;
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
    const State before{state};
    StateUndo made;
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({core}, 2, 0, sub, 92)}, 5, params, made, reason), reason);
    BOOST_CHECK(state.Names().at(www).parent == HashName("core"));
    BOOST_CHECK((Texts(Resolve(state, "www.core.x", dns::A)) == std::vector<std::string>{"A 198.51.100.9"}));
    // The squatter's coin carries no name any more.
    BOOST_CHECK(!state.Tokens().contains(COutPoint{squat.GetHash(), 0}));
    state.Revert(made);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(oldest_reservation_reveals)
{
    // A copy of someone's reservation, made to register their name first once they reveal it, is younger
    // than theirs and cannot reveal it.
    Consensus::BitNamesParams params;
    State state;
    std::string reason;
    StateUndo undo;
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
    // The order survives serialization, and undo.
    DataStream stream{};
    stream << state;
    State read;
    stream >> read;
    BOOST_CHECK(read == state);
    State reverted{state};
    reverted.Revert(undo);
    BOOST_CHECK(reverted == State{});
}

BOOST_AUTO_TEST_CASE(numbers_after_register_and_release_in_one_block)
{
    // A name registered and released in the same block: undone, its number does not come back.
    Consensus::BitNamesParams params;
    State state;
    std::string reason;
    const COutPoint core{Registered(state, "core", {}, 10, params)};
    const State before{state};
    StateUndo block;
    RegisterSubname sub;
    sub.label = "tmp";
    const CMutableTransaction made{Tx({core}, 2, 0, sub, 50)};
    BOOST_REQUIRE(state.ApplyTx(CTransaction{made}, 5, params, block, reason));
    BOOST_REQUIRE_MESSAGE(state.ApplyTx(CTransaction{Tx({COutPoint{made.GetHash(), 0}}, 0, 0, Release{}, 51)}, 5, params, block, reason), reason);
    state.Revert(block);
    BOOST_CHECK(state == before);
}

BOOST_AUTO_TEST_CASE(dns_resolution)
{
    Consensus::BitNamesParams params;
    State state;
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
    StateUndo undo;
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

BOOST_AUTO_TEST_CASE(undo_formats_inside_a_record)
{
    // The undo data is followed by more in its record (the deposits of the block): each format reads
    // back, the old one written before the reservation orders included, and leaves the rest alone.
    for (const bool with_seq : {false, true}) {
        StateUndo undo;
        if (with_seq) undo.next_seq = 5;
        undo.tokens.emplace_back(COutPoint{Txid::FromUint256(uint256{7}), 1}, std::nullopt);
        DataStream current{};
        current << undo;
        // The old format: no version byte in front, and no orders (an empty list and an empty
        // optional, a byte each) at the end.
        DataStream old{std::span{current}.subspan(1, current.size() - 3)};
        for (DataStream* stream : {&current, &old}) {
            *stream << uint8_t{0xab};
            StateUndo read;
            *stream >> read;
            uint8_t after;
            *stream >> after;
            BOOST_CHECK_EQUAL(after, 0xab);
            BOOST_CHECK(read.next_seq == undo.next_seq);
            BOOST_CHECK(read.tokens == undo.tokens);
            BOOST_CHECK(read.reservation_orders.empty() && !read.next_reservation_order);
        }
    }
    StateUndo orders;
    orders.reservation_orders.emplace_back(Txid::FromUint256(uint256{9}), 3);
    orders.next_reservation_order = 4;
    DataStream stream{};
    stream << orders << uint8_t{0xab};
    StateUndo read;
    uint8_t after;
    stream >> read >> after;
    BOOST_CHECK_EQUAL(after, 0xab);
    BOOST_CHECK(read.reservation_orders == orders.reservation_orders);
    BOOST_CHECK(read.next_reservation_order == orders.next_reservation_order);
}

BOOST_AUTO_TEST_SUITE_END()
