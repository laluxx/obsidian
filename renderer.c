#include <stdlib.h>
#include <string.h>
#include "renderer.h"
#include "context.h"
#include "common.h"

static Vertex vertices[MAX_VERTICES];
static uint32_t vertex_count = 0;

static VkDevice device;
static VkPhysicalDevice physicalDevice;
static VkCommandPool commandPool;
static VkQueue graphicsQueue;

static VkBuffer vertexBuffer;
static VkDeviceMemory vertexBufferMemory;

PushConstants pushConstants;


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

void vertex_with_normal(vec3 pos, vec4 color, vec3 normal) {
    if (vertex_count >= MAX_VERTICES) return;
    glm_vec3_copy(pos, vertices[vertex_count].pos);
    glm_vec4_copy(color, vertices[vertex_count].color);
    glm_vec3_copy(normal, vertices[vertex_count].normal);
    vertex_count++;
}


void vertex(vec3 pos, vec4 color) {
    if (vertex_count >= MAX_VERTICES) return;
    glm_vec3_copy(pos, vertices[vertex_count].pos);
    glm_vec4_copy(color, vertices[vertex_count].color);
    vertex_count++;
}

void line(vec3 a, vec3 b, vec4 color) {
    vec3 normal = {0.0f, 1.0f, 0.0f}; // Lines don't need accurate normals
    vertex_with_normal(a, color, normal);
    vertex_with_normal(b, color, normal);
}

void triangle(vec3 a, vec3 b, vec3 c, vec4 color) {
    // Calculate face normal using cross product
    vec3 edge1, edge2, normal;
    glm_vec3_sub(b, a, edge1);
    glm_vec3_sub(c, a, edge2);
    glm_vec3_cross(edge1, edge2, normal);
    glm_vec3_normalize(normal);
    
    // All three vertices share the same face normal
    vertex_with_normal(a, color, normal);
    vertex_with_normal(b, color, normal);
    vertex_with_normal(c, color, normal);
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

    // Front face (z=-s, normal pointing -Z)
    vec3 n_front = {0.0f, 0.0f, -1.0f};
    vertex_with_normal(a, color, n_front);
    vertex_with_normal(b, color, n_front);
    vertex_with_normal(c, color, n_front);
    vertex_with_normal(a, color, n_front);
    vertex_with_normal(c, color, n_front);
    vertex_with_normal(d, color, n_front);

    // Back face (z=+s, normal pointing +Z)
    vec3 n_back = {0.0f, 0.0f, 1.0f};
    vertex_with_normal(e, color, n_back);
    vertex_with_normal(h, color, n_back);
    vertex_with_normal(g, color, n_back);
    vertex_with_normal(e, color, n_back);
    vertex_with_normal(g, color, n_back);
    vertex_with_normal(f, color, n_back);

    // Left face (x=-s, normal pointing -X)
    vec3 n_left = {-1.0f, 0.0f, 0.0f};
    vertex_with_normal(a, color, n_left);
    vertex_with_normal(d, color, n_left);
    vertex_with_normal(h, color, n_left);
    vertex_with_normal(a, color, n_left);
    vertex_with_normal(h, color, n_left);
    vertex_with_normal(e, color, n_left);

    // Right face (x=+s, normal pointing +X)
    vec3 n_right = {1.0f, 0.0f, 0.0f};
    vertex_with_normal(b, color, n_right);
    vertex_with_normal(f, color, n_right);
    vertex_with_normal(g, color, n_right);
    vertex_with_normal(b, color, n_right);
    vertex_with_normal(g, color, n_right);
    vertex_with_normal(c, color, n_right);

    // Top face (y=+s, normal pointing +Y)
    vec3 n_top = {0.0f, 1.0f, 0.0f};
    vertex_with_normal(d, color, n_top);
    vertex_with_normal(c, color, n_top);
    vertex_with_normal(g, color, n_top);
    vertex_with_normal(d, color, n_top);
    vertex_with_normal(g, color, n_top);
    vertex_with_normal(h, color, n_top);

    // Bottom face (y=-s, normal pointing -Y)
    vec3 n_bottom = {0.0f, -1.0f, 0.0f};
    vertex_with_normal(a, color, n_bottom);
    vertex_with_normal(e, color, n_bottom);
    vertex_with_normal(f, color, n_bottom);
    vertex_with_normal(a, color, n_bottom);
    vertex_with_normal(f, color, n_bottom);
    vertex_with_normal(b, color, n_bottom);
}

