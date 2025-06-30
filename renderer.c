#include "renderer.h"
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
//  | |  .  | |   funny cube
//  | e-----|-f
//  |/      |/
//  a-------b

void cube(vec3 origin, float size, vec4 color) {
    vec3 a, b, c, d, e, f, g, h;
    glm_vec3_copy(origin, a);
    glm_vec3_add(origin, (vec3){size, 0, 0}, b);
    glm_vec3_add(origin, (vec3){size, size, 0}, c);
    glm_vec3_add(origin, (vec3){0, size, 0}, d);
    glm_vec3_add(origin, (vec3){0, 0, size}, e);
    glm_vec3_add(origin, (vec3){size, 0, size}, f);
    glm_vec3_add(origin, (vec3){size, size, size}, g);
    glm_vec3_add(origin, (vec3){0, size, size}, h);

    // Front face (a, b, c, d) // Back face (e, f, g, h)
    triangle(a, b, c, color);  triangle(e, f, g, color);
    triangle(a, c, d, color);  triangle(e, g, h, color);

    // Front face (z=0)
    triangle(a, b, c, color); // a (0,0,0), b (s,0,0), c (s,s,0)
    triangle(a, c, d, color); // a (0,0,0), c (s,s,0), d (0,s,0)

    // Back face (z=s)
    triangle(e, h, g, color); // e (0,0,s), h (0,s,s), g (s,s,s)
    triangle(e, g, f, color); // e (0,0,s), g (s,s,s), f (s,0,s)

    // Left face (x=0)
    triangle(a, d, h, color); // a (0,0,0), d (0,s,0), h (0,s,s)
    triangle(a, h, e, color); // a (0,0,0), h (0,s,s), e (0,0,s)

    // Right face (x=s)
    triangle(b, f, g, color); // b (s,0,0), f (s,0,s), g (s,s,s)
    triangle(b, g, c, color); // b (s,0,0), g (s,s,s), c (s,s,0)

    // Top face (y=s)
    triangle(d, c, g, color); // d (0,s,0), c (s,s,0), g (s,s,s)
    triangle(d, g, h, color); // d (0,s,0), g (s,s,s), h (0,s,s)

    // Bottom face (y=0)
    triangle(a, e, f, color); // a (0,0,0), e (0,0,s), f (s,0,s)
    triangle(a, f, b, color); // a (0,0,0), f (s,0,s), b (s,0,0)
}


void renderer_upload() {
    void* data;
    vkMapMemory(device, vertexBufferMemory, 0, sizeof(vertices), 0, &data);
    memcpy(data, vertices, vertex_count * sizeof(Vertex));
    vkUnmapMemory(device, vertexBufferMemory);
}

void renderer_draw(VkCommandBuffer cmd) {
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, offsets);
    vkCmdDraw(cmd, vertex_count, 1, 0, 0);
}

void renderer_clear() {
    vertex_count = 0;
}
