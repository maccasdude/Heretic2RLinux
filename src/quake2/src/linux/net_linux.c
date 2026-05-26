//
// net_linux.c
//
// Copyright 1998 Raven Software
// Linux port 2026.
//
// POSIX/BSD-sockets equivalent of net_wins.c.
//
// Differences from the Windows version:
//
//   * WinSock startup/cleanup (WSAStartup/WSACleanup) are no-ops here; BSD
//     sockets need no init.
//   * Error codes come from `errno` rather than WSAGetLastError(). The
//     WSAEWOULDBLOCK check becomes EAGAIN/EWOULDBLOCK.
//   * `closesocket()` is just `close()`.
//   * The IPX code path is stubbed out: Linux kernels removed kernel-level
//     IPX support years ago, and even where it still exists it requires
//     special privileges. IPX never made sense for anyone reaching the
//     internet anyway. NET_OpenIPX() etc. become no-ops, and the rest of the
//     engine continues to work because it always checks `ipx_sockets[]` != 0
//     before sending.
//   * The H2 "net latency emulation" code that was in the loopback path is
//     retained verbatim.
//

#include "qcommon.h"
#include "Random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/ioctl.h>

// Match the Windows symbol names so the rest of the engine compiles cleanly.
#define WSAGetLastError()  errno
#define closesocket(s)     close(s)
#define INVALID_SOCKET     (-1)
#define SOCKET_ERROR       (-1)
typedef int SOCKET;

#define MAX_LOOPBACK	4

typedef struct
{
    byte data[MAX_MSGLEN];
    int datalen;
} loopmsg_t;

typedef struct // H2
{
    byte data[MAX_MSGLEN];
    int datalen;
    uint timestamp;
    qboolean is_free;
} loopmsg2_t;

typedef struct
{
    loopmsg_t msgs[MAX_LOOPBACK];
    int get;
    int send;
} loopback_t;

#define NUM_SOCKETS         3 //mxd
#define NUM_LOOPMESSAGES    20 //mxd

static loopback_t loopbacks[NUM_SOCKETS];
static loopmsg2_t loopmessages[NUM_SOCKETS][NUM_LOOPMESSAGES]; // H2
static int ip_sockets [NUM_SOCKETS];
static int ipx_sockets[NUM_SOCKETS];                            // stays zero on Linux

static cvar_t* net_shownet;
static cvar_t* noudp;
static cvar_t* noipx;
static cvar_t* ipxfix; // H2

// ============================================================================
// Address translation (no IPX)
// ============================================================================

static void NetadrToSockadr(const netadr_t* a, struct sockaddr* s)
{
    memset(s, 0, sizeof(struct sockaddr_in));
    switch (a->type) {
    case NA_BROADCAST: {
        struct sockaddr_in* s_in = (struct sockaddr_in*)s;
        s_in->sin_family       = AF_INET;
        s_in->sin_port         = a->port;
        s_in->sin_addr.s_addr  = INADDR_BROADCAST;
    } break;

    case NA_IP: {
        struct sockaddr_in* s_in = (struct sockaddr_in*)s;
        s_in->sin_family       = AF_INET;
        s_in->sin_addr.s_addr  = *(int*)&a->ip;
        s_in->sin_port         = a->port;
    } break;

    case NA_IPX:
    case NA_BROADCAST_IPX:
        // No IPX on Linux - leave the sockaddr cleared.
        break;

    default:
        break;
    }
}

static void SockadrToNetadr(struct sockaddr* s, netadr_t* a)
{
    if (s->sa_family == AF_INET) {
        const struct sockaddr_in* s_in = (struct sockaddr_in*)s;
        a->type = NA_IP;
        memcpy(a->ip, &s_in->sin_addr.s_addr, sizeof(s_in->sin_addr.s_addr));
        a->port = s_in->sin_port;
    }
    // Anything else: leave as-is (no IPX).
}

qboolean NET_CompareAdr(const netadr_t* a, const netadr_t* b)
{
    if (a->type != b->type) return false;
    switch (a->type) {
    case NA_LOOPBACK: return true;
    case NA_IP:       return (a->port == b->port && memcmp(a->ip, b->ip, 4) == 0);
    case NA_IPX:      return (a->port == b->port && memcmp(a->ipx, b->ipx, 10) == 0);
    default:          return false;
    }
}

qboolean NET_CompareBaseAdr(const netadr_t* a, const netadr_t* b)
{
    if (a->type != b->type) return false;
    switch (a->type) {
    case NA_LOOPBACK: return true;
    case NA_IP:       return (memcmp(a->ip, b->ip, 4) == 0);
    case NA_IPX:      return (memcmp(a->ipx, b->ipx, 10) == 0);
    default:          return false;
    }
}

