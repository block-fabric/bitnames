# BitNames: names on a sidechain

BitNames is a sidechain of Chains (slot 3) on which names are owned like coins. A name points to
addresses, keys and DNS records; its owner can be paid with messages only they can read. It is
inspired by **BitNames by LayerTwo Labs** (plain-bitnames), built anew on this Bitcoin Core based
sidechain, with Bitcoin's keys and transactions, without the defects of the original, and with
features of Namecoin added: DNS records on the chain, subnames, public names and a DNS
server for the `.x` domain.

## Names

- **Taking a name** takes two steps, so that nobody can see it coming and take it first. A
  *reservation* commits to the name without showing it (HMAC-SHA256, under a nonce only the wallet
  can make, of the name's hash and the script of the output that carries the reservation); once it
  is deep enough in the chain, the *registration* reveals the nonce and creates the name. The window
  ("Names", in the sidebar) does both steps.
- **A reservation is revealed only once it is deep enough** (`reveal_depth` blocks: 6 on the main
  network, 3 on the test network and signet, 2 on regtest; `getbitnamesinfo` shows it): a
  registration in block *h* of a reservation made in block *r* needs *h - r* ≥ `reveal_depth`.
  Whoever makes a block sees every registration waiting for it, name and nonce; without this rule
  they could reserve the same name under a nonce of their own and register it in their own block,
  ahead of the one they saw. A reservation made after seeing a name revealed is never deep enough
  before that registration is in a block, unless whoever makes the blocks keeps it out of
  `reveal_depth` blocks in a row.
- **Copies of a reservation.** A reservation's commitment is public once it is in the mempool, so
  anyone can make another with the same commitment, in the same block even (whoever makes the block,
  or pays a higher fee). The commitment binds the script of the reservation's output (kept with the
  reservation, table `0x2d`), and a registration opens it with that script: a copy carried by
  anyone else's output opens nothing, and one carried by an output of the maker's script is the
  maker's to spend. So copies neither stop a registration nor take the name. These are the rules of
  the second audit (`audit2_height`); before them a reservation committed to the name alone, and
  the oldest reservation of a commitment revealed it (which let whoever made a block stop a
  registration with a copy put ahead of it, or, once a same-block copy no longer stopped it, take
  the name through the copy). A reservation made before those rules and revealed after them is
  stopped by any other of its commitment from the same block or an earlier one; a commitment to the
  name alone made after them opens nothing.
- **A name is known on the chain by the SHA-256 hash of its text.** A registration may also make
  the text *public*, so that the name can be listed and searched for, as Namecoin names are.
- **The text of a name.** The wallet takes names of lower-case letters, digits, `-` and `_`, as DNS
  looks names up in lower case (`Core.x` is `core.x`). The chain knows hashes only, so a name of
  any other text can be registered by its hash; the commands then refer to it by its hash
  (`0x...`) or number, never by text, and its text, even if published, is not shown. Commands
  that take a name (node and wallet alike, and `delegate` in DNS records) refuse any other text
  (upper case, a look-alike letter of another script such as Cyrillic `а`, a space) rather than
  look up the name of its hash, which anyone could have registered.
- **Dotted names are subnames, never hashes.** `pay.shop` in a command is the subname `pay` of
  `shop`, if there is one, and an error if not. The name whose hash is SHA-256 of `pay.shop` (which
  anyone can register by its hash) is reachable only as `0x` and its hash.
- **Names are permanent.** They do not expire. Their owner can give them away
  (`transfername`) or release them (`releasename`): a released name leaves the registry with its
  data, and anyone can register it again. A reservation that is not needed can be released too
  (`releasereservation`).
- **Numbers.** Each name gets a number in the order of registration, shown as BitNames shows it,
  `1739-0029` for the first. Commands take a number with `#` in front, `#1739-0029`: without it,
  `1739-0029` is a text like any other, which anyone can register as a name.
- **Data.** Every field is optional: a 32-byte commitment to data kept elsewhere, an IPv4 and an
  IPv6 address with port, a key messages to the owner are encrypted to, a key the owner signs
  with, the paymail fee, and DNS records. Every value is kept with the transaction and height that
  set it: `getname <name> <height>` gives the data of a name at any height, `getnamehistory` all
  its values, its records included.
