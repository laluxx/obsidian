#include "renderer.h"
#include "context.h"

#include <stdlib.h>
#include <string.h>

static Vertex vertices[MAX_VERTICES];
static uint32_t vertex_count = 0;

static VkDevice device;
static VkPhysicalDevice physicalDevice;
static VkCommandPool commandPool;
static VkQueue graphicsQueue;

static VkBuffer vertexBuffer;
static VkDeviceMemory vertexBufferMemory;

void renderer_init(VkDevice dev, VkPhysicalDevice physDev, VkCommandPool cmdPool, VkQueue queue) {
    device = dev;
    physicalDevice = physDev;
    commandPool = cmdPool;
    graphicsQueue = queue;

    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(vertices),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(device, &bufferInfo, NULL, &vertexBuffer);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(device, vertexBuffer, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = 0
    };

    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((memRequirements.memoryTypeBits & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == 
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            allocInfo.memoryTypeIndex = i;
            break;
        }
    }

    vkAllocateMemory(device, &allocInfo, NULL, &vertexBufferMemory);
    vkBindBufferMemory(device, vertexBuffer, vertexBufferMemory, 0);
}

void renderer_shutdown() {
    vkDestroyBuffer(device, vertexBuffer, NULL);
    vkFreeMemory(device, vertexBufferMemory, NULL);
}

void vertex(vec3 pos, vec4 color) {
    if (vertex_count >= MAX_VERTICES) return;
    glm_vec3_copy(pos, vertices[vertex_count].pos);
    glm_vec4_copy(color, vertices[vertex_count].color);
    vertex_count++;
}

void line(vec3 a, vec3 b, vec4 color) {
    vertex(a, color);
    vertex(b, color);
}

void triangle(vec3 a, vec3 b, vec3 c, vec4 color) {
    vertex(a, color);
    vertex(b, color);
    vertex(c, color);
}


//    h-------g
//   /|      /|
//  d-------c |
//  | |  .  | |  <- Origin
//  | e-----|-f
//  |/      |/
//  a-------b

void cube(vec3 origin, float size, vec4 color) {
    float s = size / 2.0f;
    vec3 a, b, c, d, e, f, g, h;

    // Define vertices relative to center
    glm_vec3_add(origin, (vec3){-s, -s, -s}, a); // a
    glm_vec3_add(origin, (vec3){+s, -s, -s}, b); // b
    glm_vec3_add(origin, (vec3){+s, +s, -s}, c); // c
    glm_vec3_add(origin, (vec3){-s, +s, -s}, d); // d
    glm_vec3_add(origin, (vec3){-s, -s, +s}, e); // e
    glm_vec3_add(origin, (vec3){+s, -s, +s}, f); // f
    glm_vec3_add(origin, (vec3){+s, +s, +s}, g); // g
    glm_vec3_add(origin, (vec3){-s, +s, +s}, h); // h

    // Now the triangles stay exactly the same!
    // Front face (z=-s)
    triangle(a, b, c, color); triangle(a, c, d, color);
    // Back face (z=+s)
    triangle(e, h, g, color); triangle(e, g, f, color);
    // Left face (x=-s)
    triangle(a, d, h, color); triangle(a, h, e, color);
    // Right face (x=+s)
    triangle(b, f, g, color); triangle(b, g, c, color);
    // Top face (y=+s)
    triangle(d, c, g, color); triangle(d, g, h, color);
    // Bottom face (y=-s)
    triangle(a, e, f, color); triangle(a, f, b, color);
}

