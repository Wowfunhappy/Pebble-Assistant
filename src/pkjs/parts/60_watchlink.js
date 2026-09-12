//
// The watch link.
//
// AppMessage carries one dictionary at a time and nacks anything sent while the
// previous transfer is outstanding, so everything goes through a queue.  Text is
// split on UTF-8 boundaries into chunks small enough to survive the iOS
// transport, and reassembled on the watch.
//
// These constants mirror src/c/assistant.h and are append-only.
//

var WREQ_HELLO = 1, WREQ_ASK = 2, WREQ_CANCEL = 3, WREQ_NEW_CHAT = 4,
    WREQ_OPEN_LIST = 5, WREQ_LIST_ACTION = 6, WREQ_GET_TURN = 7,
    WREQ_WAKEUP_FIRED = 8, WREQ_SNOOZE = 9;

var PEVT_READY = 1, PEVT_STATUS = 2, PEVT_TURN_BEGIN = 3, PEVT_Q_CHUNK = 4,
    PEVT_A_CHUNK = 5, PEVT_TURN_END = 6, PEVT_LIST_BEGIN = 7, PEVT_LIST_ITEM = 8,
    PEVT_LIST_END = 9, PEVT_ERROR = 10, PEVT_WAKEUP_SET = 11, PEVT_WAKEUP_CLR = 12,
    PEVT_SETTINGS = 13, PEVT_TOAST = 14;

var LIST_CHATS = 1, LIST_SETTINGS = 2, LIST_MODELS = 3, LIST_EFFORT = 4, LIST_QUICK = 5;

var ACT_NONE = 0, ACT_OPEN_CHAT = 1, ACT_SET_MODEL = 2, ACT_SET_EFFORT = 3,
    ACT_TOGGLE = 4, ACT_SUBMENU = 5, ACT_NEW_CHAT = 6, ACT_QUICK = 7, ACT_CLOSE = 8;

var ROW_FLAG_CURRENT = 1, ROW_FLAG_ON = 2, ROW_FLAG_OFF = 4,
    ROW_FLAG_CHEVRON = 8, ROW_FLAG_ACCENT = 16;

var SFLAG_SEARCH = 1, SFLAG_LOCATION = 2, SFLAG_HEALTH = 4, SFLAG_CONFIGURED = 8,
    SFLAG_CONFIRM_DICT = 16, SFLAG_AUTO_DICT = 32;

var SPIN_NONE = 0, SPIN_THINKING = 1, SPIN_SEARCHING = 2, SPIN_TOOL = 3;

// Matches MAX_ANSWER_LEN / MAX_QUESTION_LEN on the watch, minus room for the
// terminating NUL.
var WATCH_ANSWER_BYTES = 2000;
var WATCH_QUESTION_BYTES = 560;
var CHUNK_BYTES = 240;

var _outQueue = [];
var _outBusy = false;
var _outRetries = 0;

function pumpWatchQueue() {
  if (_outBusy || !_outQueue.length) return;
  _outBusy = true;
  var payload = _outQueue[0];

  Pebble.sendAppMessage(payload, function () {
    _outBusy = false;
    _outRetries = 0;
    _outQueue.shift();
    pumpWatchQueue();
  }, function () {
    _outBusy = false;
    _outRetries++;
    if (_outRetries > 4) {
      log('dropping watch message after repeated nacks');
      _outRetries = 0;
      _outQueue.shift();
    }
    setTimeout(pumpWatchQueue, 120 * _outRetries);
  });
}

function sendToWatch(payload) {
  // A backlog means the watch app is gone; keeping it would only delay the
  // messages that matter when it comes back.
  if (_outQueue.length > 60) _outQueue.splice(0, _outQueue.length - 30);
  _outQueue.push(payload);
  pumpWatchQueue();
}

function clearWatchQueue() {
  _outQueue = [];
}

// --- settings mirror --------------------------------------------------------

