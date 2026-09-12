//
// The model catalog.
//
// Which models an account can use is answered by the Codex backend itself:
//
//   GET https://chatgpt.com/backend-api/codex/models?client_version=X.Y.Z
//
// authenticated exactly like a chat request.  It returns, per model, the slug,
// a display name and description, a `visibility` of list/hide/none, a
// `priority` for ordering, and -- importantly -- `supported_reasoning_levels`,
// which differ from model to model.  Hardcoding any of that goes stale fast, so
// nothing here ships a model list; it only caches what the backend said.
//
// The catalog is refreshed on launch and after every settings save, and cached
// so a flaky connection never empties the watch menus.
//

var MODEL_CATALOG_KEY = 'model_catalog_v1';
var CODEX_MODELS_URL = 'https://chatgpt.com/backend-api/codex/models';

// The Codex CLI generation whose wire protocol this app implements.  The
// backend uses it to decide what a client can be told about, and each model
// carries a `minimal_client_version` we honour below.
var CODEX_CLIENT_VERSION = '0.153.0';

var CATALOG_MAX_AGE_MS = 6 * 60 * 60 * 1000;

function compareVersions(a, b) {
  var left = String(a || '0').split('.');
  var right = String(b || '0').split('.');
  for (var i = 0; i < 3; i++) {
    var l = parseInt(left[i], 10) || 0;
    var r = parseInt(right[i], 10) || 0;
    if (l !== r) return l < r ? -1 : 1;
  }
  return 0;
}

function catalogRecord() {
  return storeGet(MODEL_CATALOG_KEY, null);
}

function catalogModels() {
  var record = catalogRecord();
  return (record && record.models) || [];
}

function catalogModel(slug) {
  if (!slug) return null;
  var models = catalogModels();
  for (var i = 0; i < models.length; i++) {
    if (models[i].slug === slug) return models[i];
  }
  return null;
}

function catalogAge() {
  var record = catalogRecord();
  return record && record.fetched_at ? (Date.now() - record.fetched_at) : Infinity;
}

function sortByPriority(models) {
  return models.slice().sort(function (a, b) {
    return (b.priority || 0) - (a.priority || 0);
  });
}

// Models the backend wants in a picker, and that this client is new enough to
// drive.  A model we cannot speak to is worse than a model that is not listed.
function catalogPickerModels() {
  var models = catalogModels();
  var out = [];
  for (var i = 0; i < models.length; i++) {
    var model = models[i];
    if (!model || !model.slug) continue;
    if (model.visibility !== 'list') continue;
    if (model.minimal_client_version &&
        compareVersions(CODEX_CLIENT_VERSION, model.minimal_client_version) < 0) continue;
    out.push(model);
  }
  return sortByPriority(out);
}

// Everything worth showing on the phone, including models the backend keeps out
// of the picker, so they can be switched on deliberately.
function catalogAllModels() {
  var models = catalogModels();
  var out = [];
  for (var i = 0; i < models.length; i++) {
    if (models[i] && models[i].slug && models[i].visibility !== 'none') out.push(models[i]);
  }
  return sortByPriority(out);
}

// Keep only the fields the watch and config page use; the full payload is tens
// of kilobytes per model and would blow the config URL budget.
function trimCatalogModel(model) {
  var levels = [];
  var supported = model.supported_reasoning_levels || [];
  for (var i = 0; i < supported.length; i++) {
    if (!supported[i] || !supported[i].effort) continue;
    levels.push({ effort: supported[i].effort, description: supported[i].description || '' });
  }
  return {
    slug: model.slug,
    display_name: model.display_name || model.slug,
    description: model.description || '',
    visibility: model.visibility || 'list',
    priority: model.priority || 0,
    default_reasoning_level: model.default_reasoning_level || '',
    supported_reasoning_levels: levels,
    minimal_client_version: model.minimal_client_version || '',
    context_window: model.context_window || 0,
    supports_search_tool: model.supports_search_tool !== false
  };
}

var _catalogInFlight = false;

function fetchModelCatalog(force, onDone) {
  if (!onDone) onDone = function () {};
  if (_catalogInFlight) { onDone(null, catalogModels()); return; }
  if (!force && catalogAge() < CATALOG_MAX_AGE_MS) { onDone(null, catalogModels()); return; }

  ensureAccessToken(function (authErr, tokens) {
    if (authErr) { onDone(authErr, catalogModels()); return; }
    _catalogInFlight = true;

    var headers = {
      'Accept': 'application/json',
      'Authorization': 'Bearer ' + tokens.access_token,
      'OpenAI-Beta': 'responses=experimental',
      'originator': CODEX_ORIGINATOR,
      'session_id': uuid4()
    };
    if (tokens.account_id) headers['chatgpt-account-id'] = tokens.account_id;

    httpRequest({
      method: 'GET',
      url: CODEX_MODELS_URL + '?client_version=' + encodeURIComponent(CODEX_CLIENT_VERSION),
      headers: headers,
      timeout: 40000
    }, function (err, res) {
      _catalogInFlight = false;
      if (err || !res) { onDone(err || new Error('Could not reach the model list'), catalogModels()); return; }
      if (res.status < 200 || res.status >= 300) {
        var detail = safeParse(res.body, null);
        var message = (detail && detail.error && detail.error.message) || ('HTTP ' + res.status);
        onDone(new Error('Model list failed: ' + message), catalogModels());
        return;
      }
      var body = safeParse(res.body, null);
      var models = body && body.models;
      if (!models || !models.length) {
        onDone(new Error('Model list came back empty'), catalogModels());
        return;
      }
      var trimmed = [];
      for (var i = 0; i < models.length; i++) {
        if (models[i] && models[i].slug) trimmed.push(trimCatalogModel(models[i]));
      }
      storeSet(MODEL_CATALOG_KEY, {
        fetched_at: Date.now(),
        client_version: CODEX_CLIENT_VERSION,
        models: trimmed
      });
      log('model catalog: ' + trimmed.length + ' models, ' +
          catalogPickerModels().length + ' offered on the watch');
      onDone(null, trimmed);
    });
  });
}
