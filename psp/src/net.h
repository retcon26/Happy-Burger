#pragma once

#include <stdint.h>

// Ad-hoc wireless multiplayer transport. Every PSP joins one ad-hoc group ("GmodPSP") and talks
// with PDP datagrams: broadcasts for the host's beacons and world snapshots, unicast for the rest.
// The game protocol on top of this lives in game.c (host-authoritative: the host runs physics,
// NPCs and doors; clients send their position, aim and actions).

#define NET_MAX_PLAYERS 4
#define NET_VERSION 4 // 2: rope packets name the joint type (sliders); 3: player models;
                      // 4: the host's ragdolls sent to clients, and their physics gun holds

typedef enum { NET_OFF, NET_CONNECTING, NET_READY, NET_FAILED } NetState;

int net_start(void); // loads the network modules and starts joining the group; 0 on success
NetState net_poll(void);
const char *net_status(void); // human-readable state or error
void net_stop(void);
int net_active(void);

// data goes to mac, or to everyone when mac is NULL. Returns <0 on error.
int net_send(const uint8_t *mac, const void *data, int len);
// Non-blocking. Returns the packet length (0 when nothing is waiting); mac gets the sender.
int net_recv(uint8_t mac[6], void *buf, int max);
const uint8_t *net_my_mac(void);
const char *net_nickname(void); // the PSP's system nickname

// --- Packets -----------------------------------------------------------------------------------

#define NET_MAGIC 0xA7
enum {
    PK_BEACON = 1, // host -> all: "a game is running here"
    PK_JOIN,       // client -> host
    PK_WELCOME,    // host -> client: your slot and the map
    PK_FULL,       // host -> client: no room
    PK_CSTATE,     // client -> host: player state and actions
    PK_PLAYERS,    // host -> all: players and NPCs
    PK_BODIES,     // host -> all: a chunk of physics bodies
    PK_ENTS,       // host -> all: doors, buttons, lights
    PK_ROPES,      // host -> all: rope constraints, for drawing
    PK_LEAVE,      // either way: goodbye
    PK_RAGINFO,    // host -> all: a ragdoll's skeleton, from its pose at death
    PK_RAGPOSE,    // host -> all: a ragdoll's points
};

typedef struct {
    uint8_t magic, type, slot, version;
} NetHeader;

typedef struct {
    NetHeader h;
    char map[32];
    char host[16];
    uint8_t players, max, pad[2];
} PkBeacon;

typedef struct {
    NetHeader h;
    char name[16];
    uint32_t nonce; // new for each join attempt, so the host can tell a rejoin from a resend
} PkJoin;

typedef struct {
    NetHeader h; // h.slot is the joining player's slot
    char map[32];
} PkWelcome;

// --- Lobby (the Join Game screen) ----------------------------------------------------------------

typedef struct {
    uint8_t mac[6];
    char map[32];
    char host[16];
    int players, max;
    float age; // seconds since the last beacon
} NetHost;

// Reads beacons and join replies. Returns 1 once a join was accepted (see net_lobby_result),
// -1 if the host was full, else 0.
int net_lobby_update(float dt, NetHost *hosts, int max_hosts, int *num_hosts);
void net_lobby_join(const NetHost *h); // starts asking this host to join (resent until answered)
void net_lobby_result(char *map, int map_len, int *slot, uint8_t host_mac[6]);
