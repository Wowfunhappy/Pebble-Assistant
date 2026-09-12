//
// The Codex backend client.
//
// This talks to the same endpoint the Codex CLI uses when you are signed in with
// a ChatGPT account: POST https://chatgpt.com/backend-api/codex/responses, a
// streaming Responses API call authenticated with the OAuth access token plus
// the chatgpt-account-id header.  Streaming is not optional there, so the reply
// arrives as SSE and is parsed out of the response body.
//

var CODEX_URL = 'https://chatgpt.com/backend-api/codex/responses';
var CODEX_ORIGINATOR = 'codex_cli_rs';
var MAX_TOOL_ROUNDS = 6;

// --- SSE --------------------------------------------------------------------

function parseSseEvents(text) {
  var events = [];
  var lines = String(text).split(/\r?\n/);
  var buffer = [];
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i];
    if (line === '') {
      if (buffer.length) { events.push(buffer.join('\n')); buffer = []; }
      continue;
    }
    if (line.charAt(0) === ':') continue;             // comment/keepalive
    if (line.indexOf('data:') === 0) {
      var payload = line.substring(5);
      if (payload.charAt(0) === ' ') payload = payload.substring(1);
      buffer.push(payload);
    }
  }
  if (buffer.length) events.push(buffer.join('\n'));
  return events;
}

// Walks the stream and returns the produced output items, or an error.
//
// The Codex backend sends `response.completed` with an EMPTY `output` array --
// the items themselves only ever arrive as `response.output_item.done` events,
// so they have to be accumulated as they stream past.  Reading `output` off the
// completion event alone yields nothing at all.  Other Responses-compatible
// backends do populate it, so a non-empty one wins if it shows up.
function readResponseStream(text) {
  var events = parseSseEvents(text);
  var items = [];
  var completed = false;
  var failure = null;

  for (var i = 0; i < events.length; i++) {
    if (events[i] === '[DONE]') continue;
    var ev = safeParse(events[i], null);
    if (!ev) continue;

    if (ev.type === 'response.output_item.done') {
      if (ev.item) items.push(ev.item);
    } else if (ev.type === 'response.completed') {
      completed = true;
      var carried = (ev.response && ev.response.output) || [];
      if (carried.length) items = carried;
    } else if (ev.type === 'response.failed' || ev.type === 'response.incomplete') {
      var reason = (ev.response && ev.response.error && ev.response.error.message) ||
                   (ev.response && ev.response.incomplete_details &&
                    ev.response.incomplete_details.reason) || 'Model stopped early';
      failure = new Error(reason);
    } else if (ev.type === 'error') {
      failure = new Error((ev.error && ev.error.message) || ev.message || 'Stream error');
    }
  }

  if (failure) return { output: null, error: failure };
  // Each output_item.done is a complete item, so a stream cut short after one
  // still carries usable content; only a stream with nothing in it is a failure.
  if (!items.length) {
    return { output: null, error: new Error(completed ? 'Empty reply' : 'No reply received') };
  }
  return { output: items, error: null };
}

// Turns a partial stream into a short status line for the watch.
function progressStatusFrom(partialText) {
  var events = parseSseEvents(partialText);
  var status = null;
  for (var i = 0; i < events.length; i++) {
    var ev = safeParse(events[i], null);
    if (!ev || !ev.type) continue;
    if (ev.type === 'response.output_item.added' && ev.item) {
      if (ev.item.type === 'web_search_call') status = { text: 'Searching the web', spin: SPIN_SEARCHING };
      else if (ev.item.type === 'function_call') status = { text: toolStatusLabel(ev.item.name), spin: SPIN_TOOL };
      else if (ev.item.type === 'reasoning') status = { text: 'Thinking', spin: SPIN_THINKING };
    } else if (ev.type === 'response.output_text.delta') {
      status = { text: 'Writing', spin: SPIN_THINKING };
    }
  }
  return status;
}

// --- request ----------------------------------------------------------------

// Items echoed back from a previous response keep their reasoning payloads (the
// model needs them) but lose server-assigned ids, which the API rejects on a
// store:false request.
function sanitizeItem(item) {
  if (!item || typeof item !== 'object') return item;
  var copy = {};
  for (var key in item) {
    if (!Object.prototype.hasOwnProperty.call(item, key)) continue;
    if (key === 'id' || key === 'status') continue;
    copy[key] = item[key];
  }
  return copy;
}

function sanitizeInput(input) {
  var out = [];
  for (var i = 0; i < input.length; i++) out.push(sanitizeItem(input[i]));
  return out;
}

function userMessage(text) {
  return { type: 'message', role: 'user', content: [{ type: 'input_text', text: String(text) }] };
}

