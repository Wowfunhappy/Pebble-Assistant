//
// Wiring.  This is the only part that registers Pebble event listeners.
//

var DIAGNOSTICS_KEY = 'diagnostics_v1';
var _currentRun = null;

// --- instructions -----------------------------------------------------------

function buildInstructions() {
  var s = settings();
  var now = new Date();
  var lines = [];

  lines.push(s.system_prompt || DEFAULT_SYSTEM_PROMPT);
  lines.push('');
  lines.push('## Context');
  lines.push('You are running on a Pebble Time 2 smartwatch. The user speaks to you and ' +
             'reads your answer on a 200x228 pixel screen, often while walking.');
  lines.push('Current local time: ' + WEEKDAYS[now.getDay()] + ' ' + MONTHS[now.getMonth()] + ' ' +
             now.getDate() + ' ' + now.getFullYear() + ', ' + friendlyTime(now) +
             ' (UTC' + tzOffsetString(now) + ').');

  var places = s.places || [];
  if (places.length) {
    var names = [];
    for (var i = 0; i < places.length; i++) if (places[i] && places[i].name) names.push(places[i].name);
    if (names.length) lines.push('Places the user has named: ' + names.join(', ') + '.');
  }

  lines.push('');
  lines.push('## Answer style');
  lines.push('- Lead with the answer. No preamble, no restating the question.');
  lines.push('- Aim for under 60 words unless the user asks for detail.');
  lines.push('- Plain text only: no markdown, no headings, no tables, no emoji, no URLs. ' +
             'Cite a source by name if it matters, never as a link.');
  lines.push('- Speak in short sentences. Numbers and times in a form that reads aloud well.');
  lines.push('- When you use a tool that changes something, say plainly what you did.');
  lines.push('- If a tool fails, say so in one sentence rather than guessing at the answer.');

  return lines.join('\n');
}

// --- asking -----------------------------------------------------------------

function askModel(question, retried) {
  var text = String(question || '').replace(/^\s+|\s+$/g, '');
  if (!text) return;

  if (!hasCredentials()) {
    sendError('Paste your Codex auth.json in Assistant settings on your phone.');
    return;
  }

  // The model to use is whatever the backend currently offers.  On a first run,
  // or after the cache is cleared, fetch that before asking anything.
  if (!activeModel() && !retried) {
    sendStatus('Finding models', SPIN_TOOL);
    fetchModelCatalog(true, function (catalogErr) {
      if (!activeModel()) {
        sendError(catalogErr ? catalogErr.message : 'No models available on this account.');
        return;
      }
      sendSettings(PEVT_SETTINGS);
      askModel(text, true);
    });
    return;
  }

  var input = conversationInput();
  input.push(userMessage(text));
  sendStatus('Thinking', SPIN_THINKING);

  if (_currentRun) { _currentRun.cancel(); _currentRun = null; }
  _currentRun = codexRunTurn({
    model: activeModel(),
    instructions: buildInstructions(),
    tools: buildToolDefinitions(),
    effort: currentEffort(),
    input: input,
    sessionId: conversationSessionId(),
    onStatus: function (label, spin) { sendStatus(label, spin); },
    onDone: function (err, result) {
      _currentRun = null;
      if (err) {
        logErr('askModel', err);
        sendError(err.message || 'Something went wrong');
        return;
      }
      var answer = (result && result.text) || '';
      if (!answer) answer = 'No answer came back.';
      rememberLiveInput(result.input);
      var chat = appendTurn(text, answer);
      sendTurnToWatch(turnAt(chat, chat.turns.length - 1));
    }
  });
}

// --- watch requests ---------------------------------------------------------

function handleToggle(id) {
  var s = settings();
  switch (id) {
    case 1: updateSettings({ web_search: !s.web_search }); break;
    case 2: updateSettings({ location_enabled: !s.location_enabled }); break;
    case 3: updateSettings({ confirm_dictation: !s.confirm_dictation }); break;
    case 4: updateSettings({ auto_dictation: !s.auto_dictation }); break;
    default: break;
  }
  sendSettings(PEVT_SETTINGS);
}

