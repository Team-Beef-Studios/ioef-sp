/*
===========================================================================
sv_debug_server.c - Live debug inspection server for MCP integration

Provides a TCP-based JSON protocol for real-time inspection of:
- Entity state (entityState_t, entityShared_t)
- Player state (playerState_t)
- Server status (server_t, clients)
- Struct memory layouts (field offsets, sizes)
- Field validation (range checks, NaN detection)

Enable with cvar: sv_debugPort (default 29070, 0 = disabled)
===========================================================================
*/

#include "server.h"
#include "../qcommon/sp_types.h"
#include <inttypes.h>
#include <stddef.h>
#include <math.h>

// Use Q3's Q_isnan for portability; add a simple isinf fallback
#ifndef IS_NAN
#define IS_NAN(x) (Q_isnan(x))
#endif
#ifndef IS_INF
#define IS_INF(x) (!Q_isnan(x) && Q_isnan((x) - (x)))
#endif

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET debug_socket_t;
  typedef int socklen_t;
  #define DEBUG_INVALID_SOCK INVALID_SOCKET
  #define DEBUG_SOCK_ERROR SOCKET_ERROR
  #define debug_closesocket closesocket
  #define debug_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
  /* ws2_32.lib linked via Makefile LIBS on MinGW; MSVC would need: #pragma comment(lib, "ws2_32.lib") */
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <errno.h>
  #include <unistd.h>
  typedef int debug_socket_t;
  #define DEBUG_INVALID_SOCK -1
  #define DEBUG_SOCK_ERROR -1
  #define debug_closesocket close
  #define debug_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

#define DEBUG_MAX_CLIENTS   4
#define DEBUG_RECV_BUF      8192
#define DEBUG_SEND_BUF      (256 * 1024)
#define DEBUG_DEFAULT_PORT  29070

// ------------------------------------------------------------------
// Client connection state
// ------------------------------------------------------------------

typedef struct {
	debug_socket_t  sock;
	char            recvBuf[DEBUG_RECV_BUF];
	int             recvLen;
	char            sendBuf[DEBUG_SEND_BUF];
	int             sendLen;
} debugClient_t;

static debug_socket_t   debugListenSock = DEBUG_INVALID_SOCK;
static debugClient_t    debugClients[DEBUG_MAX_CLIENTS];
static cvar_t           *sv_debugPort;
static qboolean         debugInitialized = qfalse;

// ------------------------------------------------------------------
// Log ring buffer — captures Com_Printf output
// ------------------------------------------------------------------

#define LOG_RING_LINES  512
#define LOG_LINE_MAX    256

static char     logRing[LOG_RING_LINES][LOG_LINE_MAX];
static int      logRingHead = 0;      // next write slot
static int      logRingCount = 0;     // total lines ever written (monotonic ID)
static char     logLineBuf[LOG_LINE_MAX]; // accumulates partial line
static int      logLineBufLen = 0;

// ------------------------------------------------------------------
// Entity watchpoints
// ------------------------------------------------------------------

#define MAX_WATCHPOINTS 16

typedef struct {
	qboolean    active;
	int         entNum;
	char        field[32];
	int         lastValue;
} watchpoint_t;

static watchpoint_t watchpoints[MAX_WATCHPOINTS];

// ------------------------------------------------------------------
// VM call tracing
// ------------------------------------------------------------------

#define VMTRACE_RING_SIZE 256

typedef struct {
	int     command;
	int     serverTime;
	int     durationUsec;
} vmTraceEntry_t;

static vmTraceEntry_t vmTraceRing[VMTRACE_RING_SIZE];
static int            vmTraceHead = 0;
static int            vmTraceCount = 0;
static qboolean       vmTraceEnabled = qfalse;

// ------------------------------------------------------------------
// SP syscall / game-import trace ring buffer
// ------------------------------------------------------------------

#define SPTRACE_RING_SIZE 2048
#define SPTRACE_STR_MAX   128

typedef struct {
	int         id;             // syscall or import ID
	char        name[48];       // human-readable name
	intptr_t    args[6];        // first 6 integer args
	char        strArg[SPTRACE_STR_MAX]; // first string arg (if any)
	intptr_t    retVal;         // return value
	int         serverTime;
	int         frameNum;
	qboolean    isImport;       // qtrue = game import, qfalse = cgame syscall
} spTraceEntry_t;

static spTraceEntry_t spTraceRing[SPTRACE_RING_SIZE];
static int            spTraceHead = 0;
static int            spTraceCount = 0;
static qboolean       spTraceEnabled = qfalse;

void DebugServer_TraceSPCall(int id, const char *name, qboolean isImport,
	intptr_t a0, intptr_t a1, intptr_t a2, intptr_t a3, intptr_t a4, intptr_t a5,
	const char *strArg, intptr_t retVal)
{
	if (!spTraceEnabled) return;
	spTraceEntry_t *e = &spTraceRing[spTraceHead % SPTRACE_RING_SIZE];
	e->id = id;
	Q_strncpyz(e->name, name, sizeof(e->name));
	e->args[0] = a0; e->args[1] = a1; e->args[2] = a2;
	e->args[3] = a3; e->args[4] = a4; e->args[5] = a5;
	if (strArg && strArg[0])
		Q_strncpyz(e->strArg, strArg, sizeof(e->strArg));
	else
		e->strArg[0] = '\0';
	e->retVal = retVal;
	e->serverTime = sv.time;
	if ( sv_fps && sv_fps->integer > 0 ) {
		e->frameNum = ( sv.time * sv_fps->integer ) / 1000;
	} else {
		e->frameNum = sv.time / 50;
	}
	e->isImport = isImport;
	spTraceHead++;
	spTraceCount++;
}

// ------------------------------------------------------------------
// JSON response builder helpers
// ------------------------------------------------------------------

typedef struct {
	char    *buf;
	int     len;
	int     cap;
	qboolean overflowed;
} jsonBuf_t;

static void jb_init(jsonBuf_t *jb, char *buf, int cap) {
	jb->buf = buf;
	jb->len = 0;
	jb->cap = cap;
	jb->overflowed = qfalse;
}

static void jb_raw_len(jsonBuf_t *jb, const char *s, int slen) {
	if (jb->len + slen < jb->cap) {
		memcpy(jb->buf + jb->len, s, slen);
		jb->len += slen;
	} else {
		jb->overflowed = qtrue;
	}
}

static void jb_raw(jsonBuf_t *jb, const char *s) {
	jb_raw_len(jb, s, strlen(s));
}

static void jb_printf(jsonBuf_t *jb, const char *fmt, ...) {
	va_list ap;
	int avail = jb->cap - jb->len;
	if (avail <= 0) {
		jb->overflowed = qtrue;
		return;
	}
	va_start(ap, fmt);
	int n = vsnprintf(jb->buf + jb->len, avail, fmt, ap);
	va_end(ap);
	if (n > 0 && n < avail) {
		jb->len += n;
	} else if (n != 0) {
		jb->overflowed = qtrue;
	}
}

static int jb_remaining(const jsonBuf_t *jb) {
	return jb->cap - jb->len;
}

static qboolean jb_can_append(const jsonBuf_t *jb, int extraLen) {
	return jb_remaining(jb) > extraLen;
}

static void jb_str(jsonBuf_t *jb, const char *key, const char *val) {
	// Escape basic JSON chars in val
	jb_printf(jb, "\"%s\":\"", key);
	for (const char *p = val; *p; p++) {
		if (*p == '"') jb_raw(jb, "\\\"");
		else if (*p == '\\') jb_raw(jb, "\\\\");
		else if (*p == '\n') jb_raw(jb, "\\n");
		else if (*p == '\r') jb_raw(jb, "\\r");
		else if (*p == '\t') jb_raw(jb, "\\t");
		else { char c[2] = {*p, 0}; jb_raw(jb, c); }
	}
	jb_raw(jb, "\"");
}

static void jb_int(jsonBuf_t *jb, const char *key, int val) {
	jb_printf(jb, "\"%s\":%d", key, val);
}

static void jb_intptr(jsonBuf_t *jb, const char *key, intptr_t val) {
	jb_printf(jb, "\"%s\":%" PRIdPTR, key, val);
}

static void jb_intptr_array6(jsonBuf_t *jb, const char *key, const intptr_t vals[6]) {
	jb_printf(jb,
		"\"%s\":[%" PRIdPTR ",%" PRIdPTR ",%" PRIdPTR ",%" PRIdPTR ",%" PRIdPTR ",%" PRIdPTR "]",
		key, vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
}

static void jb_float(jsonBuf_t *jb, const char *key, float val) __attribute__((unused));
static void jb_float(jsonBuf_t *jb, const char *key, float val) {
	if (IS_NAN(val)) jb_printf(jb, "\"%s\":\"NaN\"", key);
	else if (IS_INF(val)) jb_printf(jb, "\"%s\":\"Inf\"", key);
	else jb_printf(jb, "\"%s\":%.4f", key, val);
}

static void jb_bool(jsonBuf_t *jb, const char *key, qboolean val) {
	jb_printf(jb, "\"%s\":%s", key, val ? "true" : "false");
}

static void jb_vec3(jsonBuf_t *jb, const char *key, const vec3_t v) {
	jb_printf(jb, "\"%s\":[%.4f,%.4f,%.4f]", key, v[0], v[1], v[2]);
}

// ------------------------------------------------------------------
// Simple JSON command parser (extracts cmd, string args, int args)
// ------------------------------------------------------------------

typedef struct {
	char cmd[64];
	char strArg1[128];
	char strArg2[128];
	int  intArg1;
	int  intArg2;
	float floatArg1;
	qboolean hasIntArg1;
	qboolean hasIntArg2;
	qboolean hasFloatArg1;
	qboolean hasBoolArg1;
	qboolean boolArg1;
} debugCmd_t;

static const char *json_skip_ws(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	return p;
}

static const char *json_extract_string(const char *p, char *out, int outSz) {
	if (*p != '"') return NULL;
	p++;
	int i = 0;
	while (*p && *p != '"') {
		if (*p == '\\' && *(p+1)) { p++; }
		if (i < outSz - 1) out[i++] = *p;
		p++;
	}
	out[i] = 0;
	if (*p == '"') p++;
	return p;
}

static void debug_parse_cmd(const char *json, debugCmd_t *cmd) {
	memset(cmd, 0, sizeof(*cmd));

	const char *p = json_skip_ws(json);
	if (*p != '{') return;
	p++;

	while (*p && *p != '}') {
		p = json_skip_ws(p);
		if (*p != '"') { p++; continue; }

		char key[64] = {0};
		p = json_extract_string(p, key, sizeof(key));
		if (!p) return;
		p = json_skip_ws(p);
		if (*p == ':') p++;
		p = json_skip_ws(p);

		if (!strcmp(key, "cmd")) {
			p = json_extract_string(p, cmd->cmd, sizeof(cmd->cmd));
		} else if (!strcmp(key, "name") || !strcmp(key, "struct") || !strcmp(key, "field") || !strcmp(key, "str1")) {
			p = json_extract_string(p, cmd->strArg1, sizeof(cmd->strArg1));
		} else if (!strcmp(key, "command") || !strcmp(key, "op") || !strcmp(key, "str2")) {
			p = json_extract_string(p, cmd->strArg2, sizeof(cmd->strArg2));
		} else if (!strcmp(key, "num") || !strcmp(key, "value")) {
			if (*p == '"') {
				char tmp[64];
				p = json_extract_string(p, tmp, sizeof(tmp));
				if (!strcmp(key, "num")) { cmd->intArg1 = atoi(tmp); cmd->hasIntArg1 = qtrue; }
				else { cmd->intArg2 = atoi(tmp); cmd->hasIntArg2 = qtrue; }
			} else if (*p == 't' || *p == 'f') {
				cmd->hasBoolArg1 = qtrue;
				cmd->boolArg1 = (*p == 't') ? qtrue : qfalse;
				while (*p && *p != ',' && *p != '}') p++;
			} else {
				int neg = 0;
				if (*p == '-') { neg = 1; p++; }
				int v = 0;
					while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
				if (*p == '.') { while (*p && *p != ',' && *p != '}' && *p != ' ') p++; }
				if (neg) v = -v;
				if (!strcmp(key, "num")) { cmd->intArg1 = v; cmd->hasIntArg1 = qtrue; }
				else { cmd->intArg2 = v; cmd->hasIntArg2 = qtrue; }
			}
		} else if (!strcmp(key, "active_only") || !strcmp(key, "active")) {
			cmd->hasBoolArg1 = qtrue;
			cmd->boolArg1 = (*p == 't') ? qtrue : qfalse;
			while (*p && *p != ',' && *p != '}') p++;
		} else {
			// skip value
			if (*p == '"') {
				char tmp[128];
				p = json_extract_string(p, tmp, sizeof(tmp));
			} else {
				while (*p && *p != ',' && *p != '}') p++;
			}
		}
		if (!p) return;
		p = json_skip_ws(p);
		if (*p == ',') p++;
	}
}

// ------------------------------------------------------------------
// Response handlers
// ------------------------------------------------------------------

static void debug_cmd_status(jsonBuf_t *jb) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "status");
	jb_raw(jb, ",");

	if (sv.state == SS_DEAD) {
		jb_str(jb, "state", "dead");
	} else if (sv.state == SS_LOADING) {
		jb_str(jb, "state", "loading");
	} else {
		jb_str(jb, "state", "game");
	}

	jb_raw(jb, ",");
	jb_int(jb, "time", sv.time);
	jb_raw(jb, ",");
	jb_int(jb, "serverId", sv.serverId);
	jb_raw(jb, ",");
	jb_int(jb, "num_entities", sv.num_entities);
	jb_raw(jb, ",");
	jb_int(jb, "maxclients", sv_maxclients ? sv_maxclients->integer : 0);
	jb_raw(jb, ",");
	jb_str(jb, "mapname", sv_mapname ? sv_mapname->string : "");
	jb_raw(jb, ",");
	jb_int(jb, "fps", sv_fps ? sv_fps->integer : 0);
	jb_raw(jb, ",");
	jb_int(jb, "snapshotCounter", sv.snapshotCounter);
	jb_raw(jb, ",");
	jb_int(jb, "gentitySize", sv.gentitySize);
	jb_raw(jb, ",");
	jb_int(jb, "gameClientSize", sv.gameClientSize);

	// Active clients
	jb_raw(jb, ",\"clients\":[");
	if (svs.clients) {
		int first = 1;
		int i;
		for (i = 0; i < sv_maxclients->integer; i++) {
			client_t *cl = &svs.clients[i];
			if (cl->state == CS_FREE) continue;
			if (!first) jb_raw(jb, ",");
			first = 0;
			jb_raw(jb, "{");
			jb_int(jb, "num", i);
			jb_raw(jb, ",");
			jb_str(jb, "name", cl->name);
			jb_raw(jb, ",");
			const char *stateStr = "unknown";
			switch(cl->state) {
				case CS_ZOMBIE: stateStr = "zombie"; break;
				case CS_CONNECTED: stateStr = "connected"; break;
				case CS_PRIMED: stateStr = "primed"; break;
				case CS_ACTIVE: stateStr = "active"; break;
				default: break;
			}
			jb_str(jb, "state", stateStr);
			jb_raw(jb, ",");
			jb_int(jb, "ping", cl->ping);
			jb_raw(jb, ",");
			jb_int(jb, "rate", cl->rate);
			jb_raw(jb, "}");
		}
	}
	jb_raw(jb, "]");

	jb_raw(jb, "}\n");
}

static void debug_write_trajectory(jsonBuf_t *jb, const char *name, const trajectory_t *tr) {
	jb_printf(jb, "\"%s\":{", name);
	const char *typeStr = "unknown";
	switch(tr->trType) {
		case TR_STATIONARY: typeStr = "stationary"; break;
		case TR_INTERPOLATE: typeStr = "interpolate"; break;
		case TR_LINEAR: typeStr = "linear"; break;
		case TR_LINEAR_STOP: typeStr = "linear_stop"; break;
		case TR_SINE: typeStr = "sine"; break;
		case TR_GRAVITY: typeStr = "gravity"; break;
	}
	jb_str(jb, "type", typeStr);
	jb_raw(jb, ",");
	jb_int(jb, "trTime", tr->trTime);
	jb_raw(jb, ",");
	jb_int(jb, "trDuration", tr->trDuration);
	jb_raw(jb, ",");
	jb_vec3(jb, "trBase", tr->trBase);
	jb_raw(jb, ",");
	jb_vec3(jb, "trDelta", tr->trDelta);
	jb_raw(jb, "}");
}

static void debug_write_entityState(jsonBuf_t *jb, const entityState_t *es) {
	jb_raw(jb, "\"s\":{");
	jb_int(jb, "number", es->number);
	jb_raw(jb, ","); jb_int(jb, "eType", es->eType);
	jb_raw(jb, ","); jb_int(jb, "eFlags", es->eFlags);
	jb_raw(jb, ","); debug_write_trajectory(jb, "pos", &es->pos);
	jb_raw(jb, ","); debug_write_trajectory(jb, "apos", &es->apos);
	jb_raw(jb, ","); jb_int(jb, "time", es->time);
	jb_raw(jb, ","); jb_int(jb, "time2", es->time2);
	jb_raw(jb, ","); jb_vec3(jb, "origin", es->origin);
	jb_raw(jb, ","); jb_vec3(jb, "origin2", es->origin2);
	jb_raw(jb, ","); jb_vec3(jb, "angles", es->angles);
	jb_raw(jb, ","); jb_vec3(jb, "angles2", es->angles2);
	jb_raw(jb, ","); jb_int(jb, "otherEntityNum", es->otherEntityNum);
	jb_raw(jb, ","); jb_int(jb, "otherEntityNum2", es->otherEntityNum2);
	jb_raw(jb, ","); jb_int(jb, "groundEntityNum", es->groundEntityNum);
	jb_raw(jb, ","); jb_int(jb, "constantLight", es->constantLight);
	jb_raw(jb, ","); jb_int(jb, "loopSound", es->loopSound);
	jb_raw(jb, ","); jb_int(jb, "modelindex", es->modelindex);
	jb_raw(jb, ","); jb_int(jb, "modelindex2", es->modelindex2);
	jb_raw(jb, ","); jb_int(jb, "clientNum", es->clientNum);
	jb_raw(jb, ","); jb_int(jb, "frame", es->frame);
	jb_raw(jb, ","); jb_int(jb, "solid", es->solid);
	jb_raw(jb, ","); jb_int(jb, "event", es->event);
	jb_raw(jb, ","); jb_int(jb, "eventParm", es->eventParm);
	jb_raw(jb, ","); jb_int(jb, "powerups", es->powerups);
	jb_raw(jb, ","); jb_int(jb, "weapon", es->weapon);
	jb_raw(jb, ","); jb_int(jb, "legsAnim", es->legsAnim);
	jb_raw(jb, ","); jb_int(jb, "torsoAnim", es->torsoAnim);
	jb_raw(jb, "}");
}

