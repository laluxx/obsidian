/// node_topology.c — real hierarchy validation and parent-first invariants
#include "node_topology.h"
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TestNode { unsigned char padding[7]; int32_t parent; uint64_t payload; } TestNode;

/// Valid fixtures and parent-first property
static void valid_fixture(void) {
    TestNode nodes[7] = {
        {{0}, 4, 0}, {{0}, -1, 1}, {{0}, 0, 2}, {{0}, 1, 3},
        {{0}, 1, 4}, {{0}, 2, 5}, {{0}, -1, 6}
    };
    uint32_t order[7], stack[7]; uint8_t state[7];
    assert(obs_node_topological_order(nodes,7,sizeof(TestNode),
           offsetof(TestNode,parent),order,stack,state));
    int32_t position[7];
    for (unsigned i=0;i<7;i++) position[order[i]]=(int32_t)i;
    for (unsigned i=0;i<7;i++)
        if (nodes[i].parent>=0) assert(position[nodes[i].parent]<position[i]);
}

/// Reject cycles, invalid edges, and malformed storage
static void invalid_fixtures(void) {
    TestNode nodes[4] = {{{0},-1,0},{{0},0,0},{{0},1,0},{{0},2,0}};
    uint32_t order[4],stack[4]; uint8_t state[4];
#define CHECK() obs_node_topological_order(nodes,4,sizeof(TestNode),offsetof(TestNode,parent),order,stack,state)
    assert(CHECK());
    nodes[0].parent=3; assert(!CHECK()); // long cycle
    nodes[0].parent=-1;
    nodes[2].parent=2; assert(!CHECK()); // self cycle
    nodes[2].parent=4; assert(!CHECK()); // out of range
    nodes[2].parent=-2; assert(!CHECK()); // illegal sentinel
    nodes[2].parent=-1; assert(CHECK());
    assert(!obs_node_topological_order(NULL,4,sizeof(TestNode),offsetof(TestNode,parent),order,stack,state));
    assert(!obs_node_topological_order(nodes,4,1,0,order,stack,state));
    assert(!obs_node_topological_order(nodes,4,sizeof(TestNode),sizeof(TestNode),order,stack,state));
    assert(!obs_node_topological_order(nodes,4,sizeof(TestNode),offsetof(TestNode,parent),stack,stack,state));
    assert(obs_node_topological_order(NULL,0,0,0,NULL,NULL,NULL));
#undef CHECK
}

/// Deterministic adversarial topologies
static uint32_t rand_state = 0x9e3779b9u;
static uint32_t rand32(void) {rand_state ^= rand_state << 13; rand_state ^= rand_state >> 17; rand_state ^= rand_state << 5; return rand_state;}
static void stress(void) {
    enum { COUNT=127, ROUNDS=50000 };
    TestNode nodes[COUNT]; uint32_t order[COUNT],stack[COUNT]; uint8_t state[COUNT];
    for (unsigned round=0;round<ROUNDS;round++) {
        uint32_t permutation[COUNT];
        for (unsigned i=0;i<COUNT;i++) permutation[i]=i;
        for (unsigned i=COUNT-1;i>0;i--) {
            unsigned j=rand32()%(i+1);
            uint32_t temp=permutation[i];permutation[i]=permutation[j];permutation[j]=temp;
        }
        for (unsigned i=0;i<COUNT;i++) {
            unsigned idx=permutation[i];
            nodes[idx].parent = (i==0 || rand32()%11==0) ? -1 :
                                (int32_t)permutation[rand32()%i];
        }
        assert(obs_node_topological_order(nodes,COUNT,sizeof(TestNode),
                 offsetof(TestNode,parent),order,stack,state));
        int32_t position[COUNT];
        for (unsigned i=0;i<COUNT;i++) position[order[i]]=(int32_t)i;
        for (unsigned i=0;i<COUNT;i++)
            if(nodes[i].parent>=0) assert(position[nodes[i].parent]<position[i]);
        if ((round % 7) == 0) {
            int32_t descendant=0;
            while(nodes[descendant].parent>=0) descendant=nodes[descendant].parent;
            if (descendant != 0) {
                nodes[descendant].parent=0;
                assert(!obs_node_topological_order(nodes,COUNT,sizeof(TestNode),
                         offsetof(TestNode,parent),order,stack,state));
            }
        }
    }
}
int main(void) {valid_fixture();invalid_fixtures();stress();puts("node_topology: 50,000 randomized forests and failure cases PASS");}
