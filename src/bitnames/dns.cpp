// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitnames/dns.h>

#include <bitnames/rpcutil.h>
#include <common/args.h>
#include <compat/compat.h>
#include <logging.h>
#include <netaddress.h>
#include <netbase.h>
#include <sync.h>
#include <util/strencodings.h>
#include <util/threadnames.h>
#include <validation.h>

#include <univalue.h>

#include <atomic>
#include <memory>
#include <thread>

namespace bitnames {

namespace dns {
std::string TypeName(uint16_t type)
{
    switch (type) {
    case A: return "A";
    case NS: return "NS";
    case CNAME: return "CNAME";
    case SOA: return "SOA";
    case MX: return "MX";
    case TXT: return "TXT";
    case AAAA: return "AAAA";
    case SRV: return "SRV";
    case TLSA: return "TLSA";
    case ANY: return "ANY";
    }
    return strprintf("TYPE%u", type);
}

std::optional<uint16_t> ParseType(const std::string& name)
{
    for (const uint16_t t : {A, NS, CNAME, SOA, MX, TXT, AAAA, SRV, TLSA, ANY}) {
        if (ToUpper(name) == TypeName(t)) return t;
    }
    return std::nullopt;
}
} // namespace dns

namespace {
using namespace dns;

/** A domain name in the wire format: labels, each after its length, then a zero. Nullopt if it is not a valid name. */
std::optional<std::vector<unsigned char>> EncodeName(const std::string& name)
{
    std::vector<unsigned char> out;
    if (!name.empty()) {
        size_t start{0};
        while (start <= name.size()) {
            const size_t end{std::min(name.find('.', start), name.size())};
            const size_t length{end - start};
            if (length == 0 || length > 63) return std::nullopt;
            out.push_back(static_cast<unsigned char>(length));
            out.insert(out.end(), name.begin() + start, name.begin() + end);
            start = end + 1;
            if (end == name.size()) break;
        }
    }
    out.push_back(0);
    if (out.size() > 255) return std::nullopt;
    return out;
}

void PutU16(std::vector<unsigned char>& out, uint16_t v)
{
    out.push_back(v >> 8);
    out.push_back(v & 0xff);
}

void PutU32(std::vector<unsigned char>& out, uint32_t v)
{
    PutU16(out, v >> 16);
    PutU16(out, v & 0xffff);
}

/** A list from a field that may be one value or an array of them. */
std::vector<UniValue> Many(const UniValue& node, const std::string& key)
{
    if (!node.isObject() || !node.exists(key)) return {};
    const UniValue& v{node[key]};
    if (v.isArray()) return v.getValues();
    return {v};
}

class Collector
{
public:
    Collector(std::string owner, std::string base) : m_owner{std::move(owner)}, m_base{std::move(base)} {}

    /** A host name of the records: absolute if it ends with a dot, else under the name the records belong to. */
    std::string Host(std::string host) const
    {
        if (host.empty() || host == "@") return m_base;
        if (host.back() == '.') {
            host.pop_back();
            return ToLower(host);
        }
        return ToLower(host) + "." + m_base;
    }

    void Add(uint16_t type, std::vector<unsigned char> rdata, std::string text)
    {
        m_answer.records.push_back({m_owner, type, std::move(rdata), std::move(text)});
    }

    void AddName(uint16_t type, const std::string& host, std::vector<unsigned char> prefix = {}, std::string text_prefix = {})
    {
        const std::string absolute{Host(host)};
        const auto encoded{EncodeName(absolute)};
        if (!encoded) return;
        prefix.insert(prefix.end(), encoded->begin(), encoded->end());
        Add(type, std::move(prefix), text_prefix + absolute + ".");
    }

