/* ============================================================================
   RECOMP SERVICE — THE INTEGRATION LAYER
   ============================================================================
   Talks to the C++ host (src/gui/webview_host.cpp) over the WebView2
   postMessage bridge. When no bridge exists (plain `npm run dev` in a
   browser), a simulation keeps the UI demoable end-to-end.

   Bridge protocol (host is authoritative):
     UI → host:  { rpc: <seq>, cmd: <string>, args: <object> }
     host → UI:  { rpc: <seq>, ok: <bool>, data?, error? }         (response)
                 { event: "job", id, kind: "log"|"progress"|"status", ... }
                 { event: "game", running: <bool>, profileId }
   ============================================================================ */
import type {
  FSEntry,
  GameProfile,
  JobLog,
  JobStatus,
  RecompileJob,
  ToolchainInfo,
} from "../lib/types";
import { FALLBACK_PROFILES, RECOMP_PHASES, STAGES, phaseIndexForStage } from "../lib/appData";
import { uid } from "../lib/format";

export { RECOMP_PHASES, STAGES };

/* ------------------------------ event bus ------------------------------- */

export type JobEvent =
  | { type: "log"; id: string; line: JobLog }
  | { type: "progress"; id: string; progress: number; phaseIndex: number }
  | { type: "status"; id: string; status: JobStatus; error?: string; deployDir?: string };

/** host-pushed non-job events (running game exited, …) */
export type AppEvent = { type: "game_exited"; libraryId: string };

type JobListener = (e: JobEvent) => void;
type AppListener = (e: AppEvent) => void;
const jobListeners = new Set<JobListener>();
const appListeners = new Set<AppListener>();

export const jobEvents = {
  subscribe(cb: JobListener): () => void {
    jobListeners.add(cb);
    return () => jobListeners.delete(cb);
  },
  publish(e: JobEvent) {
    jobListeners.forEach((cb) => cb(e));
  },
};

export const appEvents = {
  subscribe(cb: AppListener): () => void {
    appListeners.add(cb);
    return () => appListeners.delete(cb);
  },
  publish(e: AppEvent) {
    appListeners.forEach((cb) => cb(e));
  },
};

/* --------------------------- WebView2 bridge ---------------------------- */

interface WebView2Bridge {
  postMessage(message: unknown): void;
  addEventListener(type: "message", listener: (e: { data: unknown }) => void): void;
}

declare global {
  interface Window {
    chrome?: { webview?: WebView2Bridge };
  }
}

const bridge: WebView2Bridge | null =
  typeof window !== "undefined" ? (window.chrome?.webview ?? null) : null;

/** true when running inside the native Glue360 host */
export const hasBridge = bridge !== null;

let rpcSeq = 1;
const pendingRpc = new Map<number, { resolve: (v: unknown) => void; reject: (e: Error) => void }>();

function call(cmd: string, args: Record<string, unknown> = {}): Promise<unknown> {
  if (!bridge) return Promise.reject(new Error(`no bridge for ${cmd}`));
  const rpc = rpcSeq++;
  const { promise, resolve, reject } = Promise.withResolvers<unknown>();
  pendingRpc.set(rpc, { resolve, reject });
  bridge.postMessage({ rpc, cmd, args });
  return promise;
}

/* ------- host message validation (type guards over unknown) ------- */

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === "object" && v !== null;
}

function str(v: unknown, def = ""): string {
  return typeof v === "string" ? v : def;
}

function num(v: unknown, def = 0): number {
  return typeof v === "number" && Number.isFinite(v) ? v : def;
}

const LOG_LEVELS: Record<string, JobLog["level"]> = {
  info: "info",
  ok: "ok",
  warn: "warn",
  err: "err",
};

const JOB_STATUSES: Record<string, JobStatus> = {
  running: "running",
  done: "done",
  failed: "failed",
  cancelled: "cancelled",
};

