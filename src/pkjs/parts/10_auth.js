//
// ChatGPT / Codex authentication.
//
// The user pastes the contents of ~/.codex/auth.json, which is the file the
// Codex CLI writes after `codex login`:
//
//   { "OPENAI_API_KEY": null,
//     "tokens": { "id_token": "...", "access_token": "...",
//                 "refresh_token": "...", "account_id": "..." },
//     "last_refresh": "..." }
//
// Access tokens are short lived, so they get refreshed here against the same
// OAuth client the CLI uses.  Refreshed tokens are cached separately from the
// pasted blob, and the cache is dropped whenever a different blob is pasted.
//

var AUTH_CACHE_KEY = 'auth_cache_v1';
var OAUTH_CLIENT_ID = 'app_EMoamEEZ73f0CkXaXp7hrann';
var OAUTH_TOKEN_URL = 'https://auth.openai.com/oauth/token';
var TOKEN_REFRESH_MARGIN_S = 300;

function fingerprint(str) {
  // Cheap, non-cryptographic: only used to notice that the pasted blob changed.
  var hash = 5381;
  for (var i = 0; i < str.length; i++) {
    hash = ((hash << 5) + hash + str.charCodeAt(i)) | 0;
  }
  return String(hash);
}

function parsePastedAuth() {
  var raw = settings().auth_json || '';
  if (!raw) return null;
  var parsed = safeParse(raw, null);
  if (!parsed) return null;

  // Accept either the whole auth.json or just its `tokens` object.
  var tokens = parsed.tokens && typeof parsed.tokens === 'object' ? parsed.tokens : parsed;
  if (!tokens || !tokens.access_token) return null;

  return {
    access_token: tokens.access_token,
    refresh_token: tokens.refresh_token || '',
    account_id: tokens.account_id || '',
    id_token: tokens.id_token || '',
    fingerprint: fingerprint(raw)
  };
}

function accountIdFrom(tokens) {
  if (tokens.account_id) return tokens.account_id;
  var sources = [tokens.id_token, tokens.access_token];
  for (var i = 0; i < sources.length; i++) {
    var claims = decodeJwtPayload(sources[i]);
    if (!claims) continue;
    var auth = claims['https://api.openai.com/auth'];
    if (auth && auth.chatgpt_account_id) return auth.chatgpt_account_id;
    if (claims.chatgpt_account_id) return claims.chatgpt_account_id;
  }
  return '';
}

function tokenExpiry(accessToken) {
  var claims = decodeJwtPayload(accessToken);
  if (claims && isFiniteNumber(claims.exp)) return claims.exp;
  return 0;
}

// The tokens currently in force: the refreshed pair when we have one that still
// belongs to the pasted blob, otherwise the pasted pair itself.
function authTokens() {
  var pasted = parsePastedAuth();
  if (!pasted) return null;

  var cached = storeGet(AUTH_CACHE_KEY, null);
  if (cached && cached.fingerprint === pasted.fingerprint && cached.access_token) {
    return {
      access_token: cached.access_token,
      refresh_token: cached.refresh_token || pasted.refresh_token,
      account_id: cached.account_id || accountIdFrom(pasted),
      id_token: cached.id_token || pasted.id_token,
      fingerprint: pasted.fingerprint
    };
  }
  if (cached && cached.fingerprint !== pasted.fingerprint) storeDel(AUTH_CACHE_KEY);

  pasted.account_id = accountIdFrom(pasted);
  return pasted;
}

function authStatus() {
  var tokens = authTokens();
  if (!tokens) return { ok: false, reason: 'No auth.json pasted yet' };
  var claims = decodeJwtPayload(tokens.id_token) || decodeJwtPayload(tokens.access_token) || {};
  var profile = claims['https://api.openai.com/profile'] || {};
  var auth = claims['https://api.openai.com/auth'] || {};
  var exp = tokenExpiry(tokens.access_token);
  return {
    ok: true,
    email: profile.email || claims.email || '',
    plan: auth.chatgpt_plan_type || '',
    account_id: tokens.account_id || '',
    expires_at: exp ? new Date(exp * 1000).toISOString() : '',
    expired: exp ? (exp <= Math.floor(Date.now() / 1000)) : false,
    can_refresh: !!tokens.refresh_token
  };
}

var _refreshInFlight = null;

function refreshAccessToken(tokens, onDone) {
  if (!tokens.refresh_token) {
    onDone(new Error('Sign-in expired. Run `codex login` and paste the new auth.json.'), null);
    return;
  }
  if (_refreshInFlight) {
    _refreshInFlight.push(onDone);
    return;
  }
  _refreshInFlight = [onDone];

  function settle(err, result) {
    var waiting = _refreshInFlight || [];
    _refreshInFlight = null;
    for (var i = 0; i < waiting.length; i++) {
      try { waiting[i](err, result); } catch (e) { logErr('refresh callback', e); }
    }
  }

  httpRequest({
    method: 'POST',
    url: OAUTH_TOKEN_URL,
    timeout: 30000,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      client_id: OAUTH_CLIENT_ID,
      grant_type: 'refresh_token',
      refresh_token: tokens.refresh_token
    })
  }, function (err, res) {
    if (err || !res) { settle(err || new Error('Token refresh failed'), null); return; }
    if (res.status < 200 || res.status >= 300) {
      var detail = safeParse(res.body, null);
      var code = detail && (detail.error || detail.code || '');
      var message = (code === 'invalid_grant' || res.status === 400)
        ? 'Sign-in expired. Run `codex login` and paste the new auth.json.'
        : 'Token refresh failed (' + res.status + ')';
      settle(new Error(message), null);
      return;
    }
    var body = safeParse(res.body, null);
    if (!body || !body.access_token) { settle(new Error('Token refresh returned no token'), null); return; }

    var updated = {
      fingerprint: tokens.fingerprint,
      access_token: body.access_token,
      refresh_token: body.refresh_token || tokens.refresh_token,
      id_token: body.id_token || tokens.id_token,
      account_id: tokens.account_id || ''
    };
    if (!updated.account_id) updated.account_id = accountIdFrom(updated);
    storeSet(AUTH_CACHE_KEY, updated);
    settle(null, updated);
  });
}

// Hands back a usable access token, refreshing first when the current one is
// within a few minutes of expiry.
function ensureAccessToken(onDone) {
  var tokens = authTokens();
  if (!tokens) {
    onDone(new Error('Paste your Codex auth.json in Settings first.'), null);
    return;
  }
  var exp = tokenExpiry(tokens.access_token);
  var now = Math.floor(Date.now() / 1000);
  if (exp && exp - now > TOKEN_REFRESH_MARGIN_S) {
    onDone(null, tokens);
    return;
  }
  refreshAccessToken(tokens, function (err, refreshed) {
    if (err) {
      // An unexpired-but-unrefreshable token is still worth a try.
      if (exp && exp > now) { onDone(null, tokens); return; }
      onDone(err, null);
      return;
    }
    onDone(null, refreshed);
  });
}
