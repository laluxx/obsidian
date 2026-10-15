/// gltf_topology_fixture.c — real cgltf parsing through the validated graph order
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "node_topology.h"
#include "node_overrides.h"
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct TestNode { int32_t parent; uint64_t identity; } TestNode;
static const char fixture[] =
    "{\"asset\":{\"version\":\"2.0\"},"
    "\"scene\":0,\"scenes\":[{\"nodes\":[2]}],"
    "\"nodes\":["
    "{\"name\":\"leaf\",\"translation\":[0,2,0]},"
    "{\"name\":\"child\",\"children\":[0],\"translation\":[1,0,0]},"
    "{\"name\":\"root\",\"children\":[1],\"translation\":[0,0,3]}]}";

int main(void) {
    cgltf_data *data = NULL;
    cgltf_options options = {0};
    assert(cgltf_parse(&options, fixture, sizeof(fixture) - 1, &data) == cgltf_result_success);
    assert(data != NULL && data->nodes_count == 3);
    TestNode nodes[3];
    for (uint32_t i=0;i<3;i++) {
        nodes[i].parent = data->nodes[i].parent ?
            (int32_t)(data->nodes[i].parent - data->nodes) : -1;
        nodes[i].identity=i;
    }
    uint32_t order[3], stack[3]; uint8_t states[3];
    assert(obs_node_topological_order(nodes,3,sizeof(TestNode),
          offsetof(TestNode,parent),order,stack,states));
    assert(order[0] == 2 && order[1] == 1 && order[2] == 0);

    float local[4][4];
    cgltf_node_transform_local(&data->nodes[1], &local[0][0]);
    const float extra[3] = {0, 4, 0};
    obs_node_apply_local_translation(local, extra);
    assert(local[3][0] == 1 && local[3][1] == 4 && local[3][2] == 0);
    cgltf_free(data);
    puts("gltf_topology_fixture: parsed glTF parent order and persistent offset PASS");
}
