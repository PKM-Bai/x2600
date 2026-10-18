/* ReGBA implementation of the synchronous backend. GPL-2.0-or-later. */
#ifndef X2600_REGBA_ADAPTER_H
#define X2600_REGBA_ADAPTER_H

#include "emu_backend.h"

/* The current ReGBA core is a singleton. Its global state stays private to
 * this implementation; the session uses only this operation table. */
const emu_backend_ops_t *regba_adapter_backend(void);

#endif
