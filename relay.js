// relay.js - relay for Cays Sanity ArcDPS Cooldown HUD
//
// Endpoints:
//  POST /update
//    {
//      room,             // string, e.g. "bags"
//      clientId,         // string, unique per client
//      name,             // optional display name
//      prof,             // profession id (int)
//      pluginVer,        // string, e.g. "0.90"
//      subgroup,         // squad subgroup index (int, 0 if none)
//      entries: [        // cooldown entries
//        { label, ready, left, skillid }
//      ],
//      groupOrder: {     // optional, per-profession order
//        "1": ["clientA", "clientB"],
//        "7": ["clientX", "clientY"]
//      }
//    }
//
//  GET /aggregate?room=bags
//    {
//      room,
//      peers: [{
//        clientId,
//        name,
//        prof,
//        pluginVer,
//        subgroup,
//        entries: [{ label, ready, left, skillid }]
//      }],
//      groupOrder?: { "1": [...], ... }
//    }
//
//  GET /health -> { ok: true }
//
//  GET /download/arcdps_cooldowns.dll
//    - downloads local DLL file
//
//  GET /
//    - HTML status page

const express = require('express');
const path = require('path');

const app = express();
app.use(express.json({ limit: '64kb' }));

// roomName -> Map(clientId -> { name, prof, pluginVer, subgroup, entries, ts })
const rooms = new Map();

// roomName -> { [prof]: [clientId, ...] }
const roomOrders = new Map();

// optional: assign default names like "spirit 1", "spirit 2"
function assignName(room, provided) {
  if (provided && provided.trim().length) return provided;
  const used = new Set();
  const m = rooms.get(room);
  if (m) {
    for (const v of m.values()) {
      used.add((v.name || '').trim().toLowerCase());
    }
  }
  let i = 1;
  while (used.has(`spirit ${i}`)) i++;
  return `spirit ${i}`;
}

function getRoom(room) {
  if (!rooms.has(room)) rooms.set(room, new Map());
  return rooms.get(room);
}

// prune clients not updated in 15s
function prune() {
  const cutoff = Date.now() - 15000;
  for (const m of rooms.values()) {
    for (const [cid, v] of m.entries()) {
      if (v.ts < cutoff) m.delete(cid);
    }
  }
}

app.post('/update', (req, res) => {
  const {
    room = 'bags',
    clientId,
    name,
    entries,
    prof,
    pluginVer,
    subgroup,
    groupOrder
  } = req.body || {};

  if (!clientId || !Array.isArray(entries)) {
    return res.status(400).json({ ok: false, err: 'bad payload' });
  }

  const m = getRoom(room);
  const fixedName = assignName(room, name);

  m.set(clientId, {
    name: fixedName,
    prof: Number.isInteger(prof) ? prof : 0,
    pluginVer: typeof pluginVer === 'string' ? pluginVer : null,
    subgroup: Number.isInteger(subgroup) ? subgroup : 0,
    entries: entries.map(e => ({
      label: String(e.label || ''),
      ready: !!e.ready,
      left: typeof e.left === 'number' ? e.left : null,
      skillid: typeof e.skillid === 'number' ? e.skillid : 0
    })),
    ts: Date.now()
  });

  // If this payload includes a groupOrder, treat it as the shared order for this room
  if (groupOrder && typeof groupOrder === 'object') {
    roomOrders.set(room, groupOrder);
  }

  prune();
  res.json({ ok: true, assignedName: fixedName });
});

app.get('/aggregate', (req, res) => {
  const room = req.query.room || 'bags';
  const m = getRoom(room);
  prune();

  const peers = [];
  for (const [clientId, v] of m.entries()) {
    peers.push({
      clientId,
      name: v.name || 'unknown',
      prof: v.prof || 0,
      pluginVer: v.pluginVer || null,
      subgroup: v.subgroup || 0,
      entries: v.entries || []
    });
  }

  const body = { room, peers };
  const order = roomOrders.get(room);
  if (order) {
    body.groupOrder = order;
  }

  res.json(body);
});

app.get('/health', (_req, res) => res.json({ ok: true }));