function handleHostMessage(data: unknown): void {
  if (!isRecord(data)) return;

  // RPC response
  if (typeof data.rpc === "number") {
    const waiter = pendingRpc.get(data.rpc);
    if (!waiter) return;
    pendingRpc.delete(data.rpc);
    if (data.ok === true) waiter.resolve(data.data);
    else waiter.reject(new Error(str(data.error, "host call failed")));
    return;
  }

  // Job event
  if (data.event === "job" && typeof data.id === "string") {
    const id = data.id;
    if (data.kind === "log") {
      jobEvents.publish({
        type: "log",
        id,
        line: {
          t: num(data.t),
          level: LOG_LEVELS[str(data.level)] ?? "info",
          msg: str(data.msg),
        },
      });
    } else if (data.kind === "progress") {
      jobEvents.publish({
        type: "progress",
        id,
        progress: num(data.progress),
        phaseIndex:
          typeof data.stageId === "string"
            ? phaseIndexForStage(data.stageId)
            : num(data.phaseIndex),
      });
    } else if (data.kind === "status") {
      const status = JOB_STATUSES[str(data.status)];
      if (status) {
        jobEvents.publish({
          type: "status",
          id,
          status,
          error: typeof data.error === "string" ? data.error : undefined,
          deployDir: typeof data.deployDir === "string" ? data.deployDir : undefined,
        });
      }
    }
    return;
  }

  // Game exited
  if (data.event === "game" && data.running === false) {
    appEvents.publish({ type: "game_exited", libraryId: str(data.libraryId) });
  }
}

if (bridge) bridge.addEventListener("message", (e) => handleHostMessage(e.data));

/* ------------------------------- public API ------------------------------- */

function parseProfile(v: unknown): GameProfile | null {
  if (!isRecord(v) || typeof v.id !== "string") return null;
  return {
    id: v.id,
    name: str(v.name, v.id),
    titleId: str(v.titleId),
    sdkVersion: str(v.sdkVersion),
    runtimeFlags: Array.isArray(v.runtimeFlags)
      ? v.runtimeFlags.filter((f): f is string => typeof f === "string")
      : [],
    customRuntime: v.customRuntime === true,
    cvarCount: num(v.cvarCount),
  };
}

export async function listProfiles(): Promise<GameProfile[]> {
  if (!bridge) return FALLBACK_PROFILES;
  const data = await call("list_profiles");
  if (!Array.isArray(data)) return [];
  return data.map(parseProfile).filter((p): p is GameProfile => p !== null);
}

export async function checkDeps(sdkPath = "", sdkSourcePath = ""): Promise<ToolchainInfo> {
  if (!bridge) {
    return {
      ok: true,
      appVersion: "dev",
      sdkRoot: "(simulated — no host)",
      sdkVersion: "0.8.0",
      clangVersion: "22.1.8",
      cmakeVersion: "4.2.1",
      ninjaVersion: "1.13.2",
      msvcVersion: "14.44",
      issues: [],
    };
  }
  const d = await call("check_deps", { sdkPath, sdkSourcePath });
  const r = isRecord(d) ? d : {};
  return {
    ok: r.ok === true,
    appVersion: str(r.appVersion),
    sdkRoot: str(r.sdkRoot),
    sdkVersion: str(r.sdkVersion),
    clangVersion: str(r.clangVersion),
    cmakeVersion: str(r.cmakeVersion),
    ninjaVersion: str(r.ninjaVersion),
    msvcVersion: str(r.msvcVersion),
    issues: Array.isArray(r.issues) ? r.issues.filter((i): i is string => typeof i === "string") : [],
  };
}

/** Host-authoritative path settings. Accent is merged in by the store. */
export interface HostSettings {
  sdkPath: string;
  sdkSourcePath: string;
  outputRoot: string;
  defaultProfileId: string | null;
  cleanBuild: boolean;
}

export async function getHostSettings(): Promise<HostSettings | null> {
  if (!bridge) return null;
  const d = await call("get_settings");
  const r = isRecord(d) ? d : {};
  return {
    sdkPath: str(r.sdkPath),
    sdkSourcePath: str(r.sdkSourcePath),
    outputRoot: str(r.outputRoot),
    defaultProfileId: typeof r.defaultProfileId === "string" && r.defaultProfileId ? r.defaultProfileId : null,
    cleanBuild: r.cleanBuild === true,
  };
}

export async function saveHostSettings(s: HostSettings): Promise<void> {
  if (!bridge) return;
  await call("save_settings", { ...s, defaultProfileId: s.defaultProfileId ?? "" });
}

/** Native executable picker for importing an existing compiled game. */
export async function pickExe(): Promise<string | null> {
  if (!bridge) return null;
  const d = await call("pick_exe");
  return typeof d === "string" && d ? d : null;
}

