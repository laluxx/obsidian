#define _POSIX_C_SOURCE 200809L
/// omdl_cache.c — deterministic cache layout and I/O rejection tests
#include "omdl_cache.h"
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/// Fixtures
static const size_t sizes[OMDL_SECTION_COUNT] = {
    [OMDL_MESHES]=8, [OMDL_VERTICES]=12, [OMDL_INDICES]=4,
    [OMDL_MORPHS]=24, [OMDL_MESHLETS]=32,
    [OMDL_MESHLET_BOUNDS]=16, [OMDL_MESHLET_SKINS]=12,
    [OMDL_MESHLET_VERTICES]=4, [OMDL_MESHLET_TRIANGLES]=1,
    [OMDL_NODES]=80, [OMDL_SKINS]=12, [OMDL_JOINTS]=4,
    [OMDL_IBMS]=64, [OMDL_ANIMATIONS]=76,
    [OMDL_CHANNELS]=20, [OMDL_FLOATS]=4,
};
#define MAGIC UINT32_C(0x4C444D54)

static FILE *image(OmdlHeader header) {
    FILE *f=tmpfile(); assert(f);
    assert(fwrite(&header, sizeof header, 1, f) == 1);
    OmdlLayout layout;
    /* This fixture independently calculates the payload length, not through
     * the layout validator that it tests. */
    uint64_t payload = (uint64_t)header.mesh_count*8 +
        (uint64_t)header.total_vertices*12 + (uint64_t)header.total_indices*4 +
        (uint64_t)header.total_morph_deltas*24 +
        (uint64_t)header.total_meshlets*(32+16+12) +
        (uint64_t)header.total_meshlet_vertices*4 +
        (uint64_t)header.total_meshlet_triangles +
        (uint64_t)header.node_count*80 +
        (uint64_t)header.skin_count*12 + (uint64_t)header.total_joints*(4+64) +
        (uint64_t)header.anim_count*76 +
        (uint64_t)header.total_channels*20 + (uint64_t)header.total_floats*4;
    assert(payload < 65536);
    unsigned char zero[512]={0};
    while (payload) {
        size_t n=payload < sizeof zero ? (size_t)payload : sizeof zero;
        assert(fwrite(zero,1,n,f)==n);
        payload-=n;
    }
    (void)layout;
    rewind(f);
    return f;
}

static OmdlHeader example(void) {
    return (OmdlHeader){.magic=MAGIC,.mesh_count=2,.total_vertices=5,
        .total_indices=6,.total_morph_deltas=10,.total_meshlets=2,
        .total_meshlet_vertices=16,.total_meshlet_triangles=36,
        .node_count=3,.skin_count=1,.anim_count=1,.total_joints=2,
        .total_channels=1,.total_floats=25};
}

/// V2 container, checksum and source coherency tests
static FILE *v2_image(uint64_t source, uint64_t abi) {
    FILE *f=tmpfile(); assert(f);
    assert(omdl_cache_v2_begin(f));
    OmdlHeader h=example();
    assert(omdl_cache_write(f,&h,sizeof h,1));
    uint64_t payload=(uint64_t)h.mesh_count*8 +
        (uint64_t)h.total_vertices*12 + (uint64_t)h.total_indices*4 +
        (uint64_t)h.total_morph_deltas*24 +
        (uint64_t)h.total_meshlets*(32+16+12) +
        (uint64_t)h.total_meshlet_vertices*4 +
        (uint64_t)h.total_meshlet_triangles +
        (uint64_t)h.node_count*80 + (uint64_t)h.skin_count*12 +
        (uint64_t)h.total_joints*(4+64) +
        (uint64_t)h.anim_count*76 +
        (uint64_t)h.total_channels*20 + (uint64_t)h.total_floats*4;
    unsigned char buffer[512]={0};
    while (payload) {
        size_t n=payload < sizeof buffer ? (size_t)payload : sizeof buffer;
        assert(omdl_cache_write(f,buffer,1,n));
        payload -= n;
    }
    assert(omdl_cache_v2_finish(f,source,abi));
    rewind(f);
    return f;
}

static void mutate(FILE *f, long offset, unsigned char value) {
    assert(fseek(f,offset,SEEK_SET)==0);
    assert(fputc(value,f)!=EOF);
    assert(fflush(f)==0);
}

