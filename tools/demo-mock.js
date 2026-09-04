/* Stand-in for the device, used only by the GitHub Pages demo.
 *
 * It replaces window.fetch with canned responses so the real, unmodified
 * data/app.js can be driven in a browser with no hardware attached. Nothing
 * here ships to the ESP32 -- see tools/make-demo.sh.
 */
'use strict';

(function () {
  const CMDS = [
    { id: '00000001', name: 'AC_Cool_24_Auto', group: 'Living Room AC', protocol: 'UNKNOWN', bits: 0, raw: 583, frames: 2, khz: 38, repeats: 1, forceRaw: true,  created: 1772668800 },
    { id: '00000002', name: 'AC_Off',          group: 'Living Room AC', protocol: 'UNKNOWN', bits: 0, raw: 291, frames: 1, khz: 38, repeats: 1, forceRaw: true,  created: 1772668800 },
    { id: '00000003', name: 'AC_Heat_22_Auto', group: 'Living Room AC', protocol: 'UNKNOWN', bits: 0, raw: 583, frames: 2, khz: 38, repeats: 1, forceRaw: false, created: 1772668800 },
    { id: '00000004', name: 'TV_Power',        group: 'Bedroom TV',     protocol: 'NEC',     bits: 32, raw: 67, frames: 1, khz: 38, repeats: 1, forceRaw: false, created: 1772668800 },
    { id: '00000005', name: 'TV_Volume_Up',    group: 'Bedroom TV',     protocol: 'NEC',     bits: 32, raw: 67, frames: 1, khz: 38, repeats: 3, forceRaw: false, created: 1772668800 },
    { id: '00000006', name: 'TV_Input_HDMI1',  group: 'Bedroom TV',     protocol: 'SAMSUNG', bits: 32, raw: 71, frames: 1, khz: 38, repeats: 1, forceRaw: false, created: 0 },
    { id: '00000007', name: 'Fan_Speed',       group: 'Study Fan',      protocol: 'RC5',     bits: 13, raw: 23, frames: 1, khz: 36, repeats: 2, forceRaw: false, created: 0 },
    { id: '00000008', name: 'Fan_Off',         group: 'Study Fan',      protocol: 'RC5',     bits: 13, raw: 23, frames: 1, khz: 36, repeats: 2, forceRaw: false, created: 0 }
  ];

  const bootMs = Date.now();

  const status = () => ({
    ok: true, fw: '1.0.0', host: 'ir-blaster', mdns: 'ir-blaster.local',
    ip: '192.168.1.42', wifi: 'connected', ssid: 'HomeNet', rssi: -52,
    portal: false, apSsid: 'IR-Blaster-A1B2C3',
    uptime: 864000 + Math.floor((Date.now() - bootMs) / 1000),
    heap: 148320, heapMin: 131044, heapMax: 110592,
    fsUsed: 61440, fsTotal: 917504,
    commands: CMDS.length, maxCommands: 128, schedules: 2,
    lastSent: state.lastSent, lastSentAgo: 12,
    txCount: state.txCount, rxCount: 96,
    mqtt: 'connected', mqttError: '',
    time: new Date().toISOString().replace('T', ' ').slice(0, 19),
    resetReason: 1, learn: state.learn.state, monitor: false, authEnabled: false
  });

  const state = {
    lastSent: 'AC_Cool_24_Auto',
    txCount: 1841,
    learn: { state: 'idle', error: '', remainingMs: 0, protocol: '', bits: 0,
             raw: 0, frames: 0, value: '0x0', lastSeen: '' },
    learnDeadline: 0,
    schedules: [
      { id: 1, enabled: true,  hour: 23, minute: 0,  dow: 127, repeats: 0, cmd: '00000002', label: 'Bedtime AC off' },
      { id: 2, enabled: false, hour: 7,  minute: 30, dow: 62,  repeats: 2, cmd: '00000001', label: 'Morning cool' }
    ],
    nextSchedId: 3
  };

  const settings = {
    ok: true, hostname: 'ir-blaster', wifiSsid: 'HomeNet', wifiPassSet: true,
    apPassSet: true, authEnabled: false, authUser: 'admin', authPassSet: false,
    mqttEnabled: true, mqttHost: '192.168.1.10', mqttPort: 1883,
    mqttUser: 'homeassistant', mqttPassSet: true, mqttBase: 'irblaster',
    haDiscovery: true, haPrefix: 'homeassistant',
    tz: 'PKT-5', ntp1: 'pool.ntp.org', ntp2: 'time.nist.gov',
    defaultRepeats: 1, repeatGapMs: 40, frameGapMs: 25, defaultFreqKhz: 38,
    markExcessUs: 0, learnTimeoutMs: 20000, ledEnabled: true
  };

  const LOGS = [
    '0.312 I === ESP32 Universal IR Blaster 1.0.0 ===',
    '0.315 I boot: reset reason = power-on',
    '0.402 I settings: loaded (ssid=\'HomeNet\')',
    '0.688 I store: 8 commands, 14208 bytes',
    '0.691 I ir: rx=14 tx=4 buffer=1024',
    '0.702 I sched: 2 schedules loaded',
    '0.940 I wifi: connecting to \'HomeNet\'',
    '1.104 I http: listening on port 80',
    '3.882 I wifi: connected, ip=192.168.1.42 rssi=-52',
    '3.890 I mdns: http://ir-blaster.local',
    '4.220 I ntp: time is 2026-09-03 09:14:20',
    '4.512 I mqtt: connected to 192.168.1.10:1883',
    '4.690 I mqtt: published discovery for 8 commands',
    '4.701 I wdt: armed at 20 s',
    '4.703 I boot: ready, free heap 168320 bytes'
  ];

  function groupsOf(list) {
    return Array.from(new Set(list.map((c) => c.group))).sort();
  }

  function handle(path, opts) {
    const body = opts && opts.body ? JSON.parse(opts.body) : {};

    switch (path) {
      case '/api/status':   return status();
      case '/api/commands': return { ok: true, groups: groupsOf(CMDS), commands: CMDS };
      case '/api/settings': return settings;
      case '/api/logs':     return { ok: true, lines: LOGS };

      case '/api/commands/send': {
        const c = CMDS.find((x) => x.id === body.id || x.name === body.name);
        if (!c) return { ok: false, error: 'unknown command' };
        state.lastSent = c.name;
        state.txCount++;
        return { ok: true, sent: c.name };
      }

      case '/api/commands/rename': {
        const c = CMDS.find((x) => x.id === body.id);
        if (!c) return { ok: false, error: 'unknown command' };
        c.name = body.name;
        c.group = body.group || 'Ungrouped';
        return { ok: true };
      }

      case '/api/commands/options': {
        const c = CMDS.find((x) => x.id === body.id);
        if (!c) return { ok: false, error: 'unknown command' };
        c.repeats = body.repeats;
        c.khz = body.khz;
        c.forceRaw = body.forceRaw;
        return { ok: true };
      }

      case '/api/commands/delete': {
        const i = CMDS.findIndex((x) => x.id === body.id);
        if (i < 0) return { ok: false, error: 'unknown command' };
        CMDS.splice(i, 1);
        return { ok: true };
      }

      case '/api/learn/start':
        // Pretend a remote is pressed a moment after arming.
        state.learnDeadline = Date.now() + 20000;
        state.learn = { state: 'waiting', error: '', remainingMs: 20000,
                        protocol: '', bits: 0, raw: 0, frames: 0,
                        value: '0x0', lastSeen: '' };
        setTimeout(() => {
          if (state.learn.state !== 'waiting') return;
          state.learn = { state: 'captured', error: '', remainingMs: 0,
                          protocol: 'NEC', bits: 32, raw: 67, frames: 1,
                          value: '0x20DF10EF', lastSeen: '' };
        }, 2600);
        return { ok: true };

      case '/api/learn/cancel':
        state.learn = { state: 'idle', error: '', remainingMs: 0, protocol: '',
                        bits: 0, raw: 0, frames: 0, value: '0x0', lastSeen: '' };
        return { ok: true };

      case '/api/learn/state': {
        if (state.learn.state === 'waiting') {
          const left = state.learnDeadline - Date.now();
          if (left <= 0) {
            state.learn = { state: 'timeout',
                            error: 'no IR signal received before the timeout',
                            remainingMs: 0, protocol: '', bits: 0, raw: 0,
                            frames: 0, value: '0x0', lastSeen: '' };
          } else {
            state.learn.remainingMs = left;
          }
        }
        return Object.assign({ ok: true }, state.learn);
      }

      case '/api/learn/test': return { ok: true };

      case '/api/learn/save': {
        if (state.learn.state !== 'captured') return { ok: false, error: 'nothing captured' };
        if (!body.name) return { ok: false, error: 'name required' };
        if (CMDS.some((c) => c.name.toLowerCase() === body.name.toLowerCase()))
          return { ok: false, error: 'a command with that name exists' };
        const id = String(CMDS.length + 1).padStart(8, '0');
        CMDS.push({ id, name: body.name, group: body.group || 'Ungrouped',
                    protocol: state.learn.protocol, bits: state.learn.bits,
                    raw: state.learn.raw, frames: state.learn.frames, khz: 38,
                    repeats: 1, forceRaw: false,
                    created: Math.floor(Date.now() / 1000) });
        state.learn = { state: 'idle', error: '', remainingMs: 0, protocol: '',
                        bits: 0, raw: 0, frames: 0, value: '0x0', lastSeen: '' };
        return { ok: true, id };
      }

      case '/api/schedules':
        if (!opts || opts.method !== 'POST') {
          return { ok: true,
                   now: new Date().toISOString().replace('T', ' ').slice(0, 19),
                   schedules: state.schedules };
        }
        if (body.id) {
          const s = state.schedules.find((x) => x.id === body.id);
          if (!s) return { ok: false, error: 'unknown schedule' };
          Object.assign(s, body);
          return { ok: true, id: s.id };
        }
        if (!body.cmd) return { ok: false, error: 'unknown command' };
        state.schedules.push(Object.assign({}, body, { id: state.nextSchedId }));
        return { ok: true, id: state.nextSchedId++ };

      case '/api/schedules/delete': {
        const i = state.schedules.findIndex((x) => x.id === body.id);
        if (i < 0) return { ok: false, error: 'unknown schedule' };
        state.schedules.splice(i, 1);
        return { ok: true };
      }

      case '/api/wifi/scan':
        return { ok: true, networks: [
          { ssid: 'HomeNet', rssi: -52, secure: true },
          { ssid: 'HomeNet-5G', rssi: -61, secure: true },
          { ssid: 'Neighbour-2.4', rssi: -78, secure: true },
          { ssid: 'GuestWiFi', rssi: -80, secure: false }
        ] };

      case '/api/monitor': return { ok: true };

      case '/api/library/protocols':
        return { ok: true, protocols: [
          { id: 24, name: 'COOLIX' }, { id: 26, name: 'DAIKIN' },
          { id: 32, name: 'GREE' },   { id: 41, name: 'MIDEA' },
          { id: 55, name: 'TCL112AC' }, { id: 58, name: 'SAMSUNG_AC' }] };

      case '/api/library/ac/preview':
      case '/api/library/ac/send':
      case '/api/library/ac/save': {
        const s = (body.protocol || 'GREE') + ' · ' + (body.power ? 'on' : 'off') +
          ' · ' + (body.mode || 'cool') + ' · ' + (body.degrees || 24) + 'C' +
          ' · fan ' + (body.fan || 'auto');
        if (path.endsWith('/save')) {
          if (!body.name) return { ok: false, error: 'name required to save' };
          const id = String(CMDS.length + 1).padStart(8, '0');
          CMDS.push({ id, name: body.name, group: body.group || 'Air Conditioner',
                      protocol: body.protocol, bits: 448, raw: 56, frames: 1,
                      khz: 38, repeats: 1, forceRaw: false,
                      created: Math.floor(Date.now() / 1000) });
          return { ok: true, summary: s, id };
        }
        return { ok: true, summary: s };
      }

      case '/api/selftest':
        return {
          ok: true, pass: true, rxIdleOk: true,
          idleLowSamples: 0, idleSamples: 200,
          received: true, matched: true, attempts: 1,
          verdict: 'transmitter and receiver are both working',
          expectedProtocol: 'NEC', expectedValue: '0x20DF10EF',
          protocol: 'NEC', value: '0x20DF10EF', bits: 32, raw: 67
        };

      case '/api/system/factory-reset':
      case '/api/system/reboot':
      case '/api/system/portal':
      case '/api/mqtt/discovery':
      case '/api/import':
        return { ok: true };

      default:
        return { ok: false, error: 'demo: ' + path + ' is not simulated' };
    }
  }

  window.fetch = function (path, opts) {
    const p = String(path).split('?')[0];
    let body;
    try {
      body = handle(p, opts);
    } catch (e) {
      body = { ok: false, error: 'demo error: ' + e.message };
    }
    // A little latency, so the UI's loading states are actually exercised.
    return new Promise((resolve) => {
      setTimeout(() => resolve({
        ok: true, status: 200,
        text: () => Promise.resolve(JSON.stringify(body))
      }), 60);
    });
  };

  // Settings are not persisted in the demo, and saving them would be a lie.
  window.addEventListener('DOMContentLoaded', () => {
    const bar = document.createElement('div');
    bar.style.cssText =
      'background:#f59e0b;color:#12151a;font:600 13px/1.45 system-ui,sans-serif;' +
      'padding:9px 16px;text-align:center';
    bar.innerHTML =
      'Interactive demo &mdash; no device attached. Data is simulated in your ' +
      'browser and resets on reload. ' +
      '<a href="https://github.com/hammad42/esp32-ir-blaster" ' +
      'style="color:#12151a">Source on GitHub</a>';
    document.body.insertBefore(bar, document.body.firstChild);
  });
})();