function settingsFlags() {
  var s = settings();
  var flags = 0;
  if (s.web_search) flags |= SFLAG_SEARCH;
  if (s.location_enabled) flags |= SFLAG_LOCATION;
  if (hasCredentials()) flags |= SFLAG_CONFIGURED;
  if (s.confirm_dictation) flags |= SFLAG_CONFIRM_DICT;
  if (s.auto_dictation) flags |= SFLAG_AUTO_DICT;
  return flags;
}

function sendSettings(eventType) {
  sendToWatch({
    PEVT: eventType || PEVT_SETTINGS,
    PSTR: trimText(modelLabel(activeModel()), 30),
    PINT: effortIndex(),
    PINT2: settingsFlags(),
    PFLAG: clamp(settings().font_scale || 0, 0, 2)
  });
}

function sendStatus(text, spinner) {
  sendToWatch({ PEVT: PEVT_STATUS, PSTR: trimText(text, 40),
                PINT: typeof spinner === 'number' ? spinner : SPIN_THINKING });
}

function sendError(text) {
  sendToWatch({ PEVT: PEVT_ERROR, PSTR: trimText(text, 120) });
}

function sendToast(text) {
  sendToWatch({ PEVT: PEVT_TOAST, PSTR: trimText(text, 40) });
}

// --- turns ------------------------------------------------------------------

function sendTurnToWatch(turn) {
  if (!turn) return;
  sendToWatch({
    PEVT: PEVT_TURN_BEGIN,
    PINT: turn.index,
    PINT2: turn.count,
    PSTR: trimText(turn.title || 'Assistant', 30)
  });

  var question = truncateBytes(plainify(turn.question || ''), WATCH_QUESTION_BYTES);
  var chunks = splitUtf8(question, CHUNK_BYTES);
  var i;
  for (i = 0; i < chunks.length; i++) {
    sendToWatch({ PEVT: PEVT_Q_CHUNK, PSTR: chunks[i] });
  }

  var answer = plainify(turn.answer || '');
  var limit = clamp(settings().reply_char_limit || 1500, 200, 4000);
  answer = trimText(answer, limit);
  answer = truncateBytes(answer, WATCH_ANSWER_BYTES);
  chunks = splitUtf8(answer, CHUNK_BYTES);
  for (i = 0; i < chunks.length; i++) {
    sendToWatch({ PEVT: PEVT_A_CHUNK, PSTR: chunks[i] });
  }

  sendToWatch({ PEVT: PEVT_TURN_END, PINT: turn.is_live ? 1 : 0 });
}

// --- lists ------------------------------------------------------------------

function makeRow(label, sub, action, arg, flags) {
  return {
    label: trimText(label || '', 36),
    sub: trimText(sub || '', 28),
    action: action || ACT_NONE,
    arg: typeof arg === 'number' ? arg : 0,
    flags: flags || 0
  };
}

function sendList(listId, title, rows, selected) {
  sendToWatch({ PEVT: PEVT_LIST_BEGIN, PINT: listId, PINT2: rows.length,
                PSTR: trimText(title, 28) });
  for (var i = 0; i < rows.length && i < 20; i++) {
    var row = rows[i];
    sendToWatch({
      PEVT: PEVT_LIST_ITEM,
      PINT: i,
      PINT2: row.action,
      PFLAG: row.flags,
      PSTR: row.label + '\x1f' + row.sub + '\x1f' + row.arg
    });
  }
  sendToWatch({ PEVT: PEVT_LIST_END, PINT: listId,
                PINT2: typeof selected === 'number' ? selected : 0 });
}

function relativeAge(timestamp) {
  var delta = Date.now() - timestamp;
  if (delta < 60000) return 'just now';
  if (delta < 3600000) return Math.round(delta / 60000) + 'm ago';
  if (delta < 86400000) return Math.round(delta / 3600000) + 'h ago';
  var days = Math.round(delta / 86400000);
  if (days < 7) return days + 'd ago';
  var date = new Date(timestamp);
  return MONTHS[date.getMonth()] + ' ' + date.getDate();
}

