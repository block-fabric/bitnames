// BitNames against a live mainchain (run_sidechain.py --script bitnames.js): the names driven through the
// pages as a user would. The wallet gets coins (a deposit by the helper of the mainchain wallet, merged
// mining on). Then: Get a name finds a free one and takes it, the app following the reservation, the blocks it
// stays hidden and the registration by itself; My names sets its records (checked by the wallet's rules), its
// profile (new keys, a paymail fee) and makes a subname; Paymail pays the name a message and reads it in the
// inbox; DNS shows the server of the node (-dnsbind) and looks a domain up; the directory finds the name; the
// subname is given to another wallet. Pictures of every page of BitNames on the way, in the layouts and looks.
(async () => {
  const { host, rpc, sleep, log, T, waitFor, check, snapshot, text, finish } = E2E;
  const NAME = 'satoshi';
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];
  const set = (el, value) => {
    el.value = value;
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  };
  const openDialog = () => $$('dialog[open]').pop();
  const button = (label, root = document) => $$('button', root).find((b) => b.textContent.trim().replace(/\s+/g, ' ').startsWith(label));
  async function click(label, root) {
    const b = await waitFor(`the button "${label}"`, () => {
      const el = button(label, typeof root === 'function' ? root() : root);
      return el && !el.disabled ? el : null;
    });
    b.click();
    await sleep(150);
  }
  async function clickId(id, ms = 30000) {
    const b = await waitFor(`#${id} enabled`, () => {
      const el = document.getElementById(id);
      return el && !el.disabled ? el : null;
    }, ms);
    b.click();
    await sleep(150);
  }
  async function answerDialog(title, label) {
    const d = await waitFor(`the dialog "${title}"`, () => {
      const el = openDialog();
      return el && $('h2', el) && $('h2', el).textContent.trim() === title ? el : null;
    });
    await click(label, d);
    await waitFor(`the dialog "${title}" to go`, () => openDialog() !== d || !d.open);
  }
  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(350);
  }
  async function shot(name) {
    const overlay = $$('dialog[open]').find((d) => /Synchronizing/.test(d.textContent));
    if (overlay) {
      const hide = button('Hide', overlay);
      if (hide) hide.click();
      await sleep(300);
    }
    await snapshot(name);
  }
  const getname = (n) => rpc('getname', [n]).catch(() => null);

  try {
    // What pays or signs is confirmed in the dialog of the host (audit 7 #1): every question accepted here, checked at the end.
    await host('test.confirmAnswer', { always: true });
    await waitFor('the node to run', () => T() && T().app.phase === 'ready' && T().node.tip, 120000);
    await waitFor('the node to reach the mainchain', async () => (await rpc('getmainchaininfo')).connected, 60000);
    await check('BitNames, the dashboard and Midnight by default', T().app.info.name === 'BitNames' && T().prefs.layout === 'dashboard' && (T().prefs.theme ?? 'midnight') === 'midnight', `${T().app.info.name} ${T().prefs.layout}`);

    // --- A wallet with coins ---
    await rpc('createwallet', ['bn']);
    await waitFor('the wallet in the page', () => T().wallets.current === 'bn');
    const deposit = await rpc('getdepositaddress', [], 'bn');
    // The wallet of the mainchain node pays: on the desktop's word (audit port-1 #4), accepted here.
    await host('test.confirmAnswer', { yes: true });
    await host('peg.deposit', { destination: deposit.depositaddress, amount: '20' });
    await rpc('setbmm', [true, await rpc('getnewaddress', [], 'bn'), true]);
    await waitFor('the deposit credited', async () => (await rpc('getbalances', [], 'bn')).mine.trusted >= 20, 240000);
    await check('coins in the wallet', true);

    await go('home');
    await waitFor('the cards of BitNames on the home', () => text('.home').includes('Coming up') && text('.home').includes('Registered last'), 30000);
    await sleep(1200);
    await shot('bn-home-empty');
    await go('names');
    await sleep(800);
    await shot('bn-names-empty');

    // --- Get a name: free, then reserved, hidden, revealed and registered by the app ---
    await go('register');
    set(await waitFor('the search', () => $('#name-search')), NAME);
    set(await waitFor('the name free', () => $('.bn-verdict.free') && $('#name-ipv4'), 20000), '203.0.113.7');
    await sleep(300);
    await shot('bn-register-free');
    await clickId('name-get');
    await answerDialog('Get the name', 'Get it');
    await waitFor('the registration on its way', () => $$('[aria-label="Registration of satoshi.x"]').length === 1, 30000);
    await check('the reservation sent: the name hidden', (await rpc('listmynames', [], 'bn')).some((m) => m.kind === 'reservation') || (await rpc('getmempoolinfo')).size > 0);
    const reg = '[aria-label="Registration of satoshi.x"]';
    await waitFor('the reservation in a block', () => /Hidden for|Deep enough|Revealed:|Yours since/.test(text(reg)), 120000);
    await shot('bn-register-progress');
    const registered = await waitFor('the name registered by the app', async () => {
      const n = await getname(NAME);
      return n && (await rpc('listmynames', [], 'bn')).some((m) => m.kind === 'name' && m.namehash === n.namehash) ? n : null;
    }, 240000);
    await check('the name taken in two steps, its address set, public', registered.name === NAME && registered.data.ipv4 === '203.0.113.7', JSON.stringify(registered));
    await waitFor('the page says it is yours', () => /Yours since block/.test(text(reg)), 60000);
    await sleep(600);
    await shot('bn-register-done');
    // A taken name, said as it is typed.
    set($('#name-search'), NAME);
    await waitFor('the name already yours', () => $('.bn-verdict.free') && text('.register').includes('Already yours'), 20000);

    // --- My names: records, profile, a subname ---
    await go('names', 'records');
    const view = () => $('section[aria-label="A name"]');
    await waitFor('the name in My names', () => view() && text('section[aria-label="A name"]').includes(NAME), 30000);
    await click('Add a record', view);
    await sleep(200);
    let rows = $$('.bn-record', view());
    set($('input[aria-label="Value"]', rows[rows.length - 1]), '2001:db8::7');
    await waitFor('the row refused as an A record', () => text('section[aria-label="A name"]').includes('A: an IPv4 address'), 5000);
    await check('a row that is not of its type is refused as it is typed', $('#records-save').disabled);
    set($('select[aria-label="Type"]', rows[rows.length - 1]), 'AAAA');
    await click('www → the name', view);
    await click('Mail server', view);
    await sleep(600);
    await shot('bn-records-edit');
    await clickId('records-save', 10000);
    await answerDialog('Save the records', 'Save');
    const recs = await waitFor('the records on the chain', async () => {
      const r = await rpc('getnamerecords', [NAME]);
      return r.length === 3 ? r : null;
    }, 120000);
    await check('records saved: AAAA, www, MX', recs.some((r) => r.type === 'AAAA' && r.value === '2001:db8::7') && recs.some((r) => r.host === 'www' && r.type === 'CNAME') && recs.some((r) => r.type === 'MX'), JSON.stringify(recs));
    const dns = await rpc('resolvename', [`${NAME}.x`, 'AAAA']);
    const www = await rpc('resolvename', [`www.${NAME}.x`, 'ANY']);
    await check('the DNS of the node answers with them', dns.status === 'NOERROR' && dns.records.some((r) => r.data === '2001:db8::7') && www.records.some((r) => r.type === 'CNAME' && r.data === `${NAME}.x.`), JSON.stringify({ dns, www }));

    $$('[role="tab"]', view()).find((b) => b.textContent.trim() === 'Profile').click();
    await sleep(400);
    await click('Use new keys of this wallet', view);
    await waitFor('the keys', () => $('#profile-encryptionkey') && $('#profile-encryptionkey').value.length === 66, 20000);
    set($('#profile-paymailfee'), '0.001');
    await sleep(300);
    await shot('bn-profile');
    await clickId('profile-save');
    await answerDialog('Save the profile', 'Save');
    await waitFor('the profile on the chain', async () => {
      const n = await getname(NAME);
      return n && n.data.encryptionkey && n.data.paymailfee === 0.001;
    }, 120000);
    await check('new keys and a paymail fee set', true);

    $$('[role="tab"]', view()).find((b) => b.textContent.trim() === 'Subnames').click();
    await sleep(400);
    set(await waitFor('the label', () => $('#subname-label')), 'blog');
    await clickId('subname-create');
    await answerDialog('Make a subname', 'Make it');
    const sub = await waitFor('the subname', () => getname(`blog.${NAME}`), 120000);
    await check('a subname made', sub.parent === registered.namehash, JSON.stringify(sub));
    await waitFor('the subname listed', () => $('table[aria-label="Subnames"]') && text('table[aria-label="Subnames"]').includes(`blog.${NAME}`), 60000);
    await shot('bn-subnames');

    // --- Paymail: pay the name a message, then read it ---
    await go('paymail', 'send');
    set(await waitFor('the field To', () => $('#paymail-to')), NAME);
    await waitFor('the fee of the name', () => text('.paymail').includes('takes paid messages of at least'), 20000);
    set($('#paymail-text'), 'Hello from the end-to-end test');
    await sleep(300);
    await shot('bn-paymail-send');
    await clickId('paymail-send');
    await answerDialog('Send paymail', 'Send');
    await waitFor('the paymail in a block', async () => (await rpc('listpaymail', [], 'bn')).some((m) => m.confirmations > 0), 120000);
    await go('paymail', 'inbox');
    await waitFor('the message in the inbox', () => text('.paymail').includes('Hello from the end-to-end test'), 60000);
    await check('paymail sent and read', true);
    await shot('bn-paymail-inbox');

    // --- DNS: the server of the node, a lookup ---
    await go('dns');
    await waitFor('the server on', () => text('.dns').includes('This node answers DNS for .x'), 20000);
    set($('#dns-domain'), NAME);
    set($('select[aria-label="Record type"]'), 'AAAA');
    await clickId('dns-lookup');
    await waitFor('the answer', () => $('table[aria-label="Answer"]') && text('table[aria-label="Answer"]').includes('2001:db8::7'), 20000);
    await check('the lookup tester shows the answer of the node', text('.dns').includes('NOERROR'));
    await shot('bn-dns');

    // --- The directory ---
    await go('directory');
    set(await waitFor('the search', () => $('#directory-search')), 'sato');
    await waitFor('the name found', () => $$('.bn-item').some((b) => b.textContent.includes(NAME)), 30000);
    $$('.bn-item').find((b) => b.textContent.includes(NAME) && !b.textContent.includes('blog')).click();
    await waitFor('the name, read only', () => view() && !$('#records-save'), 20000);
    await sleep(600);
    await check('found in the directory, read only', true);
    await shot('bn-directory');

    // --- Give the subname to another wallet ---
    await rpc('createwallet', ['bn2']);
    const other = await rpc('getnewaddress', [], 'bn2');
    T().selectWallet('bn');
    await waitFor('the wallet bn shown', () => T().wallets.current === 'bn', 10000);
    T().goTo('names', 'transfer', `0x${sub.namehash}`);
    await sleep(500);
    await waitFor('the subname picked', () => view() && text('section[aria-label="A name"]').includes(`blog.${NAME}`), 30000);
    set(await waitFor('the field To', () => $('#give-to')), other);
    await clickId('give-send', 20000);
    await answerDialog('Give the name', 'Give it');
    await waitFor('the subname in the other wallet', async () => (await rpc('listmynames', [], 'bn2')).some((m) => m.kind === 'name' && m.namehash === sub.namehash), 120000);
    await check('the subname given to another wallet', true);

    // --- Every page of BitNames, in the layouts and looks ---
    await go('names');
    await sleep(800);
    await go('home');
    await sleep(1500);
    await shot('bn-home-midnight');
    for (const page of ['register', 'names', 'directory', 'paymail', 'dns']) {
      await go(page);
      await sleep(900);
      await shot(`bn-${page}-midnight`);
    }
    T().savePrefs({ theme: 'light' });
    for (const page of ['home', 'register', 'names', 'directory', 'paymail', 'dns']) {
      await go(page);
      await sleep(900);
      await shot(`bn-${page}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'workspace' });
    for (const page of ['home', 'register', 'names', 'dns']) {
      await go(page);
      await sleep(1000);
      await shot(`bn-workspace-${page}-midnight`);
    }
    T().savePrefs({ theme: 'light' });
    for (const page of ['home', 'names', 'directory']) {
      await go(page);
      await sleep(1000);
      await shot(`bn-workspace-${page}-light`);
    }
    T().savePrefs({ theme: 'midnight', layout: 'dashboard' });

    await rpc('setbmm', [false]);
    await log(`NAME ${registered.namehash}`);
    {
      // The confirmations of the desktop: what the chain's pages paid or signed, each with its facts.
      const titles = ["Change the records of a name", "Create a subname", "Register a name", "Release a name", "Release a reservation", "Reserve a name", "Send paymail", "Sign as a name", "Transfer a name", "Update a name"];
      const asked = (await host('test.confirmed', {})).filter((q) => titles.includes(q.title));
      await check(`the desktop confirmed what the pages paid or signed (${asked.length})`, asked.length > 0 && asked.every((q) => q.rows.length > 1), JSON.stringify(asked.map((q) => q.title)));
    }
    await finish();
  } catch (e) {
    await finish(e);
  }
})();
