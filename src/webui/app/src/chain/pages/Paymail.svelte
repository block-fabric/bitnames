<script lang="ts">
  // Paymail: messages with a payment, that only the holder of a name can read. The inbox of the wallet
  // (listpaymail: those that paid at least the fee of a name of the wallet, decrypted); paying a name with a
  // message (sendpaymail, the fee of the name looked up as its text is typed); and by hand, encrypting to a
  // name, opening a message to the wallet, signing as a name of the wallet and checking a signature of a name.
  // The Messages tab of the Names page of chains-qt.
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import NoWallet from '../../components/NoWallet.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { stacked } from '../../lib/actions';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav, PAGES, setOwnTab, takeFocus } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets, walletsEnabled } from '../../lib/state.svelte';
  import { coinsToSats, satsToCoins } from '../../lib/units';
  import { CHAIN } from '../config';
  import '../bn/bn.css';
  import Domain from '../bn/Domain.svelte';
  import { MAX_MEMO_BYTES, MEMO_OVERHEAD, queryKind, utf8Bytes, withoutTld, type NameInfo, type Paymail } from '../bn/model';
  import { bn, startNames, walletLocked } from '../bn/store.svelte';

  $effect(() => startNames());

  const noWallet = $derived(!walletsEnabled() || wallets.current === null);
  const TABS = [
    { id: 'inbox', label: 'Inbox' },
    { id: 'send', label: 'Send' },
    { id: 'tools', label: 'Encrypt and sign' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'inbox');

  // --- Inbox ---
  let inbox = $state<Paymail[]>([]);
  let inboxState = $state<'idle' | 'loading' | 'locked' | 'error'>('idle');
  let inboxError = $state('');
  async function loadInbox(interactive: boolean) {
    const wallet = wallets.current;
    if (wallet === null) return;
    // Opening the messages needs the keys: a locked wallet waits for the user to ask.
    if (!interactive && walletLocked(wallet)) {
      inboxState = 'locked';
      return;
    }
    inboxState = 'loading';
    try {
      const got = await walletRpc<Paymail[]>(wallet, 'listpaymail', []);
      if (got === null) {
        inboxState = 'locked';
        return;
      }
      inbox = Array.isArray(got) ? got : [];
      inboxState = 'idle';
    } catch (e) {
      inboxError = errorText(e);
      inboxState = 'error';
    }
  }
  $effect(() => {
    void wallets.current;
    void bn.version;
    if (tab === 'inbox' && !noWallet) void loadInbox(false);
  });

  // --- Send ---
  let to = $state('');
  let target = $state<NameInfo | null>(null);
  let targetProblem = $state('');
  let amount = $state<bigint | null>(null);
  let amountKey = $state(0);
  let message = $state('');
  let busy = $state(false);
  $effect(() => {
    const key = takeFocus('paymail');
    if (key) to = key;
  });
  const toText = $derived(queryKind(to) === 'domain' ? (withoutTld(to) ?? to.trim()) : to.trim());
  let generation = 0;
  $effect(() => {
    const q = toText;
    const mine = ++generation;
    target = null;
    targetProblem = '';
    if (!q) return;
    if (queryKind(q) === 'bad') {
      targetProblem = t('A name, a subname, a number (#1739-0029) or a hash (0x…).');
      return;
    }
    const timer = setTimeout(async () => {
      try {
        const found = await rpc.raw<NameInfo>('getname', [q]);
        if (mine !== generation) return;
        target = found;
        if (!found.data?.encryptionkey) targetProblem = t('This name has no encryption key: it takes no messages.');
        else if (found.data.paymailfee === undefined) targetProblem = t('This name takes no paid messages (it has no paymail fee).');
        else if (amount === null || amount < coinsToSats(found.data.paymailfee)) {
          amount = coinsToSats(found.data.paymailfee);
          amountKey++;
        }
      } catch (e) {
        if (mine === generation) targetProblem = errorText(e);
      }
    }, 300);
    return () => clearTimeout(timer);
  });
  const fee = $derived(target?.data?.paymailfee !== undefined ? coinsToSats(target.data.paymailfee) : null);
  const memoBytes = $derived(utf8Bytes(message) + MEMO_OVERHEAD);
  const tooLong = $derived(memoBytes > MAX_MEMO_BYTES);
  const canSend = $derived(!!target && !targetProblem && amount !== null && fee !== null && amount >= fee && message.trim().length > 0 && !tooLong);

  async function send() {
    const wallet = wallets.current;
    if (wallet === null || !target || !canSend || amount === null) return;
    const r = await ask({
      title: t('Send paymail'),
      message: t('Pay %1 to %2, with this message?', `${satsToCoins(amount)} ${CHAIN.coin.ticker}`, target.name ? `${target.name}.x` : `#${target.seq}`),
      code: message,
      informative: t('Only the holder of the name can read the message. The payment goes to the address that holds the name.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Send', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const txid = await walletRpc<string>(wallet, 'sendpaymail', [toText, satsToCoins(amount), message]);
      if (txid === null) return;
      toast('success', t('Paymail sent'), t('In the transaction %1.', shortHash(txid, 10)));
      message = '';
    } catch (e) {
      await inform(t('Send paymail'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  // --- Encrypt and sign ---
  let toolName = $state('');
  let toolText = $state('');
  let signature = $state('');
  let output = $state('');
  let outputIsChain = $state(false);
  let toolBusy = $state(false);
  async function tool(what: 'encrypt' | 'decrypt' | 'sign' | 'verify') {
    const wallet = wallets.current;
    if (wallet === null) return;
    toolBusy = true;
    output = '';
    try {
      if (what === 'encrypt') {
        output = await rpc.raw<string>('encryptnamemessage', [toolName.trim(), toolText], { wallet });
        outputIsChain = true;
      } else if (what === 'decrypt') {
        const text = await walletRpc<string>(wallet, 'decryptnamemessage', [toolText.trim()]);
        if (text === null) return;
        output = text;
        outputIsChain = true;
      } else if (what === 'sign') {
        const sig = await walletRpc<string>(wallet, 'signasname', [toolName.trim(), toolText]);
        if (sig === null) return;
        output = sig;
        outputIsChain = true;
      } else {
        const good = await rpc.raw<boolean>('verifynamesignature', [toolName.trim(), toolText, signature.trim()]);
        output = good ? t('Good: the holder of this name signed this text.') : t('Not a signature of this text by this name.');
        outputIsChain = false;
      }
    } catch (e) {
      await inform(t('Encrypt and sign'), errorText(e), 'error');
    } finally {
      toolBusy = false;
    }
  }
</script>

<div class="page bn-page paymail">
  <header class="page-head"><h1>Paymail</h1></header>

  {#if noWallet}
    <NoWallet page={PAGES.paymail} />
  {:else}
    <TabRow label="Paymail" tabs={TABS} current={tab} select={(id) => setOwnTab('paymail', id)} />

    {#if tab === 'inbox'}
      <section class="panel" aria-label="Inbox">
        <div class="panel-head"><h2>Received</h2><button type="button" class="btn small ghost" onclick={() => loadInbox(true)}><Icon name="refresh" size={14} /> Refresh</button></div>
        {#if inboxState === 'locked'}
          <div class="bn-empty">
            <p>The messages are opened with the keys of the wallet, which is locked.</p>
            <button type="button" class="btn primary" onclick={() => loadInbox(true)}><Icon name="unlock" size={15} /> Unlock and read</button>
          </div>
        {:else if inboxState === 'error'}
          <p class="bn-empty"><SafeText text={inboxError} /></p>
        {:else if inbox.length === 0}
          <p class="bn-empty">{inboxState === 'loading' ? t('Opening the messages…') : t('No paid messages yet. A name takes them once it has an encryption key and a paymail fee (My names, Profile).')}</p>
        {:else}
          <div class="side-table-wrap">
            <table class="side-table" use:stacked aria-label="Paymail received">
              <thead><tr><th>Message</th><th class="end">Paid</th><th class="end">Confirmations</th><th>Transaction</th></tr></thead>
              <tbody>
                {#each inbox as m (m.txid + ':' + m.vout)}
                  <tr>
                    <td class="msg">{#if m.message !== undefined}<SafeText text={m.message} />{:else}<span class="faint">{t('(not for a key of this wallet)')}</span>{/if}</td>
                    <td class="end"><Amount sats={coinsToSats(m.amount)} /></td>
                    <td class="end num">{m.confirmations}</td>
                    <td class="mono" translate="no">{shortHash(m.txid, 8)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/if}
      </section>
    {:else if tab === 'send'}
      <section class="panel">
        <div class="panel-head"><h2>Pay a name, with a message only its holder can read</h2></div>
        <form class="panel-body bn-form narrow" onsubmit={(e) => { e.preventDefault(); send(); }}>
          <label class="field">
            <span class="label">To</span>
            <input id="paymail-to" type="text" class="mono" placeholder="satoshi" bind:value={to} spellcheck="false" autocomplete="off" autocapitalize="off" />
            {#if targetProblem}<span class="error"><SafeText text={targetProblem} /></span>
            {:else if target}<span class="hint"><Domain name={target.name} seq={target.seq} /> · {t('takes paid messages of at least')} <Amount sats={fee} /></span>{/if}
          </label>
          {#key amountKey}<AmountInput id="paymail-amount" label="Pay" bind:value={amount} min={fee ?? undefined} minError={t('The name takes paid messages of this amount at least')} />{/key}
          <label class="field">
            <span class="label">Message</span>
            <textarea id="paymail-text" rows="4" bind:value={message}></textarea>
            <span class="hint" class:error={tooLong}>{t('%1 of %2 bytes, encrypted', memoBytes, MAX_MEMO_BYTES)}</span>
          </label>
          <div class="row"><button type="submit" class="btn primary" id="paymail-send" disabled={busy || !canSend}><Icon name="send" size={15} /> Send</button></div>
        </form>
      </section>
    {:else}
      <section class="panel">
        <div class="panel-head"><h2>Encrypt, open, sign and check</h2></div>
        <div class="panel-body bn-form">
          <label class="field">
            <span class="label">Name</span>
            <input id="tool-name" type="text" class="mono" placeholder="A name: to encrypt to, to sign as (yours), or whose signature to check" bind:value={toolName} spellcheck="false" autocomplete="off" />
          </label>
          <label class="field">
            <span class="label">Text, or an encrypted message</span>
            <textarea id="tool-text" rows="4" class="mono" bind:value={toolText}></textarea>
          </label>
          <label class="field">
            <span class="label">Signature (to check one)</span>
            <input id="tool-signature" type="text" class="mono" bind:value={signature} spellcheck="false" autocomplete="off" />
          </label>
          <div class="row">
            <button type="button" class="btn" disabled={toolBusy || !toolName.trim() || !toolText} onclick={() => tool('encrypt')}><Icon name="lock" size={15} /> Encrypt to the name</button>
            <button type="button" class="btn" disabled={toolBusy || !toolText.trim()} onclick={() => tool('decrypt')}><Icon name="unlock" size={15} /> Open a message</button>
            <button type="button" class="btn" disabled={toolBusy || !toolName.trim() || !toolText} onclick={() => tool('sign')}><Icon name="edit" size={15} /> Sign as the name</button>
            <button type="button" class="btn" disabled={toolBusy || !toolName.trim() || !toolText || !signature.trim()} onclick={() => tool('verify')}><Icon name="shield" size={15} /> Check the signature</button>
          </div>
          {#if output}
            <div class="side-code" role="status">
              <span class="mono">{#if outputIsChain}<SafeText text={output} />{:else}{output}{/if}</span>
              {#if outputIsChain}<CopyButton text={output} label="Copy" />{/if}
            </div>
          {/if}
        </div>
      </section>
    {/if}
  {/if}
</div>

<style>
  .narrow {
    max-width: 40rem;
  }
  .msg {
    white-space: pre-wrap;
    overflow-wrap: anywhere;
    max-width: 40rem;
  }
  .end {
    text-align: right;
  }
</style>