void sphere(vec3 center, float radius, int latDiv, int longDiv, vec4 color) {
    for (int lat = 0; lat < latDiv; ++lat) {
        float theta1 = (float)lat / latDiv * GLM_PI;
        float theta2 = (float)(lat + 1) / latDiv * GLM_PI;

        for (int lon = 0; lon < longDiv; ++lon) {
            float phi1 = (float)lon / longDiv * 2.0f * GLM_PI;
            float phi2 = (float)(lon + 1) / longDiv * 2.0f * GLM_PI;

            // Calculate vertices
            vec3 v0 = {
                center[0] + radius * sinf(theta1) * cosf(phi1),
                center[1] + radius * cosf(theta1),
                center[2] + radius * sinf(theta1) * sinf(phi1)
            };
            vec3 v1 = {
                center[0] + radius * sinf(theta2) * cosf(phi1),
                center[1] + radius * cosf(theta2),
                center[2] + radius * sinf(theta2) * sinf(phi1)
            };
            vec3 v2 = {
                center[0] + radius * sinf(theta2) * cosf(phi2),
                center[1] + radius * cosf(theta2),
                center[2] + radius * sinf(theta2) * sinf(phi2)
            };
            vec3 v3 = {
                center[0] + radius * sinf(theta1) * cosf(phi2),
                center[1] + radius * cosf(theta1),
                center[2] + radius * sinf(theta1) * sinf(phi2)
            };

            // For sphere, normal at each vertex points from center to vertex
            vec3 n0, n1, n2, n3;
            glm_vec3_sub(v0, center, n0);
            glm_vec3_normalize(n0);
            glm_vec3_sub(v1, center, n1);
            glm_vec3_normalize(n1);
            glm_vec3_sub(v2, center, n2);
            glm_vec3_normalize(n2);
            glm_vec3_sub(v3, center, n3);
            glm_vec3_normalize(n3);

            // First triangle
            vertex_with_normal(v0, color, n0);
            vertex_with_normal(v1, color, n1);
            vertex_with_normal(v2, color, n2);

            // Second triangle
            vertex_with_normal(v0, color, n0);
            vertex_with_normal(v2, color, n2);
            vertex_with_normal(v3, color, n3);
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
    glm_mat4_identity(pushConstants.model);
    // ambientOcclusionEnabled should be set elsewhere before drawing
    
    vkCmdPushConstants(
        cmd,
        context.pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PushConstants),
        &pushConstants
    );
    
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, offsets);
    vkCmdDraw(cmd, vertex_count, 1, 0, 0);
}

void renderer_clear() {
    vertex_count = 0;
}


void mesh(VkCommandBuffer cmd, Mesh* mesh) {
    glm_mat4_copy(mesh->model, pushConstants.model);
    // ambientOcclusionEnabled should be set elsewhere before drawing
    
    vkCmdPushConstants(
        cmd,
        context.pipelineLayout,
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0,
        sizeof(PushConstants),
        &pushConstants
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


/// 2D

static Vertex2D vertices2D[MAX_VERTICES];
static uint32_t vertexCount2D = 0;

void renderer2D_init(VulkanContext* context) {
    // Create 2D vertex buffer
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = sizeof(vertices2D),
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    vkCreateBuffer(context->device, &bufferInfo, NULL, &context->vertexBuffer2D);

    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(context->device, context->vertexBuffer2D, &memRequirements);

    VkMemoryAllocateInfo allocInfo = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(context->physicalDevice, memRequirements.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
    };

    vkAllocateMemory(context->device, &allocInfo, NULL, &context->vertexBufferMemory2D);
    vkBindBufferMemory(context->device, context->vertexBuffer2D, context->vertexBufferMemory2D, 0);
}

void renderer2D_quad(vec2 position, vec2 size, Color color) {
    if (vertexCount2D + 6 > MAX_VERTICES) return;

    float x = position[0], y = position[1];
    float w = size[0], h = size[1];

    // Two triangles to form a quad
    Vertex2D quad[6] = {
        {{x, y}, color, {0.0f, 0.0f}},
        {{x + w, y}, color, {1.0f, 0.0f}},
        {{x + w, y + h}, color, {1.0f, 1.0f}},
        
        {{x, y}, color, {0.0f, 0.0f}},
        {{x + w, y + h}, color, {1.0f, 1.0f}},
        {{x, y + h}, color, {0.0f, 1.0f}}
    };

    memcpy(&vertices2D[vertexCount2D], quad, sizeof(quad));
    vertexCount2D += 6;
}

void renderer2D_upload(VulkanContext* context) {
    if (vertexCount2D == 0) return;
    
    void* data;
    vkMapMemory(context->device, context->vertexBufferMemory2D, 0, sizeof(vertices2D), 0, &data);
    memcpy(data, vertices2D, vertexCount2D * sizeof(Vertex2D));
    vkUnmapMemory(context->device, context->vertexBufferMemory2D);
}

void renderer2D_draw(VkCommandBuffer cmd) {
    if (vertexCount2D == 0) return;

    // Create orthographic projection for 2D
    mat4 projection;
    glm_ortho(0.0f, (float)context.swapChainExtent.width, 
              (float)context.swapChainExtent.height, 0.0f, 
              -1.0f, 1.0f, projection);
    
    // Push constants for 2D pipeline
    vkCmdPushConstants(
        cmd,
        context.pipelineLayout2D,  // Use 2D pipeline layout!
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(mat4),
        &projection
    );
    
    // Bind 2D pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, context.graphicsPipeline2D);
    
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &context.vertexBuffer2D, offsets);
    vkCmdDraw(cmd, vertexCount2D, 1, 0, 0);
}

void renderer2D_clear(void) {
    vertexCount2D = 0;
}
