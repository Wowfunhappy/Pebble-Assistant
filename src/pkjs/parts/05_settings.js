//
// Settings live in one object in localStorage.  The config page receives the
// whole thing in the URL hash and hands back a full replacement, so adding a
// field here and a control there is all a new setting needs.
//
// Models are deliberately NOT listed here.  The set of models an account can
// use, their display names, and which reasoning levels each one accepts all
// come from the Codex backend's /models endpoint (see 15_models.js).  Anything
// hardcoded would be wrong within weeks.
//

var SETTINGS_KEY = 'settings_v1';
var CONFIG_URL = 'https://wowfunhappy.github.io/Pebble-Assistant/';

// Reasoning levels are per-model; these are only the display names for the
// level ids the backend uses.
var EFFORT_LABELS = {
  none: 'None',
  minimal: 'Minimal',
  low: 'Low',
  medium: 'Medium',
  high: 'High',
  xhigh: 'Extra high',
  max: 'Max',
  ultra: 'Ultra'
};

// Used only until the first catalog fetch succeeds, so the menus are never
// empty on a cold start.
var FALLBACK_EFFORTS = [
  { effort: 'low', description: 'Fast responses with lighter reasoning' },
  { effort: 'medium', description: 'Balances speed and reasoning depth' },
  { effort: 'high', description: 'Greater reasoning depth' }
];

var DEFAULT_QUICK_PROMPTS = [
  "What's next on my calendar?",
  'What are my reminders for today?',
  'Summarize my day'
];

var DEFAULT_SYSTEM_PROMPT = 'You are a helpful assistant.';

function defaultSettings() {
  return {
    version: 2,
    auth_json: '',
    system_prompt: DEFAULT_SYSTEM_PROMPT,

    // Model selection is by slug against the fetched catalog.  Empty strings
    // mean "whatever the catalog says is best", which keeps working when the
    // account gains or loses models.
    default_model: '',
    active_model: '',
    hidden_models: [],
    effort: '',

    web_search: true,
    location_enabled: true,
    font_scale: 0,
    auto_dictation: true,
    confirm_dictation: false,
    reply_char_limit: 1500,

    // Calendar (iCloud CalDAV) -- read and write.
    cal_url: '',
    cal_user: '',
    cal_pass: '',
    cal_collection: '',
    ics_feeds: [],

    // Reminders (a separate CalDAV server, e.g. Baikal).
    rem_url: '',
    rem_user: '',
    rem_pass: '',
    rem_collection: '',

    // Named places for location-triggered reminders.
    places: [],

    // Apple Notes over a Gmail account, reached through the Gmail API.
    gmail_client_id: '',
    gmail_client_secret: '',
    gmail_refresh_token: '',
    notes_label: 'Notes',

    quick_prompts: DEFAULT_QUICK_PROMPTS.slice(),
    timeline_pins: true
  };
}

var _settings = null;

function settings() {
  if (_settings) return _settings;
  var stored = storeGet(SETTINGS_KEY, null);
  var base = defaultSettings();
  if (stored && typeof stored === 'object') {
    for (var key in base) {
      if (!Object.prototype.hasOwnProperty.call(base, key)) continue;
      if (typeof stored[key] !== 'undefined' && stored[key] !== null) base[key] = stored[key];
    }
  }
  if (!base.system_prompt) base.system_prompt = DEFAULT_SYSTEM_PROMPT;
  if (!base.hidden_models) base.hidden_models = [];
  _settings = base;
  return _settings;
}

function saveSettings(next) {
  _settings = next;
  storeSet(SETTINGS_KEY, next);
}

function updateSettings(patch) {
  var s = settings();
  for (var key in patch) {
    if (Object.prototype.hasOwnProperty.call(patch, key)) s[key] = patch[key];
  }
  saveSettings(s);
  return s;
}

// --- models -----------------------------------------------------------------

function isHidden(slug) {
  var hidden = settings().hidden_models || [];
  for (var i = 0; i < hidden.length; i++) if (hidden[i] === slug) return true;
  return false;
}

// What the watch offers: what the backend lists, minus anything switched off on
// the phone, most capable first.
function visibleModels() {
  var models = catalogPickerModels();
  var out = [];
  for (var i = 0; i < models.length; i++) {
    if (!isHidden(models[i].slug)) out.push(models[i]);
  }
  return out.length ? out : models;
}

function modelLabel(slug) {
  var model = catalogModel(slug);
  if (model) return model.display_name || model.slug;
  return slug || 'No model';
}

function defaultModelSlug() {
  var wanted = settings().default_model;
  var choices = visibleModels();
  var i;
  for (i = 0; i < choices.length; i++) if (choices[i].slug === wanted) return wanted;
  return choices.length ? choices[0].slug : '';
}

function activeModel() {
  var wanted = settings().active_model;
  var choices = visibleModels();
  for (var i = 0; i < choices.length; i++) if (choices[i].slug === wanted) return wanted;
  return defaultModelSlug();
}

function setActiveModel(slug) {
  updateSettings({ active_model: slug });
  // A model may not accept the reasoning level the last one did.
  var levels = effortsFor(slug);
  var current = settings().effort;
  var supported = false;
  for (var i = 0; i < levels.length; i++) if (levels[i].effort === current) supported = true;
  if (!supported) updateSettings({ effort: '' });
}

// --- reasoning levels -------------------------------------------------------

function effortsFor(slug) {
  var model = catalogModel(slug);
  var levels = model && model.supported_reasoning_levels;
  if (levels && levels.length) return levels;
  return FALLBACK_EFFORTS;
}

function effortLabelFor(id) {
  return EFFORT_LABELS[id] || titleCaseFirst(String(id || ''));
}

// The chosen level, or the model's own default when nothing is chosen or the
// chosen one is not offered by this model.
function currentEffort() {
  var slug = activeModel();
  var levels = effortsFor(slug);
  var wanted = settings().effort;
  var i;
  for (i = 0; i < levels.length; i++) if (levels[i].effort === wanted) return wanted;

  var model = catalogModel(slug);
  var fallback = model && model.default_reasoning_level;
  if (fallback) {
    for (i = 0; i < levels.length; i++) if (levels[i].effort === fallback) return fallback;
  }
  for (i = 0; i < levels.length; i++) if (levels[i].effort === 'medium') return 'medium';
  return levels.length ? levels[0].effort : 'medium';
}

function effortIndex() {
  var levels = effortsFor(activeModel());
  var current = currentEffort();
  for (var i = 0; i < levels.length; i++) if (levels[i].effort === current) return i;
  return 0;
}

function effortLabel() {
  return effortLabelFor(currentEffort());
}

// --- capability checks used by the settings menu and tool gating -------------

function hasCredentials() {
  var parsed = authTokens();
  return !!(parsed && parsed.access_token);
}

function calendarConfigured() {
  var s = settings();
  return !!(s.cal_url && s.cal_user && s.cal_pass);
}

function remindersConfigured() {
  var s = settings();
  return !!(s.rem_url && s.rem_user && s.rem_pass);
}

function notesConfigured() {
  var s = settings();
  return !!(s.gmail_client_id && s.gmail_refresh_token);
}

function findPlace(name) {
  if (!name) return null;
  var places = settings().places || [];
  var needle = String(name).toLowerCase();
  var i;
  for (i = 0; i < places.length; i++) {
    if (places[i] && String(places[i].name || '').toLowerCase() === needle) return places[i];
  }
  for (i = 0; i < places.length; i++) {
    var label = String(places[i].name || '').toLowerCase();
    if (label && (label.indexOf(needle) >= 0 || needle.indexOf(label) >= 0)) return places[i];
  }
  return null;
}