static qboolean NET_StringToSockaddr(const char* s, struct sockaddr* sadr)
{
    memset(sadr, 0, sizeof(struct sockaddr_in));
    struct sockaddr_in* sa_in = (struct sockaddr_in*)sadr;

    sa_in->sin_family = AF_INET;
    sa_in->sin_port   = 0;

    char copy[128];
    strcpy_s(copy, sizeof(copy), s);

    // Strip trailing :port if present.
    for (char* colon = copy; *colon != 0; colon++) {
        if (*colon == ':') {
            *colon = 0;
            sa_in->sin_port = htons((uint16_t)Q_atoi(colon + 1));
            break;
        }
    }

    if (copy[0] >= '0' && copy[0] <= '9') {
        if (inet_pton(AF_INET, copy, &sa_in->sin_addr) != 1) return false;
    } else {
        struct addrinfo  hints;
        struct addrinfo* res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(copy, NULL, &hints, &res) != 0 || !res) return false;
        memcpy(&sa_in->sin_addr,
               &((struct sockaddr_in*)res->ai_addr)->sin_addr,
               sizeof(sa_in->sin_addr));
        freeaddrinfo(res);
    }
    return true;
}

char* NET_AdrToString(const netadr_t* a)
{
    static char s[64];

    switch (a->type) {
    case NA_LOOPBACK:
        Com_sprintf(s, sizeof(s), "loopback");
        break;
    case NA_IP:
        Com_sprintf(s, sizeof(s), "%i.%i.%i.%i:%i",
                    a->ip[0], a->ip[1], a->ip[2], a->ip[3], ntohs(a->port));
        break;
    default:
        Com_sprintf(s, sizeof(s),
                    "%02x%02x%02x%02x:%02x%02x%02x%02x%02x%02x:%i",
                    a->ipx[0], a->ipx[1], a->ipx[2], a->ipx[3],
                    a->ipx[4], a->ipx[5], a->ipx[6], a->ipx[7],
                    a->ipx[8], a->ipx[9], ntohs(a->port));
        break;
    }
    return s;
}

qboolean NET_StringToAdr(const char* s, netadr_t* a)
{
    struct sockaddr sadr;
    if (strcmp(s, "localhost") == 0) {
        memset(a, 0, sizeof(netadr_t));
        a->type = NA_LOOPBACK;
        return true;
    }
    if (NET_StringToSockaddr(s, &sadr)) {
        SockadrToNetadr(&sadr, a);
        return true;
    }
    return false;
}

static char* NET_ErrorString(void)
{
    return strerror(errno);
}

qboolean NET_IsLocalAddress(const netadr_t* a)
{
    return a->type == NA_LOOPBACK;
}

// ============================================================================
// LOOPBACK BUFFERS (verbatim from net_wins.c, with timeGetTime() rewritten)
// ============================================================================

static uint H2R_TimeMs(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint)((ts.tv_sec * 1000ll) + (ts.tv_nsec / 1000000ll));
}

static qboolean NET_GetLoopPacket(const netsrc_t sock, netadr_t* n_from, sizebuf_t* n_message)
{
    if (net_latency->value > 0.0f && net_latency->value < 2000.0f) {
        const uint time = H2R_TimeMs();
        loopmsg2_t* msg = &loopmessages[sock][0];

        for (int i = 0; i < NUM_LOOPMESSAGES; i++, msg++) {
            if (msg->is_free && msg->timestamp < time) {
                msg->is_free = false;
                memcpy(n_message->data, msg->data, msg->datalen);
                n_message->cursize = msg->datalen;
                memset(n_from, 0, sizeof(*n_from));
                n_from->type = NA_LOOPBACK;
                return true;
            }
        }
        return false;
    }

    loopback_t* loop = &loopbacks[sock];
    if (loop->send - loop->get > MAX_LOOPBACK) loop->get = loop->send - MAX_LOOPBACK;
    if (loop->get >= loop->send) return false;

    const int i = loop->get & (MAX_LOOPBACK - 1);
    loop->get++;

    memcpy(n_message->data, loop->msgs[i].data, loop->msgs[i].datalen);
    n_message->cursize = loop->msgs[i].datalen;
    memset(n_from, 0, sizeof(*n_from));
    n_from->type = NA_LOOPBACK;
    return true;
}