    Answer m_answer;

private:
    std::string m_owner;
    std::string m_base;
};

/** The records of a node of the records of a name (the name itself, or a subdomain), of a type. */
void Collect(const UniValue& node, uint16_t qtype, Collector& out)
{
    const auto want{[&](uint16_t type) { return qtype == type || qtype == ANY; }};
    // An alias stands for all records of the node.
    if (node.exists("alias") && node["alias"].isStr()) {
        out.AddName(CNAME, node["alias"].get_str());
        return;
    }
    if (want(A)) {
        for (const UniValue& ip : Many(node, "ip")) {
            if (!ip.isStr()) continue;
            const auto addr{LookupHost(ip.get_str(), /*fAllowLookup=*/false)};
            in_addr in4;
            if (!addr || !addr->IsIPv4() || !addr->GetInAddr(&in4)) continue;
            std::vector<unsigned char> rdata(4);
            std::memcpy(rdata.data(), &in4, 4);
            out.Add(A, std::move(rdata), addr->ToStringAddr());
        }
    }
    if (want(AAAA)) {
        for (const UniValue& ip : Many(node, "ip6")) {
            if (!ip.isStr()) continue;
            const auto addr{LookupHost(ip.get_str(), /*fAllowLookup=*/false)};
            in6_addr in6;
            if (!addr || !addr->IsIPv6() || !addr->GetIn6Addr(&in6)) continue;
            std::vector<unsigned char> rdata(16);
            std::memcpy(rdata.data(), &in6, 16);
            out.Add(AAAA, std::move(rdata), addr->ToStringAddr());
        }
    }
    if (want(NS)) {
        for (const UniValue& host : Many(node, "ns")) {
            if (host.isStr()) out.AddName(NS, host.get_str());
        }
    }
    if (want(TXT)) {
        for (const UniValue& txt : Many(node, "txt")) {
            if (!txt.isStr()) continue;
            const std::string& text{txt.get_str()};
            std::vector<unsigned char> rdata;
            for (size_t i{0}; i < text.size() || i == 0; i += 255) {
                const size_t length{std::min<size_t>(255, text.size() - i)};
                rdata.push_back(length);
                rdata.insert(rdata.end(), text.begin() + i, text.begin() + i + length);
                if (text.empty()) break;
            }
            out.Add(TXT, std::move(rdata), "\"" + text + "\"");
        }
    }
    if (want(MX)) {
        // [preference, host], in either order, or the host alone.
        for (const UniValue& mx : Many(node, "mx")) {
            std::optional<int64_t> preference;
            std::optional<std::string> host;
            for (const UniValue& part : mx.isArray() ? mx.getValues() : std::vector<UniValue>{mx}) {
                if (part.isNum()) preference = part.getInt<int64_t>();
                if (part.isStr()) host = part.get_str();
            }
            if (!host) continue;
            const uint16_t pref{static_cast<uint16_t>(std::clamp<int64_t>(preference.value_or(10), 0, 65535))};
            std::vector<unsigned char> prefix;
            PutU16(prefix, pref);
            out.AddName(MX, *host, prefix, strprintf("%u ", pref));
        }
    }
    if (want(SRV)) {
        // [priority, weight, port, target]
        for (const UniValue& srv : Many(node, "srv")) {
            if (!srv.isArray() || srv.size() != 4 || !srv[0].isNum() || !srv[1].isNum() || !srv[2].isNum() || !srv[3].isStr()) continue;
            std::vector<unsigned char> prefix;
            uint16_t v[3];
            for (int i{0}; i < 3; ++i) {
                v[i] = static_cast<uint16_t>(std::clamp<int64_t>(srv[i].getInt<int64_t>(), 0, 65535));
                PutU16(prefix, v[i]);
            }
            out.AddName(SRV, srv[3].get_str(), prefix, strprintf("%u %u %u ", v[0], v[1], v[2]));
        }
    }
    if (want(TLSA)) {
        // [usage, selector, matching type, data in hex]
        for (const UniValue& tls : Many(node, "tls")) {
            if (!tls.isArray() || tls.size() != 4 || !tls[0].isNum() || !tls[1].isNum() || !tls[2].isNum() || !tls[3].isStr()) continue;
            const auto data{TryParseHex<unsigned char>(tls[3].get_str())};
            if (!data) continue;
            std::vector<unsigned char> rdata;
            for (int i{0}; i < 3; ++i) rdata.push_back(static_cast<unsigned char>(std::clamp<int64_t>(tls[i].getInt<int64_t>(), 0, 255)));
            rdata.insert(rdata.end(), data->begin(), data->end());
            out.Add(TLSA, std::move(rdata), strprintf("%d %d %d %s", tls[0].getInt<int64_t>(), tls[1].getInt<int64_t>(), tls[2].getInt<int64_t>(), tls[3].get_str()));
        }
    }
}

/** A node of the map: an object, or a string that stands for {"ip": string}. */
UniValue AsNode(const UniValue& v)
{
    if (v.isObject()) return v;
    UniValue node(UniValue::VOBJ);
    if (v.isStr()) node.pushKV("ip", v.get_str());
    return node;
}
} // namespace

namespace {
/** The records of a name as a node: its records, and its addresses where they have none of their own. */
UniValue RootNode(const NameRecord& record)
{
    const NameData data{record.Current()};
    UniValue root(UniValue::VOBJ);
    if (data.records) {
        UniValue parsed;
        if (parsed.read(*data.records) && parsed.isObject()) root = parsed;
    }
    if (!root.exists("ip") && !root.exists("alias") && data.ipv4) {
        root.pushKV("ip", strprintf("%u.%u.%u.%u", data.ipv4->ip[0], data.ipv4->ip[1], data.ipv4->ip[2], data.ipv4->ip[3]));
    }
    if (!root.exists("ip6") && !root.exists("alias") && data.ipv6) {
        in6_addr addr;
        std::memcpy(&addr, data.ipv6->ip.data(), 16);
        root.pushKV("ip6", CNetAddr{addr}.ToStringAddr());
    }
    return root;
}

/** The name a "delegate" entry names: its text, or the hash of a name ("0x" + 64 hex digits). */
std::optional<NameHash> DelegateTarget(const UniValue& value)
{
    if (!value.isStr() || value.get_str().empty()) return std::nullopt;
    const std::string& text{value.get_str()};
    if (text.size() == 66 && text.starts_with("0x")) {
        if (const auto hash{uint256::FromHex(text.substr(2))}) return *hash;
    }
    return HashName(ToLower(text));
}
} // namespace

dns::Answer Resolve(const State& state, const std::string& qname, uint16_t qtype)
{
    std::string name{ToLower(qname)};
    if (!name.empty() && name.back() == '.') name.pop_back();
    std::vector<std::string> labels;
    for (size_t start{0};;) {
        const size_t end{std::min(name.find('.', start), name.size())};
        labels.push_back(name.substr(start, end - start));
        if (end == name.size()) break;
        start = end + 1;
    }
    Answer answer;
    if (labels.empty() || labels.back() != DNS_TLD) {
        answer.rcode = REFUSED;
        return answer;
    }
    if (labels.size() == 1) return answer;
    const auto lookup{[&](const NameHash& hash) -> const NameRecord* {
        const auto it{state.Names().find(hash)};
        return it == state.Names().end() ? nullptr : &it->second;
    }};
    NameHash current{HashName(labels[labels.size() - 2])};
    const NameRecord* record{lookup(current)};
    if (!record) {
        answer.rcode = NXDOMAIN;
        return answer;
    }
    UniValue node{RootNode(*record)};
    // The domain relative host names are under: that of the name whose records are read.
    std::string base{labels[labels.size() - 2] + "." + DNS_TLD};
    int delegations{0};
    // A node that delegates to another name stands for the records of that name.
    const auto follow{[&]() -> bool {
        while (node.isObject() && node.exists("delegate")) {
            const auto target{DelegateTarget(node["delegate"])};
            const NameRecord* other{target ? lookup(*target) : nullptr};
            if (!other || ++delegations > 8) return false;
            current = *target;
            record = other;
            node = RootNode(*other);
        }
        return true;
    }};
    if (!follow()) {
        answer.rcode = SERVFAIL;
        return answer;
    }

    // Down from the name to the subdomain asked for: a subname on the chain first, then the map of
    // the records, where "*" matches any label.
    for (size_t i{labels.size() - 2}; i-- > 0;) {
        const std::string& label{labels[i]};
        const NameHash sub{SubnameHash(current, label)};
        if (const NameRecord* subname{lookup(sub)}) {
            current = sub;
            record = subname;
            node = RootNode(*subname);
            base = label + "." + base;
        } else {
            const UniValue& map{node.exists("map") ? node["map"] : UniValue{}};
            if (map.isObject() && map.exists(label)) {
                node = AsNode(map[label]);
            } else if (map.isObject() && map.exists("*")) {
                node = AsNode(map["*"]);
            } else {
                answer.rcode = NXDOMAIN;
                return answer;
            }
        }
        if (!follow()) {
            answer.rcode = SERVFAIL;
            return answer;
        }
    }
    // The records under "" in the map belong to the node itself.
    if (node.exists("map") && node["map"].isObject() && node["map"].exists("")) {
        const UniValue self{AsNode(node["map"][""])};
        for (const std::string& key : self.getKeys()) {
            if (!node.exists(key)) node.pushKV(key, self[key]);
        }
    }
    Collector collector{name, base};
    Collect(node, qtype, collector);
    collector.m_answer.rcode = NOERROR;
    return collector.m_answer;
}

std::vector<unsigned char> DnsRespond(const std::vector<unsigned char>& query, const std::function<dns::Answer(const std::string&, uint16_t)>& resolve)
{
    if (query.size() < 12) return {};
    const uint16_t flags{static_cast<uint16_t>(query[2] << 8 | query[3])};
    if (flags & 0x8000) return {}; // a response, not a question
    const uint8_t opcode{static_cast<uint8_t>((flags >> 11) & 0xf)};
    const uint16_t qdcount{static_cast<uint16_t>(query[4] << 8 | query[5])};

    const auto reply{[&](Rcode rcode, size_t question_end, const std::vector<Record>& records, const std::string& qname) {
        std::vector<unsigned char> out{query[0], query[1]};
        PutU16(out, 0x8000 | (opcode << 11) | 0x0400 | (flags & 0x0100) | rcode);
        PutU16(out, question_end > 12 ? 1 : 0);
        PutU16(out, records.size());
        PutU16(out, 0);
        PutU16(out, 0);
        out.insert(out.end(), query.begin() + 12, query.begin() + question_end);
        for (const Record& record : records) {
            if (record.name == qname) {
                PutU16(out, 0xc00c); // the name of the question
            } else {
                const auto encoded{EncodeName(record.name)};
                if (!encoded) continue;
                out.insert(out.end(), encoded->begin(), encoded->end());
            }
            PutU16(out, record.type);
            PutU16(out, 1); // IN
            PutU32(out, DNS_TTL);
            PutU16(out, record.rdata.size());
            out.insert(out.end(), record.rdata.begin(), record.rdata.end());
        }
        // Too large for UDP without EDNS: say so, and leave the answers out.
        if (out.size() > 512) {
            out.resize(question_end);
            out[2] |= 0x02;
            out[6] = 0;
            out[7] = 0;
        }
        return out;
    }};

    if (opcode != 0) return reply(NOTIMP, 12, {}, {});
    if (qdcount != 1) return reply(FORMERR, 12, {}, {});
    size_t p{12};
    std::string qname;
    while (true) {
        if (p >= query.size()) return reply(FORMERR, 12, {}, {});
        const uint8_t length{query[p++]};
        if (length == 0) break;
        if (length > 63 || p + length > query.size()) return reply(FORMERR, 12, {}, {});
        if (!qname.empty()) qname += '.';
        qname.append(reinterpret_cast<const char*>(&query[p]), length);
        p += length;
        if (qname.size() > 253) return reply(FORMERR, 12, {}, {});
    }
    if (p + 4 > query.size()) return reply(FORMERR, 12, {}, {});
    const uint16_t qtype{static_cast<uint16_t>(query[p] << 8 | query[p + 1])};
    const uint16_t qclass{static_cast<uint16_t>(query[p + 2] << 8 | query[p + 3])};
    const size_t question_end{p + 4};
    if (qclass != 1 && qclass != 255) return reply(REFUSED, question_end, {}, qname);
    const Answer answer{resolve(qname, qtype)};
    return reply(answer.rcode, question_end, answer.records, ToLower(qname));
}

namespace {
class DnsServer
{
public:
    ~DnsServer() { Stop(); }