static void test_v2(void) {
    const size_t extra[]={sizeof(void*), 4, 8};
    uint64_t abi=omdl_cache_abi_tag(sizes,extra,3);
    uint64_t source=UINT64_C(0x1234567890abcdef);
    FILE *f=v2_image(source,abi);
    OmdlLayout l={0};
    assert(omdl_cache_layout_v2(f,MAGIC,sizes,source,abi,&l));
    assert(l.offset[OMDL_MESHES]==OMDL_V2_ENVELOPE_BYTES+sizeof(OmdlHeader));
    assert(l.total_bytes>OMDL_V2_ENVELOPE_BYTES+sizeof(OmdlHeader));
    OmdlLayout sentinel=l;
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source+1,abi,&l));
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source,abi+1,&l));
    assert(memcmp(&l,&sentinel,sizeof l)==0);
    fclose(f);
    assert(omdl_cache_digest_bytes(0,"123456789",9)==UINT64_C(0x6c40df5f0b497347));
    assert(omdl_cache_digest_size(123,456)!=omdl_cache_digest_size(123,457));
    assert(omdl_cache_abi_tag(sizes,extra,3)!=omdl_cache_abi_tag(sizes,extra,2));
    f=v2_image(source,abi);
    /* Corrupt the payload itself, not its checksum. */
    mutate(f,OMDL_V2_ENVELOPE_BYTES+sizeof(OmdlHeader)+5,0x7f);
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source,abi,&l));
    fclose(f);
    f=v2_image(source,abi);
    mutate(f,8,3); /* incompatible version */
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source,abi,&l));
    fclose(f);
    f=v2_image(source,abi);
    mutate(f,48,1); /* nonzero reserved field */
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source,abi,&l));
    fclose(f);
    f=v2_image(source,abi);
    assert(fseek(f,0,SEEK_END)==0 && fputc(0x2a,f)!=EOF);
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source,abi,&l));
    fclose(f);
    f=v2_image(source,abi);
    assert(fseek(f,-1,SEEK_END)==0);
    long end=ftell(f);
    assert(end>0);
    /* C23/portable ftruncate not required: create a shortened copy. */
    FILE *shorter=tmpfile(); assert(shorter);
    assert(fseek(f,0,SEEK_SET)==0);
    while (ftell(f)<end) {
        int c=fgetc(f); assert(c!=EOF);
        assert(fputc(c,shorter)!=EOF);
    }
    assert(!omdl_cache_layout_v2(shorter,MAGIC,sizes,source,abi,&l));
    fclose(shorter);
    fclose(f);
    f=v2_image(source,abi);
    /* Even with an updated checksum, an inconsistent native section table
     * is rejected before the renderer sees the geometry. */
    mutate(f,OMDL_V2_ENVELOPE_BYTES+4,0x7f);
    assert(omdl_cache_v2_finish(f,source,abi));
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source,abi,&l));
    fclose(f);
    f=image(example());
    assert(!omdl_cache_layout_v2(f,MAGIC,sizes,source,abi,&l)); /* legacy */
    fclose(f);
    char pathname[]="/tmp/obsidian-omdl-source-XXXXXX";
    int fd=mkstemp(pathname); assert(fd >= 0);
    assert(write(fd,"original",8)==8);
    close(fd);
    uint64_t original=0, modified=0;
    assert(omdl_cache_digest_file(pathname,&original));
    fd=open(pathname, O_WRONLY); assert(fd >= 0);
    assert(write(fd,"changed!",8)==8);
    close(fd);
    assert(omdl_cache_digest_file(pathname,&modified));
    assert(original != modified);
    assert(unlink(pathname)==0);
    uint64_t untouched=original;
    assert(!omdl_cache_digest_file(pathname,&untouched) && untouched==original);
    for (unsigned i=0;i<2000;++i) {
        f=v2_image(source+i,abi);
        assert(omdl_cache_layout_v2(f,MAGIC,sizes,source+i,abi,&l));
        fclose(f);
    }
}

