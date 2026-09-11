#ifndef CONNECTION_H
#define CONNECTION_H

#include <sys/dispatch.h>
#include <pthread.h>
#include "../common.h"

// Connection mode
typedef enum {
    CONN_MODE_LOCAL = 0,   // Single VM - uses /dev/name/local/
    CONN_MODE_GLOBAL = 1   // Multi VM - uses /dev/name/global/
} connection_mode_t;

// Connection state structure
typedef struct {
    int coid;                    // Connection ID
    int connected;               // Connection status
    int connection_msg_printed;  // For logging
    char service_path[128];      // Full service path to connect to
    char service_name[64];       // Service name only
    connection_mode_t mode;      // Local or global mode
    pthread_mutex_t *mutex;      // Pointer to shared mutex
    pthread_mutex_t io_mutex;    // Serializes sends and connection closure
    uint64_t generation;        // Changes whenever the connection is replaced
    int connecting;
} connection_t;

// Initialize connection structure
// service_name: just the name (e.g., "traffic_local_controller")
// mode: CONN_MODE_LOCAL or CONN_MODE_GLOBAL
void connection_init(connection_t *conn, const char *service_name,
                     connection_mode_t mode, pthread_mutex_t *mutex);

// Try to establish a connection from a dedicated connection thread.
// Name-service discovery may block; no state mutex is held during discovery.
// Returns: 1 if newly connected, 0 if already connected or failed
int connection_try_connect(connection_t *conn);

// Close connection
void connection_close(connection_t *conn);

// Close only the connection on which a failed operation was attempted.
void connection_close_generation(connection_t *conn, uint64_t generation);
uint64_t connection_generation(connection_t *conn);

// Call after all threads using the connection have been joined.
void connection_destroy(connection_t *conn);

// Check if connected
int connection_is_connected(connection_t *conn);

// Get a snapshot of the connection ID; use send_message() for actual sends.
int connection_get_coid(connection_t *conn);

// Register this controller with name service
// service_name: just the name (e.g., "traffic_local_controller")
// mode: CONN_MODE_LOCAL or CONN_MODE_GLOBAL
// Returns: name_attach_t pointer or NULL on failure
name_attach_t* connection_register_service(const char *service_name, connection_mode_t mode);

// Unregister service
void connection_unregister_service(name_attach_t *attach);

// Parse command line for -l (local) or -g (global) flag
// Returns: CONN_MODE_LOCAL or CONN_MODE_GLOBAL (default)
connection_mode_t connection_parse_args(int argc, char *argv[]);

// Get mode string for display
const char* connection_mode_str(connection_mode_t mode);

#endif // CONNECTION_H
