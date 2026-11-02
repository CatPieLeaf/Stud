// The guest end of gfxstream's ring transport, on memory render-host shares.
#pragma once

#include "gfxstream/guest/IOStream.h"

// A render stream on the ring transport, or null where render-host offers
// none (the caller then uses StudStream).
gfxstream::guest::IOStream* createStudAsgStream();
