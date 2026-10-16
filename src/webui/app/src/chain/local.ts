// The pages of this chain of its own, beside those of the kit: the one file of the interface (with
// LocalPages.svelte, which shows them, and chain.json) that a chain made from the template changes, so
// that merges of the template stay clean. See doc/webui.md, "Pages of a chain of its own".
//
// BitNames: getting a name (search, then reservation and registration as one flow), the names of the wallet
// (records, profile, subnames, giving away, releasing), the directory of every name, paymail (paid messages,
// encrypting and signing as a name), and the DNS of .x (the server of the node, a lookup tester); their
// components in LocalPages.svelte (chain/pages, chain/bn), their RPC methods in src/webui/local.cpp, the cards
// of the home in LocalCards.svelte, their icons in icons.ts.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  /** Stable: the page id (go('names')), the command page.<id>. */
  id: string;
  title: string;
  icon: IconName;
  /** What the page is for, in a line (tooltips, the palette). */
  summary: string;
  /**
   * The section of the sidebars and the area of the workspace it goes in. What concerns the chain comes
   * before the tools, whatever the section (the chainBeforeTools tests).
   */
  section: 'wallet' | 'chain' | 'tools';
  /** The page needs a loaded wallet. */
  wallet?: boolean;
  shortcut?: string;
  /** More words to find it by in the palette. */
  keywords?: string;
  /** A quick action of the dashboard's home that opens it (after Send and Receive, with the chain's). */
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  /** Its name in Simple mode, in plain words, where it differs from the title ("My bets" for Portfolio). */
  simpleTitle?: string;
  /** Only in the navigation of Advanced mode (lib/uimode.svelte.ts): Simple mode reaches it by the palette and links. */
  advanced?: boolean;
}

export const LOCAL_PAGES = [
  { id: 'register', title: 'Get a name', icon: 'tagplus', section: 'chain', summary: 'Find a name that is free and take it: reserved, then registered, step by step', wallet: true, keywords: 'bitnames register reserve search available domain buy claim', quick: { label: 'Get a name', icon: 'tagplus' } },
  { id: 'names', title: 'My names', icon: 'tag', section: 'chain', shortcut: 'Alt+6', summary: 'The names of the wallet: DNS records, profile and keys, subnames, giving away, releasing', wallet: true, keywords: 'bitnames records dns subnames transfer release keys profile' },
  { id: 'directory', title: 'Name directory', icon: 'atlas', section: 'chain', summary: 'Every name of the registry: search, browse, and see any name whole', keywords: 'bitnames explore search lookup whois registry', advanced: true },
  { id: 'paymail', title: 'Paymail', icon: 'mail', section: 'chain', summary: 'Messages with a payment that only the holder of a name reads; encrypt and sign as a name', wallet: true, keywords: 'bitnames paid messages encrypt decrypt sign verify inbox', quick: { label: 'Paymail', icon: 'mail', tab: 'send' }, advanced: true },
  { id: 'dns', title: 'DNS', icon: 'globe', section: 'chain', summary: 'The DNS server of .x names on this node, and a lookup tester', keywords: 'bitnames dnsbind resolve lookup resolver .x domain', advanced: true },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

/** Items of the menus (the Menus button and the palette) of this chain: in the group 'Chains' (before the tools). */
export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'bn.records', label: 'Edit DNS records…', page: 'names', tab: 'records', tip: 'The records of a name of the wallet: what yourname.x answers', keywords: 'dns records a aaaa cname mx' },
  { id: 'bn.subname', label: 'Make a subname…', page: 'names', tab: 'subnames', tip: 'A subname of a name of the wallet, for this wallet or anyone', keywords: 'subname subdomain' },
  { id: 'bn.give', label: 'Give a name away…', page: 'names', tab: 'transfer', tip: 'Give a name of the wallet to another address, or release it', keywords: 'transfer release' },
  { id: 'bn.send-paymail', label: 'Send paymail…', page: 'paymail', tab: 'send', tip: 'Pay a name, with a message only its holder can read', keywords: 'paid message' },
  { id: 'bn.sign', label: 'Sign as a name…', page: 'paymail', tab: 'tools', tip: 'Sign a text with the signing key of a name, or check a signature', keywords: 'signature verify encrypt decrypt' },
  { id: 'bn.lookup', label: 'Look up a .x domain', page: 'dns', tip: 'What the DNS server of the node answers for a domain', keywords: 'resolve dns' },
] as const satisfies readonly LocalCommand[];
