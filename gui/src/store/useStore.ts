/* ============================================================
   GLUE360 — APP STORE (zustand)
   The C++ host is authoritative for profiles, path settings and
   toolchain state (synced by boot()). The store persists library
   presentation state (games, accent) to localStorage.
   ============================================================ */
import { create } from "zustand";
import { persist } from "zustand/middleware";
import type {
  AppSettings,
  Game,
  GameProfile,
  RecompileJob,
  ToolchainInfo,
  View,
} from "../lib/types";
import { gameFromProfile, metaFor } from "../lib/appData";
import { joinWindowsPath, parentWindowsPath, safeWindowsFolderName, uid } from "../lib/format";
import {
  appEvents,
  cancelRecompile,
  checkDeps,
  getHostSettings,
  hasBridge,
  jobEvents,
  launchGame,
  listProfiles,
  saveHostSettings,
  startRecompile,
  stopGame,
  type JobEvent,
} from "../services/recompService";

export interface WizardDraft {
  profileId: string | null;
  title: string;
  cover: string;
  isoPath: string | null;
  outputRoot: string;
  outputDir: string;
  clean: boolean;
}

interface WizardState {
  open: boolean;
  step: 0 | 1 | 2 | 3;
  draft: WizardDraft;
  activeJobId: string | null;
}

export interface ImportCompiledGameInput {
  title: string;
  gameDir: string;
  exePath: string;
}

interface State {
  view: View;
  selectedGameId: string;
  games: Game[];
  profiles: GameProfile[];
  jobs: Record<string, RecompileJob>;
  jobOrder: string[];
  settings: AppSettings;
  toolchain: ToolchainInfo | null;
  booted: boolean;

  boot: () => Promise<void>;
  refreshToolchain: () => Promise<void>;

  setView: (v: View) => void;
  selectGame: (id: string) => void;

  openWizard: (prefillProfileId?: string) => void;
  closeWizard: () => void;
  wizardGoTo: (step: WizardState["step"]) => void;
  setDraft: (patch: Partial<WizardDraft>) => void;
  setDraftOutputRoot: (root: string) => void;
  beginRecompile: () => Promise<void>;

  cancelJob: (jobId: string) => void;
  clearFinishedJobs: () => void;

  launchGameById: (id: string) => Promise<void>;
  stopGameById: (id: string) => void;
  importCompiledGame: (input: ImportCompiledGameInput) => void;

  updateSettings: (patch: Partial<AppSettings>) => void;

  wizard: WizardState;

  /** internal: service events → state */
  _applyJobEvent: (e: JobEvent) => void;
}

const DEFAULT_SETTINGS: AppSettings = {
  sdkPath: "",
  sdkSourcePath: "",
  outputRoot: "",
  defaultProfileId: null,
  cleanBuild: false,
  accent: "green",
};

const INITIAL_DRAFT: WizardDraft = {
  profileId: null,
  title: "",
  cover: "",
  isoPath: null,
  outputRoot: "",
  outputDir: "",
  clean: false,
};

function defaultOutputDir(
  outputRoot: string,
  profileId: string,
  profiles: GameProfile[],
): string {
  const profile = profiles.find((p) => p.id === profileId);
  const meta = metaFor(profileId, profile?.name);
  const duplicateTitle = profiles.some(
    (p) => p.id !== profileId && metaFor(p.id, p.name).title.toLowerCase() === meta.title.toLowerCase(),
  );
  const folderName = safeWindowsFolderName(
    duplicateTitle ? `${meta.title} (${profileId})` : meta.title,
  );
  return outputRoot ? joinWindowsPath(outputRoot, folderName) : "";
}

