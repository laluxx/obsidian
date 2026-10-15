#define _POSIX_C_SOURCE 200809L
#include "headless_job.h"
#include "atomic_file.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint32_t count;
    uint32_t fail_at;
    uint32_t total;
    uint32_t final_count;
} Trace;

static bool fake_driver(uint32_t index, bool final, void *context) {
    Trace *trace = context;
    assert(index == trace->count++);
    if (final) {
        assert(index + 1 == trace->total);
        trace->final_count++;
    }
    return index != trace->fail_at;
}

static void read_exact(const char *path, char *buf, size_t length) {
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fread(buf, 1, length, file) == length);
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
}

int main(void) {
    uint32_t out = 77;
    assert(obs_headless_parse_frames("1", &out) && out == 1);
    assert(obs_headless_parse_frames("10000", &out) && out == 10000);
    for (const char *const *p = (const char *const[]){
        "", "0", "10001", "9999999999999999999999", "-1", "+1", "2 ",
        " 2", "12x", "１", NULL
    }; *p; ++p) {
        out = 42;
        assert(!obs_headless_parse_frames(*p, &out) && out == 42);
    }
    assert(!obs_headless_parse_frames(NULL, &out));
    assert(!obs_headless_parse_frames("5", NULL));
    for (uint32_t n = 1; n <= 128; ++n) {
        Trace trace = {.fail_at = UINT32_MAX, .total = n};
        uint32_t completed = UINT32_MAX;
        assert(obs_headless_execute_frames(n, fake_driver, &trace, &completed));
        assert(trace.count == n && completed == n && trace.final_count == 1);
        for (uint32_t fail = 0; fail < n; ++fail) {
            trace = (Trace){.fail_at = fail, .total = n};
            assert(!obs_headless_execute_frames(n, fake_driver, &trace, &completed));
            assert(trace.count == fail + 1 && completed == fail);
            assert(trace.final_count == (unsigned)(fail == n - 1));
        }
    }
    out = 55;
    assert(!obs_headless_execute_frames(0, fake_driver, NULL, &out) && out == 0);
    assert(!obs_headless_execute_frames(10001, fake_driver, NULL, &out));
    assert(!obs_headless_execute_frames(1, NULL, NULL, &out));

    char directory[] = "/tmp/obsidian-agent-XXXXXX";
    assert(mkdtemp(directory));
    char image[256], report[256], invalid[256], sample[256];
    snprintf(image, sizeof(image), "%s/frame.rgba", directory);
    snprintf(report, sizeof(report), "%s/report.json", directory);
    snprintf(invalid, sizeof(invalid), "%s/nonexistent/file", directory);
    snprintf(sample, sizeof(sample), "%s/quote-\"-\\-name.rgba", directory);
    const char old[] = "prior image content";
    assert(obs_atomic_write_bytes(image, old, sizeof(old)));
    char contents[128];
    read_exact(image, contents, sizeof(old));
    assert(memcmp(contents, old, sizeof(old)) == 0);
    assert(!obs_atomic_write_bytes(invalid, old, sizeof(old)));
    read_exact(image, contents, sizeof(old));
    assert(memcmp(contents, old, sizeof(old)) == 0);
    assert(!obs_atomic_write_bytes(image, NULL, 4));
    char raw[24] = {0};
    for (size_t i = 0; i < sizeof(raw); ++i) raw[i] = (char)(i * 7);
    assert(obs_atomic_write_bytes(image, raw, sizeof(raw)));
    read_exact(image, contents, sizeof(raw));
    assert(memcmp(contents, raw, sizeof(raw)) == 0);
    struct stat stat_info;
    assert(stat(image, &stat_info) == 0 && stat_info.st_size == (off_t)sizeof(raw));

    ObsAgentReceipt receipt = {
        .image_path = sample, .width = 2, .height = 3,
        .completed_frames = 19, .source = OBS_AGENT_SOURCE_FILE
    };
    assert(obs_headless_write_receipt(report, &receipt));
    FILE *f = fopen(report, "rb");
    assert(f);
    char json[1024];
    size_t size = fread(json, 1, sizeof(json) - 1, f);
    json[size] = 0;
    fclose(f);
    assert(strstr(json, "\"schema\":\"obsidian.headless.v1\""));
    assert(strstr(json, "\"status\":\"ok\""));
    assert(strstr(json, "\"bytes\":24"));
    assert(strstr(json, "\"frames\":19"));
    assert(strstr(json, "\"source\":\"file\""));
    assert(strstr(json, "quote-\\\"-\\\\-name.rgba"));
    receipt.width = 0;
    assert(!obs_headless_write_receipt(report, &receipt));
    receipt.width = 2;
    assert(!obs_headless_write_receipt(sample, &(ObsAgentReceipt){
        .image_path = sample, .width = 2, .height = 2,
        .completed_frames = 1, .source = OBS_AGENT_SOURCE_NONE
    }));
    assert(!obs_headless_write_receipt(invalid, &receipt));
    /* A failed receipt may not truncate the previous successful receipt. */
    read_exact(report, json, size);
    assert(unlink(report) == 0);
    assert(unlink(image) == 0);
    assert(rmdir(directory) == 0);
    puts("headless agent production driver / atomic output / receipt: PASS");
    return 0;
}
