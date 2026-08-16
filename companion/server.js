#!/usr/bin/env node
/**
 * Claude Buddy companion server.
 *
 * Reads the local Claude Code OAuth credentials, polls Anthropic's usage
 * endpoint (the same data `/usage` shows) plus local ~/.claude activity,
 * and serves a compact JSON snapshot for the WT32-SC01 buddy display.
 *
 * No npm dependencies — Node 18+ built-ins only.
 *
 *   node server.js            → serves http://0.0.0.0:8787/status
 */
const http = require('http');
const https = require('https');
const fs = require('fs');
const path = require('path');
const os = require('os');

const PORT = Number(process.env.BUDDY_PORT || 8787);
const CLAUDE_DIR = path.join(os.homedir(), '.claude');
const CREDS_FILE = path.join(CLAUDE_DIR, '.credentials.json');
const PROJECTS_DIR = path.join(CLAUDE_DIR, 'projects');
const USAGE_URL = 'https://api.anthropic.com/api/oauth/usage';
const OAUTH_TOKEN_URL = 'https://console.anthropic.com/v1/oauth/token';
const OAUTH_CLIENT_ID = '9d1c250a-e61b-44d9-88ed-5944d1962f5e'; // Claude Code's public OAuth client id
const POLL_MS = 120_000;         // upstream usage poll (gentle: the endpoint 429s if hammered)
const CACHE_FILE = path.join(__dirname, '.cache.json');
const POLL_MAX_MS = 10 * 60_000; // backoff cap after repeated 429s
const STALE_MS = 10 * 60_000;    // report ok:false only when data is older than this
const ACTIVITY_WINDOW_MS = 5 * 60_000; // "working" if a session file changed in the last 5 min

// ---------------------------------------------------------------- state
let snapshot = {
  ok: false,
  error: 'starting',
  updatedAt: null,
  plan: null,
  limits: [],    // [{kind, label, pct, severity, resetsAt, isActive}, ...]
  activity: { active: false, lastActivityAgoSec: null, activeSessions: 0, tokensToday: 0 },
};

// ---------------------------------------------------------------- helpers
function readCreds() {
  const raw = JSON.parse(fs.readFileSync(CREDS_FILE, 'utf8'));
  const oauth = raw.claudeAiOauth;
  if (!oauth || !oauth.accessToken) throw new Error('no claudeAiOauth in credentials file');
  return oauth;
}

function httpsGetJson(url, headers) {
  return new Promise((resolve, reject) => {
    const req = https.get(url, { headers, timeout: 15_000 }, (res) => {
      let body = '';
      res.on('data', (c) => (body += c));
      res.on('end', () => {
        if (res.statusCode >= 200 && res.statusCode < 300) {
          try { resolve(JSON.parse(body)); } catch (e) { reject(new Error('bad JSON from usage endpoint')); }
        } else {
          reject(new Error(`usage endpoint HTTP ${res.statusCode}: ${body.slice(0, 200)}`));
        }
      });
    });
    req.on('timeout', () => req.destroy(new Error('usage endpoint timeout')));
    req.on('error', reject);
  });
}

function httpsPostJson(url, body) {
  return new Promise((resolve, reject) => {
    const data = JSON.stringify(body);
    const u = new URL(url);
    const req = https.request(
      {
        hostname: u.hostname,
        path: u.pathname,
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(data) },
        timeout: 15_000,
      },
      (res) => {
        let b = '';
        res.on('data', (c) => (b += c));
        res.on('end', () => {
          if (res.statusCode >= 200 && res.statusCode < 300) {
            try { resolve(JSON.parse(b)); } catch { reject(new Error('bad JSON from oauth endpoint')); }
          } else {
            reject(new Error(`token refresh HTTP ${res.statusCode}: ${b.slice(0, 200)}`));
          }
        });
      },
    );
    req.on('timeout', () => req.destroy(new Error('token refresh timeout')));
    req.on('error', reject);
    req.write(data);
    req.end();
  });
}

/**
 * Refresh the access token with the stored refresh token (the desktop app
 * keeps its own session and doesn't update this file, so the access token
 * here goes stale every few hours). The rotated tokens are written back to
 * the credentials file atomically so Claude Code's CLI stays logged in too.
 */
async function refreshOAuth(oauth) {
  const resp = await httpsPostJson(OAUTH_TOKEN_URL, {
    grant_type: 'refresh_token',
    refresh_token: oauth.refreshToken,
    client_id: OAUTH_CLIENT_ID,
  });
  const raw = JSON.parse(fs.readFileSync(CREDS_FILE, 'utf8'));
  raw.claudeAiOauth = {
    ...raw.claudeAiOauth,
    accessToken: resp.access_token,
    refreshToken: resp.refresh_token || raw.claudeAiOauth.refreshToken,
    expiresAt: Date.now() + (resp.expires_in ? resp.expires_in * 1000 : 3600_000),
  };
  const tmp = CREDS_FILE + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(raw));
  fs.renameSync(tmp, CREDS_FILE);
  console.log(`[${new Date().toISOString()}] OAuth token refreshed, expires ${new Date(raw.claudeAiOauth.expiresAt).toISOString()}`);
  return raw.claudeAiOauth;
}

