import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import * as net from "node:net";
import { spawn, execSync } from "node:child_process";
import * as path from "node:path";
import * as fs from "node:fs";

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

const GAME_HOST = process.env.IOEF_DEBUG_HOST ?? "127.0.0.1";
const GAME_PORT = parseInt(process.env.IOEF_DEBUG_PORT ?? "29070", 10);
const CONNECT_TIMEOUT = 3000;
const RESPONSE_TIMEOUT = 5000;

// Resolve game paths relative to this MCP server's location (mcp/dist/)
const MCP_ROOT = path.resolve(
  path.dirname(new URL(import.meta.url).pathname).replace(/^\/([A-Z]:)/i, "$1")
);
const IOEF_ROOT = path.resolve(MCP_ROOT, "..", "..");
const BUILD_DIR = path.join(IOEF_ROOT, "build", process.env.IOEF_BUILD_DIR ?? "release-mingw32-x86");
const DEDICATED_EXE = path.join(BUILD_DIR, process.env.IOEF_DED_EXE ?? "ioq3ded.x86.exe");
const CLIENT_EXE = path.join(BUILD_DIR, process.env.IOEF_CLIENT_EXE ?? "ioquake3.x86.exe");

// ---------------------------------------------------------------------------
// Game process management
// ---------------------------------------------------------------------------

const DED_EXE_NAME = path.basename(DEDICATED_EXE);
const CLIENT_EXE_NAME = path.basename(CLIENT_EXE);

function isGameRunning(): boolean {
  for (const name of [DED_EXE_NAME, CLIENT_EXE_NAME]) {
    try {
      const out = execSync(`tasklist /FI "IMAGENAME eq ${name}" /NH`, {
        encoding: "utf8",
        stdio: ["pipe", "pipe", "pipe"],
      });
      if (out.includes(name)) return true;
    } catch {}
  }
  return false;
}

function isDebugPortOpen(): Promise<boolean> {
  return new Promise((resolve) => {
    const s = new net.Socket();
    s.setTimeout(1000);
    s.on("connect", () => {
      s.destroy();
      resolve(true);
    });
    s.on("timeout", () => {
      s.destroy();
      resolve(false);
    });
    s.on("error", () => {
      resolve(false);
    });
    s.connect(GAME_PORT, GAME_HOST);
  });
}

function launchGame(
  mode: "dedicated" | "client",
  map: string,
  extraArgs: string[]
): { pid: number; exe: string } {
  const exe = mode === "dedicated" ? DEDICATED_EXE : CLIENT_EXE;

  if (!fs.existsSync(exe)) {
    throw new Error(`Game executable not found: ${exe}`);
  }

  const args = [
    "+set", "sv_debugPort", String(GAME_PORT),
    ...(mode === "dedicated" ? ["+set", "dedicated", "1"] : []),
    "+map", map,
    ...extraArgs,
  ];

  const child = spawn(exe, args, {
    cwd: BUILD_DIR,
    detached: true,
    stdio: "ignore",
  });
  child.unref();

  return { pid: child.pid ?? 0, exe: path.basename(exe) };
}

async function waitForDebugPort(
  timeoutMs: number = 15000,
  pollMs: number = 500
): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await isDebugPortOpen()) return true;
    await new Promise((r) => setTimeout(r, pollMs));
  }
  return false;
}

// ---------------------------------------------------------------------------
// TCP query to the game engine's debug server
// ---------------------------------------------------------------------------

