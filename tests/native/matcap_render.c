#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { WIDTH = 256, HEIGHT = 256 };

static void render(const char *executable, const char *path, bool matcap) {
  char command[1024];
  int length = snprintf(
      command, sizeof(command),
      "%s --headless-raw %s --width %d --height %d%s",
      executable, path, WIDTH, HEIGHT, matcap ? " --test-matcap" : "");
  assert(length > 0 && (size_t)length < sizeof(command));
  assert(system(command) == 0);
}

static FILE *open_capture(char path[]) {
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  FILE *file = fopen(path, "rb");
  assert(file);
  return file;
}

int main(int argc, char **argv) {
  const char *executable = argc > 1 ? argv[1] : "./obsidian";
  char pbr_path[] = "/tmp/obsidian-pbr-XXXXXX";
  char matcap_path[] = "/tmp/obsidian-matcap-XXXXXX";
  FILE *pbr = open_capture(pbr_path);
  FILE *matcap = open_capture(matcap_path);
  fclose(pbr);
  fclose(matcap);

  render(executable, pbr_path, false);
  render(executable, matcap_path, true);
  pbr = fopen(pbr_path, "rb");
  matcap = fopen(matcap_path, "rb");
  assert(pbr && matcap);
  size_t changed = 0;
  for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; ++i) {
    uint8_t a[4], b[4];
    assert(fread(a, sizeof(a), 1, pbr) == 1);
    assert(fread(b, sizeof(b), 1, matcap) == 1);
    if (abs((int)a[0] - b[0]) + abs((int)a[1] - b[1]) +
            abs((int)a[2] - b[2]) >
        12)
      ++changed;
  }
  assert(fgetc(pbr) == EOF && fgetc(matcap) == EOF);
  fclose(pbr);
  fclose(matcap);
  unlink(pbr_path);
  unlink(matcap_path);
  assert(changed > 100);
  printf("PBR versus studio matcap: %zu visibly changed pixels PASS\n",
         changed);
  return 0;
}
