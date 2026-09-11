#include "connection.h"
#include <string.h>
#include <stdio.h>
#include <getopt.h>

#define LOCAL_NAME_PREFIX  "/dev/name/local/"
#define GLOBAL_NAME_PREFIX "/dev/name/global/"

void connection_init(connection_t *conn, const char *service_name,
                     connection_mode_t mode, pthread_mutex_t *mutex) {
    memset(conn, 0, sizeof(*conn));
    conn->coid = -1;
    conn->mode = mode;
    conn->mutex = mutex;
    pthread_mutex_init(&conn->io_mutex, NULL);

    // Store service name
    strncpy(conn->service_name, service_name, sizeof(conn->service_name) - 1);
    conn->service_name[sizeof(conn->service_name) - 1] = '\0';

    // Build full path based on mode
    const char *prefix = (mode == CONN_MODE_LOCAL) ? LOCAL_NAME_PREFIX : GLOBAL_NAME_PREFIX;
    snprintf(conn->service_path, sizeof(conn->service_path), "%s%s", prefix, service_name);
}

static void cancel_discovery(void *arg) {
    connection_t *conn = (connection_t *)arg;
    pthread_mutex_lock(conn->mutex);
    conn->connecting = 0;
    pthread_mutex_unlock(conn->mutex);
}

int connection_try_connect(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    if (conn->connected || conn->connecting) {
        pthread_mutex_unlock(conn->mutex);
        return 0;
    }
    conn->connecting = 1;
    uint64_t generation = conn->generation;
    pthread_mutex_unlock(conn->mutex);

    int coid;
    int flags = conn->mode == CONN_MODE_GLOBAL ? NAME_FLAG_ATTACH_GLOBAL : 0;
    pthread_cleanup_push(cancel_discovery, conn);
    coid = name_open(conn->service_name, flags);
    pthread_cleanup_pop(0);

    pthread_mutex_lock(conn->mutex);
    conn->connecting = 0;
    if (coid != -1 && conn->generation == generation && !conn->connected) {
        conn->coid = coid;
        conn->connected = 1;
        conn->generation++;
        conn->connection_msg_printed = 0;
        pthread_mutex_unlock(conn->mutex);
        return 1;
    }

    int print_waiting = coid == -1 && !conn->connection_msg_printed;
    if (print_waiting) {
        conn->connection_msg_printed = 1;
    }
    pthread_mutex_unlock(conn->mutex);

    if (coid != -1) {
        name_close(coid);
    } else if (print_waiting) {
        char ts[32];
        get_timestamp(ts, sizeof(ts));
        printf("%s[%s] Waiting connection to %s%s\n",
               COLOR_YELLOW, ts, conn->service_path, COLOR_RESET);
        fflush(stdout);
    }
    return 0;
}

static void close_connection(connection_t *conn, uint64_t generation, int check_generation) {
    pthread_mutex_lock(&conn->io_mutex);
    pthread_mutex_lock(conn->mutex);
    if (check_generation && conn->generation != generation) {
        pthread_mutex_unlock(conn->mutex);
        pthread_mutex_unlock(&conn->io_mutex);
        return;
    }
    int coid = conn->coid;
    conn->coid = -1;
    conn->connected = 0;
    conn->connection_msg_printed = 0;
    conn->generation++;
    pthread_mutex_unlock(conn->mutex);

    if (coid != -1) {
        name_close(coid);
    }
    pthread_mutex_unlock(&conn->io_mutex);
}

void connection_close(connection_t *conn) {
    close_connection(conn, 0, 0);
}

void connection_close_generation(connection_t *conn, uint64_t generation) {
    close_connection(conn, generation, 1);
}

uint64_t connection_generation(connection_t *conn) {
    pthread_mutex_lock(conn->mutex);
    uint64_t generation = conn->generation;
    pthread_mutex_unlock(conn->mutex);
    return generation;
}

void connection_destroy(connection_t *conn) {
    connection_close(conn);
    pthread_mutex_destroy(&conn->io_mutex);
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

    // One receive thread replies inline, so clients may time out independently.
    // _NTO_CHF_UNBLOCK would keep a reply-blocked client waiting for this server.
    int chid = ChannelCreate(_NTO_CHF_DISCONNECT);
    if (chid == -1) {
        return NULL;
    }
    dispatch_t *dispatch = dispatch_create_channel(chid, 0);
    if (dispatch == NULL) {
        ChannelDestroy(chid);
        return NULL;
    }
    name_attach_t *attach = name_attach(dispatch, service_name, flags);
    if (attach == NULL) {
        int saved_errno = errno;
        dispatch_destroy(dispatch);
        errno = saved_errno;
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
