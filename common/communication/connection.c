#include "connection.h"
#include <string.h>
#include <stdio.h>
#include <getopt.h>

#define LOCAL_NAME_PREFIX  "/dev/name/local/"
#define GLOBAL_NAME_PREFIX "/dev/name/global/"

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

    // Build full path based on mode
    const char *prefix = (mode == CONN_MODE_LOCAL) ? LOCAL_NAME_PREFIX : GLOBAL_NAME_PREFIX;
    snprintf(conn->service_path, sizeof(conn->service_path), "%s%s", prefix, service_name);
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
    int flags = (mode == CONN_MODE_GLOBAL) ? NAME_FLAG_ATTACH_GLOBAL : 0;

    name_attach_t *attach = name_attach(NULL, service_name, flags);
    if (attach == NULL) {
        fprintf(stderr, "Failed to register with name service: %s\n", strerror(errno));
        if (mode == CONN_MODE_GLOBAL) {
            fprintf(stderr, "For global mode, make sure GNS is running:\n");
            fprintf(stderr, "  Central VM: gns -s\n");
            fprintf(stderr, "  Other VMs:  gns -c /net/<central-vm>/dev/name/gns\n");
        }
        return NULL;
    }

    const char *prefix = (mode == CONN_MODE_LOCAL) ? LOCAL_NAME_PREFIX : GLOBAL_NAME_PREFIX;
    printf("Registered as '%s' in %s namespace\n",
           service_name,
           (mode == CONN_MODE_LOCAL) ? "local" : "global");
    printf("Accessible via: %s%s\n", prefix, service_name);
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
