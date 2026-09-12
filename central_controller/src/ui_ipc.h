#ifndef CENTRAL_UI_IPC_H
#define CENTRAL_UI_IPC_H

#include <stddef.h>

/* This is a private, local-node interface between Central and its display.
 * It does not change the shared Local/Train protocol. */
#ifndef CENTRAL_UI_SERVICE
#ifdef CENTRAL_SERVICE
#define CENTRAL_UI_SERVICE CENTRAL_SERVICE "_ui"
#else
#define CENTRAL_UI_SERVICE "traffic_central_controller_ui"
#endif
#endif

#define CENTRAL_UI_REQUEST_SIZE 256
#define CENTRAL_UI_RESPONSE_SIZE 16384
#define CENTRAL_UI_TIMEOUT_MS 500

typedef int (*central_ui_callback_t)(const char *request, char *output,
                                    size_t capacity, void *context);

struct central_ui_server_state;
typedef struct {
    struct central_ui_server_state *state;
} central_ui_server_t;

/* The callback runs in one dedicated receiver thread. It must copy snapshots
 * under the core lock, render outside that lock, and never wait on peer IPC,
 * filesystem I/O, or terminal I/O. The returned text must be NUL terminated. */
int central_ui_server_start(central_ui_server_t *server, const char *name,
                            central_ui_callback_t callback, void *context);
void central_ui_server_stop(central_ui_server_t *server);
/* Stops and joins the receiver before releasing its callback context. */
void central_ui_server_destroy(central_ui_server_t *server);

/* A failed/expired request may have reached the core. Never automatically
 * resend a mutating command; inspect command history and reported status. */
int central_ui_client_request(const char *name, const char *request,
                              char *output, size_t capacity, int *status);

#endif
