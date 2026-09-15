#include "local_process.h"

#include <sys/wait.h>

/* The launcher owns only the children it starts, never existing controllers. */
static void stop_children(pid_t *children, int count) {
    int i, attempt;
    for (i = 0; i < count; ++i) kill(children[i], SIGTERM);
    for (attempt = 0; attempt < 30; ++attempt) {
        int alive = 0;
        for (i = 0; i < count; ++i) {
            if (children[i] <= 0) continue;
            if (waitpid(children[i], NULL, WNOHANG) == children[i]) children[i] = 0;
            else alive = 1;
        }
        if (!alive) return;
        usleep(100000);
    }
    for (i = 0; i < count; ++i) {
        if (children[i] <= 0) continue;
        kill(children[i], SIGKILL);
        while (waitpid(children[i], NULL, 0) == -1 && errno == EINTR) {}
    }
}

static int start_child(pid_t *child, const char *role, connection_mode_t mode,
                       int id, const char *service, const char *endpoint) {
    int attempt;
    *child = fork();
    if (*child == -1) return -1;
    if (*child == 0) {
        int result = local_process_run(role, mode, (uint8_t)id, service, 0);
        _exit(result);
    }
    for (attempt = 0; attempt < 30 && !local_stopping; ++attempt) {
        int coid = open_service(endpoint,
                                !strcmp(role, "comm") && mode == CONN_MODE_GLOBAL);
        if (coid != -1) {
            name_close(coid);
            return 0;
        }
        usleep(100000);
    }
    fprintf(stderr, "Could not start %s for I%d\n", role, id + 1);
    return -1;
}

int run_launcher(connection_mode_t mode) {
    pid_t children[NUM_INTERSECTIONS * 2];
    char services[NUM_INTERSECTIONS][LOCAL_SERVICE_NAME_MAX];
    char cores[NUM_INTERSECTIONS][LOCAL_SERVICE_NAME_MAX + 8];
    int count = 0, result = EXIT_FAILURE, i;
    name_attach_t *lock = attach_service("traffic_local_launcher", 0);
    if (!lock) {
        fprintf(stderr, "Local launcher already running or service unavailable.\n");
        return EXIT_FAILURE;
    }
    for (i = 0; i < NUM_INTERSECTIONS; ++i) {
        int coid;
        snprintf(services[i], sizeof(services[i]), "traffic_local_I%d", i + 1);
        snprintf(cores[i], sizeof(cores[i]), "%s_core", services[i]);
        coid = open_service(cores[i], 0);
        if (coid == -1) coid = open_service(services[i], mode == CONN_MODE_GLOBAL);
        if (coid != -1) {
            name_close(coid);
            fprintf(stderr, "I%d already running. Stop existing Local processes first.\n", i + 1);
            goto cleanup;
        }
    }
    puts("Starting six Local controllers and communication processes...");
    fflush(stdout);
    for (i = 0; i < NUM_INTERSECTIONS && !local_stopping; ++i) {
        int started = start_child(&children[count], "core", mode, i, services[i], cores[i]);
        if (children[count] > 0) ++count;
        if (started != 0) goto cleanup;
        started = start_child(&children[count], "comm", mode, i, services[i], services[i]);
        if (children[count] > 0) ++count;
        if (started != 0) goto cleanup;
    }
    if (!local_stopping) result = run_ui(cores[0], I1, 1);
cleanup:
    stop_children(children, count);
    name_detach(lock, 0);
    puts("Local launcher stopped.");
    return result;
}
