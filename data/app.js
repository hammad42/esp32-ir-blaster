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
    if (name === 'library') loadLibrary();
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

/* Three outcomes worth telling apart, because they point at different faults:
   the send failed (firmware/storage), it succeeded but nothing came back (the
   emitter), or it was heard (the blaster is fine, look at the appliance). */
function fireVerdict(f) {
  if (!f.txOk) return { cls: 'bad',  text: 'failed', detail: f.error || 'the send was refused' };
  if (!f.heard) return { cls: 'warn', text: 'not heard',
                         detail: 'sent, but the receiver heard nothing come back' };

  const what = (f.protocol === 'UNKNOWN')
    ? `${f.rawLen} timings`
    : `${f.protocol}, ${f.bits} bits, ${f.value}`;

  // Hearing something is not hearing ourselves. Only "match" means the frame
  // that came back is the one that went out.
  if (f.match === 'match')
    return { cls: 'ok', text: 'confirmed', detail: 'received ' + what + ', matching what was sent' };
  if (f.match === 'mismatch')
    return { cls: 'bad', text: 'mismatch',
             detail: 'received ' + what + ', which is NOT what was sent' };
  return { cls: 'warn', text: 'heard',
           detail: 'received ' + what + ' — identity not checkable for this command' };
}

async function loadFireLog() {
  try {
    const r = await api('/api/schedules/log');
    const box = $('#fire-log');
    if (!box) return;
    if (!r.fires || !r.fires.length) {
      box.innerHTML = '<p class="empty">No schedule has fired yet.</p>';
      return;
    }
    box.innerHTML = r.fires.map((f) => {
      const v = fireVerdict(f);
      const when = f.at ? new Date(f.at * 1000).toLocaleString() : 'unknown time';
      return `<div class="fire">
        <span class="pill ${v.cls}">${v.text}</span>
        <div class="grow">
          <div>${esc(f.label)} <span class="muted">→ ${esc(f.command)}</span></div>
          <div class="cmd-meta">${esc(when)} · ${esc(v.detail)}</div>
        </div>
      </div>`;
    }).join('');
  } catch (e) { /* the tab is still usable without the log */ }
}

onClick('#btn-fire-clear', async () => {
  if (!confirm('Clear the record of past fires?')) return;
  try { await post('/api/schedules/log/clear', {}); loadFireLog(); }
  catch (e) { toast(e.message, 'bad'); }
});

