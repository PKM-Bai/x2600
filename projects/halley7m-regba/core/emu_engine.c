/* Default backend selection. GPL-2.0-or-later. */
#include "emu_engine.h"
#include "emu_session.h"
#ifdef EMU_ENGINE_MGBA
#include "../engines/mgba/mgba_adapter.h"
#else
#include "../engines/regba/regba_adapter.h"
#endif

int emu_engine_start(void)
{
#ifdef EMU_ENGINE_MGBA
    return emu_session_start(mgba_adapter_backend());
#else
    return emu_session_start(regba_adapter_backend());
#endif
}
