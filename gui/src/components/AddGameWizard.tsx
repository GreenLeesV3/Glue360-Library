/* ============================================================
   RECOMPILE WIZARD — Game → Source ISO → Output → Recompile
   Steps map 1:1 onto the real pipeline inputs:
     game    = profile (profiles/<id>/profile.toml, host-listed)
     iso     = --iso        (native file picker via bridge)
     output  = --output     (+ clean / SDK source advanced opts)
     build   = orchestrator run with live stage progress
   ============================================================ */
import { useEffect, useState } from "react";
import { AnimatePresence, motion } from "framer-motion";
import {
  Gamepad2, Disc3, FolderCog, Cpu, X, ChevronLeft, ChevronRight,
  Check, CheckCircle2, AlertTriangle, RefreshCcw, Rocket, FolderOpen,
  Wrench, HardDrive,
} from "lucide-react";
import { useStore } from "../store/useStore";
import { metaFor, STAGES } from "../lib/appData";
import { hasBridge, pickDir, pickIso, openFolder } from "../services/recompService";
import { fmtDuration } from "../lib/format";
import { cn } from "../utils/cn";
import ConsoleStream from "./ConsoleStream";
import FileBrowser from "./FileBrowser";
import { PrimaryBtn, GhostBtn, ProgressBar, Toggle } from "./ui";

const STEPS = [
  { label: "Game", icon: Gamepad2 },
  { label: "Source ISO", icon: Disc3 },
  { label: "Output", icon: FolderCog },
  { label: "Recompile", icon: Cpu },
] as const;

export default function AddGameWizard() {
  const wizard = useStore((s) => s.wizard);
  const closeWizard = useStore((s) => s.closeWizard);

  return (
    <AnimatePresence>{wizard.open && <WizardShell onRequestClose={closeWizard} />}</AnimatePresence>
  );
}

/* ----------------------------------------------------------- */

function WizardShell({ onRequestClose }: { onRequestClose: () => void }) {
  const wizard = useStore((s) => s.wizard);
  const activeJob = useStore((s) => (wizard.activeJobId ? s.jobs[wizard.activeJobId] : undefined));
  const building = activeJob?.status === "running";

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape" && !building) onRequestClose();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [building, onRequestClose]);

  return (
    <motion.div
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      exit={{ opacity: 0 }}
      className="fixed inset-0 z-50 flex items-center justify-center p-6"
    >
      <div className="absolute inset-0 bg-black/75 backdrop-blur-sm" onClick={() => !building && onRequestClose()} />
      <motion.div
        initial={{ opacity: 0, y: 28, scale: 0.97 }}
        animate={{ opacity: 1, y: 0, scale: 1 }}
        exit={{ opacity: 0, y: 16, scale: 0.98 }}
        transition={{ type: "spring", stiffness: 240, damping: 26 }}
        className="panel relative flex h-[min(700px,94vh)] w-[min(980px,96vw)] flex-col overflow-hidden rounded-2xl shadow-[0_40px_120px_-20px_rgba(0,0,0,0.9)]"
      >
        {/* header */}
        <div className="flex shrink-0 items-center justify-between border-b border-white/8 px-6 py-4">
          <div className="flex items-center gap-3">
            <span className="flex h-8 w-8 items-center justify-center rounded-lg bg-[var(--accent)] text-black shadow-[0_0_16px_color-mix(in_srgb,var(--accent)_50%,transparent)]">
              <Cpu className="h-4.5 w-4.5" strokeWidth={2.5} />
            </span>
            <div>
              <h2 className="font-display text-[15px] font-bold tracking-[0.2em] text-white uppercase">
                Recompile Xbox 360 Game
              </h2>
              <p className="text-[10.5px] tracking-[0.14em] text-white/40 uppercase">
                RexGlue360 pipeline · ISO → native x64
              </p>
            </div>
          </div>

          {/* stepper */}
          <div className="flex items-center">
            {STEPS.map((s, i) => {
              const Icon = s.icon;
              const done = wizard.step > i;
              const on = wizard.step === i;
              return (
                <div key={s.label} className="flex items-center">
                  {i > 0 && <div className={cn("mx-2 h-px w-8", done || on ? "bg-[var(--accent)]/60" : "bg-white/12")} />}
                  <div className="flex items-center gap-2">
                    <span
                      className={cn(
                        "flex h-7 w-7 items-center justify-center rounded-full border text-[11px] font-bold transition-all",
                        done
                          ? "border-transparent bg-[var(--accent)] text-black"
                          : on
                            ? "border-[var(--accent)] text-[var(--accent)] shadow-[0_0_12px_color-mix(in_srgb,var(--accent)_35%,transparent)]"
                            : "border-white/15 text-white/35",
                      )}
                    >
                      {done ? <Check className="h-3.5 w-3.5" strokeWidth={3} /> : <Icon className="h-3.5 w-3.5" />}
                    </span>
                    <span
                      className={cn(
                        "font-display hidden text-[10.5px] font-bold tracking-[0.18em] uppercase lg:block",
                        on ? "text-white" : done ? "text-white/55" : "text-white/30",
                      )}
                    >
                      {s.label}
                    </span>
                  </div>
                </div>
              );
            })}
          </div>

          <GhostBtn className="!h-9 !px-2.5" onClick={() => !building && onRequestClose()} disabled={building}>
            <X className="h-4 w-4" />
          </GhostBtn>
        </div>

        {/* body */}
        <div className="min-h-0 flex-1 overflow-hidden px-6 py-5">
          <AnimatePresence mode="wait">
            <motion.div
              key={wizard.step}
              initial={{ opacity: 0, x: 26 }}
              animate={{ opacity: 1, x: 0 }}
              exit={{ opacity: 0, x: -26 }}
              transition={{ duration: 0.22, ease: [0.2, 0.8, 0.2, 1] }}
              className="h-full"
            >
              {wizard.step === 0 && <StepGame />}
              {wizard.step === 1 && <StepIso />}
              {wizard.step === 2 && <StepOutput />}
              {wizard.step === 3 && <StepBuild />}
            </motion.div>
          </AnimatePresence>
        </div>

        {wizard.step < 3 && <WizardFooter />}
      </motion.div>
    </motion.div>
  );
}

