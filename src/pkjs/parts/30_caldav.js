//
// CalDAV.
//
// Two independent accounts are supported because that is how the author's data
// actually lives: calendars on iCloud, reminders on a Baikal server.  Each is
// just a base URL plus username/password; the collection URL is discovered once
// and cached, or can be pinned by hand in Settings when discovery is awkward.
//
// PebbleKit JS only gives us XMLHttpRequest, so PROPFIND/REPORT go through
// xhr.open() with a custom verb.  If a companion app ever refuses those verbs
// the failure is reported verbatim by the self-test rather than silently
// degrading, because there is no honest fallback for a two-way protocol.
//

var CALDAV_CACHE_KEY = 'caldav_cache_v1';
var DAV_NS = 'xmlns:d="DAV:" xmlns:c="urn:ietf:params:xml:ns:caldav" xmlns:cs="http://calendarserver.org/ns/"';
var APPLE_PROXIMITY_TRIGGER = '19760401T005545Z';

// --- tiny XML helpers (no DOMParser in PebbleKit JS) ------------------------

function xmlTagPattern(tag, flags) {
  return new RegExp('<(?:[A-Za-z0-9_-]+:)?' + tag + '(?:\\s[^>]*)?>([\\s\\S]*?)<\\/(?:[A-Za-z0-9_-]+:)?' + tag + '>', flags || '');
}

function xmlFindAll(xml, tag) {
  var out = [];
  var re = xmlTagPattern(tag, 'g');
  var match;
  while ((match = re.exec(xml)) !== null) out.push(match[1]);
  return out;
}

function xmlFindFirst(xml, tag) {
  var match = xmlTagPattern(tag).exec(xml);
  return match ? match[1] : null;
}

function xmlHasTag(xml, tag) {
  return new RegExp('<(?:[A-Za-z0-9_-]+:)?' + tag + '(?:[\\s/][^>]*)?/?>').test(xml);
}

