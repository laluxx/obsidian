CC = gcc
CFLAGS = -std=c23 -I/usr/include/freetype2 -Wall -Wextra -g3 -o3 
LDFLAGS = -lvulkan -lglfw -lcglm -lm -lfreetype -lfontconfig
GLSLANG = glslangValidator
XXD = xxd

# For Linux
INCLUDES = -I/usr/include
LIBRARIES = -L/usr/lib/x86_64-linux-gnu

# Project settings
TARGET = obsidian
SOURCES = $(wildcard *.c)
OBJECTS = $(SOURCES:.c=.o)

# Shaders
SHADER_VERTS = $(wildcard *.vert)
SHADER_FRAGS = $(wildcard *.frag)
SHADER_SPVS = $(SHADER_VERTS:.vert=.vert.spv) $(SHADER_FRAGS:.frag=.frag.spv)
SPV_HEADERS = $(SHADER_SPVS:.spv=.spv.h)

# Compile shaders to SPIR-V
%.vert.spv: %.vert
	$(GLSLANG) -V $< -o $@

%.frag.spv: %.frag
	$(GLSLANG) -V $< -o $@

# Convert SPIR-V to C header
%.spv.h: %.spv
	$(XXD) -i $< > $@


all: $(TARGET)

# Compile shaders if they are newer
%.spv: %.vert
	$(GLSLANG) -V $< -o $@

%.spv: %.frag
	$(GLSLANG) -V $< -o $@

# Convert to C header (xxd -i)
%.spv.h: %.spv
	$(XXD) -i $< > $@

# Build executable
$(TARGET): $(SPV_HEADERS) $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LIBRARIES) $(LDFLAGS)


# Object files
%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(TARGET) *.o *.spv *.spv.h

.PHONY: all clean