function WizardFooter() {
  const wizard = useStore((s) => s.wizard);
  const closeWizard = useStore((s) => s.closeWizard);
  const wizardGoTo = useStore((s) => s.wizardGoTo);
  const beginRecompile = useStore((s) => s.beginRecompile);
  const d = wizard.draft;

  const canNext =
    (wizard.step === 0 && !!d.profileId) ||
    (wizard.step === 1 && !!d.isoPath) ||
    (wizard.step === 2 && d.outputDir.trim().length > 0);

  return (
    <div className="flex shrink-0 items-center justify-between border-t border-white/8 px-6 py-4">
      <GhostBtn onClick={closeWizard}>Cancel</GhostBtn>
      <div className="flex items-center gap-3">
        {wizard.step > 0 && (
          <GhostBtn onClick={() => wizardGoTo((wizard.step - 1) as 0 | 1 | 2)}>
            <ChevronLeft className="h-4 w-4" /> Back
          </GhostBtn>
        )}
        {wizard.step < 2 && (
          <PrimaryBtn onClick={() => wizardGoTo((wizard.step + 1) as 1 | 2)} disabled={!canNext}>
            Continue <ChevronRight className="h-4 w-4" />
          </PrimaryBtn>
        )}
        {wizard.step === 2 && (
          <PrimaryBtn onClick={() => void beginRecompile()} disabled={!canNext}>
            <Cpu className="h-4 w-4" /> Begin Recompile
          </PrimaryBtn>
        )}
      </div>
    </div>
  );
}

/* ------------------------- STEP 0: GAME (profile) ------------------------- */

