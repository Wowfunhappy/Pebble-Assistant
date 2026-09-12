//
// Apple Notes, via Gmail.
//
// When Apple Notes is set up against a Gmail account rather than iCloud, each
// note is stored as an IMAP message in a mailbox Gmail shows as the label
// "Notes".  The message is an ordinary email carrying two Apple-specific
// headers -- X-Uniform-Type-Identifier: com.apple.mail-note and a stable UUID --
// and an HTML body whose first line is the note's title.
//
// So writing a note is inserting such a message under that label with the Gmail
// API, and reading notes is a Gmail search.  Nothing here touches the user's
// mail: every call is scoped to the Notes label.
//

var GOOGLE_TOKEN_URL = 'https://oauth2.googleapis.com/token';
var GMAIL_API = 'https://gmail.googleapis.com/gmail/v1/users/me';
var GOOGLE_TOKEN_CACHE = 'google_token_v1';
var GMAIL_META_CACHE = 'gmail_meta_v1';

function googleAccessToken(onDone) {
  var s = settings();
  if (!s.gmail_client_id || !s.gmail_refresh_token) {
    onDone(new Error('Notes are not connected. Add a Google token in Settings.'), null);
    return;
  }

  var cached = storeGet(GOOGLE_TOKEN_CACHE, null);
  if (cached && cached.token && cached.expires_at > Date.now() + 60000 &&
      cached.client_id === s.gmail_client_id) {
    onDone(null, cached.token);
    return;
  }

  var form = 'client_id=' + encodeURIComponent(s.gmail_client_id) +
             '&refresh_token=' + encodeURIComponent(s.gmail_refresh_token) +
             '&grant_type=refresh_token';
  if (s.gmail_client_secret) {
    form += '&client_secret=' + encodeURIComponent(s.gmail_client_secret);
  }

  httpRequest({
    method: 'POST',
    url: GOOGLE_TOKEN_URL,
    timeout: 30000,
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: form
  }, function (err, res) {
    if (err || !res) { onDone(err || new Error('Google sign-in failed'), null); return; }
    var body = safeParse(res.body, null);
    if (res.status < 200 || res.status >= 300 || !body || !body.access_token) {
      var reason = (body && (body.error_description || body.error)) || ('HTTP ' + res.status);
      onDone(new Error('Google sign-in failed: ' + reason), null);
      return;
    }
    storeSet(GOOGLE_TOKEN_CACHE, {
      token: body.access_token,
      client_id: s.gmail_client_id,
      expires_at: Date.now() + ((body.expires_in || 3600) * 1000)
    });
    onDone(null, body.access_token);
  });
}

function gmailRequest(method, path, body, onDone) {
  googleAccessToken(function (err, token) {
    if (err) { onDone(err, null); return; }
    var headers = { 'Authorization': 'Bearer ' + token };
    if (body) headers['Content-Type'] = 'application/json';
    httpRequest({
      method: method,
      url: GMAIL_API + path,
      timeout: 40000,
      headers: headers,
      body: body ? JSON.stringify(body) : null
    }, function (err2, res) {
      if (err2) { onDone(err2, null); return; }
      var parsed = safeParse(res.body, null);
      if (res.status === 401 || res.status === 403) {
        storeDel(GOOGLE_TOKEN_CACHE);
        var detail = parsed && parsed.error && parsed.error.message;
        onDone(new Error('Gmail refused the request: ' + (detail || res.status)), null);
        return;
      }
      if (res.status < 200 || res.status >= 300) {
        var msg = (parsed && parsed.error && parsed.error.message) || ('HTTP ' + res.status);
        onDone(new Error('Gmail error: ' + msg), null);
        return;
      }
      onDone(null, parsed || {});
    });
  });
}

// The Notes label id and the account's own address, both cached: they never
// change, and every note write needs them.
function gmailMeta(onDone) {
  var s = settings();
  var wanted = s.notes_label || 'Notes';
  var cached = storeGet(GMAIL_META_CACHE, null);
  if (cached && cached.label_id && cached.label_name === wanted && cached.email) {
    onDone(null, cached);
    return;
  }

  gmailRequest('GET', '/profile', null, function (err, profile) {
    if (err) { onDone(err, null); return; }
    gmailRequest('GET', '/labels', null, function (err2, labels) {
      if (err2) { onDone(err2, null); return; }
      var list = (labels && labels.labels) || [];
      var found = null;
      for (var i = 0; i < list.length; i++) {
        if (list[i] && list[i].name === wanted) { found = list[i]; break; }
      }
      function done(label) {
        var meta = {
          label_id: label.id,
          label_name: wanted,
          email: profile.emailAddress || ''
        };
        storeSet(GMAIL_META_CACHE, meta);
        onDone(null, meta);
      }
      if (found) { done(found); return; }
      // Apple Mail creates this label the first time it syncs a note; create it
      // ourselves so the first note from the watch works on a fresh account.
      gmailRequest('POST', '/labels', {
        name: wanted,
        labelListVisibility: 'labelShow',
        messageListVisibility: 'show'
      }, function (err3, created) {
        if (err3) { onDone(err3, null); return; }
        done(created);
      });
    });
  });
}

