PLATFORM ?= x86_64
CC = qcc -Vgcc_nto$(PLATFORM)
CFLAGS ?= -std=c11 -D_QNX_SOURCE -Wall -Wextra -Werror -g
OUTPUT_DIR ?= build/$(PLATFORM)
TARGET = $(OUTPUT_DIR)/protocol_monitor_test
TRANSPORT_TARGET = $(OUTPUT_DIR)/transport_test
COMMANDS_TARGET = $(OUTPUT_DIR)/commands_test
CENTRAL_TEST_TARGET = $(OUTPUT_DIR)/central_integration_test
CENTRAL_TARGET = $(OUTPUT_DIR)/central_controller_test
TRANSPORT_SOURCES = ../common/communication/connection.c ../common/communication/send.c ../common/communication/receive.c
TRANSPORT_HEADERS = ../common/communication/connection.h ../common/communication/send.h ../common/communication/receive.h ../common/common.h ../common/protocol.h
CENTRAL_SOURCES = ../central_controller/src/central_controller.c ../central_controller/src/monitor.c ../central_controller/src/commands.c
COMMON_HEADERS = $(wildcard ../common/*.h ../common/communication/*.h)

.PHONY: all test
all: $(TARGET) $(TRANSPORT_TARGET) $(COMMANDS_TARGET) $(CENTRAL_TEST_TARGET) $(CENTRAL_TARGET)

$(TARGET) $(TRANSPORT_TARGET) $(COMMANDS_TARGET) $(CENTRAL_TEST_TARGET) $(CENTRAL_TARGET): $(COMMON_HEADERS)

$(TARGET): protocol_monitor_test.c ../central_controller/src/monitor.c ../central_controller/src/monitor.h ../common/protocol.h
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ protocol_monitor_test.c ../central_controller/src/monitor.c

$(TRANSPORT_TARGET): transport_test.c $(TRANSPORT_SOURCES) $(TRANSPORT_HEADERS)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ transport_test.c $(TRANSPORT_SOURCES)

$(COMMANDS_TARGET): commands_test.c ../central_controller/src/commands.c ../central_controller/src/commands.h ../common/protocol.h
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -o $@ commands_test.c ../central_controller/src/commands.c

$(CENTRAL_TEST_TARGET): central_integration_test.c integration_services.h $(TRANSPORT_SOURCES) $(TRANSPORT_HEADERS)
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -include integration_services.h -o $@ central_integration_test.c $(TRANSPORT_SOURCES)

$(CENTRAL_TARGET): $(CENTRAL_SOURCES) integration_services.h $(TRANSPORT_SOURCES) $(TRANSPORT_HEADERS) ../central_controller/src/monitor.h ../central_controller/src/commands.h
	@mkdir -p $(OUTPUT_DIR)
	$(CC) $(CFLAGS) -include integration_services.h -o $@ $(CENTRAL_SOURCES) $(TRANSPORT_SOURCES)

test: all
	./$(TARGET)
	./$(TRANSPORT_TARGET)
	./$(COMMANDS_TARGET)
	./$(CENTRAL_TEST_TARGET) ./$(CENTRAL_TARGET)
