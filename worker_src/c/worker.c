// Notice Me — background worker: while an active (not sedentary) task is in a focus phase it watches the
// accelerometer, and when the wearer has sat still for too long it brings the app up to nudge them. The app starts
// the worker for such a session and kills it afterwards (CONTRACTS.md §5).
#include <pebble_worker.h>
#include "../../src/c/nm_ipc.h"
#include "../../src/c/nm_motion.h"
#include "../../src/c/nm_session.h"

#define BATCH 25  // samples per update: 2.5 s at 10 Hz

static NmSession s_session;
static NmSettings s_settings;
static NmMotion s_motion;
static bool s_listening;

static void nudge(void) {
  persist_write_int(NM_KEY_NUDGE, 1);
  AppWorkerMessage msg = { 0, 0, 0 };
  app_worker_send_message(NM_MSG_NUDGE, &msg);  // the app is open
  worker_launch_app();                          // or it is not
}

static void accel_handler(AccelData *data, uint32_t num_samples) {
  int32_t now = (int32_t)time(NULL);
  // Past the end of the focus phase the app (woken by its wakeup) sends NM_MSG_RELOAD; until then just wait.
  if (!nm_session_watching(&s_session, now)) return;
  int16_t xyz[BATCH * 3];
  int n = 0;
  for (uint32_t i = 0; i < num_samples && n < BATCH; i++) {
    if (data[i].did_vibrate) continue;  // the watch shaking itself is not the wearer moving
    xyz[3 * n] = data[i].x;
    xyz[3 * n + 1] = data[i].y;
    xyz[3 * n + 2] = data[i].z;
    n++;
  }
  nm_motion_feed(&s_motion, xyz, n, nm_motion_threshold(s_settings.sensitivity), now);
  if (nm_motion_nudge_due(&s_motion, now, (int32_t)s_settings.nudge_min * 60)) nudge();
}

static void reload(void) {
  nm_settings_defaults(&s_settings);
  persist_read_data(NM_KEY_SETTINGS, &s_settings, sizeof s_settings);
  if (persist_read_data(NM_KEY_SESSION, &s_session, sizeof s_session) != (int)sizeof s_session) nm_session_stop(&s_session);
  int32_t now = (int32_t)time(NULL);
  bool want = nm_session_watching(&s_session, now);
  if (want && !s_listening) {
    nm_motion_reset(&s_motion, now);
    accel_data_service_subscribe(BATCH, accel_handler);
    accel_service_set_sampling_rate(ACCEL_SAMPLING_10HZ);
  } else if (!want && s_listening) {
    accel_data_service_unsubscribe();
  }
  s_listening = want;
}

static void message_handler(uint16_t type, AppWorkerMessage *data) {
  if (type == NM_MSG_RELOAD) reload();
  else if (type == NM_MSG_ACK) nm_motion_reset(&s_motion, (int32_t)time(NULL));
}

int main(void) {
  app_worker_message_subscribe(message_handler);
  reload();
  worker_event_loop();
  if (s_listening) accel_data_service_unsubscribe();
  app_worker_message_unsubscribe();
}