async function fetchUsage() {
  let oauth = readCreds();
  if (oauth.expiresAt && oauth.expiresAt < Date.now() + 120_000) {
    oauth = await refreshOAuth(oauth);
  }
  return httpsGetJson(USAGE_URL, {
    Authorization: `Bearer ${oauth.accessToken}`,
    'anthropic-beta': 'oauth-2025-04-20',
    'User-Agent': 'claude-buddy-companion/1.0',
    Accept: 'application/json',
  });
}

/**
 * Map the usage response into an ordered list of limit bars for the display.
 * Primary source is the `limits` array (same data the /usage command renders);
 * falls back to the legacy top-level five_hour/seven_day objects.
 */
function mapLimits(usage) {
  const out = [];
  if (Array.isArray(usage.limits) && usage.limits.length) {
    for (const l of usage.limits) {
      let label = l.kind;
      if (l.kind === 'session') label = '5h';
      else if (l.kind === 'weekly_all') label = 'Semana';
      else if (l.kind === 'weekly_scoped') label = l.scope?.model?.display_name || 'Modelo';
      out.push({
        kind: l.kind,
        label,
        pct: typeof l.percent === 'number' ? Math.round(l.percent) : null,
        severity: l.severity || 'normal',
        resetsAt: l.resets_at || null,
        isActive: !!l.is_active,
      });
    }
  } else {
    for (const [key, name] of [['five_hour', '5h'], ['seven_day', 'Semana']]) {
      const v = usage[key];
      if (v && typeof v === 'object' && typeof v.utilization === 'number') {
        out.push({ kind: key, label: name, pct: Math.round(v.utilization), severity: 'normal', resetsAt: v.resets_at || null, isActive: false });
      }
    }
  }
  return out;
}

/** Most recent mtime among session .jsonl files, plus count changed recently. */
function scanActivity() {
  let newest = 0;
  let activeSessions = 0;
  const cutoff = Date.now() - ACTIVITY_WINDOW_MS;
  let dirs = [];
  try { dirs = fs.readdirSync(PROJECTS_DIR); } catch { return { newest, activeSessions }; }
  for (const d of dirs) {
    let files = [];
    const dir = path.join(PROJECTS_DIR, d);
    try { files = fs.readdirSync(dir); } catch { continue; }
    for (const f of files) {
      if (!f.endsWith('.jsonl')) continue;
      try {
        const m = fs.statSync(path.join(dir, f)).mtimeMs;
        if (m > newest) newest = m;
        if (m > cutoff) activeSessions++;
      } catch { /* file may vanish mid-scan */ }
    }
  }
  return { newest, activeSessions };
}

/* Tokens used today, counted straight from the session transcripts
 * (~/.claude/projects/**\/*.jsonl) — Claude Code's stats-cache.json is only
 * recomputed occasionally, so it can lag by weeks. Incremental: we remember
 * the byte offset already parsed per file and only read what was appended. */
let tokenDay = null;
let tokensTodayCount = 0;
const fileOffsets = new Map(); // path -> bytes already parsed

function refreshTokens() {
  const today = new Date().toLocaleDateString('sv');
  if (tokenDay !== today) { tokenDay = today; tokensTodayCount = 0; fileOffsets.clear(); }
  const dayStart = new Date(`${today}T00:00:00`).getTime();
  let dirs = [];
  try { dirs = fs.readdirSync(PROJECTS_DIR); } catch { return; }
  for (const d of dirs) {
    const dir = path.join(PROJECTS_DIR, d);
    let files = [];
    try { files = fs.readdirSync(dir); } catch { continue; }
    for (const f of files) {
      if (!f.endsWith('.jsonl')) continue;
      const p = path.join(dir, f);
      let st;
      try { st = fs.statSync(p); } catch { continue; }
      if (st.mtimeMs < dayStart) continue; // not touched today
      const prev = fileOffsets.get(p) || 0;
      if (st.size <= prev) continue;
      try {
        const fd = fs.openSync(p, 'r');
        const buf = Buffer.alloc(st.size - prev);
        fs.readSync(fd, buf, 0, buf.length, prev);
        fs.closeSync(fd);
        const text = buf.toString('utf8');
        const lastNl = text.lastIndexOf('\n');
        if (lastNl < 0) continue; // no complete line yet; retry next round
        fileOffsets.set(p, prev + Buffer.byteLength(text.slice(0, lastNl + 1)));
        for (const line of text.slice(0, lastNl).split('\n')) {
          if (!line.includes('"usage"')) continue;
          try {
            const j = JSON.parse(line);
            const u = j.message?.usage;
            if (!u) continue;
            if (j.timestamp && new Date(j.timestamp).toLocaleDateString('sv') !== today) continue;
            tokensTodayCount += (u.input_tokens || 0) + (u.output_tokens || 0);
          } catch { /* partial or non-JSON line */ }
        }
      } catch { /* file vanished mid-read */ }
    }
  }
}

