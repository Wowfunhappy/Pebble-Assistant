/* eslint-env browser */
/* global Pebble */
//
// Shared helpers.  Everything in this file is ES5 on purpose: the Core Devices
// iOS companion runs PebbleKit JS through a JavaScriptCore context whose exact
// language level has changed between releases, and ES5 is the level that has
// never broken.
//

var APP_VERSION = '1.0.0';
var LOG_PREFIX = '[assistant] ';

function log(msg) {
  try { console.log(LOG_PREFIX + msg); } catch (e) { /* no console on some hosts */ }
}

function logErr(where, err) {
  var detail = '';
  try { detail = (err && (err.stack || err.message)) || String(err); } catch (e) { detail = '?'; }
  log('ERROR ' + where + ': ' + detail);
}

function clamp(value, low, high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

function isFiniteNumber(n) {
  return typeof n === 'number' && isFinite(n);
}

function safeParse(text, fallback) {
  if (typeof text !== 'string' || !text) return fallback;
  try { return JSON.parse(text); } catch (e) { return fallback; }
}

function storeGet(key, fallback) {
  try {
    var raw = localStorage.getItem(key);
    if (raw === null || typeof raw === 'undefined') return fallback;
    return safeParse(raw, fallback);
  } catch (e) {
    return fallback;
  }
}

function storeSet(key, value) {
  try {
    localStorage.setItem(key, JSON.stringify(value));
    return true;
  } catch (e) {
    logErr('storeSet ' + key, e);
    return false;
  }
}

function storeDel(key) {
  try { localStorage.removeItem(key); } catch (e) { /* ignore */ }
}

function uuid4() {
  var out = '';
  var hex = '0123456789abcdef';
  for (var i = 0; i < 36; i++) {
    if (i === 8 || i === 13 || i === 18 || i === 23) { out += '-'; continue; }
    if (i === 14) { out += '4'; continue; }
    var r = Math.floor(Math.random() * 16);
    if (i === 19) r = (r & 0x3) | 0x8;
    out += hex.charAt(r);
  }
  return out;
}

// --- text -------------------------------------------------------------------

function utf8Length(str) {
  var bytes = 0;
  for (var i = 0; i < str.length; i++) {
    var code = str.charCodeAt(i);
    if (code < 0x80) bytes += 1;
    else if (code < 0x800) bytes += 2;
    else if (code >= 0xd800 && code <= 0xdbff) { bytes += 4; i++; }
    else bytes += 3;
  }
  return bytes;
}

// Split on UTF-8 byte boundaries so a multi-byte character is never cut in half
// on the way to the watch.
function splitUtf8(str, maxBytes) {
  var chunks = [];
  var start = 0;
  var bytes = 0;
  for (var i = 0; i < str.length; i++) {
    var code = str.charCodeAt(i);
    var size;
    var surrogate = false;
    if (code < 0x80) size = 1;
    else if (code < 0x800) size = 2;
    else if (code >= 0xd800 && code <= 0xdbff) { size = 4; surrogate = true; }
    else size = 3;

    if (bytes + size > maxBytes) {
      chunks.push(str.substring(start, i));
      start = i;
      bytes = 0;
    }
    bytes += size;
    if (surrogate) i++;
  }
  if (start < str.length) chunks.push(str.substring(start));
  return chunks;
}

function truncateBytes(str, maxBytes) {
  if (utf8Length(str) <= maxBytes) return str;
  var chunks = splitUtf8(str, maxBytes);
  return chunks.length ? chunks[0] : '';
}

function trimText(str, maxChars) {
  if (typeof str !== 'string') return '';
  str = str.replace(/\s+$/, '');
  if (str.length <= maxChars) return str;
  return str.substring(0, maxChars - 1) + '…';
}

// Strip the formatting a chat model reaches for by default.  A 200px screen has
// no use for markdown tables, and headings just waste a line.
function plainify(text) {
  if (typeof text !== 'string') return '';
  var out = text;
  out = out.replace(/```[a-zA-Z0-9_+-]*\n?/g, '');
  out = out.replace(/`([^`]+)`/g, '$1');
  out = out.replace(/^\s{0,3}#{1,6}\s*/gm, '');
  out = out.replace(/\*\*([^*]+)\*\*/g, '$1');
  out = out.replace(/(^|[^*])\*([^*\n]+)\*/g, '$1$2');
  out = out.replace(/^\s*[-*+]\s+/gm, '• ');
  out = out.replace(/\[([^\]]+)\]\((?:[^)]*)\)/g, '$1');
  out = out.replace(/^\s*\|.*\|\s*$/gm, '');
  out = out.replace(/\n{3,}/g, '\n\n');
  return out.replace(/^\s+|\s+$/g, '');
}

function titleCaseFirst(str) {
  if (!str) return '';
  return str.charAt(0).toUpperCase() + str.substring(1);
}

// --- base64 (btoa/atob are not dependable in PebbleKit JS) ------------------

var B64_CHARS = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function bytesToBase64(bytes, urlSafe) {
  var out = '';
  var i;
  for (i = 0; i + 2 < bytes.length; i += 3) {
    var n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
    out += B64_CHARS.charAt((n >> 18) & 63) + B64_CHARS.charAt((n >> 12) & 63) +
           B64_CHARS.charAt((n >> 6) & 63) + B64_CHARS.charAt(n & 63);
  }
  var rest = bytes.length - i;
  if (rest === 1) {
    var a = bytes[i] << 16;
    out += B64_CHARS.charAt((a >> 18) & 63) + B64_CHARS.charAt((a >> 12) & 63) + '==';
  } else if (rest === 2) {
    var b = (bytes[i] << 16) | (bytes[i + 1] << 8);
    out += B64_CHARS.charAt((b >> 18) & 63) + B64_CHARS.charAt((b >> 12) & 63) +
           B64_CHARS.charAt((b >> 6) & 63) + '=';
  }
  if (urlSafe) out = out.replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
  return out;
}

function stringToUtf8Bytes(str) {
  var bytes = [];
  for (var i = 0; i < str.length; i++) {
    var c = str.charCodeAt(i);
    if (c < 0x80) {
      bytes.push(c);
    } else if (c < 0x800) {
      bytes.push(0xc0 | (c >> 6), 0x80 | (c & 63));
    } else if (c >= 0xd800 && c <= 0xdbff && i + 1 < str.length) {
      var lo = str.charCodeAt(++i);
      var cp = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
      bytes.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 63),
                 0x80 | ((cp >> 6) & 63), 0x80 | (cp & 63));
    } else {
      bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
    }
  }
  return bytes;
}

