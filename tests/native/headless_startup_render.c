#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { WIDTH = 256, HEIGHT = 256 };

int main(int argc, char **argv) {
  const char *executable = argc > 1 ? argv[1] : "./obsidian";
  const bool antialiasing_test =
      argc > 2 && strcmp(argv[2], "--test-antialiasing") == 0;
  char path[] = "/tmp/obsidian-startup-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);

  char command[512];
  const bool validate = getenv("OBSIDIAN_TEST_VULKAN_VALIDATION") != NULL;
  char log_path[] = "/tmp/obsidian-validation-XXXXXX";
  int log_fd = -1;
  if (validate) {
    assert(setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1) == 0);
    assert(setenv("VK_LAYER_VALIDATE_SYNC", "1", 1) == 0);
    assert(setenv("OBSIDIAN_VK_DEBUG_LABELS", "1", 1) == 0);
    log_fd = mkstemp(log_path);
    assert(log_fd >= 0);
    close(log_fd);
  }
  int n = snprintf(command, sizeof(command),
                   "%s --headless-raw %s --width %d --height %d%s%s%s",
                   executable, path, WIDTH, HEIGHT,
                   antialiasing_test ? " --frames 4 --test-antialiasing" : "",
                   validate ? " > " : "", validate ? log_path : "");
  if (validate && n > 0 && (size_t)n < sizeof(command)) {
    size_t used = (size_t)n;
    n = snprintf(command + used, sizeof(command) - used, " 2>&1");
    assert(n > 0 && (size_t)n < sizeof(command) - used);
  }
  assert(n > 0 && (size_t)n < sizeof(command));
  assert(system(command) == 0);
  if (validate) {
    FILE *log = fopen(log_path, "r");
    assert(log);
    char line[2048];
    bool labels_enabled = false;
    while (fgets(line, sizeof(line), log)) {
      assert(strstr(line, "Validation Error:") == NULL);
      assert(strstr(line, "Vulkan debug object naming failed") == NULL);
      if (strstr(line, "Vulkan command labels enabled (VK_EXT_debug_utils)"))
        labels_enabled = true;
    }
    assert(labels_enabled);
    fclose(log);
    unlink(log_path);
  }

  FILE *image = fopen(path, "rb");
  assert(image);
  size_t foreground = 0;
  uint8_t center[4] = {0};
  for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; ++i) {
    uint8_t rgba[4];
    assert(fread(rgba, sizeof(rgba), 1, image) == 1);
    if (rgba[0] > 8 || rgba[1] > 8 || rgba[2] > 8)
      foreground++;
    if (i == (size_t)(HEIGHT / 2) * WIDTH + WIDTH / 2)
      for (size_t c = 0; c < sizeof(center); ++c)
        center[c] = rgba[c];
  }
  assert(fgetc(image) == EOF);
  fclose(image);
  unlink(path);

  /* A centered 2-unit startup cube occupies far more than this even at 256². */
  assert(foreground > 1000);
  /* Other editor overlays can be non-background; the center must be cube. */
  assert(center[0] > 8 || center[1] > 8 || center[2] > 8);
  assert(center[3] == 255);
  printf(
      "headless startup cube: center RGBA=(%u,%u,%u,%u), %zu visible pixels\n",
      center[0], center[1], center[2], center[3], foreground);

  char no_sun_path[] = "/tmp/obsidian-no-sun-XXXXXX";
  fd = mkstemp(no_sun_path);
  assert(fd >= 0);
  close(fd);
  char no_sun_log_path[] = "/tmp/obsidian-no-sun-log-XXXXXX";
  int no_sun_log_fd = -1;
  if (validate) {
    no_sun_log_fd = mkstemp(no_sun_log_path);
    assert(no_sun_log_fd >= 0);
    close(no_sun_log_fd);
  }
  n = snprintf(command, sizeof(command),
               "%s --headless-raw %s --width %d --height %d --test-no-sun%s%s%s",
               executable, no_sun_path, WIDTH, HEIGHT,
               validate ? " > " : "", validate ? no_sun_log_path : "",
               validate ? " 2>&1" : "");
  assert(n > 0 && (size_t)n < sizeof(command));
  assert(system(command) == 0);
  if (validate) {
    FILE *log = fopen(no_sun_log_path, "r");
    assert(log);
    char line[2048];
    while (fgets(line, sizeof(line), log)) {
      assert(strstr(line, "Validation Error:") == NULL);
      assert(strstr(line, "Vulkan debug object naming failed") == NULL);
    }
    fclose(log);
    unlink(no_sun_log_path);
  }

  image = fopen(no_sun_path, "rb");
  assert(image);
  foreground = 0;
  memset(center, 0, sizeof(center));
  for (size_t i = 0; i < (size_t)WIDTH * HEIGHT; ++i) {
    uint8_t rgba[4];
    assert(fread(rgba, sizeof(rgba), 1, image) == 1);
    if (rgba[0] > 8 || rgba[1] > 8 || rgba[2] > 8)
      foreground++;
    if (i == (size_t)(HEIGHT / 2) * WIDTH + WIDTH / 2)
      memcpy(center, rgba, sizeof(center));
  }
  assert(fgetc(image) == EOF);
  fclose(image);
  unlink(no_sun_path);
  assert(foreground > 1000);
  assert(center[0] > 8 || center[1] > 8 || center[2] > 8);
  assert(center[3] == 255);
  puts("headless zero-sun scene: lit fallback renders without directional or shadow work PASS");
  return 0;
}
