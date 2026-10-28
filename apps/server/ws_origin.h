#ifndef MYGAMES_DEV_WS_ORIGIN_H
#define MYGAMES_DEV_WS_ORIGIN_H

#include <stdbool.h>

/*
 * Development gateway only (127.0.0.1). A browser's non-null Origin must
 * name a loopback HTTP(S) page, with optional canonical TCP port.
 *
 * NULL is accepted for CLI/native WebSocket clients that do not send Origin;
 * it is NOT proof of identity. Public services require authenticated
 * principals, TLS/WSS and server-side authorization in addition to Origin.
 */
bool mygames_dev_ws_origin_allowed(const char *origin);

#endif
