/// agent_client — external C consumer, no engine or Vulkan private headers
#include "obsidian_agent.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s MODULE.mon IMAGE.rgba REPORT.json\n", argv[0]);
        return EXIT_FAILURE;
    }
    const ObsAgentConfig config = {
        .abi_version = OBS_AGENT_ABI_VERSION,
        .width = 160, .height = 90, .call_camera_policy = true
    };
    ObsAgentError error = {0};
    ObsAgentEngine *engine = obs_agent_open(&config, &error);
    if (!engine) goto failed;
    if (obs_agent_load_file(engine, argv[1], &error) != OBS_AGENT_OK)
        goto close_failed;
    const ObsAgentSphere sphere = {
        .position = {0.0, 0.0, -2.0}, .radius = 0.5,
        .rgba = {1.0, 0.2, 0.15, 1.0}, .roughness = 0.5, .metallic = 0.1
    };
    ObsAgentSdfRef ref = {0};
    if (obs_agent_sphere_create(engine, &sphere, &ref, &error) != OBS_AGENT_OK)
        goto close_failed;
    const double moved[3] = {0.75, 0.25, -2.0};
    const double blue[4] = {0.1, 0.25, 0.95, 1.0};
    if (obs_agent_sphere_set_position(engine, ref, moved, &error) != OBS_AGENT_OK ||
        obs_agent_sphere_set_material(engine, ref, blue, 0.2, 0.7, &error) != OBS_AGENT_OK)
        goto close_failed;
    ObsAgentSphere state;
    if (obs_agent_sphere_query(engine, ref, &state, &error) != OBS_AGENT_OK ||
        fabs(state.position[0] - moved[0]) > 1e-5 ||
        fabs(state.rgba[2] - blue[2]) > 1e-5 ||
        fabs(state.metallic - 0.7) > 1e-5) {
        fprintf(stderr, "Production scene state mismatch after typed mutation\n");
        goto close_failed;
    }
    if (obs_agent_sphere_query(engine, (ObsAgentSdfRef){UINT64_MAX},
                               &state, &error) != OBS_AGENT_STALE_REFERENCE) {
        fprintf(stderr, "Invalid SDF reference was accepted\n");
        goto close_failed;
    }
    for (unsigned frame = 0; frame < 3; ++frame)
        if (obs_agent_step(engine, NULL, NULL, &error) != OBS_AGENT_OK)
            goto close_failed;
    if (obs_agent_capture(engine, argv[2], argv[3], NULL, NULL, &error) !=
        OBS_AGENT_OK) goto close_failed;
    struct stat image = {0};
    if (stat(argv[2], &image) != 0 || image.st_size != 160 * 90 * 4) {
        fprintf(stderr, "Image size is not the promised RGBA8 extent\n");
        goto close_failed;
    }
    FILE *report = fopen(argv[3], "r");
    if (!report) goto close_failed;
    char json[1024];
    size_t count = fread(json, 1, sizeof(json) - 1, report);
    json[count] = 0;
    int closed = fclose(report);
    if (closed != 0 || !strstr(json, "\"schema\":\"obsidian.headless.v1\"") ||
        !strstr(json, "\"frames\":4")) {
        fprintf(stderr, "Agent receipt did not match four rendered frames\n");
        goto close_failed;
    }
    if (obs_agent_close(engine, &error) != OBS_AGENT_OK) goto failed;
    puts("PASS: external C -> live SDF scene + MonadC -> Vulkan frames + image/JSON");
    return EXIT_SUCCESS;
close_failed:
    fprintf(stderr, "Agent failure %d: %s\n", (int)error.code, error.message);
    (void)obs_agent_close(engine, NULL);
    return EXIT_FAILURE;
failed:
    fprintf(stderr, "Agent failure %d: %s\n", (int)error.code, error.message);
    return EXIT_FAILURE;
}
