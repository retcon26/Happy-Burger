#include "net.h"

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>
#include <psputility_sysparam.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "settings.h"


#define GROUP_NAME "GmodPSP"
#define CONNECT_TIMEOUT_US 15000000
#define WIFI_TIMEOUT_US 30000000
#define JOIN_RESEND 0.5f
#define HOST_EXPIRE 4.0f
#define PEER_EXPIRE_US 10000000 // Wi-Fi: how long an address we heard from counts as in the game

static const uint8_t k_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Wi-Fi: the addresses that sent us game packets lately. "Send to everyone" goes to each of them,
// since an access point (or the internet) has no ad-hoc broadcast.
#define MAX_INET_PEERS 16
typedef struct {
    uint8_t addr[6];
    SceInt64 heard;
} InetPeer;

static struct {
    NetState state;
    int mode, mode_host; // NET_MODE_*; Wi-Fi: listening on the game port
    int modules;  // bit 0 common, bit 1 adhoc, bit 2 inet
    int inited;   // bit 0 sceNet, bit 1 adhoc, bit 2 adhocctl, bit 3 connected; Wi-Fi: bit 4 inet, 5 resolver, 6 apctl
    int pdp;
    int sock;     // Wi-Fi: the UDP socket
    uint32_t ip, mask; // Wi-Fi: ours, network byte order
    uint8_t mac[6];
    char nick[16];
    char status[64];
    SceInt64 t_start;
    InetPeer peers[MAX_INET_PEERS];
    // Lobby
    int joining;
    uint32_t nonce;
    NetHost join_target;
    char password[16]; // what joins send (net_lobby_set_password)
    // Online Servers: the server list asked last, and the servers it gave
    uint8_t list_addr[6];
    int num_listed; // -1: no answer yet
    uint8_t listed[NET_LIST_MAX][6];
    float join_timer;
    char welcome_map[32];
    int welcome_slot;
} N = {.pdp = -1, .sock = -1};

static void set_status(const char *msg, int code) {
    if (code) {
        snprintf(N.status, sizeof(N.status), "%s (%08X)", msg, (unsigned int)code);
    } else {
        snprintf(N.status, sizeof(N.status), "%s", msg);
    }
}

static int fail(const char *msg, int code) {
    net_stop(); // before the message: stopping says "Offline"
    set_status(msg, code);
    N.state = NET_FAILED;
    return -1;
}

int net_active(void) {
    return N.state == NET_READY;
}

const char *net_status(void) {
    return N.status;
}

const uint8_t *net_my_mac(void) {
    return N.mac;
}

void net_set_mode(int mode, int host) {
    N.mode = mode;
    N.mode_host = host;
}

int net_mode(void) {
    return N.mode;
}

const char *net_nickname(void) {
    if (g_settings.name[0]) return g_settings.name; // Multiplayer > Profile
    if (!N.nick[0]) {

        char buf[128] = "";
        if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, buf, sizeof(buf)) < 0 || !buf[0]) {
            snprintf(buf, sizeof(buf), "Player");
        }
        snprintf(N.nick, sizeof(N.nick), "%s", buf);
    }
    return N.nick;
}

// --- Addresses (Wi-Fi): 4 bytes of IPv4 then the port, both in network order --------------------

static void addr_pack(uint8_t out[6], uint32_t ip_net, uint16_t port_host) {
    memcpy(out, &ip_net, 4);
    out[4] = (uint8_t)(port_host >> 8);
    out[5] = (uint8_t)(port_host & 0xFF);
}

static void addr_sockaddr(const uint8_t a[6], struct sockaddr_in *sa) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_len = sizeof(*sa);
    sa->sin_family = AF_INET;
    memcpy(&sa->sin_addr.s_addr, a, 4);
    sa->sin_port = htons((uint16_t)((a[4] << 8) | a[5]));
}

void net_addr_text(const uint8_t a[6], char *out, int len) {
    snprintf(out, len, "%d.%d.%d.%d:%d", a[0], a[1], a[2], a[3], (a[4] << 8) | a[5]);
}

// "1.2.3.4" to network order; 0 if it isn't one
static int parse_ipv4(const char *s, uint32_t *out) {
    unsigned int b[4];
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &b[0], &b[1], &b[2], &b[3], &tail) != 4) return 0;
    if (b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255) return 0;
    uint8_t bytes[4] = {(uint8_t)b[0], (uint8_t)b[1], (uint8_t)b[2], (uint8_t)b[3]};
    memcpy(out, bytes, 4);
    return 1;
}

