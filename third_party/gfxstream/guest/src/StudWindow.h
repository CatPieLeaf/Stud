// Stud's side of gfxstream's window surfaces: an ANativeWindow's buffers are
// host colour buffers, and a queued buffer is a finished frame.
#pragma once

#include <cstdint>

extern "C" {
// Called with each frame's colour buffer handle, its size, and the native
// window it was drawn for, when eglSwapBuffers queues it. Stud's presenter
// hands it to render-host; null until one is set.
typedef void (*StudFramePresenter)(void* window, uint32_t color_buffer, int width, int height);
void stud_gfxstream_set_presenter(StudFramePresenter presenter);
}