// ---------------------------------------------------------------- pollers
let lastGoodAt = 0;
let lastError = null;
let pollDelay = POLL_MS;

/* Survive restarts: reload the last good snapshot so a relaunch doesn't
 * show "no data" while the (rate-limited) endpoint warms back up. */
try {
  const cached = JSON.parse(fs.readFileSync(CACHE_FILE, 'utf8'));
  if (cached.snapshot && cached.lastGoodAt) {
    snapshot = cached.snapshot;
    lastGoodAt = cached.lastGoodAt;
  }
} catch { /* no cache yet */ }

async function refreshUsage() {
  try {
    const oauth = readCreds();
    const usage = await fetchUsage();
    snapshot.error = null;
    snapshot.plan = oauth.subscriptionType || null;
    snapshot.limits = mapLimits(usage);
    snapshot.updatedAt = new Date().toISOString();
    lastGoodAt = Date.now();
    lastError = null;
    pollDelay = POLL_MS;
    try { fs.writeFileSync(CACHE_FILE, JSON.stringify({ snapshot, lastGoodAt })); } catch { /* best effort */ }
  } catch (e) {
    lastError = String(e.message || e);
    // Transient failures (esp. 429 rate limits) keep serving the last good
    // data; back off so we stop poking a throttled endpoint.
    if (lastError.includes('429')) pollDelay = Math.min(pollDelay * 2, POLL_MAX_MS);
    else pollDelay = POLL_MS;
    console.error(`[${new Date().toISOString()}] usage poll failed (next in ${pollDelay / 1000}s): ${lastError}`);
  } finally {
    setTimeout(refreshUsage, pollDelay);
  }
}

function refreshActivity() {
  const { newest, activeSessions } = scanActivity();
  const ago = newest ? Math.round((Date.now() - newest) / 1000) : null;
  snapshot.activity = {
    active: newest > Date.now() - ACTIVITY_WINDOW_MS,
    lastActivityAgoSec: ago,
    activeSessions,
    tokensToday: tokensTodayCount,
  };
}

// ---------------------------------------------------------------- status
function buildStatus() {
  refreshActivity(); // cheap, keeps "working" fresh
  const now = Date.now();
  const fresh = lastGoodAt > 0 && now - lastGoodAt < STALE_MS;
  return {
    ...snapshot,
    ok: fresh,
    error: fresh ? null : lastError || snapshot.error,
    staleSec: lastGoodAt ? Math.round((now - lastGoodAt) / 1000) : null,
    dateLocal: new Date().toLocaleDateString('sv'), // YYYY-MM-DD, for daily counters on the display
    lastError,
    limits: snapshot.limits.map((l) => ({
      ...l,
      resetsInSec: l.resetsAt ? Math.max(0, Math.round((Date.parse(l.resetsAt) - now) / 1000)) : null,
    })),
  };
}

/**
 * Push the status to the display(s). The buddy may sit on a sibling subnet
 * it can't reach us FROM (cascaded home routers), but we can reach IT — so
 * the companion delivers instead of waiting to be polled.
 * Comma-separated override: BUDDY_PUSH=http://192.168.68.63/status,...
 */
const PUSH_TARGETS = (process.env.BUDDY_PUSH ||
  'http://192.168.68.63/status,http://192.168.100.116/status')
  .split(',').map((s) => s.trim()).filter(Boolean);
const pushOk = new Map();

async function pushToBuddies() {
  const body = JSON.stringify(buildStatus());
  for (const url of PUSH_TARGETS) {
    try {
      const r = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body,
        signal: AbortSignal.timeout(4000),
      });
      if (r.ok && pushOk.get(url) !== true) console.log(`[push] delivering to ${url}`);
      pushOk.set(url, r.ok);
    } catch {
      if (pushOk.get(url) === true) console.log(`[push] lost ${url}`);
      pushOk.set(url, false);
    }
  }
}
setInterval(pushToBuddies, 15_000);

// ---------------------------------------------------------------- server
const server = http.createServer((req, res) => {
  if (req.url === '/status') {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
    res.end(JSON.stringify(buildStatus()));
  } else if (req.url === '/') {
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('claude-buddy companion. GET /status for JSON.\n');
  } else {
    res.writeHead(404);
    res.end();
  }
});

server.listen(PORT, '0.0.0.0', () => {
  const nets = os.networkInterfaces();
  const ips = Object.values(nets).flat().filter((n) => n && n.family === 'IPv4' && !n.internal).map((n) => n.address);
  console.log(`claude-buddy companion listening on port ${PORT}`);
  console.log(`LAN address(es): ${ips.map((ip) => `http://${ip}:${PORT}/status`).join('  ')}`);
});

refreshUsage(); // self-reschedules with backoff
refreshTokens();
refreshActivity();
setInterval(refreshTokens, 60_000);
