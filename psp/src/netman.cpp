#include "netman.h"
#include "debuglog.h"

#include <psptypes.h>
#include <pspnet.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspwlan.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_netconf.h>
#include <pspthreadman.h>
#include <psphttp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

/*
 * PSP network manager implementation, see netman.h.
 *
 * WiFi check uses sceWlanGetSwitchState(); network initialisation
 * loads the PSP net modules via sceUtilityLoadNetModule();
 * connect() shows the system netconf dialog for AP selection and
 * polls until the link is up; http_download() is a minimal
 * HTTP/1.0 GET over raw sockets.
 */

/* ---- internal state ---- */

static bool infra_ready = false;
static bool connected   = false;

/* ---- WiFi hardware check ---- */

bool NetMan::wifi_available()
{
    /* sceWlanGetSwitchState:
     *   0 = switch off (or no hardware)
     *   1 = switch on, not connected
     *   2 = switch on, connected  */
    int state = sceWlanGetSwitchState();
    return state != 0;
}

/* ---- Module loading / infrastructure ---- */

bool NetMan::init()
{
    if (infra_ready)
        return true;

    int rc;

    rc = sceNetInit(0x20000, 0x20, 0x1000, 0x20, 0x1000);
    if (rc < 0) {
        dbglog("NetMan: sceNetInit failed (0x%08x)\n", rc);
        return false;
    }

    rc = sceNetInetInit();
    if (rc < 0) { dbglog("NetMan: sceNetInetInit failed (0x%08x)\n", rc); return false; }

    rc = sceNetApctlInit(0x1800, 0x42);
    if (rc < 0) { dbglog("NetMan: sceNetApctlInit failed (0x%08x)\n", rc); return false; }

    /* Load the user-mode net modules for infrastructure WiFi. */
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (rc < 0) { dbglog("NetMan: load COMMON failed (0x%08x)\n", rc); return false; }
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (rc < 0) { dbglog("NetMan: load INET failed (0x%08x)\n", rc); return false; }
    rc = sceUtilityLoadNetModule(PSP_NET_MODULE_HTTP);
    if (rc < 0) { dbglog("NetMan: load HTTP failed (0x%08x)\n", rc); return false; }

    infra_ready = true;
    return true;
}

void NetMan::shutdown()
{
    if (!infra_ready)
        return;

    disconnect();

    sceUtilityUnloadNetModule(PSP_NET_MODULE_HTTP);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);

    sceNetApctlTerm();
    sceNetInetTerm();
    sceNetTerm();

    infra_ready = false;
}

/* ---- AP connection via system dialog ---- */

bool NetMan::connect()
{
    if (connected)
        return true;
    if (!infra_ready)
        return false;

    /* Show the system netconf dialog for AP selection.
     * PSP_NETCONF_ACTION_CONNECTAP lets the user pick a configured
     * access point.  The dialog runs through the utility subsystem. */
    pspUtilityNetconfData data;
    memset(&data, 0, sizeof(data));
    data.base.size = sizeof(data);
    data.base.language = 1;       /* English */
    data.base.buttonSwap = 0;
    data.base.graphicsThread = 0x11;
    data.base.accessThread = 0x13;
    data.base.fontThread = 0x12;
    data.base.soundThread = 0x14;
    data.action = PSP_NETCONF_ACTION_CONNECTAP;

    int rc = sceUtilityNetconfInitStart(&data);
    if (rc < 0) {
        dbglog("NetMan: netconf dialog init failed (0x%08x)\n", rc);
        return false;
    }

    /* Drive the dialog to completion. */
    bool accepted = false;
    int loop_count = 0;
    const int max_loops = 6000;  /* ~60 seconds at 10ms per loop */
    for (;;) {
        int status = sceUtilityNetconfGetStatus();
        if (status == PSP_UTILITY_DIALOG_NONE) {
            /* Dialog not yet visible; keep waiting. */
        } else if (status == PSP_UTILITY_DIALOG_VISIBLE) {
            sceUtilityNetconfUpdate(1);
        } else if (status == PSP_UTILITY_DIALOG_QUIT) {
            sceUtilityNetconfShutdownStart();
            accepted = true;
            break;
        } else if (status == PSP_UTILITY_DIALOG_FINISHED) {
            break;
        } else if (status < 0) {
            dbglog("NetMan: netconf dialog error (status=%d)\n", status);
            break;
        }
        if (++loop_count >= max_loops) {
            dbglog("NetMan: dialog timeout\n");
            sceUtilityNetconfShutdownStart();
            break;
        }
        sceKernelDelayThread(10000);  /* 10 ms */
    }

    if (!accepted) {
        dbglog("NetMan: AP selection cancelled or failed\n");
        return false;
    }

    /* Poll the AP state until the link reaches "obtained IP"
     * (state 4) or an error. */
    for (int attempt = 0; attempt < 300; ++attempt) {  /* ~30 s */
        int state = 0;
        sceNetApctlGetState(&state);
        if (state == 4) {
            /* Connected: read and log the IP address. */
            SceNetApctlInfo info;
            memset(&info, 0, sizeof(info));
            sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info);
            connected = true;
            return true;
        }
        if (state == 0) {
            return false;
        }
        sceKernelDelayThread(100000);  /* 100 ms */
    }

    dbglog("NetMan: connection timeout\n");
    return false;
}

