/* Smoke test for the LinuxSCP Commander web client: boots the real page in
 * jsdom with a stubbed backend and checks that the UI renders and reacts. */
const fs = require('fs');
const path = require('path');

function loadJsdom() {
  const candidates = ['jsdom', path.join(process.cwd(), 'node_modules', 'jsdom')];
  for (const dir of (process.env.NODE_PATH || '').split(path.delimiter)) {
    if (dir) candidates.push(path.join(dir, 'jsdom'));
  }
  for (const candidate of candidates) {
    try { return require(candidate); } catch (error) { /* try the next place */ }
  }
  console.error('This test needs jsdom. Install it with:  npm install jsdom');
  process.exit(2);
}
const { JSDOM } = loadJsdom();

/* Usage: node ClientTest.js [path/to/index.html]
 *
 * Needs jsdom:  npm install jsdom   (in any directory on NODE_PATH or above
 * the page). The backend is stubbed, so no LinuxSCP build is required. */
const PAGE = process.argv[2] || path.join(__dirname, '..', 'web', 'index.html');
const html = fs.readFileSync(PAGE, 'utf8');

const LOCAL_ENTRIES = [
  { name: 'a.txt', size: 9, is_dir: false, date: 'Oct 13 07:07', permissions: '-rw-r--r--' },
  { name: 'sub', size: 0, is_dir: true, date: 'Oct 13 07:08', permissions: 'drwxr-xr-x' },
  { name: 'big.bin', size: 1048576, is_dir: false, date: 'Oct 13 07:09', permissions: '-rw-r--r--' }
];
const REMOTE_ENTRIES = [
  { name: 'keep.txt', size: 22, is_dir: false, date: 'Oct 13 07:07', permissions: '-rw-r--r--' },
  { name: 'testdir', size: 60, is_dir: true, date: 'Oct 13 07:08', permissions: 'drwxr-xr-x' }
];

const responses = {
  '/api/status': {
    tabs: [{
      id: 1, name: 'local test', connected: true,
      session: { protocol: 'sftp', host: '127.0.0.1', port: 2222, username: 'user', hostkey: '*' },
      remote_dir: '/tmp/slowremote', local_dir: '/tmp/slowlocal',
      observed_hostkey: 'ssh-ed25519 255 SHA256:abc', error: ''
    }],
    active_tab: 1, paused: false, binary: '/usr/bin/linuxscp',
    local_dir: '/tmp/slowlocal', remote_dir: '/tmp/slowremote',
    sessions_file: '/tmp/cfg/sessions.ini'
  },
  '/api/local/list': { success: true, path: '/tmp/slowlocal', entries: LOCAL_ENTRIES },
  '/api/remote/list': { success: true, path: '/tmp/slowremote', entries: REMOTE_ENTRIES },
  '/api/transfers': {
    transfers: [
      { id: 1, type: 'Download', status: 'Running', source: '/r/a.txt', destination: '/l/a.txt',
        bytes_done: 5, bytes_total: 10, percent: 50, detail: '' },
      { id: 2, type: 'Synchronize (both)', status: 'Failed', source: '/l', destination: '/r',
        bytes_done: 0, bytes_total: 0, percent: 0, detail: 'boom', error: 'connection lost' }
    ],
    paused: false
  },
  '/api/logs': { logs: [{ time: '07:00', level: 'info', text: 'Connected' }] },
  '/api/sessions': {
    file: '/tmp/cfg/sessions.ini',
    sessions: [{ name: 'GUI/test', protocol: 'sftp', host: '127.0.0.1', port: 2222,
      username: 'user', keyfile: '/tmp/key', hostkey: '*', initial_dir: '/tmp/slowremote',
      local_dir: '/tmp/slowlocal', last_used: 1791876426 }]
  },
  '/api/remote/edit': { success: true, path: '/tmp/slowremote/keep.txt',
    content: 'keep\n', info: { size: 5, mtime: 1791875271, temp: '/tmp/x', path: '/tmp/slowremote/keep.txt' } },
  '/api/sync/preview': { success: true, plan: {
    header: { local: '/tmp/slowlocal', remote: '/tmp/slowremote', mode: 'both', criteria: 'time', mask: '(none)' },
    items: [
      { action: 'download', path: 'keep.txt', reason: 'the remote file is newer' },
      { action: 'delete-remote', path: 'old.bak', reason: 'deleted locally' },
      { action: 'conflict', path: 'a.txt', reason: 'changed on both sides' },
      { action: 'unchanged', path: 'skip.log', reason: 'up to date' }
    ],
    summary: { download: 1, upload: 0, 'delete-local': 0, 'delete-remote': 1, conflict: 1, unchanged: 1 } } },
  '/api/remote/search': { success: true, matches: [
    { path: '/tmp/slowremote/keep.txt', name: 'keep.txt', is_dir: false, size: 22,
      date: 'Oct 13 07:07', permissions: '-rw-r--r--' }], truncated: false },
  '/api/command/exec': { success: true, exit_code: 0, stdout: '/tmp/slowremote\n', stderr: '' },
  /* A new site has no host key, so the backend asks instead of connecting. */
  '/api/tabs/open': {
    success: false,
    error: 'The host key is not cached; use -hostkey to accept it',
    challenge: { tab: 7, host: '127.0.0.1', port: 2222,
      fingerprint: 'ssh-ed25519 255 SHA256:2cxQVcO3EC5SbPVedJPA1XHIV2sEvZzPVTIiBgoZaxM' },
    tab: { id: 7, name: 'local test', connected: false, error: '' }
  },
  '/api/tabs/verify': { success: true, tab: {
    id: 7, name: 'local test', connected: true,
    session: { protocol: 'sftp', host: '127.0.0.1', port: 2222, username: 'user',
      hostkey: 'ssh-ed25519 255 SHA256:2cxQVcO3EC5SbPVedJPA1XHIV2sEvZzPVTIiBgoZaxM' },
    remote_dir: '/tmp/slowremote', local_dir: '/tmp/slowlocal',
    observed_hostkey: 'ssh-ed25519 255 SHA256:2cxQVcO3EC5SbPVedJPA1XHIV2sEvZzPVTIiBgoZaxM',
    error: '' } }
};

