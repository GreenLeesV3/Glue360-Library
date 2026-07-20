/* ============================================================
   GLUE360 — CORE TYPES
   Contracts between the UI and the recomp bridge. The C++ host
   (src/gui/webview_host.cpp) is authoritative for profiles,
   settings paths, and toolchain state. The UI persists only
   presentation state (library entries, accent).
   ============================================================ */

export type GameStatus =
  | "not_compiled" // profile known, no usable build yet
  | "queued"
  | "recompiling"
  | "recompiled" // build deployed — can launch
  | "running"
  | "failed";

export interface BuildRecord {
  id: string;
  date: number;
  success: boolean;
  durationSec: number;
  profileName: string;
  deployDir: string | null;
}

export interface Game {
  id: string; // profile id for built-in entries; generated id for imports
  source: "profile" | "imported";
  title: string;
  cover: string; // data-URI SVG (generated) — drop real box art via appData
  isoPath: string | null;
  profileId: string | null;
  status: GameStatus;
  lastPlayed: number | null;
  description: string;
  tags: string[];
  builds: BuildRecord[];
  deployDir: string | null; // folder containing the playable executable
  exePath: string | null;
  userDataDir: string | null; // portable runtime root: saves, profiles and cache
  shaderCacheDir: string | null; // <exe dir>\user_data\cache\shaders
  launchError: string | null;
  /** exe path missing on disk at boot (files deleted externally, moved, …) */
  filesMissing?: boolean;
  titleId: string; // Xbox 360 title ID, e.g. 415607E2
  addedAt: number;
}

/** Mirrors profiles/<id>/profile.toml — read by the C++ host (authoritative). */
export interface GameProfile {
  id: string; // directory name, e.g. "spiderman3"
  name: string; // [profile] name
  titleId: string; // [profile] title_id
  sdkVersion: string; // [profile] sdk_version
  /** [build] runtime_flags — provenance of the shipped custom runtime */
  runtimeFlags: string[];
  /** [build] custom_runtime_dll present → ships a custom rexruntime.dll */
  customRuntime: boolean;
  /** [build] requires_sdk_source or runtime_patches present → needs an SDK
   *  source tree for the custom runtime build (e.g. Captain America's XCTD
   *  compression patch). */
  requiresSdkSource: boolean;
  /** number of [cvars] entries forced by the profile */
  cvarCount: number;
}

export type JobStatus = "running" | "done" | "failed" | "cancelled";

export interface JobLog {
  t: number; // ms since job start
  level: "info" | "ok" | "warn" | "err";
  msg: string;
}

export interface RecompileJob {
  id: string;
  title: string;
  cover: string;
  isoPath: string;
  profileId: string;
  outputDir: string;
  status: JobStatus;
  progress: number; // 0..100
  phaseIndex: number; // index into STAGES
  startedAt: number;
  endedAt?: number;
  logs: JobLog[];
  error?: string;
  deployDir?: string; // set by the final "done" status event
}

/** Host-authoritative paths + defaults. UI-only prefs (accent) live beside them. */
export interface AppSettings {
  sdkPath: string; // prebuilt SDK root ("" = auto-detect via dep checker)
  sdkSourcePath: string; // optional — SDK source tree for custom runtime builds
  outputRoot: string; // parent dir for per-game outputs
  defaultProfileId: string | null;
  cleanBuild: boolean; // default for the wizard's "clean build" toggle
  accent: "green" | "crimson" | "frost" | "violet"; // UI-only
}

/** Real dependency-checker readout (recomp::deps::check_dependencies). */
export interface ToolchainInfo {
  ok: boolean;
  appVersion: string;
  sdkRoot: string;
  sdkVersion: string;
  clangVersion: string;
  cmakeVersion: string;
  ninjaVersion: string;
  msvcVersion: string;
  issues: string[];
}

export type View = "library" | "queue" | "system";

/* ------ virtual filesystem entry (dev-sim FileBrowser only) ------ */
export interface FSEntry {
  name: string;
  type: "dir" | "file";
  size?: number; // bytes
  modified?: string;
}