/** Native ISO file picker. Returns null when cancelled. */
export async function pickIso(): Promise<string | null> {
  if (!bridge) return null;
  const d = await call("pick_iso");
  return typeof d === "string" && d ? d : null;
}

/** Native folder picker. Returns null when cancelled. */
export async function pickDir(title: string): Promise<string | null> {
  if (!bridge) return null;
  const d = await call("pick_dir", { title });
  return typeof d === "string" && d ? d : null;
}

export async function openFolder(path: string): Promise<void> {
  if (!bridge) return;
  await call("open_folder", { path });
}

export interface StartRecompileInput {
  title: string;
  cover: string;
  isoPath: string;
  profileId: string;
  outputDir: string;
  clean: boolean;
  sdkPath: string;
  sdkSourcePath: string;
}

export async function startRecompile(input: StartRecompileInput): Promise<RecompileJob> {
  const base: RecompileJob = {
    id: uid("job"),
    title: input.title,
    cover: input.cover,
    isoPath: input.isoPath,
    profileId: input.profileId,
    outputDir: input.outputDir,
    status: "running",
    progress: 0,
    phaseIndex: 0,
    startedAt: Date.now(),
    logs: [],
  };

  if (!bridge) {
    simulateJob(base);
    return base;
  }

  const d = await call("start_recompile", {
    iso: input.isoPath,
    profile: input.profileId,
    output: input.outputDir,
    clean: input.clean,
    sdk: input.sdkPath,
    sdkSource: input.sdkSourcePath,
  });
  const r = isRecord(d) ? d : {};
  return { ...base, id: str(r.jobId, base.id) };
}

export function cancelRecompile(jobId: string): void {
  if (bridge) {
    void call("cancel_recompile", { jobId });
    return;
  }
  simCancel(jobId);
}

export async function launchGame(
  libraryId: string,
  exePath: string,
  workingDir: string,
): Promise<void> {
  if (bridge) {
    await call("launch_game", { libraryId, exePath, workingDir });
    return;
  }
  console.info(`[sim] launchGame(${libraryId}) @ ${exePath}`);
}

export async function stopGame(libraryId: string): Promise<void> {
  if (bridge) {
    await call("stop_game", { libraryId });
    return;
  }
  console.info(`[sim] stopGame(${libraryId})`);
}

/** True when a filesystem path exists (host-checked; false in dev sim). */
export async function pathExists(path: string): Promise<boolean> {
  if (bridge) {
    return (await call("path_exists", { path })) === true;
  }
  return true;
}

export interface DeleteFilesResult {
  path: string;
  ok: boolean;
  missing?: boolean;
  error?: string;
  preservedUserDataTo?: string;
}

/**
 * Delete game files via the host (JS cannot remove trees). The host refuses
 * anything that isn't a recognized Glue360 workspace/deploy dir and refuses
 * outright while the game is running. Saves/shader cache are preserved by
 * default (moved beside the deleted tree) unless preserveUserData is false.
 */
export async function deleteGameFiles(
  libraryId: string,
  paths: string[],
  preserveUserData = true,
): Promise<DeleteFilesResult[]> {
  if (bridge) {
    return (await call("delete_game_files", {
      libraryId,
      paths,
      preserveUserData,
    })) as DeleteFilesResult[];
  }
  console.info(`[sim] deleteGameFiles(${libraryId}):`, paths);
  return paths.map((path) => ({ path, ok: true }));
}

/* ============================================================================
   SIMULATION — dev-mode only (`npm run dev` in a browser, no C++ host).
   Log lines mirror the real pipeline's phrasing so the demo looks honest.
   ============================================================================ */

const SIM_LOGS: string[][] = [
  [
    "Reading XDVDFS descriptor…",
    "Extracting game files (built-in reader)…",
    "extracted: default.xex",
    "extracted: game assets",
  ],
  ["rexglue init --xex default.xex", "Project scaffold created", "Manifest written"],
  [
    "rexglue codegen manifest.toml",
    "Analyzing PPC functions…",
    "Lifting instructions to C++…",
    "Emitting recomp shards…",
    "Generated 92 shards.",
  ],
  ["Copying profile sources…", "Rendering cvar template…", "Applied 5 source files"],
  ["Using profile-provided custom_runtime_dll", "Runtime stage complete"],
  [
    "cmake -G Ninja (clang-cl)…",
    "Building CXX objects…",
    "[42/101] spiderman3_recomp.17.cpp",
    "[97/101] spiderman3_recomp.88.cpp",
    "Linking CXX executable…",
  ],
  ["Copying game exe…", "Rendering runtime TOML…", "Copying game data…", "Deploy complete"],
];

