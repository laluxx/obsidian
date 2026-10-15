/// node_topology_bench.c — CPU-only cached-order vs repeated parent traversal
#include "node_topology.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <time.h>

typedef struct Node { int32_t parent; } Node;
static volatile uint64_t sink;
static uint64_t ns(void) {
    struct timespec t; timespec_get(&t, TIME_UTC);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
int main(int argc,char **argv) {
    const unsigned count = argc > 1 ? (unsigned)strtoul(argv[1],NULL,10) : 4096;
    const unsigned frames = argc > 2 ? (unsigned)strtoul(argv[2],NULL,10) : 3000;
    if(!count || count>200000 || !frames || frames>100000) return 2;
    Node *nodes=calloc(count,sizeof(*nodes));
    uint32_t *order=malloc(count*sizeof(*order)),*stack=malloc(count*sizeof(*stack));
    uint8_t *states=malloc(count),*resolved=malloc(count);
    if(!nodes || !order || !stack || !states || !resolved) return 2;
    for(unsigned i=0;i<count;i++) nodes[i].parent = (i+1==count) ? -1 : (int32_t)(i+1);
    if(!obs_node_topological_order(nodes,count,sizeof(Node),offsetof(Node,parent),order,stack,states)) return 2;
    uint64_t before=ns();
    for(unsigned f=0;f<frames;f++) {
        memset(resolved,0,count);
        for(unsigned n=0;n<count;n++) {
            uint32_t curr=n,depth=0;
            while(!resolved[curr]) {
                stack[depth++]=curr;
                int32_t p=nodes[curr].parent;
                if(p<0) break;
                curr=(uint32_t)p;
            }
            while(depth) {unsigned i=stack[--depth]; resolved[i]=1; sink+=(uint64_t)(i & 1);}
        }
    }
    uint64_t legacy=ns()-before;
    before=ns();
    for(unsigned f=0;f<frames;f++)
        for(unsigned k=0;k<count;k++) sink+=(uint64_t)(order[k] & 1);
    uint64_t prepared=ns()-before;
    printf("node topology CPU-only (n=%u, frames=%u): repeated-walk %.3f ns/node, prepared-order %.3f ns/node; no Vulkan, bones or FFI\n",
           count,frames,(double)legacy/(count*(double)frames),(double)prepared/(count*(double)frames));
    free(nodes);free(order);free(stack);free(states);free(resolved);
    return 0;
}