function base64Encode(str, urlSafe) {
  return bytesToBase64(stringToUtf8Bytes(str), urlSafe);
}

function base64DecodeToString(b64) {
  var clean = String(b64).replace(/-/g, '+').replace(/_/g, '/').replace(/[^A-Za-z0-9+/=]/g, '');
  var bytes = [];
  for (var i = 0; i < clean.length; i += 4) {
    var n = 0, count = 0;
    for (var j = 0; j < 4; j++) {
      var ch = clean.charAt(i + j);
      if (!ch || ch === '=') { n = n << 6; continue; }
      n = (n << 6) | B64_CHARS.indexOf(ch);
      count++;
    }
    if (count > 1) bytes.push((n >> 16) & 255);
    if (count > 2) bytes.push((n >> 8) & 255);
    if (count > 3) bytes.push(n & 255);
  }
  // Decode UTF-8.
  var out = '';
  for (var k = 0; k < bytes.length; k++) {
    var b = bytes[k];
    if (b < 0x80) {
      out += String.fromCharCode(b);
    } else if (b >= 0xc0 && b < 0xe0) {
      out += String.fromCharCode(((b & 31) << 6) | (bytes[++k] & 63));
    } else if (b >= 0xe0 && b < 0xf0) {
      out += String.fromCharCode(((b & 15) << 12) | ((bytes[++k] & 63) << 6) | (bytes[++k] & 63));
    } else {
      var cp = ((b & 7) << 18) | ((bytes[++k] & 63) << 12) |
               ((bytes[++k] & 63) << 6) | (bytes[++k] & 63);
      cp -= 0x10000;
      out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 1023));
    }
  }
  return out;
}

function decodeJwtPayload(token) {
  if (typeof token !== 'string') return null;
  var parts = token.split('.');
  if (parts.length < 2) return null;
  return safeParse(base64DecodeToString(parts[1]), null);
}

// --- time -------------------------------------------------------------------

function pad2(n) { return (n < 10 ? '0' : '') + n; }

function tzOffsetString(date) {
  var mins = -date.getTimezoneOffset();
  var sign = mins >= 0 ? '+' : '-';
  mins = Math.abs(mins);
  return sign + pad2(Math.floor(mins / 60)) + ':' + pad2(mins % 60);
}