    bool Start(const CService& bind, ChainstateManager& chainman, std::string& error)
    {
        sockaddr_storage storage;
        socklen_t length{sizeof(storage)};
        if (!bind.GetSockAddr(reinterpret_cast<sockaddr*>(&storage), &length)) {
            error = "cannot use the address";
            return false;
        }
        m_socket = ::socket(storage.ss_family, SOCK_DGRAM, IPPROTO_UDP);
        if (m_socket == INVALID_SOCKET) {
            error = "cannot open a socket";
            return false;
        }
#ifdef WIN32
        DWORD timeout{500};
#else
        timeval timeout{0, 500000};
#endif
        ::setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        if (::bind(m_socket, reinterpret_cast<sockaddr*>(&storage), length) != 0) {
            error = strprintf("cannot listen on %s (is it in use?)", bind.ToStringAddrPort());
            CloseSocket(m_socket);
            m_socket = INVALID_SOCKET;
            return false;
        }
        m_thread = std::thread{[this, &chainman] {
            util::ThreadRename("bitnamesdns");
            Run(chainman);
        }};
        LogInfo("BitNames DNS server for .%s listening on %s", DNS_TLD, bind.ToStringAddrPort());
        return true;
    }

    void Stop()
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        if (m_socket != INVALID_SOCKET) CloseSocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

private:
    void Run(ChainstateManager& chainman)
    {
        std::vector<unsigned char> buffer(4096);
        const auto resolve{[&chainman](const std::string& qname, uint16_t qtype) {
            LOCK(::cs_main);
            return Resolve(chainman.ActiveChainstate().m_scdb.m_side.BitNames(), qname, qtype);
        }};
        while (!m_stop) {
            sockaddr_storage from;
            socklen_t from_length{sizeof(from)};
            const auto received{::recvfrom(m_socket, reinterpret_cast<char*>(buffer.data()), buffer.size(), 0, reinterpret_cast<sockaddr*>(&from), &from_length)};
            if (received <= 0) continue;
            const std::vector<unsigned char> query(buffer.begin(), buffer.begin() + received);
            const std::vector<unsigned char> response{DnsRespond(query, resolve)};
            if (response.empty()) continue;
            ::sendto(m_socket, reinterpret_cast<const char*>(response.data()), response.size(), 0, reinterpret_cast<sockaddr*>(&from), from_length);
        }
    }