const TOKEN = 'test-token-0123456789abcdef';

const calls = [];
const errors = [];

const dom = new JSDOM(html, {
  url: 'http://localhost:8080/',
  runScripts: 'dangerously',
  pretendToBeVisual: true,
  beforeParse(window) {
    // What the backend injects into the page it serves.
    window.LSCP_TOKEN = TOKEN;
    window.fetch = (url, options) => {
      const path = String(url).replace('http://localhost:8080', '').split('?')[0];
      calls.push({ path: path, options: options });
      const body = responses[path];
      if (body === undefined) {
        return Promise.resolve({ ok: true, json: () => Promise.resolve({ success: true }) });
      }
      return Promise.resolve({ ok: true, json: () => Promise.resolve(body) });
    };
    window.prompt = () => 'asked';
    window.confirm = () => true;
    window.alert = () => {};
    window.addEventListener('error', event => errors.push('window error: ' + event.message));
    window.addEventListener('unhandledrejection', event =>
      errors.push('unhandled rejection: ' + event.reason));
  }
});

const { window } = dom;

function check(name, condition, extra) {
  const status = condition ? 'ok  ' : 'FAIL';
  console.log(status + ' ' + name + (condition || !extra ? '' : ' -- ' + extra));
  if (!condition) process.exitCode = 1;
}

function text(id) {
  const node = window.document.getElementById(id);
  return node ? node.textContent.trim() : '<missing #' + id + '>';
}

function value(id) {
  const node = window.document.getElementById(id);
  return node ? node.value : '<missing #' + id + '>';
}

function options(id) {
  const node = window.document.getElementById(id);
  return node ? Array.prototype.slice.call(node.options) : [];
}

