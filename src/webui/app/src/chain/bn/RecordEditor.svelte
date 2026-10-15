<script lang="ts">
  // The DNS records of a name: a row per record (host, type, value, and the numbers of its type), what each
  // answers for, checked as they are typed, then by the wallet's own rules (bitnames.records.check: the words
  // setnamerecords would refuse them with, and the bytes they take of the 1024 a name holds). Saved in one
  // transaction (setnamerecords); read only for a name of someone else, or records rows cannot stand for.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { request } from '../../lib/bridge';
  import { css } from '../../lib/actions';
  import { ask, errorText, inform, walletRpc } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { rpc } from '../../lib/rpc';
  import { toast, wallets } from '../../lib/state.svelte';
  import { MAX_RECORDS_BYTES, RECORD_TYPES, rowDomain, rowForWallet, rowProblem, rowsFrom, type RecordRow, type RecordType } from './model';
  import { refreshNames } from './store.svelte';

  let {
    handle,
    name,
    records = undefined,
    editable = false,
  }: {
    /** The name as commands take it (0x and its hash). */
    handle: string;
    /** Its text, for the domains each row answers for (null: private). */
    name: string | null;
    /** The records of its data (getname), to know whether rows can stand for them. */
    records?: Record<string, unknown>;
    editable?: boolean;
  } = $props();

  interface EditRow extends RecordRow {
    key: number;
  }
  let nextKey = 1;
  let saved = $state<RecordRow[]>([]);
  let rows = $state<EditRow[]>([]);
  let loading = $state(true);
  let loadError = $state('');
  let rowsKeep = $state(true);
  let check = $state<{ ok: boolean; bytes?: number; error?: string } | null>(null);
  let busy = $state(false);

  const canEdit = $derived(editable && rowsKeep && !loading && !loadError);
  const problems = $derived(rows.map((r) => rowProblem(r)));
  const dirty = $derived(JSON.stringify(rows.map(rowForWallet)) !== JSON.stringify(saved.map(rowForWallet)));
  const bytes = $derived(check?.ok ? (check.bytes ?? 0) : 0);

  async function load() {
    loading = true;
    loadError = '';
    try {
      const got = rowsFrom(await rpc.raw<unknown>('getnamerecords', [handle]));
      saved = got;
      rows = got.map((r) => ({ ...r, key: nextKey++ }));
      rowsKeep = records ? await request<boolean>('host', 'bitnames.records.editable', { records }).catch(() => false) : true;
    } catch (e) {
      loadError = errorText(e);
    } finally {
      loading = false;
    }
  }

  // Read again only when the name or its records on the chain change: a new block that leaves them as they were
  // keeps what is being edited.
  let shown = '';
  $effect(() => {
    const now = `${handle}|${JSON.stringify(records ?? null)}`;
    if (now === shown) return;
    shown = now;
    void load();
  });

  // The wallet's own check, once the typing stops.
  let generation = 0;
  $effect(() => {
    const list = rows.map(rowForWallet);
    const mine = ++generation;
    if (!canEdit) return;
    const timer = setTimeout(async () => {
      try {
        const got = await request<{ ok: boolean; bytes?: number; error?: string }>('host', 'bitnames.records.check', { rows: list });
        if (mine === generation) check = got;
      } catch {
        if (mine === generation) check = null;
      }
    }, 250);
    return () => clearTimeout(timer);
  });

  function add(type: RecordType = 'A', host = '@', value = '', extra: Partial<RecordRow> = {}) {
    rows = [...rows, { key: nextKey++, host, type, value, ...extra }];
  }

  function remove(key: number) {
    rows = rows.filter((r) => r.key !== key);
  }

  function revert() {
    rows = saved.map((r) => ({ ...r, key: nextKey++ }));
  }

  function num(text: string): number | undefined {
    return text.trim() === '' ? undefined : Number(text);
  }

  async function save() {
    const wallet = wallets.current;
    if (wallet === null || !canEdit || problems.some(Boolean) || (check && !check.ok)) return;
    const r = await ask({
      title: t('Save the records'),
      message: rows.length ? t('Save %1 record(s) for this name? They replace the records it has.', rows.length) : t('Remove every record of this name?'),
      informative: t('The change is a transaction of the wallet: the name answers with the new records after the next block.'),
      level: 'question',
      choices: [
        { id: 'no', label: 'Cancel' },
        { id: 'yes', label: 'Save', kind: 'primary' },
      ],
    });
    if (r.choice !== 'yes') return;
    busy = true;
    try {
      const txid = await walletRpc<string>(wallet, 'setnamerecords', [handle, rows.map(rowForWallet)]);
      if (txid === null) return;
      saved = rows.map(({ key: _key, ...rest }) => rest);
      toast('success', t('Records saved'), t('They answer after the next block.'));
      void refreshNames();
    } catch (e) {
      await inform(t('Save the records'), errorText(e), 'error');
    } finally {
      busy = false;
    }
  }
