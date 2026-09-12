PLATFORM ?= x86_64
CC = qcc -Vgcc_nto$(PLATFORM)
CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -O2
OUTPUT_DIR = ../build/tests-$(PLATFORM)
COMMON_HEADERS = ../../common/common.h ../../common/protocol.h
IPC_SOURCES = ../src/ipc.c ../src/commands.c
IPC_HEADERS = ../src/ipc.h ../src/commands.h
CORE_EXTRA = ../src/operator_policy.c ../src/ui_ipc.c
CORE_HEADERS = ../src/operator_policy.h ../src/ui_ipc.h ../src/version.h
REAL_TRAIN_SOURCES = ../../train_controller/src/ui/train_ui.c \
    ../../train_controller/src/crossing/crossing.c ../../train_controller/src/crossing/rail_sim.c \
    ../../train_controller/src/crossing/crossing_test.c ../../common/communication/connection.c \
    ../../common/communication/send.c ../../common/communication/receive.c
REAL_TRAIN_HEADERS = $(wildcard ../../train_controller/src/ui/*.h ../../train_controller/src/crossing/*.h ../../common/communication/*.h)

.PHONY: all run clean
all: $(OUTPUT_DIR)/monitor_test $(OUTPUT_DIR)/commands_test $(OUTPUT_DIR)/ipc_test $(OUTPUT_DIR)/integration_test $(OUTPUT_DIR)/central_fixture $(OUTPUT_DIR)/global_transport_test $(OUTPUT_DIR)/operator_policy_test $(OUTPUT_DIR)/ui_ipc_test $(OUTPUT_DIR)/central_features_test $(OUTPUT_DIR)/real_train_test $(OUTPUT_DIR)/real_train_fixture

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

$(OUTPUT_DIR)/global_transport_test: global_transport_test.c $(IPC_SOURCES) $(IPC_HEADERS) $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ global_transport_test.c $(IPC_SOURCES)

$(OUTPUT_DIR)/central_fixture: ../src/central_controller.c ../src/monitor.c ../src/monitor.h $(IPC_SOURCES) $(IPC_HEADERS) $(COMMON_HEADERS) $(CORE_EXTRA) $(CORE_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -DCENTRAL_SERVICE=\"traffic_test_central_only\" -DCENTRAL_LOCAL_SERVICE=\"traffic_test_central_only_local\" -DCENTRAL_TRAIN_SERVICE=\"traffic_test_central_only_train\" -o $@ ../src/central_controller.c ../src/monitor.c $(IPC_SOURCES) $(CORE_EXTRA)

$(OUTPUT_DIR)/operator_policy_test: operator_policy_test.c ../src/operator_policy.c ../src/operator_policy.h $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ operator_policy_test.c ../src/operator_policy.c

$(OUTPUT_DIR)/ui_ipc_test: ui_ipc_test.c ../src/ui_ipc.c ../src/ui_ipc.h | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ ui_ipc_test.c ../src/ui_ipc.c

$(OUTPUT_DIR)/central_features_test: central_features_test.c $(IPC_SOURCES) $(IPC_HEADERS) ../src/ui_ipc.c ../src/ui_ipc.h $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ central_features_test.c $(IPC_SOURCES) ../src/ui_ipc.c

$(OUTPUT_DIR)/real_train_test: real_train_test.c $(IPC_SOURCES) $(IPC_HEADERS) ../src/ui_ipc.c ../src/ui_ipc.h $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ real_train_test.c $(IPC_SOURCES) ../src/ui_ipc.c

# Compile teammate sources unchanged, retaining their existing warning policy.
$(OUTPUT_DIR)/real_train_fixture: real_train_fixture.c ../../train_controller/src/train_controller.c $(REAL_TRAIN_SOURCES) $(REAL_TRAIN_HEADERS) $(COMMON_HEADERS) | $(OUTPUT_DIR)
	$(CC) -std=gnu11 -Wall -O0 -g -o $@ real_train_fixture.c $(REAL_TRAIN_SOURCES)

run: all
	$(OUTPUT_DIR)/monitor_test
	$(OUTPUT_DIR)/commands_test
	$(OUTPUT_DIR)/ipc_test
	$(OUTPUT_DIR)/integration_test $(OUTPUT_DIR)/central_fixture
	$(OUTPUT_DIR)/operator_policy_test
	$(OUTPUT_DIR)/ui_ipc_test
	$(OUTPUT_DIR)/central_features_test $(OUTPUT_DIR)/central_fixture
	$(OUTPUT_DIR)/real_train_test $(OUTPUT_DIR)/central_fixture $(OUTPUT_DIR)/real_train_fixture

clean:
	rm -f $(OUTPUT_DIR)/monitor_test $(OUTPUT_DIR)/commands_test $(OUTPUT_DIR)/ipc_test $(OUTPUT_DIR)/integration_test $(OUTPUT_DIR)/central_fixture $(OUTPUT_DIR)/global_transport_test $(OUTPUT_DIR)/operator_policy_test $(OUTPUT_DIR)/ui_ipc_test $(OUTPUT_DIR)/central_features_test $(OUTPUT_DIR)/real_train_test $(OUTPUT_DIR)/real_train_fixture
