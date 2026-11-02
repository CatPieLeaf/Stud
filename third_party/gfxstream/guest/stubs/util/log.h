// Mesa's log calls, for the guest ring stream taken from Mesa: gfxstream's
// own log.
#pragma once
#include "gfxstream/common/logging.h"
#define mesa_loge(...) GFXSTREAM_ERROR(__VA_ARGS__)
#define mesa_logw(...) GFXSTREAM_WARNING(__VA_ARGS__)
#define mesa_logi(...) GFXSTREAM_DEBUG(__VA_ARGS__)
#define mesa_logd(...) GFXSTREAM_DEBUG(__VA_ARGS__)