// download local arcdps_cooldowns.dll
app.get('/download/arcdps_cooldowns.dll', (req, res) => {
  const filePath = path.join(__dirname, 'arcdps_cooldowns.dll');
  res.download(filePath, 'arcdps_cooldowns.dll', err => {
    if (err) {
      console.error('Error sending arcdps_cooldowns.dll:', err);
      if (!res.headersSent) res.sendStatus(404);
    }
  });
});

// simple HTML status page with plugin version + subgroup + download link
// simple HTML status page with plugin version + subgroup + download link
// simple HTML status page with plugin version + subgroup + download link
app.get('/', (req, res) => {
  prune();

  if (!rooms.has('bags')) rooms.set('bags', new Map());

  const now = Date.now();
  const LAST_SEEN_LIVE = 15000;   // ms threshold for "live"
  const LAST_SEEN_STALE = 45000;  // ms threshold for "stale"

  const data = [];

  for (const [roomName, m] of rooms.entries()) {
    const peers = [];

    for (const [clientId, v] of m.entries()) {
      const lastSeenMsAgo = now - v.ts;
      let status = 'offline';
      if (lastSeenMsAgo <= LAST_SEEN_LIVE) {
        status = 'live';
      } else if (lastSeenMsAgo <= LAST_SEEN_STALE) {
        status = 'stale';
      }

      peers.push({
        clientId,
        name: v.name || 'unknown',
        prof: v.prof || 0,
        pluginVer: v.pluginVer || null,
        subgroup: v.subgroup || 0,
        entriesCount: (v.entries || []).length,
        lastSeenMsAgo,
        status
      });
    }

    let live = 0, stale = 0, offline = 0;
    for (const p of peers) {
      if (p.status === 'live') live++;
      else if (p.status === 'stale') stale++;
      else offline++;
    }

    let relayStatus = 'Offline';
    if (!peers.length) {
      relayStatus = 'Offline';
    } else if (live > 0 && offline === 0 && stale <= Math.max(1, peers.length / 3)) {
      relayStatus = 'Live';
    } else if (live === 0 && stale === 0) {
      relayStatus = 'Offline';
    } else {
      relayStatus = 'Mixed';
    }

    data.push({ room: roomName, peers, relayStatus });
  }

  const totalRelays = data.length;
  let totalClients = 0;
  let liveClients = 0;
  const latencies = [];

  for (const r of data) {
    for (const p of r.peers) {
      totalClients++;
      if (p.status === 'live') liveClients++;
      if (typeof p.lastSeenMsAgo === 'number') latencies.push(p.lastSeenMsAgo);
    }
  }

  const avgLatencyMs = latencies.length
    ? Math.round(latencies.reduce((a, b) => a + b, 0) / latencies.length)
    : null;

  const latencyText = avgLatencyMs == null
    ? 'Latency: waiting for clients…'
    : `Latency: ~${avgLatencyMs} ms average`;

  const clientsSubtitle = totalClients === 0
    ? '(no clients connected)'
    : `(${totalClients} client${totalClients === 1 ? '' : 's'} across ${totalRelays} relay${totalRelays === 1 ? '' : 's'})`;

  const serverTime = new Date().toISOString().replace('T', ' ').slice(0, 16);

  function escHtml(str) {
    return String(str == null ? '' : str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  const html = `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8" />
  <title>Relay Status 🟩</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <style>
    :root {
      --bg: #050816;
      --bg-elevated: #0b1020;
      --bg-elevated-softer: #111827;
      --border-subtle: rgba(148, 163, 184, 0.18);
      --accent: #22c55e;
      --accent-soft: rgba(34, 197, 94, 0.15);
      --accent-alt: #38bdf8;
      --text: #e5e7eb;
      --text-soft: #9ca3af;
      --danger: #fb7185;
      --warning: #facc15;
      --radius-lg: 18px;
      --radius-xl: 26px;
      --shadow-soft: 0 18px 45px rgba(15, 23, 42, 0.85);
      --shadow-subtle: 0 8px 24px rgba(15, 23, 42, 0.7);
      --transition-fast: 160ms ease-out;
      --transition-med: 220ms ease-out;
    }

    * {
      box-sizing: border-box;
    }

    html, body {
      margin: 0;
      padding: 0;
      font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: radial-gradient(circle at top, #1d293b 0, #020617 40%, #020617 100%);
      color: var(--text);
      min-height: 100%;
      -webkit-font-smoothing: antialiased;
      text-rendering: optimizeLegibility;
    }

    body::before {
      content: "";
      position: fixed;
      inset: 0;
      background:
        radial-gradient(circle at 0% 0%, rgba(56, 189, 248, 0.16), transparent 60%),
        radial-gradient(circle at 100% 0%, rgba(34, 197, 94, 0.12), transparent 55%);
      opacity: 0.9;
      pointer-events: none;
      z-index: -1;
    }

    body {
      padding: 24px;
      display: flex;
      justify-content: center;
      align-items: flex-start;
    }

    .page {
      width: 100%;
      max-width: 1120px;
    }

    /* HERO */

    .hero {
      display: grid;
      grid-template-columns: minmax(0, 3fr) minmax(0, 2.4fr);
      gap: 32px;
      margin-bottom: 32px;
      align-items: stretch;
    }

    @media (max-width: 900px) {
      .hero {
        grid-template-columns: 1fr;
      }
    }

    .hero-main {
      background: linear-gradient(135deg, rgba(15, 23, 42, 0.96), rgba(15, 23, 42, 0.96));
      border-radius: var(--radius-xl);
      padding: 28px 26px 26px;
      box-shadow: var(--shadow-soft);
      position: relative;
      overflow: hidden;
    }

    .hero-main::before {
      content: "";
      position: absolute;
      inset: -1px;
      border-radius: inherit;
      padding: 1px;
      background: linear-gradient(135deg, rgba(56, 189, 248, 0.7), rgba(34, 197, 94, 0.5), rgba(129, 140, 248, 0.4));
      -webkit-mask:
        linear-gradient(#000 0 0) content-box,
        linear-gradient(#000 0 0);
      -webkit-mask-composite: xor;
      mask-composite: exclude;
      opacity: 0.65;
      pointer-events: none;
    }

    .hero-badge {
      display: inline-flex;
      align-items: center;
      gap: 0.4rem;
      padding: 5px 11px 5px 6px;
      border-radius: 999px;
      background: rgba(15, 118, 110, 0.14);
      border: 1px solid rgba(45, 212, 191, 0.4);
      color: #a5f3fc;
      font-size: 11px;
      letter-spacing: 0.12em;
      text-transform: uppercase;
      margin-bottom: 10px;
    }

    .hero-badge-dot {
      width: 9px;
      height: 9px;
      border-radius: 999px;
      background: radial-gradient(circle at 30% 20%, #bbf7d0, #22c55e);
      box-shadow: 0 0 0 6px rgba(34, 197, 94, 0.22);
    }

    .hero-title {
      font-size: clamp(28px, 4vw, 34px);
      margin: 0 0 6px;
      letter-spacing: -0.03em;
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 0.4rem;
    }

    .hero-title span.status-pill {
      font-size: 20px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      width: 26px;
      height: 26px;
      border-radius: 999px;
      background: rgba(22, 163, 74, 0.16);
      box-shadow: 0 0 0 1px rgba(34, 197, 94, 0.5);
    }

    .hero-subtitle {
      margin: 4px 0 14px;
      color: var(--text-soft);
      font-size: 14px;
      max-width: 480px;
    }

    .hero-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-bottom: 14px;
    }

    .btn {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 0.5rem;
      padding: 9px 16px;
      border-radius: 999px;
      font-size: 13px;
      font-weight: 500;
      border: 1px solid transparent;
      cursor: pointer;
      transition:
        transform var(--transition-fast),
        box-shadow var(--transition-fast),
        background var(--transition-fast),
        border-color var(--transition-fast),
        color var(--transition-fast);
      text-decoration: none;
      white-space: nowrap;
    }

    .btn-primary {
      background: linear-gradient(135deg, #22c55e, #4ade80);
      color: #022c22;
      box-shadow: 0 10px 30px rgba(22, 163, 74, 0.6);
    }

    .btn-primary:hover {
      transform: translateY(-1px);
      box-shadow: 0 14px 40px rgba(22, 163, 74, 0.75);
    }

    .btn-ghost {
      background: rgba(15, 23, 42, 0.9);
      color: var(--text-soft);
      border-color: rgba(148, 163, 184, 0.44);
    }

    .btn-ghost:hover {
      background: rgba(15, 23, 42, 1);
      color: var(--text);
      transform: translateY(-1px);
      box-shadow: var(--shadow-subtle);
    }

    .btn-icon {
      width: 18px;
      height: 18px;
      border-radius: 999px;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      background: rgba(15, 23, 42, 0.6);
      border: 1px solid rgba(15, 23, 42, 0.9);
      font-size: 11px;
    }

    .hero-note {
      margin: 0;
      font-size: 12px;
      color: var(--text-soft);
      line-height: 1.5;
    }

    .hero-note code {
      font-size: 11px;
      background: rgba(15, 23, 42, 0.9);
      padding: 2px 5px;
      border-radius: 999px;
      border: 1px solid rgba(148, 163, 184, 0.5);
      color: #e5e7eb;
    }

    .hero-meta {
      margin-top: 18px;
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      font-size: 11px;
      color: var(--text-soft);
    }

    .hero-meta-pill {
      display: inline-flex;
      align-items: center;
      gap: 0.4rem;
      padding: 5px 10px;
      border-radius: 999px;
      background: rgba(15, 23, 42, 0.9);
      border: 1px solid rgba(148, 163, 184, 0.4);
    }

    .hero-side {
      display: flex;
      flex-direction: column;
      gap: 14px;
    }

    .card {
      background: rgba(15, 23, 42, 0.95);
      border-radius: var(--radius-lg);
      border: 1px solid var(--border-subtle);
      padding: 18px 16px;
      box-shadow: var(--shadow-subtle);
      position: relative;
      overflow: hidden;
    }

    .card::before {
      content: "";
      position: absolute;
      inset: 0;
      background: radial-gradient(circle at 0 0, rgba(56, 189, 248, 0.2), transparent 55%);
      opacity: 0.45;
      pointer-events: none;
    }

    .stat-grid {
      display: grid;
      grid-template-columns: repeat(3, minmax(0, 1fr));
      gap: 10px;
    }

    @media (max-width: 500px) {
      .stat-grid {
        grid-template-columns: repeat(2, minmax(0, 1fr));
      }
    }

    .stat {
      padding: 10px 10px 9px;
      border-radius: 14px;
      background: rgba(15, 23, 42, 0.88);
      border: 1px solid rgba(148, 163, 184, 0.35);
      position: relative;
      overflow: hidden;
    }

    .stat-label {
      font-size: 11px;
      color: var(--text-soft);
      margin-bottom: 4px;
    }

    .stat-value {
      font-size: 18px;
      font-weight: 600;
      letter-spacing: -0.03em;
      display: flex;
      align-items: baseline;
      gap: 0.25rem;
    }

    .stat-sub {
      font-size: 10px;
      color: var(--text-soft);
      margin-top: 3px;
    }

    .latency-pill {
      display: inline-flex;
      align-items: center;
      padding: 4px 8px;
      border-radius: 999px;
      background: rgba(15, 23, 42, 0.9);
      border: 1px solid rgba(148, 163, 184, 0.45);
      font-size: 11px;
      gap: 0.45rem;
      color: var(--text-soft);
    }

    .latency-indicator {
      width: 9px;
      height: 9px;
      border-radius: 999px;
      background: linear-gradient(to bottom, #22c55e, #16a34a);
      box-shadow: 0 0 0 4px rgba(22, 163, 74, 0.35);
    }

    .stat-chip {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      padding: 3px 7px;
      border-radius: 999px;
      border: 1px solid rgba(52, 211, 153, 0.7);
      background: rgba(6, 95, 70, 0.65);
      color: #a7f3d0;
      font-size: 10px;
      text-transform: uppercase;
      letter-spacing: 0.09em;
      gap: 0.25rem;
    }

    .stat-chip.dot::before {
      content: "";
      width: 6px;
      height: 6px;
      border-radius: 10px;
      background: radial-gradient(circle at 30% 20%, #bbf7d0, #22c55e);
    }

    /* SECTION / TABLES */

    .section {
      background: rgba(15, 23, 42, 0.97);
      border-radius: var(--radius-xl);
      border: 1px solid var(--border-subtle);
      padding: 22px 20px 18px;
      box-shadow: var(--shadow-soft);
      position: relative;
      overflow: hidden;
    }

    .section::before {
      content: "";
      position: absolute;
      inset: 0;
      background: radial-gradient(circle at 100% 0, rgba(56, 189, 248, 0.16), transparent 55%);
      opacity: 0.55;
      pointer-events: none;
    }

    .section-inner {
      position: relative;
      z-index: 1;
    }

    .section-header {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      margin-bottom: 14px;
    }

    .section-title {
      margin: 0;
      font-size: 16px;
      display: flex;
      align-items: center;
      gap: 0.4rem;
    }

    .section-title span {
      font-size: 12px;
      color: var(--text-soft);
      font-weight: 400;
    }

    .section-tools {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
    }

    .field {
      position: relative;
    }

    .field input,
    .field select {
      background: rgba(15, 23, 42, 0.96);
      border-radius: 999px;
      border: 1px solid rgba(148, 163, 184, 0.55);
      color: var(--text);
      font-size: 12px;
      padding: 7px 10px 7px 28px;
      outline: none;
      min-width: 160px;
      transition: border-color var(--transition-med), box-shadow var(--transition-med), background var(--transition-med);
    }

    .field select {
      padding-left: 10px;
    }

    .field input::placeholder {
      color: rgba(148, 163, 184, 0.7);
    }

    .field input:focus,
    .field select:focus {
      border-color: rgba(56, 189, 248, 0.9);
      box-shadow: 0 0 0 1px rgba(56, 189, 248, 0.9), 0 0 30px rgba(8, 47, 73, 1);
      background: #020617;
    }

    .field-icon {
      position: absolute;
      left: 9px;
      top: 50%;
      transform: translateY(-50%);
      font-size: 12px;
      color: rgba(148, 163, 184, 0.8);
      pointer-events: none;
    }

    /* Relay cards and tables */

    .relay-list {
      display: flex;
      flex-direction: column;
      gap: 14px;
    }

    .relay-card {
      border-radius: 18px;
      background: rgba(15, 23, 42, 0.96);
      border: 1px solid rgba(148, 163, 184, 0.5);
      padding: 12px 12px 10px;
      overflow: hidden;
      position: relative;
    }

    .relay-card-header {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      margin-bottom: 8px;
    }

    .relay-name {
      font-size: 13px;
      font-weight: 500;
      display: flex;
      align-items: center;
      gap: 0.4rem;
    }

    .relay-tag {
      font-size: 10px;
      text-transform: uppercase;
      letter-spacing: 0.11em;
      padding: 3px 8px;
      border-radius: 999px;
      border: 1px solid rgba(148, 163, 184, 0.6);
      color: var(--text-soft);
      background: rgba(15, 23, 42, 0.9);
    }

    .relay-count {
      font-size: 11px;
      color: var(--text-soft);
    }

    .status-chip {
      display: inline-flex;
      align-items: center;
      gap: 0.35rem;
      padding: 4px 9px;
      border-radius: 999px;
      font-size: 10px;
      font-weight: 500;
      text-transform: uppercase;
      letter-spacing: 0.08em;
    }

    .status-chip-dot {
      width: 7px;
      height: 7px;
      border-radius: 999px;
      background: currentColor;
    }

    .status-live {
      background: rgba(22, 163, 74, 0.16);
      color: #bbf7d0;
      border: 1px solid rgba(34, 197, 94, 0.75);
    }

    .status-mixed {
      background: rgba(234, 179, 8, 0.12);
      color: #facc15;
      border: 1px solid rgba(250, 204, 21, 0.7);
    }

    .status-offline {
      background: rgba(127, 29, 29, 0.4);
      color: #fecaca;
      border: 1px solid rgba(248, 113, 113, 0.85);
    }

    .relay-table-wrapper {
      border-radius: 14px;
      overflow: hidden;
      border: 1px solid rgba(51, 65, 85, 0.9);
      background: radial-gradient(circle at top, rgba(15, 23, 42, 0.98), rgba(15, 23, 42, 1));
    }

    .relay-table {
      width: 100%;
      border-collapse: collapse;
      font-size: 11px;
    }

    .relay-table thead {
      background: linear-gradient(to right, #020617, #020617);
    }

    .relay-table th,
    .relay-table td {
      padding: 6px 8px;
      text-align: left;
      border-bottom: 1px solid rgba(30, 64, 175, 0.6);
      white-space: nowrap;
    }

    .relay-table th {
      font-size: 10px;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      color: rgba(148, 163, 184, 0.9);
      background: rgba(15, 23, 42, 1);
    }

    .relay-table tbody tr:nth-child(odd) {
      background: rgba(15, 23, 42, 0.88);
    }

    .relay-table tbody tr:nth-child(even) {
      background: rgba(15, 23, 42, 0.95);
    }

    .relay-table tbody tr:hover {
      background: rgba(15, 23, 42, 1);
    }

    .relay-table td.status-cell {
      font-size: 10px;
      font-weight: 500;
    }

    .status-pill {
      border-radius: 999px;
      padding: 3px 7px;
      display: inline-flex;
      align-items: center;
      gap: 0.3rem;
      font-size: 10px;
    }

    .status-pill-dot {
      width: 7px;
      height: 7px;
      border-radius: 999px;
      background: currentColor;
    }

    .status-pill-live {
      background: rgba(22, 163, 74, 0.16);
      color: #4ade80;
      border: 1px solid rgba(34, 197, 94, 0.72);
    }

    .status-pill-stale {
      background: rgba(234, 179, 8, 0.19);
      color: #facc15;
      border: 1px solid rgba(250, 204, 21, 0.85);
    }

    .status-pill-offline {
      background: rgba(127, 29, 29, 0.55);
      color: #fecaca;
      border: 1px solid rgba(248, 113, 113, 0.9);
    }

    .relay-empty {
      font-size: 12px;
      color: var(--text-soft);
      padding: 6px 0 4px;
    }

    /* Responsive table as cards on small screens */

    @media (max-width: 720px) {
      .relay-table,
      .relay-table thead,
      .relay-table tbody,
      .relay-table th,
      .relay-table td,
      .relay-table tr {
        display: block;
      }

      .relay-table thead {
        display: none;
      }

      .relay-table tbody tr {
        margin: 2px 0;
        padding: 6px 8px 5px;
        border-bottom: 1px solid rgba(30, 64, 175, 0.8);
      }

      .relay-table td {
        border: none;
        position: relative;
        padding: 3px 0 3px 86px;
        white-space: normal;
      }

      .relay-table td::before {
        content: attr(data-label);
        position: absolute;
        left: 0;
        top: 2px;
        font-size: 10px;
        font-weight: 500;
        text-transform: uppercase;
        letter-spacing: 0.1em;
        color: rgba(148, 163, 184, 0.9);
      }

      .relay-table td:first-child {
        font-weight: 600;
      }
    }

    /* FOOTER */

    .footer {
      margin-top: 16px;
      font-size: 11px;
      color: rgba(148, 163, 184, 0.7);
      display: flex;
      flex-wrap: wrap;
      justify-content: space-between;
      gap: 8px;
    }

    .footer a {
      color: var(--accent-alt);
      text-decoration: none;
    }

    .footer a:hover {
      text-decoration: underline;
    }
  </style>
</head>
<body>
  <div class="page">
    <header class="hero">
      <div class="hero-main">
        <div class="hero-badge">
          <span class="hero-badge-dot"></span>
          Relay status · live
        </div>

        <h1 class="hero-title">
          arcdps Cooldowns Relay
          <span class="status-pill">🟩</span>
        </h1>

        <p class="hero-subtitle">
          Monitor who is connected to your Guild&nbsp;Wars&nbsp;2 cooldown relay in real time. Clean overview, low overhead, automatic updates.
        </p>

        <div class="hero-actions">
          <a class="btn btn-primary" href="/download/arcdps_cooldowns.dll" download>
            <span class="btn-icon">⬇</span>
            <span>Download <strong>arcdps_cooldowns.dll</strong></span>
          </a>
          <button class="btn btn-ghost" id="scrollToTable">
            <span class="btn-icon">👁</span>
            View active clients
          </button>
        </div>

        <p class="hero-note">
          Place <code>arcdps_cooldowns.dll</code> in your GW2 main folder next to <code>arcdps.dll</code>.
          The plugin will <strong>auto-update</strong> when new versions are available.
        </p>

        <div class="hero-meta">
          <div class="hero-meta-pill">
            <span>🕒</span>
            <span id="lastUpdatedText">Page rendered at ${escHtml(serverTime)} (server time)</span>
          </div>
          <div class="hero-meta-pill">
            <span>🔄</span>
            Uses the relay to sync squad cooldowns across clients.
          </div>
        </div>
      </div>

      <aside class="hero-side">
        <div class="card">
          <div class="stat-grid">
            <div class="stat">
              <div class="stat-label">Relays</div>
              <div class="stat-value">
                <span id="totalRelays">${totalRelays}</span>
              </div>
              <div class="stat-sub">Configured endpoints</div>
            </div>
            <div class="stat">
              <div class="stat-label">Connected clients</div>
              <div class="stat-value">
                <span id="totalClients">${totalClients}</span>
              </div>
              <div class="stat-sub">Across all relays</div>
            </div>
            <div class="stat">
              <div class="stat-label">Live clients</div>
              <div class="stat-value">
                <span id="liveClients">${liveClients}</span>
              </div>
              <div class="stat-sub">Seen in last 15 seconds</div>
            </div>
          </div>

          <div style="margin-top: 14px; display:flex; align-items:center; justify-content:space-between; gap:8px;">
            <div class="latency-pill">
              <span class="latency-indicator"></span>
              <span id="latencySummary">${escHtml(latencyText)}</span>
            </div>
            <span class="stat-chip dot">
              Live feed
            </span>
          </div>
        </div>
      </aside>
    </header>

    <main class="section" id="relaySection">
      <div class="section-inner">
        <div class="section-header">
          <h2 class="section-title">
            Connected clients
            <span id="clientsSubtitle">${escHtml(clientsSubtitle)}</span>
          </h2>

          <div class="section-tools">
            <div class="field">
              <span class="field-icon">🔍</span>
              <input
                id="filterInput"
                type="search"
                placeholder="Filter by name, profession or client id..."
                autocomplete="off"
              />
            </div>

            <div class="field">
              <select id="relaySelect">
                <option value="all">All relays</option>
                ${data.map((r, idx) => {
                  const label = r.room || `Relay ${idx + 1}`;
                  return '<option value="' + idx + '">' + escHtml(label) + '</option>';
                }).join('')}
              </select>
            </div>
          </div>
        </div>

        <div id="relayList" class="relay-list">
          ${data.map((r, idx) => {
            const roomLabel = r.room || `Relay ${idx + 1}`;
            const peers = r.peers || [];
            const statusClass =
              r.relayStatus === 'Live'
                ? 'status-live'
                : r.relayStatus === 'Mixed'
                ? 'status-mixed'
                : 'status-offline';

            const rows = peers.map((p, i) => {
              let peerStatusLabel = 'Offline';
              let peerStatusClass = 'status-pill-offline';
              if (p.status === 'live') {
                peerStatusLabel = 'Live';
                peerStatusClass = 'status-pill-live';
              } else if (p.status === 'stale') {
                peerStatusLabel = 'Stale';
                peerStatusClass = 'status-pill-stale';
              }

              return `
                <tr>
                  <td data-label="#">${i + 1}</td>
                  <td data-label="ClientId">${escHtml(p.clientId)}</td>
                  <td data-label="Name">${escHtml(p.name)}</td>
                  <td data-label="Prof">${escHtml(p.prof)}</td>
                  <td data-label="Subgroup">${escHtml(p.subgroup || '')}</td>
                  <td data-label="PluginVer">${escHtml(p.pluginVer || '')}</td>
                  <td data-label="Entries">${escHtml(p.entriesCount)}</td>
                  <td data-label="Last seen (ms)">${escHtml(p.lastSeenMsAgo)}</td>
                  <td data-label="Status" class="status-cell">
                    <span class="status-pill ${peerStatusClass}">
                      <span class="status-pill-dot"></span>
                      <span>${peerStatusLabel}</span>
                    </span>
                  </td>
                </tr>
              `;
            }).join('');

            return `
              <article class="relay-card" data-room-index="${idx}">
                <div class="relay-card-header">
                  <div style="display:flex;align-items:center;gap:6px;">
                    <div class="relay-name">${escHtml(roomLabel)}</div>
                    <span class="relay-tag">#${idx + 1}</span>
                  </div>
                  <div style="display:flex;align-items:center;gap:8px;">
                    <div class="relay-count">${peers.length} client${peers.length === 1 ? '' : 's'}</div>
                    <span class="status-chip ${statusClass}">
                      <span class="status-chip-dot"></span>
                      <span>${escHtml(r.relayStatus)}</span>
                    </span>
                  </div>
                </div>
                ${peers.length
                  ? `
                    <div class="relay-table-wrapper">
                      <table class="relay-table">
                        <thead>
                          <tr>
                            <th>#</th>
                            <th>ClientId</th>
                            <th>Name</th>
                            <th>Prof</th>
                            <th>Subgroup</th>
                            <th>PluginVer</th>
                            <th>Entries</th>
                            <th>Last seen (ms)</th>
                            <th>Status</th>
                          </tr>
                        </thead>
                        <tbody>
                          ${rows}
                        </tbody>
                      </table>
                    </div>
                  `
                  : `<p class="relay-empty">No clients connected to this relay.</p>`
                }
              </article>
            `;
          }).join('')}
        </div>

        <p class="relay-empty" id="noResultsMessage" style="display:none;">
          No clients match this filter.
        </p>
      </div>
    </main>

    <footer class="footer">
      <div>
        Relay Status 🟩 · lightweight GW2 cooldown relay monitor.
      </div>
      <div>
        Data reflects clients seen in the last 15 seconds.
      </div>
    </footer>
  </div>

  <script>
    (function () {
      const scrollBtn = document.getElementById('scrollToTable');
      const relaySection = document.getElementById('relaySection');
      if (scrollBtn && relaySection) {
        scrollBtn.addEventListener('click', function () {
          relaySection.scrollIntoView({ behavior: 'smooth', block: 'start' });
        });
      }

      const filterInput = document.getElementById('filterInput');
      const relaySelect = document.getElementById('relaySelect');
      const cards = Array.prototype.slice.call(document.querySelectorAll('.relay-card'));
      const noResults = document.getElementById('noResultsMessage');

      function applyFilter() {
        const term = (filterInput.value || '').trim().toLowerCase();
        const selectedRelay = relaySelect.value;
        let anyVisible = false;

        cards.forEach(function (card) {
          const roomIndex = card.getAttribute('data-room-index');
          if (selectedRelay !== 'all' && selectedRelay !== roomIndex) {
            card.style.display = 'none';
            return;
          }

          const rows = card.querySelectorAll('tbody tr');
          if (!rows.length) {
            // No rows; only hide if there is a text filter
            card.style.display = term ? 'none' : '';
            if (!term) anyVisible = true;
            return;
          }

          if (!term) {
            rows.forEach(function (row) { row.style.display = ''; });
            card.style.display = '';
            anyVisible = true;
            return;
          }

          let cardHasMatch = false;
          rows.forEach(function (row) {
            const rowText = row.innerText.toLowerCase();
            const match = rowText.indexOf(term) !== -1;
            row.style.display = match ? '' : 'none';
            if (match) cardHasMatch = true;
          });

          card.style.display = cardHasMatch ? '' : 'none';
          if (cardHasMatch) anyVisible = true;
        });

        if (noResults) {
          noResults.style.display = anyVisible ? 'none' : '';
        }
      }

      if (filterInput && relaySelect) {
        filterInput.addEventListener('input', applyFilter);
        relaySelect.addEventListener('change', applyFilter);
        applyFilter();
      }
    })();
  </script>
</body>
</html>`;

  res.type('html').send(html);
});



const PORT = process.env.PORT || 3456;
const HOST = process.env.HOST || '127.0.0.1'; // keep local; reverse proxy terminates TLS
app.listen(PORT, HOST, () => {
  console.log(`relay listening on http://${HOST}:${PORT}`);
});
