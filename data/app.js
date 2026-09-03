/* ESP32 IR Blaster - web UI logic.
   Plain modern JavaScript, no build step, no dependencies: the file that ships is the
   file that runs, which matters when the next person to debug this device is
   doing it three years from now with a browser we have never seen. */
'use strict';

const $  = (sel) => document.querySelector(sel);
const $$ = (sel) => Array.from(document.querySelectorAll(sel));

let STATE = {
  commands: [],
  groups: [],
  group: '*',
  search: '',
  settings: {},
  learnPoll: null,
  statusPoll: null,
  dow: 0x7f,
  busy: false
};

/* ------------------------------------------------------------------ utils */

function toast(msg, kind) {
  const el = document.createElement('div');
  el.className = 'toast' + (kind ? ' ' + kind : '');
  el.textContent = msg;
  $('#toasts').appendChild(el);
  setTimeout(() => {
    el.style.transition = 'opacity .3s';
    el.style.opacity = '0';
    setTimeout(() => el.remove(), 320);
  }, kind === 'bad' ? 5200 : 2600);
}

async function api(path, opts) {
  const res = await fetch(path, Object.assign({ cache: 'no-store' }, opts || {}));
  if (res.status === 401) throw new Error('Authentication required - reload the page');
  let body = null;
  const text = await res.text();
  if (text) {
    try { body = JSON.parse(text); }
    catch (e) { throw new Error('Malformed reply from the device'); }
  }
  if (!res.ok || (body && body.ok === false)) {
    throw new Error((body && body.error) || ('HTTP ' + res.status));
  }
  return body || {};
}

const post = (path, obj) =>
  api(path, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(obj || {})
  });

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, (c) => (
    { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
  ));
}

function humanBytes(n) {
  if (n == null) return '—';
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(2) + ' MB';
}

function humanUptime(sec) {
  if (!sec && sec !== 0) return '—';
  const d = Math.floor(sec / 86400);
  const h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  if (d) return `${d}d ${h}h ${m}m`;
  if (h) return `${h}h ${m}m`;
  return `${m}m ${sec % 60}s`;
}