function StepGame() {
  const draft = useStore((s) => s.wizard.draft);
  const setDraft = useStore((s) => s.setDraft);
  const profiles = useStore((s) => s.profiles);

  return (
    <div className="flex h-full flex-col gap-4">
      <p className="text-[12px] text-white/45">
        Each game profile bundles everything its title needs — entrypoint hints, runtime patches,
        forced cvars, deploy config. Profiles live in{" "}
        <span className="font-mono2 text-white/60">profiles\&lt;id&gt;\profile.toml</span>.
      </p>
      <div className="grid min-h-0 flex-1 grid-cols-3 gap-4 overflow-y-auto pb-2">
        {profiles.map((p) => {
          const meta = metaFor(p.id, p.name);
          const on = draft.profileId === p.id;
          return (
            <button
              key={p.id}
              onClick={() => setDraft({ profileId: p.id })}
              className={cn(
                "group relative flex flex-col overflow-hidden rounded-2xl border text-left transition-all",
                on
                  ? "border-[var(--accent)]/70 shadow-[0_0_30px_color-mix(in_srgb,var(--accent)_25%,transparent)]"
                  : "border-white/10 opacity-75 hover:border-white/25 hover:opacity-100",
              )}
            >
              <div className="relative aspect-[2/3] w-full overflow-hidden">
                <img src={meta.cover} alt="" className="h-full w-full object-cover" />
                {on && (
                  <span className="absolute top-2.5 right-2.5 flex h-6 w-6 items-center justify-center rounded-full bg-[var(--accent)] text-black shadow-[0_0_14px_color-mix(in_srgb,var(--accent)_60%,transparent)]">
                    <Check className="h-3.5 w-3.5" strokeWidth={3.5} />
                  </span>
                )}
                <div className="absolute inset-x-0 bottom-0 bg-gradient-to-t from-black/90 to-transparent p-3 pt-8">
                  <span className="font-display block text-[13px] leading-tight font-bold tracking-[0.08em] text-white uppercase">
                    {meta.title}
                  </span>
                  <span className="font-mono2 mt-1 block text-[9.5px] text-white/45">
                    title {p.titleId} · SDK {p.sdkVersion}
                  </span>
                </div>
              </div>
              <div className="flex flex-wrap items-center gap-1.5 p-2.5">
                {p.customRuntime && (
                  <span className="chip !text-[9px] border-[color-mix(in_srgb,var(--accent)_35%,transparent)] bg-[color-mix(in_srgb,var(--accent)_10%,transparent)] text-[var(--accent)]">
                    <Wrench className="h-2.5 w-2.5" /> custom runtime
                  </span>
                )}
                <span className="chip !text-[9px]">{p.cvarCount} cvars</span>
                {p.runtimeFlags.length > 0 && (
                  <span className="chip !text-[9px]">{p.runtimeFlags.length} runtime flags</span>
                )}
              </div>
            </button>
          );
        })}
        {profiles.length === 0 && (
          <div className="col-span-3 flex items-center justify-center rounded-xl border border-dashed border-white/12 p-8 text-[12.5px] text-white/35">
            No profiles found — check the profiles\ directory next to the app.
          </div>
        )}
      </div>
    </div>
  );
}

/* -------------------------- STEP 1: ISO -------------------------- */

function StepIso() {
  const draft = useStore((s) => s.wizard.draft);
  const setDraft = useStore((s) => s.setDraft);
  const profile = useStore((s) => s.profiles.find((p) => p.id === s.wizard.draft.profileId));
  const [picking, setPicking] = useState(false);

  const browse = async () => {
    setPicking(true);
    try {
      const path = await pickIso();
      if (path) setDraft({ isoPath: path });
    } finally {
      setPicking(false);
    }
  };

  if (!hasBridge) {
    // dev-sim fallback: in-app browser over the virtual FS
    return (
      <div className="flex h-full flex-col">
        <p className="mb-3 text-[12px] text-white/45">
          Dev simulation — pick any file. In the native app this is a Windows file dialog.
        </p>
        <FileBrowser
          className="min-h-0 flex-1"
          initialPath="D:/Games/XBOX360"
          onSelect={(path) => setDraft({ isoPath: path })}
        />
      </div>
    );
  }

  return (
    <div className="mx-auto flex h-full max-w-[640px] flex-col items-center justify-center gap-6 text-center">
      <span className="flex h-20 w-20 items-center justify-center rounded-full border border-white/10 bg-white/4 text-[var(--accent)]">
        <Disc3 className="h-9 w-9" />
      </span>
      <div>
        <h3 className="font-display text-xl font-bold tracking-[0.12em] text-white uppercase">
          Select the game ISO
        </h3>
        <p className="mx-auto mt-2 max-w-md text-[12.5px] leading-relaxed text-white/45">
          Point at your own dump of{" "}
          <span className="text-white/75">{profile?.name ?? "the game"}</span>. The pipeline
          verifies the XDVDFS image and extracts <span className="font-mono2">default.xex</span>{" "}
          plus game assets. Nothing is downloaded — your disc, your build.
        </p>
      </div>

      {draft.isoPath ? (
        <div className="panel-deep flex w-full items-center gap-3 rounded-xl px-4 py-3.5">
          <Disc3 className="h-5 w-5 shrink-0 text-[var(--accent)]" />
          <span className="font-mono2 min-w-0 flex-1 truncate text-left text-[12px] text-white/80">
            {draft.isoPath}
          </span>
          <CheckCircle2 className="h-4.5 w-4.5 shrink-0 text-[var(--accent)]" />
        </div>
      ) : (
        <div className="w-full rounded-xl border border-dashed border-white/15 px-4 py-5 text-[12px] text-white/30">
          No ISO selected yet
        </div>
      )}

      <PrimaryBtn onClick={() => void browse()} disabled={picking}>
        <FolderOpen className="h-4 w-4" /> {draft.isoPath ? "Choose a different ISO…" : "Browse for ISO…"}
      </PrimaryBtn>
    </div>
  );
}

