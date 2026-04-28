#include "font.h"
#include "theme.h"
#include <stdlib.h>
#include <stdbool.h>
#include "vulkan_setup.h"
#include <cglm/cglm.h>
#include "camera.h"
#include "renderer.h"
#include "context.h"
#include "common.h"
#include "window.h"
#include "vertico.h"
#include "editor.h"
#include "gizmo.h"
#include <inttypes.h>
#include "obj.h"
#include "gltf_loader.h"

vec4 red    = {1.0f, 0.0f, 0.0f, 1.0f};
vec4 green  = {0.0f, 1.0f, 0.0f, 1.0f};
vec4 blue   = {0.0f, 0.0f, 1.0f, 1.0f};
vec4 yellow = {1.0f, 1.0f, 0.0f, 1.0f};

int main() {
    /* context.currentFrame = 0; */

    initWindow(WIDTH, HEIGHT, "Obsidian Engine");

    /* load_obj("./assets/obj/teapot.obj", "teapot",  red); */
    /* load_obj("./assets/obj/cow.obj", "cow", blue); */

    // Load textures
    int32_t tex1 = texture_pool_add(&context, "./assets/textures/puta.jpg");
    int32_t tex2 = texture_pool_add(&context, "./assets/textures/prototype/Orange/texture_01.png");
    int32_t tex3 = texture_pool_add(&context, "./assets/textures/prototype/Orange/texture_05.png");
    int32_t tex4 = texture_pool_add(&context, "./assets/textures/prototype/Dark/texture_03.png");
    int32_t tex5 = texture_pool_add(&context, "./assets/textures/pengu.png");

    Texture2D* texture1 = texture_pool_get(tex1);
    Texture2D* texture2 = texture_pool_get(tex2);
    Texture2D* texture3 = texture_pool_get(tex3);
    Texture2D* texture4 = texture_pool_get(tex4);
    Texture2D* texture5 = texture_pool_get(tex5);

    /* load_gltf("./assets/gltf/TwoSidedPlane/glTF/TwoSidedPlane.gltf", &scene);     // FAIL TODO CULL and toggle switch in the inspector for backface culling */

    /* load_gltf("./assets/gltf/VertexColorTest.glb", &scene);                     // PASS */
    /* load_gltf("./assets/gltf/xmp.glb", &scene);                                 // PASS */
    /* load_gltf("./assets/gltf/AnimatedCube/glTF/AnimatedCube.gltf", &scene);     // PASS */
    /* load_gltf("./assets/gltf/MetalRoughSpheres.glb", &scene);                   // PASS */
    /* load_gltf("./assets/gltf/AnimatedMorphCube.glb", &scene);                   // PASS */
    /* load_gltf("./assets/gltf/AnimatedMorphSphere.glb", &scene);                 // PASS */
    /* load_gltf("./assets/gltf/AlphaBlendModeTest.glb", &scene);                  // PASS */
    /* load_gltf("./assets/gltf/UnlitTest.glb", &scene);                           // PASS */
    /* load_gltf("./assets/gltf/Unicode❤♻Test.glb", &scene);                      // PASS */
    /* load_gltf("./assets/gltf/SimpleMorph/glTF/SimpleMorph.gltf", &scene);       // PASS */
    /* load_gltf("./assets/gltf/MaterialsVariantsShoe.glb", &scene);               // TODO variants in the editor */
    /* load_gltf("./assets/gltf/BoxAnimated.glb", &scene);                         // PASS */
    /* load_gltf("./assets/gltf/Box.glb", &scene);                                 // PASS */
    /* load_gltf("./assets/gltf/Corset.glb", &scene);                              // PASS (but it's really small) */
    /* load_gltf("./assets/gltf/CesiumMan.glb", &scene);                           // PASS */
    /* load_gltf("./assets/gltf/Fox.glb", &scene);                                 // PASS */
    /* load_gltf("./assets/gltf/RecursiveSkeletons.glb", &scene);                  // PASS */
    /* load_gltf("./assets/gltf/RiggedFigure.glb", &scene);                        // PASS */
    /* load_gltf("./assets/gltf/Sponza/glTF/Sponza.gltf", &scene);                 // PASS */
    /* load_gltf("./assets/gltf/CarbonFibre.glb", &scene);                         // PASS */
    /* load_gltf("./assets/gltf/MorphStressTest.glb", &scene);                     // PASS */
    /* load_gltf("./assets/gltf/Avocado.glb", &scene);                             // PASS */
    /* load_gltf("./assets/gltf/Lantern.glb", &scene);                             // PASS */
    /* load_gltf("./assets/gltf/TextureCoordinateTest.glb", &scene);               // PASS */
    /* load_gltf("./assets/gltf/AnimatedColorsCube.glb", &scene);                  // FAIL */
    /* load_gltf("./assets/gltf/CubeVisibility.glb", &scene);                      // FAIL */
    /* load_gltf("./assets/gltf/EmissiveStrengthTest.glb", &scene);                // FAIL also the sun goes thorugh walls */
    /* load_gltf("./assets/gltf/InterpolationTest.glb", &scene);                   // FAIL */
    /* load_gltf("./assets/gltf/DragonAttenuation.glb", &scene);                   // PASS */
    /* load_gltf("./assets/gltf/ABeautifulGame/glTF/ABeautifulGame.gltf", &scene); // PASS */
    /* load_gltf("./assets/gltf/MosquitoInAmber/glTF-Binary/MosquitoInAmber.glb", &scene); // PASS */
    /* load_gltf("./assets/gltf/DragonDispersion.glb", &scene);                    // FAIL */
    /* load_gltf("./assets/gltf/DispersionTest.glb", &scene);                      // FAIL */
    /* load_gltf("./assets/gltf/IridescenceMetallicSpheres/glTF/IridescenceMetallicSpheres.gltf", &scene); // FAIL KHR_materials_iridescence */
    /* load_gltf("./assets/gltf/IORTestGrid.glb", &scene);                         // FAIL */
    /* load_gltf("./assets/gltf/TransmissionTest.glb", &scene);                    // FAIL */
    /* load_gltf("./assets/gltf/NodePerformanceTest.glb", &scene);                    // FAIL */


    Font *jetbrains = load_font("./assets/fonts/JetBrainsMono-Regular.ttf", 81);


    // Bake and load the HDR Environment Map
    loadIBL(&context, "./assets/hdr/monochrome_studio_02_4k.hdr");
    /* loadIBL(&context, "./assets/hdr/meadow_2_4k.hdr"); */
    /* loadIBL(&context, "./assets/hdr/ferndale_studio_05_4k.hdr"); */

    // Load our beautiful 4K rock material automatically!
    Material rockMat = load_pbr_material_dir("./assets/textures/rock_wall_10_4k.blend/textures");

    // Variables for delta time
    double lastFrameTime = glfwGetTime();

    cube((vec3){0.0f, 0.0f, 10.0f}, 3.0f, WHITE);

    while (!windowShouldClose()) {
        double currentFrameTime = glfwGetTime();
        lastFrameTime = currentFrameTime;

        beginFrame();

        // 3D GEOMETRY
        /* vec3 v0 = { -0.03f, -0.03f, 0.0f }; */
        /* vec3 v1 = {  0.03f, -0.03f, 0.0f }; */
        /* vec3 v2 = {  0.03f,  0.03f, 0.0f }; */
        /* vec3 v3 = { -0.03f,  0.03f, 0.0f }; */
        /* vec3 center = { 0.0f, 0.0f, 0.0f }; */
        /* triangle(v0, center, v1, RED); */
        /* triangle(v1, center, v2, GREEN); */
        /* triangle(v2, center, v3, BLUE); */
        /* triangle(v3, center, v0, YELLOW); */


        // 64x64 is smooth and highly detailed, generating 24,576 dynamic vertices per frame!
        /* set_material(&rockMat); */
        /* sphere((vec3){0.0f, 0.0f, 30.0f}, 5.0f, 64, 64, WHITE); */
        /* reset_material(); */

        // Translate the cube directly in front of the camera so we can actually see it!
        /* if (scene.meshes.count > 0) { */
        /*     mat4 offset_transform; */
        /*     glm_mat4_identity(offset_transform); */
        /*     glm_translate(offset_transform, (vec3){0.0f, 3.0f, 5.0f}); // X=0, Y=3 (Camera Height), Z=5 (Forward) */

        /*     mat4 temp; */
        /*     glm_mat4_copy(scene.meshes.items[0].model, temp); */
        /*     glm_mat4_mul(offset_transform, temp, scene.meshes.items[0].model); */
        /*     markMeshesSSBODirty(&context); */
        /* } */

        // TODO
        /* set_material(&rockMat); */
        /* cube((vec3){0.0f, 0.0f, 10.0f}, 3.0f, WHITE); */
        /* reset_material(); */


        /* text(jetbrains, "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~", 150, 50, WHITE); */

        // Draw 3D text at world position
        /* text3D(jetbrains, "Hello 3D!", (vec3){0.0f, 5.0f, 10.0f}, 6, WHITE); */
        /* text3D(jetbrains, "Press SPACE", (vec3){0.0f, 4.5f, 10.0f}, 6, RED); */

        // 2D GEOMETRY
        /* quad2D((vec2){10, 10}, (vec2){50, 50}, BLUE); */
        /* quad2D((vec2){70, 10}, (vec2){50, 50}, WHITE); */
        /* quad2D((vec2){10, 70}, (vec2){50, 50}, RED); */
        /* quad2D((vec2){70, 70}, (vec2){50, 50}, GREEN); */

        fps(jetbrains, 200, 200, RED);


        /* texture2D((vec2){100, 200}, (vec2){200, 200}, texture1, WHITE); */
        /* texture2D((vec2){500, 200}, (vec2){150, 150}, texture2, WHITE); */
        /* texture2D((vec2){300, 300}, (vec2){600, 600}, texture2, WHITE); */

        // Render axes
        float lineLength = 10000.0f; // Very long lines to appear "infinite"
        line((vec3){-lineLength, 0.0f, 0.0f}, (vec3){lineLength, 0.0f, 0.0f}, CT.x);
        line((vec3){0.0f, -lineLength, 0.0f}, (vec3){0.0f, lineLength, 0.0f}, CT.y);
        line((vec3){0.0f, 0.0f, -lineLength}, (vec3){0.0f, 0.0f, lineLength}, CT.z);

        endFrame();
    }

    vkDeviceWaitIdle(context.device);
    editor_cleanup();
    texture_pool_cleanup(&context);
    cleanup(&context);

    return EXIT_SUCCESS;
}
