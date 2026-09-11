PLATFORM ?= x86_64
CC = qcc -Vgcc_nto$(PLATFORM)
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2
OUTPUT_DIR = ../build/tests-$(PLATFORM)
COMMON_HEADERS = ../../common/common.h ../../common/protocol.h
IPC_SOURCES = ../src/ipc.c ../src/commands.c
IPC_HEADERS = ../src/ipc.h ../src/commands.h

.PHONY: all run clean
all: $(OUTPUT_DIR)/monitor_test $(OUTPUT_DIR)/commands_test $(OUTPUT_DIR)/ipc_test $(OUTPUT_DIR)/integration_test $(OUTPUT_DIR)/central_fixture

$(OUTPUT_DIR):
	mkdir -p $@

$(OUTPUT_DIR)/monitor_test: monitor_test.c ../src/monitor.c ../src/monitor.h $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ monitor_test.c ../src/monitor.c

$(OUTPUT_DIR)/commands_test: commands_test.c ../src/commands.c ../src/commands.h $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ commands_test.c ../src/commands.c

$(OUTPUT_DIR)/ipc_test: ipc_test.c $(IPC_SOURCES) $(IPC_HEADERS) $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ ipc_test.c $(IPC_SOURCES)

$(OUTPUT_DIR)/integration_test: integration_test.c $(IPC_SOURCES) $(IPC_HEADERS) $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ integration_test.c $(IPC_SOURCES)

$(OUTPUT_DIR)/central_fixture: ../src/central_controller.c ../src/monitor.c ../src/monitor.h $(IPC_SOURCES) $(IPC_HEADERS) $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -DCENTRAL_SERVICE=\"traffic_test_central_only\" -DCENTRAL_LOCAL_SERVICE=\"traffic_test_central_only_local\" -DCENTRAL_TRAIN_SERVICE=\"traffic_test_central_only_train\" -o $@ ../src/central_controller.c ../src/monitor.c $(IPC_SOURCES)

run: all
	$(OUTPUT_DIR)/monitor_test
	$(OUTPUT_DIR)/commands_test
	$(OUTPUT_DIR)/ipc_test
	$(OUTPUT_DIR)/integration_test $(OUTPUT_DIR)/central_fixture

clean:
	rm -f $(OUTPUT_DIR)/monitor_test $(OUTPUT_DIR)/commands_test $(OUTPUT_DIR)/ipc_test $(OUTPUT_DIR)/integration_test $(OUTPUT_DIR)/central_fixture
