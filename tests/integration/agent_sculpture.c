/// Standalone external client: no private engine or Vulkan includes.
#include "obsidian_agent.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
/* Weak but useful visible-output gate: a successfully sized, uniform raw
 * buffer is not evidence of a rendered scene. This does not substitute for
 * a GPU golden-image or per-object shader correctness test. */
static int has_nonuniform_pixels(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char first[4], pixel[4];
    if (fread(first, 1, sizeof(first), file) != sizeof(first)) {
        fclose(file);
        return 0;
    }
    unsigned varied = 0;
    while (fread(pixel, 1, sizeof(pixel), file) == sizeof(pixel)) {
        if (memcmp(first, pixel, sizeof(pixel)) != 0 && ++varied >= 64) break;
    }
    const int success = varied >= 64 && !ferror(file);
    fclose(file);
    return success;
}
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s sculpture.mon output.rgba receipt.json\n", argv[0]);
        return 2;
    }
    ObsAgentConfig cfg = {.abi_version = OBS_AGENT_ABI_VERSION,
                          .width = 640, .height = 360, .call_camera_policy = false};
    ObsAgentError error = {0};
    ObsAgentEngine *engine = obs_agent_open(&cfg, &error);
    if (!engine) goto failure;
    if (obs_agent_load_file(engine, argv[1], &error) != OBS_AGENT_OK) goto close_failure;
    uint32_t count = 0;
    if (obs_agent_scene_sphere_count(engine, &count, &error) != OBS_AGENT_OK || count != 0) {
        fprintf(stderr, "Scene mutated during compilation rather than on-frame\n");
        goto close_failure;
    }
    for (int i = 0; i < 5; ++i) {
        if (obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK) goto close_failure;
    }
    if (obs_agent_scene_sphere_count(engine, &count, &error) != OBS_AGENT_OK || count != 5) {
        fprintf(stderr, "Compiled Monad did not construct five production SDF spheres\n");
        goto close_failure;
    }
    ObsAgentSphere v = {0};
    if (obs_agent_scene_sphere_at(engine, 0, &v, &error) != OBS_AGENT_OK ||
        fabs(v.position[0] - 0.60) > 0.001 ||
        fabs(v.rgba[0] - 0.95) > 0.001) {
        fprintf(stderr, "Live Monad frame mutations were not committed\n");
        goto close_failure;
    }
    /* Monad constructed and picked the left sphere; the external client
     * independently ray-picks the very same scene object and edits its PBR. */
    const double from[3] = {-1.6, 0, -3}, toward[3] = {0, 0, 1};
    ObsAgentRayHit hit = {0};
    if (obs_agent_scene_pick_ray(engine, from, toward, 10, &hit, &error) != OBS_AGENT_OK ||
        hit.index != 1 || hit.ref.ticket == 0 ||
        obs_agent_sphere_query(engine, hit.ref, &v, &error) != OBS_AGENT_OK ||
        fabs(v.rgba[1] - 0.75) > 0.001) {
        fprintf(stderr, "Monad-side sphere picking/tint or shared identity failed\n");
        goto close_failure;
    }
    /* The script first selected + edited this object through Monad and
     * performed undo/redo during frame 2. We now continue the same history
     * through the PUBLIC external C interface: select, edit, undo, redo. */
    ObsAgentEditHistory history = {0};
    if (obs_agent_scene_history(engine, &history, &error) != OBS_AGENT_OK ||
        history.selected.ticket != hit.ref.ticket || history.undo_count != 1)
        goto close_failure;
    if (obs_agent_scene_select(engine, hit.ref, &error) != OBS_AGENT_OK)
        goto close_failure;
    ObsAgentSphere edited = v;
    edited.rgba[0] = 0.2; edited.rgba[1] = 0.8;
    edited.rgba[2] = 0.25; edited.rgba[3] = 1.0;
    edited.roughness = 0.2; edited.metallic = 0.6;
    if (obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_MATERIAL,
                                       &edited, &error) != OBS_AGENT_OK ||
        obs_agent_scene_history(engine, &history, &error) != OBS_AGENT_OK ||
        history.undo_count != 2 || history.redo_count != 0 ||
        obs_agent_scene_undo(engine, &error) != OBS_AGENT_OK ||
        obs_agent_sphere_query(engine, hit.ref, &v, &error) != OBS_AGENT_OK ||
        fabs(v.rgba[1] - 0.75) > 0.001 ||
        obs_agent_scene_redo(engine, &error) != OBS_AGENT_OK)
        goto close_failure;
    /* Cycle 53: a non-undo command breaks the undo sequence. The ordinary
     * next undo undoes the previous undo (re-applies the material change).
     * undo-only skips inverse records; explicit redo restores it. */
    if (obs_agent_scene_undo(engine, &error) != OBS_AGENT_OK ||
        obs_agent_scene_command_boundary(engine, &error) != OBS_AGENT_OK ||
        obs_agent_scene_undo(engine, &error) != OBS_AGENT_OK ||
        obs_agent_sphere_query(engine, hit.ref, &v, &error) != OBS_AGENT_OK ||
        fabs(v.rgba[1] - 0.8) > 0.001 ||
        obs_agent_scene_undo_only(engine, &error) != OBS_AGENT_OK ||
        obs_agent_sphere_query(engine, hit.ref, &v, &error) != OBS_AGENT_OK ||
        fabs(v.rgba[1] - 0.75) > 0.001 ||
        obs_agent_scene_redo(engine, &error) != OBS_AGENT_OK)
        goto close_failure;
    /* Cycle 54: a real externally authored live color preview, Escape-style
     * cancel, and a committed preview with Emacs-style undo. */
    ObsAgentSphere preview = v;
    preview.rgba[1] = 0.42;
    if (obs_agent_scene_begin_group(engine, &error) != OBS_AGENT_OK ||
        obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_MATERIAL,
                                      &preview, &error) != OBS_AGENT_OK ||
        /* Submit a real Vulkan frame WHILE the speculative tint is active. */
        obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK ||
        obs_agent_scene_abort_group(engine, &error) != OBS_AGENT_OK ||
        obs_agent_sphere_query(engine, hit.ref, &v, &error) != OBS_AGENT_OK ||
        fabs(v.rgba[1] - 0.8) > 0.001)
        goto close_failure;
    preview = v;
    preview.rgba[1] = 0.9;
    if (obs_agent_scene_begin_group(engine, &error) != OBS_AGENT_OK ||
        obs_agent_scene_edit_selected(engine, OBS_AGENT_EDIT_MATERIAL,
                                      &preview, &error) != OBS_AGENT_OK ||
        obs_agent_scene_end_group(engine, &error) != OBS_AGENT_OK ||
        obs_agent_scene_undo(engine, &error) != OBS_AGENT_OK ||
        obs_agent_sphere_query(engine, hit.ref, &v, &error) != OBS_AGENT_OK ||
        fabs(v.rgba[1] - 0.8) > 0.001)
        goto close_failure;
    if (obs_agent_capture(engine, argv[2], argv[3], NULL, NULL, &error) != OBS_AGENT_OK)
        goto close_failure;
    struct stat image = {0};
    if (stat(argv[2], &image) != 0 || image.st_size != 640 * 360 * 4) {
        fprintf(stderr, "Actual GPU RGBA capture size mismatch\n");
        goto close_failure;
    }
    if (!has_nonuniform_pixels(argv[2])) {
        fprintf(stderr, "Capture is uniform; cannot accept this as a visible scene\n");
        goto close_failure;
    }
    if (obs_agent_scene_sphere_at(engine, 0, &v, &error) != OBS_AGENT_OK ||
        fabs(v.position[0] - 0.84) > 0.001) {
        fprintf(stderr, "Capture frame did not advance compiled Monad scene\n");
        goto close_failure;
    }
    if (obs_agent_sphere_query(engine, hit.ref, &v, &error) != OBS_AGENT_OK ||
        fabs(v.rgba[1] - 0.8) > 0.001 || fabs(v.metallic - 0.6) > 0.001) {
        fprintf(stderr, "Cross-boundary material edit did not survive Vulkan frame\n");
        goto close_failure;
    }
    if (obs_agent_close(engine, &error) != OBS_AGENT_OK) goto failure;
    puts("Cycle 52 PASS: Monad selected/edited sphere, agent continued shared undo/redo history, Vulkan captured image");
    return 0;
close_failure:
    (void)obs_agent_close(engine, NULL);
failure:
    fprintf(stderr, "Cycle 52 FAILURE: %d %s\n", error.code, error.message);
    return 1;
}