(async () => {
  await new Promise(resolve => setTimeout(resolve, 400));

  // Boot fetched what it needs.
  const paths = calls.map(call => call.path);
  check('boot queried the status', paths.includes('/api/status'));
  check('boot listed the local pane', paths.includes('/api/local/list'));
  check('boot listed the remote pane', paths.includes('/api/remote/list'));

  const doc = window.document;
  check('local pane rendered 3 rows',
    doc.getElementById('localFileList').children.length === 3,
    doc.getElementById('localFileList').children.length);
  check('remote pane rendered 2 rows',
    doc.getElementById('remoteFileList').children.length === 2);
  check('one session tab', doc.getElementById('tabStrip').children.length === 1);
  check('connected badge', text('statusBadge').startsWith('Connected'), text('statusBadge'));
  check('host key notice shown for a wildcard session',
    doc.getElementById('hostKeyBar').style.display === 'flex',
    doc.getElementById('hostKeyBar').style.display);
  check('queue tab counts 2 tasks', text('tabQueueBtn') === 'Transfer Queue (2)', text('tabQueueBtn'));
  check('queue rendered a progress bar',
    doc.getElementById('queueList').innerHTML.indexOf('progress-bar') >= 0);
  check('failed task offers Retry',
    doc.getElementById('queueList').innerHTML.indexOf('Retry') >= 0);
  check('sizes are formatted', text('localFileList').indexOf('1.0 MB') >= 0, text('localFileList'));

  // Selecting a file, then opening the editor on the remote side.
  doc.getElementById('remoteFileList').children[0]
    .dispatchEvent(new window.MouseEvent('click', { bubbles: true }));
  check('clicking a row selects it',
    doc.getElementById('remoteFileList').children[0].className === 'selected',
    doc.getElementById('remoteFileList').children[0].className);
  check('summary counts the selection', text('remoteSummary').indexOf('1 selected') >= 0,
    text('remoteSummary'));

  await window.editSelected();
  await new Promise(resolve => setTimeout(resolve, 100));
  check('editor opened', doc.getElementById('editorModal').className.indexOf('open') >= 0);
  check('editor loaded the content', doc.getElementById('editorText').value === 'keep\n',
    JSON.stringify(doc.getElementById('editorText').value));

  // Editing marks it dirty, saving posts the new content.
  const area = doc.getElementById('editorText');
  area.value = 'keep\nmore\n';
  area.dispatchEvent(new window.Event('input', { bubbles: true }));
  check('editing marks the file dirty', text('editorStatus').indexOf('Modified') >= 0,
    text('editorStatus'));
  await window.saveEditorFile();
  await new Promise(resolve => setTimeout(resolve, 100));
  const save = calls.filter(call => call.path === '/api/remote/save').pop();
  check('save posted the new content',
    save && JSON.parse(save.options.body).content === 'keep\nmore\n');
  check('save sent the base modification time',
    save && JSON.parse(save.options.body).base_mtime === 1791875271);

  // Synchronize: preview renders the plan.
  window.openSynchronize();
  check('sync dialog prefills the panes', doc.getElementById('syncLocal').value === '/tmp/slowlocal');
  await window.previewSynchronization();
  await new Promise(resolve => setTimeout(resolve, 100));
  check('plan listed four items',
    doc.getElementById('syncPreview').children.length === 4,
    doc.getElementById('syncPreview').children.length);
  check('plan summary mentions the conflict',
    text('syncSummary').indexOf('1 conflict') >= 0, text('syncSummary'));
  check('plan items are colour coded by action',
    doc.getElementById('syncPreview').innerHTML.indexOf('plan-item conflict') >= 0);
  doc.getElementById('syncMode').value = 'both';
  await window.runSynchronization();
  await new Promise(resolve => setTimeout(resolve, 100));
  const run = calls.filter(call => call.path === '/api/sync/run').pop();
  check('running the plan posts the settings',
    run && JSON.parse(run.options.body).mode === 'both');
  check('the contents are one of the comparisons offered',
    options('syncCriteria').some(option => option.value === 'checksum') &&
    options('syncCriteria').some(option => option.value === 'time,checksum'),
    options('syncCriteria').map(option => option.value).join('|'));

  // In the 'both' direction the record of the previous synchronization
  // decides, so the dialog stops offering the comparisons the console would
  // refuse there rather than letting a preview fail on one of them.
  doc.getElementById('syncMode').value = 'both';
  doc.getElementById('syncMode').dispatchEvent(new window.Event('change'));
  check('the both direction refuses the comparisons it cannot make',
    options('syncCriteria').filter(option => option.disabled)
      .map(option => option.value).join('|') === 'checksum|time,checksum|none',
    options('syncCriteria').filter(option => option.disabled)
      .map(option => option.value).join('|'));
  check('the both direction keeps a comparison it can make',
    value('syncCriteria') === 'time', value('syncCriteria'));
  check('the both direction says what decides instead',
    text('syncHint').indexOf('previous synchronization') >= 0, text('syncHint'));
  doc.getElementById('syncMode').value = 'local';
  doc.getElementById('syncMode').dispatchEvent(new window.Event('change'));
  check('a one-way direction offers every comparison again',
    options('syncCriteria').every(option => !option.disabled));

  // A list of comparisons reaches the console as the client got it, and the
  // default one is left out of the command line.
  doc.getElementById('syncCriteria').value = 'time,checksum';
  await window.runSynchronization();
  await new Promise(resolve => setTimeout(resolve, 100));
  const listed = calls.filter(call => call.path === '/api/sync/run').pop();
  check('the chosen comparison is posted with the plan',
    listed && JSON.parse(listed.options.body).criteria === 'time,checksum',
    listed && listed.options.body);

  // Search.
  window.openSearch();
  doc.getElementById('searchPattern').value = '*.txt';
  await window.runSearch();
  await new Promise(resolve => setTimeout(resolve, 100));
  check('search lists one match', text('searchSummary').indexOf('1 match') >= 0, text('searchSummary'));

  // Site manager.
  window.openSiteManager();
  await new Promise(resolve => setTimeout(resolve, 150));
  check('site manager lists the stored session',
    doc.getElementById('siteList').children.length === 1 &&
    doc.getElementById('siteList').textContent.indexOf('GUI/test') >= 0);
  doc.getElementById('siteList').children[0].dispatchEvent(new window.MouseEvent('click', { bubbles: true }));
  check('clicking a site fills the form', doc.getElementById('siteHost').value === '127.0.0.1',
    doc.getElementById('siteHost').value);
  await window.saveSite();
  await new Promise(resolve => setTimeout(resolve, 100));
  const saved = calls.filter(call => call.path === '/api/sessions/save').pop();
  check('saving posts the name', saved && JSON.parse(saved.options.body).name === 'GUI/test');
  check('saving does not post the password',
    saved && !JSON.parse(saved.options.body).session.password);

  // CLI terminal.
  window.switchTab('cli');
  doc.getElementById('cliInput').value = 'pwd ; ls';
  await window.execCliCommand();
  await new Promise(resolve => setTimeout(resolve, 100));
  check('terminal shows the command output',
    text('cliOutput').indexOf('/tmp/slowremote') >= 0, text('cliOutput').slice(-80));
  window.switchTab('log');
  await new Promise(resolve => setTimeout(resolve, 150));
  check('log tab renders entries', doc.getElementById('logOutput').children.length === 1,
    doc.getElementById('logOutput').children.length);
  window.switchTab('queue');

  // Keyboard shortcuts: Ctrl+A in the focused pane.
  doc.getElementById('localPane').dispatchEvent(new window.MouseEvent('click', { bubbles: true }));
  doc.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'a', ctrlKey: true, bubbles: true }));
  check('Ctrl+A selects every entry', text('localSummary').indexOf('3 selected') >= 0,
    text('localSummary'));
  doc.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'F9', bubbles: true }));
  check('F9 opens the synchronize dialog',
    doc.getElementById('syncModal').className.indexOf('open') >= 0);
  doc.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
  check('Escape closes it', doc.getElementById('syncModal').className.indexOf('open') < 0);

  // ---- Synchronized browsing ------------------------------------------
  // The boot stub answers every list call with one fixed response, which
  // cannot show navigation; switch to a path-aware stub for this section.
  const LOCAL_DIRS = {
    '/tmp/slowlocal': LOCAL_ENTRIES,
    '/tmp/slowlocal/sub': [
      { name: 'inner.txt', size: 3, is_dir: false, date: 'Oct 13 08:00',
        permissions: '-rw-r--r--' }
    ]
  };
  const REMOTE_DIRS = {
    '/tmp/slowremote': REMOTE_ENTRIES,
    '/tmp/slowremote/sub': [
      { name: 'inner-remote.txt', size: 4, is_dir: false, date: 'Oct 13 08:01',
        permissions: '-rw-r--r--' }
    ]
    // Deliberately no '/tmp/slowremote/testdir' -> '/tmp/slowlocal/testdir'
    // mapping: 'testdir' exists only on the remote side.
  };
  const browseCalls = [];
  window.fetch = (url, options) => {
    const raw = String(url).replace('http://localhost:8080', '');
    const parts = raw.split('?');
    const path = parts[0];
    calls.push({ path: path, options: options });
    browseCalls.push(raw);
    const params = new URLSearchParams(parts[1] || '');
    if (path === '/api/local/list' || path === '/api/remote/list') {
      const isLocal = path === '/api/local/list';
      const dir = params.get('path') ||
        (isLocal ? '/tmp/slowlocal' : '/tmp/slowremote');
      const table = isLocal ? LOCAL_DIRS : REMOTE_DIRS;
      const body = table[dir] ?
        { success: true, path: dir, entries: table[dir] } :
        { success: false, error: 'No such directory: ' + dir };
      return Promise.resolve({ ok: true, json: () => Promise.resolve(body) });
    }
    const body = responses[path];
    if (body === undefined) {
      return Promise.resolve(
        { ok: true, json: () => Promise.resolve({ success: true }) });
    }
    return Promise.resolve({ ok: true, json: () => Promise.resolve(body) });
  };
  const wait = (ms) => new Promise(resolve => setTimeout(resolve, ms));

  window.toggleSyncBrowse();
  check('synchronized browsing lights up when toggled',
    doc.getElementById('syncBrowseBtn').className.indexOf('active') >= 0,
    doc.getElementById('syncBrowseBtn').className);
  await doc.getElementById('localFileList').children[1].ondblclick();
  await wait(80);
  check('entering sub locally moved the local pane',
    value('localPathInput') === '/tmp/slowlocal/sub', value('localPathInput'));
  check('the remote pane followed into its own sub',
    value('remotePathInput') === '/tmp/slowremote/sub', value('remotePathInput'));
  check('the follow asked for the counterpart path',
    browseCalls.some(url =>
      url.indexOf('path=' + encodeURIComponent('/tmp/slowremote/sub')) >= 0),
    browseCalls.slice(-4).join(' | '));
  check('the counterpane lists the sub contents',
    text('remoteFileList').indexOf('inner-remote.txt') >= 0,
    text('remoteFileList'));

  await window.goUp('local');
  await wait(80);
  check('going up carries the pair back to the roots',
    value('localPathInput') === '/tmp/slowlocal' &&
    value('remotePathInput') === '/tmp/slowremote',
    value('localPathInput') + ' / ' + value('remotePathInput'));

  // A directory the other side does not have: neither pane moves.
  await window.openEntry('remote',
    REMOTE_ENTRIES.find(entry => entry.name === 'testdir'));
  await wait(80);
  check('a missing counterpart leaves both panes alone',
    value('localPathInput') === '/tmp/slowlocal' &&
    value('remotePathInput') === '/tmp/slowremote',
    value('localPathInput') + ' / ' + value('remotePathInput'));

  window.toggleSyncBrowse();
  check('toggling synchronized browsing off',
    doc.getElementById('syncBrowseBtn').className.indexOf('active') < 0,
    doc.getElementById('syncBrowseBtn').className);
  await doc.getElementById('localFileList').children[1].ondblclick();
  await wait(80);
  check('with it off the other pane stays put',
    value('localPathInput') === '/tmp/slowlocal/sub' &&
    value('remotePathInput') === '/tmp/slowremote',
    value('localPathInput') + ' / ' + value('remotePathInput'));
  await window.goUp('local');
  await wait(80);

  // ---- Drag and drop between the panes --------------------------------
  function fakeTransfer() {
    const store = {};
    return {
      effectAllowed: '', dropEffect: '',
      setData(kind, value) { store[kind] = value; },
      getData(kind) { return store[kind] || ''; }
    };
  }
  const dragDt = fakeTransfer();
  const dragStart = { dataTransfer: dragDt };
  window.beginDrag('local', LOCAL_ENTRIES[0], dragStart);
  check('the drag payload carries the dragged names',
    JSON.parse(dragDt.getData('text/plain')).names.join(',') === 'a.txt',
    dragDt.getData('text/plain'));
  check('the payload names the source pane',
    JSON.parse(dragDt.getData('text/plain')).side === 'local',
    dragDt.getData('text/plain'));
  check('dragging an unselected entry selects it',
    window.selectedNames('local').join(',') === 'a.txt',
    window.selectedNames('local').join(','));

  const overEvent = {
    dataTransfer: dragDt,
    preventDefault() { this.prevented = true; }
  };
  window.dragOverPane('remote', overEvent);
  check('dragover on the other pane is accepted',
    overEvent.prevented === true, 'preventDefault not called');
  check('the drop target highlights',
    doc.getElementById('remotePane').className.indexOf('drop-target') >= 0,
    doc.getElementById('remotePane').className);

  const uploadsBefore = calls.filter(
    call => call.path === '/api/transfer/upload').length;
  await window.dropOnPane('remote', { dataTransfer: dragDt,
    preventDefault() {} });
  await wait(150);
  const uploadCalls = calls.filter(
    call => call.path === '/api/transfer/upload');
  check('dropping on the other pane queues an upload',
    uploadCalls.length === uploadsBefore + 1,
    uploadsBefore + ' -> ' + uploadCalls.length);
  const uploadBody = uploadCalls.length ?
    JSON.parse(uploadCalls[uploadCalls.length - 1].options.body) : {};
  check('the upload carries exactly the dragged names',
    (uploadBody.names || []).join(',') === 'a.txt',
    JSON.stringify(uploadBody.names));
  check('the upload is rooted in both panes',
    uploadBody.local_dir === '/tmp/slowlocal' &&
    uploadBody.remote_dir === '/tmp/slowremote',
    uploadBody.local_dir + ' / ' + uploadBody.remote_dir);
  check('the highlight cleared after the drop',
    doc.getElementById('remotePane').className.indexOf('drop-target') < 0,
    doc.getElementById('remotePane').className);

  // A drop on the pane the items came from queues nothing.
  const sameDt = fakeTransfer();
  window.beginDrag('local', LOCAL_ENTRIES[0], { dataTransfer: sameDt });
  const sameBefore = calls.filter(
    call => call.path === '/api/transfer/upload').length;
  await window.dropOnPane('local', { dataTransfer: sameDt,
    preventDefault() {} });
  await wait(60);
  check('a same-pane drop does not queue anything',
    calls.filter(call => call.path === '/api/transfer/upload').length ===
      sameBefore);

  // Every API call has to carry this run's token.
  check('every API call carries the token',
    calls.length > 0 && calls.every(call => call.options && call.options.headers &&
      call.options.headers['X-LSCP-Token'] === TOKEN),
    calls.filter(call => !(call.options && call.options.headers &&
      call.options.headers['X-LSCP-Token'] === TOKEN)).map(c => c.path).join(','));

  // Host keys are verified instead of accepted.
  window.openSiteManager();
  await new Promise(resolve => setTimeout(resolve, 100));
  await window.connectFromSiteManager();
  await new Promise(resolve => setTimeout(resolve, 100));
  check('an unknown host key opens the verification dialog',
    doc.getElementById('hostKeyModal').className.indexOf('open') >= 0);
  check('the dialog shows the fingerprint',
    text('hostKeyFingerprint') ===
      'ssh-ed25519 255 SHA256:2cxQVcO3EC5SbPVedJPA1XHIV2sEvZzPVTIiBgoZaxM',
    text('hostKeyFingerprint'));
  check('the dialog names the host',
    text('hostKeyQuestion').indexOf('127.0.0.1 (port 2222)') >= 0, text('hostKeyQuestion'));
  await window.acceptHostKey();
  await new Promise(resolve => setTimeout(resolve, 150));
  const verify = calls.filter(call => call.path === '/api/tabs/verify').pop();
  check('connecting pins the fingerprint that was shown',
    verify && JSON.parse(verify.options.body).fingerprint ===
      'ssh-ed25519 255 SHA256:2cxQVcO3EC5SbPVedJPA1XHIV2sEvZzPVTIiBgoZaxM');
  check('remembering the key is the default',
    verify && JSON.parse(verify.options.body).remember === true);
  check('the pinned connection becomes the active tab',
    text('statusBadge').indexOf('Connected') >= 0, text('statusBadge'));

  window.showHelp();
  check('help shows the token of this run', text('helpToken') === TOKEN, text('helpToken'));
  window.closeModal('helpModal');

  check('no script errors', errors.length === 0, errors.join(' | '));
  window.close();
})();
