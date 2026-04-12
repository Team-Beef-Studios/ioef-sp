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
// JSON response builder helpers
// ------------------------------------------------------------------

typedef struct {
	char    *buf;
	int     len;
	int     cap;
} jsonBuf_t;

static void jb_init(jsonBuf_t *jb, char *buf, int cap) {
	jb->buf = buf;
	jb->len = 0;
	jb->cap = cap;
}

static void jb_raw(jsonBuf_t *jb, const char *s) {
	int slen = strlen(s);
	if (jb->len + slen < jb->cap) {
		memcpy(jb->buf + jb->len, s, slen);
		jb->len += slen;
	}
}

static void jb_printf(jsonBuf_t *jb, const char *fmt, ...) {
	va_list ap;
	int avail = jb->cap - jb->len;
	if (avail <= 0) return;
	va_start(ap, fmt);
	int n = vsnprintf(jb->buf + jb->len, avail, fmt, ap);
	va_end(ap);
	if (n > 0 && n < avail) jb->len += n;
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
		} else if (!strcmp(key, "name") || !strcmp(key, "struct") || !strcmp(key, "field")) {
			p = json_extract_string(p, cmd->strArg1, sizeof(cmd->strArg1));
		} else if (!strcmp(key, "command") || !strcmp(key, "op")) {
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
		jb_str(jb, "available", "entityState_t, entityShared_t, playerState_t, sharedEntity_t, trajectory_t, usercmd_t, all");
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
	} else if (!strcmp(cmd.cmd, "ping")) {
		jb_raw(&jb, "{\"type\":\"pong\"}\n");
	} else {
		jb_raw(&jb, "{");
		jb_str(&jb, "type", "error");
		jb_raw(&jb, ",");
		jb_str(&jb, "error", "unknown command");
		jb_raw(&jb, ",");
		jb_str(&jb, "available", "status, entity, entities, player, layout, validate, search, cvar, ping");
		jb_raw(&jb, "}\n");
	}

	client->sendLen += jb.len;
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
					if (line[0] != '\0') {
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
