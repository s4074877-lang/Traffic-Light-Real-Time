#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// Protocol version
#define PROTOCOL_VERSION 1

// Message types
typedef enum {
    MSG_TEST = 1,           // Test message between controllers
    MSG_HEARTBEAT = 2       // Keep-alive (future use)
} msg_type_t;

// Controller types
typedef enum {
    CONTROLLER_LOCAL = 1,
    CONTROLLER_TRAIN = 2,
    CONTROLLER_CENTRAL = 3
} controller_type_t;

// Message header
typedef struct {
    uint16_t type;          // msg_type_t
    uint16_t src;           // Source controller (controller_type_t)
    uint16_t dst;           // Destination controller (controller_type_t)
    uint16_t reserved;      // Padding
    char timestamp[32];     // HH:MM:SS format
} msg_header_t;

// Test message payload
typedef struct {
    msg_header_t header;
    char data[64];          // Optional message data
} test_message_t;

// Reply structure
typedef struct {
    int8_t status;          // 0 = success, -1 = error
    char timestamp[32];     // Reply timestamp
} reply_t;

#endif // PROTOCOL_H
