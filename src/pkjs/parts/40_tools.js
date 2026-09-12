//
// Tools.
//
// Only tools whose backing service is actually configured are advertised to the
// model.  That matters more than it sounds: a model told it has a calendar will
// cheerfully claim to have checked one, so an unconfigured CalDAV account is
// better represented by the tool simply not existing.
//

var TIMER_STATE_KEY = 'timers_v1';
var LOCATION_CACHE_KEY = 'location_v1';
var LOCATION_MAX_AGE_MS = 10 * 60 * 1000;

function toolStatusLabel(name) {
  switch (name) {
    case 'get_calendar_events':   return 'Checking calendar';
    case 'create_calendar_event': return 'Adding to calendar';
    case 'list_reminders':        return 'Reading reminders';
    case 'add_reminder':          return 'Adding reminder';
    case 'complete_reminder':     return 'Updating reminder';
    case 'set_timer':             return 'Setting timer';
    case 'cancel_timer':          return 'Clearing timer';
    case 'save_note':             return 'Saving note';
    case 'search_notes':          return 'Searching notes';
    case 'get_location':          return 'Finding you';
    case 'calculate':             return 'Calculating';
    default:                      return 'Working';
  }
}

// --- calculator -------------------------------------------------------------
//
// A real parser rather than eval(): the model's input is untrusted text, and a
// wrist calculator does not need a JavaScript interpreter behind it.
//

function calcTokenize(input) {
  var tokens = [];
  var i = 0;
  var text = String(input);
  while (i < text.length) {
    var ch = text.charAt(i);
    if (/\s/.test(ch)) { i++; continue; }
    if (/[0-9.]/.test(ch)) {
      var num = '';
      while (i < text.length) {
        var digit = text.charAt(i);
        if (/[0-9.]/.test(digit)) { num += digit; i++; continue; }
        // A comma is a thousands separator only between digits in groups of
        // three -- anywhere else it separates function arguments.
        if (digit === ',' && /\d$/.test(num) && /^\d{3}(?!\d)/.test(text.substring(i + 1))) {
          i++;
          continue;
        }
        break;
      }
      tokens.push({ type: 'num', value: parseFloat(num) });
      continue;
    }
    if (/[a-zA-Z_]/.test(ch)) {
      var word = '';
      while (i < text.length && /[a-zA-Z_0-9]/.test(text.charAt(i))) word += text.charAt(i++);
      tokens.push({ type: 'name', value: word.toLowerCase() });
      continue;
    }
    if ('+-*/%^(),'.indexOf(ch) >= 0) { tokens.push({ type: ch }); i++; continue; }
    throw new Error('Unexpected character "' + ch + '"');
  }
  return tokens;
}

var CALC_FUNCTIONS = {
  sqrt: Math.sqrt, abs: Math.abs, round: Math.round, floor: Math.floor,
  ceil: Math.ceil, sin: Math.sin, cos: Math.cos, tan: Math.tan,
  ln: Math.log, log: function (x) { return Math.log(x) / Math.LN10; },
  exp: Math.exp,
  pow: function (a, b) { return Math.pow(a, b); },
  min: Math.min, max: Math.max
};
var CALC_CONSTANTS = { pi: Math.PI, e: Math.E };

function calcEvaluate(expression) {
  var tokens = calcTokenize(expression);
  var pos = 0;

  function peek() { return tokens[pos]; }
  function eat(type) {
    var token = tokens[pos];
    if (!token || token.type !== type) throw new Error('Expected ' + type);
    pos++;
    return token;
  }

  function parsePrimary() {
    var token = peek();
    if (!token) throw new Error('Unexpected end of expression');
    if (token.type === '-') { pos++; return -parsePrimary(); }
    if (token.type === '+') { pos++; return parsePrimary(); }
    if (token.type === 'num') { pos++; return token.value; }
    if (token.type === '(') {
      pos++;
      var value = parseSum();
      eat(')');
      return value;
    }
    if (token.type === 'name') {
      pos++;
      var name = token.value;
      if (peek() && peek().type === '(') {
        pos++;
        var args = [];
        if (peek() && peek().type !== ')') {
          args.push(parseSum());
          while (peek() && peek().type === ',') { pos++; args.push(parseSum()); }
        }
        eat(')');
        var fn = CALC_FUNCTIONS[name];
        if (!fn) throw new Error('Unknown function "' + name + '"');
        return fn.apply(null, args);
      }
      if (Object.prototype.hasOwnProperty.call(CALC_CONSTANTS, name)) return CALC_CONSTANTS[name];
      throw new Error('Unknown name "' + name + '"');
    }
    throw new Error('Unexpected token');
  }

  function parsePower() {
    var base = parsePrimary();
    if (peek() && peek().type === '^') { pos++; return Math.pow(base, parsePower()); }
    return base;
  }

  function parseProduct() {
    var value = parsePower();
    while (peek() && (peek().type === '*' || peek().type === '/' || peek().type === '%')) {
      var op = tokens[pos++].type;
      var rhs = parsePower();
      if (op === '*') value *= rhs;
      else if (op === '/') value /= rhs;
      else value %= rhs;
    }
    return value;
  }

  function parseSum() {
    var value = parseProduct();
    while (peek() && (peek().type === '+' || peek().type === '-')) {
      var op = tokens[pos++].type;
      var rhs = parseProduct();
      if (op === '+') value += rhs; else value -= rhs;
    }
    return value;
  }

  var result = parseSum();
  if (pos < tokens.length) throw new Error('Trailing input');
  if (!isFiniteNumber(result)) throw new Error('Result is not a finite number');
  return result;
}