static void debug_write_entityShared(jsonBuf_t *jb, const entityShared_t *r) {
	jb_raw(jb, "\"r\":{");
	jb_bool(jb, "linked", r->linked);
	jb_raw(jb, ","); jb_int(jb, "linkcount", r->linkcount);
	jb_raw(jb, ","); jb_int(jb, "svFlags", r->svFlags);
	jb_raw(jb, ","); jb_int(jb, "singleClient", r->singleClient);
	jb_raw(jb, ","); jb_bool(jb, "bmodel", r->bmodel);
	jb_raw(jb, ","); jb_vec3(jb, "mins", r->mins);
	jb_raw(jb, ","); jb_vec3(jb, "maxs", r->maxs);
	jb_raw(jb, ","); jb_int(jb, "contents", r->contents);
	jb_raw(jb, ","); jb_vec3(jb, "absmin", r->absmin);
	jb_raw(jb, ","); jb_vec3(jb, "absmax", r->absmax);
	jb_raw(jb, ","); jb_vec3(jb, "currentOrigin", r->currentOrigin);
	jb_raw(jb, ","); jb_vec3(jb, "currentAngles", r->currentAngles);
	jb_raw(jb, ","); jb_int(jb, "ownerNum", r->ownerNum);

	// Decode svFlags into readable strings
	jb_raw(jb, ",\"svFlagsDecoded\":[");
	int flagFirst = 1;
	if (r->svFlags & SVF_NOCLIENT) { jb_raw(jb, "\"NOCLIENT\""); flagFirst = 0; }
	if (r->svFlags & SVF_BOT) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"BOT\""); flagFirst = 0; }
	if (r->svFlags & SVF_BROADCAST) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"BROADCAST\""); flagFirst = 0; }
	if (r->svFlags & SVF_PORTAL) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"PORTAL\""); flagFirst = 0; }
	if (r->svFlags & SVF_USE_CURRENT_ORIGIN) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"USE_CURRENT_ORIGIN\""); flagFirst = 0; }
	if (r->svFlags & SVF_SINGLECLIENT) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"SINGLECLIENT\""); flagFirst = 0; }
	if (r->svFlags & SVF_CAPSULE) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"CAPSULE\""); flagFirst = 0; }
	if (r->svFlags & SVF_NOTSINGLECLIENT) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"NOTSINGLECLIENT\""); flagFirst = 0; }
#ifdef ELITEFORCE
	if (r->svFlags & SVF_SHIELD_BBOX) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"SHIELD_BBOX\""); flagFirst = 0; }
	if (r->svFlags & SVF_ELIMINATED) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"ELIMINATED\""); flagFirst = 0; }
	if (r->svFlags & SVF_CLIENTMASK) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"CLIENTMASK\""); }
#else
	if (r->svFlags & SVF_CLIENTMASK) { if (!flagFirst) jb_raw(jb, ","); jb_raw(jb, "\"CLIENTMASK\""); }
#endif
	jb_raw(jb, "]");

	jb_raw(jb, "}");
}

static void debug_cmd_entity(jsonBuf_t *jb, int num) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "entity");

	if (!sv.gentities || num < 0 || num >= sv.num_entities) {
		jb_raw(jb, ",");
		jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}

	sharedEntity_t *ent = SV_GentityNum(num);
	jb_raw(jb, ",");
	jb_int(jb, "num", num);
	jb_raw(jb, ",");
	jb_bool(jb, "active", ent->r.linked);
	jb_raw(jb, ",");

	debug_write_entityState(jb, &ent->s);
	jb_raw(jb, ",");
	debug_write_entityShared(jb, &ent->r);

	// svEntity_t data from engine side
	svEntity_t *sve = &sv.svEntities[num];
	jb_raw(jb, ",\"sv\":{");
	jb_int(jb, "numClusters", sve->numClusters);
	jb_raw(jb, ",");
	jb_int(jb, "snapshotCounter", sve->snapshotCounter);
	jb_raw(jb, ",");
	jb_int(jb, "areanum", sve->areanum);
	jb_raw(jb, ",");
	jb_int(jb, "areanum2", sve->areanum2);
	jb_raw(jb, "}");

	jb_raw(jb, "}\n");
}

static void debug_cmd_entities(jsonBuf_t *jb, qboolean activeOnly) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "entity_list");
	jb_raw(jb, ",");
	jb_int(jb, "total", sv.num_entities);
	jb_raw(jb, ",\"entities\":[");

	if (sv.gentities) {
		int first = 1;
		int i;
		for (i = 0; i < sv.num_entities; i++) {
			sharedEntity_t *ent = SV_GentityNum(i);
			if (activeOnly && !ent->r.linked) continue;

			if (!first) jb_raw(jb, ",");
			first = 0;

			jb_raw(jb, "{");
			jb_int(jb, "num", i);
			jb_raw(jb, ","); jb_bool(jb, "linked", ent->r.linked);
			jb_raw(jb, ","); jb_int(jb, "eType", ent->s.eType);
			jb_raw(jb, ","); jb_int(jb, "eFlags", ent->s.eFlags);
			jb_raw(jb, ","); jb_vec3(jb, "origin", ent->s.origin);
			jb_raw(jb, ","); jb_int(jb, "modelindex", ent->s.modelindex);
			jb_raw(jb, ","); jb_int(jb, "svFlags", ent->r.svFlags);
			jb_raw(jb, ","); jb_int(jb, "clientNum", ent->s.clientNum);
			jb_raw(jb, ","); jb_int(jb, "weapon", ent->s.weapon);
			jb_raw(jb, ","); jb_int(jb, "contents", ent->r.contents);
			jb_raw(jb, "}");
		}
	}

	jb_raw(jb, "]}\n");
}

static void debug_cmd_player(jsonBuf_t *jb, int num) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "player");

	if (!sv.gameClients || num < 0 || num >= sv_maxclients->integer) {
		jb_raw(jb, ",");
		jb_str(jb, "error", "invalid client number");
		jb_raw(jb, "}\n");
		return;
	}

	client_t *cl = &svs.clients[num];
	if (cl->state == CS_FREE) {
		jb_raw(jb, ",");
		jb_str(jb, "error", "client slot is free");
		jb_raw(jb, "}\n");
		return;
	}

	playerState_t *ps = SV_GameClientNum(num);

	jb_raw(jb, ",");
	jb_int(jb, "num", num);
	jb_raw(jb, ",");
	jb_str(jb, "name", cl->name);
	jb_raw(jb, ",");
	jb_int(jb, "ping", cl->ping);

	jb_raw(jb, ",\"ps\":{");
	jb_int(jb, "commandTime", ps->commandTime);
	jb_raw(jb, ","); jb_int(jb, "pm_type", ps->pm_type);
	jb_raw(jb, ","); jb_int(jb, "pm_flags", ps->pm_flags);
	jb_raw(jb, ","); jb_int(jb, "pm_time", ps->pm_time);
	jb_raw(jb, ","); jb_int(jb, "bobCycle", ps->bobCycle);
	jb_raw(jb, ","); jb_vec3(jb, "origin", ps->origin);
	jb_raw(jb, ","); jb_vec3(jb, "velocity", ps->velocity);
	jb_raw(jb, ","); jb_int(jb, "weaponTime", ps->weaponTime);
#ifdef ELITEFORCE
	jb_raw(jb, ","); jb_int(jb, "rechargeTime", ps->rechargeTime);
	jb_raw(jb, ","); jb_int(jb, "useTime", ps->useTime);
	jb_raw(jb, ","); jb_int(jb, "introTime", ps->introTime);
#endif
	jb_raw(jb, ","); jb_int(jb, "gravity", ps->gravity);
	jb_raw(jb, ","); jb_int(jb, "speed", ps->speed);
	jb_raw(jb, ","); jb_int(jb, "groundEntityNum", ps->groundEntityNum);
	jb_raw(jb, ","); jb_int(jb, "legsTimer", ps->legsTimer);
	jb_raw(jb, ","); jb_int(jb, "legsAnim", ps->legsAnim);
	jb_raw(jb, ","); jb_int(jb, "torsoTimer", ps->torsoTimer);
	jb_raw(jb, ","); jb_int(jb, "torsoAnim", ps->torsoAnim);
	jb_raw(jb, ","); jb_int(jb, "movementDir", ps->movementDir);
	jb_raw(jb, ","); jb_int(jb, "eFlags", ps->eFlags);
	jb_raw(jb, ","); jb_int(jb, "eventSequence", ps->eventSequence);
	jb_raw(jb, ","); jb_int(jb, "clientNum", ps->clientNum);
	jb_raw(jb, ","); jb_int(jb, "weapon", ps->weapon);
	jb_raw(jb, ","); jb_int(jb, "weaponstate", ps->weaponstate);
	jb_raw(jb, ","); jb_vec3(jb, "viewangles", ps->viewangles);
	jb_raw(jb, ","); jb_int(jb, "viewheight", ps->viewheight);
	jb_raw(jb, ","); jb_int(jb, "damageEvent", ps->damageEvent);
	jb_raw(jb, ","); jb_int(jb, "damageYaw", ps->damageYaw);
	jb_raw(jb, ","); jb_int(jb, "damagePitch", ps->damagePitch);
	jb_raw(jb, ","); jb_int(jb, "damageCount", ps->damageCount);
#ifdef ELITEFORCE
	jb_raw(jb, ","); jb_int(jb, "damageShieldCount", ps->damageShieldCount);
#endif

	// Stats array
	jb_raw(jb, ",\"stats\":[");
	{ int i; for (i = 0; i < MAX_STATS; i++) {
		if (i) jb_raw(jb, ",");
		jb_printf(jb, "%d", ps->stats[i]);
	}}
	jb_raw(jb, "]");

	// Persistant array
	jb_raw(jb, ",\"persistant\":[");
	{ int i; for (i = 0; i < MAX_PERSISTANT; i++) {
		if (i) jb_raw(jb, ",");
		jb_printf(jb, "%d", ps->persistant[i]);
	}}
	jb_raw(jb, "]");

	// Powerups array
	jb_raw(jb, ",\"powerups\":[");
	{ int i; for (i = 0; i < MAX_POWERUPS; i++) {
		if (i) jb_raw(jb, ",");
		jb_printf(jb, "%d", ps->powerups[i]);
	}}
	jb_raw(jb, "]");

	// Ammo array
	jb_raw(jb, ",\"ammo\":[");
	{ int i; for (i = 0; i < MAX_WEAPONS; i++) {
		if (i) jb_raw(jb, ",");
		jb_printf(jb, "%d", ps->ammo[i]);
	}}
	jb_raw(jb, "]");

	jb_raw(jb, ","); jb_int(jb, "ping", ps->ping);
	jb_raw(jb, ","); jb_int(jb, "entityEventSequence", ps->entityEventSequence);
	jb_raw(jb, "}");

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Memory layout reporting - uses offsetof() for compile-time accuracy
// ------------------------------------------------------------------

#define LAYOUT_FIELD(jb, type, field, first) do { \
	if (!first) jb_raw(jb, ","); \
	jb_raw(jb, "{"); \
	jb_str(jb, "name", #field); \
	jb_raw(jb, ","); jb_int(jb, "offset", (int)offsetof(type, field)); \
	jb_raw(jb, ","); jb_int(jb, "size", (int)sizeof(((type*)0)->field)); \
	jb_raw(jb, "}"); \
	first = 0; \
} while(0)