function modal(html) {
  $('#modal').innerHTML = html;
  $('#overlay').hidden = false;
}
function closeModal() { $('#overlay').hidden = true; $('#modal').innerHTML = ''; }
$('#overlay').addEventListener('click', (e) => { if (e.target.id === 'overlay') closeModal(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeModal(); });

/* -------------------------------------------------------------------- tabs */

$$('#tabs .tab').forEach((btn) => {
  btn.addEventListener('click', () => {
    $$('#tabs .tab').forEach((b) => b.classList.toggle('active', b === btn));
    const name = btn.dataset.tab;
    $$('.panel').forEach((p) => p.classList.toggle('active', p.id === 'tab-' + name));
    if (name === 'schedules') loadSchedules();
    if (name === 'settings') loadSettings();
    if (name === 'system') { loadLogs(); }
  });
});

/* ------------------------------------------------------------------ status */

async function refreshStatus() {
  try {
    const s = await api('/api/status');
    $('#dot-live').className = 'dot live';
    $('#fw-version').textContent = 'v' + s.fw;

    const pills = [];
    if (s.wifi === 'connected') {
      pills.push(['ok', `${s.ip} · ${s.rssi} dBm`]);
    } else {
      pills.push(['bad', 'WiFi disconnected']);
    }
    if (s.portal) pills.push(['warn', 'Setup AP: ' + s.apSsid]);
    if (s.mqtt === 'connected') pills.push(['ok', 'MQTT']);
    else if (s.mqtt === 'disconnected') pills.push(['bad', 'MQTT down']);
    pills.push(['', s.commands + '/' + s.maxCommands + ' cmds']);
    pills.push(['', humanBytes(s.heap) + ' free']);
    if (s.lastSent) pills.push(['', 'last: ' + s.lastSent]);

    $('#status-pills').innerHTML = pills
      .map(([k, t]) => `<span class="pill ${k}">${esc(t)}</span>`)
      .join('');

    renderSystem(s);
  } catch (e) {
    $('#dot-live').className = 'dot dead';
    $('#status-pills').innerHTML = '<span class="pill bad">device unreachable</span>';
  }
}

function renderSystem(s) {
  const rows = [
    ['Firmware', 'v' + s.fw],
    ['Hostname', s.host + ' (' + s.mdns + ')'],
    ['IP address', s.ip],
    ['WiFi', s.wifi === 'connected' ? `${s.ssid} · ${s.rssi} dBm` : 'disconnected'],
    ['Setup portal', s.portal ? 'active (' + s.apSsid + ')' : 'off'],
    ['MQTT', s.mqtt + (s.mqttError ? ' · ' + s.mqttError : '')],
    ['Local time', s.time || 'not synchronised'],
    ['Uptime', humanUptime(s.uptime)],
    ['Free heap', humanBytes(s.heap) + ' (min ' + humanBytes(s.heapMin) + ')'],
    ['Largest free block', humanBytes(s.heapMax)],
    ['Storage', humanBytes(s.fsUsed) + ' of ' + humanBytes(s.fsTotal)],
    ['Commands', s.commands + ' of ' + s.maxCommands],
    ['Schedules', String(s.schedules)],
    ['Signals sent', String(s.txCount)],
    ['Signals received', String(s.rxCount)],
    ['Last reset reason', String(s.resetReason)]
  ];
  $('#sys-kv').innerHTML = rows
    .map(([k, v]) => `<div><span>${esc(k)}</span><span>${esc(v)}</span></div>`)
    .join('');
}

/* ---------------------------------------------------------------- commands */

async function loadCommands() {
  const r = await api('/api/commands');
  STATE.commands = r.commands || [];
  STATE.groups = r.groups || [];
  renderGroups();
  renderCommands();
  fillCommandPickers();
}

function renderGroups() {
  const chips = ['<span class="chip' + (STATE.group === '*' ? ' active' : '') +
                 '" data-g="*">All</span>'];
  STATE.groups.forEach((g) => {
    chips.push('<span class="chip' + (STATE.group === g ? ' active' : '') +
               '" data-g="' + esc(g) + '">' + esc(g) + '</span>');
  });
  $('#group-chips').innerHTML = chips.join('');
  $$('#group-chips .chip').forEach((c) => {
    c.addEventListener('click', () => {
      STATE.group = c.dataset.g;
      renderGroups();
      renderCommands();
    });
  });

  $('#group-options').innerHTML =
    STATE.groups.map((g) => `<option value="${esc(g)}">`).join('');
}

function renderCommands() {
  const q = STATE.search.toLowerCase();
  const list = STATE.commands.filter((c) =>
    (STATE.group === '*' || c.group === STATE.group) &&
    (!q || c.name.toLowerCase().includes(q) || c.group.toLowerCase().includes(q))
  );

  $('#commands-empty').hidden = STATE.commands.length !== 0;

  const byGroup = {};
  list.forEach((c) => { (byGroup[c.group] = byGroup[c.group] || []).push(c); });

  let html = '';
  Object.keys(byGroup).sort().forEach((g) => {
    html += `<div class="group-title">${esc(g)}</div><div class="cmd-grid">`;
    byGroup[g].forEach((c) => {
      const meta = `${esc(c.protocol)} · ${c.raw} timings` +
                   (c.frames > 1 ? ` · ${c.frames} parts` : '') +
                   (c.repeats > 1 ? ` · x${c.repeats}` : '');
      html += `<div class="cmd" data-id="${c.id}">
        <button class="cmd-main" data-act="send" title="Send">
          <div class="cmd-name">${esc(c.name)}</div>
          <div class="cmd-meta">${meta}</div>
        </button>
        <button class="icon-btn" data-act="menu" title="Options">&#9881;</button>
      </div>`;
    });
    html += '</div>';
  });
  $('#command-list').innerHTML = html;

  $$('#command-list .cmd').forEach((el) => {
    const id = el.dataset.id;
    el.querySelector('[data-act=send]').addEventListener('click', () => sendCommand(id, el));
    el.querySelector('[data-act=menu]').addEventListener('click', () => openCommandMenu(id));
  });
}

async function sendCommand(id, el) {
  if (STATE.busy) return;
  STATE.busy = true;
  if (el) el.classList.add('sending');
  try {
    const r = await post('/api/commands/send', { id });
    toast('Sent ' + r.sent, 'ok');
  } catch (e) {
    toast(e.message, 'bad');
  } finally {
    STATE.busy = false;
    if (el) setTimeout(() => el.classList.remove('sending'), 250);
  }
}

function openCommandMenu(id) {
  const c = STATE.commands.find((x) => x.id === id);
  if (!c) return;
  modal(`
    <h3>${esc(c.name)}</h3>
    <div class="grid2">
      <label>Name<input id="m-name" maxlength="32" value="${esc(c.name)}"></label>
      <label>Group<input id="m-group" list="group-options" maxlength="24" value="${esc(c.group)}"></label>
      <label>Repeats<input type="number" id="m-repeats" min="1" max="20" value="${c.repeats}"></label>
      <label>Carrier (kHz)<input type="number" id="m-khz" min="30" max="60" value="${c.khz}"></label>
    </div>
    <label class="switch"><input type="checkbox" id="m-raw" ${c.forceRaw ? 'checked' : ''}><span></span>
      Always replay raw timings</label>
    <p class="note">
      Protocol <b>${esc(c.protocol)}</b>${c.bits ? ', ' + c.bits + ' bits' : ''} ·
      ${c.raw} timings${c.frames > 1 ? ' in ' + c.frames + ' parts' : ''}.
      Turn on raw replay if the appliance ignores this command.
    </p>
    <div class="row">
      <button class="btn primary" id="m-save">Save</button>
      <button class="btn" id="m-send">Send now</button>
      <button class="btn danger" id="m-del">Delete</button>
      <button class="btn" id="m-close">Close</button>
    </div>`);

  $('#m-close').onclick = closeModal;
  $('#m-send').onclick = () => sendCommand(id, null);

  $('#m-save').onclick = async () => {
    try {
      await post('/api/commands/rename', {
        id,
        name: $('#m-name').value.trim(),
        group: $('#m-group').value.trim()
      });
      await post('/api/commands/options', {
        id,
        repeats: +$('#m-repeats').value,
        khz: +$('#m-khz').value,
        forceRaw: $('#m-raw').checked
      });
      closeModal();
      await loadCommands();
      toast('Saved', 'ok');
    } catch (e) { toast(e.message, 'bad'); }
  };

  $('#m-del').onclick = async () => {
    if (!confirm('Delete "' + c.name + '" permanently?')) return;
    try {
      await post('/api/commands/delete', { id });
      closeModal();
      await loadCommands();
      loadSchedules();
      toast('Deleted', 'ok');
    } catch (e) { toast(e.message, 'bad'); }
  };
}

$('#search').addEventListener('input', (e) => {
  STATE.search = e.target.value;
  renderCommands();
});

/* ------------------------------------------------------------------- learn */

function setLearnUi(st) {
  const head = $('#learn-headline');
  const detail = $('#learn-detail');
  const waiting = st.state === 'waiting';
  const captured = st.state === 'captured';

  $('#btn-learn-start').hidden = waiting;
  $('#btn-learn-cancel').hidden = !waiting;
  $('#btn-learn-append').hidden = !captured;
  $('#btn-learn-test').hidden = !captured;
  $('#learn-save').hidden = !captured;
  $('#learn-bar').hidden = !waiting;

  head.className = 'learn-big';
  if (waiting) {
    head.classList.add('waiting');
    head.textContent = 'Listening… ' + Math.ceil(st.remainingMs / 1000) + 's';
    detail.textContent = 'Point the remote at the receiver and press a button.';
    const total = STATE.settings.learnTimeoutMs || 20000;
    $('#learn-bar-fill').style.width = (100 * st.remainingMs / total) + '%';
  } else if (captured) {
    head.classList.add('ok');
    head.textContent = 'Captured';
    detail.textContent =
      `${st.protocol}${st.bits ? ' · ' + st.bits + ' bits' : ''} · ` +
      `${st.raw} timings · ${st.frames} part${st.frames > 1 ? 's' : ''}` +
      (st.value && st.value !== '0x0' ? ' · ' + st.value : '');
    $('#btn-learn-start').textContent = 'Capture again';
  } else if (st.state === 'timeout') {
    head.classList.add('bad');
    head.textContent = 'Nothing received';
    detail.textContent = st.error || 'Try again, closer to the receiver.';
    $('#btn-learn-start').textContent = 'Start listening';
  } else if (st.state === 'error') {
    head.classList.add('bad');
    head.textContent = 'Capture failed';
    detail.textContent = st.error || 'Unknown error.';
    $('#btn-learn-start').textContent = 'Start listening';
  } else {
    head.textContent = 'Ready';
    detail.textContent = 'Nothing captured yet.';
    $('#btn-learn-start').textContent = 'Start listening';
  }

  if ($('#monitor-toggle').checked && st.lastSeen) $('#monitor-out').textContent = st.lastSeen;
}

async function pollLearn() {
  try {
    const st = await api('/api/learn/state');
    setLearnUi(st);
    // Keep polling while a capture could still arrive; stop once it settles.
    if (st.state !== 'waiting' && !$('#monitor-toggle').checked) stopLearnPoll();
  } catch (e) { stopLearnPoll(); }
}

function startLearnPoll() {
  if (STATE.learnPoll) return;
  STATE.learnPoll = setInterval(pollLearn, 400);
  pollLearn();
}
function stopLearnPoll() {
  if (STATE.learnPoll) clearInterval(STATE.learnPoll);
  STATE.learnPoll = null;
}

$('#btn-learn-start').onclick = async () => {
  try {
    await post('/api/learn/start', { append: false });
    startLearnPoll();
  } catch (e) { toast(e.message, 'bad'); }
};
$('#btn-learn-append').onclick = async () => {
  try {
    await post('/api/learn/start', { append: true });
    startLearnPoll();
    toast('Press the next part of the sequence', 'ok');
  } catch (e) { toast(e.message, 'bad'); }
};
$('#btn-learn-cancel').onclick = async () => {
  try { await post('/api/learn/cancel'); await pollLearn(); }
  catch (e) { toast(e.message, 'bad'); }
};
$('#btn-learn-test').onclick = async () => {
  try { await post('/api/learn/test'); toast('Test signal sent', 'ok'); }
  catch (e) { toast(e.message, 'bad'); }
};
$('#btn-learn-discard').onclick = async () => {
  try { await post('/api/learn/cancel'); } catch (e) { /* ignore */ }
  $('#learn-name').value = '';
  await pollLearn();
};

$('#btn-learn-save').onclick = async () => {
  const name = $('#learn-name').value.trim();
  if (!name) { toast('Give the command a name first', 'bad'); return; }
  try {
    await post('/api/learn/save', { name, group: $('#learn-group').value.trim() });
    $('#learn-name').value = '';
    await loadCommands();
    await pollLearn();
    toast('Saved "' + name + '"', 'ok');
  } catch (e) { toast(e.message, 'bad'); }
};

/* --------------------------------------------------------------- self-test */

// The one button drives both the default test and the custom-code one, so it
// says which it will do -- otherwise filling in the fields and pressing a
// button labelled "Run self-test" gives no clue that the fields were used.
function refreshSelfTestButton() {
  const btn = $('#btn-selftest');
  if (!btn) return;
  const proto = ($('#st-protocol').value || '').trim();
  const val = ($('#st-value').value || '').trim();
  const bits = ($('#st-bits').value || '').trim();
  const custom = proto || val || bits;
  btn.textContent = custom
    ? `Send ${(proto || 'NEC').toUpperCase()} ${val || '(default value)'}`
    : 'Run self-test';
}

['#st-protocol', '#st-value', '#st-bits'].forEach((sel) => {
  const el = $(sel);
  if (el) el.addEventListener('input', refreshSelfTestButton);
});

// Bound defensively. A stale cached page can pair new markup with old script
// or the reverse; a missing element must not throw here, because a top-level
// exception would take every handler defined *after* it down as well.
function onClick(sel, fn) {
  const el = $(sel);
  if (el) el.onclick = fn;
  else console.warn('missing element ' + sel + ' - stale cached page?');
}

onClick('#btn-selftest', async () => {
  const btn = $('#btn-selftest');
  btn.disabled = true;
  $('#selftest-busy').hidden = false;
  $('#selftest-result').hidden = true;
  try {
    // Only send the fields that were actually filled in; an empty body means
    // "use the built-in NEC test".
    const body = {};
    const proto = ($('#st-protocol').value || '').trim();
    const val = ($('#st-value').value || '').trim();
    const bits = ($('#st-bits').value || '').trim();
    if (proto) body.protocol = proto.toUpperCase();
    if (val) body.value = val;
    if (bits) body.bits = +bits;

    // The device blocks for up to ~1.5 s running this, so the button stays
    // disabled rather than letting a second request stack up behind it.
    const r = await post('/api/selftest', body);
    const head = $('#selftest-headline');
    head.className = 'learn-big ' + (r.pass ? 'ok' : 'bad');
    head.textContent = r.pass ? 'PASS' : 'FAIL';
    $('#selftest-verdict').textContent = r.verdict;

    const rows = [
      ['Receiver idle line', r.rxIdleOk
        ? `OK (${r.idleLowSamples}/${r.idleSamples} low)`
        : `not idle (${r.idleLowSamples}/${r.idleSamples} low)`],
      ['Signal returned', r.received ? 'yes' : 'no'],
      ['Attempts', String(r.attempts)],
      ['Expected', `${r.expectedProtocol} ${r.expectedValue}` +
        (r.expectedBits ? ` · ${r.expectedBits} bits` : '')]
    ];
    if (r.received) {
      rows.push(['Received', `${r.protocol} ${r.value || ''} · ${r.bits} bits`]);
      rows.push(['Raw timings', String(r.raw)]);
    }
    $('#selftest-kv').innerHTML = rows
      .map(([k, v]) => `<div><span>${esc(k)}</span><span>${esc(v)}</span></div>`)
      .join('');

    $('#selftest-result').hidden = false;
    toast(r.pass ? 'Self-test passed' : 'Self-test failed', r.pass ? 'ok' : 'bad');
  } catch (e) {
    toast(e.message, 'bad');
  } finally {
    btn.disabled = false;
    $('#selftest-busy').hidden = true;
  }
});

onClick('#btn-selftest-clear', () => {
  ['#st-protocol', '#st-value', '#st-bits'].forEach((s) => {
    const el = $(s);
    if (el) el.value = '';
  });
  refreshSelfTestButton();
});

$('#monitor-toggle').addEventListener('change', async (e) => {
  try {
    await post('/api/monitor', { on: e.target.checked });
    if (e.target.checked) { $('#monitor-out').textContent = 'Listening…'; startLearnPoll(); }
    else { $('#monitor-out').textContent = '—'; stopLearnPoll(); }
  } catch (err) { toast(err.message, 'bad'); }
});

/* --------------------------------------------------------------- schedules */

const DOW_NAMES = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

function renderDowPicker() {
  $('#sch-dows').innerHTML = DOW_NAMES
    .map((d, i) => `<div class="dow${(STATE.dow >> i) & 1 ? ' on' : ''}" data-i="${i}">${d}</div>`)
    .join('');
  $$('#sch-dows .dow').forEach((el) => {
    el.addEventListener('click', () => {
      STATE.dow ^= (1 << +el.dataset.i);
      renderDowPicker();
    });
  });
}

function dowLabel(mask) {
  if ((mask & 0x7f) === 0x7f) return 'every day';
  if ((mask & 0x7f) === 0x3e) return 'weekdays';
  if ((mask & 0x7f) === 0x41) return 'weekends';
  return DOW_NAMES.filter((_, i) => (mask >> i) & 1).join(' ');
}

function fillCommandPickers() {
  const opts = STATE.commands
    .map((c) => `<option value="${c.id}">${esc(c.group)} — ${esc(c.name)}</option>`)
    .join('');
  const sel = $('#sch-cmd');
  const keep = sel.value;
  sel.innerHTML = opts || '<option value="">(no commands stored)</option>';
  if (keep) sel.value = keep;
}

async function loadSchedules() {
  try {
    const r = await api('/api/schedules');
    $('#sch-clock').textContent = r.now ? 'Device clock: ' + r.now : '';
    $('#sch-time-warning').hidden = !!r.now;

    if (!r.schedules.length) {
      $('#schedule-list').innerHTML = '<p class="empty">No schedules yet.</p>';
      return;
    }
    $('#schedule-list').innerHTML = r.schedules.map((s) => {
      const cmd = STATE.commands.find((c) => c.id === s.cmd);
      const hh = String(s.hour).padStart(2, '0');
      const mm = String(s.minute).padStart(2, '0');
      return `<div class="sch${s.enabled ? '' : ' off'}" data-id="${s.id}">
        <div class="time">${hh}:${mm}</div>
        <div class="grow">
          <div>${esc(s.label || (cmd ? cmd.name : s.cmd))}</div>
          <div class="cmd-meta">${esc(cmd ? cmd.group + ' — ' + cmd.name : 'missing command')}
            · ${dowLabel(s.dow)}${s.repeats ? ' · x' + s.repeats : ''}</div>
        </div>
        <label class="switch"><input type="checkbox" data-act="toggle"
          ${s.enabled ? 'checked' : ''}><span></span></label>
        <button class="icon-btn" data-act="del" title="Delete">&#10005;</button>
      </div>`;
    }).join('');

    $$('#schedule-list .sch').forEach((el) => {
      const id = +el.dataset.id;
      const s = r.schedules.find((x) => x.id === id);
      el.querySelector('[data-act=toggle]').addEventListener('change', async (e) => {
        try {
          await post('/api/schedules', Object.assign({}, s, { enabled: e.target.checked }));
          loadSchedules();
        } catch (err) { toast(err.message, 'bad'); }
      });
      el.querySelector('[data-act=del]').addEventListener('click', async () => {
        try { await post('/api/schedules/delete', { id }); loadSchedules(); }
        catch (err) { toast(err.message, 'bad'); }
      });
    });
  } catch (e) { toast(e.message, 'bad'); }
}

$('#btn-sch-add').onclick = async () => {
  const cmd = $('#sch-cmd').value;
  if (!cmd) { toast('Learn a command first', 'bad'); return; }
  const [h, m] = ($('#sch-time').value || '23:00').split(':');
  try {
    await post('/api/schedules', {
      id: 0, enabled: true, hour: +h, minute: +m, dow: STATE.dow,
      repeats: +$('#sch-repeats').value || 0,
      cmd, label: $('#sch-label').value.trim()
    });
    $('#sch-label').value = '';
    loadSchedules();
    toast('Schedule added', 'ok');
  } catch (e) { toast(e.message, 'bad'); }
};

/* ---------------------------------------------------------------- settings */

const SETTING_IDS = [
  'hostname', 'wifiSsid', 'mqttHost', 'mqttPort', 'mqttUser', 'mqttBase',
  'haPrefix', 'tz', 'ntp1', 'ntp2', 'defaultRepeats', 'repeatGapMs',
  'frameGapMs', 'defaultFreqKhz', 'markExcessUs', 'learnTimeoutMs', 'authUser'
];
const SETTING_BOOLS = ['mqttEnabled', 'haDiscovery', 'ledEnabled', 'authEnabled'];

async function loadSettings() {
  try {
    const s = await api('/api/settings');
    STATE.settings = s;
    SETTING_IDS.forEach((k) => { const el = $('#set-' + k); if (el) el.value = (s[k] === undefined || s[k] === null) ? '' : s[k]; });
    SETTING_BOOLS.forEach((k) => { const el = $('#set-' + k); if (el) el.checked = !!s[k]; });
    $('#wifi-pass-hint').textContent = s.wifiPassSet ? '(stored)' : '(not set)';
  } catch (e) { toast(e.message, 'bad'); }
}

$('#btn-save-settings').onclick = async () => {
  const body = {};
  SETTING_IDS.forEach((k) => {
    const el = $('#set-' + k);
    if (!el) return;
    body[k] = el.type === 'number' ? (+el.value || 0) : el.value.trim();
  });
  SETTING_BOOLS.forEach((k) => { const el = $('#set-' + k); if (el) body[k] = el.checked; });

  // Empty password boxes mean "leave the stored secret alone", so they are
  // omitted from the request entirely rather than sent as "".
  ['wifiPass', 'apPass', 'mqttPass', 'authPass'].forEach((k) => {
    const v = $('#set-' + k).value;
    if (v) body[k] = v;
  });

  if (body.authEnabled && !body.authPass && !STATE.settings.authPassSet) {
    toast('Set a password before enabling authentication', 'bad');
    return;
  }

  try {
    const r = await post('/api/settings', body);
    ['wifiPass', 'apPass', 'mqttPass', 'authPass'].forEach((k) => { $('#set-' + k).value = ''; });
    if (r.wifiChanged) {
      $('#settings-hint').textContent =
        'WiFi settings changed — the device is reconnecting and may move to a new address.';
      toast('Saved. Reconnecting to WiFi…', 'ok');
    } else {
      $('#settings-hint').textContent = '';
      toast('Settings saved', 'ok');
    }
    loadSettings();
  } catch (e) { toast(e.message, 'bad'); }
};

$('#btn-wifi-scan').onclick = async () => {
  $('#scan-status').textContent = 'Scanning…';
  try {
    const r = await api('/api/wifi/scan');
    const seen = {};
    const nets = r.networks.filter((n) => n.ssid && !seen[n.ssid] && (seen[n.ssid] = 1));
    nets.sort((a, b) => b.rssi - a.rssi);
    $('#ssid-options').innerHTML =
      nets.map((n) => `<option value="${esc(n.ssid)}">${n.rssi} dBm${n.secure ? '' : ' (open)'}</option>`).join('');
    $('#scan-status').textContent = nets.length + ' networks found — open the SSID box';
  } catch (e) {
    $('#scan-status').textContent = '';
    toast(e.message, 'bad');
  }
};

$('#btn-mqtt-rediscover').onclick = async () => {
  try { await post('/api/mqtt/discovery'); toast('Discovery republished', 'ok'); }
  catch (e) { toast(e.message, 'bad'); }
};

/* ------------------------------------------------------------ backup / OTA */

$('#btn-export').onclick = () => { window.location.href = '/api/export'; };

$('#import-file').addEventListener('change', async (e) => {
  const file = e.target.files[0];
  if (!file) return;
  e.target.value = '';

  let data;
  try {
    data = JSON.parse(await file.text());
  } catch (err) {
    toast('That file is not valid JSON', 'bad');
    return;
  }
  const list = Array.isArray(data) ? data : data.commands;
  if (!Array.isArray(list) || !list.length) {
    toast('No commands found in that file', 'bad');
    return;
  }
  if (!confirm(`Import ${list.length} command(s)? Existing commands are kept; ` +
               'entries whose name already exists will be skipped.')) return;

  const bar = $('#import-bar');
  const log = $('#import-log');
  bar.hidden = false;
  log.hidden = false;
  log.textContent = '';

  let ok = 0, skipped = 0;
  for (let i = 0; i < list.length; i++) {
    // One request per command: the device parses a single entry at a time, so
    // a 100-command archive never needs to fit in its RAM.
    try {
      await post('/api/import', list[i]);
      ok++;
    } catch (err) {
      skipped++;
      log.textContent += `${list[i].name || '(unnamed)'}: ${err.message}\n`;
    }
    $('#import-bar-fill').style.width = (100 * (i + 1) / list.length) + '%';
  }
  log.textContent += `\nDone: ${ok} imported, ${skipped} skipped.`;
  await loadCommands();
  toast(`Imported ${ok} of ${list.length}`, skipped ? 'bad' : 'ok');
});

function uploadOta(file, url, label) {
  return new Promise((resolve, reject) => {
    const bar = $('#ota-bar');
    bar.hidden = false;
    const form = new FormData();
    form.append('image', file, file.name);

    const xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    // fetch() cannot report upload progress, which is the whole point here.
    xhr.upload.onprogress = (ev) => {
      if (ev.lengthComputable) {
        $('#ota-bar-fill').style.width = (100 * ev.loaded / ev.total) + '%';
      }
    };
    xhr.onload = () => {
      let msg = xhr.responseText;
      try { msg = JSON.parse(xhr.responseText).error || msg; } catch (e) { /* keep raw */ }
      if (xhr.status >= 200 && xhr.status < 300) resolve();
      else reject(new Error(label + ' update failed: ' + msg));
    };
    xhr.onerror = () => reject(new Error('Connection lost during the upload'));
    xhr.send(form);
  });
}

async function handleOta(input, url, label) {
  const file = input.files[0];
  if (!file) return;
  input.value = '';
  if (!confirm(`Upload ${file.name} (${humanBytes(file.size)}) as the ${label}? ` +
               'The device will reboot when it finishes.')) return;
  try {
    await uploadOta(file, url, label);
    toast(label + ' updated — rebooting', 'ok');
    setTimeout(() => window.location.reload(), 9000);
  } catch (e) {
    toast(e.message, 'bad');
  } finally {
    $('#ota-bar').hidden = true;
    $('#ota-bar-fill').style.width = '0';
  }
}

$('#ota-fw').addEventListener('change', (e) => handleOta(e.target, '/api/ota/firmware', 'firmware'));
$('#ota-fs').addEventListener('change', (e) => handleOta(e.target, '/api/ota/filesystem', 'filesystem'));

/* ------------------------------------------------------------ system tools */

async function loadLogs() {
  try {
    const r = await api('/api/logs');
    $('#log-out').textContent = r.lines.join('\n') || '—';
    $('#log-out').scrollTop = $('#log-out').scrollHeight;
  } catch (e) { /* the log is a nicety; never block the tab on it */ }
}
$('#btn-log-refresh').onclick = loadLogs;

$('#btn-portal').onclick = async () => {
  try { await post('/api/system/portal'); toast('Setup portal toggled', 'ok'); refreshStatus(); }
  catch (e) { toast(e.message, 'bad'); }
};

$('#btn-reboot').onclick = async () => {
  if (!confirm('Reboot the device now?')) return;
  try {
    await post('/api/system/reboot');
    toast('Rebooting…', 'ok');
    setTimeout(() => window.location.reload(), 7000);
  } catch (e) { toast(e.message, 'bad'); }
};

$('#btn-factory').onclick = async () => {
  if (!confirm('This erases every learned command, all schedules and the WiFi ' +
               'credentials. Continue?')) return;
  if (prompt('Type ERASE to confirm.') !== 'ERASE') return;
  try {
    await post('/api/system/factory-reset', { confirm: 'ERASE' });
    toast('Factory reset — the device is restarting into setup mode', 'ok');
  } catch (e) { toast(e.message, 'bad'); }
};

/* -------------------------------------------------------------------- boot */

function startStatusPoll() {
  if (STATE.statusPoll) clearInterval(STATE.statusPoll);
  // Polling stops while the tab is hidden: no point waking a phone's radio to
  // refresh a page nobody is looking at.
  STATE.statusPoll = setInterval(() => {
    if (!document.hidden) refreshStatus();
  }, 3000);
}

document.addEventListener('visibilitychange', () => { if (!document.hidden) refreshStatus(); });

(async function init() {
  renderDowPicker();
  await refreshStatus();
  startStatusPoll();
  try { await loadCommands(); } catch (e) { toast(e.message, 'bad'); }
  await loadSettings();
  await pollLearn();
})();