async function queryGame(command: Record<string, unknown>): Promise<unknown> {
  return new Promise((resolve, reject) => {
    const socket = new net.Socket();
    let buffer = "";
    let settled = false;

    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        socket.destroy();
        reject(new Error("Timeout waiting for game response"));
      }
    }, RESPONSE_TIMEOUT);

    socket.setTimeout(CONNECT_TIMEOUT);

    socket.on("connect", () => {
      socket.write(JSON.stringify(command) + "\n");
    });

    socket.on("data", (data) => {
      buffer += data.toString();
      const nlIdx = buffer.indexOf("\n");
      if (nlIdx !== -1) {
        const line = buffer.slice(0, nlIdx).trim();
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          socket.destroy();
          try {
            resolve(JSON.parse(line));
          } catch {
            resolve({ raw: line });
          }
        }
      }
    });

    socket.on("timeout", () => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        socket.destroy();
        reject(
          new Error(
            `Connection timeout — is the game running with sv_debugPort ${GAME_PORT}?`
          )
        );
      }
    });

    socket.on("error", (err) => {
      if (!settled) {
        settled = true;
        clearTimeout(timer);
        reject(
          new Error(
            `Cannot connect to game at ${GAME_HOST}:${GAME_PORT} — ${err.message}. ` +
              `Use the launch_game tool to start the game first.`
          )
        );
      }
    });

    socket.connect(GAME_PORT, GAME_HOST);
  });
}

function formatJson(obj: unknown): string {
  return JSON.stringify(obj, null, 2);
}

// ---------------------------------------------------------------------------
// MCP Server
// ---------------------------------------------------------------------------

const server = new McpServer({
  name: "ioef-game-inspector",
  version: "1.2.0",
});

// -- game_status ------------------------------------------------------------

server.tool(
  "game_status",
  "Check if the ioEF game is running and whether the debug port is reachable. Use this to decide whether you need to call launch_game first. Returns process state, port connectivity, and if connected, full server status (map, entities, clients, fps).",
  {},
  async () => {
    const running = isGameRunning();
    const portOpen = await isDebugPortOpen();

    const info: Record<string, unknown> = {
      process_running: running,
      debug_port_open: portOpen,
      debug_port: GAME_PORT,
      build_dir: BUILD_DIR,
      dedicated_exe_exists: fs.existsSync(DEDICATED_EXE),
      client_exe_exists: fs.existsSync(CLIENT_EXE),
    };

    if (portOpen) {
      try {
        const status = await queryGame({ cmd: "status" });
        info.server = status;
      } catch {}
    }

    return { content: [{ type: "text", text: formatJson(info) }] };
  }
);

// -- launch_game ------------------------------------------------------------

server.tool(
  "launch_game",
  "Launch the ioEF game engine and load a map. Starts the dedicated server or full client, waits for the debug port to become available, then returns. The game must be built first (ioquake3.x86_64.exe / ioq3ded.x86_64.exe in the build directory).",
  {
    map: z
      .string()
      .default("ctf_and1")
      .describe("Map name to load (e.g. 'ctf_and1', 'hm_voy1', 'borg1')"),
    mode: z
      .enum(["dedicated", "client"])
      .default("dedicated")
      .describe("'dedicated' for headless server (default), 'client' for full game with graphics"),
    extra_args: z
      .array(z.string())
      .default([])
      .describe("Additional command-line arguments (e.g. ['+set', 'sv_maxclients', '16'])"),
    wait: z
      .boolean()
      .default(true)
      .describe("Wait for the debug port to become available before returning (default: true)"),
  },
  async ({ map, mode, extra_args, wait }) => {
    // Check if already running and connected
    if (await isDebugPortOpen()) {
      const status = await queryGame({ cmd: "status" });
      return {
        content: [
          {
            type: "text",
            text: formatJson({
              already_running: true,
              message: "Game is already running with debug port active.",
              status,
            }),
          },
        ],
      };
    }

    // Check if process is running but port not open (map not loaded yet)
    if (isGameRunning()) {
      if (wait) {
        const ready = await waitForDebugPort(20000);
        if (ready) {
          const status = await queryGame({ cmd: "status" });
          return {
            content: [
              {
                type: "text",
                text: formatJson({
                  was_starting: true,
                  message: "Game was already starting — debug port is now ready.",
                  status,
                }),
              },
            ],
          };
        }
      }
      return {
        content: [
          {
            type: "text",
            text: formatJson({
              process_running: true,
              debug_port_open: false,
              message:
                "Game process is running but debug port is not open. A map may not be loaded, or sv_debugPort may be 0.",
            }),
          },
        ],
      };
    }

    // Launch
    let result: { pid: number; exe: string };
    try {
      result = launchGame(mode, map, extra_args);
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : String(err);
      return {
        content: [{ type: "text", text: formatJson({ error: msg }) }],
        isError: true,
      };
    }

    const response: Record<string, unknown> = {
      launched: true,
      pid: result.pid,
      exe: result.exe,
      mode,
      map,
    };

    if (wait) {
      const ready = await waitForDebugPort(20000);
      response.debug_port_ready = ready;
      if (ready) {
        try {
          response.status = await queryGame({ cmd: "status" });
        } catch {}
      } else {
        response.message =
          "Game launched but debug port not yet available. The map may still be loading — try game_status in a few seconds.";
      }
    }

    return { content: [{ type: "text", text: formatJson(response) }] };
  }
);