</script>

<div class="bn-records">
  {#if loading}
    <p class="bn-hint">Reading the records…</p>
  {:else if loadError}
    <p class="bn-error"><SafeText text={loadError} /></p>
  {:else}
    {#if editable && !rowsKeep}
      <p class="side-warn">These records hold more than rows can show (written by hand as JSON): they are shown here, and changed with setnamerecords or updatename in the console.</p>
    {/if}
    {#if rows.length === 0}
      <p class="bn-hint">
        {#if canEdit}No records: the name answers with its IPv4 and IPv6 addresses, if it has them. Add one below.{:else}No records.{/if}
      </p>
    {/if}
    {#each rows as row, i (row.key)}
      <div class="bn-record" class:invalid={!!problems[i] && canEdit} role="group" aria-label={t('Record %1', i + 1)}>
        {#if canEdit}
          <input type="text" class="mono" aria-label="Host" placeholder="@" bind:value={row.host} spellcheck="false" autocomplete="off" />
          <select aria-label="Type" bind:value={row.type}>
            {#each RECORD_TYPES as ty (ty)}<option value={ty}>{ty}</option>{/each}
          </select>
          <input type="text" class="mono value" aria-label="Value" bind:value={row.value} spellcheck="false" autocomplete="off" placeholder={row.type === 'A' ? '203.0.113.7' : row.type === 'AAAA' ? '2001:db8::7' : row.type === 'TLSA' ? '3 1 1 ab12…' : row.type === 'DELEGATE' ? 'othername' : row.type === 'TXT' ? 'v=spf1 -all' : 'mail'} />
          <span class="nums">
            {#if row.type === 'MX' || row.type === 'SRV'}
              <input type="number" min="0" max="65535" aria-label="Priority" title="Priority" placeholder="10" value={row.priority ?? ''} oninput={(e) => (row.priority = num(e.currentTarget.value))} />
            {/if}
            {#if row.type === 'SRV'}
              <input type="number" min="0" max="65535" aria-label="Weight" title="Weight" placeholder="0" value={row.weight ?? ''} oninput={(e) => (row.weight = num(e.currentTarget.value))} />
              <input type="number" min="0" max="65535" aria-label="Port" title="Port" placeholder="0" value={row.port ?? ''} oninput={(e) => (row.port = num(e.currentTarget.value))} />
            {/if}
          </span>
          <button type="button" class="btn icon small ghost" aria-label="Remove this record" title="Remove this record" onclick={() => remove(row.key)}><Icon name="trash" size={15} /></button>
        {:else}
          <span class="mono"><SafeText text={row.host || '@'} /></span>
          <span class="badge">{row.type}</span>
          <span class="mono value"><SafeText text={row.value} /></span>
          <span class="nums num">{#if row.priority !== undefined}{row.priority}{/if}{#if row.type === 'SRV'} · {row.weight ?? 0} · {row.port ?? 0}{/if}</span>
          <span></span>
        {/if}
        {#if problems[i] && canEdit}
          <span class="problem">{problems[i]}</span>
        {:else if name}
          <span class="answers">{t('Answers for')} <SafeText text={rowDomain(row, name)} /></span>
        {/if}
      </div>
    {/each}

    {#if canEdit}
      <div class="row">
        <button type="button" class="btn small" onclick={() => add('A', rows.length ? '' : '@')}><Icon name="plus" size={14} /> Add a record</button>
        <button type="button" class="btn small ghost" onclick={() => add('CNAME', 'www', '@')}>www → the name</button>
        <button type="button" class="btn small ghost" onclick={() => add('MX', '@', 'mail', { priority: 10 })}>Mail server</button>
        <button type="button" class="btn small ghost" onclick={() => add('TXT', '@', '')}>Text</button>
      </div>
      <div class="bn-meter">
        <span>{t('%1 of %2 bytes', bytes, MAX_RECORDS_BYTES)}</span>
        <div class="bn-bar" aria-hidden="true"><span use:css={{ width: `${Math.min(100, (bytes / MAX_RECORDS_BYTES) * 100)}%` }}></span></div>
      </div>
      {#if check && !check.ok && !problems.some(Boolean)}<p class="bn-error" role="alert"><SafeText text={check.error ?? ''} /></p>{/if}
      <div class="row">
        <button type="button" class="btn primary" id="records-save" disabled={busy || !dirty || problems.some(Boolean) || (check !== null && !check.ok)} onclick={save}><Icon name="check" size={15} /> Save the records</button>
        <button type="button" class="btn ghost" disabled={busy || !dirty} onclick={revert}>Revert</button>
        <span class="bn-hint">Host @ is the name itself, www is www under it, * anything under it. A value without a final dot is under the name.</span>
      </div>
    {/if}
  {/if}
</div>