// The root list.  Settings first, New Chat second, history below -- and New Chat
// is preselected, so backing out of the microphone leaves the retry one press
// away.
function sendChatsList() {
  var rows = [
    makeRow('Settings', modelLabel(activeModel()), ACT_SUBMENU, LIST_SETTINGS, ROW_FLAG_CHEVRON),
    makeRow('New Chat', '', ACT_NEW_CHAT, 0, ROW_FLAG_ACCENT)
  ];
  var chats = chatList();
  for (var i = 0; i < chats.length && rows.length < 20; i++) {
    rows.push(makeRow(chats[i].title,
                      relativeAge(chats[i].updated) + '  ·  ' + chats[i].turns + ' turns',
                      ACT_OPEN_CHAT, chats[i].index,
                      chats[i].active ? ROW_FLAG_CURRENT : 0));
  }
  sendList(LIST_CHATS, 'Assistant', rows, 1);
}

function sendSettingsList() {
  var s = settings();
  var rows = [
    makeRow('Model', modelLabel(activeModel()), ACT_SUBMENU, LIST_MODELS, ROW_FLAG_CHEVRON),
    makeRow('Thinking', effortLabel(), ACT_SUBMENU, LIST_EFFORT, ROW_FLAG_CHEVRON),
    makeRow('Web search', '', ACT_TOGGLE, 1, s.web_search ? ROW_FLAG_ON : ROW_FLAG_OFF),
    makeRow('Location', '', ACT_TOGGLE, 2, s.location_enabled ? ROW_FLAG_ON : ROW_FLAG_OFF),
    makeRow('Confirm speech', '', ACT_TOGGLE, 3, s.confirm_dictation ? ROW_FLAG_ON : ROW_FLAG_OFF),
    makeRow('Listen on open', '', ACT_TOGGLE, 4, s.auto_dictation ? ROW_FLAG_ON : ROW_FLAG_OFF)
  ];
  if ((s.quick_prompts || []).length) {
    rows.splice(2, 0, makeRow('Quick prompts', '', ACT_SUBMENU, LIST_QUICK, ROW_FLAG_CHEVRON));
  }
  rows.push(makeRow('Text size', ['Normal', 'Large', 'Extra large'][clamp(s.font_scale || 0, 0, 2)],
                    ACT_TOGGLE, 5, 0));
  sendList(LIST_SETTINGS, 'Settings', rows, 0);
}

function sendModelsList() {
  var models = visibleModels();
  var current = activeModel();
  var rows = [];
  for (var i = 0; i < models.length; i++) {
    rows.push(makeRow(models[i].label || models[i].id, '', ACT_SET_MODEL, i,
                      models[i].id === current ? ROW_FLAG_CURRENT : 0));
  }
  if (!rows.length) rows.push(makeRow('No models enabled', 'Add one in phone settings', ACT_NONE, 0, 0));
  var selected = 0;
  for (var j = 0; j < models.length; j++) if (models[j].id === current) selected = j;
  sendList(LIST_MODELS, 'Model', rows, selected);
}

function sendEffortList() {
  var rows = [];
  var current = effortIndex();
  for (var i = 0; i < EFFORTS.length; i++) {
    rows.push(makeRow(EFFORTS[i].label, EFFORTS[i].hint, ACT_SET_EFFORT, i,
                      i === current ? ROW_FLAG_CURRENT : 0));
  }
  sendList(LIST_EFFORT, 'Thinking', rows, current);
}

function sendQuickList() {
  var prompts = settings().quick_prompts || [];
  var rows = [];
  for (var i = 0; i < prompts.length; i++) {
    rows.push(makeRow(prompts[i], '', ACT_QUICK, i, 0));
  }
  if (!rows.length) rows.push(makeRow('None yet', 'Add them in phone settings', ACT_NONE, 0, 0));
  sendList(LIST_QUICK, 'Quick prompts', rows, 0);
}

function sendListById(listId) {
  switch (listId) {
    case LIST_CHATS:    sendChatsList(); break;
    case LIST_SETTINGS: sendSettingsList(); break;
    case LIST_MODELS:   sendModelsList(); break;
    case LIST_EFFORT:   sendEffortList(); break;
    case LIST_QUICK:    sendQuickList(); break;
    default: break;
  }
}
