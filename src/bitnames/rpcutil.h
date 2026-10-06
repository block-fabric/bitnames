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

/**
 * A name given by its text, the hex of its hash ("0x" + 64 digits), or its number ("1739-0029").
 * Text with dots is a subname ("blog.core") if `exists` finds one along that path, else a name of
 * that text.
 */
NameHash ParseNameArg(const UniValue& value, const State* state = nullptr, const std::function<bool(const NameHash&)>& exists = {});
/** The full text of a name, "blog.core" for a subname, if it and its parents are public; else empty. */
std::string FullText(const State& state, const NameRecord& record);

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