interface SimState {
  job: RecompileJob;
  timer: ReturnType<typeof setInterval> | null;
}
const simJobs = new Map<string, SimState>();

function simulateJob(job: RecompileJob): void {
  const entry: SimState = { job, timer: null };
  simJobs.set(job.id, entry);
  const pushLog = (level: JobLog["level"], msg: string) => {
    jobEvents.publish({ type: "log", id: job.id, line: { t: Date.now() - job.startedAt, level, msg } });
  };
  pushLog("ok", `pipeline session started — ${job.title}`);
  pushLog("info", `iso: ${job.isoPath}`);
  pushLog("info", `output: ${job.outputDir}`);

  entry.timer = setInterval(() => {
    job.progress = Math.min(100, job.progress + 1.2 + Math.random() * 2.4);
    job.phaseIndex = Math.min(STAGES.length - 1, Math.floor((job.progress / 100) * STAGES.length));
    jobEvents.publish({ type: "progress", id: job.id, progress: job.progress, phaseIndex: job.phaseIndex });
    if (Math.random() < 0.55) {
      const pool = SIM_LOGS[job.phaseIndex];
      pushLog(Math.random() < 0.25 ? "ok" : "info", pool[Math.floor(Math.random() * pool.length)]);
    }
    if (job.progress >= 100) {
      clearInterval(entry.timer ?? undefined);
      entry.timer = null;
      pushLog("ok", `Pipeline complete: ${STAGES.length} stage(s) succeeded`);
      jobEvents.publish({
        type: "status",
        id: job.id,
        status: "done",
        deployDir: `${job.outputDir}/standalone`,
      });
    }
  }, 220);
}

function simCancel(jobId: string): void {
  const entry = simJobs.get(jobId);
  if (!entry) return;
  clearInterval(entry.timer ?? undefined);
  entry.timer = null;
  jobEvents.publish({
    type: "log",
    id: jobId,
    line: { t: Date.now() - entry.job.startedAt, level: "warn", msg: "user: build cancelled" },
  });
  jobEvents.publish({ type: "status", id: jobId, status: "cancelled" });
}

/* ------ dev-sim virtual filesystem (FileBrowser in `npm run dev`) ------ */

const SIM_FS: Record<string, FSEntry[]> = {
  "C:/": [
    { name: "Games", type: "dir" },
    { name: "Users", type: "dir" },
  ],
  "C:/Games": [{ name: "ISOs", type: "dir" }],
  "C:/Games/ISOs": [
    { name: "Spider-Man 3 (USA, Europe).iso", type: "file", size: 7_838_315_520, modified: "2026-07-01" },
    { name: "Jurassic - The Hunted (USA).iso", type: "file", size: 7_838_315_520, modified: "2026-07-01" },
    { name: "Spider-Man - Web of Shadows (USA) (En,Fr).iso", type: "file", size: 7_838_315_520, modified: "2026-07-01" },
  ],
  "D:/": [{ name: "Games", type: "dir" }],
  "D:/Games": [{ name: "XBOX360", type: "dir" }],
  "D:/Games/XBOX360": [
    { name: "Spider-Man 3 (USA, Europe).iso", type: "file", size: 7_838_315_520, modified: "2026-07-01" },
  ],
};

export async function browseDirectory(path: string): Promise<FSEntry[]> {
  const { promise, resolve } = Promise.withResolvers<void>();
  setTimeout(resolve, 60);
  await promise;
  return SIM_FS[normalizePath(path)] ?? [];
}

export function normalizePath(p: string): string {
  let n = p.replace(/\\/g, "/").replace(/\/{2,}/g, "/");
  if (/^[A-Za-z]:$/.test(n)) n += "/";
  if (n.length > 3 && n.endsWith("/")) n = n.slice(0, -1);
  return n;
}

export function parentPath(p: string): string {
  const n = normalizePath(p);
  if (/^[A-Za-z]:\/?$/.test(n)) return n;
  const idx = n.lastIndexOf("/");
  const parent = idx <= 2 ? n.slice(0, 3) : n.slice(0, idx);
  return normalizePath(parent);
}
