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
const BUILD_DIR = path.join(IOEF_ROOT, "build", "release-mingw64-x86_64");
const DEDICATED_EXE = path.join(BUILD_DIR, "ioq3ded.x86_64.exe");
const CLIENT_EXE = path.join(BUILD_DIR, "ioquake3.x86_64.exe");

// ---------------------------------------------------------------------------
// Game process management
// ---------------------------------------------------------------------------

function isGameRunning(): boolean {
  try {
    const out = execSync("tasklist /FI \"IMAGENAME eq ioq3ded.x86_64.exe\" /NH", {
      encoding: "utf8",
      stdio: ["pipe", "pipe", "pipe"],
    });
    if (out.includes("ioq3ded.x86_64.exe")) return true;
  } catch {}
  try {
    const out = execSync("tasklist /FI \"IMAGENAME eq ioquake3.x86_64.exe\" /NH", {
      encoding: "utf8",
      stdio: ["pipe", "pipe", "pipe"],
    });
    if (out.includes("ioquake3.x86_64.exe")) return true;
  } catch {}
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
  version: "1.1.0",
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
    for (const name of ["ioq3ded.x86_64.exe", "ioquake3.x86_64.exe"]) {
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