    static void CloseSocket(SOCKET s)
    {
#ifdef WIN32
        closesocket(s);
#else
        close(s);
#endif
    }

    SOCKET m_socket{INVALID_SOCKET};
    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

std::unique_ptr<DnsServer> g_dns_server;
std::string g_dns_address;
} // namespace

std::optional<std::string> DnsServerAddress()
{
    if (!g_dns_server) return std::nullopt;
    return g_dns_address;
}

bool StartDnsServer(const ArgsManager& args, ChainstateManager& chainman, std::string& error)
{
    if (!args.IsArgSet("-dnsbind")) return true;
    const std::string where{args.GetArg("-dnsbind", "")};
    const auto bind{Lookup(where.empty() ? "127.0.0.1" : where, DEFAULT_DNS_PORT, /*fAllowLookup=*/false)};
    if (!bind) {
        error = strprintf("Invalid -dnsbind address: %s", where);
        return false;
    }
    g_dns_server = std::make_unique<DnsServer>();
    std::string why;
    if (!g_dns_server->Start(*bind, chainman, why)) {
        g_dns_server.reset();
        error = strprintf("The BitNames DNS server (-dnsbind=%s) did not start: %s", where, why);
        return false;
    }
    g_dns_address = bind->ToStringAddrPort();
    return true;
}

void StopDnsServer() { g_dns_server.reset(); }

} // namespace bitnames