// --- location ---------------------------------------------------------------

function currentLocation(onDone) {
  var cached = storeGet(LOCATION_CACHE_KEY, null);
  if (cached && cached.at && (Date.now() - cached.at) < LOCATION_MAX_AGE_MS) {
    onDone(null, cached);
    return;
  }
  if (typeof navigator === 'undefined' || !navigator.geolocation) {
    onDone(new Error('Location is unavailable on this phone'), null);
    return;
  }
  navigator.geolocation.getCurrentPosition(function (position) {
    var fix = {
      lat: position.coords.latitude,
      lon: position.coords.longitude,
      accuracy_m: position.coords.accuracy,
      at: Date.now(),
      place: ''
    };
    // Reverse geocoding is a nicety, not a requirement: coordinates alone are
    // still a usable answer, so a failure here must not fail the tool call.
    httpRequest({
      method: 'GET',
      timeout: 12000,
      url: 'https://api.bigdatacloud.net/data/reverse-geocode-client?latitude=' +
           fix.lat + '&longitude=' + fix.lon + '&localityLanguage=en'
    }, function (err, res) {
      if (!err && res && res.status === 200) {
        var body = safeParse(res.body, null);
        if (body) {
          var bits = [];
          if (body.locality) bits.push(body.locality);
          if (body.principalSubdivision) bits.push(body.principalSubdivision);
          if (body.countryName) bits.push(body.countryName);
          fix.place = bits.join(', ');
        }
      }
      storeSet(LOCATION_CACHE_KEY, fix);
      onDone(null, fix);
    });
  }, function (posErr) {
    onDone(new Error('Could not get a location fix' +
                     (posErr && posErr.message ? ': ' + posErr.message : '')), null);
  }, { timeout: 15000, maximumAge: 120000, enableHighAccuracy: false });
}

// --- timers -----------------------------------------------------------------

function timerState() { return storeGet(TIMER_STATE_KEY, { next_cookie: 1, timers: [] }); }

function saveTimerState(state) { storeSet(TIMER_STATE_KEY, state); }

function scheduleTimer(label, when) {
  var state = timerState();
  var cookie = state.next_cookie || 1;
  state.next_cookie = (cookie % 900) + 1;
  state.timers.push({ cookie: cookie, label: label, at: when.getTime() });

  // Drop anything that has already fired so the list stays honest.
  var live = [];
  for (var i = 0; i < state.timers.length; i++) {
    if (state.timers[i].at > Date.now() - 60000) live.push(state.timers[i]);
  }
  state.timers = live;
  saveTimerState(state);

  // Two deliveries on purpose: the watch Wakeup fires with no phone nearby, and
  // the Timeline pin makes the same reminder visible by scrolling forward.
  sendToWatch({ PEVT: PEVT_WAKEUP_SET, PINT: Math.floor(when.getTime() / 1000),
                PINT2: cookie, PSTR: trimText(label, 40) });
  addTimelinePin('pa-timer-' + cookie, label, when);
  return cookie;
}

// With no label and no cookie this clears every timer, which is what
// "cancel my timers" should do.
function cancelTimers(label, cookie) {
  var state = timerState();
  var kept = [];
  var removed = 0;
  var needle = label ? String(label).toLowerCase() : '';
  for (var i = 0; i < state.timers.length; i++) {
    var timer = state.timers[i];
    var match;
    if (isFiniteNumber(cookie)) match = (timer.cookie === cookie);
    else match = !needle || String(timer.label || '').toLowerCase().indexOf(needle) >= 0;
    if (match) {
      sendToWatch({ PEVT: PEVT_WAKEUP_CLR, PINT: timer.cookie });
      removeTimelinePin('pa-timer-' + timer.cookie);
      removed++;
    } else {
      kept.push(timer);
    }
  }
  state.timers = kept;
  saveTimerState(state);
  return removed;
}