static void NET_SendLoopPacket(const netsrc_t sock, const int length, const void* data)
{
    if (net_latency->value > 0.0f && net_latency->value < 2000.0f) {
        const uint time = H2R_TimeMs();
        loopmsg2_t* msg = &loopmessages[sock ^ 1][0];

        for (int i = 0; i < NUM_LOOPMESSAGES; i++, msg++) {
            if (!msg->is_free) {
                msg->is_free = true;
                const float nl = net_latency->value;
                msg->timestamp = time + (int)(nl + flrand(-nl * 0.25f, nl * 0.25f));
                memcpy(msg, data, length);
                msg->datalen = length;
            }
        }
        return;
    }

    loopback_t* loop = &loopbacks[sock ^ 1];
    const int index = loop->send & (MAX_LOOPBACK - 1);
    loop->send++;
    memcpy(loop->msgs[index].data, data, length);
    loop->msgs[index].datalen = length;
}

// ============================================================================
// Real packet I/O
// ============================================================================

qboolean NET_GetPacket(const netsrc_t sock, netadr_t* n_from, sizebuf_t* n_message)
{
    if (NET_GetLoopPacket(sock, n_from, n_message)) {
        if (net_receiverate->value > 0.0f && net_receiverate->value < 1.0f
                && flrand(0.0f, 1.0f) > net_receiverate->value) {
            n_message->cursize = 0;
            return false;
        }
        return true;
    }

    // Try IP only (IPX is stubbed - the IPX socket array stays at 0).
    int net_socket = ip_sockets[sock];
    if (net_socket == 0) return false;

    struct sockaddr from;
    socklen_t fromlen = sizeof(from);
    const ssize_t ret = recvfrom(net_socket, n_message->data, n_message->maxsize, 0,
                                 &from, &fromlen);
    if (ret == -1) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            if ((int)dedicated->value)
                Com_Printf("NET_GetPacket: %s from %s\n",
                           NET_ErrorString(), NET_AdrToString(n_from));
            else
                Com_Error(ERR_DROP, "NET_GetPacket: %s from %s",
                          NET_ErrorString(), NET_AdrToString(n_from));
        }
        return false;
    }

    SockadrToNetadr(&from, n_from);
    if (ret == n_message->maxsize) {
        Com_Printf("Oversize packet from %s\n", NET_AdrToString(n_from));
        return false;
    }

    if (net_receiverate->value > 0.0f && net_receiverate->value < 1.0f
            && flrand(0.0f, 1.0f) > net_receiverate->value) {
        n_message->cursize = 0;
        return false;
    }
    n_message->cursize = (int)ret;
    return true;
}

void NET_SendPacket(const netsrc_t sock, const int length, const void* data, const netadr_t* to)
{
    struct sockaddr addr;
    int net_socket = 0;

    if (net_sendrate->value > 0.0f && net_sendrate->value < 1.0f
            && net_sendrate->value < flrand(0.0f, 1.0f)) return;

    switch (to->type) {
    case NA_LOOPBACK:
        NET_SendLoopPacket(sock, length, data);
        return;
    case NA_IP:
    case NA_BROADCAST:
        net_socket = ip_sockets[sock];
        break;
    case NA_IPX:
    case NA_BROADCAST_IPX:
        // IPX stubbed: nothing to do.
        return;
    default:
        Com_Error(ERR_FATAL, "NET_SendPacket: bad address type");
        return;
    }

    if (net_socket == 0) return;

    NetadrToSockadr(to, &addr);

    if (sendto(net_socket, data, length, 0, &addr, sizeof(addr)) == -1) {
        int err = errno;
        if (err == EAGAIN || err == EWOULDBLOCK) return;
        if (err == EADDRNOTAVAIL && (to->type == NA_BROADCAST || to->type == NA_BROADCAST_IPX))
            return;

        if ((int)dedicated->value)
            Com_Printf("NET_SendPacket ERROR: %s\n", NET_ErrorString());
        else if (err == EADDRNOTAVAIL)
            Com_DPrintf("NET_SendPacket Warning: %s : %s\n",
                        NET_ErrorString(), NET_AdrToString(to));
        else
            Com_Error(ERR_DROP, "NET_SendPacket ERROR: %s\n", NET_ErrorString());
    }
}

// ============================================================================
// Socket open / configure
// ============================================================================

