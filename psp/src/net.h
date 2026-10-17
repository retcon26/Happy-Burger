#pragma once

#include <stdint.h>

// Multiplayer transport, two ways:
//   Ad-hoc: every PSP joins one ad-hoc group ("GmodPSP") and talks with PDP datagrams: broadcasts for
//     the host's beacons and world snapshots, unicast for the rest.
//   Wi-Fi: UDP through an access point, to games on the same network (a PSP hosting over Wi-Fi) or
//     dedicated servers anywhere (retcons-mod-server, on a PC). Addresses are the IPv4 address and
//     port packed into the 6 bytes the game uses for a MAC.
// The game protocol on top of this lives in game.c (host-authoritative: the host runs physics,
// NPCs and doors; clients send their position, aim and actions).

#define NET_MAX_PLAYERS 8 // slots: a PSP hosting takes 4 (itself and 3), a dedicated server up to 8 (7 players)
#define NET_PSP_HOST_SLOTS 4
#define NET_PORT 18256
#define NET_LIST_PORT 18257 // server lists (retcons-mod-server in list mode)
#define NET_LIST_MAX 64     // servers in one list answer
#define NET_VERSION 15 // 2: rope packets name the joint type (sliders); 3: player models;
                       // 4: the host's ragdolls sent to clients, and their physics gun holds; 5: vehicles;
                       // 6: Profile colours, mingebags; 7: TF2 teams (NPC numbers moved); 8: devices, hydraulics;
                       // 9: Wi-Fi and dedicated servers (PK_INFO, beacon flags); 10: AR2, crossbow, RPG, grenades,
                       // bug bait (PK_PROJ, CMD_LAUNCH, CMD_SQUEEZE); 11: medkits and chargers (CMD_PICKUP, CMD_HEAL);
                       // 12: Face and Finger Poser (CMD_RAG_POSE, RagInfo face and hands);
                       // 13: 8 player slots, server passwords (PkJoin.password, PK_FULL reasons, BEACON_PASSWORD);
                       // 14: quick chat (CMD_QUICKCHAT, CMD_GESTURE, NetPlayer.gesture);
                       // 15: Lua gamemodes (PK_GMSTATE, PK_GMEVENTS, the gamemode in beacons and welcomes)

enum { NET_MODE_ADHOC, NET_MODE_INET };

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

// Which way the next net_start() connects. host: a host over Wi-Fi listens on NET_PORT.
void net_set_mode(int mode, int host);
int net_mode(void);
// Wi-Fi: "name", "1.2.3.4" or either with ":port" to an address; 0 on success (names wait for DNS)
int net_resolve(const char *text, uint8_t addr[6]);
void net_addr_text(const uint8_t addr[6], char *out, int len); // "1.2.3.4:18256"

// --- Packets -----------------------------------------------------------------------------------

#define NET_MAGIC 0xA7
enum {
    PK_BEACON = 1, // host -> all: "a game is running here"
    PK_JOIN,       // client -> host
    PK_WELCOME,    // host -> client: your slot and the map
    PK_FULL,       // host -> client: can't join; h.slot says why (JOIN_REFUSED_*)
    PK_CSTATE,     // client -> host: player state and actions
    PK_PLAYERS,    // host -> all: players and NPCs
    PK_BODIES,     // host -> all: a chunk of physics bodies
    PK_ENTS,       // host -> all: doors, buttons, lights
    PK_ROPES,      // host -> all: rope constraints, for drawing
    PK_LEAVE,      // either way: goodbye
    PK_RAGINFO,    // host -> all: a ragdoll's skeleton, from its pose at death
    PK_RAGPOSE,    // host -> all: a ragdoll's points
    PK_VOICE,      // client -> host -> all others (or host -> all): a frame of voice chat
    PK_CHAT,       // host -> all: a line in the chat feed (mingebags' rubbish)
    PK_DEVICES,    // host -> all: lamps, lights, buttons and emitters
    PK_INFO,       // player -> host: "tell me about your game" (the host answers with a beacon)
    PK_HEARTBEAT,  // dedicated server -> server list: "list me" (every 30 seconds)
    PK_LIST_REQ,   // player -> server list: "which servers are there?"
    PK_LIST,       // server list -> player: their addresses (each then answers PK_INFO with a beacon)
    PK_PROJ,       // host -> all: rockets, bolts and bug bait in flight, and the latest explosions
    PK_GMSTATE,    // host -> all: the gamemode's rules for each player, and its networked variables
    PK_GMEVENTS,   // host -> all: the gamemode's latest messages, sounds and ammo handouts
};
#define NET_MODE_LEN 16 // a gamemode's name (its file in data/lua/gamemodes, without .lua; "" sandbox)


typedef struct {
    uint8_t magic, type, slot, version;
} NetHeader;

typedef struct {
    NetHeader h;
    char text[48];
} PkChat;


typedef struct {
    NetHeader h;
    char map[32];
    char host[16];
    uint8_t players, max, flags, pad;
    char mode[NET_MODE_LEN]; // the gamemode ("" sandbox)
} PkBeacon;
#define BEACON_DEDICATED 1 // a dedicated server: nobody plays on the host
#define BEACON_PASSWORD 2  // joining needs the server's password

enum { JOIN_REFUSED_FULL, JOIN_REFUSED_PASSWORD }; // PK_FULL's h.slot

// h.version is the game's: a list only offers players the servers they can join
typedef struct {
    NetHeader h;
    uint8_t addr[6]; // the address to list (IP 0.0.0.0: the one the heartbeat came from) and the game port
    uint8_t pad[2];
} PkHeartbeat;

typedef struct {
    NetHeader h;
    uint8_t count, pad[3];
    uint8_t addr[NET_LIST_MAX][6];
} PkList;

typedef struct {
    NetHeader h;
    char name[16];
    uint32_t nonce; // new for each join attempt, so the host can tell a rejoin from a resend
    char password[16]; // a dedicated server's password ("" if none)
} PkJoin;

typedef struct {
    NetHeader h; // h.slot is the joining player's slot
    char map[32];
    char mode[NET_MODE_LEN]; // the gamemode to load the map with ("" sandbox)
} PkWelcome;

// --- Lobby (the Join Game screen) ----------------------------------------------------------------

typedef struct {
    uint8_t mac[6];
    char map[32];
    char host[16];
    int players, max;
    int dedicated;
    int password; // joining it needs a password
    char mode[NET_MODE_LEN]; // its gamemode ("" sandbox)
    float age; // seconds since the last beacon
} NetHost;

// Reads beacons and join replies. Returns 1 once a join was accepted (see net_lobby_result),
// -1 if the host was full, -2 if the password was wrong (or missing), else 0.
int net_lobby_update(float dt, NetHost *hosts, int max_hosts, int *num_hosts);
void net_lobby_join(const NetHost *h); // starts asking this host to join (resent until answered)
// The password the next joins send ("": none); it stays set, so joining again after a map change reuses it
void net_lobby_set_password(const char *password);
// Asks a host about its game (it answers with a beacon); NULL asks everyone nearby
void net_lobby_query(const uint8_t *addr);
// Asks a server list for its servers; once it answers, net_lobby_query(NULL) asks each of them too
void net_lobby_list_request(const uint8_t *list_addr);
int net_lobby_listed(void); // how many servers the list gave (-1: no answer yet)
void net_lobby_result(char *map, int map_len, int *slot, uint8_t host_mac[6]);
const char *net_lobby_mode(void); // the gamemode the accepted join's welcome named ("" sandbox)
