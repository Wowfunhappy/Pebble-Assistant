//
// Settings live in one object in localStorage.  The config page receives the
// whole thing in the URL hash and hands back a full replacement, so adding a
// field here and a control there is all a new setting needs.
//

var SETTINGS_KEY = 'settings_v1';
var CONFIG_URL = 'https://wowfunhappy.github.io/Pebble-Assistant/';

var EFFORTS = [
  { id: 'minimal', label: 'Minimal', hint: 'Fastest' },
  { id: 'low',     label: 'Low',     hint: 'Quick' },
  { id: 'medium',  label: 'Medium',  hint: 'Balanced' },
  { id: 'high',    label: 'High',    hint: 'Slowest, most careful' }
];

// A starting catalog only.  The config page refreshes this list from
// docs/models.json on every open, and the user can add any model id by hand, so
// a new model never needs an app update.
var DEFAULT_MODELS = [
  { id: 'gpt-5.1-codex',      label: 'GPT-5.1 Codex',      show: true },
  { id: 'gpt-5.1-codex-mini', label: 'GPT-5.1 Codex mini', show: true },
  { id: 'gpt-5.1-codex-max',  label: 'GPT-5.1 Codex max',  show: false },
  { id: 'gpt-5-codex',        label: 'GPT-5 Codex',        show: false }
];

var DEFAULT_QUICK_PROMPTS = [
  "What's next on my calendar?",
  'What are my reminders for today?',
  'Summarize my day'
];

var DEFAULT_SYSTEM_PROMPT = 'You are a helpful assistant.';

function defaultSettings() {
  return {
    version: 1,
    auth_json: '',
    system_prompt: DEFAULT_SYSTEM_PROMPT,
    models: DEFAULT_MODELS.slice(),
    default_model: 'gpt-5.1-codex',
    active_model: '',
    effort: 'medium',
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
  if (!base.models || !base.models.length) base.models = DEFAULT_MODELS.slice();
  if (!base.system_prompt) base.system_prompt = DEFAULT_SYSTEM_PROMPT;
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

function visibleModels() {
  var list = settings().models || [];
  var out = [];
  for (var i = 0; i < list.length; i++) {
    if (list[i] && list[i].id && list[i].show !== false) out.push(list[i]);
  }
  if (!out.length && list.length) out.push(list[0]);
  return out;
}

function modelLabel(id) {
  var list = settings().models || [];
  for (var i = 0; i < list.length; i++) {
    if (list[i] && list[i].id === id) return list[i].label || list[i].id;
  }
  return id;
}

function activeModel() {
  var s = settings();
  var candidates = visibleModels();
  var wanted = s.active_model || s.default_model;
  for (var i = 0; i < candidates.length; i++) {
    if (candidates[i].id === wanted) return wanted;
  }
  return candidates.length ? candidates[0].id : (s.default_model || 'gpt-5.1-codex');
}

function setActiveModel(id) {
  updateSettings({ active_model: id });
}

function effortIndex() {
  var current = settings().effort || 'medium';
  for (var i = 0; i < EFFORTS.length; i++) {
    if (EFFORTS[i].id === current) return i;
  }
  return 2;
}

function effortLabel() {
  return EFFORTS[effortIndex()].label;
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