static void debug_cmd_layout(jsonBuf_t *jb, const char *structName) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "layout");
	jb_raw(jb, ",");
	jb_str(jb, "struct", structName);

	if (!strcmp(structName, "entityState_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(entityState_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, entityState_t, number, f);
		LAYOUT_FIELD(jb, entityState_t, eType, f);
		LAYOUT_FIELD(jb, entityState_t, eFlags, f);
		LAYOUT_FIELD(jb, entityState_t, pos, f);
		LAYOUT_FIELD(jb, entityState_t, apos, f);
		LAYOUT_FIELD(jb, entityState_t, time, f);
		LAYOUT_FIELD(jb, entityState_t, time2, f);
		LAYOUT_FIELD(jb, entityState_t, origin, f);
		LAYOUT_FIELD(jb, entityState_t, origin2, f);
		LAYOUT_FIELD(jb, entityState_t, angles, f);
		LAYOUT_FIELD(jb, entityState_t, angles2, f);
		LAYOUT_FIELD(jb, entityState_t, otherEntityNum, f);
		LAYOUT_FIELD(jb, entityState_t, otherEntityNum2, f);
		LAYOUT_FIELD(jb, entityState_t, groundEntityNum, f);
		LAYOUT_FIELD(jb, entityState_t, constantLight, f);
		LAYOUT_FIELD(jb, entityState_t, loopSound, f);
		LAYOUT_FIELD(jb, entityState_t, modelindex, f);
		LAYOUT_FIELD(jb, entityState_t, modelindex2, f);
		LAYOUT_FIELD(jb, entityState_t, clientNum, f);
		LAYOUT_FIELD(jb, entityState_t, frame, f);
		LAYOUT_FIELD(jb, entityState_t, solid, f);
		LAYOUT_FIELD(jb, entityState_t, event, f);
		LAYOUT_FIELD(jb, entityState_t, eventParm, f);
		LAYOUT_FIELD(jb, entityState_t, powerups, f);
		LAYOUT_FIELD(jb, entityState_t, weapon, f);
		LAYOUT_FIELD(jb, entityState_t, legsAnim, f);
		LAYOUT_FIELD(jb, entityState_t, torsoAnim, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "entityShared_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(entityShared_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, entityShared_t, linked, f);
		LAYOUT_FIELD(jb, entityShared_t, linkcount, f);
		LAYOUT_FIELD(jb, entityShared_t, svFlags, f);
		LAYOUT_FIELD(jb, entityShared_t, singleClient, f);
		LAYOUT_FIELD(jb, entityShared_t, bmodel, f);
		LAYOUT_FIELD(jb, entityShared_t, mins, f);
		LAYOUT_FIELD(jb, entityShared_t, maxs, f);
		LAYOUT_FIELD(jb, entityShared_t, contents, f);
		LAYOUT_FIELD(jb, entityShared_t, absmin, f);
		LAYOUT_FIELD(jb, entityShared_t, absmax, f);
		LAYOUT_FIELD(jb, entityShared_t, currentOrigin, f);
		LAYOUT_FIELD(jb, entityShared_t, currentAngles, f);
		LAYOUT_FIELD(jb, entityShared_t, ownerNum, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "playerState_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(playerState_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, playerState_t, commandTime, f);
		LAYOUT_FIELD(jb, playerState_t, pm_type, f);
		LAYOUT_FIELD(jb, playerState_t, bobCycle, f);
		LAYOUT_FIELD(jb, playerState_t, pm_flags, f);
		LAYOUT_FIELD(jb, playerState_t, pm_time, f);
		LAYOUT_FIELD(jb, playerState_t, origin, f);
		LAYOUT_FIELD(jb, playerState_t, velocity, f);
		LAYOUT_FIELD(jb, playerState_t, weaponTime, f);
#ifdef ELITEFORCE
		LAYOUT_FIELD(jb, playerState_t, rechargeTime, f);
		LAYOUT_FIELD(jb, playerState_t, useTime, f);
		LAYOUT_FIELD(jb, playerState_t, introTime, f);
#endif
		LAYOUT_FIELD(jb, playerState_t, gravity, f);
		LAYOUT_FIELD(jb, playerState_t, speed, f);
		LAYOUT_FIELD(jb, playerState_t, delta_angles, f);
		LAYOUT_FIELD(jb, playerState_t, groundEntityNum, f);
		LAYOUT_FIELD(jb, playerState_t, legsTimer, f);
		LAYOUT_FIELD(jb, playerState_t, legsAnim, f);
		LAYOUT_FIELD(jb, playerState_t, torsoTimer, f);
		LAYOUT_FIELD(jb, playerState_t, torsoAnim, f);
		LAYOUT_FIELD(jb, playerState_t, movementDir, f);
		LAYOUT_FIELD(jb, playerState_t, eFlags, f);
		LAYOUT_FIELD(jb, playerState_t, eventSequence, f);
		LAYOUT_FIELD(jb, playerState_t, events, f);
		LAYOUT_FIELD(jb, playerState_t, eventParms, f);
		LAYOUT_FIELD(jb, playerState_t, clientNum, f);
		LAYOUT_FIELD(jb, playerState_t, weapon, f);
		LAYOUT_FIELD(jb, playerState_t, weaponstate, f);
		LAYOUT_FIELD(jb, playerState_t, viewangles, f);
		LAYOUT_FIELD(jb, playerState_t, viewheight, f);
		LAYOUT_FIELD(jb, playerState_t, damageEvent, f);
		LAYOUT_FIELD(jb, playerState_t, damageYaw, f);
		LAYOUT_FIELD(jb, playerState_t, damagePitch, f);
		LAYOUT_FIELD(jb, playerState_t, damageCount, f);
		LAYOUT_FIELD(jb, playerState_t, stats, f);
		LAYOUT_FIELD(jb, playerState_t, persistant, f);
		LAYOUT_FIELD(jb, playerState_t, powerups, f);
		LAYOUT_FIELD(jb, playerState_t, ammo, f);
		LAYOUT_FIELD(jb, playerState_t, ping, f);
		LAYOUT_FIELD(jb, playerState_t, entityEventSequence, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "sp_entityState_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(sp_entityState_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, sp_entityState_t, number, f);
		LAYOUT_FIELD(jb, sp_entityState_t, eType, f);
		LAYOUT_FIELD(jb, sp_entityState_t, eFlags, f);
		LAYOUT_FIELD(jb, sp_entityState_t, pos, f);
		LAYOUT_FIELD(jb, sp_entityState_t, apos, f);
		LAYOUT_FIELD(jb, sp_entityState_t, time, f);
		LAYOUT_FIELD(jb, sp_entityState_t, time2, f);
		LAYOUT_FIELD(jb, sp_entityState_t, origin, f);
		LAYOUT_FIELD(jb, sp_entityState_t, origin2, f);
		LAYOUT_FIELD(jb, sp_entityState_t, angles, f);
		LAYOUT_FIELD(jb, sp_entityState_t, angles2, f);
		LAYOUT_FIELD(jb, sp_entityState_t, otherEntityNum, f);
		LAYOUT_FIELD(jb, sp_entityState_t, otherEntityNum2, f);
		LAYOUT_FIELD(jb, sp_entityState_t, groundEntityNum, f);
		LAYOUT_FIELD(jb, sp_entityState_t, constantLight, f);
		LAYOUT_FIELD(jb, sp_entityState_t, loopSound, f);
		LAYOUT_FIELD(jb, sp_entityState_t, modelindex, f);
		LAYOUT_FIELD(jb, sp_entityState_t, modelindex2, f);
		LAYOUT_FIELD(jb, sp_entityState_t, modelindex3, f);
		LAYOUT_FIELD(jb, sp_entityState_t, clientNum, f);
		LAYOUT_FIELD(jb, sp_entityState_t, frame, f);
		LAYOUT_FIELD(jb, sp_entityState_t, solid, f);
		LAYOUT_FIELD(jb, sp_entityState_t, event, f);
		LAYOUT_FIELD(jb, sp_entityState_t, eventParm, f);
		LAYOUT_FIELD(jb, sp_entityState_t, powerups, f);
		LAYOUT_FIELD(jb, sp_entityState_t, weapon, f);
		LAYOUT_FIELD(jb, sp_entityState_t, legsAnim, f);
		LAYOUT_FIELD(jb, sp_entityState_t, legsAnimTimer, f);
		LAYOUT_FIELD(jb, sp_entityState_t, torsoAnim, f);
		LAYOUT_FIELD(jb, sp_entityState_t, torsoAnimTimer, f);
		LAYOUT_FIELD(jb, sp_entityState_t, scale, f);
		LAYOUT_FIELD(jb, sp_entityState_t, pushVec, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "sp_playerState_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(sp_playerState_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, sp_playerState_t, commandTime, f);
		LAYOUT_FIELD(jb, sp_playerState_t, pm_type, f);
		LAYOUT_FIELD(jb, sp_playerState_t, bobCycle, f);
		LAYOUT_FIELD(jb, sp_playerState_t, pm_flags, f);
		LAYOUT_FIELD(jb, sp_playerState_t, pm_time, f);
		LAYOUT_FIELD(jb, sp_playerState_t, origin, f);
		LAYOUT_FIELD(jb, sp_playerState_t, velocity, f);
		LAYOUT_FIELD(jb, sp_playerState_t, weaponTime, f);
		LAYOUT_FIELD(jb, sp_playerState_t, rechargeTime, f);
		LAYOUT_FIELD(jb, sp_playerState_t, useTime, f);
		LAYOUT_FIELD(jb, sp_playerState_t, gravity, f);
		LAYOUT_FIELD(jb, sp_playerState_t, leanofs, f);
		LAYOUT_FIELD(jb, sp_playerState_t, friction, f);
		LAYOUT_FIELD(jb, sp_playerState_t, speed, f);
		LAYOUT_FIELD(jb, sp_playerState_t, delta_angles, f);
		LAYOUT_FIELD(jb, sp_playerState_t, groundEntityNum, f);
		LAYOUT_FIELD(jb, sp_playerState_t, legsAnim, f);
		LAYOUT_FIELD(jb, sp_playerState_t, legsAnimTimer, f);
		LAYOUT_FIELD(jb, sp_playerState_t, torsoAnim, f);
		LAYOUT_FIELD(jb, sp_playerState_t, torsoAnimTimer, f);
		LAYOUT_FIELD(jb, sp_playerState_t, scale, f);
		LAYOUT_FIELD(jb, sp_playerState_t, movementDir, f);
		LAYOUT_FIELD(jb, sp_playerState_t, eFlags, f);
		LAYOUT_FIELD(jb, sp_playerState_t, eventSequence, f);
		LAYOUT_FIELD(jb, sp_playerState_t, events, f);
		LAYOUT_FIELD(jb, sp_playerState_t, eventParms, f);
		LAYOUT_FIELD(jb, sp_playerState_t, externalEvent, f);
		LAYOUT_FIELD(jb, sp_playerState_t, externalEventParm, f);
		LAYOUT_FIELD(jb, sp_playerState_t, externalEventTime, f);
		LAYOUT_FIELD(jb, sp_playerState_t, clientNum, f);
		LAYOUT_FIELD(jb, sp_playerState_t, weapon, f);
		LAYOUT_FIELD(jb, sp_playerState_t, weaponstate, f);
		LAYOUT_FIELD(jb, sp_playerState_t, viewangles, f);
		LAYOUT_FIELD(jb, sp_playerState_t, viewheight, f);
		LAYOUT_FIELD(jb, sp_playerState_t, damageEvent, f);
		LAYOUT_FIELD(jb, sp_playerState_t, damageYaw, f);
		LAYOUT_FIELD(jb, sp_playerState_t, damagePitch, f);
		LAYOUT_FIELD(jb, sp_playerState_t, damageCount, f);
		LAYOUT_FIELD(jb, sp_playerState_t, stats, f);
		LAYOUT_FIELD(jb, sp_playerState_t, persistant, f);
		LAYOUT_FIELD(jb, sp_playerState_t, powerups, f);
		LAYOUT_FIELD(jb, sp_playerState_t, ammo, f);
		LAYOUT_FIELD(jb, sp_playerState_t, borgAdaptHits, f);
		LAYOUT_FIELD(jb, sp_playerState_t, pushVec, f);
		LAYOUT_FIELD(jb, sp_playerState_t, ping, f);
		LAYOUT_FIELD(jb, sp_playerState_t, leanStopDebounceTime, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "sp_snapshot_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(sp_snapshot_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, sp_snapshot_t, snapFlags, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, ping, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, serverTime, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, areamask, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, cmdNum, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, ps, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, numEntities, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, entities, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, numConfigstringChanges, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, configstringNum, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, numServerCommands, f);
		LAYOUT_FIELD(jb, sp_snapshot_t, serverCommandSequence, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "sharedEntity_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(sharedEntity_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, sharedEntity_t, s, f);
		LAYOUT_FIELD(jb, sharedEntity_t, r, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "trajectory_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(trajectory_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, trajectory_t, trType, f);
		LAYOUT_FIELD(jb, trajectory_t, trTime, f);
		LAYOUT_FIELD(jb, trajectory_t, trDuration, f);
		LAYOUT_FIELD(jb, trajectory_t, trBase, f);
		LAYOUT_FIELD(jb, trajectory_t, trDelta, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "usercmd_t")) {
		jb_raw(jb, ",");
		jb_int(jb, "totalSize", (int)sizeof(usercmd_t));
		jb_raw(jb, ",\"fields\":[");
		int f = 1;
		LAYOUT_FIELD(jb, usercmd_t, serverTime, f);
#ifdef ELITEFORCE
		LAYOUT_FIELD(jb, usercmd_t, buttons, f);
		LAYOUT_FIELD(jb, usercmd_t, weapon, f);
#endif
		LAYOUT_FIELD(jb, usercmd_t, angles, f);
#ifndef ELITEFORCE
		LAYOUT_FIELD(jb, usercmd_t, buttons, f);
		LAYOUT_FIELD(jb, usercmd_t, weapon, f);
#endif
		LAYOUT_FIELD(jb, usercmd_t, forwardmove, f);
		LAYOUT_FIELD(jb, usercmd_t, rightmove, f);
		LAYOUT_FIELD(jb, usercmd_t, upmove, f);
		jb_raw(jb, "]");

	} else if (!strcmp(structName, "all")) {
		// Return summary of all known struct sizes
		jb_raw(jb, ",\"structs\":{");
		jb_int(jb, "entityState_t", (int)sizeof(entityState_t));
		jb_raw(jb, ","); jb_int(jb, "entityShared_t", (int)sizeof(entityShared_t));
		jb_raw(jb, ","); jb_int(jb, "sharedEntity_t", (int)sizeof(sharedEntity_t));
		jb_raw(jb, ","); jb_int(jb, "playerState_t", (int)sizeof(playerState_t));
		jb_raw(jb, ","); jb_int(jb, "sp_entityState_t", (int)sizeof(sp_entityState_t));
		jb_raw(jb, ","); jb_int(jb, "sp_playerState_t", (int)sizeof(sp_playerState_t));
		jb_raw(jb, ","); jb_int(jb, "sp_snapshot_t", (int)sizeof(sp_snapshot_t));
		jb_raw(jb, ","); jb_int(jb, "trajectory_t", (int)sizeof(trajectory_t));
		jb_raw(jb, ","); jb_int(jb, "usercmd_t", (int)sizeof(usercmd_t));
		jb_raw(jb, ","); jb_int(jb, "svEntity_t", (int)sizeof(svEntity_t));
		jb_raw(jb, ","); jb_int(jb, "gentitySize_runtime", sv.gentitySize);
		jb_raw(jb, ","); jb_int(jb, "gameClientSize_runtime", sv.gameClientSize);
		jb_raw(jb, "}");

	} else {
		jb_raw(jb, ",");
		jb_str(jb, "error", "unknown struct name");
		jb_raw(jb, ",");
		jb_str(jb, "available", "entityState_t, entityShared_t, playerState_t, sp_entityState_t, sp_playerState_t, sp_snapshot_t, sharedEntity_t, trajectory_t, usercmd_t, all");
	}

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Struct field validation
// ------------------------------------------------------------------

static void debug_cmd_validate(jsonBuf_t *jb, int num) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "validation");
	jb_raw(jb, ",");
	jb_int(jb, "num", num);

	if (!sv.gentities || num < 0 || num >= sv.num_entities) {
		jb_raw(jb, ",");
		jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}

	sharedEntity_t *ent = SV_GentityNum(num);
	jb_raw(jb, ",\"issues\":[");
	int issueFirst = 1;

	#define ISSUE(msg) do { \
		if (!issueFirst) jb_raw(jb, ","); \
		jb_printf(jb, "\"%s\"", msg); \
		issueFirst = 0; \
	} while(0)

	// Entity number consistency
	if (ent->s.number != num) ISSUE("s.number does not match entity index");

	// Origin NaN check
	if (IS_NAN(ent->s.origin[0]) || IS_NAN(ent->s.origin[1]) || IS_NAN(ent->s.origin[2]))
		ISSUE("s.origin contains NaN");
	if (IS_INF(ent->s.origin[0]) || IS_INF(ent->s.origin[1]) || IS_INF(ent->s.origin[2]))
		ISSUE("s.origin contains Inf");

	// Origin reasonable range (>65k units is suspicious in Q3)
	if (fabs(ent->s.origin[0]) > 65536.0f || fabs(ent->s.origin[1]) > 65536.0f || fabs(ent->s.origin[2]) > 65536.0f)
		ISSUE("s.origin is beyond map bounds (>65536)");

	// Trajectory base NaN
	if (IS_NAN(ent->s.pos.trBase[0]) || IS_NAN(ent->s.pos.trBase[1]) || IS_NAN(ent->s.pos.trBase[2]))
		ISSUE("pos.trBase contains NaN");
	if (IS_NAN(ent->s.pos.trDelta[0]) || IS_NAN(ent->s.pos.trDelta[1]) || IS_NAN(ent->s.pos.trDelta[2]))
		ISSUE("pos.trDelta contains NaN");

	// Angles sanity
	if (IS_NAN(ent->s.angles[0]) || IS_NAN(ent->s.angles[1]) || IS_NAN(ent->s.angles[2]))
		ISSUE("s.angles contains NaN");

	// Velocity in trajectory
	if (IS_NAN(ent->s.apos.trBase[0]) || IS_NAN(ent->s.apos.trBase[1]) || IS_NAN(ent->s.apos.trBase[2]))
		ISSUE("apos.trBase contains NaN");

	// Entity type range
	if (ent->s.eType < 0 || ent->s.eType > 255)
		ISSUE("eType out of expected range (0-255)");

	// Linked but no contents and not noclient - might be an issue
	if (ent->r.linked && ent->r.contents == 0 && !(ent->r.svFlags & SVF_NOCLIENT) && ent->s.eType == 0)
		ISSUE("linked entity with zero contents, no svFlags, and eType 0 - possibly uninitialized");

	// Bounding box checks
	if (ent->r.linked) {
		if (ent->r.mins[0] > ent->r.maxs[0] || ent->r.mins[1] > ent->r.maxs[1] || ent->r.mins[2] > ent->r.maxs[2])
			ISSUE("mins > maxs (inverted bounding box)");
		if (ent->r.absmin[0] > ent->r.absmax[0] || ent->r.absmin[1] > ent->r.absmax[1] || ent->r.absmin[2] > ent->r.absmax[2])
			ISSUE("absmin > absmax (inverted absolute bounds)");
	}

	// currentOrigin vs s.origin drift check (if USE_CURRENT_ORIGIN)
	if (ent->r.svFlags & SVF_USE_CURRENT_ORIGIN) {
		float dx = ent->r.currentOrigin[0] - ent->s.origin[0];
		float dy = ent->r.currentOrigin[1] - ent->s.origin[1];
		float dz = ent->r.currentOrigin[2] - ent->s.origin[2];
		float dist = (float)sqrt(dx*dx + dy*dy + dz*dz);
		if (dist > 10000.0f)
			ISSUE("currentOrigin extremely far from s.origin (>10000 units)");
	}

	// otherEntityNum range
	if (ent->s.otherEntityNum < 0 || ent->s.otherEntityNum >= MAX_GENTITIES)
		if (ent->s.otherEntityNum != ENTITYNUM_NONE)
			ISSUE("otherEntityNum out of range");

	// groundEntityNum range
	if (ent->s.groundEntityNum < 0 || ent->s.groundEntityNum >= MAX_GENTITIES)
		if (ent->s.groundEntityNum != ENTITYNUM_NONE)
			ISSUE("groundEntityNum out of range");

	// ownerNum range
	if (ent->r.ownerNum < 0 || ent->r.ownerNum >= MAX_GENTITIES)
		if (ent->r.ownerNum != ENTITYNUM_NONE)
			ISSUE("r.ownerNum out of range");

	// Trajectory type range
	if (ent->s.pos.trType < TR_STATIONARY || ent->s.pos.trType > TR_GRAVITY)
		ISSUE("pos.trType has invalid value");
	if (ent->s.apos.trType < TR_STATIONARY || ent->s.apos.trType > TR_GRAVITY)
		ISSUE("apos.trType has invalid value");

	#undef ISSUE

	jb_raw(jb, "],");
	jb_bool(jb, "valid", issueFirst);  // valid if no issues were found

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Entity search
// ------------------------------------------------------------------

static void debug_cmd_search(jsonBuf_t *jb, const char *field, const char *op, int value) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "search_results");
	jb_raw(jb, ",");
	jb_str(jb, "field", field);
	jb_raw(jb, ",");
	jb_str(jb, "op", op[0] ? op : "eq");
	jb_raw(jb, ",");
	jb_int(jb, "value", value);
	jb_raw(jb, ",\"matches\":[");

	if (sv.gentities) {
		int first = 1;
		int i;
		for (i = 0; i < sv.num_entities; i++) {
			sharedEntity_t *ent = SV_GentityNum(i);
			int fieldVal = 0;
			qboolean found = qfalse;

			// Map field name to actual value
			if (!strcmp(field, "eType")) { fieldVal = ent->s.eType; found = qtrue; }
			else if (!strcmp(field, "eFlags")) { fieldVal = ent->s.eFlags; found = qtrue; }
			else if (!strcmp(field, "weapon")) { fieldVal = ent->s.weapon; found = qtrue; }
			else if (!strcmp(field, "modelindex")) { fieldVal = ent->s.modelindex; found = qtrue; }
			else if (!strcmp(field, "clientNum")) { fieldVal = ent->s.clientNum; found = qtrue; }
			else if (!strcmp(field, "svFlags")) { fieldVal = ent->r.svFlags; found = qtrue; }
			else if (!strcmp(field, "contents")) { fieldVal = ent->r.contents; found = qtrue; }
			else if (!strcmp(field, "linked")) { fieldVal = ent->r.linked; found = qtrue; }
			else if (!strcmp(field, "groundEntityNum")) { fieldVal = ent->s.groundEntityNum; found = qtrue; }
			else if (!strcmp(field, "solid")) { fieldVal = ent->s.solid; found = qtrue; }
			else if (!strcmp(field, "event")) { fieldVal = ent->s.event; found = qtrue; }
			else if (!strcmp(field, "powerups")) { fieldVal = ent->s.powerups; found = qtrue; }
			else if (!strcmp(field, "ownerNum")) { fieldVal = ent->r.ownerNum; found = qtrue; }

			if (!found) continue;

			qboolean match = qfalse;
			if (!strcmp(op, "eq") || !op[0]) match = (fieldVal == value);
			else if (!strcmp(op, "ne") || !strcmp(op, "neq")) match = (fieldVal != value);
			else if (!strcmp(op, "gt")) match = (fieldVal > value);
			else if (!strcmp(op, "lt")) match = (fieldVal < value);
			else if (!strcmp(op, "gte") || !strcmp(op, "ge")) match = (fieldVal >= value);
			else if (!strcmp(op, "lte") || !strcmp(op, "le")) match = (fieldVal <= value);
			else if (!strcmp(op, "and") || !strcmp(op, "mask")) match = ((fieldVal & value) != 0);

			if (match) {
				if (!first) jb_raw(jb, ",");
				first = 0;
				jb_raw(jb, "{");
				jb_int(jb, "num", i);
				jb_raw(jb, ","); jb_int(jb, "fieldValue", fieldVal);
				jb_raw(jb, ","); jb_int(jb, "eType", ent->s.eType);
				jb_raw(jb, ","); jb_vec3(jb, "origin", ent->s.origin);
				jb_raw(jb, ","); jb_bool(jb, "linked", ent->r.linked);
				jb_raw(jb, "}");
			}
		}
	}

	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Cvar query
// ------------------------------------------------------------------

static void debug_cmd_cvar(jsonBuf_t *jb, const char *name) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "cvar");
	jb_raw(jb, ",");
	jb_str(jb, "name", name);
	jb_raw(jb, ",");
	jb_str(jb, "value", Cvar_VariableString(name));
	jb_raw(jb, ",");
	jb_int(jb, "integer", Cvar_VariableIntegerValue(name));
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Validate all entities (bulk scan)
// ------------------------------------------------------------------

static void debug_cmd_validate_all(jsonBuf_t *jb) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "validation_summary");

	if (!sv.gentities) {
		jb_raw(jb, ",");
		jb_str(jb, "error", "no game entities loaded");
		jb_raw(jb, "}\n");
		return;
	}

	int totalIssues = 0;
	int entitiesWithIssues = 0;

	jb_raw(jb, ",\"entities_with_issues\":[");
	int first = 1;
	int i;

	for (i = 0; i < sv.num_entities; i++) {
		sharedEntity_t *ent = SV_GentityNum(i);
		if (!ent->r.linked && i >= sv_maxclients->integer) continue;  // skip unlinked non-client entities

		// Quick validation checks
		int issues = 0;
		if (ent->s.number != i) issues++;
		if (IS_NAN(ent->s.origin[0]) || IS_NAN(ent->s.origin[1]) || IS_NAN(ent->s.origin[2])) issues++;
		if (IS_NAN(ent->s.pos.trBase[0]) || IS_NAN(ent->s.pos.trBase[1]) || IS_NAN(ent->s.pos.trBase[2])) issues++;
		if (IS_NAN(ent->s.angles[0]) || IS_NAN(ent->s.angles[1]) || IS_NAN(ent->s.angles[2])) issues++;
		if (ent->r.linked && (ent->r.mins[0] > ent->r.maxs[0])) issues++;
		if (ent->s.pos.trType < TR_STATIONARY || ent->s.pos.trType > TR_GRAVITY) issues++;
		if (fabs(ent->s.origin[0]) > 65536.0f || fabs(ent->s.origin[1]) > 65536.0f || fabs(ent->s.origin[2]) > 65536.0f) issues++;

		if (issues > 0) {
			if (!first) jb_raw(jb, ",");
			first = 0;
			jb_printf(jb, "{\"num\":%d,\"issues\":%d,\"eType\":%d,\"linked\":%s}",
				i, issues, ent->s.eType, ent->r.linked ? "true" : "false");
			entitiesWithIssues++;
			totalIssues += issues;
		}
	}

	jb_raw(jb, "],");
	jb_int(jb, "totalEntities", sv.num_entities);
	jb_raw(jb, ",");
	jb_int(jb, "entitiesChecked", sv.num_entities);
	jb_raw(jb, ",");
	jb_int(jb, "entitiesWithIssues", entitiesWithIssues);
	jb_raw(jb, ",");
	jb_int(jb, "totalIssues", totalIssues);

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Cvar list — walk the entire cvar linked list
// ------------------------------------------------------------------

extern cvar_t *cvar_vars;

static void debug_cmd_cvarlist(jsonBuf_t *jb, const char *filter) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "cvarlist");
	jb_raw(jb, ",\"cvars\":[");

	cvar_t *cv;
	int first = 1;
	int count = 0;
	for (cv = cvar_vars; cv; cv = cv->next) {
		if (filter[0] && !strstr(cv->name, filter)) continue;
		if (!first) jb_raw(jb, ",");
		first = 0;
		jb_raw(jb, "{");
		jb_str(jb, "name", cv->name);
		jb_raw(jb, ","); jb_str(jb, "value", cv->string);
		jb_raw(jb, ","); jb_int(jb, "integer", cv->integer);
		jb_raw(jb, ","); jb_int(jb, "flags", cv->flags);
		if (cv->latchedString) { jb_raw(jb, ","); jb_str(jb, "latched", cv->latchedString); }
		if (cv->resetString) { jb_raw(jb, ","); jb_str(jb, "default", cv->resetString); }
		if (cv->description) { jb_raw(jb, ","); jb_str(jb, "description", cv->description); }
		jb_raw(jb, "}");
		count++;
	}

	jb_raw(jb, "],");
	jb_int(jb, "count", count);
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Configstring dump
// ------------------------------------------------------------------

static void debug_cmd_configstrings(jsonBuf_t *jb, int startIdx, int endIdx) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "configstrings");

	if (endIdx <= 0 || endIdx > MAX_CONFIGSTRINGS) endIdx = MAX_CONFIGSTRINGS;
	if (startIdx < 0) startIdx = 0;

	jb_raw(jb, ",\"strings\":[");
	int first = 1;
	int i;
	for (i = startIdx; i < endIdx; i++) {
		if (!sv.configstrings[i] || !sv.configstrings[i][0]) continue;
		if (!first) jb_raw(jb, ",");
		first = 0;
		jb_raw(jb, "{");
		jb_int(jb, "index", i);
		jb_raw(jb, ",");
		jb_str(jb, "value", sv.configstrings[i]);
		jb_raw(jb, "}");
	}
	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Log ring buffer — capture/query
// ------------------------------------------------------------------

void DebugServer_LogCapture(const char *text) {
	if (!debugInitialized) return;

	const char *p = text;
	while (*p) {
		if (*p == '\n' || logLineBufLen >= LOG_LINE_MAX - 1) {
			logLineBuf[logLineBufLen] = '\0';
			if (logLineBufLen > 0) {
				Q_strncpyz(logRing[logRingHead % LOG_RING_LINES], logLineBuf, LOG_LINE_MAX);
				logRingHead++;
				logRingCount++;
			}
			logLineBufLen = 0;
			if (*p == '\n') p++;
			continue;
		}
		logLineBuf[logLineBufLen++] = *p;
		p++;
	}
}

static void debug_cmd_log(jsonBuf_t *jb, int sinceId, int maxLines, const char *filter) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "log");

	if (maxLines <= 0 || maxLines > LOG_RING_LINES) maxLines = 100;

	int startIdx = logRingHead - LOG_RING_LINES;
	if (startIdx < 0) startIdx = 0;
	if (sinceId > startIdx) startIdx = sinceId;

	jb_raw(jb, ",\"lines\":[");
	int first = 1;
	int count = 0;
	int i;
	for (i = startIdx; i < logRingHead && count < maxLines; i++) {
		const char *line = logRing[i % LOG_RING_LINES];
		if (filter[0] && !strstr(line, filter)) continue;
		if (!first) jb_raw(jb, ",");
		first = 0;
		jb_raw(jb, "{");
		jb_int(jb, "id", i);
		jb_raw(jb, ",");
		jb_str(jb, "text", line);
		jb_raw(jb, "}");
		count++;
	}
	jb_raw(jb, "],");
	jb_int(jb, "nextId", logRingHead);
	jb_raw(jb, ",");
	jb_int(jb, "totalCaptured", logRingCount);
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Entity modification — write fields on sharedEntity_t
// ------------------------------------------------------------------