export const useStore = create<State>()(
  persist(
    (set, get) => ({
      view: "library",
      selectedGameId: "spiderman3",
      games: [],
      profiles: [],
      jobs: {},
      jobOrder: [],
      settings: DEFAULT_SETTINGS,
      toolchain: null,
      booted: false,
      wizard: { open: false, step: 0, draft: INITIAL_DRAFT, activeJobId: null },

      boot: async () => {
        // Profiles are host-authoritative; fall back to seeds in dev sim.
        const profiles = await listProfiles();
        set((s) => {
          // Profile entries remain authoritative, while user-imported compiled
          // games are profile-independent and must survive restarts.
          const byId = new Map(s.games.map((g) => [g.id, g]));
          const profileGames = profiles.map((p) => {
            const existing = byId.get(p.id);
            if (!existing) return gameFromProfile(p);
            const meta = metaFor(p.id, p.name);
            const deployDir = existing.deployDir;
            return {
              ...existing,
              source: "profile" as const,
              profileId: p.id,
              title: meta.title,
              cover: meta.cover,
              description: meta.description,
              tags: meta.tags,
              titleId: p.titleId,
              exePath: existing.exePath ?? (deployDir ? joinWindowsPath(deployDir, `${p.id}.exe`) : null),
              userDataDir: existing.userDataDir ?? (deployDir ? joinWindowsPath(deployDir, "user_data") : null),
              shaderCacheDir:
                existing.shaderCacheDir ??
                (deployDir ? joinWindowsPath(deployDir, "user_data\\cache\\shaders") : null),
              launchError: null,
              // stale transient statuses from a previous session
              status:
                existing.status === "recompiling" || existing.status === "running" || existing.status === "queued"
                  ? deployDir
                    ? ("recompiled" as const)
                    : ("not_compiled" as const)
                  : existing.status,
            };
          });
          const importedGames = s.games
            .filter((g) => g.source === "imported")
            .map((g) => ({
              ...g,
              status: g.status === "running" ? ("recompiled" as const) : g.status,
              launchError: null,
            }));
          const games = [...profileGames, ...importedGames];
          const selectedGameId = games.some((g) => g.id === s.selectedGameId)
            ? s.selectedGameId
            : (games[0]?.id ?? "");
          return { profiles, games, selectedGameId, booted: true };
        });

        if (hasBridge) {
          const host = await getHostSettings();
          if (host) {
            set((s) => ({ settings: { ...s.settings, ...host } }));
          }
        }
        await get().refreshToolchain();
      },

      refreshToolchain: async () => {
        try {
          const s = get().settings;
          const toolchain = await checkDeps(s.sdkPath, s.sdkSourcePath);
          set({ toolchain });
        } catch {
          set({ toolchain: null });
        }
      },

      setView: (view) => set({ view }),
      selectGame: (id) => set({ selectedGameId: id, view: "library" }),

      openWizard: (prefillProfileId) =>
        set((s) => {
          const profileId = prefillProfileId ?? s.settings.defaultProfileId ?? null;
          const profile = s.profiles.find((p) => p.id === profileId);
          const game = s.games.find((g) => g.id === profileId);
          const meta = profile ? metaFor(profile.id, profile.name) : null;
          return {
            wizard: {
              open: true,
              step: 0,
              draft: {
                ...INITIAL_DRAFT,
                profileId: profile?.id ?? null,
                title: meta?.title ?? "",
                cover: meta?.cover ?? "",
                isoPath: game?.isoPath ?? null,
                outputRoot: s.settings.outputRoot,
                outputDir: profile
                  ? defaultOutputDir(s.settings.outputRoot, profile.id, s.profiles)
                  : "",
                clean: s.settings.cleanBuild,
              },
              activeJobId: null,
            },
          };
        }),
      closeWizard: () =>
        set((s) => ({ wizard: { ...s.wizard, open: false, activeJobId: null } })),
      wizardGoTo: (step) => set((s) => ({ wizard: { ...s.wizard, step } })),
      setDraft: (patch) =>
        set((s) => {
          const draft = { ...s.wizard.draft, ...patch };
          // picking a profile refreshes title/cover/output defaults
          if (patch.profileId && patch.profileId !== s.wizard.draft.profileId) {
            const profile = s.profiles.find((p) => p.id === patch.profileId);
            const meta = profile ? metaFor(profile.id, profile.name) : null;
            const game = s.games.find((g) => g.id === patch.profileId);
            draft.title = meta?.title ?? draft.title;
            draft.cover = meta?.cover ?? draft.cover;
            draft.isoPath = game?.isoPath ?? null;
            draft.outputDir = defaultOutputDir(draft.outputRoot, patch.profileId, s.profiles);
          }
          return { wizard: { ...s.wizard, draft } };
        }),
      setDraftOutputRoot: (root) =>
        set((s) => {
          const profileId = s.wizard.draft.profileId;
          const outputDir = profileId ? defaultOutputDir(root, profileId, s.profiles) : "";
          return {
            wizard: {
              ...s.wizard,
              draft: { ...s.wizard.draft, outputRoot: root, outputDir },
            },
          };
        }),

      beginRecompile: async () => {
        const { wizard, settings } = get();
        const { draft } = wizard;
        if (!draft.isoPath || !draft.profileId || !draft.outputDir) return;

        const job = await startRecompile({
          title: draft.title,
          cover: draft.cover,
          isoPath: draft.isoPath,
          profileId: draft.profileId,
          outputDir: draft.outputDir,
          clean: draft.clean,
          sdkPath: settings.sdkPath,
          sdkSourcePath: settings.sdkSourcePath,
        });
        set((s) => ({
          jobs: { ...s.jobs, [job.id]: job },
          jobOrder: [job.id, ...s.jobOrder],
          wizard: { ...s.wizard, step: 3, activeJobId: job.id },
          games: s.games.map((g) =>
            g.id === draft.profileId
              ? { ...g, status: "recompiling", isoPath: draft.isoPath }
              : g,
          ),
        }));
      },

      cancelJob: (jobId) => cancelRecompile(jobId),

      clearFinishedJobs: () =>
        set((s) => {
          const jobs = { ...s.jobs };
          s.jobOrder.forEach((id) => {
            if (jobs[id] && jobs[id].status !== "running") delete jobs[id];
          });
          return { jobs, jobOrder: s.jobOrder.filter((id) => jobs[id]) };
        }),

      importCompiledGame: (input) =>
        set((s) => {
          const id = uid("imported");
          const meta = metaFor(id, input.title);
          const exeDir = parentWindowsPath(input.exePath) || input.gameDir;
          const userDataDir = joinWindowsPath(exeDir, "user_data");
          const game: Game = {
            id,
            source: "imported",
            title: input.title.trim(),
            cover: meta.cover,
            isoPath: null,
            profileId: null,
            status: "recompiled",
            lastPlayed: null,
            description: "Existing native Xbox 360 recompilation imported from disk.",
            tags: ["Imported build"],
            builds: [],
            deployDir: input.gameDir,
            exePath: input.exePath,
            userDataDir,
            shaderCacheDir: joinWindowsPath(userDataDir, "cache\\shaders"),
            launchError: null,
            titleId: "",
            addedAt: Date.now(),
          };
          return {
            games: [...s.games, game],
            selectedGameId: id,
            view: "library" as const,
          };
        }),
      launchGameById: async (id) => {
        const game = get().games.find((g) => g.id === id);
        if (!game || game.status === "running" || !game.exePath || !game.deployDir) return;
        try {
          await launchGame(game.id, game.exePath, game.deployDir);
          set((s) => ({
            games: s.games.map((g) =>
              g.id === id
                ? { ...g, status: "running", lastPlayed: Date.now(), launchError: null }
                : g.status === "running"
                  ? { ...g, status: "recompiled" }
                  : g,
            ),
          }));
        } catch (error) {
          const message = error instanceof Error ? error.message : "Could not launch the game.";
          set((s) => ({
            games: s.games.map((g) => (g.id === id ? { ...g, launchError: message } : g)),
          }));
        }
      },

      stopGameById: (id) => {
        const game = get().games.find((g) => g.id === id);
        if (!game) return;
        stopGame(game.id);
        set((s) => ({
          games: s.games.map((g) => (g.id === id ? { ...g, status: "recompiled" } : g)),
        }));
      },

      updateSettings: (patch) => {
        set((s) => ({ settings: { ...s.settings, ...patch } }));
        // Path/default changes are host state — persist them there too.
        if (
          hasBridge &&
          ("sdkPath" in patch ||
            "sdkSourcePath" in patch ||
            "outputRoot" in patch ||
            "defaultProfileId" in patch ||
            "cleanBuild" in patch)
        ) {
          const s = get().settings;
          void saveHostSettings({
            sdkPath: s.sdkPath,
            sdkSourcePath: s.sdkSourcePath,
            outputRoot: s.outputRoot,
            defaultProfileId: s.defaultProfileId,
            cleanBuild: s.cleanBuild,
          });
        }
      },

      _applyJobEvent: (e) =>
        set((s) => {
          const job = s.jobs[e.id];
          if (!job) return s;
          let nextJob = { ...job };
          if (e.type === "log") {
            // Real builds stream thousands of lines — cap the retained buffer
            // so per-event array copies stay cheap. Full logs live on disk at
            // <output>\.recomp\logs\recomp.log.
            const logs = [...job.logs, e.line];
            nextJob.logs = logs.length > 500 ? logs.slice(-500) : logs;
          }
          if (e.type === "progress")
            nextJob = { ...nextJob, progress: e.progress, phaseIndex: e.phaseIndex };
          if (e.type === "status") {
            nextJob = {
              ...nextJob,
              status: e.status,
              endedAt: Date.now(),
              error: e.error,
              deployDir: e.deployDir ?? nextJob.deployDir,
              progress: e.status === "done" ? 100 : nextJob.progress,
            };
            if (e.status === "done") return commitBuild(s, e.id, nextJob);
            if (e.status === "cancelled" || e.status === "failed") {
              const games = s.games.map((g) =>
                g.id === nextJob.profileId && g.status === "recompiling"
                  ? { ...g, status: "failed" as const }
                  : g,
              );
              return { ...s, games, jobs: { ...s.jobs, [e.id]: nextJob } };
            }
          }
          return { ...s, jobs: { ...s.jobs, [e.id]: nextJob } };
        }),
    }),
    {
      name: "glue360-deck",
      version: 2,
      partialize: (s) => ({
        games: s.games,
        settings: s.settings,
        selectedGameId: s.selectedGameId,
      }),
    },
  ),
);

