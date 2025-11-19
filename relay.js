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
app.get('/', (req, res) => {
  prune();

  if (!rooms.has('bags')) rooms.set('bags', new Map());

  const data = [];
  for (const [roomName, m] of rooms.entries()) {
    const peers = [];
    for (const [clientId, v] of m.entries()) {
      peers.push({
        clientId,
        name: v.name,
        prof: v.prof,
        pluginVer: v.pluginVer,
        subgroup: v.subgroup,
        entriesCount: (v.entries || []).length,
        lastSeenMsAgo: Date.now() - v.ts
      });
    }
    data.push({ room: roomName, peers });
  }

  res.type('html').send(`
    <html>
      <head>
        <title>Relay Status &#x1F7E9; </title>
        <style>
          body { font-family: sans-serif; padding: 1rem; background: #111; color: #eee; }
          h1, h2 { margin: 0.5rem 0; }
          table { border-collapse: collapse; margin-bottom: 1rem; }
          th, td { border: 1px solid #444; padding: 4px 8px; font-size: 13px; }
          th { background: #222; }
          a { color: #7cf; }
        </style>
      </head>
      <body>
        <h1>Relay Status &#x1F7E9; </h1>

        <p>
          <a href="/download/arcdps_cooldowns.dll" download>
            arcdps_cooldowns.dll
          </a>
place in GW2 main folder next to arcdps
<b>autoupdates</b>
        </p>

        ${data.map(r => `
          <h2></h2>
          <table>
            <tr>
              <th>#</th>
              <th>ClientId</th>
              <th>Name</th>
              <th>Prof</th>
              <th>Subgroup</th>
              <th>PluginVer</th>
              <th>Entries</th>
              <th>Last seen (ms ago)</th>
            </tr>
            ${r.peers.map((p, i) => `
              <tr>
                <td>${i + 1}</td>
                <td>${p.clientId}</td>
                <td>${p.name}</td>
                <td>${p.prof}</td>
                <td>${p.subgroup || ''}</td>
                <td>${p.pluginVer || ''}</td>
                <td>${p.entriesCount}</td>
                <td>${p.lastSeenMsAgo}</td>
              </tr>
            `).join('')}
          </table>
        `).join('')}
      </body>
    </html>
  `);
});

const PORT = process.env.PORT || 3456;
const HOST = process.env.HOST || '127.0.0.1'; // keep local; reverse proxy terminates TLS
app.listen(PORT, HOST, () => {
  console.log(`relay listening on http://${HOST}:${PORT}`);
});