static int debug_get_entity_field(sharedEntity_t *ent, const char *field) {
	if (!strcmp(field, "eType")) return ent->s.eType;
	if (!strcmp(field, "eFlags")) return ent->s.eFlags;
	if (!strcmp(field, "weapon")) return ent->s.weapon;
	if (!strcmp(field, "modelindex")) return ent->s.modelindex;
	if (!strcmp(field, "modelindex2")) return ent->s.modelindex2;
	if (!strcmp(field, "clientNum")) return ent->s.clientNum;
	if (!strcmp(field, "frame")) return ent->s.frame;
	if (!strcmp(field, "solid")) return ent->s.solid;
	if (!strcmp(field, "event")) return ent->s.event;
	if (!strcmp(field, "eventParm")) return ent->s.eventParm;
	if (!strcmp(field, "powerups")) return ent->s.powerups;
	if (!strcmp(field, "loopSound")) return ent->s.loopSound;
	if (!strcmp(field, "constantLight")) return ent->s.constantLight;
	if (!strcmp(field, "otherEntityNum")) return ent->s.otherEntityNum;
	if (!strcmp(field, "groundEntityNum")) return ent->s.groundEntityNum;
	if (!strcmp(field, "svFlags")) return ent->r.svFlags;
	if (!strcmp(field, "contents")) return ent->r.contents;
	if (!strcmp(field, "ownerNum")) return ent->r.ownerNum;
	if (!strcmp(field, "legsAnim")) return ent->s.legsAnim;
	if (!strcmp(field, "torsoAnim")) return ent->s.torsoAnim;
	return 0;
}

static qboolean debug_set_entity_field(sharedEntity_t *ent, const char *field, int value) {
	if (!strcmp(field, "eType")) { ent->s.eType = value; return qtrue; }
	if (!strcmp(field, "eFlags")) { ent->s.eFlags = value; return qtrue; }
	if (!strcmp(field, "weapon")) { ent->s.weapon = value; return qtrue; }
	if (!strcmp(field, "modelindex")) { ent->s.modelindex = value; return qtrue; }
	if (!strcmp(field, "modelindex2")) { ent->s.modelindex2 = value; return qtrue; }
	if (!strcmp(field, "clientNum")) { ent->s.clientNum = value; return qtrue; }
	if (!strcmp(field, "frame")) { ent->s.frame = value; return qtrue; }
	if (!strcmp(field, "solid")) { ent->s.solid = value; return qtrue; }
	if (!strcmp(field, "event")) { ent->s.event = value; return qtrue; }
	if (!strcmp(field, "eventParm")) { ent->s.eventParm = value; return qtrue; }
	if (!strcmp(field, "powerups")) { ent->s.powerups = value; return qtrue; }
	if (!strcmp(field, "loopSound")) { ent->s.loopSound = value; return qtrue; }
	if (!strcmp(field, "svFlags")) { ent->r.svFlags = value; return qtrue; }
	if (!strcmp(field, "contents")) { ent->r.contents = value; return qtrue; }
	if (!strcmp(field, "ownerNum")) { ent->r.ownerNum = value; return qtrue; }
	// origin components
	if (!strcmp(field, "origin.x")) { ent->s.origin[0] = (float)value; ent->s.pos.trBase[0] = (float)value; ent->r.currentOrigin[0] = (float)value; return qtrue; }
	if (!strcmp(field, "origin.y")) { ent->s.origin[1] = (float)value; ent->s.pos.trBase[1] = (float)value; ent->r.currentOrigin[1] = (float)value; return qtrue; }
	if (!strcmp(field, "origin.z")) { ent->s.origin[2] = (float)value; ent->s.pos.trBase[2] = (float)value; ent->r.currentOrigin[2] = (float)value; return qtrue; }
	return qfalse;
}

static void debug_cmd_set_entity(jsonBuf_t *jb, int num, const char *field, int value) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "set_entity");

	if (!sv.gentities || num < 0 || num >= sv.num_entities) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}
	if (!field[0]) {
		jb_raw(jb, ","); jb_str(jb, "error", "field name required");
		jb_raw(jb, "}\n");
		return;
	}

	sharedEntity_t *ent = SV_GentityNum(num);
	int oldVal = debug_get_entity_field(ent, field);

	if (!debug_set_entity_field(ent, field, value)) {
		jb_raw(jb, ","); jb_str(jb, "error", "unknown or read-only field");
		jb_raw(jb, ","); jb_str(jb, "field", field);
		jb_raw(jb, "}\n");
		return;
	}

	// Re-link if the entity is currently linked (updates spatial hash)
	if (ent->r.linked) {
		SV_LinkEntity(ent);
	}

	jb_raw(jb, ","); jb_int(jb, "num", num);
	jb_raw(jb, ","); jb_str(jb, "field", field);
	jb_raw(jb, ","); jb_int(jb, "oldValue", oldVal);
	jb_raw(jb, ","); jb_int(jb, "newValue", value);
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Spatial trace (ray cast)
// ------------------------------------------------------------------

static void debug_cmd_trace(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "trace");

	// Parse "x1,y1,z1" from strArg1 and "x2,y2,z2" from strArg2
	vec3_t start = {0,0,0}, end = {0,0,0};
	sscanf(cmd->strArg1, "%f,%f,%f", &start[0], &start[1], &start[2]);
	sscanf(cmd->strArg2, "%f,%f,%f", &end[0], &end[1], &end[2]);

	int contentmask = cmd->hasIntArg2 ? cmd->intArg2 : -1; // MASK_ALL by default

	trace_t tr;
	SV_Trace(&tr, start, NULL, NULL, end, ENTITYNUM_NONE, contentmask, 0);

	jb_raw(jb, ","); jb_vec3(jb, "start", start);
	jb_raw(jb, ","); jb_vec3(jb, "end", end);
	jb_raw(jb, ","); jb_float(jb, "fraction", tr.fraction);
	jb_raw(jb, ","); jb_vec3(jb, "endpos", tr.endpos);
	jb_raw(jb, ","); jb_bool(jb, "allsolid", tr.allsolid);
	jb_raw(jb, ","); jb_bool(jb, "startsolid", tr.startsolid);
	jb_raw(jb, ","); jb_int(jb, "entityNum", tr.entityNum);
	jb_raw(jb, ","); jb_int(jb, "surfaceFlags", tr.surfaceFlags);
	jb_raw(jb, ","); jb_int(jb, "contents", tr.contents);
	jb_raw(jb, ","); jb_vec3(jb, "planeNormal", tr.plane.normal);
	jb_raw(jb, ","); jb_float(jb, "planeDist", tr.plane.dist);
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Entities in box (spatial query)
// ------------------------------------------------------------------

static void debug_cmd_entities_in_box(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "entities_in_box");

	vec3_t mins = {0,0,0}, maxs = {0,0,0};
	sscanf(cmd->strArg1, "%f,%f,%f", &mins[0], &mins[1], &mins[2]);
	sscanf(cmd->strArg2, "%f,%f,%f", &maxs[0], &maxs[1], &maxs[2]);

	int entityList[MAX_GENTITIES];
	int count = SV_AreaEntities(mins, maxs, entityList, MAX_GENTITIES);

	jb_raw(jb, ","); jb_vec3(jb, "mins", mins);
	jb_raw(jb, ","); jb_vec3(jb, "maxs", maxs);
	jb_raw(jb, ","); jb_int(jb, "count", count);
	jb_raw(jb, ",\"entities\":[");
	int first = 1;
	int i;
	for (i = 0; i < count; i++) {
		sharedEntity_t *ent = SV_GentityNum(entityList[i]);
		if (!first) jb_raw(jb, ",");
		first = 0;
		jb_raw(jb, "{");
		jb_int(jb, "num", entityList[i]);
		jb_raw(jb, ","); jb_int(jb, "eType", ent->s.eType);
		jb_raw(jb, ","); jb_vec3(jb, "origin", ent->s.origin);
		jb_raw(jb, "}");
	}
	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Entity watchpoints
// ------------------------------------------------------------------