/* ------------------------ STEP 2: OUTPUT ------------------------ */

function StepOutput() {
  const draft = useStore((s) => s.wizard.draft);
  const setDraft = useStore((s) => s.setDraft);
  const setDraftOutputRoot = useStore((s) => s.setDraftOutputRoot);
  const settings = useStore((s) => s.settings);
  const profiles = useStore((s) => s.profiles);
  const [advanced, setAdvanced] = useState(false);

  const selectedProfile = profiles.find((p) => p.id === draft.profileId);
  const needsSdkSource = selectedProfile?.requiresSdkSource === true;

  const browseOutput = async () => {
    const dir = await pickDir("Select the folder that will contain your game folders");
    if (dir) setDraftOutputRoot(dir);
  };
  const browseSdkSource = async () => {
    const dir = await pickDir("Select RexGlue SDK source tree");
    if (dir) setDraft({ sdkSourcePath: dir });
  };

  return (
    <div className="mx-auto flex h-full max-w-[640px] flex-col justify-center gap-5">
      <div>
        <label className="mb-1.5 block text-[11px] font-bold tracking-[0.22em] text-white/45 uppercase">
          Games Root Folder
        </label>
        <div className="flex items-center gap-2">
          <input
            value={draft.outputRoot}
            onChange={(e) => setDraftOutputRoot(e.target.value)}
            placeholder="e.g. C:\Games"
            className="field font-mono2 !text-[12.5px]"
            spellCheck={false}
          />
          {hasBridge && (
            <GhostBtn className="!h-11 shrink-0 !px-4" onClick={() => void browseOutput()}>
              <FolderOpen className="h-4 w-4" /> Browse
            </GhostBtn>
          )}
        </div>
        <div className="panel-deep mt-3 flex items-center gap-2.5 rounded-[10px] px-3 py-2.5">
          <HardDrive className="h-4 w-4 shrink-0 text-[var(--accent)]" />
          <div className="min-w-0">
            <div className="text-[9.5px] font-bold tracking-[0.16em] text-white/35 uppercase">
              This game will use
            </div>
            <div className="font-mono2 truncate text-[11.5px] text-white/70">
              {draft.outputDir || "<games root>\\<game name>"}
            </div>
          </div>
        </div>
        <p className="mt-2 text-[11.5px] leading-relaxed text-white/35">
          Every title gets its own named folder. This game&apos;s pipeline cache will be in{" "}
          <span className="font-mono2 text-white/55">{`${draft.outputDir || "<games root>\\<game name>"}\\.recomp\\`}</span>{" "}
          and the playable package in <span className="font-mono2 text-white/55">standalone\</span>.
        </p>
      </div>

      <div className="panel rounded-xl">
        <button
          onClick={() => setAdvanced(!advanced)}
          className="flex w-full items-center justify-between px-4 py-3 text-left"
        >
          <span className="flex items-center gap-2 text-[11px] font-bold tracking-[0.2em] text-white/55 uppercase">
            <Wrench className="h-3.5 w-3.5" /> Advanced
          </span>
          <ChevronRight className={cn("h-4 w-4 text-white/35 transition-transform", advanced && "rotate-90")} />
        </button>
        {advanced && (
          <div className="flex flex-col gap-4 border-t border-white/8 px-4 py-4">
            <Toggle
              label="Clean build — wipe previous stage outputs and start fresh"
              checked={draft.clean}
              onChange={(v) => setDraft({ clean: v })}
            />
            <div>
              <label className="mb-1.5 block text-[10px] font-bold tracking-[0.18em] text-white/40 uppercase">
                Alternate SDK Source Tree (this recompile only)
              </label>
              {needsSdkSource && (
                <div className="mb-2 flex items-center gap-2 rounded-lg border border-amber-400/25 bg-amber-400/8 px-3 py-2 text-[11px] text-amber-200">
                  <AlertTriangle className="h-3.5 w-3.5 shrink-0" />
                  {selectedProfile?.name} needs a custom runtime build (runtime patches) — point this at
                  the SDK source tree, or the game will not work correctly.
                </div>
              )}
              <div className="flex items-center gap-2">
                <input
                  value={draft.sdkSourcePath}
                  onChange={(e) => setDraft({ sdkSourcePath: e.target.value })}
                  placeholder={settings.sdkSourcePath || "Bundled SDK (default) — pick a source tree only for runtime patches"}
                  className="field font-mono2 !py-2 !text-[11.5px]"
                  spellCheck={false}
                />
                {hasBridge && (
                  <GhostBtn className="!h-9 shrink-0 !px-3" onClick={() => void browseSdkSource()}>
                    <FolderOpen className="h-3.5 w-3.5" />
                  </GhostBtn>
                )}
              </div>
              <p className="mt-1.5 text-[10.5px] leading-relaxed text-white/30">
                Leave empty to use the app's bundled SDK — the normal, supported path. Set a source
                tree only when a profile needs a custom runtime build (e.g. Captain America's XCTD
                patch). Your global default lives in Settings.
              </p>
            </div>
          </div>
        )}
      </div>

      <div className="flex items-center gap-2.5 rounded-xl border border-white/8 bg-white/3 px-4 py-3">
        <HardDrive className="h-4 w-4 shrink-0 text-[var(--accent)]" />
        <p className="text-[11.5px] leading-relaxed text-white/45">
          Expect roughly <span className="text-white/70">15–25 GB</span> free space per title
          (extracted assets + generated C++ + native build) and a first build of several minutes.
        </p>
      </div>
    </div>
  );
}

