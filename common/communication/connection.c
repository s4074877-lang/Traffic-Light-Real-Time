#include "connection.h"
#include <string.h>
#include <stdio.h>
#include <getopt.h>

#define LOCAL_NAME_PREFIX  "/dev/name/local/"

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

    // Build full path - use local namespace
    snprintf(conn->service_path, sizeof(conn->service_path), "%s%s", LOCAL_NAME_PREFIX, service_name);
}

void connection_init_remote(connection_t *conn, const char *service_name,
                            const char *remote_vm, pthread_mutex_t *mutex) {
    conn->coid = -1;
    conn->connected = 0;
    conn->connection_msg_printed = 0;
    conn->mode = CONN_MODE_GLOBAL;
    conn->mutex = mutex;

    // Store service name
    strncpy(conn->service_name, service_name, sizeof(conn->service_name) - 1);
    conn->service_name[sizeof(conn->service_name) - 1] = '\0';

    // Build full path using remote VM: /net/{vm}/dev/name/local/{service}
    snprintf(conn->service_path, sizeof(conn->service_path),
             "/net/%s/dev/name/local/%s", remote_vm, service_name);
}

int connection_try_connect(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    int is_connected = conn->connected;
    pthread_mutex_unlock(conn->mutex);

    if (is_connected) {
        return 0;  // Already connected
    }

    int coid = name_open(conn->service_path, 0);
    if (coid != -1) {
        pthread_mutex_lock(conn->mutex);
        conn->coid = coid;
        conn->connected = 1;
        conn->connection_msg_printed = 0;
        pthread_mutex_unlock(conn->mutex);
        return 1;  // Newly connected
    }

    // Connection failed - silently retry
    return 0;  // Not connected
}

void connection_close(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    if (conn->coid != -1) {
        name_close(conn->coid);
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
    // Always register in LOCAL namespace
    // For global mode, other VMs connect via /net/{this_vm}/dev/name/local/{service}
    name_attach_t *attach = name_attach(NULL, service_name, 0);
    if (attach == NULL) {
        fprintf(stderr, "Failed to register with name service: %s\n", strerror(errno));
        return NULL;
    }

    printf("Registered as '%s' in local namespace\n", service_name);
    printf("Local access: %s%s\n", LOCAL_NAME_PREFIX, service_name);
    if (mode == CONN_MODE_GLOBAL) {
        printf("Remote access: /net/{this_vm}/dev/name/local/%s\n", service_name);
    }
    fflush(stdout);

    return attach;
}

void connection_unregister_service(name_attach_t *attach) {
    if (attach != NULL) {
        name_detach(attach, 0);
    }
}

connection_mode_t connection_parse_args(int argc, char *argv[]) {
    connection_mode_t mode = CONN_MODE_LOCAL;  // Default to local
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