function handleListAction(action, arg) {
  switch (action) {
    case ACT_OPEN_CHAT: {
      var chat = openChatByIndex(arg);
      if (!chat) { sendError('That conversation is gone'); return; }
      sendSettings(PEVT_SETTINGS);
      var turn = turnAt(chat, chat.turns.length - 1);
      if (turn) sendTurnToWatch(turn);
      else sendError('That conversation is empty');
      break;
    }
    case ACT_SET_MODEL: {
      var models = visibleModels();
      if (arg >= 0 && arg < models.length) {
        setActiveModel(models[arg].slug);
        sendSettings(PEVT_SETTINGS);
        sendToast(modelLabel(models[arg].slug));
      }
      break;
    }
    case ACT_SET_EFFORT: {
      var levels = effortsFor(activeModel());
      if (arg >= 0 && arg < levels.length) {
        updateSettings({ effort: levels[arg].effort });
        sendSettings(PEVT_SETTINGS);
        sendToast('Thinking: ' + effortLabelFor(levels[arg].effort));
      }
      break;
    }
    case ACT_TOGGLE:
      handleToggle(arg);
      break;
    default:
      break;
  }
}

function handleGetTurn(index) {
  var chat = activeChat();
  var turn = turnAt(chat, index);
  if (turn) sendTurnToWatch(turn);
}

function handleWakeupFired(cookie) {
  var state = timerState();
  var kept = [];
  for (var i = 0; i < state.timers.length; i++) {
    if (state.timers[i].cookie === cookie) removeTimelinePin('pa-timer-' + cookie);
    else kept.push(state.timers[i]);
  }
  state.timers = kept;
  saveTimerState(state);
}

function handleSnooze(cookie, minutes) {
  var state = timerState();
  var label = 'Reminder';
  for (var i = 0; i < state.timers.length; i++) {
    if (state.timers[i].cookie === cookie) { label = state.timers[i].label; break; }
  }
  cancelTimers(null, cookie);
  scheduleTimer(label, new Date(Date.now() + (minutes || 9) * 60000));
}

function handleWatchMessage(payload) {
  var req = payload.WREQ;
  if (typeof req === 'undefined') return;
  var str = typeof payload.WSTR === 'string' ? payload.WSTR : '';
  var a = typeof payload.WINT === 'number' ? payload.WINT : 0;
  var b = typeof payload.WINT2 === 'number' ? payload.WINT2 : 0;

  switch (req) {
    case WREQ_HELLO:
      // The watch app just launched, so begin a new conversation rather than
      // appending to whatever was open last time.
      startFreshConversation();
      sendSettings(PEVT_READY);
      break;
    case WREQ_ASK:
      askModel(str);
      break;
    case WREQ_CANCEL:
      if (_currentRun) { _currentRun.cancel(); _currentRun = null; }
      break;
    case WREQ_NEW_CHAT:
      newChat();
      sendSettings(PEVT_SETTINGS);
      break;
    case WREQ_OPEN_LIST:
      sendListById(a);
      break;
    case WREQ_LIST_ACTION:
      handleListAction(a, b);
      break;
    case WREQ_GET_TURN:
      handleGetTurn(a);
      break;
    case WREQ_WAKEUP_FIRED:
      handleWakeupFired(a);
      break;
    case WREQ_SNOOZE:
      handleSnooze(a, b);
      break;
    default:
      break;
  }
}

// --- diagnostics ------------------------------------------------------------
//
// The config page cannot reach CalDAV or Gmail itself (no CORS), so the checks
// run here after a save and the results are handed back the next time the page
// opens.  That is what turns "it silently does nothing" into "here is the step
// that failed".
//

function runDiagnostics(onDone) {
  var report = { at: Date.now(), models: 'not signed in', calendar: 'not configured',
                 reminders: 'not configured', notes: 'not configured' };

  function finishCalendar(next) {
    if (!calendarConfigured()) { next(); return; }
    caldavFetchEvents(new Date(), new Date(Date.now() + 86400000), function (err, events) {
      report.calendar = err ? ('FAILED: ' + err.message)
                            : ('OK - ' + events.length + ' event(s) in the next 24h');
      next();
    });
  }
  function finishReminders(next) {
    if (!remindersConfigured()) { next(); return; }
    caldavFetchTodos(false, function (err, todos) {
      report.reminders = err ? ('FAILED: ' + err.message)
                             : ('OK - ' + todos.length + ' open reminder(s)');
      next();
    });
  }
  function finishNotes(next) {
    if (!notesConfigured()) { next(); return; }
    gmailMeta(function (err, meta) {
      report.notes = err ? ('FAILED: ' + err.message)
                         : ('OK - label "' + meta.label_name + '" on ' + meta.email);
      next();
    });
  }

  function finishModels(next) {
    if (!hasCredentials()) { next(); return; }
    fetchModelCatalog(true, function (err, models) {
      report.models = err ? ('FAILED: ' + err.message)
                          : ('OK - ' + (models || []).length + ' model(s), ' +
                             visibleModels().length + ' offered on the watch');
      next();
    });
  }

  finishModels(function () {
  finishCalendar(function () {
    finishReminders(function () {
      finishNotes(function () {
        storeSet(DIAGNOSTICS_KEY, report);
        if (onDone) onDone(report);
      });
    });
  });
  });
}