function toIcalUtc(date) {
  return date.getUTCFullYear() + pad2(date.getUTCMonth() + 1) + pad2(date.getUTCDate()) + 'T' +
         pad2(date.getUTCHours()) + pad2(date.getUTCMinutes()) + pad2(date.getUTCSeconds()) + 'Z';
}

function toIcalDate(date) {
  return date.getFullYear() + pad2(date.getMonth() + 1) + pad2(date.getDate());
}

function toLocalIso(date) {
  return date.getFullYear() + '-' + pad2(date.getMonth() + 1) + '-' + pad2(date.getDate()) +
         'T' + pad2(date.getHours()) + ':' + pad2(date.getMinutes()) + ':' +
         pad2(date.getSeconds()) + tzOffsetString(date);
}

var WEEKDAYS = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
var MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun',
              'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

function friendlyTime(date) {
  var h = date.getHours();
  var suffix = h >= 12 ? 'pm' : 'am';
  var hour = h % 12;
  if (hour === 0) hour = 12;
  return hour + ':' + pad2(date.getMinutes()) + suffix;
}

function friendlyDate(date) {
  var now = new Date();
  var sameDay = date.getFullYear() === now.getFullYear() &&
                date.getMonth() === now.getMonth() &&
                date.getDate() === now.getDate();
  if (sameDay) return 'today';
  var tomorrow = new Date(now.getTime() + 86400000);
  if (date.getFullYear() === tomorrow.getFullYear() &&
      date.getMonth() === tomorrow.getMonth() &&
      date.getDate() === tomorrow.getDate()) return 'tomorrow';
  return WEEKDAYS[date.getDay()] + ' ' + MONTHS[date.getMonth()] + ' ' + date.getDate();
}

// Parses the date shapes a model actually emits: ISO with or without a zone,
// plain dates, and "YYYY-MM-DD HH:MM".
function parseFlexibleDate(value) {
  if (!value) return null;
  if (value instanceof Date) return isNaN(value.getTime()) ? null : value;
  var str = String(value).replace(/^\s+|\s+$/g, '');
  if (/^\d{4}-\d{2}-\d{2}$/.test(str)) {
    var d = str.split('-');
    return new Date(parseInt(d[0], 10), parseInt(d[1], 10) - 1, parseInt(d[2], 10), 0, 0, 0, 0);
  }
  var normalized = str.replace(' ', 'T');
  if (/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(:\d{2})?$/.test(normalized)) {
    var parts = normalized.split('T');
    var ymd = parts[0].split('-');
    var hms = parts[1].split(':');
    return new Date(parseInt(ymd[0], 10), parseInt(ymd[1], 10) - 1, parseInt(ymd[2], 10),
                    parseInt(hms[0], 10), parseInt(hms[1], 10),
                    hms.length > 2 ? parseInt(hms[2], 10) : 0, 0);
  }
  var parsed = new Date(normalized);
  return isNaN(parsed.getTime()) ? null : parsed;
}

// --- HTTP -------------------------------------------------------------------

function httpRequest(options, onDone) {
  var method = options.method || 'GET';
  var xhr = new XMLHttpRequest();
  var finished = false;

  function finish(err, res) {
    if (finished) return;
    finished = true;
    try { onDone(err, res); } catch (e) { logErr('httpRequest callback', e); }
  }

  try {
    xhr.open(method, options.url, true);
  } catch (e) {
    finish(new Error('Cannot open ' + method + ' request'), null);
    return null;
  }

  xhr.timeout = options.timeout || 60000;
  if (options.headers) {
    for (var name in options.headers) {
      if (!Object.prototype.hasOwnProperty.call(options.headers, name)) continue;
      try { xhr.setRequestHeader(name, options.headers[name]); } catch (e2) { /* ignore */ }
    }
  }

  xhr.onload = function () {
    finish(null, { status: xhr.status, body: xhr.responseText || '', xhr: xhr });
  };
  xhr.onerror = function () {
    finish(new Error('Network error'), { status: xhr.status || 0, body: xhr.responseText || '' });
  };
  xhr.ontimeout = function () { finish(new Error('Timed out'), null); };
  if (options.onProgress) {
    xhr.onprogress = function () {
      try { options.onProgress(xhr.responseText || ''); } catch (e3) { /* ignore */ }
    };
  }

  try {
    xhr.send(typeof options.body === 'undefined' ? null : options.body);
  } catch (e4) {
    finish(e4, null);
  }
  return xhr;
}