static void debug_cmd_watch(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "watch");

	if (!strcmp(cmd->strArg2, "add")) {
		int slot = -1;
		int i;
		for (i = 0; i < MAX_WATCHPOINTS; i++) {
			if (!watchpoints[i].active) { slot = i; break; }
		}
		if (slot < 0) {
			jb_raw(jb, ","); jb_str(jb, "error", "max watchpoints reached");
			jb_raw(jb, "}\n");
			return;
		}
		watchpoints[slot].active = qtrue;
		watchpoints[slot].entNum = cmd->intArg1;
		Q_strncpyz(watchpoints[slot].field, cmd->strArg1, sizeof(watchpoints[slot].field));
		if (sv.gentities && cmd->intArg1 >= 0 && cmd->intArg1 < sv.num_entities) {
			watchpoints[slot].lastValue = debug_get_entity_field(SV_GentityNum(cmd->intArg1), cmd->strArg1);
		}
		jb_raw(jb, ","); jb_str(jb, "action", "added");
		jb_raw(jb, ","); jb_int(jb, "slot", slot);
		jb_raw(jb, ","); jb_int(jb, "entNum", cmd->intArg1);
		jb_raw(jb, ","); jb_str(jb, "field", cmd->strArg1);
	} else if (!strcmp(cmd->strArg2, "remove")) {
		if (cmd->intArg1 >= 0 && cmd->intArg1 < MAX_WATCHPOINTS) {
			watchpoints[cmd->intArg1].active = qfalse;
			jb_raw(jb, ","); jb_str(jb, "action", "removed");
			jb_raw(jb, ","); jb_int(jb, "slot", cmd->intArg1);
		} else {
			jb_raw(jb, ","); jb_str(jb, "error", "invalid slot");
		}
	} else if (!strcmp(cmd->strArg2, "clear")) {
		int i;
		for (i = 0; i < MAX_WATCHPOINTS; i++) watchpoints[i].active = qfalse;
		jb_raw(jb, ","); jb_str(jb, "action", "cleared");
	} else {
		// List watchpoints and check for changes
		jb_raw(jb, ",\"watchpoints\":[");
		int first = 1;
		int i;
		for (i = 0; i < MAX_WATCHPOINTS; i++) {
			if (!watchpoints[i].active) continue;
			if (!first) jb_raw(jb, ",");
			first = 0;
			int curVal = 0;
			qboolean changed = qfalse;
			if (sv.gentities && watchpoints[i].entNum >= 0 && watchpoints[i].entNum < sv.num_entities) {
				curVal = debug_get_entity_field(SV_GentityNum(watchpoints[i].entNum), watchpoints[i].field);
				if (curVal != watchpoints[i].lastValue) {
					changed = qtrue;
					watchpoints[i].lastValue = curVal;
				}
			}
			jb_raw(jb, "{");
			jb_int(jb, "slot", i);
			jb_raw(jb, ","); jb_int(jb, "entNum", watchpoints[i].entNum);
			jb_raw(jb, ","); jb_str(jb, "field", watchpoints[i].field);
			jb_raw(jb, ","); jb_int(jb, "value", curVal);
			jb_raw(jb, ","); jb_bool(jb, "changed", changed);
			jb_raw(jb, "}");
		}
		jb_raw(jb, "]");
	}

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// VM call trace
// ------------------------------------------------------------------

void DebugServer_TraceVMCall(int command, int serverTime, int durationUsec) {
	if (!vmTraceEnabled) return;
	vmTraceEntry_t *e = &vmTraceRing[vmTraceHead % VMTRACE_RING_SIZE];
	e->command = command;
	e->serverTime = serverTime;
	e->durationUsec = durationUsec;
	vmTraceHead++;
	vmTraceCount++;
}

static void debug_cmd_vmtrace(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "vmtrace");

	if (!strcmp(cmd->strArg2, "start")) {
		vmTraceEnabled = qtrue;
		vmTraceHead = 0;
		vmTraceCount = 0;
		jb_raw(jb, ","); jb_str(jb, "action", "started");
	} else if (!strcmp(cmd->strArg2, "stop")) {
		vmTraceEnabled = qfalse;
		jb_raw(jb, ","); jb_str(jb, "action", "stopped");
		jb_raw(jb, ","); jb_int(jb, "totalCalls", vmTraceCount);
	} else {
		// Dump recent calls
		jb_raw(jb, ","); jb_bool(jb, "enabled", vmTraceEnabled);
		jb_raw(jb, ","); jb_int(jb, "totalCalls", vmTraceCount);
		jb_raw(jb, ",\"calls\":[");
		int startIdx = vmTraceHead - VMTRACE_RING_SIZE;
		if (startIdx < 0) startIdx = 0;
		int sinceId = cmd->hasIntArg1 ? cmd->intArg1 : startIdx;
		if (sinceId < startIdx) sinceId = startIdx;

		int first = 1;
		int i;
		for (i = sinceId; i < vmTraceHead; i++) {
			vmTraceEntry_t *e = &vmTraceRing[i % VMTRACE_RING_SIZE];
			if (!first) jb_raw(jb, ",");
			first = 0;

			const char *cmdName = "unknown";
			switch(e->command) {
				case 0: cmdName = "GAME_INIT"; break;
				case 1: cmdName = "GAME_SHUTDOWN"; break;
				case 2: cmdName = "GAME_CLIENT_CONNECT"; break;
				case 3: cmdName = "GAME_CLIENT_BEGIN"; break;
				case 4: cmdName = "GAME_CLIENT_USERINFO_CHANGED"; break;
				case 5: cmdName = "GAME_CLIENT_DISCONNECT"; break;
				case 6: cmdName = "GAME_CLIENT_COMMAND"; break;
				case 7: cmdName = "GAME_CLIENT_THINK"; break;
				case 8: cmdName = "GAME_RUN_FRAME"; break;
				case 9: cmdName = "GAME_CONSOLE_COMMAND"; break;
				case 10: cmdName = "BOTAI_START_FRAME"; break;
			}
			jb_raw(jb, "{");
			jb_int(jb, "id", i);
			jb_raw(jb, ","); jb_str(jb, "command", cmdName);
			jb_raw(jb, ","); jb_int(jb, "commandId", e->command);
			jb_raw(jb, ","); jb_int(jb, "serverTime", e->serverTime);
			jb_raw(jb, ","); jb_int(jb, "durationUsec", e->durationUsec);
			jb_raw(jb, "}");
		}
		jb_raw(jb, "]");
	}
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Memory stats
// ------------------------------------------------------------------

static void debug_cmd_memory(jsonBuf_t *jb) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "memory");
	jb_raw(jb, ","); jb_int(jb, "hunkFreeBytes", Hunk_MemoryRemaining());
	jb_raw(jb, ","); jb_int(jb, "gentitySize", sv.gentitySize);
	jb_raw(jb, ","); jb_int(jb, "gameClientSize", sv.gameClientSize);
	jb_raw(jb, ","); jb_int(jb, "numEntities", sv.num_entities);
	jb_raw(jb, ","); jb_int(jb, "maxEntities", MAX_GENTITIES);
	jb_raw(jb, ","); jb_int(jb, "maxClients", sv_maxclients ? sv_maxclients->integer : 0);
	jb_raw(jb, ","); jb_int(jb, "numSnapshotEntities", svs.numSnapshotEntities);
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Player state modification
// ------------------------------------------------------------------

static void debug_cmd_set_player(jsonBuf_t *jb, int num, const char *field, int value) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "set_player");

	if (!sv.gameClients || num < 0 || num >= sv_maxclients->integer) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid client number");
		jb_raw(jb, "}\n");
		return;
	}
	if (!field[0]) {
		jb_raw(jb, ","); jb_str(jb, "error", "field name required");
		jb_raw(jb, "}\n");
		return;
	}

	playerState_t *ps = SV_GameClientNum(num);
	int oldVal = 0;
	qboolean found = qtrue;

	if (!strcmp(field, "pm_type")) { oldVal = ps->pm_type; ps->pm_type = value; }
	else if (!strcmp(field, "pm_flags")) { oldVal = ps->pm_flags; ps->pm_flags = value; }
	else if (!strcmp(field, "gravity")) { oldVal = ps->gravity; ps->gravity = value; }
	else if (!strcmp(field, "speed")) { oldVal = ps->speed; ps->speed = value; }
	else if (!strcmp(field, "weapon")) { oldVal = ps->weapon; ps->weapon = value; }
	else if (!strcmp(field, "weaponstate")) { oldVal = ps->weaponstate; ps->weaponstate = value; }
	else if (!strcmp(field, "weaponTime")) { oldVal = ps->weaponTime; ps->weaponTime = value; }
	else if (!strcmp(field, "viewheight")) { oldVal = ps->viewheight; ps->viewheight = value; }
	else if (!strcmp(field, "eFlags")) { oldVal = ps->eFlags; ps->eFlags = value; }
	else if (!strcmp(field, "groundEntityNum")) { oldVal = ps->groundEntityNum; ps->groundEntityNum = value; }
	else if (!strcmp(field, "origin.x")) { oldVal = (int)ps->origin[0]; ps->origin[0] = (float)value; }
	else if (!strcmp(field, "origin.y")) { oldVal = (int)ps->origin[1]; ps->origin[1] = (float)value; }
	else if (!strcmp(field, "origin.z")) { oldVal = (int)ps->origin[2]; ps->origin[2] = (float)value; }
	else if (!strcmp(field, "velocity.x")) { oldVal = (int)ps->velocity[0]; ps->velocity[0] = (float)value; }
	else if (!strcmp(field, "velocity.y")) { oldVal = (int)ps->velocity[1]; ps->velocity[1] = (float)value; }
	else if (!strcmp(field, "velocity.z")) { oldVal = (int)ps->velocity[2]; ps->velocity[2] = (float)value; }
	else if (!strcmp(field, "viewangles.yaw")) { oldVal = (int)ps->viewangles[1]; ps->viewangles[1] = (float)value; }
	else if (!strcmp(field, "viewangles.pitch")) { oldVal = (int)ps->viewangles[0]; ps->viewangles[0] = (float)value; }
	else {
		// stat/ammo/powerup by index: "stats.0" .. "stats.15", "ammo.0" .. "ammo.15", "powerups.0" ..
		if (!strncmp(field, "stats.", 6)) {
			int idx = atoi(field + 6);
			if (idx >= 0 && idx < MAX_STATS) { oldVal = ps->stats[idx]; ps->stats[idx] = value; }
			else found = qfalse;
		} else if (!strncmp(field, "ammo.", 5)) {
			int idx = atoi(field + 5);
			if (idx >= 0 && idx < MAX_WEAPONS) { oldVal = ps->ammo[idx]; ps->ammo[idx] = value; }
			else found = qfalse;
		} else if (!strncmp(field, "persistant.", 11)) {
			int idx = atoi(field + 11);
			if (idx >= 0 && idx < MAX_PERSISTANT) { oldVal = ps->persistant[idx]; ps->persistant[idx] = value; }
			else found = qfalse;
		} else if (!strncmp(field, "powerups.", 9)) {
			int idx = atoi(field + 9);
			if (idx >= 0 && idx < MAX_POWERUPS) { oldVal = ps->powerups[idx]; ps->powerups[idx] = value; }
			else found = qfalse;
		} else {
			found = qfalse;
		}
	}

	if (!found) {
		jb_raw(jb, ","); jb_str(jb, "error", "unknown field");
		jb_raw(jb, ","); jb_str(jb, "field", field);
		jb_raw(jb, ","); jb_str(jb, "available", "pm_type, pm_flags, gravity, speed, weapon, weaponstate, weaponTime, viewheight, eFlags, groundEntityNum, origin.x/y/z, velocity.x/y/z, viewangles.pitch/yaw, stats.N, ammo.N, persistant.N, powerups.N");
		jb_raw(jb, "}\n");
		return;
	}

	jb_raw(jb, ","); jb_int(jb, "num", num);
	jb_raw(jb, ","); jb_str(jb, "field", field);
	jb_raw(jb, ","); jb_int(jb, "oldValue", oldVal);
	jb_raw(jb, ","); jb_int(jb, "newValue", value);
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Time control — pause, step, timescale
// ------------------------------------------------------------------

static qboolean debugPaused = qfalse;
static int      debugStepFrames = 0;

qboolean DebugServer_IsPaused(void) {
	if (debugStepFrames > 0) {
		debugStepFrames--;
		return qfalse;  // allow this frame
	}
	return debugPaused;
}

static void debug_cmd_time(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "time_control");

	if (!strcmp(cmd->strArg2, "pause")) {
		debugPaused = qtrue;
		debugStepFrames = 0;
		jb_raw(jb, ","); jb_str(jb, "action", "paused");
	} else if (!strcmp(cmd->strArg2, "resume")) {
		debugPaused = qfalse;
		debugStepFrames = 0;
		jb_raw(jb, ","); jb_str(jb, "action", "resumed");
	} else if (!strcmp(cmd->strArg2, "step")) {
		int n = cmd->hasIntArg1 ? cmd->intArg1 : 1;
		if (n < 1) n = 1;
		debugStepFrames = n;
		jb_raw(jb, ","); jb_str(jb, "action", "stepping");
		jb_raw(jb, ","); jb_int(jb, "frames", n);
	} else if (!strcmp(cmd->strArg2, "timescale")) {
		if (cmd->hasIntArg1) {
			// intArg1 is value * 100 to support fractional (e.g. 50 = 0.5x, 200 = 2x)
			float ts = cmd->intArg1 / 100.0f;
			if (ts < 0.01f) ts = 0.01f;
			if (ts > 100.0f) ts = 100.0f;
			Cvar_Set("timescale", va("%f", ts));
			jb_raw(jb, ","); jb_str(jb, "action", "timescale_set");
			jb_raw(jb, ","); jb_float(jb, "timescale", ts);
		}
	} else {
		// status query
		jb_raw(jb, ","); jb_bool(jb, "paused", debugPaused);
		jb_raw(jb, ","); jb_int(jb, "stepFramesRemaining", debugStepFrames);
		jb_raw(jb, ","); jb_float(jb, "timescale", com_timescale ? com_timescale->value : 1.0f);
		jb_raw(jb, ","); jb_int(jb, "serverTime", sv.time);
	}

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Map list — query available BSPs from pk3 files
// ------------------------------------------------------------------

static void debug_cmd_maplist(jsonBuf_t *jb) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "maplist");

	char listBuf[16384];
	int nMaps = FS_GetFileList("maps", ".bsp", listBuf, sizeof(listBuf));

	jb_raw(jb, ","); jb_int(jb, "count", nMaps);
	jb_raw(jb, ",\"maps\":[");

	const char *p = listBuf;
	int first = 1;
	int i;
	for (i = 0; i < nMaps; i++) {
		if (!first) jb_raw(jb, ",");
		first = 0;
		// strip .bsp extension
		char name[MAX_QPATH];
		Q_strncpyz(name, p, sizeof(name));
		int len = strlen(name);
		if (len > 4 && !Q_stricmp(name + len - 4, ".bsp")) {
			name[len - 4] = '\0';
		}
		jb_printf(jb, "\"%s\"", name);
		p += strlen(p) + 1;
	}

	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Entity snapshot diff — capture state, compare later
// ------------------------------------------------------------------

#define SNAP_MAX_ENTS 1024

typedef struct {
	qboolean    valid;
	int         serverTime;
	int         numEntities;
	struct {
		int     number;
		int     eType;
		int     eFlags;
		vec3_t  origin;
		int     weapon;
		int     modelindex;
		int     svFlags;
		int     contents;
		qboolean linked;
	} ents[SNAP_MAX_ENTS];
} entitySnapshot_t;

static entitySnapshot_t debugSnapshot;

static void debug_capture_snapshot(void) {
	debugSnapshot.valid = qtrue;
	debugSnapshot.serverTime = sv.time;
	debugSnapshot.numEntities = sv.num_entities;
	int i;
	for (i = 0; i < sv.num_entities && i < SNAP_MAX_ENTS; i++) {
		sharedEntity_t *ent = SV_GentityNum(i);
		debugSnapshot.ents[i].number = ent->s.number;
		debugSnapshot.ents[i].eType = ent->s.eType;
		debugSnapshot.ents[i].eFlags = ent->s.eFlags;
		VectorCopy(ent->s.origin, debugSnapshot.ents[i].origin);
		debugSnapshot.ents[i].weapon = ent->s.weapon;
		debugSnapshot.ents[i].modelindex = ent->s.modelindex;
		debugSnapshot.ents[i].svFlags = ent->r.svFlags;
		debugSnapshot.ents[i].contents = ent->r.contents;
		debugSnapshot.ents[i].linked = ent->r.linked;
	}
}

