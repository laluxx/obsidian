#include "resize_policy.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    assert(!obs_resize_rebuild_due(false, false, 1.0));
    assert(!obs_resize_rebuild_due(false, true, 0.0));
    assert(!obs_resize_rebuild_due(false, true, 0.099));
    assert(obs_resize_rebuild_due(false, true, OBS_RESIZE_SETTLE_SECONDS));
    assert(obs_resize_rebuild_due(false, true, 0.5));
    assert(obs_resize_rebuild_due(true, false, 0.0));
    assert(obs_resize_rebuild_due(true, true, 0.0));
    bool out_of_date = true;
    bool resize_pending = true;
    obs_resize_reset_after_rebuild(&out_of_date, &resize_pending);
    assert(!out_of_date && !resize_pending);
    assert(!obs_resize_rebuild_due(out_of_date, resize_pending, 1.0));
    assert(obs_resize_framebuffer_is_empty(0, 600));
    assert(obs_resize_framebuffer_is_empty(800, 0));
    assert(obs_resize_framebuffer_is_empty(-1, 600));
    assert(!obs_resize_framebuffer_is_empty(1, 1));
    assert(!obs_resize_framebuffer_is_empty(800, 600));

    puts("resize policy: live-frame debounce, settled rebuild, out-of-date priority PASS");
    return 0;
}
