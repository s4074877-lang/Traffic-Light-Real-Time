ARTIFACT = local_intersection_controller
PLATFORM ?= x86_64
BUILD_PROFILE ?= debug
OUTPUT_DIR = build/$(PLATFORM)-$(BUILD_PROFILE)
TARGET = $(OUTPUT_DIR)/$(ARTIFACT)

CC = qcc -Vgcc_nto$(PLATFORM)
INCLUDES += -I../common
CCFLAGS_debug = -g -O0 -fno-builtin
CCFLAGS_release = -O2
CCFLAGS_coverage = -g -O0 -ftest-coverage -fprofile-arcs
CCFLAGS_profile = -g -O0 -finstrument-functions
LDFLAGS_coverage = -ftest-coverage -fprofile-arcs
LIBS_profile = -lprofilingS
BUILD_FLAGS = -std=gnu11 -Wall -Wextra $(CCFLAGS_$(BUILD_PROFILE)) $(CCFLAGS)

# Each source file owns one responsibility; QNX IPC calls are in local_process.c.
SOURCES = $(wildcard src/*.c)
OBJECTS = $(patsubst src/%.c,$(OUTPUT_DIR)/%.o,$(SOURCES))
DEPENDS = $(OBJECTS:.o=.d)

.PHONY: all clean rebuild
all: $(TARGET)

$(OUTPUT_DIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(INCLUDES) $(BUILD_FLAGS) -MMD -MP -c $< -o $@

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS_$(BUILD_PROFILE)) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBS_$(BUILD_PROFILE)) $(LIBS)

clean:
	rm -f $(OBJECTS) $(DEPENDS) $(TARGET)

rebuild:
	$(MAKE) clean
	$(MAKE) all

-include $(DEPENDS)
