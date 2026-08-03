#!/usr/bin/env node
/**
 * One-time provisioning for the Claude Buddy display.
 *
 * Performs Claude's standard OAuth authorization-code + PKCE login (the same
 * flow `claude login` uses) so the buddy gets its OWN token pair — independent
 * from Claude Code's session — then delivers it to the display over the LAN.
 *
 *   node provision.js
 *
 * No npm dependencies. Node 18+.
 */
const crypto = require('crypto');
const readline = require('readline');
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const CLIENT_ID = '9d1c250a-e61b-44d9-88ed-5944d1962f5e'; // Claude Code public OAuth client
const REDIRECT_URI = 'https://console.anthropic.com/oauth/code/callback';
const SCOPES = 'org:create_api_key user:profile user:inference';

const b64url = (buf) => buf.toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');

async function main() {
  const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
  const ask = (q) => new Promise((res) => rl.question(q, res));

  const verifier = b64url(crypto.randomBytes(32));
  const challenge = b64url(crypto.createHash('sha256').update(verifier).digest());

  const u = new URL('https://claude.ai/oauth/authorize');
  u.searchParams.set('code', 'true');
  u.searchParams.set('client_id', CLIENT_ID);
  u.searchParams.set('response_type', 'code');
  u.searchParams.set('redirect_uri', REDIRECT_URI);
  u.searchParams.set('scope', SCOPES);
  u.searchParams.set('code_challenge', challenge);
  u.searchParams.set('code_challenge_method', 'S256');
  u.searchParams.set('state', verifier);
  const url = u.toString();

  console.log('\n1) Abriendo el navegador para autorizar al buddy en tu cuenta de Claude.');
  console.log('   Si ves un error o no se abre, copia y pega esta URL COMPLETA a mano:\n');
  console.log(url + '\n');
  try {
    if (process.platform === 'win32') execFileSync('explorer.exe', [url]);
    else if (process.platform === 'darwin') execFileSync('open', [url]);
    else execFileSync('xdg-open', [url]);
  } catch { /* explorer returns nonzero even on success; manual paste also works */ }

  const pasted = (await ask('2) Pegá acá el código que te muestra la página: ')).trim();
  const [code, returnedState] = pasted.split('#');
  if (!code) throw new Error('código vacío');

  console.log('   Canjeando el código por tokens...');
  const resp = await fetch('https://console.anthropic.com/v1/oauth/token', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      grant_type: 'authorization_code',
      code,
      state: returnedState || verifier,
      client_id: CLIENT_ID,
      redirect_uri: REDIRECT_URI,
      code_verifier: verifier,
    }),
  });
  if (!resp.ok) throw new Error(`token exchange HTTP ${resp.status}: ${(await resp.text()).slice(0, 300)}`);
  const tok = await resp.json();
  if (!tok.access_token || !tok.refresh_token) throw new Error('respuesta sin tokens');
  console.log('   Tokens obtenidos (sesión propia del buddy).');

  /* plan name is only cosmetic on the display; grab it locally if available */
  let plan = '';
  try {
    const creds = JSON.parse(fs.readFileSync(path.join(os.homedir(), '.claude', '.credentials.json'), 'utf8'));
    plan = creds.claudeAiOauth?.subscriptionType || '';
  } catch { /* fine */ }

  const ip = (await ask('3) IP del buddy (la muestra en su pantalla): ')).trim();
  const payload = {
    accessToken: tok.access_token,
    refreshToken: tok.refresh_token,
    expiresAt: Date.now() + (tok.expires_in ? tok.expires_in * 1000 : 3600_000),
    plan,
  };
  const dev = await fetch(`http://${ip}/token`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  if (!dev.ok) throw new Error(`el buddy respondió HTTP ${dev.status}`);
  console.log('\nListo! El buddy guardó sus tokens y ya puede consultar tu cuenta solo.');
  rl.close();
}

main().catch((e) => { console.error('\nError:', e.message); process.exit(1); });
