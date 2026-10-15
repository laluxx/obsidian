/* STRICT real Monad + Vulkan acceptance of the actual editor SDF gesture
 * core. World rays mirror the GLFW gizmo's camera-plane unprojection; a
 * separate interactive validation must still verify screen pixel mapping. */
#include "obsidian_agent.h"
#include "sdf_viewport.h"
#include "sdf_property_bridge.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* An image of the correct byte count but one uniform color is not
 * evidence that the production Vulkan renderer drew the Monad scene. */
static bool captured_scene_pixels(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) != 0 || st.st_size != 640 * 360 * 4) return false;
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char first[4], pixel[4];
    if (fread(first, 1, 4, file) != 4) { fclose(file); return false; }
    unsigned varied = 0;
    while (fread(pixel, 1, 4, file) == 4)
        if (memcmp(first, pixel, 4) != 0) ++varied;
    bool pass = !ferror(file) && varied >= 64;
    fclose(file);
    return pass;
}
static bool start(double x) {
    const double origin[3] = {x, 0, -3}, direction[3] = {0, 0, 1};
    return obs_sdf_viewport_press(origin, direction, direction, 20.0);
}
static bool drag(double x) {
    const double origin[3] = {x, 0, -3}, direction[3] = {0, 0, 1};
    return obs_sdf_viewport_motion(origin, direction) &&
           obs_sdf_viewport_error() == OBS_SDF_EDIT_OK;
}
static bool position(ObsAgentEngine *engine, ObsAgentSdfRef ref,
                     double expected, ObsAgentError *error) {
    ObsAgentSphere value = {0};
    return obs_agent_sphere_query(engine, ref, &value, error) == OBS_AGENT_OK &&
           fabs(value.position[0] - expected) < 0.002;
}
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s sculpture.mon output.rgba receipt.json\n", argv[0]);
        return 2;
    }
    ObsAgentConfig cfg = {.abi_version = OBS_AGENT_ABI_VERSION,
                          .width = 640, .height = 360};
    ObsAgentError error = {0};
    ObsAgentEngine *engine = obs_agent_open(&cfg, &error);
    if (!engine) goto fail;
    if (obs_agent_load_file(engine, argv[1], &error) != OBS_AGENT_OK) goto close_fail;
    for (int i = 0; i < 4; ++i)
        if (obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK) goto close_fail;
    ObsAgentSphere sphere = {0};
    if (obs_agent_scene_sphere_at(engine, 1, &sphere, &error) != OBS_AGENT_OK ||
        fabs(sphere.position[0] + 1.6) > 0.002) goto close_fail;
    ObsAgentRayHit pick = {0};
    const double from[3] = {-1.6,0,-3}, toward[3] = {0,0,1};
    if (obs_agent_scene_pick_ray(engine, from, toward, 20, &pick, &error) != OBS_AGENT_OK ||
        pick.index != 1) goto close_fail;
    ObsAgentSdfRef ref = pick.ref;
    if (!obs_sdf_viewport_set_enabled(true) || !start(-1.6) ||
        obs_sdf_viewport_drag_ticket() != ref.ticket || !drag(-1.0) ||
        !position(engine, ref, -1.0, &error) ||
        obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK ||
        !obs_sdf_viewport_cancel() ||
        !position(engine, ref, -1.6, &error) ||
        obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK ||
        !start(-1.6) || !drag(-1.2) || !obs_sdf_viewport_release() ||
        !position(engine, ref, -1.2, &error) ||
        obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK ||
        obs_agent_scene_undo(engine, &error) != OBS_AGENT_OK ||
        !position(engine, ref, -1.6, &error) ||
        obs_agent_scene_redo(engine, &error) != OBS_AGENT_OK ||
        !position(engine, ref, -1.2, &error) ||
        obs_agent_capture(engine, argv[2], argv[3], NULL, NULL, &error) != OBS_AGENT_OK ||
        !captured_scene_pixels(argv[2]))
        goto close_fail;
    /* Cycle 57: use the visible sidebar's production bridge on a sphere
     * created by compiled Monad. It must share viewport/agent undo history. */
    ObsAgentSphere before_sidebar = {0};
    if (obs_agent_sphere_query(engine, ref, &before_sidebar, &error) != OBS_AGENT_OK)
        goto close_fail;
    ObsSdfEditValues sidebar = {.roughness = .35, .metallic = .65};
    sidebar.xyz[0] = -0.8;
    sidebar.xyz[1] = before_sidebar.position[1];
    sidebar.xyz[2] = before_sidebar.position[2];
    for (unsigned i = 0; i < 4; ++i) sidebar.rgba[i] = before_sidebar.rgba[i];
    sidebar.rgba[0] = 0.9;
    if (obs_sdf_property_apply(pick.index,
            OBS_SDF_EDIT_POSITION | OBS_SDF_EDIT_MATERIAL, &sidebar) != OBS_SDF_EDIT_OK ||
        !position(engine, ref, -0.8, &error) ||
        obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK ||
        obs_agent_scene_undo(engine, &error) != OBS_AGENT_OK ||
        !position(engine, ref, -1.2, &error) ||
        obs_agent_scene_redo(engine, &error) != OBS_AGENT_OK ||
        !position(engine, ref, -0.8, &error) ||
        obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK)
        goto close_fail;
    if (!obs_sdf_viewport_set_enabled(false)) goto close_fail;
    if (obs_agent_close(engine, &error) != OBS_AGENT_OK) goto fail;
    puts("Cycle 55 Vulkan + real Monad scene -> viewport preview/cancel/commit/undo PASS");
    return 0;
close_fail:
    (void)obs_sdf_viewport_set_enabled(false);
    (void)obs_agent_close(engine, NULL);
fail:
    fprintf(stderr, "Cycle 55 strict driver integration FAILED (%d): %s\n",
            error.code, error.message);
    return 1;
}