int net_resolve(const char *text, uint8_t addr[6]) {
    char host[64];
    snprintf(host, sizeof(host), "%s", text);
    int port = NET_PORT;
    char *colon = strrchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
        if (port <= 0 || port > 65535) return -1;
    }
    uint32_t ip;
    if (!parse_ipv4(host, &ip)) {
        if (!(N.inited & 32)) return -1; // names need Wi-Fi up (DNS)
        char buf[1024];
        int rid;
        if (sceNetResolverCreate(&rid, buf, sizeof(buf)) < 0) return -1;
        struct in_addr in;
        int r = sceNetResolverStartNtoA(rid, host, &in, 2, 3);
        sceNetResolverDelete(rid);
        if (r < 0) return -1;
        ip = in.s_addr;
    }
    addr_pack(addr, ip, (uint16_t)port);
    return 0;
}

static void peer_heard(const uint8_t a[6]) {
    SceInt64 now = sceKernelGetSystemTimeWide();
    int oldest = 0;
    for (int i = 0; i < MAX_INET_PEERS; i++) {
        if (memcmp(N.peers[i].addr, a, 6) == 0) {
            N.peers[i].heard = now;
            return;
        }
        if (N.peers[i].heard < N.peers[oldest].heard) oldest = i;
    }
    memcpy(N.peers[oldest].addr, a, 6);
    N.peers[oldest].heard = now;
}

// --- Start and stop ----------------------------------------------------------------------------

static int load_module(int m, int bit) {
    int r = sceUtilityLoadNetModule(m);
    if (r < 0 && r != (int)0x80110802) return r; // 0x80110802: already loaded
    N.modules |= bit;
    return 0;
}

static int start_adhoc(void) {
    int r;
    if ((r = load_module(PSP_NET_MODULE_COMMON, 1)) < 0) return fail("Couldn't load the network module", r);
    if ((r = load_module(PSP_NET_MODULE_ADHOC, 2)) < 0) return fail("Couldn't load the ad-hoc module", r);
    r = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (r < 0) return fail("sceNetInit failed", r);
    N.inited |= 1;
    r = sceNetAdhocInit();
    if (r < 0) return fail("Ad-hoc init failed", r);
    N.inited |= 2;
    struct productStruct product;
    memset(&product, 0, sizeof(product));
    memcpy(product.product, "ULUS99999", 9);
    r = sceNetAdhocctlInit(0x2000, 0x20, &product);
    if (r < 0) return fail("Ad-hoc control init failed", r);
    N.inited |= 4;
    char group[8] = {0};
    memcpy(group, GROUP_NAME, strlen(GROUP_NAME));
    r = sceNetAdhocctlConnect(group);
    if (r < 0) return fail("Couldn't start ad-hoc (is the WLAN switch on?)", r);
    N.inited |= 8;
    set_status("Connecting...", 0);
    return 0;
}

static int start_wifi(void) {
    int r;
    if ((r = load_module(PSP_NET_MODULE_COMMON, 1)) < 0) return fail("Couldn't load the network module", r);
    if ((r = load_module(PSP_NET_MODULE_INET, 4)) < 0) return fail("Couldn't load the internet module", r);
    r = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    if (r < 0) return fail("sceNetInit failed", r);
    N.inited |= 1;
    if ((r = sceNetInetInit()) < 0) return fail("Internet init failed", r);
    N.inited |= 16;
    if ((r = sceNetResolverInit()) < 0) return fail("DNS init failed", r);
    N.inited |= 32;
    if ((r = sceNetApctlInit(0x8000, 48)) < 0) return fail("Access point init failed", r);
    N.inited |= 64;
    // The first connection set up in System Settings > Network Settings
    int profile = 0;
    for (int i = 1; i <= 10 && !profile; i++) {
        if (sceUtilityCheckNetParam(i) == 0) profile = i;
    }
    if (!profile) return fail("No Wi-Fi connection: set one up in Network Settings", 0);
    r = sceNetApctlConnect(profile);
    if (r < 0) return fail("Couldn't connect (is the WLAN switch on?)", r);
    N.inited |= 8;
    set_status("Connecting to the access point...", 0);
    return 0;
}

