#include "assistant.h"

// ---------------------------------------------------------------------------
// Dictation.
//
// The app opens straight into the microphone -- that is the whole interaction.
// Pebble's dictation UI is a system modal that owns all four buttons while it is
// up, so the only gesture available inside it is BACK, and backing out is
// deliberately wired to reveal the chats list rather than quitting.
// ---------------------------------------------------------------------------

#define DICTATION_BUF 512

#ifdef PBL_MICROPHONE

static DictationSession *s_session;
static bool s_active;
static uint8_t s_abort_retries;

bool dictation_active(void) { return s_active; }

static void show_chats(void) {
  list_pop_submenus();
  reply_window_hide();
}

static void dictation_cb(DictationSession *session, DictationSessionStatus status,
                         char *transcription, void *context) {
  s_active = false;
  if (s_session) {
    dictation_session_destroy(s_session);
    s_session = NULL;
  }

  switch (status) {
    case DictationSessionStatusSuccess:
      s_abort_retries = 0;
      if (transcription && transcription[0]) {
        list_pop_submenus();
        reply_window_set_question(transcription);
        reply_window_set_status("Thinking", SPIN_THINKING);
        comm_queue_question(transcription);
      } else {
        show_chats();
      }
      break;

    case DictationSessionStatusFailureSystemAborted:
      // A notification or an incoming call stole the microphone.  One silent
      // retry, then give up rather than fighting the user.
      if (s_abort_retries < 1) {
        s_abort_retries++;
        dictation_start();
      } else {
        s_abort_retries = 0;
        show_chats();
      }
      break;

    case DictationSessionStatusFailureTranscriptionRejected:
    case DictationSessionStatusFailureTranscriptionRejectedWithError:
    case DictationSessionStatusFailureNoSpeechDetected:
      s_abort_retries = 0;
      show_chats();
      break;

    case DictationSessionStatusFailureConnectivityError:
      reply_window_set_error("Voice needs the phone connected");
      break;

    case DictationSessionStatusFailureDisabled:
      reply_window_set_error("Dictation is turned off in the Pebble app");
      break;

    case DictationSessionStatusFailureInternalError:
    case DictationSessionStatusFailureRecognizerError:
    default:
      reply_window_set_error("Could not hear that");
      break;
  }
}

void dictation_start(void) {
  if (s_active) return;
  if (!s_session) {
    s_session = dictation_session_create(DICTATION_BUF, dictation_cb, NULL);
    if (!s_session) {
      reply_window_set_error("Voice unavailable");
      return;
    }
  }
  // Confirmation is off by default: on a wrist assistant the extra screen costs
  // more than the occasional misheard word.  Settings can turn it back on.
  dictation_session_enable_confirmation(s_session,
      (comm_flags() & SFLAG_CONFIRM_DICT) != 0);
  dictation_session_enable_error_dialogs(s_session, false);
  s_active = true;
  dictation_session_start(s_session);
}

void dictation_cleanup(void) {
  if (s_session) {
    dictation_session_destroy(s_session);
    s_session = NULL;
  }
  s_active = false;
}

#else  // !PBL_MICROPHONE

bool dictation_active(void) { return false; }

void dictation_start(void) {
  reply_window_set_error("This watch has no microphone");
}

void dictation_cleanup(void) {}

#endif
