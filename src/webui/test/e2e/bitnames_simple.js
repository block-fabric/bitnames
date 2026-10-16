// BitNames in Simple mode (run_sidechain.py --script bitnames_simple.js --js-var SIMPLE=true): the default of
// a first start. The wallet gets coins; another wallet takes "hal" by its RPC; then Get a name takes "satoshi"
// through the page, in plain words, and My names shows it in a few words, its records behind More. Pictures in
// the Midnight and Light themes: Home, Get a name (a free name, one taken, one already yours), My names.
(async () => {
  const { host, rpc, sleep, T, waitFor, check, snapshot, text, finish } = E2E;
  const NAME = 'satoshi';
  const TAKEN = 'hal';
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(400);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
    }
    await sleep(500);
    await snapshot(name);
  }
  const getname = (n) => rpc('getname', [n]).catch(() => null);

  try {
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await check('Simple mode is the default of a first start', T().prefs.uiMode === 'simple', T().prefs.uiMode);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);

    // --- Coins, and a name taken by someone else ---
    await rpc('createwallet', ['bn']);
    await rpc('createwallet', ['other']);
    T().selectWallet('bn');
    await waitFor('the wallet in the page', () => T().wallets.current === 'bn');
    const deposit = await rpc('getdepositaddress', [], 'bn');
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], 'bn'), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], 'bn')).mine.trusted >= 20, 240000);
    await rpc('sendtoaddress', [await rpc('getnewaddress', [], 'other'), 2], 'bn');
    await waitFor('coins in the other wallet', async () => (await rpc('getbalances', [], 'other')).mine.trusted >= 2, 120000);
    await rpc('reservename', [TAKEN], 'other');
    await waitFor(`${TAKEN} registered by the other wallet`, async () => {
      if (await getname(TAKEN)) return true;
      await rpc('registername', [TAKEN, {}, true], 'other').catch(() => null);
      await sleep(2000);
      return !!(await getname(TAKEN));
    }, 240000);
    await check('a name taken by another wallet', true);
    T().savePrefs({ layout: 'dashboard', tipsDone: false });

    for (const theme of ['midnight', 'light']) {
      T().savePrefs({ theme });
      await go('home');
      await waitFor('the first steps and the cards of BitNames', () => $('.tips') && text('.home').includes('Coming up'), 30000);
      await check(`${theme}: the home without the names registered last`, !text('.home').includes('Registered last'));
      const sidebar = text('nav.sidebar .scroll');
      await check(`${theme}: Get a name first, no directory, paymail nor DNS`, sidebar.indexOf('Get a name') >= 0 && sidebar.indexOf('Get a name') < sidebar.indexOf('My names') && !/Paymail|Name directory|DNS/.test(sidebar), sidebar);
      await shot(`bn-simple-home-${theme}`);

      await go('register');
      const search = await waitFor('the search', () => $('#name-search'));
      set(search, theme === 'midnight' ? NAME : 'nakamoto');
      await waitFor('the name free', () => $('.bn-verdict.free') && text('.register').includes('What it costs'), 20000);
      await check(`${theme}: what it costs, no address to type`, !$('#name-ipv4'));
      await shot(`bn-simple-register-free-${theme}`);
      if (theme === 'midnight') {
        $('#name-get').click();
        const d = await waitFor('the confirmation', () => openDialog());
        button('Get it', d).click();
        await waitFor('the registration on its way', () => $$(`[aria-label="Registration of ${NAME}.x"]`).length === 1, 30000);
        await waitFor('the name yours', () => /Yours since block/.test(text(`[aria-label="Registration of ${NAME}.x"]`)), 300000);
        await check('the name taken through Simple mode', !!(await getname(NAME)));
      }
      set(search, TAKEN);
      await waitFor('the name taken', () => text('.register').includes('Taken'), 20000);
      await shot(`bn-simple-register-taken-${theme}`);
      set(search, NAME);
      await waitFor('the name already yours', () => text('.register').includes('Already yours'), 20000);
      await shot(`bn-simple-register-yours-${theme}`);

      await go('names');
      const view = () => $('section[aria-label="A name"]');
      await waitFor('the name in My names, in a few words', () => view() && text('section[aria-label="A name"]').includes('yours for good'), 30000);
      await check(`${theme}: the records behind More`, !$('[role="tab"]', view()));
      await shot(`bn-simple-names-${theme}`);
    }
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