void sphere(vec3 center, float radius, int latDiv, int longDiv, vec4 color) {
    for (int lat = 0; lat < latDiv; ++lat) {
        float theta1 = (float)lat / latDiv * GLM_PI;
        float theta2 = (float)(lat + 1) / latDiv * GLM_PI;

        for (int lon = 0; lon < longDiv; ++lon) {
            float phi1 = (float)lon / longDiv * 2.0f * GLM_PI;
            float phi2 = (float)(lon + 1) / longDiv * 2.0f * GLM_PI;

            vec3 v0, v1, v2, v3;
            glm_vec3_copy((vec3){
                center[0] + radius * sinf(theta1) * cosf(phi1),
                center[1] + radius * cosf(theta1),
                center[2] + radius * sinf(theta1) * sinf(phi1)
            }, v0);
            glm_vec3_copy((vec3){
                center[0] + radius * sinf(theta2) * cosf(phi1),
                center[1] + radius * cosf(theta2),
                center[2] + radius * sinf(theta2) * sinf(phi1)
            }, v1);
            glm_vec3_copy((vec3){
                center[0] + radius * sinf(theta2) * cosf(phi2),
                center[1] + radius * cosf(theta2),
                center[2] + radius * sinf(theta2) * sinf(phi2)
            }, v2);
            glm_vec3_copy((vec3){
                center[0] + radius * sinf(theta1) * cosf(phi2),
                center[1] + radius * cosf(theta1),
                center[2] + radius * sinf(theta1) * sinf(phi2)
            }, v3);

            triangle(v0, v1, v2, color);
            triangle(v0, v2, v3, color);
        }
    }
}


void renderer_upload() {
    void* data;
    vkMapMemory(device, vertexBufferMemory, 0, sizeof(vertices), 0, &data);
    memcpy(data, vertices, vertex_count * sizeof(Vertex));
    vkUnmapMemory(device, vertexBufferMemory);
}

void renderer_draw(VkCommandBuffer cmd) {
    mat4 identity;
    glm_mat4_identity(identity);
    vkCmdPushConstants(
        cmd,
        context.pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(mat4),
        identity
    );

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, offsets);
    vkCmdDraw(cmd, vertex_count, 1, 0, 0);
}

void renderer_clear() {
    vertex_count = 0;
}


void mesh(VkCommandBuffer cmd, Mesh* mesh) {
    vkCmdPushConstants(
        cmd,
        context.pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(mat4),
        mesh->model
    );

    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->vertexBuffer, offsets);
    vkCmdDraw(cmd, mesh->vertexCount, 1, 0, 0);
}

void mesh_destroy(VkDevice device, Mesh* mesh) {
    if (mesh->vertexBuffer) vkDestroyBuffer(device, mesh->vertexBuffer, NULL);
    if (mesh->vertexBufferMemory) vkFreeMemory(device, mesh->vertexBufferMemory, NULL);

    mesh->vertexBuffer = VK_NULL_HANDLE;
    mesh->vertexBufferMemory = VK_NULL_HANDLE;
    mesh->vertexCount = 0;
}

//

void meshes_init(Meshes* meshes) {
    meshes->items = NULL;
    meshes->count = 0;
    meshes->capacity = 0;
}

void meshes_add(Meshes* meshes, Mesh mesh) {
    if (meshes->count == meshes->capacity) {
        size_t new_capacity = meshes->capacity ? meshes->capacity * 2 : 4;
        meshes->items = realloc(meshes->items, new_capacity * sizeof(Mesh));
        meshes->capacity = new_capacity;
    }
    meshes->items[meshes->count++] = mesh;
}

void meshes_remove(Meshes* meshes, size_t index) {
    if (index >= meshes->count) return;
    // Destroy the mesh buffer on device!
    // mesh_destroy(device, &meshes->items[index]);
    for (size_t i = index; i < meshes->count - 1; ++i)
        meshes->items[i] = meshes->items[i + 1];
    meshes->count--;
}

void meshes_draw(VkCommandBuffer cmd, Meshes* meshes) {
    for (size_t i = 0; i < meshes->count; ++i) {
        mesh(cmd, &meshes->items[i]);
    }
}

void meshes_destroy(VkDevice device, Meshes* meshes) {
    for (size_t i = 0; i < meshes->count; ++i) {
        mesh_destroy(device, &meshes->items[i]);
    }
    free(meshes->items);
    meshes->items = NULL;
    meshes->count = 0;
    meshes->capacity = 0;
}
