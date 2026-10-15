// Icons of this chain's own pages, beside those of the kit (components/Icon.svelte): line icons on a
// 24 grid with a 1.75 stroke, one SVG path each. The pages of BitNames name them in chain/local.ts.
export const LOCAL_ICONS = {
  /** A name: a tag with its hole. */
  tag: 'M3.5 6.5a2 2 0 0 1 2-2h8.2a2 2 0 0 1 1.4.6l5.4 5.5a2 2 0 0 1 0 2.8l-5.4 5.5a2 2 0 0 1-1.4.6H5.5a2 2 0 0 1-2-2zM8 12a1.4 1.4 0 1 0 2.8 0A1.4 1.4 0 1 0 8 12',
  /** Get a name: a tag and a plus. */
  tagplus: 'M3.5 6.5a2 2 0 0 1 2-2h8.2a2 2 0 0 1 1.4.6l5.4 5.5a2 2 0 0 1 0 2.8l-5.4 5.5a2 2 0 0 1-1.4.6H5.5a2 2 0 0 1-2-2zM7 12h6M10 9v6',
  /** A subname: a branch from a line. */
  subname: 'M6 4v9a3 3 0 0 0 3 3h9M14 12l4 4-4 4M6 4h.01',
  /** The directory: an open book with a tag. */
  atlas: 'M4 5.5C6.5 4.5 9.5 4.5 12 6c2.5-1.5 5.5-1.5 8-.5v13c-2.5-1-5.5-1-8 .5-2.5-1.5-5.5-1.5-8-.5zM12 6v13.5',
  /** Paymail: an envelope. */
  mail: 'M3.5 6.5h17v11h-17zM3.5 7l8.5 6.5L20.5 7',
  /** DNS: a globe. */
  globe: 'M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18zM3 12h18M12 3c2.6 2.6 3.8 5.6 3.8 9s-1.2 6.4-3.8 9c-2.6-2.6-3.8-5.6-3.8-9S9.4 5.6 12 3',
} as const satisfies Record<string, string>;