static void debug_cmd_snapshot(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "snapshot");

	if (!strcmp(cmd->strArg2, "capture")) {
		if (!sv.gentities) {
			jb_raw(jb, ","); jb_str(jb, "error", "no entities loaded");
			jb_raw(jb, "}\n");
			return;
		}
		debug_capture_snapshot();
		jb_raw(jb, ","); jb_str(jb, "action", "captured");
		jb_raw(jb, ","); jb_int(jb, "serverTime", debugSnapshot.serverTime);
		jb_raw(jb, ","); jb_int(jb, "numEntities", debugSnapshot.numEntities);
	} else if (!strcmp(cmd->strArg2, "diff")) {
		if (!debugSnapshot.valid) {
			jb_raw(jb, ","); jb_str(jb, "error", "no snapshot captured. Use action 'capture' first.");
			jb_raw(jb, "}\n");
			return;
		}
		if (!sv.gentities) {
			jb_raw(jb, ","); jb_str(jb, "error", "no entities loaded");
			jb_raw(jb, "}\n");
			return;
		}

		jb_raw(jb, ","); jb_int(jb, "snapshotTime", debugSnapshot.serverTime);
		jb_raw(jb, ","); jb_int(jb, "currentTime", sv.time);
		jb_raw(jb, ",\"changes\":[");

		int first = 1;
		int maxEnts = sv.num_entities;
		if (debugSnapshot.numEntities > maxEnts) maxEnts = debugSnapshot.numEntities;
		if (maxEnts > SNAP_MAX_ENTS) maxEnts = SNAP_MAX_ENTS;
		int i;
		for (i = 0; i < maxEnts; i++) {
			sharedEntity_t *ent = (i < sv.num_entities) ? SV_GentityNum(i) : NULL;
			int curType = ent ? ent->s.eType : 0;
			int curFlags = ent ? ent->s.eFlags : 0;
			int curWeapon = ent ? ent->s.weapon : 0;
			int curModel = ent ? ent->s.modelindex : 0;
			int curSvFlags = ent ? ent->r.svFlags : 0;
			qboolean curLinked = ent ? ent->r.linked : qfalse;
			vec3_t curOrigin;
			if (ent) VectorCopy(ent->s.origin, curOrigin);
			else VectorClear(curOrigin);

			int oldType = (i < debugSnapshot.numEntities) ? debugSnapshot.ents[i].eType : 0;
			int oldFlags = (i < debugSnapshot.numEntities) ? debugSnapshot.ents[i].eFlags : 0;
			int oldWeapon = (i < debugSnapshot.numEntities) ? debugSnapshot.ents[i].weapon : 0;
			int oldModel = (i < debugSnapshot.numEntities) ? debugSnapshot.ents[i].modelindex : 0;
			int oldSvFlags = (i < debugSnapshot.numEntities) ? debugSnapshot.ents[i].svFlags : 0;
			qboolean oldLinked = (i < debugSnapshot.numEntities) ? debugSnapshot.ents[i].linked : qfalse;
			vec3_t oldOrigin;
			if (i < debugSnapshot.numEntities) VectorCopy(debugSnapshot.ents[i].origin, oldOrigin);
			else VectorClear(oldOrigin);

			qboolean changed = qfalse;
			if (curType != oldType || curFlags != oldFlags || curWeapon != oldWeapon ||
				curModel != oldModel || curSvFlags != oldSvFlags || curLinked != oldLinked) {
				changed = qtrue;
			}
			// Check origin with tolerance
			float dx = curOrigin[0] - oldOrigin[0];
			float dy = curOrigin[1] - oldOrigin[1];
			float dz = curOrigin[2] - oldOrigin[2];
			if (dx*dx + dy*dy + dz*dz > 1.0f) changed = qtrue;

			if (changed) {
				if (!first) jb_raw(jb, ",");
				first = 0;
				jb_raw(jb, "{");
				jb_int(jb, "num", i);
				if (curType != oldType) { jb_raw(jb, ",\"eType\":{"); jb_int(jb, "old", oldType); jb_raw(jb, ","); jb_int(jb, "new", curType); jb_raw(jb, "}"); }
				if (curFlags != oldFlags) { jb_raw(jb, ",\"eFlags\":{"); jb_int(jb, "old", oldFlags); jb_raw(jb, ","); jb_int(jb, "new", curFlags); jb_raw(jb, "}"); }
				if (curWeapon != oldWeapon) { jb_raw(jb, ",\"weapon\":{"); jb_int(jb, "old", oldWeapon); jb_raw(jb, ","); jb_int(jb, "new", curWeapon); jb_raw(jb, "}"); }
				if (curModel != oldModel) { jb_raw(jb, ",\"modelindex\":{"); jb_int(jb, "old", oldModel); jb_raw(jb, ","); jb_int(jb, "new", curModel); jb_raw(jb, "}"); }
				if (curLinked != oldLinked) { jb_raw(jb, ",\"linked\":{"); jb_bool(jb, "old", oldLinked); jb_raw(jb, ","); jb_bool(jb, "new", curLinked); jb_raw(jb, "}"); }
				if (dx*dx + dy*dy + dz*dz > 1.0f) {
					jb_raw(jb, ",\"origin\":{");
					jb_vec3(jb, "old", oldOrigin);
					jb_raw(jb, ","); jb_vec3(jb, "new", curOrigin);
					jb_raw(jb, "}");
				}
				jb_raw(jb, "}");
			}
		}
		jb_raw(jb, "]");
	} else {
		// Status
		jb_raw(jb, ","); jb_bool(jb, "hasSnapshot", debugSnapshot.valid);
		if (debugSnapshot.valid) {
			jb_raw(jb, ","); jb_int(jb, "snapshotTime", debugSnapshot.serverTime);
			jb_raw(jb, ","); jb_int(jb, "snapshotEntities", debugSnapshot.numEntities);
		}
		jb_raw(jb, ","); jb_int(jb, "currentTime", sv.time);
		jb_raw(jb, ","); jb_int(jb, "currentEntities", sv.num_entities);
	}

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Raw memory peek — read bytes from game module entity data
// ------------------------------------------------------------------

static void debug_cmd_peek(jsonBuf_t *jb, int entNum, int offset, int length) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "peek");

	if (!sv.gentities) {
		jb_raw(jb, ","); jb_str(jb, "error", "no entities loaded");
		jb_raw(jb, "}\n");
		return;
	}
	if (entNum < 0 || entNum >= sv.num_entities) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}
	if (offset < 0 || offset >= sv.gentitySize) {
		jb_raw(jb, ","); jb_str(jb, "error", "offset out of range");
		jb_raw(jb, ","); jb_int(jb, "gentitySize", sv.gentitySize);
		jb_raw(jb, "}\n");
		return;
	}
	if (length <= 0) length = 64;
	if (length > 512) length = 512;
	if (offset + length > sv.gentitySize) length = sv.gentitySize - offset;

	byte *base = (byte*)sv.gentities + sv.gentitySize * entNum;

	jb_raw(jb, ","); jb_int(jb, "entNum", entNum);
	jb_raw(jb, ","); jb_int(jb, "offset", offset);
	jb_raw(jb, ","); jb_int(jb, "length", length);
	jb_raw(jb, ","); jb_int(jb, "gentitySize", sv.gentitySize);
	jb_raw(jb, ","); jb_int(jb, "sharedEntitySize", (int)sizeof(sharedEntity_t));

	// Hex dump
	jb_raw(jb, ",\"hex\":\"");
	int i;
	for (i = 0; i < length; i++) {
		jb_printf(jb, "%02x", base[offset + i]);
	}
	jb_raw(jb, "\"");

	// Also as int32 array for easier reading
	jb_raw(jb, ",\"int32s\":[");
	int numInts = length / 4;
	for (i = 0; i < numInts; i++) {
		if (i) jb_raw(jb, ",");
		int val;
		memcpy(&val, base + offset + i * 4, 4);
		jb_printf(jb, "%d", val);
	}
	jb_raw(jb, "]");

	// Float interpretation too
	jb_raw(jb, ",\"floats\":[");
	for (i = 0; i < numInts; i++) {
		if (i) jb_raw(jb, ",");
		float val;
		memcpy(&val, base + offset + i * 4, 4);
		if (IS_NAN(val) || IS_INF(val)) jb_raw(jb, "null");
		else jb_printf(jb, "%.4f", val);
	}
	jb_raw(jb, "]");

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Point contents query
// ------------------------------------------------------------------

static void debug_cmd_point_contents(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "point_contents");

	vec3_t point = {0,0,0};
	sscanf(cmd->strArg1, "%f,%f,%f", &point[0], &point[1], &point[2]);

	int contents = SV_PointContents(point, ENTITYNUM_NONE);

	jb_raw(jb, ","); jb_vec3(jb, "point", point);
	jb_raw(jb, ","); jb_int(jb, "contents", contents);

	// Decode content flags
	jb_raw(jb, ",\"decoded\":[");
	int flagFirst = 1;
	#define CFLAG(bit, name) if (contents & (bit)) { if (!flagFirst) jb_raw(jb, ","); jb_printf(jb, "\"%s\"", name); flagFirst = 0; }
	CFLAG(1, "SOLID")
	CFLAG(8, "LAVA")
	CFLAG(16, "SLIME")
	CFLAG(32, "WATER")
	CFLAG(0x8000, "AREAPORTAL")
	CFLAG(0x10000, "PLAYERCLIP")
	CFLAG(0x20000, "MONSTERCLIP")
	CFLAG(0x2000000, "BODY")
	CFLAG(0x4000000, "CORPSE")
	CFLAG(0x8000000, "DETAIL")
	CFLAG(0x10000000, "STRUCTURAL")
	CFLAG(0x40000000, "TRIGGER")
	#undef CFLAG
	jb_raw(jb, "]");

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// String pointer scan — find string pointers in entity memory
// Detects int32 values that look like valid pointers and attempts
// to read a C string from them. Useful for classname, targetname,
// model, script_targetname, etc.
// ------------------------------------------------------------------

static qboolean debug_is_readable_pointer(const void *ptr) {
	uintptr_t addr = (uintptr_t)ptr;
	if (addr < 0x10000 || addr > 0x7FFFFFFF) return qfalse;

#ifdef _WIN32
	// VirtualQuery to check if the page is committed and readable
	MEMORY_BASIC_INFORMATION mbi;
	if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return qfalse;
	if (mbi.State != MEM_COMMIT) return qfalse;
	if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return qfalse;
	return qtrue;
#else
	return qfalse;
#endif
}

static qboolean debug_read_string_safe(uintptr_t addr, char *out, int outSz) {
	if (!debug_is_readable_pointer((const void *)addr)) return qfalse;

	const char *src = (const char *)addr;

	// Verify it looks like a printable ASCII string (at least 2 chars)
	int i;
	for (i = 0; i < outSz - 1; i++) {
		if (!debug_is_readable_pointer((const void *)(src + i))) {
			if (i < 2) return qfalse;
			out[i] = '\0';
			return qtrue;
		}
		char c = src[i];
		if (c == '\0') {
			out[i] = '\0';
			return (i >= 2); // must be at least 2 chars
		}
		if (c < 0x20 || c > 0x7e) {
			return qfalse; // not printable ASCII
		}
		out[i] = c;
	}
	out[outSz - 1] = '\0';
	return (i >= 2);
}

static void debug_cmd_strings(jsonBuf_t *jb, int entNum) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "strings");

	if (!sv.gentities) {
		jb_raw(jb, ","); jb_str(jb, "error", "no entities loaded");
		jb_raw(jb, "}\n");
		return;
	}
	if (entNum < 0 || entNum >= sv.num_entities) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}

	byte *base = (byte *)sv.gentities + sv.gentitySize * entNum;
	int sharedSize = (int)sizeof(sharedEntity_t);

	jb_raw(jb, ","); jb_int(jb, "entNum", entNum);
	jb_raw(jb, ","); jb_int(jb, "gentitySize", sv.gentitySize);
	jb_raw(jb, ","); jb_int(jb, "sharedEntitySize", sharedSize);
	jb_raw(jb, ",\"strings\":[");

	int first = 1;
	int offset;
	char strBuf[256];

	// Scan all int32-aligned positions in the entity
	for (offset = 0; offset + 4 <= sv.gentitySize; offset += 4) {
		uintptr_t val;
		memcpy(&val, base + offset, sizeof(uintptr_t));

		if (debug_read_string_safe(val, strBuf, sizeof(strBuf))) {
			if (!first) jb_raw(jb, ",");
			first = 0;
			jb_raw(jb, "{");
			jb_int(jb, "offset", offset);
			jb_raw(jb, ","); jb_str(jb, "value", strBuf);
			jb_raw(jb, ","); jb_printf(jb, "\"ptr\":\"0x%08x\"", (unsigned int)val);
			jb_raw(jb, ","); jb_str(jb, "region", offset < sharedSize ? "shared" : "private");
			jb_raw(jb, "}");
		}
	}

	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Function pointer scan — identify entity offsets that contain
// addresses within the game DLL's code range
// ------------------------------------------------------------------

static uintptr_t dllCodeBase = 0;
static uintptr_t dllCodeEnd = 0;

// Called once after game DLL loads to cache its code range
static void debug_detect_dll_range(void) {
	if (dllCodeBase != 0) return; // already detected

	if (!sv.gentities || sv.num_entities < 1) return;

	byte *base = (byte *)sv.gentities;
	int offset;

	// Gather candidate code pointers from entity 0 (worldspawn)
	// Look for clusters of values in the same 0xXX000000 range
	int counts[256] = {0};
	for (offset = (int)sizeof(sharedEntity_t); offset + 4 <= sv.gentitySize; offset += 4) {
		uintptr_t val;
		memcpy(&val, base + offset, sizeof(uintptr_t));
		if (val >= 0x10000 && val <= 0x7FFFFFFF) {
			unsigned int page = (unsigned int)(val >> 24);
			if (page < 256) counts[page]++;
		}
	}

	// Find the page with the most hits — likely the DLL base
	int bestPage = 0, bestCount = 0;
	int i;
	for (i = 0; i < 256; i++) {
		if (counts[i] > bestCount) {
			bestCount = counts[i];
			bestPage = i;
		}
	}

	if (bestCount >= 3) {
		// Refine: scan for actual min/max in that page range
		uintptr_t minAddr = 0xFFFFFFFF, maxAddr = 0;
		for (offset = (int)sizeof(sharedEntity_t); offset + 4 <= sv.gentitySize; offset += 4) {
			uintptr_t val;
			memcpy(&val, base + offset, sizeof(uintptr_t));
			if ((val >> 24) == (unsigned int)bestPage) {
				if (val < minAddr) minAddr = val;
				if (val > maxAddr) maxAddr = val;
			}
		}
		// Align to page boundaries with some margin
		dllCodeBase = minAddr & ~0xFFF;
		dllCodeEnd = (maxAddr + 0x1000) & ~0xFFF;
	}
}

static void debug_cmd_funcscan(jsonBuf_t *jb, int entNum) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "funcscan");

	if (!sv.gentities) {
		jb_raw(jb, ","); jb_str(jb, "error", "no entities loaded");
		jb_raw(jb, "}\n");
		return;
	}
	if (entNum < 0 || entNum >= sv.num_entities) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}

	debug_detect_dll_range();

	byte *base = (byte *)sv.gentities + sv.gentitySize * entNum;
	int sharedSize = (int)sizeof(sharedEntity_t);

	jb_raw(jb, ","); jb_int(jb, "entNum", entNum);
	jb_raw(jb, ","); jb_int(jb, "gentitySize", sv.gentitySize);
	jb_raw(jb, ","); jb_printf(jb, "\"dllCodeBase\":\"0x%08x\"", (unsigned int)dllCodeBase);
	jb_raw(jb, ","); jb_printf(jb, "\"dllCodeEnd\":\"0x%08x\"", (unsigned int)dllCodeEnd);
	jb_raw(jb, ",\"functions\":[");

	int first = 1;
	int offset;

	for (offset = sharedSize; offset + 4 <= sv.gentitySize; offset += 4) {
		uintptr_t val;
		memcpy(&val, base + offset, sizeof(uintptr_t));

		if (dllCodeBase && val >= dllCodeBase && val < dllCodeEnd) {
			if (!first) jb_raw(jb, ",");
			first = 0;
			jb_raw(jb, "{");
			jb_int(jb, "offset", offset);
			jb_raw(jb, ","); jb_printf(jb, "\"address\":\"0x%08x\"", (unsigned int)val);
			jb_raw(jb, ","); jb_int(jb, "privateOffset", offset - sharedSize);
			jb_raw(jb, "}");
		}
	}

	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Bulk field scan — read same offset across all active entities
// Returns distinct values and which entity numbers have each value
// ------------------------------------------------------------------

static void debug_cmd_bulkscan(jsonBuf_t *jb, int offset, int width, qboolean activeOnly) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "bulkscan");

	if (!sv.gentities) {
		jb_raw(jb, ","); jb_str(jb, "error", "no entities loaded");
		jb_raw(jb, "}\n");
		return;
	}
	if (offset < 0 || offset + 4 > sv.gentitySize) {
		jb_raw(jb, ","); jb_str(jb, "error", "offset out of range");
		jb_raw(jb, ","); jb_int(jb, "gentitySize", sv.gentitySize);
		jb_raw(jb, "}\n");
		return;
	}
	if (width != 1 && width != 2 && width != 4) width = 4;

	debug_detect_dll_range();

	jb_raw(jb, ","); jb_int(jb, "offset", offset);
	jb_raw(jb, ","); jb_int(jb, "width", width);
	jb_raw(jb, ","); jb_int(jb, "gentitySize", sv.gentitySize);

	// Collect distinct values (up to 128 unique values tracked)
	#define BULK_MAX_DISTINCT 128
	struct { int value; int count; int firstEnt; int lastEnt; } distinct[BULK_MAX_DISTINCT];
	int numDistinct = 0;
	int scanned = 0;
	int i, j;

	for (i = 0; i < sv.num_entities; i++) {
		sharedEntity_t *ent = SV_GentityNum(i);
		if (activeOnly && !ent->r.linked) continue;

		byte *entBase = (byte *)sv.gentities + sv.gentitySize * i;
		int val = 0;
		if (width == 4) memcpy(&val, entBase + offset, 4);
		else if (width == 2) { short sv2; memcpy(&sv2, entBase + offset, 2); val = sv2; }
		else { val = *(signed char *)(entBase + offset); }

		scanned++;

		// Find or insert
		qboolean found = qfalse;
		for (j = 0; j < numDistinct; j++) {
			if (distinct[j].value == val) {
				distinct[j].count++;
				distinct[j].lastEnt = i;
				found = qtrue;
				break;
			}
		}
		if (!found && numDistinct < BULK_MAX_DISTINCT) {
			distinct[numDistinct].value = val;
			distinct[numDistinct].count = 1;
			distinct[numDistinct].firstEnt = i;
			distinct[numDistinct].lastEnt = i;
			numDistinct++;
		}
	}

	jb_raw(jb, ","); jb_int(jb, "scanned", scanned);
	jb_raw(jb, ","); jb_int(jb, "distinctValues", numDistinct);
	jb_raw(jb, ",\"values\":[");
	for (i = 0; i < numDistinct; i++) {
		if (i) jb_raw(jb, ",");
		jb_raw(jb, "{");
		jb_int(jb, "value", distinct[i].value);
		jb_raw(jb, ","); jb_printf(jb, "\"hex\":\"0x%08x\"", (unsigned int)distinct[i].value);
		jb_raw(jb, ","); jb_int(jb, "count", distinct[i].count);
		jb_raw(jb, ","); jb_int(jb, "firstEnt", distinct[i].firstEnt);
		jb_raw(jb, ","); jb_int(jb, "lastEnt", distinct[i].lastEnt);

		// If this looks like a string pointer, try to resolve it
		char strBuf[128];
		if (width == 4 && debug_read_string_safe((uintptr_t)distinct[i].value, strBuf, sizeof(strBuf))) {
			jb_raw(jb, ","); jb_str(jb, "string", strBuf);
		}
		// If this looks like a function pointer, flag it
		if (width == 4 && dllCodeBase && (uintptr_t)distinct[i].value >= dllCodeBase && (uintptr_t)distinct[i].value < dllCodeEnd) {
			jb_raw(jb, ","); jb_bool(jb, "isFunc", qtrue);
		}

		jb_raw(jb, "}");
	}
	jb_raw(jb, "]");

	// Also output per-entity values for the first 64 entities
	jb_raw(jb, ",\"entities\":[");
	{
		int first = 1;
		int count = 0;
		for (i = 0; i < sv.num_entities && count < 64; i++) {
			sharedEntity_t *ent = SV_GentityNum(i);
			if (activeOnly && !ent->r.linked) continue;

			byte *entBase = (byte *)sv.gentities + sv.gentitySize * i;
			int val = 0;
			if (width == 4) memcpy(&val, entBase + offset, 4);
			else if (width == 2) { short sv2; memcpy(&sv2, entBase + offset, 2); val = sv2; }
			else { val = *(signed char *)(entBase + offset); }

			if (!first) jb_raw(jb, ",");
			first = 0;
			jb_printf(jb, "{\"num\":%d,\"value\":%d,\"hex\":\"0x%08x\"}", i, val, (unsigned int)val);
			count++;
		}
	}
	jb_raw(jb, "]");

	#undef BULK_MAX_DISTINCT
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Game client memory peek — read raw bytes from gclient_t
// ------------------------------------------------------------------

