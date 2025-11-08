CC = gcc
CFLAGS = -std=c23 -I/usr/include/freetype2 -Wall -Wextra -g3 -O3 -fPIC
LDFLAGS = -lvulkan -lglfw -lcglm -lm -lfreetype -lfontconfig
GLSLANG = glslangValidator
XXD = xxd

# Paths
INCLUDES = -I/usr/include -I.
LIBRARIES = -L/usr/lib/x86_64-linux-gnu
INSTALL_DIR = /usr

# Project settings
LIB_NAME = libobsidian
TEST_EXECUTABLE = obsidian

# Auto-detect all sources and headers
LIB_SOURCES = $(filter-out main.c, $(wildcard *.c))
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)
TEST_SOURCES = main.c
TEST_OBJECTS = $(TEST_SOURCES:.c=.o)
HEADERS = $(wildcard *.h)

# Shaders
SHADER_VERTS = $(wildcard *.vert)
SHADER_FRAGS = $(wildcard *.frag)
SHADER_SPVS = $(SHADER_VERTS:.vert=.vert.spv) $(SHADER_FRAGS:.frag=.frag.spv)
SPV_HEADERS = $(SHADER_SPVS:.spv=.spv.h)

# Default target
all: $(SPV_HEADERS) $(LIB_NAME).a $(LIB_NAME).so $(TEST_EXECUTABLE)

# Compile shaders to SPIR-V
%.vert.spv: %.vert
	$(GLSLANG) -V $< -o $@

%.frag.spv: %.frag
	$(GLSLANG) -V $< -o $@

# Convert SPIR-V to C header
%.spv.h: %.spv
	$(XXD) -i $< > $@

# Library object files (with -fPIC)
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Static library
$(LIB_NAME).a: $(SPV_HEADERS) $(LIB_OBJECTS)
	ar rcs $@ $(LIB_OBJECTS)

# Dynamic library
$(LIB_NAME).so: $(SPV_HEADERS) $(LIB_OBJECTS)
	$(CC) -shared -o $@ $(LIB_OBJECTS) $(LDFLAGS)

# Test executable (links against the static library)
$(TEST_EXECUTABLE): $(TEST_OBJECTS) $(LIB_NAME).a
	$(CC) $(TEST_OBJECTS) -o $@ -L. -lobsidian $(LDFLAGS)

# Alternative: build test executable using object files directly
$(TEST_EXECUTABLE)-direct: $(SPV_HEADERS) $(LIB_OBJECTS) $(TEST_OBJECTS)
	$(CC) $(LIB_OBJECTS) $(TEST_OBJECTS) -o $(TEST_EXECUTABLE) $(LDFLAGS)

# Installation (only installs library, not test executable)
install: $(LIB_NAME).a $(LIB_NAME).so
	install -d $(INSTALL_DIR)/lib
	install -m 644 $(LIB_NAME).a $(INSTALL_DIR)/lib
	install -m 755 $(LIB_NAME).so $(INSTALL_DIR)/lib
	install -d $(INSTALL_DIR)/include/obsidian
	install -m 644 $(HEADERS) $(INSTALL_DIR)/include/obsidian
	if [ -n "$(SPV_HEADERS)" ]; then \
		install -m 644 $(SPV_HEADERS) $(INSTALL_DIR)/include/obsidian; \
	fi
	ldconfig

# Uninstall
uninstall:
	rm -f $(INSTALL_DIR)/lib/$(LIB_NAME).a
	rm -f $(INSTALL_DIR)/lib/$(LIB_NAME).so
	rm -rf $(INSTALL_DIR)/include/obsidian
	ldconfig

# Clean
clean:
	rm -f $(LIB_OBJECTS) $(TEST_OBJECTS) *.spv *.spv.h $(LIB_NAME).a $(LIB_NAME).so $(TEST_EXECUTABLE)

.PHONY: all install uninstall clean