/// Regression cases
int main(void) {
    test_v2();
    static const unsigned char digest_source[] = {0x7f, 0x00, 0x41, 0x42, 0xff};
    char digest_path[] = "/tmp/obsidian-omdl-digest-XXXXXX";
    int digest_fd = mkstemp(digest_path);
    assert(digest_fd >= 0);
    FILE *digest_file = fdopen(digest_fd, "wb");
    assert(digest_file);
    assert(fwrite(digest_source, 1, sizeof digest_source, digest_file) ==
           sizeof digest_source);
    assert(fclose(digest_file) == 0);
    uint64_t disk_digest = 0;
    assert(omdl_cache_digest_file(digest_path, &disk_digest));
    assert(unlink(digest_path) == 0);
    uint64_t retained_bytes_digest =
        omdl_cache_digest_bytes(0, digest_source, sizeof digest_source);
    retained_bytes_digest =
        omdl_cache_digest_size(retained_bytes_digest, sizeof digest_source);
    assert(disk_digest == retained_bytes_digest);
    FILE *f=image(example());
    OmdlLayout l={0};
    assert(omdl_cache_layout(f,MAGIC,sizes,&l));
    assert(l.offset[OMDL_MESHES]==sizeof(OmdlHeader));
    assert(l.bytes[OMDL_MESHES]==16);
    assert(l.offset[OMDL_VERTICES]==sizeof(OmdlHeader)+16);
    assert(l.offset[OMDL_NODES]>l.offset[OMDL_MESHLET_TRIANGLES]);
    assert(l.total_bytes>sizeof(OmdlHeader));
    unsigned char mesh[16];
    assert(omdl_cache_read(f,&l,OMDL_MESHES,mesh));
    assert(!omdl_cache_read(f,&l,OMDL_MESHES,NULL));
    assert(omdl_cache_range(2,3,5));
    assert(!omdl_cache_range(UINT32_MAX,2,UINT32_MAX));
    assert(!omdl_cache_range(3,3,5));
    OmdlLayout sentinel=l;
    /* File with garbage trailer must be a cache miss, with output untouched. */
    assert(fseek(f,0,SEEK_END)==0);
    assert(fputc(0x7f,f)!=EOF);
    assert(!omdl_cache_layout(f,MAGIC,sizes,&l));
    assert(memcmp(&l,&sentinel,sizeof l)==0);
    fclose(f);
    f=image(example());
    assert(fseek(f,-1,SEEK_END)==0);
    long end=ftell(f); assert(end>=0);
    /* Rebuild to a shorter copy; no Linux-only truncate dependency. */
    FILE *short_file=tmpfile(); assert(short_file);
    rewind(f);
    for (long i=0;i<end;i++) {
        int c=fgetc(f); assert(c!=EOF);
        assert(fputc(c,short_file)!=EOF);
    }
    assert(!omdl_cache_layout(short_file,MAGIC,sizes,&l));
    fclose(short_file); fclose(f);
    OmdlHeader broken=example(); broken.magic=0;
    f=image(broken); assert(!omdl_cache_layout(f,MAGIC,sizes,&l)); fclose(f);
    broken=example(); broken.total_joints=0; broken.skin_count=0;
    f=image(broken); assert(omdl_cache_layout(f,MAGIC,sizes,&l)); fclose(f);
    broken=example(); broken.skin_count=0;
    f=image(broken); assert(!omdl_cache_layout(f,MAGIC,sizes,&l)); fclose(f);
    broken=example(); broken.anim_count=0;
    f=image(broken); assert(!omdl_cache_layout(f,MAGIC,sizes,&l)); fclose(f);
    f=image(example());
    for (size_t i=0;i<OMDL_SECTION_COUNT;i++) {
        size_t sz[OMDL_SECTION_COUNT]; memcpy(sz,sizes,sizeof sz);
        sz[i]=0;
        assert(!omdl_cache_layout(f,MAGIC,sz,&l));
    }
    fclose(f);
    for (uint32_t i=0; i<20000;i++) {
        OmdlHeader h=example();
        h.total_indices=(i * 2654435761u) % 40;
        h.node_count=(i*13u)%12;
        f=image(h);
        OmdlLayout v;
        assert(omdl_cache_layout(f,MAGIC,sizes,&v));
        assert(v.offset[OMDL_INDICES]<=v.offset[OMDL_MORPHS]);
        assert(v.bytes[OMDL_INDICES]==(uint64_t)h.total_indices*4);
        fclose(f);
    }
    f=tmpfile(); assert(f);
    assert(!omdl_cache_write(f,NULL,4,1));
    assert(!omdl_cache_write(f,"data",SIZE_MAX,2));
    assert(omdl_cache_write(f,NULL,1,0));
    assert(omdl_cache_write(f,"abcd",1,4));
    rewind(f); unsigned char b[4];
    assert(fread(b,1,4,f)==4 && memcmp(b,"abcd",4)==0);
    fclose(f);
    puts("omdl_cache: digest identity, 20,000 layouts, truncation and I/O PASS");
}