static void debug_cmd_client_peek(jsonBuf_t *jb, int clientNum, int offset, int length) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "client_peek");

	if (!sv.gameClients || clientNum < 0 || clientNum >= sv_maxclients->integer) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid client number");
		jb_raw(jb, "}\n");
		return;
	}
	if (offset < 0 || offset >= sv.gameClientSize) {
		jb_raw(jb, ","); jb_str(jb, "error", "offset out of range");
		jb_raw(jb, ","); jb_int(jb, "gameClientSize", sv.gameClientSize);
		jb_raw(jb, "}\n");
		return;
	}
	if (length <= 0) length = 64;
	if (length > 512) length = 512;
	if (offset + length > sv.gameClientSize) length = sv.gameClientSize - offset;

	byte *base = (byte *)sv.gameClients + sv.gameClientSize * clientNum;

	jb_raw(jb, ","); jb_int(jb, "clientNum", clientNum);
	jb_raw(jb, ","); jb_int(jb, "offset", offset);
	jb_raw(jb, ","); jb_int(jb, "length", length);
	jb_raw(jb, ","); jb_int(jb, "gameClientSize", sv.gameClientSize);
	jb_raw(jb, ","); jb_int(jb, "playerStateSize", (int)sizeof(playerState_t));

	// Hex dump
	jb_raw(jb, ",\"hex\":\"");
	int i;
	for (i = 0; i < length; i++) {
		jb_printf(jb, "%02x", base[offset + i]);
	}
	jb_raw(jb, "\"");

	// int32 array
	jb_raw(jb, ",\"int32s\":[");
	int numInts = length / 4;
	for (i = 0; i < numInts; i++) {
		if (i) jb_raw(jb, ",");
		int val;
		memcpy(&val, base + offset + i * 4, 4);
		jb_printf(jb, "%d", val);
	}
	jb_raw(jb, "]");

	// Float interpretation
	jb_raw(jb, ",\"floats\":[");
	for (i = 0; i < numInts; i++) {
		if (i) jb_raw(jb, ",");
		float val;
		memcpy(&val, base + offset + i * 4, 4);
		if (IS_NAN(val) || IS_INF(val)) jb_raw(jb, "null");
		else jb_printf(jb, "%.4f", val);
	}
	jb_raw(jb, "]");

	// String scan on this region
	jb_raw(jb, ",\"strings\":[");
	{
		int first = 1;
		int off;
		char strBuf[256];
		for (off = 0; off + 4 <= length; off += 4) {
			uintptr_t val;
			memcpy(&val, base + offset + off, sizeof(uintptr_t));
			if (debug_read_string_safe(val, strBuf, sizeof(strBuf))) {
				if (!first) jb_raw(jb, ",");
				first = 0;
				jb_raw(jb, "{");
				jb_int(jb, "offset", offset + off);
				jb_raw(jb, ","); jb_str(jb, "value", strBuf);
				jb_raw(jb, "}");
			}
		}
	}
	jb_raw(jb, "]");

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Client string scan — find string pointers in gclient_t
// ------------------------------------------------------------------

static void debug_cmd_client_strings(jsonBuf_t *jb, int clientNum) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "client_strings");

	if (!sv.gameClients || clientNum < 0 || clientNum >= sv_maxclients->integer) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid client number");
		jb_raw(jb, "}\n");
		return;
	}

	byte *base = (byte *)sv.gameClients + sv.gameClientSize * clientNum;
	int psSize = (int)sizeof(playerState_t);

	jb_raw(jb, ","); jb_int(jb, "clientNum", clientNum);
	jb_raw(jb, ","); jb_int(jb, "gameClientSize", sv.gameClientSize);
	jb_raw(jb, ","); jb_int(jb, "playerStateSize", psSize);
	jb_raw(jb, ",\"strings\":[");

	int first = 1;
	int offset;
	char strBuf[256];

	for (offset = 0; offset + 4 <= sv.gameClientSize; offset += 4) {
		uintptr_t val;
		memcpy(&val, base + offset, sizeof(uintptr_t));

		if (debug_read_string_safe(val, strBuf, sizeof(strBuf))) {
			if (!first) jb_raw(jb, ",");
			first = 0;
			jb_raw(jb, "{");
			jb_int(jb, "offset", offset);
			jb_raw(jb, ","); jb_str(jb, "value", strBuf);
			jb_raw(jb, ","); jb_printf(jb, "\"ptr\":\"0x%08x\"", (unsigned int)val);
			jb_raw(jb, ","); jb_str(jb, "region", offset < psSize ? "playerState" : "private");
			jb_raw(jb, "}");
		}
	}

	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Entity type name resolver
// ------------------------------------------------------------------

static const char *debug_eType_name(int eType) {
	switch (eType) {
		case 0: return "ET_GENERAL";
		case 1: return "ET_PLAYER";
		case 2: return "ET_ITEM";
		case 3: return "ET_MISSILE";
		case 4: return "ET_MOVER";
		case 5: return "ET_BEAM";
		case 6: return "ET_PORTAL";
		case 7: return "ET_SPEAKER";
		case 8: return "ET_PUSH_TRIGGER";
		case 9: return "ET_TELEPORT_TRIGGER";
		case 10: return "ET_INVISIBLE";
		case 11: return "ET_GRAPPLE";
		case 12: return "ET_TEAM";
		default:
			if (eType >= 13) return "ET_EVENTS+N";
			return "unknown";
	}
}

// ------------------------------------------------------------------
// Configstring index decoder
// ------------------------------------------------------------------

// CS layout: 0=SERVERINFO, 1=SYSTEMINFO, 2=MUSIC, 3=MESSAGE,
// 32..287=MODELS(256), 288..543=SOUNDS(256), 544..607=PLAYERS(64),
// 608..671=LOCATIONS, 672..735=PARTICLES

static void debug_cs_decode_index(jsonBuf_t *jb, int idx) {
	if (idx == 0) { jb_str(jb, "meaning", "CS_SERVERINFO"); }
	else if (idx == 1) { jb_str(jb, "meaning", "CS_SYSTEMINFO"); }
	else if (idx == 2) { jb_str(jb, "meaning", "CS_MUSIC"); }
	else if (idx == 3) { jb_str(jb, "meaning", "CS_MESSAGE"); }
	else if (idx == 8) { jb_str(jb, "meaning", "CS_VOTE_TIME"); }
	else if (idx == 9) { jb_str(jb, "meaning", "CS_VOTE_STRING"); }
	else if (idx == 20) { jb_str(jb, "meaning", "CS_GAME_VERSION"); }
	else if (idx == 21) { jb_str(jb, "meaning", "CS_LEVEL_START_TIME"); }
	else if (idx == 23) { jb_str(jb, "meaning", "CS_FLAGSTATUS"); }
	else if (idx == 24) { jb_str(jb, "meaning", "CS_SHADERSTATE"); }
	else if (idx == 25) { jb_str(jb, "meaning", "CS_BOTINFO"); }
	else if (idx == 27) { jb_str(jb, "meaning", "CS_ITEMS"); }
	else if (idx >= 32 && idx < 32 + 256) {
		jb_printf(jb, "\"meaning\":\"CS_MODELS[%d]\"", idx - 32);
	} else if (idx >= 288 && idx < 288 + 256) {
		jb_printf(jb, "\"meaning\":\"CS_SOUNDS[%d]\"", idx - 288);
	} else if (idx >= 544 && idx < 544 + 64) {
		jb_printf(jb, "\"meaning\":\"CS_PLAYERS[%d]\"", idx - 544);
	} else if (idx >= 608 && idx < 608 + 64) {
		jb_printf(jb, "\"meaning\":\"CS_LOCATIONS[%d]\"", idx - 608);
	} else if (idx >= 672 && idx < 672 + 64) {
		jb_printf(jb, "\"meaning\":\"CS_PARTICLES[%d]\"", idx - 672);
	}
}

// ------------------------------------------------------------------
// Annotated configstrings — with decoded index names
// ------------------------------------------------------------------

static void debug_cmd_configstrings_annotated(jsonBuf_t *jb, int startIdx, int endIdx) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "configstrings");

	if (endIdx <= 0 || endIdx > MAX_CONFIGSTRINGS) endIdx = MAX_CONFIGSTRINGS;
	if (startIdx < 0) startIdx = 0;

	jb_raw(jb, ",\"strings\":[");
	int first = 1;
	int i;
	for (i = startIdx; i < endIdx; i++) {
		if (!sv.configstrings[i] || !sv.configstrings[i][0]) continue;
		if (!first) jb_raw(jb, ",");
		first = 0;
		jb_raw(jb, "{");
		jb_int(jb, "index", i);
		jb_raw(jb, ",");
		debug_cs_decode_index(jb, i);
		jb_raw(jb, ",");
		jb_str(jb, "value", sv.configstrings[i]);
		jb_raw(jb, "}");
	}
	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Resolve modelindex to model name via configstrings
// ------------------------------------------------------------------

static const char *debug_resolve_modelindex(int modelindex) {
	if (modelindex <= 0) return "";
	int csIdx = 32 + modelindex; // CS_MODELS = 32
	if (csIdx >= MAX_CONFIGSTRINGS) return "";
	if (!sv.configstrings[csIdx]) return "";
	return sv.configstrings[csIdx];
}

static const char *debug_resolve_soundindex(int soundindex) {
	if (soundindex <= 0) return "";
	int csIdx = 288 + soundindex; // CS_SOUNDS = 288
	if (csIdx >= MAX_CONFIGSTRINGS) return "";
	if (!sv.configstrings[csIdx]) return "";
	return sv.configstrings[csIdx];
}

// ------------------------------------------------------------------
// Annotated entity list — with type names and resolved models
// ------------------------------------------------------------------

static void debug_cmd_entities_annotated(jsonBuf_t *jb, qboolean activeOnly) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "entity_list_annotated");
	jb_raw(jb, ",");
	jb_int(jb, "total", sv.num_entities);
	jb_raw(jb, ",\"entities\":[");

	if (sv.gentities) {
		int first = 1;
		int i;
		for (i = 0; i < sv.num_entities; i++) {
			sharedEntity_t *ent = SV_GentityNum(i);
			if (activeOnly && !ent->r.linked) continue;

			if (!first) jb_raw(jb, ",");
			first = 0;

			const char *modelName = debug_resolve_modelindex(ent->s.modelindex);
			const char *soundName = debug_resolve_soundindex(ent->s.loopSound);

			jb_raw(jb, "{");
			jb_int(jb, "num", i);
			jb_raw(jb, ","); jb_str(jb, "eTypeName", debug_eType_name(ent->s.eType));
			jb_raw(jb, ","); jb_int(jb, "eType", ent->s.eType);
			jb_raw(jb, ","); jb_bool(jb, "linked", ent->r.linked);
			jb_raw(jb, ","); jb_vec3(jb, "origin", ent->s.origin);
			jb_raw(jb, ","); jb_int(jb, "modelindex", ent->s.modelindex);
			if (modelName[0]) { jb_raw(jb, ","); jb_str(jb, "modelName", modelName); }
			if (soundName[0]) { jb_raw(jb, ","); jb_str(jb, "loopSoundName", soundName); }
			jb_raw(jb, ","); jb_int(jb, "svFlags", ent->r.svFlags);
			jb_raw(jb, ","); jb_int(jb, "weapon", ent->s.weapon);
			jb_raw(jb, ","); jb_int(jb, "contents", ent->r.contents);
			jb_raw(jb, "}");
		}
	}

	jb_raw(jb, "]}\n");
}

// ------------------------------------------------------------------
// Annotated single entity — full dump with all name resolutions
// ------------------------------------------------------------------

static void debug_cmd_entity_annotated(jsonBuf_t *jb, int num) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "entity_annotated");

	if (!sv.gentities || num < 0 || num >= sv.num_entities) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}

	sharedEntity_t *ent = SV_GentityNum(num);
	jb_raw(jb, ","); jb_int(jb, "num", num);
	jb_raw(jb, ","); jb_str(jb, "eTypeName", debug_eType_name(ent->s.eType));

	const char *mn = debug_resolve_modelindex(ent->s.modelindex);
	if (mn[0]) { jb_raw(jb, ","); jb_str(jb, "modelName", mn); }
	const char *mn2 = debug_resolve_modelindex(ent->s.modelindex2);
	if (mn2[0]) { jb_raw(jb, ","); jb_str(jb, "model2Name", mn2); }
	const char *sn = debug_resolve_soundindex(ent->s.loopSound);
	if (sn[0]) { jb_raw(jb, ","); jb_str(jb, "loopSoundName", sn); }

	// Include the full s and r dumps
	jb_raw(jb, ",");
	debug_write_entityState(jb, &ent->s);
	jb_raw(jb, ",");
	debug_write_entityShared(jb, &ent->r);

	svEntity_t *sve = &sv.svEntities[num];
	jb_raw(jb, ",\"sv\":{");
	jb_int(jb, "numClusters", sve->numClusters);
	jb_raw(jb, ","); jb_int(jb, "snapshotCounter", sve->snapshotCounter);
	jb_raw(jb, ","); jb_int(jb, "areanum", sve->areanum);
	jb_raw(jb, "}");

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Teleport entity — move origin + zero velocity + relink
// ------------------------------------------------------------------

static void debug_cmd_teleport(jsonBuf_t *jb, int num, const char *posStr) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "teleport");

	if (!sv.gentities || num < 0 || num >= sv.num_entities) {
		jb_raw(jb, ","); jb_str(jb, "error", "invalid entity number");
		jb_raw(jb, "}\n");
		return;
	}

	vec3_t pos = {0,0,0};
	sscanf(posStr, "%f,%f,%f", &pos[0], &pos[1], &pos[2]);

	sharedEntity_t *ent = SV_GentityNum(num);

	vec3_t oldOrigin;
	VectorCopy(ent->s.origin, oldOrigin);

	// Set all origin representations
	VectorCopy(pos, ent->s.origin);
	VectorCopy(pos, ent->s.pos.trBase);
	VectorCopy(pos, ent->r.currentOrigin);
	ent->s.pos.trType = TR_STATIONARY;
	ent->s.pos.trTime = sv.time;
	VectorClear(ent->s.pos.trDelta);

	// If this is a player entity, also update playerState
	if (num < sv_maxclients->integer && sv.gameClients) {
		playerState_t *ps = SV_GameClientNum(num);
		VectorCopy(pos, ps->origin);
		VectorClear(ps->velocity);
	}

	if (ent->r.linked) {
		SV_LinkEntity(ent);
	}

	jb_raw(jb, ","); jb_int(jb, "num", num);
	jb_raw(jb, ","); jb_vec3(jb, "oldOrigin", oldOrigin);
	jb_raw(jb, ","); jb_vec3(jb, "newOrigin", pos);
	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Multi-command batch — execute array of commands, return array
// ------------------------------------------------------------------

static void debug_handle_batch(debugClient_t *client, const char *json);

// ------------------------------------------------------------------
// SP syscall/import trace query
// ------------------------------------------------------------------