/* ---------------- build completion → library mutation ------------------- */

function commitBuild(
  s: Pick<State, "games" | "jobs" | "jobOrder">,
  jobId: string,
  job: RecompileJob,
): Partial<State> {
  const durationSec = ((job.endedAt ?? Date.now()) - job.startedAt) / 1000;
  const games = s.games.map((g) =>
    g.id === job.profileId
      ? {
          ...g,
          status: "recompiled" as const,
          isoPath: job.isoPath,
          deployDir: job.deployDir ?? g.deployDir,
          exePath: job.deployDir ? joinWindowsPath(job.deployDir, `${job.profileId}.exe`) : g.exePath,
          userDataDir: job.deployDir ? joinWindowsPath(job.deployDir, "user_data") : g.userDataDir,
          shaderCacheDir: job.deployDir
            ? joinWindowsPath(job.deployDir, "user_data\\cache\\shaders")
            : g.shaderCacheDir,
          builds: [
            {
              id: uid("build"),
              date: Date.now(),
              success: true,
              durationSec,
              profileName: job.profileId,
              deployDir: job.deployDir ?? null,
            },
            ...g.builds,
          ],
        }
      : g,
  );
  return { games, jobs: { ...s.jobs, [jobId]: job } };
}

/* ------------- single subscription: service events → store -------------- */
jobEvents.subscribe((e) => useStore.getState()._applyJobEvent(e));
appEvents.subscribe((e) => {
  if (e.type === "game_exited") {
    useStore.setState((s) => ({
      games: s.games.map((g) =>
        g.id === e.libraryId && g.status === "running"
          ? { ...g, status: "recompiled" }
          : g,
      ),
    }));
  }
});