- **Giving a name on.** `transfername <name> <address>` also deletes, in the same transaction,
  the encryption key, the signing key and the paymail fee of the name: they are keys of the wallet
  that gives it, which would still receive the name's messages and sign as it. `false` as a third
  argument keeps them (for a name moved to another wallet of the same owner). The window asks, with
  deleting them checked.
- **Subnames.** The owner of `core` can make `blog.core` and give it to any address
  (`createsubname core blog <address>`). A subname is a name of its own: its holder sets its data
  and records, gives it on, and makes subnames of it. On the chain a subname is known by SHA-256 of
  its parent's hash and its label. A top-level name registered by that hash (a squat) gives way when
  the subname is made (`createsubname` makes it over one). A subname made again after it was
  released starts with no subnames: those an earlier holder made, left under it by a release before
  releasing took them along, are released then (second audit). Until then such a subname, older
  than its parent's registration, is on the chain but not the new holder's: no text names it (its
  holder still reaches it by its hash) and DNS does not answer for it, and its name is not shown.
- **A subname depends on its parent, as a DNS subdomain depends on its domain.** What its holder
  can rely on: nobody else can update it, move it, release it alone or make subnames of it, and
  the holder of the parent cannot make another subname of the same label while it exists. What
  its holder cannot rely on: **when the parent is released, its subnames are released with it**,
  at any depth, whoever holds them; whoever registers the parent next starts with an empty
  namespace and can make the same labels again, for anyone. And the parent's holder can, at any
  time, make the subname's domain answer elsewhere: DNS resolves down from the top-level name, so
  a `delegate` in the records of the parent (or of a level above it) is followed before the
  subname is reached, and `blog.core.x` then resolves under the name delegated to. A subname is as lasting as the holder of its parent wants it to be: hold one of a
  name whose holder you trust, or take a top-level name.
- **Bounds.** A subname is at most 8 levels under its top-level name, and a name has at most 1000
  names under it, at any depth (from the rules of the second audit). Releasing a name releases
  them all in one transaction: the bound keeps the work of checking it (in the mempool, for each
  replacement, after every block, and in the block) small, and a tree of that size costs whoever
  builds it 1000 transactions. The mempool (policy, not consensus) takes no transaction that releases
  more than 1001 names in all (a name and the most it may have under it; subnames swept from under a
  name registered again, and a squat a subname replaces with what is under it, count too): it checks
  name transactions again after every block, under the chain's lock, and stops counting there, so
  that a transaction waiting in it costs that much at most however its trees grow. A block may
  release more: each name is released once and was made by a transaction of its own, so a block's
  work is bounded by what was paid for. A larger tree (made on the test network before the bounds)
  is released by its holders releasing parts of it first, or by whoever makes a block.
- **ICANN names.** An authority set per network can mark names as ICANN domain names. None is set
  on the test network. The authority marks a *hash*: whoever registered `example.com` first, by
  text or by its hash, holds the name it marks. The authority has to check who holds a name before
  marking it; the mark does not move the name to the domain's owner.

## How it is carried

Names and reservations are carried by outputs of no value. A *marker*, an OP_RETURN output of the
transaction, lists which outputs carry names and which carry reservations, and says what the
transaction does (reserve, register, update, make a subname, release, mark ICANN names). The names
and reservations a transaction spends go to the outputs it lists, in order; what it creates comes
last. Their number cannot change except as the operation says, so a name cannot be lost by
accident: a transaction that spends a name without carrying it on is invalid.

## Messages

- `getnewnamekeys` makes keys to set on a name: an encryption key (secp256k1) and a signing key
  (BIP340, x-only).
- `encryptnamemessage` encrypts to the encryption key of a name, as BIP324 sets up its
  connections: an ElligatorSwift Diffie-Hellman with a key made for the message, then
  ChaCha20-Poly1305. `decryptnamemessage` opens a message to a key of the wallet.
- `signasname` and `verifynamesignature` sign and check messages with the signing key of a name.
- **Paymail.** `sendpaymail <name> <amount> <message>` pays the owner of a name, at least the
  name's paymail fee, with a message encrypted to it; `listpaymail` lists what the wallet received
  that paid at least the paymail fee of the wallet's name with the key the message is encrypted
  to, as the fee was in the block of the payment (anyone can send a message with any amount; one
  that paid less is left out).

## DNS records

The `records` of a name are a JSON object of at most 1024 bytes, in the format of Namecoin's
domains:

| Key | Value | DNS |
|---|---|---|
| `ip` | `"203.0.113.7"` or a list | A |
| `ip6` | `"2001:db8::7"` or a list | AAAA |
| `alias` | a host name | CNAME (stands for all records of the node) |
| `txt` | a string or a list | TXT |
| `mx` | `[[10, "mail"], ...]` | MX |
| `srv` | `[[priority, weight, port, "target"], ...]` | SRV |
| `tls` | `[[usage, selector, matching type, "hex"], ...]` | TLSA |
| `ns` | a list of host names | NS |
| `map` | `{"label": {...}, "*": {...}, "": {...}}` | subdomains; `*` matches any label, `""` adds to the node itself |
| `delegate` | another name: its text, a subname (`blog.core`), or `0x` and its hash | the records of that name stand for this node |

A `delegate` with dots is a subname, found down from its last label as commands find it, never the
top-level name of the hash of that text (anyone can register that one); `setnamerecords` refuses a
`DELEGATE` that is not a name, a subname or a hash, and DNS answers SERVFAIL for one that names
nothing.

A host name ending with a dot is absolute (`example.com.`); otherwise it is under the name
(`mail` in the records of `core` is `mail.core.x`); `@` is the name itself. Without an `ip` or
`ip6` of their own, the name's IPv4 and IPv6 addresses answer.

A domain `a.b.core.x` resolves down from `core`: for each label, a subname on the chain first,
then the `map` of the records, then `*`. `resolvename www.core.x A` shows what the DNS server
answers.

## The DNS server

Start the node with `-dnsbind=127.0.0.1:5335` (or `dnsbind=127.0.0.1:5335` in `bitnames.conf`);
it answers DNS questions for `.x` over UDP (TTL 60 seconds), and refuses the others. Answers too
large for a UDP datagram of 512 bytes are sent empty with the TC bit set. Each source (an IPv4
address, or an IPv6 /64) gets at most 20 answers a second (with bursts of 40), and the server at
most 200 a second in all (bursts of 400); questions beyond that get no answer, so that the server
cannot be used to flood a forged source address. Of 10000 sources remembered, those whose bucket is
full again are forgotten when a new one comes, at most once a second. A question has at most 16
labels, and an answer reads at most 48 names of the chain (labels, delegations and the subnames
they name), under the chain's lock; beyond that it is SERVFAIL. Bind it to a local address unless it is meant to serve others. Then send the
`.x` questions of the computer to it:

- **Linux, systemd-resolved:** `resolvectl dns lo 127.0.0.1:5335` and `resolvectl domain lo ~x`
- **Linux, dnsmasq:** `server=/x/127.0.0.1#5335`
- **macOS:** a file `/etc/resolver/x` with the lines `nameserver 127.0.0.1` and `port 5335`
- **Windows:** run the server on port 53 (`-dnsbind=127.0.0.1:53`), then in PowerShell
  `Add-DnsClientNrptRule -Namespace ".x" -NameServers 127.0.0.1`

Then `ping core.x` or a browser at `http://core.x` reach the address the name points to.

## Commands

Node: `getname`, `listnames`, `getnamehistory`, `getbitnamesinfo`, `resolvename`,
`verifynamesignature`.
Wallet: `reservename`, `registername`, `updatename`, `transfername`, `createsubname`,
`releasename`, `releasereservation`, `listmynames`, `getnewnamekeys`, `encryptnamemessage`,
`decryptnamemessage`, `signasname`, `sendpaymail`, `listpaymail`.

## Differences from the original

- Bitcoin's keys, addresses and transactions instead of ed25519 and BLAKE3; SHA-256 for name
  hashes, HMAC-SHA256 for reservations, secp256k1 for messages.
- Fixed: two registrations of the same name in one block (the original accepted both, then could
  not undo the block); transactions without inputs that could be replayed; ICANN marking, which
  could not work (its key was all zeros, its undo always failed).
- Added: DNS records, subnames, public names, release, the DNS server, paymail sending.

## Wallet

`sendall` and coin selection leave the outputs that carry names and reservations alone: they have
no value, only the commands of the names spend them, and a payment that spent one would be refused.

## Activation heights

The rules of the first audit (`audit_rules_height`) and of the second (`audit2_height`) are set per
network; on the main network, signet and regtest both are 0. On the test network they are the same
height, set at deploy, so that the first audit's rules never come after the second's.
