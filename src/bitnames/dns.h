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
 * How many questions the DNS server answers: a token bucket per source address, and one for all
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
        double total_rate{1000};
        double total_burst{2000};
        //! Sources remembered at most; beyond that, those whose bucket is full again are forgotten.
        size_t max_sources{10000};
    };
    DnsRateLimiter() : DnsRateLimiter(Limits{}) {}
    explicit DnsRateLimiter(const Limits& limits) : m_limits{limits}, m_total{limits.total_burst, {}} {}
    /** Whether a question from `source` at `now` may be answered; it is counted if so. */
    bool Allow(const std::string& source, Clock::time_point now);
    size_t Sources() const { return m_sources.size(); }

private:
    struct Bucket {
        double tokens{0};
        Clock::time_point last{};
    };
    static void Refill(Bucket& bucket, double rate, double burst, Clock::time_point now);

    Limits m_limits;
    Bucket m_total;
    bool m_started{false};
    std::unordered_map<std::string, Bucket> m_sources;
};

/** A DNS response to the query `query` (a UDP datagram); empty if it is not worth answering. */
std::vector<unsigned char> DnsRespond(const std::vector<unsigned char>& query, const std::function<dns::Answer(const std::string&, uint16_t)>& resolve);

/** Start the DNS server if -dnsbind is set. @return false if it was asked for and could not start. */
bool StartDnsServer(const ArgsManager& args, ChainstateManager& chainman, std::string& error);
void StopDnsServer();
/** Where the DNS server listens, if it runs. */
std::optional<std::string> DnsServerAddress();

} // namespace bitnames

#endif // BITCOIN_BITNAMES_DNS_H
