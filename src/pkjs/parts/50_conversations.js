//
// Conversation storage.
//
// Everything lives on the phone; the watch holds one turn at a time.  Within a
// running conversation the rich response items (reasoning, tool calls) are kept
// in memory so the model keeps its chain of thought across tool hops.  Only the
// plain question/answer text is persisted, so reopening an old chat rebuilds a
// clean transcript rather than restoring stale encrypted reasoning blobs.
//

var CHATS_KEY = 'chats_v1';
var MAX_CHATS = 20;
var MAX_TURNS_PER_CHAT = 20;

var _liveInput = null;      // rich items for the active chat, this session only
var _liveChatId = '';
var _liveSessionId = '';

function chatStore() {
  var store = storeGet(CHATS_KEY, null);
  if (!store || !store.chats) store = { chats: [], active_id: '' };
  return store;
}

function saveChatStore(store) {
  while (store.chats.length > MAX_CHATS) store.chats.pop();
  storeSet(CHATS_KEY, store);
}

function makeTitle(question) {
  var text = String(question || '').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
  text = text.replace(/^(hey|hi|ok|okay|please|can you|could you|would you)\s+/i, '');
  text = text.replace(/[?.!,]+$/, '');
  if (!text) return 'New chat';
  return titleCaseFirst(trimText(text, 38));
}

function newChat() {
  var store = chatStore();
  var chat = {
    id: uuid4(),
    title: 'New chat',
    created: Date.now(),
    updated: Date.now(),
    turns: []
  };
  store.chats.unshift(chat);
  store.active_id = chat.id;
  saveChatStore(store);
  _liveInput = [];
  _liveChatId = chat.id;
  _liveSessionId = uuid4();
  return chat;
}

function findChat(store, id) {
  for (var i = 0; i < store.chats.length; i++) {
    if (store.chats[i].id === id) return store.chats[i];
  }
  return null;
}

function activeChat() {
  var store = chatStore();
  var chat = findChat(store, store.active_id);
  if (chat) return chat;
  if (store.chats.length) {
    store.active_id = store.chats[0].id;
    saveChatStore(store);
    return store.chats[0];
  }
  return newChat();
}

// Chats that have never produced a turn are not worth listing or keeping.
function pruneEmptyChats() {
  var store = chatStore();
  var kept = [];
  for (var i = 0; i < store.chats.length; i++) {
    if (store.chats[i].turns.length || store.chats[i].id === store.active_id) {
      kept.push(store.chats[i]);
    }
  }
  store.chats = kept;
  saveChatStore(store);
}

function chatList() {
  pruneEmptyChats();
  var store = chatStore();
  var out = [];
  for (var i = 0; i < store.chats.length; i++) {
    var chat = store.chats[i];
    if (!chat.turns.length) continue;
    out.push({
      index: i,
      id: chat.id,
      title: chat.title,
      turns: chat.turns.length,
      updated: chat.updated,
      active: chat.id === store.active_id
    });
  }
  return out;
}

function openChatByIndex(index) {
  var store = chatStore();
  if (index < 0 || index >= store.chats.length) return null;
  store.active_id = store.chats[index].id;
  saveChatStore(store);
  // Switching chats abandons the in-memory reasoning context; the transcript is
  // rebuilt from stored text on the next question.
  _liveInput = null;
  _liveChatId = store.chats[index].id;
  _liveSessionId = uuid4();
  return store.chats[index];
}

function appendTurn(question, answer) {
  var store = chatStore();
  var chat = findChat(store, store.active_id) || null;
  if (!chat) {
    chat = { id: uuid4(), title: 'New chat', created: Date.now(), updated: Date.now(), turns: [] };
    store.chats.unshift(chat);
    store.active_id = chat.id;
  }
  if (!chat.turns.length) chat.title = makeTitle(question);
  chat.turns.push({ q: question, a: answer, at: Date.now() });
  while (chat.turns.length > MAX_TURNS_PER_CHAT) chat.turns.shift();
  chat.updated = Date.now();

  // Most recently used first, so the chats list reads like a history.
  for (var i = 0; i < store.chats.length; i++) {
    if (store.chats[i].id === chat.id) { store.chats.splice(i, 1); break; }
  }
  store.chats.unshift(chat);
  saveChatStore(store);
  return chat;
}

// Every launch of the watch app starts a clean conversation.  Picking up a
// half-finished chat from yesterday is never what someone means when they raise
// their wrist and start talking; the old chat is still one press away in the
// list.  An already-empty chat is reused so relaunching does not churn the
// store.
function startFreshConversation() {
  var store = chatStore();
  var current = findChat(store, store.active_id);
  if (current && !current.turns.length) {
    _liveInput = [];
    _liveChatId = current.id;
    _liveSessionId = uuid4();
    return current;
  }
  return newChat();
}

function turnAt(chat, index) {
  if (!chat || !chat.turns.length) return null;
  var count = chat.turns.length;
  var i = clamp(index, 0, count - 1);
  var turn = chat.turns[i];
  return {
    index: i,
    count: count,
    title: chat.title,
    question: turn.q,
    answer: turn.a,
    is_live: (i === count - 1)
  };
}

// The model input for the active chat: the live rich item list when we have one,
// otherwise a plain transcript rebuilt from storage.
function conversationInput() {
  var chat = activeChat();
  if (_liveInput && _liveChatId === chat.id) return _liveInput.slice();
  var input = [];
  for (var i = 0; i < chat.turns.length; i++) {
    input.push(userMessage(chat.turns[i].q));
    if (chat.turns[i].a) input.push(assistantMessage(chat.turns[i].a));
  }
  _liveChatId = chat.id;
  if (!_liveSessionId) _liveSessionId = uuid4();
  return input;
}

function rememberLiveInput(input) {
  _liveInput = input;
  _liveChatId = activeChat().id;
}

function conversationSessionId() {
  if (!_liveSessionId) _liveSessionId = uuid4();
  return _liveSessionId;
}
