<script lang="ts">
  // DNS: whether this node answers DNS for .x (-dnsbind) and where; how to send the .x questions of this
  // computer to it, per system, the commands ready to copy; and a lookup tester, which asks the node what its
  // DNS server answers for a domain and a type (resolvename), with the names of the wallet a click away. The DNS
  // chip and help of the Names page of chains-qt, and its resolving of domains in Explore.
  import CopyButton from '../../components/CopyButton.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import TabRow from '../../components/TabRow.svelte';
  import { stacked } from '../../lib/actions';
  import { errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { takeFocus } from '../../lib/nav.svelte';
  import { rpc } from '../../lib/rpc';
  import { app } from '../../lib/state.svelte';
  import { CHAIN } from '../config';
  import '../bn/bn.css';
  import { LOOKUP_TYPES, TLD, type Resolution } from '../bn/model';
  import { bn, startNames } from '../bn/store.svelte';

  $effect(() => startNames());

  const bind = $derived(bn.info?.dns ?? null);
  // Where the questions go: the address the node answers on, or the one the guide suggests.
  const host = $derived(bind ? bind.replace(/:\d+$/, '').replace(/^\[|\]$/g, '') : '127.0.0.1');
  const port = $derived(bind ? (bind.match(/:(\d+)$/)?.[1] ?? '53') : '5335');
  const conf = $derived(`${CHAIN.bin}.conf`);

  const SYSTEMS = [
    { id: 'resolved', label: 'Linux (systemd-resolved)' },
    { id: 'dnsmasq', label: 'Linux (dnsmasq)' },
    { id: 'macos', label: 'macOS' },
    { id: 'windows', label: 'Windows' },
  ];
  let system = $state(app.info?.platform === 'windows' ? 'windows' : 'resolved');
  const commands = $derived.by(() => {
    switch (system) {
      case 'resolved':
        return [`resolvectl dns lo ${host}:${port}`, `resolvectl domain lo ~${TLD}`];
      case 'dnsmasq':
        return [`server=/${TLD}/${host}#${port}`];
      case 'macos':
        return [`sudo mkdir -p /etc/resolver`, `printf 'nameserver ${host}\\nport ${port}\\n' | sudo tee /etc/resolver/${TLD}`];
      default:
        return [`Add-DnsClientNrptRule -Namespace ".${TLD}" -NameServers ${host}`];
    }
  });

  // --- The lookup tester ---
  let domain = $state('');
  let type = $state<(typeof LOOKUP_TYPES)[number]>('ANY');
  let answer = $state<Resolution | null>(null);
  let asked = $state('');
  let error = $state('');
  let busy = $state(false);
  let past = $state<{ domain: string; type: string; status: string }[]>([]);
  $effect(() => {
    const key = takeFocus('dns');
    if (key) {
      domain = key;
      void lookup();
    }
  });
  const mineDomains = $derived(bn.mine.filter((m) => m.kind === 'name' && m.name).map((m) => `${m.name}.${TLD}`).slice(0, 8));

  async function lookup() {
    let d = domain.trim().toLowerCase();
    if (!d) return;
    if (!d.endsWith(`.${TLD}`) && !d.endsWith(`.${TLD}.`) && d !== TLD) d = `${d}.${TLD}`;
    busy = true;
    error = '';
    try {
      answer = await rpc.raw<Resolution>('resolvename', [d, type]);
      asked = `${d} ${type}`;
      past = [{ domain: d, type, status: answer.status }, ...past.filter((p) => !(p.domain === d && p.type === type))].slice(0, 8);
    } catch (e) {
      answer = null;
      error = errorText(e);
    } finally {
      busy = false;
    }
  }

  function statusText(status: string): string {
    switch (status) {
      case 'NOERROR':
        return t('The domain resolves.');
      case 'NXDOMAIN':
        return t('No such domain: no name, subname or record answers for it.');
      case 'SERVFAIL':
        return t('A delegation leads nowhere, or the domain needs more names of the chain than an answer reads.');
      case 'REFUSED':
        return t('Not a domain under .x: the node answers only for .x.');
    }
    return t('The answer of the node.');
  }
</script>

<div class="page bn-page dns">
  <header class="page-head"><h1>DNS</h1></header>

  <section class="panel status-panel" aria-label="The DNS server of this node">
    <div class="panel-body">
      <span class="bn-glyph big" aria-hidden="true"><Icon name="globe" size={26} /></span>
      <div class="grow">
        {#if bind}
          <h2><span class="status ok">{t('On')}</span> {t('This node answers DNS for .%1', TLD)}</h2>
          <p class="muted">{t('On %1, over UDP. Send the .%2 questions of this computer to it (below), then a browser reaches yourname.%2.', bind, TLD)}</p>
        {:else}
          <h2><span class="status bad">{t('Off')}</span> {t('This node does not answer DNS')}</h2>
          <p class="muted">{t('Start it with -dnsbind=127.0.0.1:5335, or put dnsbind=127.0.0.1:5335 in %1, so that .%2 names resolve on this computer.', conf, TLD)}</p>
          <div class="bn-command"><code>dnsbind=127.0.0.1:5335</code><CopyButton text="dnsbind=127.0.0.1:5335" label="Copy" /></div>
        {/if}
      </div>
    </div>
  </section>

  <div class="side-cols">
    <section class="panel">
      <div class="panel-head"><h2>Look up a domain</h2></div>
      <form class="panel-body bn-form" onsubmit={(e) => { e.preventDefault(); lookup(); }}>
        <div class="row">
          <input id="dns-domain" class="grow mono" type="text" placeholder="www.yourname.x" aria-label="Domain" bind:value={domain} spellcheck="false" autocomplete="off" autocapitalize="off" />
          <select aria-label="Record type" bind:value={type} class="type">
            {#each LOOKUP_TYPES as ty (ty)}<option value={ty}>{ty}</option>{/each}
          </select>
          <button type="submit" class="btn primary" id="dns-lookup" disabled={busy || !domain.trim()}><Icon name="search" size={15} /> Look up</button>
        </div>
        {#if mineDomains.length}
          <div class="row picks">
            <span class="muted small">Your names:</span>
            {#each mineDomains as d (d)}<button type="button" class="btn small ghost" onclick={() => { domain = d; void lookup(); }}><SafeText text={d} /></button>{/each}
          </div>
        {/if}
        {#if error}<p class="bn-error" role="alert"><SafeText text={error} /></p>{/if}
        {#if answer}
          <div class="answer" aria-live="polite">
            <p><span class="bn-status" class:ok={answer.status === 'NOERROR'} class:no={answer.status !== 'NOERROR'} translate="no">{answer.status}</span> <span class="mono"><SafeText text={asked} /></span></p>
            <p class="bn-hint">{statusText(answer.status)}</p>
            {#if answer.records.length}
              <div class="side-table-wrap">
                <table class="side-table" use:stacked aria-label="Answer">
                  <thead><tr><th>Name</th><th>Type</th><th>Data</th></tr></thead>
                  <tbody>
                    {#each answer.records as r, i (i)}
                      <tr><td class="mono"><SafeText text={r.name} /></td><td><span class="badge" translate="no">{r.type}</span></td><td class="mono cut"><SafeText text={r.data} /></td></tr>
                    {/each}
                  </tbody>
                </table>
              </div>
            {/if}
          </div>
        {/if}
        {#if past.length > 1}
          <div class="past">
            <span class="muted small">Asked before:</span>
            {#each past.slice(1) as p (p.domain + p.type)}
              <button type="button" class="btn small ghost" onclick={() => { domain = p.domain; type = p.type as (typeof LOOKUP_TYPES)[number]; void lookup(); }}><SafeText text={p.domain} /> <span class="faint" translate="no">{p.type} · {p.status}</span></button>
            {/each}
          </div>
        {/if}
      </form>
    </section>

    <section class="panel">
      <div class="panel-head"><h2>Resolve .x names on this computer</h2></div>
      <div class="panel-body bn-form">
        <TabRow label="System" tabs={SYSTEMS} current={system} select={(id) => (system = id)} />
        {#each commands as c (c)}
          <div class="bn-command"><code>{c}</code><CopyButton text={c} label="Copy the command" /></div>
        {/each}
        <p class="bn-hint">
          {#if system === 'windows'}{t('In PowerShell, as an administrator. Windows sends DNS to port 53 only: start the node with -dnsbind=%1:53.', host)}
          {:else if system === 'dnsmasq'}{t('A line of the configuration of dnsmasq; restart it after.')}
          {:else if system === 'macos'}{t('The file /etc/resolver/%1 sends the .%1 questions to the node.', TLD)}
          {:else}{t('As root. They last until the next restart of systemd-resolved.')}{/if}
        </p>
        <p class="bn-hint">{t('Bind the server to a local address, unless it is meant to serve others. Each source gets at most 20 answers a second.')}</p>
      </div>
    </section>
  </div>
</div>

<style>
  .status-panel .panel-body {
    display: flex;
    gap: 1rem;
    align-items: flex-start;
    padding-top: 1.1rem;
  }
  .status-panel h2 {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.5rem;
    margin: 0 0 0.3rem;
  }
  .bn-glyph.big {
    width: 3rem;
    height: 3rem;
    border-radius: var(--radius-l);
  }
  .type {
    width: auto;
  }
  .picks,
  .past {
    display: flex;
    flex-wrap: wrap;
    align-items: center;
    gap: 0.3rem;
  }
  .answer p {
    margin: 0 0 0.4rem;
  }
  .small {
    font-size: 0.82rem;
  }
</style>