function assistantMessage(text) {
  return { type: 'message', role: 'assistant', content: [{ type: 'output_text', text: String(text) }] };
}

function extractAssistantText(output) {
  var parts = [];
  for (var i = 0; i < output.length; i++) {
    var item = output[i];
    if (!item || item.type !== 'message' || !item.content) continue;
    for (var j = 0; j < item.content.length; j++) {
      var block = item.content[j];
      if (block && (block.type === 'output_text' || block.type === 'text') && block.text) {
        parts.push(block.text);
      }
    }
  }
  return parts.join('\n').replace(/^\s+|\s+$/g, '');
}

function codexPost(body, sessionId, onStatus, onDone, isRetry) {
  ensureAccessToken(function (authErr, tokens) {
    if (authErr) { onDone(authErr, null); return; }

    var headers = {
      'Content-Type': 'application/json',
      'Accept': 'text/event-stream',
      'Authorization': 'Bearer ' + tokens.access_token,
      'OpenAI-Beta': 'responses=experimental',
      'originator': CODEX_ORIGINATOR,
      'session_id': sessionId
    };
    if (tokens.account_id) headers['chatgpt-account-id'] = tokens.account_id;

    var lastStatus = '';
    httpRequest({
      method: 'POST',
      url: CODEX_URL,
      headers: headers,
      timeout: 180000,
      body: JSON.stringify(body),
      onProgress: onStatus ? function (partial) {
        var status = progressStatusFrom(partial);
        if (status && status.text !== lastStatus) {
          lastStatus = status.text;
          onStatus(status.text, status.spin);
        }
      } : null
    }, function (err, res) {
      if (err || !res) { onDone(err || new Error('Network error'), null); return; }

      if (res.status === 401 && !isRetry) {
        // The token went stale mid-flight; force a refresh and try once more.
        storeDel(AUTH_CACHE_KEY);
        codexPost(body, sessionId, onStatus, onDone, true);
        return;
      }
      if (res.status === 429) {
        onDone(new Error('Rate limited. Try again shortly.'), null);
        return;
      }
      if (res.status < 200 || res.status >= 300) {
        var detail = safeParse(res.body, null);
        var message = (detail && detail.error && detail.error.message) ||
                      (detail && detail.detail) || '';
        if (res.status === 400 && /model/i.test(message)) {
          message = 'This model is not available on your plan.';
        }
        onDone(new Error(message || ('Request failed (' + res.status + ')')), null);
        return;
      }

      var parsed = readResponseStream(res.body);
      if (parsed.error) { onDone(parsed.error, null); return; }
      onDone(null, parsed.output);
    });
  });
}

// --- the turn loop ----------------------------------------------------------
//
// One user question can take several round trips: the model asks for a tool, we
// run it, and the result goes back as another request.  Reasoning items are
// carried forward verbatim so the model keeps its chain of thought across those
// hops, which is what the encrypted_content include is for.
//

function codexRunTurn(opts) {
  var input = sanitizeInput(opts.input.slice());
  var sessionId = opts.sessionId || uuid4();
  var rounds = 0;
  var cancelled = false;

  function finish(err, result) {
    if (cancelled) return;
    opts.onDone(err, result);
  }

  function step() {
    if (cancelled) return;
    rounds++;
    if (rounds > MAX_TOOL_ROUNDS) {
      finish(new Error('Gave up after too many tool steps'), null);
      return;
    }

    var body = {
      model: opts.model,
      instructions: opts.instructions,
      input: input,
      tools: opts.tools,
      tool_choice: 'auto',
      parallel_tool_calls: false,
      reasoning: { effort: opts.effort, summary: 'auto' },
      store: false,
      stream: true,
      include: ['reasoning.encrypted_content'],
      prompt_cache_key: sessionId
    };

    codexPost(body, sessionId, opts.onStatus, function (err, output) {
      if (err) { finish(err, null); return; }
      output = output || [];
      input = input.concat(sanitizeInput(output));

      var calls = [];
      for (var i = 0; i < output.length; i++) {
        if (output[i] && output[i].type === 'function_call') calls.push(output[i]);
      }

      if (!calls.length) {
        finish(null, { text: extractAssistantText(output), input: input, session_id: sessionId });
        return;
      }

      runToolCallsSequentially(calls, opts.onStatus, function (results) {
        if (cancelled) return;
        for (var j = 0; j < results.length; j++) {
          input.push({
            type: 'function_call_output',
            call_id: results[j].call_id,
            output: results[j].output
          });
        }
        step();
      });
    });
  }

  step();
  return { cancel: function () { cancelled = true; } };
}
