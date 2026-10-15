<script lang="ts">
  // The data of a name besides its records: where it points (an IPv4 and an IPv6 address, with a port or not),
  // a commitment to data kept elsewhere, the keys messages to the owner are encrypted to and signed with, and
  // the least a paid message pays. Editable for a name of this wallet (updatename: a field emptied is deleted),
  // read only otherwise.
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t, tn } from '../../lib/i18n.svelte';
  import { toast, wallets } from '../../lib/state.svelte';
  import { DATA_FIELDS, dataProblem, dataText, dataUpdates, type DataField, type NameData } from './model';
  import { noteChange, refreshNames } from './store.svelte';

  let { handle, data, editable = false, onsaved = undefined }: { handle: string; data: NameData | undefined; editable?: boolean; onsaved?: () => void } = $props();

  const LABELS: Record<DataField, string> = {
    ipv4: 'IPv4 address',
    ipv6: 'IPv6 address',
    commitment: 'Commitment (a hash of data kept elsewhere)',
    encryptionkey: 'Messages are encrypted to',
    signingkey: 'Messages are signed with',
    paymailfee: 'Paid messages pay at least',
  };
  const HINTS: Record<DataField, string> = {
    ipv4: 'The A record of the name, unless its records have one. With a port or not: 203.0.113.7:443',
    ipv6: 'The AAAA record of the name, unless its records have one: 2001:db8::7 or [2001:db8::7]:443',
    commitment: '64 hex digits: the SHA-256 of a zone file, a document, a key…',
    encryptionkey: 'A public key of this wallet (Use new keys): paid messages are encrypted to it',
    signingkey: 'An x-only key of this wallet: whoever checks a signature of the name checks it with this key',
    paymailfee: 'Empty: the name takes no paid messages',
  };

  let form = $state<Record<DataField, string>>(Object.fromEntries(DATA_FIELDS.map((f) => [f, ''])) as Record<DataField, string>);
  // Filled again only when the data of the chain changes (a new block that leaves it as it was keeps what is typed).
  let shown = '';
  $effect(() => {
    const now = JSON.stringify(data ?? {});
    if (now === shown) return;
    shown = now;
    form = Object.fromEntries(DATA_FIELDS.map((f) => [f, dataText(data, f)])) as Record<DataField, string>;
  });
  const problems = $derived(Object.fromEntries(DATA_FIELDS.map((f) => [f, dataProblem(f, form[f])])) as Record<DataField, string>);
  const changes = $derived(dataUpdates(data, form));
  const changed = $derived(Object.keys(changes).length > 0);
  const bad = $derived(DATA_FIELDS.some((f) => problems[f]));
  let busy = $state(false);

  async function newKeys() {
    const wallet = wallets.current;
    if (wallet === null) return;
    try {
      const keys = await walletRpc<{ encryptionkey: string; signingkey: string }>(wallet, 'getnewnamekeys', []);
      if (!keys) return;
      form.encryptionkey = keys.encryptionkey;
      form.signingkey = keys.signingkey;
    } catch (e) {
      await inform(t('New keys'), errorText(e), 'error');
    }
  }

  async function save() {
    const wallet = wallets.current;
    if (wallet === null || !changed || bad) return;
    const deleted = Object.entries(changes).filter(([, v]) => v === null).length;
    const r = await ask({
      title: t('Save the profile'),
      message: tn('Change %n field(s) of this name?', Object.keys(changes).length),
      informative: deleted ? tn('%n field(s) emptied are deleted from the name.', deleted) : t('The change is a transaction of the wallet: it shows after the next block.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Save', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const txid = await walletRpc<string>(wallet, 'updatename', [handle, changes]);
      if (txid === null) return;
      noteChange(wallet, txid, handle.slice(2));
      toast('success', t('Profile saved'), t('It shows after the next block.'));
      void refreshNames();
      onsaved?.();
    } catch (e) {
      await inform(t('Save the profile'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

{#if editable}
  <form class="bn-form" onsubmit={(e) => { e.preventDefault(); save(); }}>
    <div class="bn-fields">
      {#each DATA_FIELDS as f (f)}
        <label class="field">
          <span class="label">{t(LABELS[f])}</span>
          <input id={`profile-${f}`} type="text" class:mono={f !== 'paymailfee'} spellcheck="false" autocomplete="off" bind:value={form[f]} aria-invalid={problems[f] ? 'true' : undefined} />
          {#if problems[f]}<span class="error">{problems[f]}</span>{:else}<span class="hint">{t(HINTS[f])}</span>{/if}
        </label>
      {/each}
    </div>
    <div class="row">
      <button type="submit" class="btn primary" id="profile-save" disabled={busy || !changed || bad}><Icon name="check" size={15} /> Save the profile</button>
      <button type="button" class="btn" onclick={newKeys}><Icon name="key" size={15} /> Use new keys of this wallet</button>
      {#if changed}<span class="bn-hint">{tn('%n field(s) changed', Object.keys(changes).length)}</span>{/if}
    </div>
  </form>
{:else}
  <dl class="facts">
    {#each DATA_FIELDS as f (f)}
      <dt>{t(LABELS[f])}</dt>
      <dd>
        {#if dataText(data, f)}<span class="mono"><SafeText text={dataText(data, f)} /></span> <CopyButton text={dataText(data, f)} label="Copy" />{:else}<span class="faint">–</span>{/if}
      </dd>
    {/each}
  </dl>
{/if}
