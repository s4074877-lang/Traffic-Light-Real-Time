#include "connection.h"
#include "../common.h"
#include <string.h>
#include <stdio.h>
#include <getopt.h>
#include <fcntl.h>
#include <sys/stat.h>

// Get remote VM name for a service (returns NULL if not configured or local mode)
static const char* get_remote_vm(const char *service_name) {
    if (strcmp(service_name, LOCAL_SERVICE_NAME) == 0) {
        return (LOCAL_VM_NAME[0] != '\0') ? LOCAL_VM_NAME : NULL;
    } else if (strcmp(service_name, TRAIN_SERVICE_NAME) == 0) {
        return (TRAIN_VM_NAME[0] != '\0') ? TRAIN_VM_NAME : NULL;
    } else if (strcmp(service_name, CENTRAL_SERVICE_NAME) == 0) {
        return (CENTRAL_VM_NAME[0] != '\0') ? CENTRAL_VM_NAME : NULL;
    }
    return NULL;
}

void connection_init(connection_t *conn, const char *service_name,
                     connection_mode_t mode, pthread_mutex_t *mutex) {
    conn->coid = -1;
    conn->connected = 0;
    conn->connection_msg_printed = 0;
    conn->mode = mode;
    conn->mutex = mutex;

    // Store service name
    strncpy(conn->service_name, service_name, sizeof(conn->service_name) - 1);
    conn->service_name[sizeof(conn->service_name) - 1] = '\0';

    // Build connection path based on mode
    if (mode == CONN_MODE_LOCAL) {
        // LOCAL MODE (-l): Always use /tmp/connection/ on same VM
        // No VM names needed - all controllers on same machine
        snprintf(conn->service_path, sizeof(conn->service_path),
                 "%s/%s", CONNECTION_DIR, service_name);
    } else {
        // GLOBAL MODE (-g): Use /net/<vm>/tmp/connection/ for remote VMs
        const char *vm_name = get_remote_vm(service_name);
        if (vm_name != NULL) {
            // Remote VM via Qnet
            snprintf(conn->service_path, sizeof(conn->service_path),
                     "/net/%s%s/%s", vm_name, CONNECTION_DIR, service_name);
        } else {
            // VM name not configured - fall back to local path
            snprintf(conn->service_path, sizeof(conn->service_path),
                     "%s/%s", CONNECTION_DIR, service_name);
        }
    }
}

int connection_try_connect(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    int is_connected = conn->connected;
    int msg_printed = conn->connection_msg_printed;
    pthread_mutex_unlock(conn->mutex);

    if (is_connected) {
        return 0;  // Already connected
    }

    // Print connection attempt once
    if (!msg_printed) {
        printf("Connecting to: %s\n", conn->service_path);
        fflush(stdout);
        pthread_mutex_lock(conn->mutex);
        conn->connection_msg_printed = 1;
        pthread_mutex_unlock(conn->mutex);
    }

    // Try open() first (works with symlinks and Qnet paths)
    int coid = open(conn->service_path, O_RDWR);
    if (coid == -1) {
        // Fallback: try name_open for direct service name
        coid = name_open(conn->service_name, 0);
    }

    if (coid != -1) {
        pthread_mutex_lock(conn->mutex);
        conn->coid = coid;
        conn->connected = 1;
        pthread_mutex_unlock(conn->mutex);
        printf("Connected to: %s\n", conn->service_path);
        fflush(stdout);
        return 1;  // Newly connected
    }

    // Connection failed - silently retry
    return 0;  // Not connected
}

void connection_close(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    if (conn->coid != -1) {
        close(conn->coid);  // close() works for both open() and name_open()
        conn->coid = -1;
    }
    conn->connected = 0;
    conn->connection_msg_printed = 0;
    pthread_mutex_unlock(conn->mutex);
}

int connection_is_connected(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    int connected = conn->connected;
    pthread_mutex_unlock(conn->mutex);
    return connected;
}

int connection_get_coid(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    int coid = conn->coid;
    pthread_mutex_unlock(conn->mutex);
    return coid;
}

name_attach_t* connection_register_service(const char *service_name, connection_mode_t mode) {
    // Ensure connection directory exists
    mkdir(CONNECTION_DIR, 0755);

    // Always use LOCAL name service (no GNS required)
    // Cross-VM access works via Qnet + symlinks
    name_attach_t *attach = name_attach(NULL, service_name, 0);
    if (attach == NULL) {
        fprintf(stderr, "Failed to register service: %s\n", strerror(errno));
        return NULL;
    }

    // Create symlink: /tmp/connection/<service> -> /dev/name/local/<service>
    // This allows access via /net/<vm>/tmp/connection/<service>
    char symlink_path[256];
    char target_path[256];
    snprintf(symlink_path, sizeof(symlink_path), "%s/%s", CONNECTION_DIR, service_name);
    snprintf(target_path, sizeof(target_path), "/dev/name/local/%s", service_name);

    // Remove old symlink if exists, then create new one
    unlink(symlink_path);
    if (symlink(target_path, symlink_path) == -1) {
        fprintf(stderr, "Warning: Could not create symlink %s: %s\n",
                symlink_path, strerror(errno));
    }

    printf("Registered: /dev/name/local/%s\n", service_name);
    printf("Symlink:    %s/%s\n", CONNECTION_DIR, service_name);
    if (mode == CONN_MODE_GLOBAL) {
        printf("Qnet path:  /net/<this-vm>%s/%s\n", CONNECTION_DIR, service_name);
    }
    printf("Mode: %s (no GNS required)\n", connection_mode_str(mode));
    fflush(stdout);

    return attach;
}

void connection_unregister_service(name_attach_t *attach) {
    if (attach != NULL) {
        name_detach(attach, 0);
    }
}

connection_mode_t connection_parse_args(int argc, char *argv[]) {
    connection_mode_t mode = CONN_MODE_GLOBAL;  // Default to global
    int opt;

    // Reset getopt
    optind = 1;

    while ((opt = getopt(argc, argv, "lg")) != -1) {
        switch (opt) {
            case 'l':
                mode = CONN_MODE_LOCAL;
                break;
            case 'g':
                mode = CONN_MODE_GLOBAL;
                break;
            default:
                break;
        }
    }

    return mode;
}

const char* connection_mode_str(connection_mode_t mode) {
    return (mode == CONN_MODE_LOCAL) ? "LOCAL" : "GLOBAL";
}
