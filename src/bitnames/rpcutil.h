// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITNAMES_RPCUTIL_H
#define BITCOIN_BITNAMES_RPCUTIL_H

#include <bitnames/state.h>
#include <rpc/util.h>

#include <univalue.h>

#include <functional>
#include <vector>

/** What the commands of the node and of the wallet share: how name data is written in JSON. */
namespace bitnames {

/** The fields of the data of a name, as arguments. */
std::vector<RPCArg> DataArgs();
/** The fields of the data of a name, as a result. */
std::vector<RPCResult> DataResults();

/** Data from JSON: every field optional. */
NameData ParseData(const UniValue& value);
/** Updates from JSON: a field left out is kept, null deletes it, a value sets it. */
NameUpdates ParseUpdates(const UniValue& value);
UniValue DataToJSON(const NameData& data);

/** How a name is looked up by its hash. */
using NameLookup = std::function<std::optional<NameRecord>(const NameHash&)>;

/**
 * A name given by its text, the hex of its hash ("0x" + 64 digits), or its number ("1739-0029",
 * where `by_seq` is given). Text with dots is a subname ("blog.core"), found from its last label
 * down, each step a subname of the one above, or an ICANN name of that text; anything else with
 * dots is an error, never the name of the hash of that text (anyone can register one). Text with
 * upper-case letters is an error: DNS looks names up in lower case.
 */
NameHash ParseNameArg(const UniValue& value, const NameLookup& lookup, const std::function<std::optional<NameHash>(uint32_t)>& by_seq = {});
NameHash ParseNameArg(const UniValue& value, const State& state);
/**
 * The full text of a name, "blog.core" for a subname, if it and its parents are public and their
 * text is of the characters names are made of (IsGoodNameText); else empty, never a part of it.
 */
std::string FullText(const State& state, const NameRecord& record);
/** Whether text is what the wallet makes names of, and shows: 1 to 255 lower-case letters, digits, '-' and '_'. */
bool IsGoodNameText(const std::string& text);

/**
 * DNS records as rows: {"host": "@" or a label under the name ("www", "a.b"), "type": "A", "value": ...,
 * "priority": for MX and SRV, "weight" and "port": for SRV}. To and from the JSON the chain keeps.
 */
std::string RecordsFromRows(const UniValue& rows);
UniValue RowsFromRecords(const std::string& records);
/**
 * Refuse records that define a host a subname of the name holds: `label`, or anything under it. The
 * subname answers for them, so such records would never be seen.
 */
void CheckRecordsBesideSubnames(const std::string& records, const std::vector<std::string>& subnames);
/** The hosts right under the name that its records define (the keys of their map). */
std::vector<std::string> RecordHosts(const std::string& records);

std::string FormatSocket(const SocketV4& socket);
std::string FormatSocket(const SocketV6& socket);
std::optional<SocketV4> ParseSocketV4(const std::string& text);
std::optional<SocketV6> ParseSocketV6(const std::string& text);

} // namespace bitnames

#endif // BITCOIN_BITNAMES_RPCUTIL_H
