// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of BitNames (local.cpp): the store of the names being registered, in a file of
// the data directory; the checks of DNS records given as rows, as the wallet makes them; whether rows can
// stand for the records of a name; and the methods the pages may call, with those that need the keys.

#include <bitnames/rpcutil.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <webui/bridge.h>
#include <webui/local.h>
#include <webui/rpcpolicy.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <string>

BOOST_FIXTURE_TEST_SUITE(webui_local_tests, BasicTestingSetup)

namespace {
UniValue Rows(const std::string& json)
{
    UniValue rows;
    BOOST_REQUIRE(rows.read(json));
    return rows;
}
} // namespace

BOOST_AUTO_TEST_CASE(store_of_bitnames)
{
    const fs::path path{m_args.GetDataDirNet() / webui::local::STORE_FILE};
    BOOST_CHECK(webui::local::ReadStore(path).isNull());

    UniValue value{UniValue::VOBJ};
    UniValue pending{UniValue::VARR};
    for (int i{0}; i < 200; ++i) {
        UniValue rec{UniValue::VOBJ};
        rec.pushKV("wallet", "w");
        rec.pushKV("name", strprintf("name%d", i));
        rec.pushKV("step", "maturing");
        pending.push_back(rec);
    }
    value.pushKV("pending", pending);
    webui::local::WriteStore(path, value);
    BOOST_CHECK_EQUAL(webui::local::ReadStore(path).write(), value.write());
    BOOST_CHECK(!fs::exists(fs::PathFromString(fs::PathToString(path) + ".new")));

    // What is not an object, or too large, is refused, and the file stays as it was.
    BOOST_CHECK_THROW(webui::local::WriteStore(path, UniValue{"text"}), webui::HostError);
    UniValue huge{UniValue::VOBJ};
    huge.pushKV("x", std::string(webui::local::MAX_STORE_BYTES, 'x'));
    BOOST_CHECK_THROW(webui::local::WriteStore(path, huge), webui::HostError);
    BOOST_CHECK_EQUAL(webui::local::ReadStore(path).write(), value.write());

    // A file that cannot be read is kept aside, not lost: the names a reservation hides.
    {
        std::ofstream broken{path.std_path(), std::ios::trunc};
        broken << "{\"pending\": [";
    }
    BOOST_CHECK(webui::local::ReadStore(path).isNull());
    BOOST_CHECK(fs::exists(fs::PathFromString(fs::PathToString(path) + ".unreadable")));
}

BOOST_AUTO_TEST_CASE(record_rows_checked_as_the_wallet_does)
{
    using webui::local::CheckRecordRows;
    const UniValue good{CheckRecordRows(Rows(R"([{"type": "A", "value": "203.0.113.7"}, {"host": "www", "type": "CNAME", "value": "@"}, {"type": "MX", "value": "mail", "priority": 10}])"))};
    BOOST_CHECK(good.find_value("ok").get_bool());
    BOOST_CHECK_GT(good.find_value("bytes").getInt<int>(), 10);

    // The words of the wallet's own refusals.
    const UniValue bad_a{CheckRecordRows(Rows(R"([{"type": "A", "value": "2001:db8::7"}])"))};
    BOOST_CHECK(!bad_a.find_value("ok").get_bool());
    BOOST_CHECK(bad_a.find_value("error").get_str().find("IPv4") != std::string::npos);
    BOOST_CHECK(!CheckRecordRows(Rows(R"([{"type": "AAAA", "value": "203.0.113.7"}])")).find_value("ok").get_bool());
    BOOST_CHECK(!CheckRecordRows(Rows(R"([{"type": "TLSA", "value": "3 1"}])")).find_value("ok").get_bool());
    BOOST_CHECK(!CheckRecordRows(Rows(R"([{"type": "DELEGATE", "value": "Not A Name"}])")).find_value("ok").get_bool());
    BOOST_CHECK(!CheckRecordRows(Rows(R"([{"type": "HINFO", "value": "x"}])")).find_value("ok").get_bool());
    BOOST_CHECK(!CheckRecordRows(Rows(R"([{"type": "CNAME", "value": "a."}, {"type": "CNAME", "value": "b."}])")).find_value("ok").get_bool());
    BOOST_CHECK(!CheckRecordRows(Rows(R"([{"value": "x"}])")).find_value("ok").get_bool());
    // More than a name holds.
    const UniValue big{CheckRecordRows(Rows(strprintf(R"([{"type": "TXT", "value": "%s"}])", std::string(1100, 'x'))))};
    BOOST_CHECK(!big.find_value("ok").get_bool());
    BOOST_CHECK(big.find_value("error").get_str().find("1024") != std::string::npos);
    // Not rows at all, or too many: a bad request.
    BOOST_CHECK_THROW(CheckRecordRows(UniValue{"rows"}), webui::HostError);
    UniValue many{UniValue::VARR};
    for (int i{0}; i < 300; ++i) many.push_back(Rows(R"({"type": "TXT", "value": "x"})"));
    BOOST_CHECK_THROW(CheckRecordRows(many), webui::HostError);
}

BOOST_AUTO_TEST_CASE(records_rows_can_stand_for)
{
    using webui::local::RecordsEditable;
    BOOST_CHECK(RecordsEditable(UniValue{}));
    // What the wallet writes of rows, rows give back.
    BOOST_CHECK(RecordsEditable(Rows(bitnames::RecordsFromRows(Rows(R"([{"type": "A", "value": "203.0.113.7"}, {"host": "www", "type": "CNAME", "value": "@"}, {"type": "MX", "value": "mail", "priority": 5}])")))));
    // A map key with upper-case letters: rows have no host for it, saving rows would change it.
    BOOST_CHECK(!RecordsEditable(Rows(R"({"map": {"WWW": {"ip": ["203.0.113.7"]}}})")));
    BOOST_CHECK(!RecordsEditable(UniValue{"text"}));
}

BOOST_AUTO_TEST_CASE(methods_of_the_pages)
{
    const auto methods{webui::local::RpcMethods()};
    const auto keyed{webui::local::KeyedRpcMethods()};
    const auto confirmed{webui::local::ConfirmedRpcMethods()};
    // Every keyed method is one the pages may call, and every method that opens a message needs the keys.
    for (const std::string& m : keyed) BOOST_CHECK_MESSAGE(methods.contains(m), m);
    for (const char* m : {"listpaymail", "decryptnamemessage"}) {
        BOOST_CHECK_MESSAGE(keyed.contains(m), m);
        BOOST_CHECK_MESSAGE(webui::KeyedWalletRpc(m), m);
    }
    // What pays or signs: never the page's to call, only through the host, which confirms it on the desktop (audit 7 #1).
    for (const char* m : {"reservename", "registername", "releasereservation", "updatename", "setnamerecords", "createsubname", "transfername", "releasename", "sendpaymail", "signasname"}) {
        BOOST_CHECK_MESSAGE(confirmed.contains(m) && confirmed.at(m).kind == webui::confirm::Method::Kind::SPEND, m);
        BOOST_CHECK_MESSAGE(!methods.contains(m) && !keyed.contains(m) && !webui::KeyedWalletRpc(m), m);
        BOOST_CHECK_MESSAGE(webui::confirm::Methods().contains(m), m);
    }
    // What only reads needs no keys.
    for (const char* m : {"getname", "listnames", "listmynames", "resolvename", "getbitnamesinfo", "gettransaction", "encryptnamemessage"}) {
        BOOST_CHECK_MESSAGE(methods.contains(m), m);
        BOOST_CHECK_MESSAGE(!keyed.contains(m), m);
    }
}

BOOST_AUTO_TEST_SUITE_END()
