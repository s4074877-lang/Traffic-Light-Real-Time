PLATFORM ?= x86_64
CC = qcc -Vgcc_nto$(PLATFORM)
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O0 -g
OUTPUT_DIR = ../build/tests-$(PLATFORM)
TARGET = $(OUTPUT_DIR)/local_logic_test
INPUT_TARGET = $(OUTPUT_DIR)/local_input_test

LOCAL_SOURCES = ../src/local_state.c \
                ../src/local_config.c \
                ../src/local_traffic.c \
                ../src/local_pedestrian.c \
                ../src/local_simulation.c \
                ../src/local_commands.c \
                ../src/local_ui_commands.c \
                ../src/local_ipc.c

HEADERS = ../src/local_controller.h \
          ../src/local_process.h \
          ../../common/common.h \
          ../../common/protocol.h \
          ../../common/communication/connection.h

.PHONY: all run clean

all: $(TARGET) $(INPUT_TARGET)

$(OUTPUT_DIR):
	mkdir -p $@

$(TARGET): local_logic_test.c $(LOCAL_SOURCES) $(HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -DENABLE_DEMO_COMMANDS=1 -DENABLE_TRAFFIC_SIMULATION=1 -o $@ local_logic_test.c $(LOCAL_SOURCES)

$(INPUT_TARGET): local_input_test.c $(LOCAL_SOURCES) $(HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -DENABLE_DEMO_COMMANDS=1 -DENABLE_TRAFFIC_SIMULATION=1 -o $@ local_input_test.c $(LOCAL_SOURCES)

run: all
	$(TARGET)
	$(INPUT_TARGET)

clean:
	rm -f $(TARGET) $(INPUT_TARGET)