void NetMan::disconnect()
{
    if (!connected)
        return;

    sceNetApctlDisconnect();
    connected = false;
}

/* ---- HTTP download using sceHttp ---- */

int NetMan::http_download(const char * url, uint8_t * buf, int buf_size)
{
    if (!connected || buf == nullptr || buf_size <= 0)
        return -1;

    int rc = sceHttpInit(20000);
    if (rc < 0) {
        dbglog("NetMan: sceHttpInit failed (0x%08x)\n", rc);
        return -1;
    }

    /* Init HTTPS too (required by some PSP implementations). */
    sceHttpsInit(0, 0, 0, 0);

    int tmpl = sceHttpCreateTemplate((char *)"Vector06C-PSP/1.0", 1, 0);
    if (tmpl < 0) {
        dbglog("NetMan: create template failed (0x%08x)\n", tmpl);
        return -1;
    }

    /* Set timeouts. */
    sceHttpSetResolveTimeOut(tmpl, 5000000);  /* 5 sec */
    sceHttpSetConnectTimeOut(tmpl, 10000000); /* 10 sec */
    sceHttpSetRecvTimeOut(tmpl, 30000000);    /* 30 sec */

    int conn = sceHttpCreateConnectionWithURL(tmpl, url, 0);
    if (conn < 0) {
        dbglog("NetMan: create connection failed (0x%08x)\n", conn);
        sceHttpDeleteTemplate(tmpl);
        return -1;
    }

    int req = sceHttpCreateRequestWithURL(conn, PSP_HTTP_METHOD_GET, (char *)url, 0);
    if (req < 0) {
        dbglog("NetMan: create request failed (0x%08x)\n", req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return -1;
    }

    rc = sceHttpSendRequest(req, nullptr, 0);
    if (rc < 0) {
        dbglog("NetMan: send request failed (0x%08x)\n", rc);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return -1;
    }

    /* Read response data. */
    int body_len = 0;
    uint8_t tmp[4096];
    int n = sceHttpReadData(req, tmp, sizeof(tmp));
    if (n < 0) {
        dbglog("NetMan: read data failed (0x%08x)\n", n);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return -1;
    }

    /* Check status code. */
    int status = 0;
    rc = sceHttpGetStatusCode(req, &status);
    if (rc < 0 || status != 200) {
        dbglog("NetMan: HTTP error %d\n", status);
        sceHttpDeleteRequest(req);
        sceHttpDeleteConnection(conn);
        sceHttpDeleteTemplate(tmpl);
        return -1;
    }

    /* Copy first chunk. */
    if (n > 0) {
        int copy = n;
        if (copy > buf_size) copy = buf_size;
        memcpy(buf, tmp, copy);
        body_len = copy;
    }

    /* Read remaining data. */
    while (body_len < buf_size) {
        int chunk = buf_size - body_len;
        if (chunk > 8192) chunk = 8192;
        n = sceHttpReadData(req, buf + body_len, chunk);
        if (n <= 0)
            break;
        body_len += n;
    }

    /* Cleanup. */
    sceHttpDeleteRequest(req);
    sceHttpDeleteConnection(conn);
    sceHttpDeleteTemplate(tmpl);
    sceHttpsEnd();
    sceHttpEnd();

    return body_len;
}