static int NET_IPSocket(const char* net_interface, const int port)
{
    SOCKET newsocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (newsocket == INVALID_SOCKET) {
        if (errno != EAFNOSUPPORT)
            Com_Printf("WARNING: UDP_OpenSocket: socket: %s\n", NET_ErrorString());
        return 0;
    }

    // Non-blocking.
    int flags = fcntl(newsocket, F_GETFL, 0);
    if (flags == -1 || fcntl(newsocket, F_SETFL, flags | O_NONBLOCK) == -1) {
        Com_Printf("WARNING: UDP_OpenSocket: O_NONBLOCK: %s\n", NET_ErrorString());
        closesocket(newsocket);
        return 0;
    }

    // Broadcast capable.
    int optval = 1;
    if (setsockopt(newsocket, SOL_SOCKET, SO_BROADCAST,
                   (char*)&optval, sizeof(optval)) == -1) {
        Com_Printf("WARNING: UDP_OpenSocket: setsockopt SO_BROADCAST: %s\n",
                   NET_ErrorString());
        closesocket(newsocket);
        return 0;
    }

    // SO_REUSEADDR helps when the server restarts quickly.
    setsockopt(newsocket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));

    if (net_interface == NULL || net_interface[0] == 0
        || Q_stricmp(net_interface, "localhost") == 0) {
        address.sin_addr.s_addr = INADDR_ANY;
    } else {
        struct sockaddr_in tmp;
        if (NET_StringToSockaddr(net_interface, (struct sockaddr*)&tmp))
            address.sin_addr = tmp.sin_addr;
        else
            address.sin_addr.s_addr = INADDR_ANY;
    }

    address.sin_family = AF_INET;
    address.sin_port   = (port == PORT_ANY) ? 0 : htons((uint16_t)port);

    if (bind(newsocket, (struct sockaddr*)&address, sizeof(address)) == -1) {
        Com_Printf("WARNING: UDP_OpenSocket: bind: %s\n", NET_ErrorString());
        closesocket(newsocket);
        return 0;
    }
    return (int)newsocket;
}

static void NET_OpenIP(void)
{
    const cvar_t* ip = Cvar_Get("ip", "localhost", CVAR_NOSET);
    const qboolean is_dedicated = Cvar_IsSet("dedicated");

    if (ip_sockets[NS_SERVER] == 0) {
        int port = (int)Cvar_Get("ip_hostport", "0", CVAR_NOSET)->value;
        if (port == 0) {
            port = (int)Cvar_Get("hostport", "0", CVAR_NOSET)->value;
            if (port == 0)
                port = (int)Cvar_Get("port", va("%i", PORT_SERVER), CVAR_NOSET)->value;
        }
        ip_sockets[NS_SERVER] = NET_IPSocket(ip->string, port);
        if (is_dedicated && ip_sockets[NS_SERVER] == 0)
            Com_Error(ERR_FATAL, "Couldn't allocate dedicated server IP port");
    }

    if (is_dedicated) return;

    if (ip_sockets[NS_CLIENT] == 0) {
        int port = (int)Cvar_Get("ip_clientport", "0", CVAR_NOSET)->value;
        if (port == 0) {
            port = (int)Cvar_Get("clientport", va("%i", PORT_CLIENT), CVAR_NOSET)->value;
            if (port == 0) port = PORT_ANY;
        }
        ip_sockets[NS_CLIENT] = NET_IPSocket(ip->string, port);
        if (ip_sockets[NS_CLIENT] == 0)
            ip_sockets[NS_CLIENT] = NET_IPSocket(ip->string, PORT_ANY);
    }
}

// IPX is a no-op on Linux.
static void NET_OpenIPX(void) { (void)ipxfix; }

void NET_Config(const qboolean multiplayer)
{
    static qboolean old_config;
    if (old_config == multiplayer) return;
    old_config = multiplayer;

    if (multiplayer) {
        if (!(int)noudp->value) NET_OpenIP();
        if (!(int)noipx->value) NET_OpenIPX();
    } else {
        for (int i = 0; i < NUM_SOCKETS; i++) {
            if (ip_sockets[i]  != 0) { closesocket(ip_sockets[i]);  ip_sockets[i]  = 0; }
            if (ipx_sockets[i] != 0) { closesocket(ipx_sockets[i]); ipx_sockets[i] = 0; }
        }
    }
}

void NET_Init(void)
{
    Com_Printf("Sockets initialized (BSD)\n");
    noudp     = Cvar_Get("noudp",       "0", CVAR_NOSET);
    noipx     = Cvar_Get("noipx",       "1", CVAR_NOSET); // default off on Linux
    ipxfix    = Cvar_Get("ipxfix",      "1", 0);
    net_shownet = Cvar_Get("net_shownet", "0", 0);
}

void NET_Shutdown(void)
{
    NET_Config(false);
}