function rfc2047(text) {
  if (/^[\x20-\x7e]*$/.test(text)) return text;
  return '=?UTF-8?B?' + base64Encode(text, false) + '?=';
}

function htmlEscape(text) {
  return String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                     .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function rfc2822Date(date) {
  var days = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
  var offset = -date.getTimezoneOffset();
  var sign = offset >= 0 ? '+' : '-';
  var abs = Math.abs(offset);
  return days[date.getDay()] + ', ' + date.getDate() + ' ' + MONTHS[date.getMonth()] + ' ' +
         date.getFullYear() + ' ' + pad2(date.getHours()) + ':' + pad2(date.getMinutes()) +
         ':' + pad2(date.getSeconds()) + ' ' + sign + pad2(Math.floor(abs / 60)) + pad2(abs % 60);
}

function buildAppleNoteMessage(email, title, bodyText) {
  var now = new Date();
  var noteUuid = uuid4().toUpperCase();
  var paragraphs = String(bodyText || '').split(/\r?\n/);
  var html = '<html><head><meta http-equiv="Content-Type" content="text/html; charset=utf-8"></head>' +
             '<body><div>' + htmlEscape(title) + '</div><div><br></div>';
  for (var i = 0; i < paragraphs.length; i++) {
    html += '<div>' + (paragraphs[i] ? htmlEscape(paragraphs[i]) : '<br>') + '</div>';
  }
  html += '</body></html>';

  var headers = [
    'From: ' + email,
    'To: ' + email,
    'Subject: ' + rfc2047(title),
    'Date: ' + rfc2822Date(now),
    'X-Uniform-Type-Identifier: com.apple.mail-note',
    'X-Universally-Unique-Identifier: ' + noteUuid,
    'X-Mail-Created-Date: ' + rfc2822Date(now),
    'Mime-Version: 1.0',
    'Content-Type: text/html; charset=utf-8',
    'Content-Transfer-Encoding: base64',
    '',
    base64Encode(html, false)
  ];
  return { raw: base64Encode(headers.join('\r\n'), true), uuid: noteUuid };
}

function notesSave(title, bodyText, onDone) {
  gmailMeta(function (err, meta) {
    if (err) { onDone(err, null); return; }
    if (!meta.email) { onDone(new Error('Could not read the Gmail address'), null); return; }
    var message = buildAppleNoteMessage(meta.email, title, bodyText);
    // insert (not import) so the note bypasses filters and classification and
    // lands in the Notes mailbox exactly as Apple Mail would have written it.
    gmailRequest('POST', '/messages', {
      raw: message.raw,
      labelIds: [meta.label_id]
    }, function (err2, created) {
      if (err2) { onDone(err2, null); return; }
      onDone(null, { id: created && created.id, uuid: message.uuid });
    });
  });
}

function notesSearch(query, limit, onDone) {
  gmailMeta(function (err, meta) {
    if (err) { onDone(err, null); return; }
    var q = 'label:"' + (meta.label_name || 'Notes') + '"';
    if (query) q += ' ' + query;
    var path = '/messages?maxResults=' + (limit || 8) + '&q=' + encodeURIComponent(q);

    gmailRequest('GET', path, null, function (err2, list) {
      if (err2) { onDone(err2, null); return; }
      var ids = (list && list.messages) || [];
      if (!ids.length) { onDone(null, []); return; }

      var notes = [];
      var index = 0;
      function next() {
        if (index >= ids.length) { onDone(null, notes); return; }
        var id = ids[index++].id;
        gmailRequest('GET', '/messages/' + id +
            '?format=metadata&metadataHeaders=Subject&metadataHeaders=Date', null,
          function (err3, message) {
            if (!err3 && message) {
              var subject = '';
              var when = '';
              var headers = (message.payload && message.payload.headers) || [];
              for (var i = 0; i < headers.length; i++) {
                if (headers[i].name === 'Subject') subject = headers[i].value;
                else if (headers[i].name === 'Date') when = headers[i].value;
              }
              notes.push({
                id: id,
                title: subject || '(untitled)',
                date: when,
                snippet: message.snippet || ''
              });
            }
            next();
          });
      }
      next();
    });
  });
}
