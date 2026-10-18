/* Default backend selection. GPL-2.0-or-later. */
#include "emu_engine.h"
#include "emu_session.h"
#include "../engines/regba/regba_adapter.h"

int emu_engine_start(void)
{
    return emu_session_start(regba_adapter_backend());
}
