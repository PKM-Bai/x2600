/* Owner task for asynchronous emulator commands. GPL-2.0-or-later. */
#ifndef X2600_EMU_SESSION_H
#define X2600_EMU_SESSION_H
#include "emu_backend.h"
/* The backend and its operation table outlive the session task. */
int emu_session_start(const emu_backend_ops_t *backend);
#endif
