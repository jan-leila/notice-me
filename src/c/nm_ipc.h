// What the app and its background worker share: persistent storage keys and worker message types
// (CONTRACTS.md §5 and §6).
#pragma once

#define NM_KEY_SETTINGS 1      // NmSettings
#define NM_KEY_SESSION 2       // NmSession
#define NM_KEY_NUDGE 3         // int: 1 while a nudge from the worker has not been shown
#define NM_KEY_TASK_COUNT 9    // int
#define NM_KEY_TASK0 10        // NmTask per key, NM_KEY_TASK0 + index

#define NM_MSG_RELOAD 1        // app -> worker: session or settings changed, read them again
#define NM_MSG_ACK 2           // app -> worker: the wearer answered a nudge; restart the stillness clock
#define NM_MSG_NUDGE 3         // worker -> app: sat still too long