async function loadSchedules() {
  loadFireLog();
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

  // Absent means "leave the stored secret alone"; present means "set it to
  // this", and an explicit empty string is how a secret gets cleared. Ticking
  // Clear is the only way to send that empty string -- typing nothing still
  // means no change, which is what people expect from a password box.
  ['wifiPass', 'apPass', 'mqttPass', 'authPass'].forEach((k) => {
    const clear = $('#clr-' + k);
    if (clear && clear.checked) { body[k] = ''; return; }
    const v = $('#set-' + k).value;
    if (v) body[k] = v;
  });

  if (body.authEnabled && !body.authPass && !STATE.settings.authPassSet) {
    toast('Set a password before enabling authentication', 'bad');
    return;
  }

  try {
    const r = await post('/api/settings', body);
    // Reset both the boxes and the Clear ticks, so a second save does not
    // silently repeat a clear the user only meant once.
    ['wifiPass', 'apPass', 'mqttPass', 'authPass'].forEach((k) => {
      const el = $('#set-' + k);
      if (el) el.value = '';
      const clr = $('#clr-' + k);
      if (clr) clr.checked = false;
    });
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

// Signal bars from RSSI. -50 or better is excellent, -80 is barely usable.
function rssiBars(rssi) {
  if (rssi >= -55) return '▁▃▅▇';
  if (rssi >= -65) return '▁▃▅';
  if (rssi >= -75) return '▁▃';
  return '▁';
}

onClick('#btn-wifi-scan', async () => {
  const btn = $('#btn-wifi-scan');
  btn.disabled = true;
  $('#scan-status').textContent = 'Scanning…';
  try {
    const r = await api('/api/wifi/scan');
    const seen = {};
    const nets = r.networks.filter((n) => n.ssid && !seen[n.ssid] && (seen[n.ssid] = 1));
    nets.sort((a, b) => b.rssi - a.rssi);

    // Rendered as an explicit list, NOT a <datalist>. A datalist attached to a
    // text input filters itself by whatever is already typed there -- and the
    // SSID box is pre-filled with the current network, so a scan that found
    // nine networks would offer exactly one, with no hint why.
    const box = $('#scan-results');
    box.innerHTML = nets.map((n) => `
      <button type="button" class="scan-row" data-ssid="${esc(n.ssid)}">
        <span class="scan-bars">${rssiBars(n.rssi)}</span>
        <span class="scan-name">${esc(n.ssid)}</span>
        <span class="scan-meta">${n.rssi} dBm${n.secure ? '' : ' · open'}</span>
      </button>`).join('');
    box.hidden = nets.length === 0;

    $$('#scan-results .scan-row').forEach((row) => {
      row.addEventListener('click', () => {
        $('#set-wifiSsid').value = row.dataset.ssid;
        $$('#scan-results .scan-row').forEach((r2) => r2.classList.remove('sel'));
        row.classList.add('sel');
        $('#set-wifiPass').focus();
      });
    });

    $('#scan-status').textContent = nets.length
      ? `${nets.length} found — tap one to use it`
      : 'no networks found';
  } catch (e) {
    $('#scan-status').textContent = '';
    toast(e.message, 'bad');
  } finally {
    btn.disabled = false;
  }
});

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


/* ----------------------------------------------------------------- library */

let LIB_LOADED = false;

async function loadLibrary() {
  // 65 protocols never change at runtime, so fetch the catalogue once.
  if (LIB_LOADED) return;
  try {
    const r = await api('/api/library/protocols');
    $('#lib-protocol').innerHTML = r.protocols
      .map((p) => `<option value="${esc(p.name)}">${esc(p.name)}</option>`).join('');
    // GREE is a sensible landing spot: it is one of the most widely rebadged
    // controllers, so it is a good first guess for an unbranded unit.
    const gree = r.protocols.find((p) => p.name === 'GREE');
    if (gree) $('#lib-protocol').value = 'GREE';
    LIB_LOADED = true;
    libPreview();
  } catch (e) { toast(e.message, 'bad'); }
}

function libBody(extra) {
  return Object.assign({
    protocol: $('#lib-protocol').value,
    power: $('#lib-power').checked,
    mode: $('#lib-mode').value,
    degrees: +$('#lib-degrees').value,
    fan: $('#lib-fan').value,
    turbo: $('#lib-turbo').checked,
    econo: $('#lib-econo').checked,
    light: $('#lib-light').checked
  }, extra || {});
}

// The device's own description is authoritative -- it reflects what the
// encoder actually understood, which is not always what the form says.
let libTimer = null;
function libPreview() {
  clearTimeout(libTimer);
  libTimer = setTimeout(async () => {
    try {
      const r = await post('/api/library/ac/preview', libBody());
      $('#lib-summary').textContent = r.summary;
      const hexEl = $('#lib-hex');
      if (hexEl) {
        hexEl.textContent = r.hex || '';
        hexEl.hidden = !$('#lib-show-hex')?.checked || !r.hex;
      }
      $('#lib-status').textContent = '';
    } catch (e) {
      $('#lib-summary').textContent = '—';
      const hexEl = $('#lib-hex');
      if (hexEl) { hexEl.textContent = ''; hexEl.hidden = true; }
      $('#lib-status').textContent = e.message;
    }
  }, 200);
}

['#lib-protocol', '#lib-mode', '#lib-fan', '#lib-degrees', '#lib-power',
 '#lib-turbo', '#lib-econo', '#lib-light'].forEach((sel) => {
  const el = $(sel);
  if (!el) return;
  el.addEventListener('input', () => {
    $('#lib-degrees-val').textContent = $('#lib-degrees').value + '\u00B0C';
    libPreview();
  });
});

$('#lib-show-hex')?.addEventListener('change', () => {
  const hexEl = $('#lib-hex');
  if (hexEl) hexEl.hidden = !$('#lib-show-hex').checked || !hexEl.textContent;
});

onClick('#btn-lib-send', async () => {
  const btn = $('#btn-lib-send');
  btn.disabled = true;
  try {
    const r = await post('/api/library/ac/send', libBody());
    toast('Sent ' + r.summary, 'ok');
  } catch (e) { toast(e.message, 'bad'); }
  finally { btn.disabled = false; }
});

onClick('#btn-lib-save', async () => {
  const name = ($('#lib-name').value || '').trim();
  if (!name) { toast('Give it a name first', 'bad'); return; }
  try {
    await post('/api/library/ac/save',
               libBody({ name, group: $('#lib-group').value.trim() }));
    $('#lib-name').value = '';
    await loadCommands();
    toast('Saved "' + name + '"', 'ok');
  } catch (e) { toast(e.message, 'bad'); }
});

onClick('#btn-lib-bulk', async () => {
  const from = +$('#lib-from').value, to = +$('#lib-to').value;
  const prefix = ($('#lib-prefix').value || 'AC').trim();
  const group = $('#lib-group').value.trim();
  if (!(from >= 16 && to <= 30 && from <= to)) {
    toast('Range must be between 16 and 30, low to high', 'bad');
    return;
  }
  const total = to - from + 1;
  if (!confirm(`Create ${total} commands, "${prefix} 16" through "${prefix} ${to}"?`)) return;

  const btn = $('#btn-lib-bulk');
  const bar = $('#lib-bar'), log = $('#lib-log');
  btn.disabled = true; bar.hidden = false; log.hidden = false; log.textContent = '';

  let ok = 0, failed = 0, i = 0;
  for (let t = from; t <= to; t++) {
    const name = `${prefix} ${t}`;
    try {
      // One request per degree: the device encodes each state itself, so this
      // stays correct even for protocols with odd temperature rules.
      await post('/api/library/ac/save',
                 libBody({ degrees: t, name, group }));
      ok++;
    } catch (e) {
      failed++;
      log.textContent += `${name}: ${e.message}\n`;
    }
    i++;
    $('#lib-bar-fill').style.width = (100 * i / total) + '%';
  }
  log.textContent += `\nDone: ${ok} created, ${failed} skipped.`;
  btn.disabled = false;
  await loadCommands();
  toast(`Created ${ok} of ${total}`, failed ? 'bad' : 'ok');
});

/* -------------------------------------------------------------- library: tv */

/* The two halves of the Library tab are different enough to keep apart: the
   A/C pane builds a state and previews it, while the TV pane is a code table
   and a set of buttons. They share only the sub-tab strip. */

$$('#lib-subtabs .subtab').forEach((btn) => {
  btn.addEventListener('click', () => {
    $$('#lib-subtabs .subtab').forEach((b) => b.classList.toggle('active', b === btn));
    const which = btn.dataset.sub;
    $$('.lib-pane').forEach((p) => {
      p.classList.toggle('active', p.id === 'lib-pane-' + which);
    });
    if (which === 'tv') loadTvLibrary();
  });
});

let TV = { models: [], buttons: [], loaded: false };

/* The six the user is entitled to expect on any remote. Anything else the
   model happens to know is drawn underneath as an extra. */
const TV_CORE = ['power', 'vol_up', 'vol_down', 'ch_up', 'ch_down'];

async function loadTvLibrary() {
  if (TV.loaded) return;
  try {
    const r = await api('/api/library/tv/models');
    // The firmware emits them in table order; a dropdown wants them by name.
    TV.models = (r.models || []).sort((a, b) =>
      (a.brand + ' ' + a.model).localeCompare(b.brand + ' ' + b.model));
    TV.buttons = r.buttons || [];
    TV.loaded = true;

    if (!TV.models.length) {
      $('#tv-model').innerHTML = '<option value="">No TV models built in yet</option>';
      $('#tv-note').textContent =
        'No TV codes are compiled into this firmware. Capture a remote on the ' +
        'Learn tab and the codes can be added to the built-in table.';
      $('#tv-note').hidden = false;
      return;
    }

    $('#tv-model').innerHTML = TV.models
      .map((m) => `<option value="${esc(m.id)}">${esc(m.brand)} — ${esc(m.model)}</option>`)
      .join('');
    renderTvRemote();
  } catch (e) { toast(e.message, 'bad'); }
}

function tvCurrentModel() {
  const id = $('#tv-model').value;
  return TV.models.find((m) => m.id === id) || null;
}

function renderTvRemote() {
  const m = tvCurrentModel();
  if (!m) return;

  $('#tv-note').textContent = m.note || '';
  $('#tv-note').hidden = !m.note;
  $('#tv-remote').hidden = false;

  // Core buttons live in fixed positions in the markup, so they are only
  // enabled or disabled here -- never moved.
  TV_CORE.forEach((id) => {
    const el = $(`.tvb[data-btn="${id}"]`);
    if (!el) return;
    const known = !!m.buttons[id];
    el.disabled = !known;
    el.title = known ? tvLabel(id) : tvLabel(id) + ' — not captured yet';
  });

  // Anything else this model knows, drawn as extras. Unknown extras are not
  // rendered at all: a grid of thirteen dead buttons is noise, whereas a
  // missing volume-down is information.
  const extras = TV.buttons
    .filter((b) => !TV_CORE.includes(b.id) && m.buttons[b.id])
    .map((b) => `<button class="tvb" data-btn="${esc(b.id)}" title="${esc(b.label)}">
                   <span class="tvb-glyph">${esc(b.label)}</span></button>`)
    .join('');
  $('#tv-extras').innerHTML = extras;

  const missing = TV_CORE.filter((id) => !m.buttons[id]).map(tvLabel);
  const el = $('#tv-missing');
  if (missing.length) {
    el.innerHTML = '<b>Not captured yet:</b> ' + esc(missing.join(', ')) +
      '. Those codes cannot be derived — they are assigned per manufacturer, ' +
      'so they have to come off the real remote. Capture each one on the ' +
      'Learn tab and it can be added to the built-in table.';
    el.hidden = false;
  } else {
    el.hidden = true;
  }
}

function tvLabel(id) {
  const b = TV.buttons.find((x) => x.id === id);
  return b ? b.label : id;
}

$('#tv-model')?.addEventListener('change', renderTvRemote);

/* One delegated listener, so extras added after render still work. */
$('#tv-remote')?.addEventListener('click', async (ev) => {
  const btn = ev.target.closest('.tvb');
  if (!btn || btn.disabled) return;
  const m = tvCurrentModel();
  if (!m) return;

  const button = btn.dataset.btn;
  btn.classList.remove('failed');
  // Re-trigger the pulse even on a rapid second press.
  btn.classList.remove('sending');
  void btn.offsetWidth;
  btn.classList.add('sending');

  try {
    const r = await post('/api/library/tv/send', { model: m.id, button });
    $('#tv-caption').textContent = 'Sent ' + r.label;
  } catch (e) {
    btn.classList.add('failed');
    $('#tv-caption').textContent = e.message;
    toast(e.message, 'bad');
  } finally {
    setTimeout(() => btn.classList.remove('sending'), 450);
  }
});

onClick('#btn-tv-save-all', async () => {
  const m = tvCurrentModel();
  if (!m) return;

  const known = TV.buttons.filter((b) => m.buttons[b.id]);
  if (!known.length) { toast('This model has no captured codes yet', 'bad'); return; }
  if (!confirm(`Save ${known.length} button(s) as commands, named "${m.brand} ..."?`)) return;

  const btn = $('#btn-tv-save-all');
  const status = $('#tv-save-status');
  btn.disabled = true;

  let ok = 0, failed = 0;
  for (const b of known) {
    status.textContent = `Saving ${b.label}...`;
    try {
      await post('/api/library/tv/save', {
        model: m.id, button: b.id,
        name: `${m.brand} ${b.label}`, group: $('#tv-group').value.trim() || 'TV'
      });
      ok++;
    } catch (e) { failed++; }
  }

  status.textContent = `${ok} saved` + (failed ? `, ${failed} skipped` : '');
  btn.disabled = false;
  await loadCommands();
  toast(`Saved ${ok} of ${known.length}`, failed ? 'bad' : 'ok');
});

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
