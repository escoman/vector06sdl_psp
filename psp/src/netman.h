#pragma once

#include <inttypes.h>

/*
 * Network manager for the PSP Game Center.
 *
 * Provides WiFi connectivity and simple HTTP file downloads over
 * the PSP socket API.  The flow is:
 *
 *   1. wifi_available()  — hardware check (sceWlanGetSwitchState).
 *   2. init()            — load PSP net modules, start infra.
 *   3. connect()         — system AP selection dialog + connect.
 *   4. http_download()   — blocking GET into a caller buffer.
 *   5. disconnect()      — tear down the AP link.
 *   6. shutdown()        — unload modules, free resources.
 *
 * Every public method is idempotent: calling init() twice is safe,
 * shutdown() after shutdown() is a no-op.  Errors are reported as
 * false / negative return values; the caller decides how to present
 * them to the user.
 */
namespace NetMan {

/* True when the PSP hardware has a WiFi module and its physical
 * switch (or software toggle) is ON.  Always false on models
 * without WiFi (PSP Street / E1000). */
bool wifi_available();

/* Load the PSP network modules and initialise the infrastructure.
 * Returns true on success.  Safe to call more than once. */
bool init();

/* Show the system access-point selection dialog and connect.
 * Blocks until the link is up or an error occurs.
 * Returns true when the connection is ready for HTTP. */
bool connect();

/* Disconnect from the current AP and release the connection.
 * Safe to call when not connected. */
void disconnect();

/* Tear down the network infrastructure and unload modules.
 * Implies disconnect().  Safe to call more than once. */
void shutdown();

/* Download the body of an HTTP GET request for `url` into `buf`
 * (up to `buf_size` bytes).  Returns the number of body bytes
 * written, or -1 on any error.  The buffer is NOT null-terminated
 * by this function; the caller should use the return value. */
int http_download(const char * url, uint8_t * buf, int buf_size);

} // namespace NetMan
