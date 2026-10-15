/// gltf_source_plan — real cgltf parser, not a mock source reader
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "gltf_source_plan.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>

/// Concurrent parser fixture
static void *parallel_plan(void *unused) {
    (void)unused;
    for (int i = 0; i < 100; ++i) {
        ObsGltfSourcePlan plan;
        assert(obs_gltf_source_plan("tests/fixtures/async-triangle.gltf", &plan)
               == OBS_GLTF_PLAN_OK);
        assert(plan.triangles == 1);
    }
    return NULL;
}

/// External images are part of the observed source identity.
static void external_image_identity(void) {
    char directory[] = "/tmp/obsidian-c43-source-XXXXXX";
    assert(mkdtemp(directory));
    char source[512], image[512];
    assert(snprintf(source, sizeof(source), "%s/model.gltf", directory) > 0);
    assert(snprintf(image, sizeof(image), "%s/texture.dat", directory) > 0);
    FILE *f = fopen("tests/fixtures/async-triangle.gltf", "rb");
    assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long len = ftell(f);
    assert(len > 1 && len < 8192 && fseek(f, 0, SEEK_SET) == 0);
    char original[8192];
    assert(fread(original, 1, (size_t)len, f) == (size_t)len);
    assert(fclose(f) == 0);
    while (len && (original[len-1] == '\n' || original[len-1] == ' ')) --len;
    assert(original[len-1] == '}');
    f = fopen(source, "wb");
    assert(f);
    assert(fwrite(original, 1, (size_t)len - 1, f) == (size_t)len - 1);
    assert(fputs(",\"images\":[{\"uri\":\"texture%2Edat\"}]}", f) >= 0);
    assert(fclose(f) == 0);
    f = fopen(image, "wb");
    assert(f);
    assert(fwrite("ABCD", 1, 4, f) == 4 && fclose(f) == 0);
    ObsGltfSourcePlan a = {0}, b = {0};
    assert(obs_gltf_source_plan(source, &a) == OBS_GLTF_PLAN_OK);
    assert(a.images == 1);
    assert(obs_gltf_source_plan(source, &b) == OBS_GLTF_PLAN_OK);
    assert(a.source_fingerprint == b.source_fingerprint);
    f = fopen(image, "wb");
    assert(f);
    assert(fwrite("ABCE", 1, 4, f) == 4 && fclose(f) == 0);
    assert(obs_gltf_source_plan(source, &b) == OBS_GLTF_PLAN_OK);
    assert(a.source_fingerprint != b.source_fingerprint);
    f = fopen(image, "wb");
    assert(f && ftruncate(fileno(f), (off_t)512 * 1024 * 1024 + 1) == 0);
    assert(fclose(f) == 0);
    assert(obs_gltf_source_plan(source, &b) == OBS_GLTF_PLAN_LIMIT);
    assert(unlink(image) == 0);
    assert(obs_gltf_source_plan(source, &b) == OBS_GLTF_PLAN_IO);
    assert(unlink(source) == 0);
    assert(rmdir(directory) == 0);
}

/// Real source fixtures
int main(void) {
    external_image_identity();
    ObsGltfSourcePlan valid = {0}, again = {0};
    assert(obs_gltf_source_plan("tests/fixtures/async-triangle.gltf", &valid)
           == OBS_GLTF_PLAN_OK);
    assert(valid.meshes == 1 && valid.nodes == 1 && valid.primitives == 1);
    assert(valid.vertices == 3 && valid.indices == 3 && valid.triangles == 1);
    assert(valid.source_fingerprint != 0 && !valid.has_skinning);
    assert(obs_gltf_source_plan("tests/fixtures/async-morph-growth.gltf",
                                &again) == OBS_GLTF_PLAN_OK);
    assert(again.vertices == 200 && again.has_morph_targets);
    assert(obs_gltf_source_plan("tests/fixtures/async-meshlet-growth.gltf",
                                &again) == OBS_GLTF_PLAN_OK);
    assert(again.vertices == 9000 && again.indices == 9000 &&
           again.triangles == 3000);
    assert(obs_gltf_source_plan("tests/fixtures/async-static-unsupported.gltf", &again)
           == OBS_GLTF_PLAN_OK);
    assert(obs_gltf_source_plan("tests/fixtures/async-triangle.gltf", &again)
           == OBS_GLTF_PLAN_OK);
    assert(valid.source_fingerprint == again.source_fingerprint);
    assert(obs_gltf_source_plan("tests/fixtures/async-material.gltf", &again)
           == OBS_GLTF_PLAN_OK);
    /* Same vertex/index data but different material: the source signature
     * must distinguish them. */
    assert(valid.source_fingerprint != again.source_fingerprint);
    assert(obs_gltf_source_plan("tests/fixtures/async-lines.gltf", &again)
           == OBS_GLTF_PLAN_UNSUPPORTED);
    assert(obs_gltf_source_plan("tests/fixtures/async-bad-index.gltf", &again)
           == OBS_GLTF_PLAN_INVALID);
    assert(obs_gltf_source_plan("tests/fixtures/does-not-exist.gltf", &again)
           == OBS_GLTF_PLAN_IO);
    assert(obs_gltf_source_plan(NULL, &again) == OBS_GLTF_PLAN_INVALID);
    assert(obs_gltf_source_plan("tests/fixtures/async-triangle.gltf", NULL)
           == OBS_GLTF_PLAN_INVALID);
    assert(!strcmp(obs_gltf_plan_error_name(OBS_GLTF_PLAN_UNSUPPORTED),"unsupported"));
    pthread_t threads[4];
    for (size_t i = 0; i < 4; ++i)
        assert(pthread_create(&threads[i], NULL, parallel_plan, NULL) == 0);
    for (size_t i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    puts("real cgltf async source inventory: success, missing, invalid, unsupported OK");
    return 0;
}
