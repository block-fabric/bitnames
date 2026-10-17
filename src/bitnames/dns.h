// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BITNAMES_DNS_H
#define BITCOIN_BITNAMES_DNS_H

#include <bitnames/state.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

class ArgsManager;
class CNetAddr;
class ChainstateManager;

/**
 * A DNS server for the names: `<name>.x` and its subdomains resolve from the DNS
 * records of the name (in the format of Namecoin's domains), and, without them,
 * from its IPv4 and IPv6 addresses. Started with -dnsbind; the operating system
 * is told to ask it for `.x` (see doc/bitnames.md).
 */
namespace bitnames {

/** The top-level domain of the names. */
inline const std::string DNS_TLD{"x"};
inline constexpr uint16_t DEFAULT_DNS_PORT{5335};
inline constexpr uint32_t DNS_TTL{60};
/**
 * Most labels of a question answered (example: www.blog.core.x is 4): the top-level domain, a name,
 * MAX_SUBNAME_DEPTH subnames and a few hosts of the records under them. Longer ones are refused before
 * anything is read.
 */
inline constexpr size_t MAX_DNS_LABELS{16};
/**
 * Most names read from the state for one answer (each label, each delegation, each step down the
 * subnames a delegation names): the chain's lock is held while they are read. Beyond it, SERVFAIL. One more
 * read per question looks for an ICANN name of its last two labels (IcannStart).
 */
inline constexpr int MAX_DNS_READS{48};

namespace dns {
enum Type : uint16_t { A = 1, NS = 2, CNAME = 5, SOA = 6, MX = 15, TXT = 16, AAAA = 28, SRV = 33, TLSA = 52, ANY = 255 };
enum Rcode : uint8_t { NOERROR = 0, FORMERR = 1, SERVFAIL = 2, NXDOMAIN = 3, NOTIMP = 4, REFUSED = 5 };

struct Record {
    //! The owner name, absolute, without the final dot.
    std::string name;
    uint16_t type{0};
    std::vector<unsigned char> rdata;
    //! The data as text, for people.
    std::string text;
};

struct Answer {
    Rcode rcode{NOERROR};
    std::vector<Record> records;
};

std::string TypeName(uint16_t type);
std::optional<uint16_t> ParseType(const std::string& name);
} // namespace dns

/**
 * The answer to a question for `qname` (absolute, the final dot optional, any case) of type
 * `qtype`, from the state of the names. A name outside `.x` is refused.
 */
dns::Answer Resolve(const State& state, const std::string& qname, uint16_t qtype);

/**
 * The source a question is counted against: an IPv4 address, or the /64 network of an IPv6 address
 * (whoever has one address of a /64 has them all).
 */
std::string DnsSourceKey(const CNetAddr& address);

/**
 * How many questions the DNS server answers: a token bucket per source (DnsSourceKey), and one for all
 * of them. Over UDP the source can be forged, and an answer is larger than its question: without a
 * limit the server would send a flood to whoever a forger names, and hold the chain's lock for each.
 */
class DnsRateLimiter
{
public:
    using Clock = std::chrono::steady_clock;
    struct Limits {
        double source_rate{20};
        double source_burst{40};
        //! A local bridge for the names, not a public resolver: a few hundred questions a second.
        double total_rate{200};
        double total_burst{400};
        //! Sources remembered at most; beyond that, those whose bucket is full again are forgotten.
        size_t max_sources{10000};
        //! How often, at most, the sources are looked through for those to forget: a flood of new
        //! (forged) sources costs one pass over the table per interval, not one per question.
        std::chrono::milliseconds sweep_interval{1000};
    };
    DnsRateLimiter() : DnsRateLimiter(Limits{}) {}
    explicit DnsRateLimiter(const Limits& limits) : m_limits{limits}, m_total{limits.total_burst, {}} {}
    /** Whether a question from `source` at `now` may be answered; it is counted if so. */
    bool Allow(const std::string& source, Clock::time_point now);
    size_t Sources() const { return m_sources.size(); }
    /** How many times the sources were looked through for those to forget. */
    uint64_t Sweeps() const { return m_sweeps; }

private:
    struct Bucket {
        double tokens{0};
        Clock::time_point last{};
    };
    static void Refill(Bucket& bucket, double rate, double burst, Clock::time_point now);

    Limits m_limits;
    Bucket m_total;
    bool m_started{false};
    std::optional<Clock::time_point> m_last_sweep;
    uint64_t m_sweeps{0};
    std::unordered_map<std::string, Bucket> m_sources;
};

/** A DNS response to the query `query` (a UDP datagram); empty if it is not worth answering. */
std::vector<unsigned char> DnsRespond(const std::vector<unsigned char>& query, const std::function<dns::Answer(const std::string&, uint16_t)>& resolve);

/**
 * Where -dnsbind asks the DNS server to listen: nullopt if it is off (not set, -nodnsbind, dnsbind=0), the
 * loopback address for -dnsbind alone (or =1), else the address given.
 */
std::optional<std::string> DnsBindSetting(const ArgsManager& args);
/**
 * Start the DNS server if -dnsbind is set. A wildcard address (0.0.0.0, ::) is refused unless -dnsbindany is
 * set; any address that is not loopback is warned about. @return false if it was asked for and could not start. */
bool StartDnsServer(const ArgsManager& args, ChainstateManager& chainman, std::string& error);
void StopDnsServer();
/** Where the DNS server listens, if it runs. */
std::optional<std::string> DnsServerAddress();

} // namespace bitnames

#endif // BITCOIN_BITNAMES_DNS_H