int net_start(void) {
    if (N.state == NET_READY || N.state == NET_CONNECTING) return 0;
    memset(&N.mac, 0, sizeof(N.mac));
    memset(N.peers, 0, sizeof(N.peers));
    memset(N.list_addr, 0, sizeof(N.list_addr));
    N.num_listed = -1;
    N.joining = 0;
    if ((N.mode == NET_MODE_INET ? start_wifi() : start_adhoc()) < 0) return -1;
    N.state = NET_CONNECTING;
    N.t_start = sceKernelGetSystemTimeWide();
    return 0;
}

static void open_wifi_socket(void) {
    SceNetApctlInfo info;
    memset(&info, 0, sizeof(info));
    sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info);
    if (!parse_ipv4(info.ip, &N.ip)) N.ip = 0;
    memset(&info, 0, sizeof(info));
    sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SUBNETMASK, &info);
    if (!parse_ipv4(info.subNetMask, &N.mask)) N.mask = 0x00FFFFFF; // 255.255.255.0 in network order
    N.sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (N.sock < 0) {
        fail("Couldn't open a socket", sceNetInetGetErrno());
        return;
    }
    int one = 1;
    sceNetInetSetsockopt(N.sock, SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
    sceNetInetSetsockopt(N.sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    // Hosts listen on the game port (where LAN broadcasts and joining players go); players take any
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_len = sizeof(sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(N.mode_host ? NET_PORT : 0);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (sceNetInetBind(N.sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fail("Couldn't open the game port", sceNetInetGetErrno());
        return;
    }
    socklen_t sl = sizeof(sa);
    sceNetInetGetsockname(N.sock, (struct sockaddr *)&sa, &sl);
    addr_pack(N.mac, N.ip, ntohs(sa.sin_port));
    N.state = NET_READY;
    char msg[48];
    snprintf(msg, sizeof(msg), "Connected as %d.%d.%d.%d", N.mac[0], N.mac[1], N.mac[2], N.mac[3]);
    set_status(msg, 0);
}

NetState net_poll(void) {
    if (N.state != NET_CONNECTING) return N.state;
    SceInt64 waited = sceKernelGetSystemTimeWide() - N.t_start;
    if (N.mode == NET_MODE_INET) {
        int st = 0;
        if (sceNetApctlGetState(&st) < 0) {
            fail("Lost the access point", 0);
        } else if (st == PSP_NET_APCTL_STATE_GOT_IP) {
            open_wifi_socket();
        } else if (st == PSP_NET_APCTL_STATE_DISCONNECTED && waited > 2000000) {
            fail("Couldn't connect to the access point", 0);
        } else if (waited > WIFI_TIMEOUT_US) {
            fail("Wi-Fi connection timed out", 0);
        }
        return N.state;
    }
    int st = 0;
    if (sceNetAdhocctlGetState(&st) >= 0 && st == 1) {
        sceNetGetLocalEtherAddr(N.mac);
        N.pdp = sceNetAdhocPdpCreate(N.mac, NET_PORT, 0x10000, 0);
        if (N.pdp < 0) {
            fail("Couldn't open a socket", N.pdp);
            return N.state;
        }
        N.state = NET_READY;
        set_status("Connected", 0);
    } else if (waited > CONNECT_TIMEOUT_US) {
        fail("Ad-hoc connection timed out", 0);
    }
    return N.state;
}

void net_stop(void) {
    if (N.pdp >= 0) sceNetAdhocPdpDelete(N.pdp, 0);
    N.pdp = -1;
    if (N.sock >= 0) sceNetInetClose(N.sock);
    N.sock = -1;
    if (N.inited & 8) {
        int st = 0;
        if (N.inited & 64) {
            sceNetApctlDisconnect();
        } else {
            sceNetAdhocctlDisconnect();
        }
        // Give the disconnect a moment to go through
        for (int i = 0; i < 50; i++) {
            int r = (N.inited & 64) ? sceNetApctlGetState(&st) : sceNetAdhocctlGetState(&st);
            if (r < 0 || st == 0) break;
            sceKernelDelayThread(20000);
        }
    }
    if (N.inited & 64) sceNetApctlTerm();
    if (N.inited & 32) sceNetResolverTerm();
    if (N.inited & 16) sceNetInetTerm();
    if (N.inited & 4) sceNetAdhocctlTerm();
    if (N.inited & 2) sceNetAdhocTerm();
    if (N.inited & 1) sceNetTerm();
    N.inited = 0;
    if (N.modules & 4) sceUtilityUnloadNetModule(PSP_NET_MODULE_INET);
    if (N.modules & 2) sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
    if (N.modules & 1) sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    N.modules = 0;
    N.joining = 0;
    if (N.state != NET_FAILED) {
        N.state = NET_OFF;
        set_status("Offline", 0);
    }
}

// --- Packets -------------------------------------------------------------------------------------

static int wifi_send_to(const uint8_t a[6], const void *data, int len) {
    struct sockaddr_in sa;
    addr_sockaddr(a, &sa);
    return (int)sceNetInetSendto(N.sock, data, (size_t)len, 0, (struct sockaddr *)&sa, sizeof(sa));
}

// Everyone nearby on the access point's network, on the game port (finding games)
static int wifi_send_lan(const void *data, int len) {
    uint8_t a[6];
    addr_pack(a, N.ip | ~N.mask, NET_PORT);
    return wifi_send_to(a, data, len);
}

int net_send(const uint8_t *mac, const void *data, int len) {
    if (N.mode == NET_MODE_INET) {
        if (N.sock < 0) return -1;
        if (mac) return wifi_send_to(mac, data, len);
        // Everyone: a host's beacons go round the LAN (for Online Servers' list); the rest of a
        // host's game goes to the players it has heard from; a player's queries go round the LAN
        const NetHeader *h = (const NetHeader *)data;
        if (!N.mode_host || h->type == PK_BEACON) wifi_send_lan(data, len);
        if (!N.mode_host) return len;
        SceInt64 now = sceKernelGetSystemTimeWide();
        for (int i = 0; i < MAX_INET_PEERS; i++) {
            if (N.peers[i].heard && now - N.peers[i].heard < PEER_EXPIRE_US) wifi_send_to(N.peers[i].addr, data, len);
        }
        return len;
    }
    if (N.pdp < 0) return -1;
    return sceNetAdhocPdpSend(N.pdp, (unsigned char *)(mac ? mac : k_broadcast), NET_PORT, (void *)data, (unsigned int)len, 0, 1);
}

int net_recv(uint8_t mac[6], void *buf, int max) {
    if (N.mode == NET_MODE_INET) {
        if (N.sock < 0) return 0;
        for (;;) {
            struct sockaddr_in sa;
            socklen_t sl = sizeof(sa);
            int len = (int)sceNetInetRecvfrom(N.sock, buf, (size_t)max, 0, (struct sockaddr *)&sa, &sl);
            if (len <= 0) return 0; // nothing waiting
            addr_pack(mac, sa.sin_addr.s_addr, ntohs(sa.sin_port));
            if (len < (int)sizeof(NetHeader) || ((const NetHeader *)buf)->magic != NET_MAGIC) continue;
            if (memcmp(mac, N.mac, 6) == 0) continue; // our own broadcast
            // A player's query isn't someone in the game yet; everything else from them is
            int t = ((const NetHeader *)buf)->type;
            if (t != PK_INFO && t != PK_BEACON) peer_heard(mac); // (another host's beacon isn't a player)
            return len;
        }
    }
    if (N.pdp < 0) return 0;
    for (;;) {
        int len = max;
        unsigned short port = 0;
        int r = sceNetAdhocPdpRecv(N.pdp, mac, &port, buf, &len, 0, 1);
        if (r < 0 || len <= 0) return 0; // nothing waiting
        if (len < (int)sizeof(NetHeader) || ((const NetHeader *)buf)->magic != NET_MAGIC) continue;
        if (memcmp(mac, N.mac, 6) == 0) continue; // our own broadcast
        return len;
    }
}

// --- Lobby -------------------------------------------------------------------------------------

void net_lobby_query(const uint8_t *addr) {
    NetHeader q = {NET_MAGIC, PK_INFO, 0, NET_VERSION};
    net_send(addr, &q, sizeof(q));
    if (addr) return;
    for (int i = 0; i < N.num_listed; i++) net_send(N.listed[i], &q, sizeof(q)); // and the list's servers
}

void net_lobby_list_request(const uint8_t *list_addr) {
    if (memcmp(N.list_addr, list_addr, 6) != 0) N.num_listed = -1; // a different list
    memcpy(N.list_addr, list_addr, 6);
    NetHeader q = {NET_MAGIC, PK_LIST_REQ, 0, NET_VERSION};
    net_send(list_addr, &q, sizeof(q));
}

int net_lobby_listed(void) {
    return N.num_listed;
}

void net_lobby_set_password(const char *password) {
    snprintf(N.password, sizeof(N.password), "%s", password ? password : "");
}

void net_lobby_join(const NetHost *h) {
    N.joining = 1;
    N.nonce = sceKernelGetSystemTimeLow() | 1;
    N.join_target = *h;
    N.join_timer = 0.0f;
}

void net_lobby_result(char *map, int map_len, int *slot, uint8_t host_mac[6]) {
    snprintf(map, map_len, "%s", N.welcome_map);
    *slot = N.welcome_slot;
    memcpy(host_mac, N.join_target.mac, 6);
}

int net_lobby_update(float dt, NetHost *hosts, int max_hosts, int *num_hosts) {
    if (net_poll() != NET_READY) return 0;
    int result = 0;
    for (int i = 0; i < *num_hosts; i++) hosts[i].age += dt;
    union {
        NetHeader h;
        PkBeacon beacon;
        PkWelcome welcome;
        uint8_t raw[1500];
    } pk;
    uint8_t from[6];
    int len;
    while ((len = net_recv(from, &pk, sizeof(pk))) > 0) {
        if (pk.h.type == PK_LIST && len >= (int)offsetof(PkList, addr) && memcmp(from, N.list_addr, 6) == 0) {
            // The server list's answer: ask each of its servers about their games straight away
            const PkList *l = (const PkList *)pk.raw;
            int n = l->count;
            int fits = (len - (int)offsetof(PkList, addr)) / 6;
            if (n > fits) n = fits;
            if (n > NET_LIST_MAX) n = NET_LIST_MAX;
            memcpy(N.listed, l->addr, (size_t)n * 6);
            N.num_listed = n;
            NetHeader q = {NET_MAGIC, PK_INFO, 0, NET_VERSION};
            for (int i = 0; i < n; i++) net_send(N.listed[i], &q, sizeof(q));
            continue;
        }
        if (pk.h.type == PK_BEACON && len >= (int)sizeof(PkBeacon) && pk.h.version == NET_VERSION) {
            int k;
            for (k = 0; k < *num_hosts && memcmp(hosts[k].mac, from, 6) != 0; k++) {
            }
            if (k == *num_hosts) {
                if (*num_hosts >= max_hosts) continue;
                (*num_hosts)++;
            }
            NetHost *h = &hosts[k];
            memcpy(h->mac, from, 6);
            snprintf(h->map, sizeof(h->map), "%.31s", pk.beacon.map);
            snprintf(h->host, sizeof(h->host), "%.15s", pk.beacon.host);
            h->players = pk.beacon.players;
            h->max = pk.beacon.max;
            h->dedicated = (pk.beacon.flags & BEACON_DEDICATED) != 0;
            h->password = (pk.beacon.flags & BEACON_PASSWORD) != 0;
            h->age = 0.0f;
        } else if (N.joining && memcmp(from, N.join_target.mac, 6) == 0) {
            if (pk.h.type == PK_WELCOME && len >= (int)sizeof(PkWelcome)) {
                snprintf(N.welcome_map, sizeof(N.welcome_map), "%.31s", pk.welcome.map);
                N.welcome_slot = pk.h.slot;
                N.joining = 0;
                result = 1;
            } else if (pk.h.type == PK_FULL) {
                N.joining = 0;
                result = pk.h.slot == JOIN_REFUSED_PASSWORD ? -2 : -1;
            }
        }
    }
    // Forget hosts that went quiet
    for (int i = 0; i < *num_hosts;) {
        if (hosts[i].age > HOST_EXPIRE) {
            hosts[i] = hosts[--(*num_hosts)];
        } else {
            i++;
        }
    }
    if (N.joining) {
        N.join_timer -= dt;
        if (N.join_timer <= 0.0f) {
            PkJoin j;
            memset(&j, 0, sizeof(j));
            j.h = (NetHeader){NET_MAGIC, PK_JOIN, 0, NET_VERSION};
            snprintf(j.name, sizeof(j.name), "%s", net_nickname());
            j.nonce = N.nonce;
            memcpy(j.password, N.password, sizeof(j.password));
            net_send(N.join_target.mac, &j, sizeof(j));
            N.join_timer = JOIN_RESEND;
        }
    }
    return result;
}
