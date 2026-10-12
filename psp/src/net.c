#include "net.h"

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_adhoc.h>
#include <pspnet_adhocctl.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_sysparam.h>
#include <stdio.h>
#include <string.h>

#define NET_PORT 18256
#define GROUP_NAME "GmodPSP"
#define CONNECT_TIMEOUT_US 15000000
#define JOIN_RESEND 0.5f
#define HOST_EXPIRE 4.0f

static const uint8_t k_broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static struct {
    NetState state;
    int modules;  // bit 0 common, bit 1 adhoc
    int inited;   // bit 0 sceNet, bit 1 adhoc, bit 2 adhocctl
    int pdp;
    uint8_t mac[6];
    char nick[16];
    char status[64];
    SceInt64 t_start;
    // Lobby
    int joining;
    uint32_t nonce;
    NetHost join_target;
    float join_timer;
    char welcome_map[32];
    int welcome_slot;
} N = {.pdp = -1};

static void set_status(const char *msg, int code) {
    if (code) {
        snprintf(N.status, sizeof(N.status), "%s (%08X)", msg, (unsigned int)code);
    } else {
        snprintf(N.status, sizeof(N.status), "%s", msg);
    }
}

static int fail(const char *msg, int code) {
    set_status(msg, code);
    net_stop();
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

const char *net_nickname(void) {
    if (!N.nick[0]) {
        char buf[128] = "";
        if (sceUtilityGetSystemParamString(PSP_SYSTEMPARAM_ID_STRING_NICKNAME, buf, sizeof(buf)) < 0 || !buf[0]) {
            snprintf(buf, sizeof(buf), "Player");
        }
        snprintf(N.nick, sizeof(N.nick), "%s", buf);
    }
    return N.nick;
}

int net_start(void) {
    if (N.state == NET_READY || N.state == NET_CONNECTING) return 0;
    memset(&N.mac, 0, sizeof(N.mac));
    N.joining = 0;
    int r = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (r < 0 && r != (int)0x80110802) return fail("Couldn't load the network module", r);
    N.modules |= 1;
    r = sceUtilityLoadNetModule(PSP_NET_MODULE_ADHOC);
    if (r < 0 && r != (int)0x80110802) return fail("Couldn't load the ad-hoc module", r);
    N.modules |= 2;
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
    N.state = NET_CONNECTING;
    N.t_start = sceKernelGetSystemTimeWide();
    set_status("Connecting...", 0);
    return 0;
}

NetState net_poll(void) {
    if (N.state != NET_CONNECTING) return N.state;
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
    } else if (sceKernelGetSystemTimeWide() - N.t_start > CONNECT_TIMEOUT_US) {
        fail("Ad-hoc connection timed out", 0);
    }
    return N.state;
}

void net_stop(void) {
    if (N.pdp >= 0) sceNetAdhocPdpDelete(N.pdp, 0);
    N.pdp = -1;
    if (N.inited & 8) {
        sceNetAdhocctlDisconnect();
        // Give the disconnect a moment to go through
        for (int i = 0; i < 50; i++) {
            int st = 0;
            if (sceNetAdhocctlGetState(&st) < 0 || st == 0) break;
            sceKernelDelayThread(20000);
        }
    }
    if (N.inited & 4) sceNetAdhocctlTerm();
    if (N.inited & 2) sceNetAdhocTerm();
    if (N.inited & 1) sceNetTerm();
    N.inited = 0;
    if (N.modules & 2) sceUtilityUnloadNetModule(PSP_NET_MODULE_ADHOC);
    if (N.modules & 1) sceUtilityUnloadNetModule(PSP_NET_MODULE_COMMON);
    N.modules = 0;
    N.joining = 0;
    if (N.state != NET_FAILED) {
        N.state = NET_OFF;
        set_status("Offline", 0);
    }
}

int net_send(const uint8_t *mac, const void *data, int len) {
    if (N.pdp < 0) return -1;
    return sceNetAdhocPdpSend(N.pdp, (unsigned char *)(mac ? mac : k_broadcast), NET_PORT, (void *)data, (unsigned int)len, 0, 1);
}

int net_recv(uint8_t mac[6], void *buf, int max) {
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
            h->age = 0.0f;
        } else if (N.joining && memcmp(from, N.join_target.mac, 6) == 0) {
            if (pk.h.type == PK_WELCOME && len >= (int)sizeof(PkWelcome)) {
                snprintf(N.welcome_map, sizeof(N.welcome_map), "%.31s", pk.welcome.map);
                N.welcome_slot = pk.h.slot;
                N.joining = 0;
                result = 1;
            } else if (pk.h.type == PK_FULL) {
                N.joining = 0;
                result = -1;
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
            net_send(N.join_target.mac, &j, sizeof(j));
            N.join_timer = JOIN_RESEND;
        }
    }
    return result;
}
