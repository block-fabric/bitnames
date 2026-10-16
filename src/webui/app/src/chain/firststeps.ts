// The three first steps of BitNames, shown on Home in Simple mode until dismissed
// (components/FirstRunTips.svelte; lib/firststeps.ts). English here: the card translates them.
import type { FirstStep } from '../lib/firststeps';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring coins here', text: 'Coins come from Chains. Drivechains gives a deposit address: paste it in the Chains app (Sidechains, Move coins). They pay the small fees of a name.', icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Get a name', text: 'Type a name to see whether it is free. Taking it takes a few blocks, and it is yours for good: nothing to renew.', icon: 'tagplus', action: { label: 'Get a name', page: 'register' } },
  { title: 'Take coins back', text: 'When you are done, Drivechains sends coins back to Chains. That takes longer: the miners of Chains approve withdrawals together.', icon: 'up', action: { label: 'Send coins back', page: 'peg', tab: 'withdraw' } },
];
