<script lang="ts">
  // A name, whole: its domain, what it is (a name or a subname, its number, ICANN, public or private), who
  // holds it and since when; then its records, its profile, its subnames, giving it away or releasing it (a
  // name of this wallet) and every value it has had. My names shows the names of the wallet here, editable;
  // the directory any name, read only.
  import AddressInput from '../../components/AddressInput.svelte';
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { stacked } from '../../lib/actions';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { goTo } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import Domain from './Domain.svelte';
  import ProfileForm from './ProfileForm.svelte';
  import RecordEditor from './RecordEditor.svelte';
  import { handleOf, isLabel, isPunycode, type NameHistory, type NameInfo } from './model';
  import { bn, noteChange, refreshNames } from './store.svelte';
  import { simpleMode } from '../../lib/uimode.svelte';

  let {
    handle,
    editable = false,
    knownText = undefined,
    tab = $bindable('records'),
    ongone = undefined,
  }: {
    /** The name as commands take it: 0x and its hash. */
    handle: string;
    /** A name of this wallet: its records, profile and subnames can be changed, it can be given or released. */
    editable?: boolean;
    /** Its text as the wallet knows it (listmynames), when the chain keeps it private. */
    knownText?: string;
    tab?: string;
    /** Called once it is given away or released. */
    ongone?: () => void;
  } = $props();

  let info = $state<NameInfo | null>(null);
  let error = $state('');
  let loading = $state(true);
  let parentInfo = $state<NameInfo | null>(null);

  const text = $derived(info?.name ?? knownText ?? null);
  // Simple mode (lib/uimode.svelte.ts): the name in a few plain words; its records, profile, keys, subnames and
  // history behind "More" (Advanced shows them at once).
  const simple = $derived(simpleMode());
  let more = $state(false);
  const showMore = $derived(!simple || more);
  const TABS = $derived([
    { id: 'records', label: 'Records' },
    { id: 'profile', label: 'Profile' },
    { id: 'subnames', label: 'Subnames' },
    ...(editable ? [{ id: 'transfer', label: 'Give or release' }] : []),
    { id: 'history', label: 'History' },
  ]);

  async function load() {
    loading = true;
    error = '';
    try {
      info = await rpc.raw<NameInfo>('getname', [handle]);
      parentInfo = info?.parent ? await rpc.raw<NameInfo>('getname', [handleOf(info.parent)]).catch(() => null) : null;
    } catch (e) {
      info = null;
      error = errorText(e);
    } finally {
      loading = false;
    }
  }

  $effect(() => {
    void handle;
    void bn.version;
    void load();
  });

  // --- Subnames ---
  let subnames = $state<NameInfo[]>([]);
  let subLoaded = $state(false);
  /** Subnames listed at most; past that the tab says the list is cut. */
  const SUBNAMES_SHOWN = 2000;
  let subsCut = $state(false);
  async function loadSubnames() {
    if (!info) return;
    const hash = info.namehash;
    // The node walks the registry for them (listnames with its parent, one call): none left out, however many
    // names the chain has (audit port-2 #11: read here 1000 at a time, they stopped at 10000).
    const page = await rpc.raw<NameInfo[]>('listnames', [SUBNAMES_SHOWN, 0, null, handleOf(hash)]).catch(() => [] as NameInfo[]);
    if (!info || info.namehash !== hash) return;
    subnames = Array.isArray(page) ? page.filter((n) => n.parent === hash) : [];
    subsCut = subnames.length >= SUBNAMES_SHOWN;
    subLoaded = true;
  }
  $effect(() => {
    if (tab === 'subnames' && info) void loadSubnames();
  });

  let label = $state('');
  let holder = $state('');
  let holderValid = $state(false);
  let holderKey = $state(0);
  let busy = $state(false);
  const labelProblem = $derived(
    label.trim() && !isLabel(label.trim())
      ? t('A label is 1 to 63 lower-case letters, digits and hyphens, not at either end.')
      : isPunycode(label.trim())
        ? t('A name starting with xn-- can show in browsers as letters of another script, a look-alike of another name: the wallet does not make it.')
        : '',
  );

  async function createSubname() {
    const wallet = wallets.current;
    const l = label.trim();
    if (wallet === null || !info || !l || labelProblem || (holder.trim() && !holderValid)) return;
    const shown = text ? `${l}.${text}.x` : l;
    const r = await ask({
      title: t('Make a subname'),
      message: holder.trim() ? t('Make %1 and give it to this address?', shown) : t('Make %1, held by this wallet?', shown),
      code: holder.trim() || undefined,
      informative: t('A subname depends on its parent: if the parent is released, it is released with it. Whoever holds it sets its records and gives it on.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Make it', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const done = await walletRpc<{ txid: string; namehash: string }>(wallet, 'createsubname', holder.trim() ? [handle, l, holder.trim()] : [handle, l]);
      if (!done) return;
      noteChange(wallet, done.txid, handle.slice(2));
      toast('success', t('Subname on its way'), t('%1 exists after the next block.', shown));
      label = '';
      holder = '';
      holderKey++;
      void refreshNames();
    } catch (e) {
      await inform(t('Make a subname'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  // --- Give or release ---
  let to = $state('');
  let toValid = $state(false);
  let toKey = $state(0);
  let clearKeys = $state(true);

  async function give() {
    const wallet = wallets.current;
    if (wallet === null || !toValid) return;
    const r = await ask({
      title: t('Give the name'),
      message: t('Give %1 to this address? Only its new holder can give it back.', text ? `${text}.x` : `#${info?.seq}`),
      code: to.trim(),
      informative: clearKeys
        ? t('Its encryption key, signing key and paymail fee are deleted in the same transaction: they are keys of this wallet.')
        : t('Its keys are kept: messages to the name still come to this wallet, and it can still sign as the name, until the new holder changes them.'),
      level: 'warning',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Give it', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const txid = await walletRpc<string>(wallet, 'transfername', [handle, to.trim(), clearKeys]);
      if (txid === null) return;
      noteChange(wallet, txid, handle.slice(2), true);
      toast('success', t('Name given'), t('It goes to its new holder with the next block.'));
      to = '';
      toKey++;
      void refreshNames();
      ongone?.();
    } catch (e) {
      await inform(t('Give the name'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  async function release() {
    const wallet = wallets.current;
    if (wallet === null) return;
    const r = await ask({
      title: t('Release the name'),
      message: t('Give up %1 for good?', text ? `${text}.x` : `#${info?.seq}`),
      informative: t('Its records go with it, and so do all its subnames, at any depth, whoever holds them. Anyone can register it again. This cannot be undone.'),
      level: 'warning',
      checkbox: { label: 'I understand that this cannot be undone', checked: false },
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Release', kind: 'danger' },
      ],
    });
    if (r.choice !== 'yes') return;
    if (!r.checked) {
      await inform(t('Release the name'), t('Nothing was released: tick the box to say you understand that it cannot be undone.'), 'info');
      return;
    }
    busy = true;
    try {
      const txid = await walletRpc<string>(wallet, 'releasename', [handle, true]);
      if (txid === null) return;
      noteChange(wallet, txid, handle.slice(2), true);
      toast('success', t('Name released'), t('It leaves the registry with the next block.'));
      void refreshNames();
      ongone?.();
    } catch (e) {
      await inform(t('Release the name'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }

  // --- History ---
  function describeField(field: string): string {
    switch (field) {
      case 'commitment':
        return t('Commitment');
      case 'ipv4':
        return t('IPv4 address');
      case 'ipv6':
        return t('IPv6 address');
      case 'encryptionkey':
        return t('Encryption key');
      case 'signingkey':
        return t('Signing key');
      case 'paymailfee':
        return t('Paymail fee');
      case 'records':
        return t('Records');
    }
    return t('Field');
  }
  let history = $state<{ field: string; value: string; txid: string; height: number }[]>([]);
  async function loadHistory() {
    try {
      const h = await rpc.raw<NameHistory>('getnamehistory', [handle]);
      history = Object.entries(h ?? {})
        .flatMap(([field, list]) => (Array.isArray(list) ? list : []).map((e) => ({ field, value: e.value === null ? '' : typeof e.value === 'string' ? e.value : JSON.stringify(e.value), txid: e.txid, height: e.height })))
        .sort((a, b) => b.height - a.height);
    } catch {
      history = [];
    }
  }
  $effect(() => {
    if (tab === 'history') void loadHistory();
  });
</script>

<section class="panel" aria-label="A name">
  {#if loading && !info}
    <p class="bn-empty">Reading the name…</p>
  {:else if error || !info}
    <p class="bn-empty"><SafeText text={error} /></p>
  {:else}
    <header class="bn-head">
      <div>
        <Domain name={text} seq={info.seq} big />
        <div class="tags">
          <span class="badge accent">{info.parent ? t('Subname') : t('Name')}</span>
          {#if !simple}<span class="badge" translate="no">#{info.seq}</span>{/if}
          <span class="badge">{info.name ? t('Public') : t('Private')}</span>
          {#if info.icann}<span class="badge warning">ICANN <SafeText text={info.icann} /></span>{/if}
          <span class="badge" title={t('Names never expire: they are held until given away or released.')}>{t('Permanent')}</span>
        </div>
      </div>
      <dl class="facts small">
        {#if !simple}
          <dt>Held by</dt>
          <dd>{#if info.owner.address}<span class="mono"><SafeText text={info.owner.address} /></span> <CopyButton text={info.owner.address} label="Copy the address" />{:else}–{/if}</dd>
        {/if}
        <dt>Registered</dt>
        <dd class="num">{t('in block %1', info.registered)}</dd>
        {#if !simple}
          <dt>Hash</dt>
          <dd><span class="mono" title={info.namehash} translate="no">{shortHash(info.namehash, 10)}</span> <CopyButton text={handleOf(info.namehash)} label="Copy the hash" /></dd>
        {/if}
        {#if info.parent}
          <dt>Under</dt>
          <dd>{#if parentInfo}<Domain name={parentInfo.name} seq={parentInfo.seq} />{:else}<span class="mono" translate="no">{shortHash(info.parent, 10)}</span>{/if}</dd>
        {/if}
      </dl>
    </header>
    {#if bn.inflight.some((x) => x.wallet === wallets.current && x.name.namehash === info?.namehash)}
      <p class="side-warn bn-subnote" role="status">A change of this name waits for the next block: what it shows is the name as it is in the chain now, and the next change can be sent once that one is in a block.</p>
    {/if}
    {#if info.parent}
      <p class="side-note bn-subnote">A subname depends on its parent: if the holder of the parent releases it, this subname is released with it, and a delegation in the records of the parent takes its domain elsewhere. Only its holder updates it, gives it on or makes subnames of it.</p>
    {/if}
    <div class="panel-body">
      {#if showMore}<TabRow label="The name" tabs={TABS} current={tab} select={(id) => (tab = id)} />{/if}

      {#if !showMore}
        <div class="bn-simple">
          <p>{editable ? t('This name is yours for good: there is nothing to renew. It can point to your servers, hold your keys and take paid messages.') : t('This name is registered: its holder keeps it for good.')}</p>
          <div class="row">
            <button type="button" class="btn" onclick={() => (more = true)}>More: records, profile and keys, subnames</button>
            {#if editable}
              <button type="button" class="btn ghost" onclick={() => { more = true; tab = 'transfer'; }}>Give it away…</button>
            {/if}
          </div>
        </div>
      {:else if tab === 'records'}
        <RecordEditor {handle} name={text} records={info.data?.records} {editable} />
      {:else if tab === 'profile'}
        <ProfileForm {handle} data={info.data} {editable} />
        {#if !editable && info.data?.encryptionkey}
          <div class="row bn-gap"><button type="button" class="btn" onclick={() => goTo('paymail', 'send', handle)}><Icon name="send" size={15} /> Pay this name with a message</button></div>
        {/if}
      {:else if tab === 'subnames'}
        {#if !subLoaded}
          <p class="bn-hint">Looking through the registry…</p>
        {:else if subnames.length === 0}
          <p class="bn-hint">No subnames.</p>
        {:else}
          <div class="side-table-wrap">
            <table class="side-table" use:stacked aria-label="Subnames">
              <thead><tr><th>Subname</th><th>Number</th><th>Held by</th></tr></thead>
              <tbody>
                {#each subnames as s (s.namehash)}
                  <tr>
                    <td>{#if s.label && text}<Domain name={`${s.label}.${text}`} />{:else}<Domain name={s.name} seq={s.seq} />{/if}</td>
                    <td class="num" translate="no">#{s.seq}</td>
                    <td class="mono cut"><SafeText text={s.owner.address ?? '–'} /></td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
          {#if subsCut}<p class="bn-hint">{t('The first %1 subnames are listed; there are more.', String(SUBNAMES_SHOWN))}</p>{/if}
        {/if}
        {#if editable}
          <form class="bn-form bn-gap" onsubmit={(e) => { e.preventDefault(); createSubname(); }}>
            <h3>Make a subname</h3>
            <div class="bn-fields">
              <label class="field">
                <span class="label">Label</span>
                <input id="subname-label" type="text" class="mono" placeholder="blog" spellcheck="false" autocomplete="off" bind:value={label} aria-invalid={labelProblem ? 'true' : undefined} />
                {#if labelProblem}<span class="error">{labelProblem}</span>{:else if label.trim() && text}<span class="hint">{t('It will be %1', `${label.trim()}.${text}.x`)}</span>{/if}
              </label>
              {#key holderKey}<AddressInput id="subname-holder" label="Held by (empty: this wallet)" bind:value={holder} bind:valid={holderValid} required={false} />{/key}
            </div>
            <div class="row"><button type="submit" class="btn primary" id="subname-create" disabled={busy || !label.trim() || !!labelProblem || (!!holder.trim() && !holderValid)}><Icon name="plus" size={15} /> Make the subname</button></div>
          </form>
        {/if}
      {:else if tab === 'transfer' && editable}
        <form class="bn-form" onsubmit={(e) => { e.preventDefault(); give(); }}>
          <h3>Give it to someone</h3>
          {#key toKey}<AddressInput id="give-to" label="To" bind:value={to} bind:valid={toValid} />{/key}
          <label class="check"><input type="checkbox" bind:checked={clearKeys} /> <span>Delete its encryption key, signing key and paymail fee (they are this wallet's)</span></label>
          <div class="row"><button type="submit" class="btn" id="give-send" disabled={busy || !toValid}><Icon name="send" size={15} /> Give the name</button></div>
        </form>
        <div class="bn-danger bn-gap">
          <h3>Release it</h3>
          <p class="bn-hint">The name leaves the registry with its data, and anyone can register it again. Its subnames go with it, at any depth, whoever holds them.</p>
          <button type="button" class="btn danger" id="release-name" disabled={busy} onclick={release}><Icon name="trash" size={15} /> Release for good…</button>
        </div>
      {:else if tab === 'history'}
        {#if history.length === 0}
          <p class="bn-hint">No values set yet.</p>
        {:else}
          <div class="side-table-wrap">
            <table class="side-table" use:stacked aria-label="History">
              <thead><tr><th>Block</th><th>Field</th><th>Value</th><th>Transaction</th></tr></thead>
              <tbody>
                {#each history as h, i (i)}
                  <tr>
                    <td class="num">{h.height}</td>
                    <td>{describeField(h.field)}</td>
                    <td class="mono cut">{#if h.value}<SafeText text={h.value} />{:else}<span class="faint">{t('deleted')}</span>{/if}</td>
                    <td class="mono" translate="no">{shortHash(h.txid, 8)}</td>
                  </tr>
                {/each}
              </tbody>
            </table>
          </div>
        {/if}
      {/if}
    </div>
  {/if}
</section>

<style>
  .bn-simple {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .bn-simple p {
    margin: 0;
  }
  .bn-subnote {
    margin: 0 1.3rem 0.4rem;
  }
  .bn-gap {
    margin-top: 1rem;
  }
  .small {
    font-size: 0.88rem;
  }
  h3 {
    margin: 0;
    font-size: 1rem;
  }
</style>