static void debug_cmd_sp_trace(jsonBuf_t *jb, const debugCmd_t *cmd) {
	jb_raw(jb, "{");
	jb_str(jb, "type", "sp_trace");

	if (!strcmp(cmd->strArg2, "start")) {
		spTraceEnabled = qtrue;
		spTraceHead = 0;
		spTraceCount = 0;
		jb_raw(jb, ","); jb_str(jb, "action", "started");
	} else if (!strcmp(cmd->strArg2, "stop")) {
		spTraceEnabled = qfalse;
		jb_raw(jb, ","); jb_str(jb, "action", "stopped");
		jb_raw(jb, ","); jb_int(jb, "totalCalls", spTraceCount);
	} else if (!strcmp(cmd->strArg2, "clear")) {
		spTraceHead = 0;
		spTraceCount = 0;
		jb_raw(jb, ","); jb_str(jb, "action", "cleared");
	} else {
		// Read trace entries with optional filters
		jb_raw(jb, ","); jb_bool(jb, "enabled", spTraceEnabled);
		jb_raw(jb, ","); jb_int(jb, "totalCalls", spTraceCount);
		jb_raw(jb, ",\"entries\":[");

		int startIdx = spTraceHead - SPTRACE_RING_SIZE;
		if (startIdx < 0) startIdx = 0;
		int sinceId = cmd->hasIntArg1 ? cmd->intArg1 : startIdx;
		if (sinceId < startIdx) sinceId = startIdx;
		if (sinceId > spTraceHead) sinceId = spTraceHead;

		int maxEntries = cmd->hasIntArg2 ? cmd->intArg2 : 200;
		if (maxEntries <= 0 || maxEntries > SPTRACE_RING_SIZE) maxEntries = 200;

		// Optional name filter from strArg1
		const char *nameFilter = cmd->strArg1[0] ? cmd->strArg1 : NULL;

		int first = 1;
		int count = 0;
		int nextId = sinceId;
		qboolean truncated = qfalse;
		int i;
		const int trailerReserve = 96;
		for (i = sinceId; i < spTraceHead && count < maxEntries; i++) {
			spTraceEntry_t *e = &spTraceRing[i % SPTRACE_RING_SIZE];
			char entryBuf[768];
			jsonBuf_t entryJb;

			// Apply name filter if specified
			if (nameFilter && !strstr(e->name, nameFilter)) {
				nextId = i + 1;
				continue;
			}

			jb_init(&entryJb, entryBuf, sizeof(entryBuf));
			jb_raw(&entryJb, "{");
			jb_int(&entryJb, "seq", i);
			jb_raw(&entryJb, ","); jb_int(&entryJb, "id", e->id);
			jb_raw(&entryJb, ","); jb_str(&entryJb, "name", e->name);
			jb_raw(&entryJb, ","); jb_bool(&entryJb, "isImport", e->isImport);
			jb_raw(&entryJb, ","); jb_int(&entryJb, "serverTime", e->serverTime);
			jb_raw(&entryJb, ","); jb_int(&entryJb, "frame", e->frameNum);
			jb_raw(&entryJb, ","); jb_intptr_array6(&entryJb, "args", e->args);
			jb_raw(&entryJb, ","); jb_intptr(&entryJb, "ret", e->retVal);
			if (e->strArg[0]) {
				jb_raw(&entryJb, ","); jb_str(&entryJb, "str", e->strArg);
			}
			jb_raw(&entryJb, "}");

			if (entryJb.overflowed ||
				!jb_can_append(jb, entryJb.len + (first ? 0 : 1) + trailerReserve)) {
				truncated = qtrue;
				break;
			}

			if (!first) jb_raw(jb, ",");
			first = 0;
			jb_raw_len(jb, entryBuf, entryJb.len);
			count++;
			nextId = i + 1;
		}
		jb_raw(jb, "],");
		jb_int(jb, "returned", count);
		jb_raw(jb, ","); jb_int(jb, "nextId", nextId);
		if (truncated) {
			jb_raw(jb, ","); jb_bool(jb, "truncated", qtrue);
		}
	}

	jb_raw(jb, "}\n");
}

// ------------------------------------------------------------------
// Command dispatch
// ------------------------------------------------------------------

static void debug_handle_command(debugClient_t *client, const char *json) {
	debugCmd_t cmd;
	debug_parse_cmd(json, &cmd);

	jsonBuf_t jb;
	jb_init(&jb, client->sendBuf + client->sendLen, DEBUG_SEND_BUF - client->sendLen);

	if (!strcmp(cmd.cmd, "status")) {
		debug_cmd_status(&jb);
	} else if (!strcmp(cmd.cmd, "entity")) {
		debug_cmd_entity(&jb, cmd.intArg1);
	} else if (!strcmp(cmd.cmd, "entities")) {
		debug_cmd_entities(&jb, cmd.hasBoolArg1 ? cmd.boolArg1 : qfalse);
	} else if (!strcmp(cmd.cmd, "player")) {
		debug_cmd_player(&jb, cmd.intArg1);
	} else if (!strcmp(cmd.cmd, "layout")) {
		debug_cmd_layout(&jb, cmd.strArg1[0] ? cmd.strArg1 : "all");
	} else if (!strcmp(cmd.cmd, "validate")) {
		if (cmd.hasIntArg1) {
			debug_cmd_validate(&jb, cmd.intArg1);
		} else {
			debug_cmd_validate_all(&jb);
		}
	} else if (!strcmp(cmd.cmd, "search")) {
		debug_cmd_search(&jb, cmd.strArg1, cmd.strArg2, cmd.intArg2);
	} else if (!strcmp(cmd.cmd, "cvar")) {
		debug_cmd_cvar(&jb, cmd.strArg1);
	} else if (!strcmp(cmd.cmd, "exec")) {
		/*
		 * Execute a console command on the server.  This allows MCP clients
		 * to change maps, toggle cvars, run "noclip", etc. without needing
		 * window focus.  The command is appended to the command buffer and
		 * will execute on the next server frame.
		 */
		if (cmd.strArg1[0]) {
			Cbuf_AddText( cmd.strArg1 );
			Cbuf_AddText( "\n" );
			jb_raw(&jb, "{");
			jb_str(&jb, "type", "exec_ok");
			jb_raw(&jb, ",");
			jb_str(&jb, "command", cmd.strArg1);
			jb_raw(&jb, "}\n");
		} else {
			jb_raw(&jb, "{");
			jb_str(&jb, "type", "error");
			jb_raw(&jb, ",");
			jb_str(&jb, "error", "exec requires a command string in 'str1'");
			jb_raw(&jb, "}\n");
		}
	} else if (!strcmp(cmd.cmd, "cvarlist")) {
		debug_cmd_cvarlist(&jb, cmd.strArg1);
	} else if (!strcmp(cmd.cmd, "configstrings")) {
		debug_cmd_configstrings_annotated(&jb, cmd.intArg1, cmd.hasIntArg2 ? cmd.intArg2 : 0);
	} else if (!strcmp(cmd.cmd, "log")) {
		debug_cmd_log(&jb, cmd.intArg1, cmd.hasIntArg2 ? cmd.intArg2 : 100, cmd.strArg1);
	} else if (!strcmp(cmd.cmd, "set_entity")) {
		debug_cmd_set_entity(&jb, cmd.intArg1, cmd.strArg1, cmd.intArg2);
	} else if (!strcmp(cmd.cmd, "trace")) {
		debug_cmd_trace(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "entities_in_box")) {
		debug_cmd_entities_in_box(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "watch")) {
		debug_cmd_watch(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "vmtrace")) {
		debug_cmd_vmtrace(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "memory")) {
		debug_cmd_memory(&jb);
	} else if (!strcmp(cmd.cmd, "set_player")) {
		debug_cmd_set_player(&jb, cmd.intArg1, cmd.strArg1, cmd.intArg2);
	} else if (!strcmp(cmd.cmd, "time")) {
		debug_cmd_time(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "maplist")) {
		debug_cmd_maplist(&jb);
	} else if (!strcmp(cmd.cmd, "snapshot")) {
		debug_cmd_snapshot(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "peek")) {
		debug_cmd_peek(&jb, cmd.intArg1, cmd.hasIntArg2 ? cmd.intArg2 : 0, 64);
	} else if (!strcmp(cmd.cmd, "point_contents")) {
		debug_cmd_point_contents(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "strings")) {
		debug_cmd_strings(&jb, cmd.intArg1);
	} else if (!strcmp(cmd.cmd, "funcscan")) {
		debug_cmd_funcscan(&jb, cmd.intArg1);
	} else if (!strcmp(cmd.cmd, "bulkscan")) {
		debug_cmd_bulkscan(&jb, cmd.intArg1, cmd.hasIntArg2 ? cmd.intArg2 : 4,
			cmd.hasBoolArg1 ? cmd.boolArg1 : qfalse);
	} else if (!strcmp(cmd.cmd, "client_peek")) {
		debug_cmd_client_peek(&jb, cmd.intArg1, cmd.hasIntArg2 ? cmd.intArg2 : 0, 64);
	} else if (!strcmp(cmd.cmd, "client_strings")) {
		debug_cmd_client_strings(&jb, cmd.intArg1);
	} else if (!strcmp(cmd.cmd, "entities_annotated")) {
		debug_cmd_entities_annotated(&jb, cmd.hasBoolArg1 ? cmd.boolArg1 : qfalse);
	} else if (!strcmp(cmd.cmd, "entity_annotated")) {
		debug_cmd_entity_annotated(&jb, cmd.intArg1);
	} else if (!strcmp(cmd.cmd, "teleport")) {
		debug_cmd_teleport(&jb, cmd.intArg1, cmd.strArg1);
	} else if (!strcmp(cmd.cmd, "batch")) {
		// Handled specially — see debug_handle_batch
		jb_raw(&jb, "{");
		jb_str(&jb, "type", "error");
		jb_raw(&jb, ",");
		jb_str(&jb, "error", "batch must be sent as a JSON array, not an object");
		jb_raw(&jb, "}\n");
	} else if (!strcmp(cmd.cmd, "sp_trace")) {
		debug_cmd_sp_trace(&jb, &cmd);
	} else if (!strcmp(cmd.cmd, "ping")) {
		jb_raw(&jb, "{\"type\":\"pong\"}\n");
	} else {
		jb_raw(&jb, "{");
		jb_str(&jb, "type", "error");
		jb_raw(&jb, ",");
		jb_str(&jb, "error", "unknown command");
		jb_raw(&jb, ",");
		jb_str(&jb, "available", "status, entity, entities, entities_annotated, entity_annotated, player, layout, validate, search, cvar, cvarlist, configstrings, log, exec, set_entity, set_player, trace, entities_in_box, watch, vmtrace, memory, time, maplist, snapshot, peek, point_contents, strings, funcscan, bulkscan, client_peek, client_strings, teleport, sp_trace, batch, ping");
		jb_raw(&jb, "}\n");
	}

	client->sendLen += jb.len;
}

// ------------------------------------------------------------------
// Batch handler — parse JSON array, execute each, wrap in array
// ------------------------------------------------------------------

static void debug_handle_batch(debugClient_t *client, const char *json) {
	// Prepend "[" to output
	if (client->sendLen < DEBUG_SEND_BUF - 1) {
		client->sendBuf[client->sendLen++] = '[';
	}

	// Walk the array: find each {...} object and dispatch it
	const char *p = json;
	if (*p == '[') p++;
	int first = 1;
	int depth = 0;
	const char *objStart = NULL;

	while (*p) {
		if (*p == '{' && depth == 0) {
			objStart = p;
			depth = 1;
		} else if (*p == '{') {
			depth++;
		} else if (*p == '}') {
			depth--;
			if (depth == 0 && objStart) {
				// Extract this object
				int objLen = (int)(p - objStart + 1);
				char objBuf[DEBUG_RECV_BUF];
				if (objLen < (int)sizeof(objBuf)) {
					memcpy(objBuf, objStart, objLen);
					objBuf[objLen] = '\0';

					if (!first && client->sendLen < DEBUG_SEND_BUF - 1) {
						client->sendBuf[client->sendLen++] = ',';
					}
					first = 0;

					// The command handler appends to sendBuf with a trailing \n
					// We need to replace that \n with nothing (it's inside our array)
					int beforeLen = client->sendLen;
					debug_handle_command(client, objBuf);
					// Remove trailing \n that the handler added
					if (client->sendLen > beforeLen &&
						client->sendBuf[client->sendLen - 1] == '\n') {
						client->sendLen--;
					}
				}
				objStart = NULL;
			}
		}
		p++;
	}

	// Close array + newline
	if (client->sendLen < DEBUG_SEND_BUF - 2) {
		client->sendBuf[client->sendLen++] = ']';
		client->sendBuf[client->sendLen++] = '\n';
	}
}

// ------------------------------------------------------------------
// Socket helpers
// ------------------------------------------------------------------

static void debug_set_nonblocking(debug_socket_t sock) {
#ifdef _WIN32
	u_long mode = 1;
	ioctlsocket(sock, FIONBIO, &mode);
#else
	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif
}

// ------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------

void DebugServer_Init(void) {
	int i;

	sv_debugPort = Cvar_Get("sv_debugPort", va("%d", DEBUG_DEFAULT_PORT), CVAR_ARCHIVE);

	for (i = 0; i < DEBUG_MAX_CLIENTS; i++) {
		debugClients[i].sock = DEBUG_INVALID_SOCK;
		debugClients[i].recvLen = 0;
		debugClients[i].sendLen = 0;
	}

	if (sv_debugPort->integer <= 0) {
		Com_Printf("Debug server disabled (sv_debugPort = 0)\n");
		debugInitialized = qfalse;
		return;
	}

#ifdef _WIN32
	{
		WSADATA wsaData;
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
			Com_Printf("Debug server: WSAStartup failed\n");
			return;
		}
	}
#endif

	struct sockaddr_in addr;
	debugListenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (debugListenSock == DEBUG_INVALID_SOCK) {
#ifdef _WIN32
		Com_Printf("Debug server: failed to create socket (WSA error %d)\n", WSAGetLastError());
#else
		Com_Printf("Debug server: failed to create socket (errno %d)\n", errno);
#endif
		return;
	}

	// Allow address reuse
	int opt = 1;
	setsockopt(debugListenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // localhost only for security
	addr.sin_port = htons((unsigned short)sv_debugPort->integer);

	if (bind(debugListenSock, (struct sockaddr*)&addr, sizeof(addr)) == DEBUG_SOCK_ERROR) {
		Com_Printf("Debug server: failed to bind to port %d\n", sv_debugPort->integer);
		debug_closesocket(debugListenSock);
		debugListenSock = DEBUG_INVALID_SOCK;
		return;
	}

	if (listen(debugListenSock, 4) == DEBUG_SOCK_ERROR) {
		Com_Printf("Debug server: failed to listen\n");
		debug_closesocket(debugListenSock);
		debugListenSock = DEBUG_INVALID_SOCK;
		return;
	}

	debug_set_nonblocking(debugListenSock);

	Com_Printf("Debug server listening on 127.0.0.1:%d\n", sv_debugPort->integer);
	debugInitialized = qtrue;
}

void DebugServer_Shutdown(void) {
	int i;

	if (!debugInitialized) return;

	for (i = 0; i < DEBUG_MAX_CLIENTS; i++) {
		if (debugClients[i].sock != DEBUG_INVALID_SOCK) {
			debug_closesocket(debugClients[i].sock);
			debugClients[i].sock = DEBUG_INVALID_SOCK;
		}
	}

	if (debugListenSock != DEBUG_INVALID_SOCK) {
		debug_closesocket(debugListenSock);
		debugListenSock = DEBUG_INVALID_SOCK;
	}

	debugInitialized = qfalse;
	Com_Printf("Debug server shut down\n");
}

void DebugServer_Frame(void) {
	int i;

	if (!debugInitialized) return;

	// Accept new connections
	{
		struct sockaddr_in clientAddr;
		int addrLen = sizeof(clientAddr);
		debug_socket_t newSock = accept(debugListenSock, (struct sockaddr*)&clientAddr, (socklen_t*)&addrLen);

		if (newSock != DEBUG_INVALID_SOCK) {
			// Find a free slot
			int slot = -1;
			for (i = 0; i < DEBUG_MAX_CLIENTS; i++) {
				if (debugClients[i].sock == DEBUG_INVALID_SOCK) {
					slot = i;
					break;
				}
			}

			if (slot >= 0) {
				debug_set_nonblocking(newSock);
				debugClients[slot].sock = newSock;
				debugClients[slot].recvLen = 0;
				debugClients[slot].sendLen = 0;
				Com_DPrintf("Debug server: client connected (slot %d)\n", slot);
			} else {
				// No free slots
				const char *msg = "{\"type\":\"error\",\"error\":\"server full\"}\n";
				send(newSock, msg, strlen(msg), 0);
				debug_closesocket(newSock);
			}
		}
	}

	// Process each connected client
	for (i = 0; i < DEBUG_MAX_CLIENTS; i++) {
		debugClient_t *client = &debugClients[i];
		if (client->sock == DEBUG_INVALID_SOCK) continue;

		// Try to send pending data
		if (client->sendLen > 0) {
			int sent = send(client->sock, client->sendBuf, client->sendLen, 0);
			if (sent > 0) {
				if (sent < client->sendLen) {
					memmove(client->sendBuf, client->sendBuf + sent, client->sendLen - sent);
				}
				client->sendLen -= sent;
			} else if (sent == DEBUG_SOCK_ERROR && !debug_wouldblock()) {
				// Connection error
				debug_closesocket(client->sock);
				client->sock = DEBUG_INVALID_SOCK;
				Com_DPrintf("Debug server: client disconnected (slot %d, send error)\n", i);
				continue;
			}
		}

		// Try to receive data
		int avail = DEBUG_RECV_BUF - client->recvLen - 1;
		if (avail > 0) {
			int recvd = recv(client->sock, client->recvBuf + client->recvLen, avail, 0);
			if (recvd > 0) {
				client->recvLen += recvd;
				client->recvBuf[client->recvLen] = '\0';

				// Process complete lines (newline-delimited JSON)
				char *line = client->recvBuf;
				char *nl;
				while ((nl = strchr(line, '\n')) != NULL) {
					*nl = '\0';
					// Trim \r if present
					if (nl > line && *(nl-1) == '\r') *(nl-1) = '\0';
					if (line[0] == '[') {
						debug_handle_batch(client, line);
					} else if (line[0] != '\0') {
						debug_handle_command(client, line);
					}
					line = nl + 1;
				}

				// Move remaining partial data to front of buffer
				if (line > client->recvBuf) {
					int remaining = client->recvLen - (int)(line - client->recvBuf);
					if (remaining > 0) {
						memmove(client->recvBuf, line, remaining);
					}
					client->recvLen = remaining;
				}
			} else if (recvd == 0) {
				// Clean disconnect
				debug_closesocket(client->sock);
				client->sock = DEBUG_INVALID_SOCK;
				Com_DPrintf("Debug server: client disconnected (slot %d)\n", i);
			} else if (!debug_wouldblock()) {
				// Error
				debug_closesocket(client->sock);
				client->sock = DEBUG_INVALID_SOCK;
				Com_DPrintf("Debug server: client disconnected (slot %d, recv error)\n", i);
			}
		}
	}
}