// --- configuration page -----------------------------------------------------

function configPayload() {
  var s = settings();
  var payload = {
    version: APP_VERSION,
    settings: s,
    auth: authStatus(),
    diagnostics: storeGet(DIAGNOSTICS_KEY, null),
    // The page cannot call the Codex API itself (no CORS), so it renders the
    // catalog this side fetched.
    catalog: catalogAllModels(),
    catalog_age_ms: catalogAge(),
    active_model: activeModel(),
    chats: []
  };
  // Conversations ride along so the page can show and export them.  The URL is
  // the only channel available, so the transcript is capped well short of what
  // a WebView will refuse to load.
  var store = chatStore();
  var budget = 200000;
  for (var i = 0; i < store.chats.length; i++) {
    var serialized = JSON.stringify(store.chats[i]);
    if (serialized.length > budget) break;
    budget -= serialized.length;
    payload.chats.push(store.chats[i]);
  }
  return payload;
}

function openConfiguration() {
  var url = CONFIG_URL + '?v=' + encodeURIComponent(APP_VERSION) +
            '&t=' + Date.now() +
            '#' + encodeURIComponent(JSON.stringify(configPayload()));
  Pebble.openURL(url);
}

function applyConfigResponse(raw) {
  if (!raw) return;
  var decoded = raw;
  try { decoded = decodeURIComponent(raw); } catch (e) { /* already plain */ }
  var response = safeParse(decoded, null);
  if (!response) { log('config returned nothing usable'); return; }

  if (response.settings && typeof response.settings === 'object') {
    var merged = settings();
    for (var key in response.settings) {
      if (Object.prototype.hasOwnProperty.call(response.settings, key)) {
        merged[key] = response.settings[key];
      }
    }
    saveSettings(merged);
    // A freshly pasted auth.json invalidates any cached access token.
    storeDel(AUTH_CACHE_KEY);
    storeDel(CALDAV_CACHE_KEY);
    storeDel(GOOGLE_TOKEN_CACHE);
    storeDel(GMAIL_META_CACHE);
  }

  if (response.action === 'clear_chats') {
    storeDel(CHATS_KEY);
    newChat();
  }

  sendSettings(PEVT_SETTINGS);
  sendToast('Settings saved');
  fetchModelCatalog(true, function () { sendSettings(PEVT_SETTINGS); });
  runDiagnostics(function (report) {
    var problems = [];
    if (/^FAILED/.test(report.models)) problems.push('models');
    if (/^FAILED/.test(report.calendar)) problems.push('calendar');
    if (/^FAILED/.test(report.reminders)) problems.push('reminders');
    if (/^FAILED/.test(report.notes)) problems.push('notes');
    if (problems.length) sendToast('Check ' + problems.join(', '));
  });
}

// --- events -----------------------------------------------------------------

Pebble.addEventListener('ready', function () {
  log('PebbleKit JS ready, version ' + APP_VERSION);
  // The JS runtime starts with the watch app, so this is a launch too.  The
  // watch also sends WREQ_HELLO, but that can be sent before this listener
  // exists and be lost; startFreshConversation reuses an empty chat, so running
  // it from both paths is harmless.
  startFreshConversation();
  sendSettings(PEVT_READY);
  // Cached, so this is usually a no-op; it keeps the model menu honest when the
  // account gains or loses models between launches.
  if (hasCredentials()) {
    fetchModelCatalog(false, function (err) {
      if (err) { logErr('model catalog', err); return; }
      sendSettings(PEVT_SETTINGS);
    });
  }
});

Pebble.addEventListener('appmessage', function (e) {
  try {
    handleWatchMessage(e.payload || {});
  } catch (err) {
    logErr('appmessage', err);
    sendError('Phone-side error');
  }
});

Pebble.addEventListener('showConfiguration', function () {
  try {
    openConfiguration();
  } catch (err) {
    logErr('showConfiguration', err);
  }
});

Pebble.addEventListener('webviewclosed', function (e) {
  try {
    applyConfigResponse(e && e.response);
  } catch (err) {
    logErr('webviewclosed', err);
  }
});