/* ------------------------- STEP 3: BUILD ------------------------- */

function StepBuild() {
  const wizard = useStore((s) => s.wizard);
  const job = useStore((s) => (wizard.activeJobId ? s.jobs[wizard.activeJobId] : undefined));
  const cancelJob = useStore((s) => s.cancelJob);
  const closeWizard = useStore((s) => s.closeWizard);
  const beginRecompile = useStore((s) => s.beginRecompile);
  const selectGame = useStore((s) => s.selectGame);
  const setView = useStore((s) => s.setView);
  const [, tick] = useState(0);

  useEffect(() => {
    if (job?.status !== "running") return;
    const t = setInterval(() => tick((n) => n + 1), 250);
    return () => clearInterval(t);
  }, [job?.status]);

  const done = job?.status === "done";
  const stopped = job?.status === "cancelled" || job?.status === "failed";
  const elapsed = job ? ((job.endedAt ?? Date.now()) - job.startedAt) / 1000 : 0;

  return (
    <div className="flex h-full flex-col gap-4">
      {/* summary strip */}
      <div className="flex flex-wrap items-center gap-2">
        <span className="chip !border-white/15 !text-white/70">
          <Gamepad2 className="h-3 w-3" /> {wizard.draft.title || "—"}
        </span>
        <span className="chip !border-white/15 !text-white/70">
          <Disc3 className="h-3 w-3" /> {wizard.draft.isoPath?.split(/[\\/]/).pop()}
        </span>
        <span className="chip !border-white/15 !text-white/70">
          <FolderCog className="h-3 w-3" /> {wizard.draft.outputDir}
        </span>
      </div>

      {done ? (
        /* ---------- success ---------- */
        <motion.div
          initial={{ opacity: 0, scale: 0.96 }}
          animate={{ opacity: 1, scale: 1 }}
          className="flex min-h-0 flex-1 flex-col items-center justify-center gap-4 text-center"
        >
          <motion.span
            initial={{ scale: 0.4 }}
            animate={{ scale: 1 }}
            transition={{ type: "spring", stiffness: 260, damping: 14 }}
            className="flex h-20 w-20 items-center justify-center rounded-full bg-[var(--accent)] text-black shadow-[0_0_50px_color-mix(in_srgb,var(--accent)_60%,transparent)]"
          >
            <CheckCircle2 className="h-10 w-10" strokeWidth={2.2} />
          </motion.span>
          <h3 className="font-display text-2xl font-bold tracking-[0.14em] text-white uppercase">
            Build Complete
          </h3>
          <p className="max-w-sm text-[13px] text-white/50">
            <span className="text-[var(--accent)]">{wizard.draft.title}</span> recompiled to native
            x64 in <span className="font-mono2">{fmtDuration(elapsed)}</span>. The standalone
            folder is ready to launch.
          </p>
          <div className="mt-2 flex items-center gap-3">
            {job?.deployDir && (
              <GhostBtn onClick={() => void openFolder(job.deployDir ?? "")}>
                <FolderOpen className="h-4 w-4" /> Open Folder
              </GhostBtn>
            )}
            <PrimaryBtn
              onClick={() => {
                if (wizard.draft.profileId) selectGame(wizard.draft.profileId);
                closeWizard();
                setView("library");
              }}
            >
              <Rocket className="h-4 w-4" /> Open in Library
            </PrimaryBtn>
          </div>
          <div className="w-full max-w-lg pt-2">
            <ConsoleStream logs={job?.logs.slice(-40) ?? []} className="h-36" />
          </div>
        </motion.div>
      ) : stopped ? (
        /* ---------- cancelled / failed ---------- */
        <div className="flex min-h-0 flex-1 flex-col items-center justify-center gap-4 text-center">
          <span className="flex h-20 w-20 items-center justify-center rounded-full bg-red-500/15 text-red-400 ring-1 ring-red-500/40">
            <AlertTriangle className="h-9 w-9" />
          </span>
          <h3 className="font-display text-2xl font-bold tracking-[0.14em] text-white uppercase">
            Build {job?.status === "failed" ? "Failed" : "Cancelled"}
          </h3>
          <p className="max-w-md text-[13px] text-white/50">
            The pipeline stopped at{" "}
            <span className="text-white/80">{STAGES[job?.phaseIndex ?? 0].label}</span>.
            {job?.error && <span className="mt-1 block text-red-300/80">{job.error}</span>}
            Full logs are in <span className="font-mono2 text-white/60">.recomp\logs\recomp.log</span>{" "}
            inside the output folder.
          </p>
          <div className="mt-2 flex items-center gap-3">
            <GhostBtn onClick={closeWizard}>Close</GhostBtn>
            <PrimaryBtn onClick={() => void beginRecompile()}>
              <RefreshCcw className="h-4 w-4" /> Retry Build
            </PrimaryBtn>
          </div>
          <div className="w-full max-w-lg pt-2">
            <ConsoleStream logs={job?.logs.slice(-40) ?? []} className="h-36" />
          </div>
        </div>
      ) : (
        /* ---------- running ---------- */
        <div className="grid min-h-0 flex-1 grid-cols-[300px_1fr] gap-4">
          <div className="flex min-h-0 flex-col gap-4">
            {/* progress */}
            <div className="panel rounded-xl p-5">
              <div className="flex items-end justify-between">
                <span className="font-display text-[44px] leading-none font-bold text-[var(--accent)] tabular-nums">
                  {Math.floor(job?.progress ?? 0)}
                  <span className="text-xl text-white/40">%</span>
                </span>
                <span className="font-mono2 text-[11px] text-white/40">
                  {fmtDuration(elapsed)}
                </span>
              </div>
              <ProgressBar value={job?.progress ?? 0} className="mt-3" />
              <p className="font-display mt-2.5 text-[11px] font-bold tracking-[0.16em] text-white/60 uppercase">
                {STAGES[job?.phaseIndex ?? 0].label}…
              </p>
            </div>
            {/* stage list */}
            <div className="panel min-h-0 flex-1 overflow-y-auto rounded-xl p-3">
              {STAGES.map((p, i) => {
                const cur = job?.phaseIndex ?? 0;
                return (
                  <div key={p.id} className="flex items-center gap-2.5 rounded-lg px-2.5 py-[7px]">
                    {i < cur ? (
                      <Check className="h-3.5 w-3.5 shrink-0 text-[var(--accent)]" strokeWidth={3} />
                    ) : i === cur ? (
                      <span className="anim-blink flex h-3.5 w-3.5 shrink-0 items-center justify-center">
                        <span className="h-2 w-2 rounded-full bg-[var(--accent)] shadow-[0_0_8px_var(--accent)]" />
                      </span>
                    ) : (
                      <span className="h-3.5 w-3.5 shrink-0 rounded-full border border-white/15" />
                    )}
                    <span
                      className={cn(
                        "font-display text-[11px] font-bold tracking-[0.12em] uppercase",
                        i < cur ? "text-white/40" : i === cur ? "text-[var(--accent)]" : "text-white/25",
                      )}
                    >
                      {p.label}
                    </span>
                  </div>
                );
              })}
            </div>
            <GhostBtn
              onClick={() => job && cancelJob(job.id)}
              className="shrink-0 !text-red-300 hover:!bg-red-500/10"
              title="Stops after the current stage finishes"
            >
              Abort Build
            </GhostBtn>
          </div>

          {/* console */}
          <ConsoleStream logs={job?.logs ?? []} className="min-h-0" />
        </div>
      )}
    </div>
  );
}