function addTimelinePin(id, title, when) {
  if (!settings().timeline_pins) return;
  if (typeof Pebble.insertTimelinePin !== 'function') return;
  try {
    Pebble.insertTimelinePin({
      id: id,
      time: when.toISOString(),
      duration: 0,
      layout: {
        type: 'genericPin',
        title: trimText(title, 60),
        tinyIcon: 'system://images/NOTIFICATION_REMINDER'
      },
      reminders: [{
        time: when.toISOString(),
        layout: {
          type: 'genericReminder',
          title: trimText(title, 60),
          tinyIcon: 'system://images/NOTIFICATION_REMINDER'
        }
      }]
    });
  } catch (e) {
    logErr('insertTimelinePin', e);
  }
}

function removeTimelinePin(id) {
  if (typeof Pebble.deleteTimelinePin !== 'function') return;
  try { Pebble.deleteTimelinePin(id); } catch (e) { logErr('deleteTimelinePin', e); }
}

// --- tool catalogue ---------------------------------------------------------

function buildToolDefinitions() {
  var s = settings();
  var tools = [];

  if (s.web_search) tools.push({ type: 'web_search' });

  tools.push({
    type: 'function', name: 'get_current_time',
    description: 'Current local date, time and timezone on the user\'s phone.',
    parameters: { type: 'object', properties: {}, additionalProperties: false }
  });

  tools.push({
    type: 'function', name: 'calculate',
    description: 'Evaluate an arithmetic expression. Supports + - * / % ^, parentheses, ' +
                 'and sqrt, abs, round, floor, ceil, sin, cos, tan, ln, log, exp, pow, min, max.',
    parameters: {
      type: 'object',
      properties: { expression: { type: 'string', description: 'e.g. "18.5% of 240" -> "240*0.185"' } },
      required: ['expression'], additionalProperties: false
    }
  });

  if (s.location_enabled) {
    tools.push({
      type: 'function', name: 'get_location',
      description: 'The user\'s approximate current location (coordinates plus a place name).',
      parameters: { type: 'object', properties: {}, additionalProperties: false }
    });
  }

  if (calendarConfigured()) {
    tools.push({
      type: 'function', name: 'get_calendar_events',
      description: 'List calendar events in a time window. Defaults to the next 24 hours.',
      parameters: {
        type: 'object',
        properties: {
          start: { type: 'string', description: 'ISO date or date-time; defaults to now' },
          end: { type: 'string', description: 'ISO date or date-time' },
          days: { type: 'number', description: 'Window length in days when end is omitted' }
        },
        additionalProperties: false
      }
    });
    tools.push({
      type: 'function', name: 'create_calendar_event',
      description: 'Add an event to the user\'s calendar.',
      parameters: {
        type: 'object',
        properties: {
          title: { type: 'string' },
          start: { type: 'string', description: 'ISO date-time, or ISO date for an all-day event' },
          end: { type: 'string' },
          all_day: { type: 'boolean' },
          location: { type: 'string' },
          notes: { type: 'string' },
          alarm_minutes: { type: 'number', description: 'Alert this many minutes before' }
        },
        required: ['title', 'start'], additionalProperties: false
      }
    });
  }

  if (remindersConfigured()) {
    tools.push({
      type: 'function', name: 'list_reminders',
      description: 'List the user\'s reminders (to-dos).',
      parameters: {
        type: 'object',
        properties: { include_completed: { type: 'boolean' } },
        additionalProperties: false
      }
    });
    var places = (s.places || []);
    var placeNames = [];
    for (var i = 0; i < places.length; i++) if (places[i] && places[i].name) placeNames.push(places[i].name);
    tools.push({
      type: 'function', name: 'add_reminder',
      description: 'Add a reminder. Give a place to have it fire on arrival or departure ' +
                   'instead of at a time' +
                   (placeNames.length ? ' (known places: ' + placeNames.join(', ') + ')' : ''),
      parameters: {
        type: 'object',
        properties: {
          title: { type: 'string' },
          due: { type: 'string', description: 'ISO date-time, or ISO date for a whole-day reminder' },
          notes: { type: 'string' },
          place: { type: 'string', description: 'Name of a configured place, e.g. "home"' },
          proximity: { type: 'string', enum: ['arrive', 'depart'] }
        },
        required: ['title'], additionalProperties: false
      }
    });
    tools.push({
      type: 'function', name: 'complete_reminder',
      description: 'Mark a reminder as done, matched by its title.',
      parameters: {
        type: 'object',
        properties: { title: { type: 'string' } },
        required: ['title'], additionalProperties: false
      }
    });
  }

  tools.push({
    type: 'function', name: 'set_timer',
    description: 'Set a timer or alarm that vibrates the watch and appears in the Pebble ' +
                 'timeline. Works without the phone nearby. Use this for short waits; use ' +
                 'add_reminder for things that belong on a to-do list.',
    parameters: {
      type: 'object',
      properties: {
        label: { type: 'string' },
        minutes: { type: 'number', description: 'Minutes from now' },
        at: { type: 'string', description: 'ISO date-time, as an alternative to minutes' }
      },
      required: ['label'], additionalProperties: false
    }
  });
  tools.push({
    type: 'function', name: 'cancel_timer',
    description: 'Cancel timers previously set on the watch.',
    parameters: {
      type: 'object',
      properties: { label: { type: 'string', description: 'Omit to cancel every timer' } },
      additionalProperties: false
    }
  });

  if (notesConfigured()) {
    tools.push({
      type: 'function', name: 'save_note',
      description: 'Save a note to the user\'s Apple Notes.',
      parameters: {
        type: 'object',
        properties: { title: { type: 'string' }, body: { type: 'string' } },
        required: ['title'], additionalProperties: false
      }
    });
    tools.push({
      type: 'function', name: 'search_notes',
      description: 'Search the user\'s Apple Notes. Omit the query to list recent notes.',
      parameters: {
        type: 'object',
        properties: { query: { type: 'string' } },
        additionalProperties: false
      }
    });
  }

  return tools;
}