// -- stop_game --------------------------------------------------------------

server.tool(
  "stop_game",
  "Stop the running ioEF game process (dedicated server or client).",
  {},
  async () => {
    const results: string[] = [];
    for (const name of [DED_EXE_NAME, CLIENT_EXE_NAME, "ioq3ded.x86_64.exe", "ioquake3.x86_64.exe"]) {
      try {
        execSync(`taskkill /F /IM "${name}"`, {
          encoding: "utf8",
          stdio: ["pipe", "pipe", "pipe"],
        });
        results.push(`Killed ${name}`);
      } catch {
        // not running
      }
    }
    if (results.length === 0) results.push("No game processes were running.");
    return {
      content: [{ type: "text", text: formatJson({ actions: results }) }],
    };
  }
);

// -- server_status ----------------------------------------------------------

server.tool(
  "server_status",
  "Get current game server status: map, time, entity count, connected clients, frame rate, and server state. Requires the game to be running — use game_status or launch_game first.",
  {},
  async () => {
    const result = await queryGame({ cmd: "status" });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- inspect_entity ---------------------------------------------------------

server.tool(
  "inspect_entity",
  "Get full state of a game entity by number — entityState_t (position, type, flags, trajectory, model, weapon, animation) and entityShared_t (collision, linking, bounding box, visibility flags). Returns everything the server knows about this entity.",
  { num: z.number().int().describe("Entity number (0 to num_entities-1)") },
  async ({ num }) => {
    const result = await queryGame({ cmd: "entity", num });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- list_entities ----------------------------------------------------------

server.tool(
  "list_entities",
  "List all entities in the game world with summary info (entity number, type, origin, flags, model). Set active_only=true to filter to only linked/active entities.",
  {
    active_only: z
      .boolean()
      .default(false)
      .describe("If true, only return linked (active) entities"),
  },
  async ({ active_only }) => {
    const result = await queryGame({ cmd: "entities", active_only });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- inspect_player ---------------------------------------------------------

server.tool(
  "inspect_player",
  "Get full player state for a connected client — playerState_t including position, velocity, weapon, health (stats), ammo, powerups, animation state, damage feedback, view angles, and movement info.",
  { num: z.number().int().describe("Client number (0 to maxclients-1)") },
  async ({ num }) => {
    const result = await queryGame({ cmd: "player", num });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- memory_layout ----------------------------------------------------------

server.tool(
  "memory_layout",
  "Get the compiled memory layout of a game struct — field names, byte offsets, and sizes. Use this to verify struct packing, find padding, and validate that field offsets match expectations. Available structs: entityState_t, entityShared_t, playerState_t, sharedEntity_t, trajectory_t, usercmd_t, or 'all' for a size summary.",
  {
    struct_name: z
      .string()
      .default("all")
      .describe(
        "Struct name: entityState_t, entityShared_t, playerState_t, sharedEntity_t, trajectory_t, usercmd_t, or 'all'"
      ),
  },
  async ({ struct_name }) => {
    const result = await queryGame({ cmd: "layout", struct: struct_name });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- validate_entity --------------------------------------------------------

server.tool(
  "validate_entity",
  "Run structural validation on a specific entity — checks for NaN/Inf in origin/angles/trajectory, inverted bounding boxes, out-of-range entity references, invalid trajectory types, suspicious field values, and consistency between s.origin and r.currentOrigin.",
  { num: z.number().int().describe("Entity number to validate") },
  async ({ num }) => {
    const result = await queryGame({ cmd: "validate", num });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- validate_all -----------------------------------------------------------

server.tool(
  "validate_all",
  "Run validation across ALL active entities and return a summary of issues found — NaN origins, inverted bounds, invalid trajectory types, out-of-range references. Great for catching corruption or uninitialized state.",
  {},
  async () => {
    const result = await queryGame({ cmd: "validate" });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- search_entities --------------------------------------------------------

server.tool(
  "search_entities",
  "Search entities by field value. Supports fields: eType, eFlags, weapon, modelindex, clientNum, svFlags, contents, linked, groundEntityNum, solid, event, powerups, ownerNum. Operators: eq, ne, gt, lt, gte, lte, mask (bitwise AND).",
  {
    field: z.string().describe("Field name to search (e.g. 'eType', 'weapon', 'svFlags')"),
    value: z.number().int().describe("Value to compare against"),
    op: z
      .enum(["eq", "ne", "gt", "lt", "gte", "lte", "mask"])
      .default("eq")
      .describe("Comparison operator (default: eq)"),
  },
  async ({ field, value, op }) => {
    const result = await queryGame({ cmd: "search", field, value, op });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- get_cvar ---------------------------------------------------------------

server.tool(
  "get_cvar",
  "Read the current value of a game engine cvar (console variable). Returns both string and integer representations.",
  { name: z.string().describe("Cvar name (e.g. 'sv_maxclients', 'g_gametype', 'sv_fps')") },
  async ({ name }) => {
    const result = await queryGame({ cmd: "cvar", name });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- ping -------------------------------------------------------------------

server.tool(
  "ping_game",
  "Test connectivity to the game's debug server. Returns pong if the game is running and the debug port is active.",
  {},
  async () => {
    const result = await queryGame({ cmd: "ping" });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- exec_command -------------------------------------------------------------

server.tool(
  "exec_command",
  "Execute a console command on the running game server. Use this to change maps ('map borg2'), toggle cheats ('noclip'), set cvars ('set g_speed 400'), etc. The command runs on the next server frame. Examples: 'map voy1', 'devmap borg1', 'noclip', 'god', 'give all', 'set r_fullbright 1; vid_restart'.",
  {
    command: z
      .string()
      .describe(
        "Console command to execute (e.g. 'map borg2', 'noclip', 'set sv_cheats 1')"
      ),
  },
  async ({ command }) => {
    const result = await queryGame({ cmd: "exec", str1: command });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- set_cvar ----------------------------------------------------------------

server.tool(
  "set_cvar",
  "Set a game engine cvar (console variable) to a new value. Shorthand for exec_command('set name value').",
  {
    name: z.string().describe("Cvar name"),
    value: z.string().describe("New value"),
  },
  async ({ name, value }) => {
    const result = await queryGame({ cmd: "exec", str1: `set ${name} ${value}` });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- cvar_list ----------------------------------------------------------------

server.tool(
  "cvar_list",
  "List all registered cvars in the engine, optionally filtered by name substring. Returns name, value, flags, default, and description for each cvar. Useful for discovering available settings.",
  {
    filter: z.string().default("").describe("Optional substring filter for cvar names (e.g. 'sv_', 'g_', 'r_')"),
  },
  async ({ filter }) => {
    const result = await queryGame({ cmd: "cvarlist", str1: filter });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- configstrings ------------------------------------------------------------

server.tool(
  "configstrings",
  "Dump the server's configstrings — indexed string table shared between server and clients. Contains model names, shader names, player info, game state, sound indices, etc. Only non-empty entries are returned.",
  {
    start: z.number().int().default(0).describe("Start index (default 0)"),
    end: z.number().int().default(0).describe("End index (default 0 = all 1024)"),
  },
  async ({ start, end }) => {
    const result = await queryGame({ cmd: "configstrings", num: start, value: end });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- game_log -----------------------------------------------------------------

server.tool(
  "game_log",
  "Read captured engine log output (Com_Printf ring buffer). Supports cursor-based polling via since_id — pass the nextId from a previous call to get only new lines. Filter by substring to find specific messages.",
  {
    since_id: z.number().int().default(0).describe("Only return lines after this ID (for polling). Default 0 = recent."),
    max_lines: z.number().int().default(100).describe("Max lines to return (default 100, max 512)"),
    filter: z.string().default("").describe("Optional substring filter"),
  },
  async ({ since_id, max_lines, filter }) => {
    const result = await queryGame({ cmd: "log", num: since_id, value: max_lines, str1: filter });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- set_entity ---------------------------------------------------------------

server.tool(
  "set_entity",
  "Modify a field on a game entity at runtime. Changes take effect immediately and the entity is re-linked in the spatial hash. Writable fields: eType, eFlags, weapon, modelindex, modelindex2, clientNum, frame, solid, event, eventParm, powerups, loopSound, svFlags, contents, ownerNum, origin.x, origin.y, origin.z, legsAnim, torsoAnim.",
  {
    num: z.number().int().describe("Entity number"),
    field: z.string().describe("Field name to modify"),
    value: z.number().int().describe("New integer value"),
  },
  async ({ num, field, value }) => {
    const result = await queryGame({ cmd: "set_entity", num, str1: field, value });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- trace --------------------------------------------------------------------

server.tool(
  "trace",
  "Cast a ray between two points in the game world and report what it hits. Returns fraction (0-1), hit position, entity number, surface flags, plane normal. Use for line-of-sight checks, collision testing, finding walls/floors.",
  {
    start: z.string().describe("Start point as 'x,y,z' (e.g. '0,-1438,-400')"),
    end: z.string().describe("End point as 'x,y,z' (e.g. '0,-1438,-500')"),
    contentmask: z.number().int().default(-1).describe("Content mask for filtering (-1 = all)"),
  },
  async ({ start, end, contentmask }) => {
    const result = await queryGame({ cmd: "trace", str1: start, str2: end, value: contentmask });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- entities_in_box ----------------------------------------------------------

server.tool(
  "entities_in_box",
  "Find all entities within an axis-aligned bounding box. Returns entity numbers, types, and positions for everything in the region.",
  {
    mins: z.string().describe("Min corner as 'x,y,z' (e.g. '-500,-500,-500')"),
    maxs: z.string().describe("Max corner as 'x,y,z' (e.g. '500,500,500')"),
  },
  async ({ mins, maxs }) => {
    const result = await queryGame({ cmd: "entities_in_box", str1: mins, str2: maxs });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- watch_entity -------------------------------------------------------------

server.tool(
  "watch_entity",
  "Set up watchpoints on entity fields to detect changes. Actions: 'add' (watch a field), 'remove' (by slot), 'clear' (remove all), or omit to poll current watchpoints and see which changed since last check.",
  {
    action: z
      .enum(["add", "remove", "clear", "poll"])
      .default("poll")
      .describe("Action to perform"),
    num: z.number().int().default(0).describe("Entity number (for 'add') or slot number (for 'remove')"),
    field: z.string().default("").describe("Field name to watch (for 'add')"),
  },
  async ({ action, num, field }) => {
    const result = await queryGame({ cmd: "watch", num, str1: field, str2: action });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- vm_trace -----------------------------------------------------------------

server.tool(
  "vm_trace",
  "Trace game VM calls (GAME_RUN_FRAME, GAME_CLIENT_THINK, etc.) with timing. Actions: 'start' to begin tracing, 'stop' to end, or omit to read captured trace data. Shows which game module functions are being called and how long each takes.",
  {
    action: z
      .enum(["start", "stop", "read"])
      .default("read")
      .describe("'start' to begin, 'stop' to end, 'read' to dump captured data"),
    since_id: z.number().int().default(0).describe("Only return entries after this ID (for polling)"),
  },
  async ({ action, since_id }) => {
    const op = action === "read" ? "" : action;
    const result = await queryGame({ cmd: "vmtrace", num: since_id, str2: op });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- memory -------------------------------------------------------------------

server.tool(
  "memory_stats",
  "Get engine memory statistics — hunk free bytes, entity/client allocation sizes, entity counts, snapshot buffer usage.",
  {},
  async () => {
    const result = await queryGame({ cmd: "memory" });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- set_player ---------------------------------------------------------------

server.tool(
  "set_player",
  "Modify a player's state at runtime. Writable fields: pm_type, pm_flags, gravity, speed, weapon, weaponstate, weaponTime, viewheight, eFlags, groundEntityNum, origin.x/y/z, velocity.x/y/z, viewangles.pitch/yaw, stats.N (0-15), ammo.N (0-15), persistant.N (0-15), powerups.N (0-15). stats.0 = health in most gametypes.",
  {
    num: z.number().int().describe("Client number (0 to maxclients-1)"),
    field: z.string().describe("Field name (e.g. 'stats.0' for health, 'origin.z', 'weapon')"),
    value: z.number().int().describe("New integer value"),
  },
  async ({ num, field, value }) => {
    const result = await queryGame({ cmd: "set_player", num, str1: field, value });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- time_control -------------------------------------------------------------

server.tool(
  "time_control",
  "Control the game's time flow. Actions: 'pause' freezes the game simulation (MCP still responds), 'resume' unpauses, 'step' advances N frames then re-pauses, 'timescale' sets speed multiplier (value in hundredths: 50=0.5x, 100=1x, 200=2x), or omit action to query current state.",
  {
    action: z
      .enum(["pause", "resume", "step", "timescale", "status"])
      .default("status")
      .describe("Action to perform"),
    value: z
      .number()
      .int()
      .default(0)
      .describe("For 'step': number of frames. For 'timescale': speed * 100 (e.g. 50 = half speed)."),
  },
  async ({ action, value }) => {
    const op = action === "status" ? "" : action;
    const result = await queryGame({ cmd: "time", num: value, str2: op });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- map_list -----------------------------------------------------------------

server.tool(
  "map_list",
  "List all available maps found in the game's pk3 files. Use this to discover what maps can be loaded with exec_command('map X') or launch_game.",
  {},
  async () => {
    const result = await queryGame({ cmd: "maplist" });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- snapshot_diff ------------------------------------------------------------

server.tool(
  "snapshot_diff",
  "Capture a snapshot of all entity state, then later diff against current state to see exactly what changed. Actions: 'capture' saves current state, 'diff' compares saved vs. current and shows all field changes, 'status' shows if a snapshot exists.",
  {
    action: z
      .enum(["capture", "diff", "status"])
      .default("status")
      .describe("'capture' to save state, 'diff' to compare, 'status' to check"),
  },
  async ({ action }) => {
    const result = await queryGame({ cmd: "snapshot", str2: action });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- memory_peek --------------------------------------------------------------

server.tool(
  "memory_peek",
  "Read raw bytes from a game entity's memory — including the game module's private data beyond sharedEntity_t. Returns hex dump, int32 array, and float interpretation. sharedEntity_t is 304 bytes; bytes 304+ are the game module's private gentity_t fields.",
  {
    entity_num: z.number().int().describe("Entity number"),
    offset: z.number().int().default(0).describe("Byte offset from entity start (0 = sharedEntity_t.s, 304+ = game private data)"),
  },
  async ({ entity_num, offset }) => {
    const result = await queryGame({ cmd: "peek", num: entity_num, value: offset });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- point_contents -----------------------------------------------------------

server.tool(
  "point_contents",
  "Query what content types exist at a specific world point — solid, water, lava, slime, trigger, playerclip, etc. Uses the engine's collision system.",
  {
    point: z.string().describe("Point as 'x,y,z' (e.g. '0,-1438,-400')"),
  },
  async ({ point }) => {
    const result = await queryGame({ cmd: "point_contents", str1: point });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- entity_strings -----------------------------------------------------------

server.tool(
  "entity_strings",
  "Scan an entity's full memory (including private game module data beyond sharedEntity_t) for string pointers. Finds classname, targetname, model paths, script names, etc. by dereferencing pointer-like int32 values and checking if they point to readable ASCII strings. Essential for identifying what an entity IS.",
  {
    entity_num: z.number().int().describe("Entity number to scan"),
  },
  async ({ entity_num }) => {
    const result = await queryGame({ cmd: "strings", num: entity_num });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- entity_funcscan ----------------------------------------------------------

server.tool(
  "entity_funcscan",
  "Scan an entity's private memory for function pointers — addresses within the game DLL's code range. Finds think, touch, use, die, reached, blocked, pain callbacks. The DLL code range is auto-detected from entity 0 (worldspawn). Cross-reference returned addresses with the decompiled function map to identify which callback is assigned.",
  {
    entity_num: z.number().int().describe("Entity number to scan"),
  },
  async ({ entity_num }) => {
    const result = await queryGame({ cmd: "funcscan", num: entity_num });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- bulk_field_scan ----------------------------------------------------------

server.tool(
  "bulk_field_scan",
  "Read the same byte offset across ALL entities and report distinct values with counts. Powerful for discovering what a field means — e.g., scanning offset 312 across all entities might reveal it's 'health' (values cluster around 100, 250, 0). String pointer values are auto-resolved. Function pointer values are flagged. Returns first 64 per-entity values for detailed analysis.",
  {
    offset: z.number().int().describe("Byte offset into the gentity_t struct to read"),
    width: z.number().int().default(4).describe("Field width: 1 (byte), 2 (short), or 4 (int32). Default: 4"),
    active_only: z.boolean().default(false).describe("Only scan linked/active entities (default: false — scan all)"),
  },
  async ({ offset, width, active_only }) => {
    const result = await queryGame({ cmd: "bulkscan", num: offset, value: width, active_only });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- client_peek --------------------------------------------------------------

server.tool(
  "client_peek",
  "Read raw bytes from a game client's memory (gclient_t). Similar to memory_peek but for client data. The first playerState_t-sized bytes are the player state; bytes after that are the game module's private client fields (clientPersistant_t, session data, NPC state, etc.). Returns hex, int32, float, and string interpretations.",
  {
    client_num: z.number().int().describe("Client number (0 to maxclients-1)"),
    offset: z.number().int().default(0).describe("Byte offset into gclient_t (0 = start of playerState_t)"),
  },
  async ({ client_num, offset }) => {
    const result = await queryGame({ cmd: "client_peek", num: client_num, value: offset });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- client_strings -----------------------------------------------------------

server.tool(
  "client_strings",
  "Scan a game client's full memory for string pointers. Finds netname, model path, userinfo strings, and other client-side data by dereferencing pointers in the gclient_t struct.",
  {
    client_num: z.number().int().describe("Client number (0 to maxclients-1)"),
  },
  async ({ client_num }) => {
    const result = await queryGame({ cmd: "client_strings", num: client_num });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- list_entities_annotated ---------------------------------------------------

server.tool(
  "list_entities_annotated",
  "List all entities with human-readable annotations — entity type names (ET_ITEM, ET_PLAYER, etc.), resolved model paths from configstrings, and loop sound names. Much more readable than raw list_entities.",
  {
    active_only: z
      .boolean()
      .default(true)
      .describe("If true, only return linked/active entities (default: true)"),
  },
  async ({ active_only }) => {
    const result = await queryGame({ cmd: "entities_annotated", active_only });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- inspect_entity_annotated -------------------------------------------------

server.tool(
  "inspect_entity_annotated",
  "Get full entity state with human-readable annotations — type name, model path resolved from configstrings, sound name. Includes all entityState_t and entityShared_t fields plus engine svEntity_t data.",
  {
    num: z.number().int().describe("Entity number"),
  },
  async ({ num }) => {
    const result = await queryGame({ cmd: "entity_annotated", num });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- teleport_entity ----------------------------------------------------------

server.tool(
  "teleport_entity",
  "Instantly move an entity to a new position. Sets origin, pos.trBase, currentOrigin, zeros velocity, sets trajectory to stationary, and re-links the entity. For player entities, also updates playerState origin and zeros velocity.",
  {
    num: z.number().int().describe("Entity number to teleport"),
    position: z
      .string()
      .describe("Target position as 'x,y,z' (e.g. '0,-1438,-300')"),
  },
  async ({ num, position }) => {
    const result = await queryGame({ cmd: "teleport", num, str1: position });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- batch_commands -----------------------------------------------------------

server.tool(
  "batch_commands",
  "Execute multiple debug commands in a single TCP round-trip. Pass an array of command objects. Returns an array of results in the same order. Much faster than calling tools one at a time when you need multiple pieces of data.",
  {
    commands: z
      .array(z.record(z.string(), z.unknown()))
      .describe(
        "Array of command objects, e.g. [{\"cmd\":\"status\"}, {\"cmd\":\"entity\",\"num\":0}, {\"cmd\":\"cvar\",\"name\":\"sv_fps\"}]"
      ),
  },
  async ({ commands }) => {
    // Send as JSON array on one line
    const batchStr = JSON.stringify(commands);
    const result = await new Promise((resolve, reject) => {
      const socket = new net.Socket();
      let buffer = "";
      let settled = false;

      const timer = setTimeout(() => {
        if (!settled) {
          settled = true;
          socket.destroy();
          reject(new Error("Timeout waiting for batch response"));
        }
      }, 10000); // longer timeout for batch

      socket.setTimeout(CONNECT_TIMEOUT);

      socket.on("connect", () => {
        socket.write(batchStr + "\n");
      });

      socket.on("data", (data) => {
        buffer += data.toString();
        const nlIdx = buffer.indexOf("\n");
        if (nlIdx !== -1) {
          const line = buffer.slice(0, nlIdx).trim();
          if (!settled) {
            settled = true;
            clearTimeout(timer);
            socket.destroy();
            try {
              resolve(JSON.parse(line));
            } catch {
              resolve({ raw: line });
            }
          }
        }
      });

      socket.on("timeout", () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          socket.destroy();
          reject(new Error("Connection timeout"));
        }
      });

      socket.on("error", (err) => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          reject(new Error(`Batch failed: ${err.message}`));
        }
      });

      socket.connect(GAME_PORT, GAME_HOST);
    });

    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// -- sp_trace -----------------------------------------------------------------

server.tool(
  "sp_trace",
  "Trace SP-specific syscalls and game imports. When enabled, captures every SP cgame syscall (ambient sound, R_DrawScreenShot, R_Scissor, force feedback, etc.) and game import call (cvar set, save/load, configstring, console commands) with arguments and return values. Actions: 'start' to begin, 'stop' to end, 'clear' to reset, or omit to read entries. Filter by name substring to find specific calls. Use since_id for polling.",
  {
    action: z
      .enum(["start", "stop", "clear", "read"])
      .default("read")
      .describe("'start' to begin tracing, 'stop' to end, 'clear' to reset, 'read' to dump"),
    since_id: z.number().int().default(0).describe("Only return entries after this seq ID (for polling)"),
    max_entries: z.number().int().min(1).max(512).default(200).describe("Max entries to return (default 200, max 512)"),
    filter: z.string().default("").describe("Optional name substring filter (e.g. 'Ambient', 'Scissor', 'Cvar')"),
  },
  async ({ action, since_id, max_entries, filter }) => {
    const op = action === "read" ? "" : action;
    const result = await queryGame({
      cmd: "sp_trace",
      num: since_id,
      value: max_entries,
      str1: filter,
      str2: op,
    });
    return { content: [{ type: "text", text: formatJson(result) }] };
  }
);

// ---------------------------------------------------------------------------
// Start
// ---------------------------------------------------------------------------

async function main() {
  const transport = new StdioServerTransport();
  await server.connect(transport);
}

main().catch((err) => {
  console.error("Fatal:", err);
  process.exit(1);
});