function xmlUnescape(text) {
  if (typeof text !== 'string') return '';
  return text.replace(/&lt;/g, '<').replace(/&gt;/g, '>')
             .replace(/&quot;/g, '"').replace(/&#39;/g, "'")
             .replace(/&amp;/g, '&');
}

function xmlEscape(text) {
  return String(text).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// --- URL helpers ------------------------------------------------------------

function urlOrigin(url) {
  var match = /^(https?:\/\/[^/]+)/i.exec(url);
  return match ? match[1] : '';
}

function resolveHref(baseUrl, href) {
  if (!href) return '';
  href = xmlUnescape(String(href)).replace(/^\s+|\s+$/g, '');
  if (/^https?:\/\//i.test(href)) return href;
  if (href.charAt(0) === '/') return urlOrigin(baseUrl) + href;
  var base = baseUrl.replace(/[^/]*$/, '');
  return base + href;
}

// --- account plumbing -------------------------------------------------------

function caldavAccount(kind) {
  var s = settings();
  if (kind === 'calendar') {
    return { kind: kind, url: s.cal_url, user: s.cal_user, pass: s.cal_pass,
             collection: s.cal_collection, component: 'VEVENT' };
  }
  return { kind: kind, url: s.rem_url, user: s.rem_user, pass: s.rem_pass,
           collection: s.rem_collection, component: 'VTODO' };
}

function caldavHeaders(account, extra) {
  var headers = { 'Content-Type': 'application/xml; charset=utf-8' };
  for (var key in (extra || {})) {
    if (Object.prototype.hasOwnProperty.call(extra, key)) headers[key] = extra[key];
  }
  return headers;
}

// --- authentication ---------------------------------------------------------
//
// iCloud and most hosted servers accept Basic.  Baikal -- and SabreDAV installs
// generally -- default to Digest and reject Basic outright.  Rather than making
// the user know which their server speaks, every request starts as Basic and, if
// the server answers 401 with a Digest challenge, is redone properly.  The
// challenge is cached per host -- not per account, since the realm belongs to the
// server -- so two accounts on one server share it and only the first request of
// a session pays for the extra round trip.

var _digestChallenges = {};

function parseDigestChallenge(header) {
  if (!header) return null;
  var match = /Digest\s+([\s\S]*)$/i.exec(header);
  if (!match) return null;
  var challenge = { realm: '', nonce: '', opaque: '', qop: '', algorithm: '', nc: 0 };
  var pattern = /([a-zA-Z0-9_-]+)\s*=\s*(?:"([^"]*)"|([^,\s]+))/g;
  var found;
  while ((found = pattern.exec(match[1])) !== null) {
    var value = (typeof found[2] === 'string') ? found[2] : found[3];
    challenge[found[1].toLowerCase()] = value;
  }
  return challenge.nonce ? challenge : null;
}

// Digest signs the request path, not the whole URL.
function requestPath(url) {
  var match = /^https?:\/\/[^/]+(\/[\s\S]*)$/i.exec(url);
  return match ? match[1] : '/';
}

function randomHex(length) {
  var out = '';
  while (out.length < length) out += Math.floor(Math.random() * 16).toString(16);
  return out.substring(0, length);
}

function digestAuthorization(account, challenge, method, url) {
  var algorithm = String(challenge.algorithm || 'MD5').toUpperCase();
  if (algorithm !== 'MD5' && algorithm !== 'MD5-SESS') return null;

  var qop = '';
  if (challenge.qop) {
    var offered = String(challenge.qop).split(',');
    for (var i = 0; i < offered.length; i++) {
      if (offered[i].replace(/^\s+|\s+$/g, '') === 'auth') qop = 'auth';
    }
    if (!qop) return null;   // auth-int is not implemented
  }

  var uri = requestPath(url);
  var cnonce = randomHex(16);
  challenge.nc = (challenge.nc || 0) + 1;
  var nc = ('00000000' + challenge.nc.toString(16));
  nc = nc.substring(nc.length - 8);

  var ha1 = md5Hex(account.user + ':' + challenge.realm + ':' + account.pass);
  if (algorithm === 'MD5-SESS') ha1 = md5Hex(ha1 + ':' + challenge.nonce + ':' + cnonce);
  var ha2 = md5Hex(method + ':' + uri);
  var response = qop
    ? md5Hex(ha1 + ':' + challenge.nonce + ':' + nc + ':' + cnonce + ':' + qop + ':' + ha2)
    : md5Hex(ha1 + ':' + challenge.nonce + ':' + ha2);

  var parts = ['username="' + account.user + '"', 'realm="' + challenge.realm + '"',
               'nonce="' + challenge.nonce + '"', 'uri="' + uri + '"',
               'response="' + response + '"'];
  if (challenge.opaque) parts.push('opaque="' + challenge.opaque + '"');
  if (challenge.algorithm) parts.push('algorithm=' + challenge.algorithm);
  if (qop) {
    parts.push('qop=' + qop);
    parts.push('nc=' + nc);
    parts.push('cnonce="' + cnonce + '"');
  }
  return 'Digest ' + parts.join(', ');
}

function readHeader(res, name) {
  try {
    if (res && res.xhr && res.xhr.getResponseHeader) {
      return res.xhr.getResponseHeader(name) || '';
    }
  } catch (e) { /* some hosts hide response headers */ }
  return '';
}

function readAuthHeader(res) { return readHeader(res, 'WWW-Authenticate'); }

// Where the request actually ended up.  iCloud bounces callers from the
// bootstrap host to a per-account shard, and relative hrefs in the reply are
// relative to the shard, not to what we asked for.
function effectiveUrl(res, requested) {
  try {
    if (res && res.xhr && res.xhr.responseURL) return res.xhr.responseURL;
  } catch (e) { /* not all hosts expose it */ }
  return (res && res.finalUrl) || requested;
}

// Every CalDAV request goes through here so authentication is handled in exactly
// one place.  Returns the raw response; status interpretation is the caller's.
function davSend(account, method, url, body, extraHeaders, onDone, isRetry, hops) {
  var headers = caldavHeaders(account, extraHeaders);
  var host = urlOrigin(url) || account.kind;
  var challenge = _digestChallenges[host];
  var authorization = challenge ? digestAuthorization(account, challenge, method, url) : null;
  headers.Authorization = authorization ||
      ('Basic ' + base64Encode(account.user + ':' + account.pass));

  httpRequest({ method: method, url: url, headers: headers, body: body, timeout: 40000 },
    function (err, res) {
      if (err) { onDone(err, res); return; }

      if (res.status === 401 && !isRetry) {
        var next = parseDigestChallenge(readAuthHeader(res));
        if (next) {
          _digestChallenges[host] = next;
          davSend(account, method, url, body, extraHeaders, onDone, true, hops);
          return;
        }
      }

      // iCloud answers the bootstrap host with a 301 to the account's shard.
      // Following it here keeps the method and the body, which a redirect
      // followed automatically may not: a PROPFIND turned into a GET comes back
      // 200 with a body that parses to nothing, and the failure then looks like
      // an empty calendar rather than a redirect.
      if (res.status === 301 || res.status === 302 ||
          res.status === 307 || res.status === 308) {
        var location = readHeader(res, 'Location');
        var depth = hops || 0;
        if (location && depth < 5) {
          // A new host may want its own credentials challenge, so allow one.
          davSend(account, method, resolveHref(url, location), body, extraHeaders,
                  onDone, false, depth + 1);
          return;
        }
        if (!location) {
          onDone(new Error('Server redirected without saying where (' + res.status + ')'), res);
          return;
        }
      }

      res.finalUrl = url;
      onDone(null, res);
    });
}

function caldavRequest(account, method, url, body, extraHeaders, onDone) {
  davSend(account, method, url, body, extraHeaders, function (err, res) {
    if (err) { onDone(err, null); return; }
    if (res.status === 401 || res.status === 403) {
      var hint = /digest/i.test(readAuthHeader(res) + ' ' + (res.body || ''))
        ? 'CalDAV needs Digest auth and the server would not accept ours -- check the password'
        : 'CalDAV rejected the username or password';
      onDone(new Error(hint), res);
      return;
    }
    if (res.status === 405 || res.status === 501) {
      onDone(new Error('The Pebble app refused a ' + method + ' request'), res);
      return;
    }
    if (res.status < 200 || res.status >= 400) {
      onDone(new Error('CalDAV ' + method + ' failed (' + res.status + ')'), res);
      return;
    }
    onDone(null, res);
  });
}

// --- discovery --------------------------------------------------------------

function caldavCache() { return storeGet(CALDAV_CACHE_KEY, {}); }

function caldavCachedCollection(account) {
  if (account.collection) return account.collection;
  var cache = caldavCache();
  var entry = cache[account.kind];
  if (entry && entry.url === account.url && entry.user === account.user) return entry.collection;
  return '';
}

function caldavRememberCollection(account, collection) {
  var cache = caldavCache();
  cache[account.kind] = { url: account.url, user: account.user, collection: collection };
  storeSet(CALDAV_CACHE_KEY, cache);
}

function propfind(account, url, depth, body, onDone) {
  caldavRequest(account, 'PROPFIND', url, body, { 'Depth': String(depth) }, onDone);
}

// current-user-principal -> calendar-home-set -> the first collection that holds
// the component we want.  Each step is skipped when the answer is already known.
function caldavDiscover(account, onDone) {
  var known = caldavCachedCollection(account);
  if (known) { onDone(null, known); return; }
  if (!account.url || !account.user || !account.pass) {
    onDone(new Error('CalDAV is not configured'), null);
    return;
  }

  var principalBody = '<?xml version="1.0" encoding="utf-8"?>' +
      '<d:propfind ' + DAV_NS + '><d:prop><d:current-user-principal/></d:prop></d:propfind>';

  propfind(account, account.url, 0, principalBody, function (err, res) {
    if (err) { onDone(err, null); return; }
    var principalBlock = xmlFindFirst(res.body, 'current-user-principal');
    var principalHref = principalBlock ? xmlFindFirst(principalBlock, 'href') : null;
    var bootstrapUrl = effectiveUrl(res, account.url);
    var principalUrl = principalHref ? resolveHref(bootstrapUrl, principalHref) : bootstrapUrl;

    var homeBody = '<?xml version="1.0" encoding="utf-8"?>' +
        '<d:propfind ' + DAV_NS + '><d:prop><c:calendar-home-set/></d:prop></d:propfind>';

    propfind(account, principalUrl, 0, homeBody, function (err2, res2) {
      if (err2) { onDone(err2, null); return; }
      var homeBlock = xmlFindFirst(res2.body, 'calendar-home-set');
      var homeHref = homeBlock ? xmlFindFirst(homeBlock, 'href') : null;
      if (!homeHref) {
        onDone(new Error('Signed in, but the server listed no calendar home'), null);
        return;
      }
      var homeUrl = resolveHref(effectiveUrl(res2, principalUrl), homeHref);

      var listBody = '<?xml version="1.0" encoding="utf-8"?>' +
          '<d:propfind ' + DAV_NS + '><d:prop>' +
          '<d:resourcetype/><d:displayname/><c:supported-calendar-component-set/>' +
          '</d:prop></d:propfind>';

      propfind(account, homeUrl, 1, listBody, function (err3, res3) {
        if (err3) { onDone(err3, null); return; }
        var responses = xmlFindAll(res3.body, 'response');
        var fallback = '';
        for (var i = 0; i < responses.length; i++) {
          var block = responses[i];
          if (!xmlHasTag(block, 'calendar')) continue;        // not a calendar collection
          var href = xmlFindFirst(block, 'href');
          if (!href) continue;
          var collectionUrl = resolveHref(effectiveUrl(res3, homeUrl), href);
          var supported = xmlFindFirst(block, 'supported-calendar-component-set') || '';
          var wants = new RegExp('name="' + account.component + '"', 'i');
          if (wants.test(supported)) {
            caldavRememberCollection(account, collectionUrl);
            onDone(null, collectionUrl);
            return;
          }
          if (!supported && !fallback) fallback = collectionUrl;
        }
        if (fallback) {
          caldavRememberCollection(account, fallback);
          onDone(null, fallback);
          return;
        }
        onDone(new Error('No ' + (account.component === 'VTODO' ? 'reminder' : 'calendar') +
                         ' list found on that server'), null);
      });
    });
  });
}

// --- iCalendar --------------------------------------------------------------

function icalUnfold(text) {
  return String(text).replace(/\r\n/g, '\n').replace(/\n[ \t]/g, '');
}

function icalFold(line) {
  if (line.length <= 73) return line;
  var out = line.substring(0, 73);
  var rest = line.substring(73);
  while (rest.length > 72) {
    out += '\r\n ' + rest.substring(0, 72);
    rest = rest.substring(72);
  }
  return out + (rest.length ? '\r\n ' + rest : '');
}

function icalEscape(text) {
  return String(text).replace(/\\/g, '\\\\').replace(/;/g, '\;')
                     .replace(/,/g, '\\,').replace(/\r?\n/g, '\\n');
}

function icalUnescape(text) {
  return String(text).replace(/\\n/gi, '\n').replace(/\\,/g, ',')
                     .replace(/\;/g, ';').replace(/\\\\/g, '\\');
}

// Returns [{ props: { NAME: { value, params } }, raw }] for each component.
function icalParseComponents(text, componentName) {
  var unfolded = icalUnfold(text);
  var lines = unfolded.split('\n');
  var components = [];
  var current = null;
  var depth = 0;

  for (var i = 0; i < lines.length; i++) {
    var line = lines[i].replace(/\s+$/, '');
    if (!line) continue;
    if (line === 'BEGIN:' + componentName) {
      current = { props: {}, raw: [line] };
      depth = 1;
      continue;
    }
    if (!current) continue;
    current.raw.push(line);
    if (line.indexOf('BEGIN:') === 0) { depth++; continue; }
    if (line.indexOf('END:') === 0) {
      depth--;
      if (depth === 0) {
        current.raw = current.raw.join('\r\n');
        components.push(current);
        current = null;
      }
      continue;
    }
    if (depth !== 1) continue;   // a property of a nested component (e.g. VALARM)

    var colon = line.indexOf(':');
    if (colon < 0) continue;
    var head = line.substring(0, colon);
    var value = line.substring(colon + 1);
    var segments = head.split(';');
    var name = segments[0].toUpperCase();
    var params = {};
    for (var p = 1; p < segments.length; p++) {
      var eq = segments[p].indexOf('=');
      if (eq > 0) params[segments[p].substring(0, eq).toUpperCase()] =
          segments[p].substring(eq + 1).replace(/^"|"$/g, '');
    }
    if (!current.props[name]) current.props[name] = { value: value, params: params };
  }
  return components;
}

function icalProp(component, name) {
  var prop = component.props[name];
  return prop ? icalUnescape(prop.value) : '';
}

// iCal dates come in three shapes.  TZID-qualified values are read as local
// time: shipping a timezone database to a watch companion is not worth it, and
// everything the user asks about is in the timezone they are standing in.
function icalParseDate(component, name) {
  var prop = component.props[name];
  if (!prop) return null;
  var value = prop.value.replace(/^\s+|\s+$/g, '');
  var allDay = (prop.params.VALUE === 'DATE') || /^\d{8}$/.test(value);
  var match = /^(\d{4})(\d{2})(\d{2})(?:T(\d{2})(\d{2})(\d{2})(Z)?)?$/.exec(value);
  if (!match) {
    var loose = parseFlexibleDate(value);
    return loose ? { date: loose, allDay: false } : null;
  }
  var year = parseInt(match[1], 10), month = parseInt(match[2], 10) - 1, day = parseInt(match[3], 10);
  var hour = match[4] ? parseInt(match[4], 10) : 0;
  var minute = match[5] ? parseInt(match[5], 10) : 0;
  var second = match[6] ? parseInt(match[6], 10) : 0;
  var date = match[7] === 'Z'
    ? new Date(Date.UTC(year, month, day, hour, minute, second))
    : new Date(year, month, day, hour, minute, second);
  return { date: date, allDay: allDay };
}

function buildIcs(componentName, lines) {
  var body = ['BEGIN:VCALENDAR', 'VERSION:2.0', 'PRODID:-//Pebble Assistant//EN',
              'CALSCALE:GREGORIAN', 'BEGIN:' + componentName];
  for (var i = 0; i < lines.length; i++) {
    if (lines[i]) body.push(icalFold(lines[i]));
  }
  body.push('END:' + componentName, 'END:VCALENDAR');
  return body.join('\r\n') + '\r\n';
}

// --- queries ----------------------------------------------------------------

function caldavReport(account, collectionUrl, body, onDone) {
  caldavRequest(account, 'REPORT', collectionUrl, body, { 'Depth': '1' }, onDone);
}

function calendarDataFrom(multistatus) {
  var out = [];
  var responses = xmlFindAll(multistatus, 'response');
  for (var i = 0; i < responses.length; i++) {
    var data = xmlFindFirst(responses[i], 'calendar-data');
    if (!data) continue;
    var href = xmlFindFirst(responses[i], 'href');
    var etagRaw = xmlFindFirst(responses[i], 'getetag');
    out.push({
      href: href ? xmlUnescape(href).replace(/^\s+|\s+$/g, '') : '',
      etag: etagRaw ? xmlUnescape(etagRaw).replace(/^\s+|\s+$/g, '') : '',
      ics: xmlUnescape(data)
    });
  }
  return out;
}

function caldavFetchEvents(from, to, onDone) {
  var account = caldavAccount('calendar');
  caldavDiscover(account, function (err, collection) {
    if (err) { onDone(err, null); return; }
    var body = '<?xml version="1.0" encoding="utf-8"?>' +
      '<c:calendar-query ' + DAV_NS + '>' +
      '<d:prop><d:getetag/><c:calendar-data/></d:prop>' +
      '<c:filter><c:comp-filter name="VCALENDAR"><c:comp-filter name="VEVENT">' +
      '<c:time-range start="' + toIcalUtc(from) + '" end="' + toIcalUtc(to) + '"/>' +
      '</c:comp-filter></c:comp-filter></c:filter></c:calendar-query>';

    caldavReport(account, collection, body, function (err2, res) {
      if (err2) { onDone(err2, null); return; }
      var entries = calendarDataFrom(res.body);
      var events = [];
      for (var i = 0; i < entries.length; i++) {
        var comps = icalParseComponents(entries[i].ics, 'VEVENT');
        for (var j = 0; j < comps.length; j++) {
          var start = icalParseDate(comps[j], 'DTSTART');
          if (!start) continue;
          var end = icalParseDate(comps[j], 'DTEND');
          events.push({
            uid: icalProp(comps[j], 'UID'),
            summary: icalProp(comps[j], 'SUMMARY') || '(no title)',
            location: icalProp(comps[j], 'LOCATION'),
            start: start.date,
            end: end ? end.date : null,
            all_day: start.allDay,
            href: entries[i].href
          });
        }
      }
      events.sort(function (a, b) { return a.start.getTime() - b.start.getTime(); });
      onDone(null, events);
    });
  });
}

function caldavFetchTodos(includeCompleted, onDone) {
  var account = caldavAccount('reminders');
  caldavDiscover(account, function (err, collection) {
    if (err) { onDone(err, null); return; }
    var filter = includeCompleted
      ? '<c:comp-filter name="VTODO"/>'
      : '<c:comp-filter name="VTODO"><c:prop-filter name="COMPLETED">' +
        '<c:is-not-defined/></c:prop-filter></c:comp-filter>';
    var body = '<?xml version="1.0" encoding="utf-8"?>' +
      '<c:calendar-query ' + DAV_NS + '>' +
      '<d:prop><d:getetag/><c:calendar-data/></d:prop>' +
      '<c:filter><c:comp-filter name="VCALENDAR">' + filter +
      '</c:comp-filter></c:filter></c:calendar-query>';

    caldavReport(account, collection, body, function (err2, res) {
      if (err2) { onDone(err2, null); return; }
      var entries = calendarDataFrom(res.body);
      var todos = [];
      for (var i = 0; i < entries.length; i++) {
        var comps = icalParseComponents(entries[i].ics, 'VTODO');
        for (var j = 0; j < comps.length; j++) {
          var due = icalParseDate(comps[j], 'DUE');
          var status = icalProp(comps[j], 'STATUS');
          todos.push({
            uid: icalProp(comps[j], 'UID'),
            summary: icalProp(comps[j], 'SUMMARY') || '(untitled)',
            notes: icalProp(comps[j], 'DESCRIPTION'),
            due: due ? due.date : null,
            due_all_day: due ? due.allDay : false,
            completed: status === 'COMPLETED' || !!icalProp(comps[j], 'COMPLETED'),
            // Apple marks a location-triggered reminder with a PROXIMITY alarm;
            // the structured location beside it carries the coordinates.
            geofenced: /X-APPLE-(PROXIMITY|STRUCTURED-LOCATION)/i.test(comps[j].raw),
            href: resolveHref(collection, entries[i].href),
            etag: entries[i].etag,
            raw: comps[j].raw
          });
        }
      }
      todos.sort(function (a, b) {
        var at = a.due ? a.due.getTime() : Infinity;
        var bt = b.due ? b.due.getTime() : Infinity;
        return at - bt;
      });
      onDone(null, todos);
    });
  });
}

// --- writes -----------------------------------------------------------------

function caldavPut(account, collection, uid, ics, etag, onDone) {
  var url = collection.replace(/\/?$/, '/') + uid + '.ics';
  var headers = { 'Content-Type': 'text/calendar; charset=utf-8' };
  if (etag) headers['If-Match'] = etag;
  else headers['If-None-Match'] = '*';

  davSend(account, 'PUT', url, ics, headers, function (err, res) {
    if (err) { onDone(err, null); return; }
    if (res.status < 200 || res.status >= 300) {
      onDone(new Error('Could not save to the server (' + res.status + ')'), null);
      return;
    }
    onDone(null, { url: url });
  });
}

function caldavCreateEvent(options, onDone) {
  var account = caldavAccount('calendar');
  caldavDiscover(account, function (err, collection) {
    if (err) { onDone(err, null); return; }
    var uid = 'pa-' + uuid4();
    var now = new Date();
    var lines = ['UID:' + uid, 'DTSTAMP:' + toIcalUtc(now),
                 'SUMMARY:' + icalEscape(options.title)];

    if (options.allDay) {
      var endDay = new Date(options.start.getTime() + 86400000);
      lines.push('DTSTART;VALUE=DATE:' + toIcalDate(options.start));
      lines.push('DTEND;VALUE=DATE:' + toIcalDate(options.end || endDay));
    } else {
      var end = options.end || new Date(options.start.getTime() + 3600000);
      lines.push('DTSTART:' + toIcalUtc(options.start));
      lines.push('DTEND:' + toIcalUtc(end));
    }
    if (options.location) lines.push('LOCATION:' + icalEscape(options.location));
    if (options.notes) lines.push('DESCRIPTION:' + icalEscape(options.notes));
    if (options.alarmMinutes) {
      lines.push('BEGIN:VALARM', 'ACTION:DISPLAY',
                 'DESCRIPTION:' + icalEscape(options.title),
                 'TRIGGER:-PT' + Math.round(options.alarmMinutes) + 'M', 'END:VALARM');
    }
    caldavPut(account, collection, uid, buildIcs('VEVENT', lines), null, function (err2) {
      if (err2) { onDone(err2, null); return; }
      onDone(null, { uid: uid });
    });
  });
}

// A reminder with a place attached carries Apple's location-trigger alarm: a
// VALARM whose TRIGGER is the sentinel date Apple uses for proximity alarms,
// plus the structured location the phone geofences against.
function caldavCreateTodo(options, onDone) {
  var account = caldavAccount('reminders');
  caldavDiscover(account, function (err, collection) {
    if (err) { onDone(err, null); return; }
    var uid = 'pa-' + uuid4();
    var lines = ['UID:' + uid, 'DTSTAMP:' + toIcalUtc(new Date()),
                 'SUMMARY:' + icalEscape(options.title), 'STATUS:NEEDS-ACTION'];
    if (options.notes) lines.push('DESCRIPTION:' + icalEscape(options.notes));
    if (options.due) {
      lines.push(options.dueAllDay
        ? 'DUE;VALUE=DATE:' + toIcalDate(options.due)
        : 'DUE:' + toIcalUtc(options.due));
      if (!options.dueAllDay) {
        lines.push('BEGIN:VALARM', 'ACTION:DISPLAY',
                   'DESCRIPTION:' + icalEscape(options.title),
                   'TRIGGER;VALUE=DATE-TIME:' + toIcalUtc(options.due),
                   'UID:' + uid + '-due', 'END:VALARM');
      }
    }
    if (options.place && isFiniteNumber(options.place.lat) && isFiniteNumber(options.place.lon)) {
      var radius = options.place.radius || 100;
      var proximity = (options.proximity === 'DEPART') ? 'DEPART' : 'ARRIVE';
      lines.push('BEGIN:VALARM', 'ACTION:DISPLAY',
                 'DESCRIPTION:' + icalEscape(options.title),
                 'TRIGGER;VALUE=DATE-TIME:' + APPLE_PROXIMITY_TRIGGER,
                 'X-APPLE-PROXIMITY:' + proximity,
                 'X-APPLE-STRUCTURED-LOCATION;VALUE=URI;X-APPLE-RADIUS=' + radius +
                   ';X-TITLE="' + String(options.place.name).replace(/"/g, '') + '"' +
                   ':geo:' + options.place.lat + ',' + options.place.lon,
                 'UID:' + uid + '-geo', 'END:VALARM');
    }
    caldavPut(account, collection, uid, buildIcs('VTODO', lines), null, function (err2) {
      if (err2) { onDone(err2, null); return; }
      onDone(null, { uid: uid });
    });
  });
}

function caldavCompleteTodo(todo, onDone) {
  var account = caldavAccount('reminders');
  var lines = String(todo.raw).split('\r\n');
  var rebuilt = [];
  for (var i = 0; i < lines.length; i++) {
    var line = lines[i];
    if (/^(STATUS|COMPLETED|PERCENT-COMPLETE):/i.test(line)) continue;
    if (line === 'BEGIN:VTODO' || line === 'END:VTODO') continue;
    rebuilt.push(line);
  }
  rebuilt.push('STATUS:COMPLETED');
  rebuilt.push('PERCENT-COMPLETE:100');
  rebuilt.push('COMPLETED:' + toIcalUtc(new Date()));

  var ics = buildIcs('VTODO', rebuilt);
  davSend(account, 'PUT', todo.href, ics, {
    'Content-Type': 'text/calendar; charset=utf-8',
    'If-Match': todo.etag || '*'
  }, function (err, res) {
    if (err) { onDone(err, null); return; }
    if (res.status < 200 || res.status >= 300) {
      onDone(new Error('Could not update the reminder (' + res.status + ')'), null);
      return;
    }
    onDone(null, true);
  });
}