// --- dispatch ---------------------------------------------------------------

function toolResult(value) { return JSON.stringify(value); }

function toolError(message) { return JSON.stringify({ ok: false, error: String(message) }); }

function describeEvent(event) {
  var when = event.all_day
    ? friendlyDate(event.start) + ' (all day)'
    : friendlyDate(event.start) + ' ' + friendlyTime(event.start);
  return {
    title: event.summary,
    when: when,
    start: toLocalIso(event.start),
    end: event.end ? toLocalIso(event.end) : null,
    all_day: !!event.all_day,
    location: event.location || undefined
  };
}

function describeTodo(todo) {
  return {
    title: todo.summary,
    due: todo.due ? toLocalIso(todo.due) : null,
    due_friendly: todo.due ? (friendlyDate(todo.due) +
        (todo.due_all_day ? '' : ' ' + friendlyTime(todo.due))) : null,
    completed: !!todo.completed,
    location_triggered: !!todo.geofenced,
    notes: todo.notes || undefined
  };
}

function runToolCall(name, argsJson, onDone) {
  var args = safeParse(argsJson, {}) || {};

  try {
    switch (name) {
      case 'get_current_time': {
        var now = new Date();
        onDone(toolResult({
          ok: true,
          local: toLocalIso(now),
          friendly: WEEKDAYS[now.getDay()] + ' ' + MONTHS[now.getMonth()] + ' ' +
                    now.getDate() + ', ' + friendlyTime(now),
          timezone_offset: tzOffsetString(now)
        }));
        return;
      }

      case 'calculate': {
        try {
          var value = calcEvaluate(args.expression || '');
          var rounded = Math.round(value * 1e10) / 1e10;
          onDone(toolResult({ ok: true, expression: args.expression, result: rounded }));
        } catch (calcErr) {
          onDone(toolError(calcErr.message || 'Could not evaluate that'));
        }
        return;
      }

      case 'get_location': {
        currentLocation(function (err, fix) {
          if (err) { onDone(toolError(err.message)); return; }
          onDone(toolResult({
            ok: true, latitude: fix.lat, longitude: fix.lon,
            accuracy_m: fix.accuracy_m, place: fix.place || undefined
          }));
        });
        return;
      }

      case 'get_calendar_events': {
        var from = parseFlexibleDate(args.start) || new Date();
        var span = isFiniteNumber(args.days) ? args.days : 1;
        var to = parseFlexibleDate(args.end) || new Date(from.getTime() + span * 86400000);
        caldavFetchEvents(from, to, function (err, events) {
          if (err) { onDone(toolError(err.message)); return; }
          var out = [];
          for (var i = 0; i < events.length && i < 25; i++) out.push(describeEvent(events[i]));
          onDone(toolResult({ ok: true, window: { from: toLocalIso(from), to: toLocalIso(to) },
                              count: out.length, events: out }));
        });
        return;
      }

      case 'create_calendar_event': {
        var start = parseFlexibleDate(args.start);
        if (!start) { onDone(toolError('Could not understand the start time')); return; }
        caldavCreateEvent({
          title: args.title,
          start: start,
          end: parseFlexibleDate(args.end),
          allDay: !!args.all_day || /^\d{4}-\d{2}-\d{2}$/.test(String(args.start)),
          location: args.location,
          notes: args.notes,
          alarmMinutes: args.alarm_minutes
        }, function (err, created) {
          if (err) { onDone(toolError(err.message)); return; }
          onDone(toolResult({ ok: true, uid: created.uid,
                              scheduled_for: toLocalIso(start) }));
        });
        return;
      }

      case 'list_reminders': {
        caldavFetchTodos(!!args.include_completed, function (err, todos) {
          if (err) { onDone(toolError(err.message)); return; }
          var out = [];
          for (var i = 0; i < todos.length && i < 30; i++) out.push(describeTodo(todos[i]));
          onDone(toolResult({ ok: true, count: out.length, reminders: out }));
        });
        return;
      }

      case 'add_reminder': {
        var place = args.place ? findPlace(args.place) : null;
        if (args.place && !place) {
          onDone(toolError('No place called "' + args.place + '" is configured. ' +
                           'Places are added in the app settings on the phone.'));
          return;
        }
        var due = parseFlexibleDate(args.due);
        caldavCreateTodo({
          title: args.title,
          notes: args.notes,
          due: due,
          dueAllDay: !!(args.due && /^\d{4}-\d{2}-\d{2}$/.test(String(args.due))),
          place: place,
          proximity: String(args.proximity || 'arrive').toUpperCase()
        }, function (err, created) {
          if (err) { onDone(toolError(err.message)); return; }
          onDone(toolResult({
            ok: true, uid: created.uid,
            trigger: place ? ('on ' + (String(args.proximity || 'arrive').toLowerCase()) +
                              ' at ' + place.name)
                           : (due ? toLocalIso(due) : 'no due date')
          }));
        });
        return;
      }

      case 'complete_reminder': {
        caldavFetchTodos(false, function (err, todos) {
          if (err) { onDone(toolError(err.message)); return; }
          var needle = String(args.title || '').toLowerCase();
          var match = null;
          for (var i = 0; i < todos.length; i++) {
            if (String(todos[i].summary).toLowerCase().indexOf(needle) >= 0) { match = todos[i]; break; }
          }
          if (!match) { onDone(toolError('No open reminder matches "' + args.title + '"')); return; }
          caldavCompleteTodo(match, function (err2) {
            if (err2) { onDone(toolError(err2.message)); return; }
            onDone(toolResult({ ok: true, completed: match.summary }));
          });
        });
        return;
      }

      case 'set_timer': {
        var when = parseFlexibleDate(args.at);
        if (!when && isFiniteNumber(args.minutes)) {
          when = new Date(Date.now() + args.minutes * 60000);
        }
        if (!when) { onDone(toolError('Give either minutes or an absolute time')); return; }
        if (when.getTime() <= Date.now()) { onDone(toolError('That time has already passed')); return; }
        scheduleTimer(args.label || 'Reminder', when);
        onDone(toolResult({ ok: true, fires_at: toLocalIso(when),
                            friendly: friendlyDate(when) + ' ' + friendlyTime(when) }));
        return;
      }

      case 'cancel_timer': {
        var removed = cancelTimers(args.label);
        onDone(toolResult({ ok: removed > 0, cancelled: removed }));
        return;
      }

      case 'save_note': {
        notesSave(args.title || 'Note', args.body || '', function (err, saved) {
          if (err) { onDone(toolError(err.message)); return; }
          onDone(toolResult({ ok: true, id: saved.id, title: args.title }));
        });
        return;
      }

      case 'search_notes': {
        notesSearch(args.query || '', 8, function (err, notes) {
          if (err) { onDone(toolError(err.message)); return; }
          onDone(toolResult({ ok: true, count: notes.length, notes: notes }));
        });
        return;
      }

      default:
        onDone(toolError('Unknown tool "' + name + '"'));
    }
  } catch (e) {
    logErr('runToolCall ' + name, e);
    onDone(toolError(e.message || 'Tool failed'));
  }
}

function runToolCallsSequentially(calls, onStatus, onDone) {
  var results = [];
  var index = 0;

  function next() {
    if (index >= calls.length) { onDone(results); return; }
    var call = calls[index++];
    if (onStatus) onStatus(toolStatusLabel(call.name), SPIN_TOOL);
    runToolCall(call.name, call.arguments, function (output) {
      results.push({ call_id: call.call_id, output: output });
      next();
    });
  }
  next();
}
