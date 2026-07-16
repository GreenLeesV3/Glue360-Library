/* ---------- game hero + build config + history (detail page) ---------- */
import { AnimatePresence, motion } from "framer-motion";
import {
  Play, Square, Cpu, Disc3, CheckCircle2, XCircle, FolderOpen, Clock3,
  Activity, Layers, Wrench, Hash, Package, Save, Sparkles, FileCode2,
} from "lucide-react";
import { useStore } from "../store/useStore";
import { openFolder } from "../services/recompService";
import { fmtDuration, timeAgo } from "../lib/format";
import { Panel, PrimaryBtn, GhostBtn, SectionHeader, StatusChip, ProgressBar, Stat } from "./ui";

export default function GameDetail() {
  const games = useStore((s) => s.games);
  const selectedId = useStore((s) => s.selectedGameId);
  const game = games.find((g) => g.id === selectedId) ?? games[0];
  const profiles = useStore((s) => s.profiles);
  const jobs = useStore((s) => s.jobs);
  const launchGameById = useStore((s) => s.launchGameById);
  const stopGameById = useStore((s) => s.stopGameById);
  const openWizard = useStore((s) => s.openWizard);

  if (!game) {
    return (
      <div className="flex h-full flex-1 items-center justify-center text-white/40">
        No game profiles found — check the profiles\ directory next to the app.
      </div>
    );
  }

  const profile = game.profileId ? profiles.find((p) => p.id === game.profileId) : undefined;
  const liveJob = game.profileId
    ? Object.values(jobs).find((j) => j.status === "running" && j.profileId === game.profileId)
    : undefined;
  const isRunning = game.status === "running";
  const playable = (game.status === "recompiled" || isRunning) && !!game.deployDir && !!game.exePath;

  return (
    <AnimatePresence mode="wait">
      <motion.div
        key={game.id}
        initial={{ opacity: 0, y: 18 }}
        animate={{ opacity: 1, y: 0 }}
        exit={{ opacity: 0, y: -12 }}
        transition={{ duration: 0.28, ease: [0.2, 0.8, 0.2, 1] }}
        className="flex min-h-0 flex-1 flex-col gap-5 overflow-y-auto pr-1 pb-4"
      >
        {/* ============================ HERO ============================ */}
        <div className="relative shrink-0 overflow-hidden rounded-2xl border border-white/10">
          <img src={game.cover} alt="" className="absolute inset-0 h-full w-full scale-110 object-cover opacity-35 blur-2xl" />
          <div className="absolute inset-0 bg-gradient-to-r from-black/80 via-black/55 to-black/25" />
          <div className="absolute inset-x-0 bottom-0 h-24 bg-gradient-to-t from-black/70 to-transparent" />

          <div className="relative flex gap-7 p-7">
            {/* cover with gloss */}
            <div className="group relative w-48 shrink-0">
              <img
                src={game.cover}
                alt={game.title}
                className="aspect-[2/3] w-full rounded-xl object-cover ring-1 ring-white/20 shadow-[0_24px_60px_-12px_rgba(0,0,0,0.9)]"
              />
              <div className="pointer-events-none absolute inset-0 overflow-hidden rounded-xl">
                <div className="absolute inset-y-0 w-1/2 bg-gradient-to-r from-transparent via-white/25 to-transparent opacity-0 transition-opacity duration-300 group-hover:opacity-60" style={{ animation: "shimmer 3s linear infinite" }} />
              </div>
              <div className="absolute inset-x-4 -bottom-3 h-6 rounded-full bg-black/60 blur-md" />
            </div>

            {/* headline block */}
            <div className="flex min-w-0 flex-1 flex-col justify-between py-1">
              <div>
                <div className="mb-3 flex flex-wrap items-center gap-2">
                  <StatusChip status={game.status} />
                  {profile && (
                    <span className="chip">
                      <Layers className="h-3 w-3" /> {profile.name}
                    </span>
                  )}
                  {profile?.customRuntime && (
                    <span className="chip border-[color-mix(in_srgb,var(--accent)_40%,transparent)] bg-[color-mix(in_srgb,var(--accent)_10%,transparent)] text-[var(--accent)]">
                      <Wrench className="h-3 w-3" /> Custom Runtime
                    </span>
                  )}
                </div>
                <h1 className="font-display text-[44px] leading-[0.95] font-bold tracking-[0.04em] text-white uppercase drop-shadow-[0_4px_20px_rgba(0,0,0,0.8)]">
                  {game.title}
                </h1>
                <p className="mt-2 flex flex-wrap items-center gap-x-4 gap-y-1 text-[12px] font-medium text-white/50">
                  <span className="flex items-center gap-1.5"><Clock3 className="h-3.5 w-3.5" /> Last played {timeAgo(game.lastPlayed)}</span>
                  <span className="flex items-center gap-1.5"><Activity className="h-3.5 w-3.5" /> {game.builds.length} builds</span>
                  {game.titleId && <span className="flex items-center gap-1.5"><Hash className="h-3.5 w-3.5" /> {game.titleId}</span>}
                </p>
              </div>

              {/* actions */}
              <div className="mt-5 flex flex-wrap items-center gap-3">
                {isRunning ? (
                  <PrimaryBtn onClick={() => stopGameById(game.id)} className="!from-red-500">
                    <Square className="h-4 w-4 fill-current" /> Stop
                  </PrimaryBtn>
                ) : (
                  <PrimaryBtn onClick={() => void launchGameById(game.id)} disabled={!playable}>
                    <Play className="h-4 w-4 fill-current" />
                    {playable ? "Launch Game" : "No Build Available"}
                  </PrimaryBtn>
                )}
                {liveJob ? (
                  <div className="panel flex h-11 items-center gap-3 rounded-[10px] px-4">
                    <span className="font-display text-xs font-bold tracking-[0.18em] text-amber-300 uppercase">
                      Recompiling… {Math.floor(liveJob.progress)}%
                    </span>
                    <ProgressBar value={liveJob.progress} className="w-40" />
                  </div>
                ) : profile && game.profileId ? (
                  <GhostBtn onClick={() => openWizard(profile.id)}>
                    <Cpu className="h-4 w-4" />
                    {game.builds.length ? "Re-run Recompile" : "Recompile Now"}
                  </GhostBtn>
                ) : null}
                {game.deployDir && (
                  <GhostBtn onClick={() => void openFolder(game.deployDir ?? "")} className="!px-3.5" title="Open standalone folder">
                    <FolderOpen className="h-4 w-4" />
                  </GhostBtn>
                )}
              </div>
              {game.launchError && (
                <p className="mt-2 rounded-lg border border-red-400/20 bg-red-400/8 px-3 py-2 text-[11px] text-red-300">
                  Launch failed: {game.launchError}
                </p>
              )}
            </div>

            {/* stats column */}
            <div className="hidden shrink-0 grid-cols-1 content-center gap-5 border-l border-white/10 pl-7 xl:grid">
              <Stat label={game.titleId ? "Title ID" : "Source"} value={game.titleId || "Imported"} />
              <Stat label="Builds" value={String(game.builds.length)} sub={game.builds[0] ? `latest ${fmtDuration(game.builds[0].durationSec)}` : game.source === "imported" ? "existing build" : "none yet"} />
              <Stat label="SDK" value={profile?.sdkVersion ?? "—"} sub={profile?.customRuntime ? "custom runtime DLL" : game.source === "imported" ? "external package" : "prebuilt runtime"} />
            </div>
          </div>
        </div>

        {/* ====================== BODY GRID ====================== */}
        <div className="grid min-h-0 grid-cols-1 gap-5 xl:grid-cols-[1fr_380px]">
          {/* left column */}
          <div className="flex min-w-0 flex-col gap-5">
            <Panel className="p-6">
              <SectionHeader icon={<Layers className="h-4 w-4" />} title="Overview" />
              <p className="text-[13.5px] leading-relaxed text-white/65">{game.description}</p>
              <div className="mt-4 flex flex-wrap gap-2">
                {game.tags.map((t) => (
                  <span key={t} className="chip">{t}</span>
                ))}
              </div>
            </Panel>

            {profile && profile.runtimeFlags.length > 0 && (
              <Panel className="p-6">
                <SectionHeader icon={<Wrench className="h-4 w-4" />} title="Runtime Patches" />
                <p className="mb-3 text-[11.5px] leading-relaxed text-white/40">
                  Flags compiled into this profile's runtime DLL (
                  <span className="font-mono2 text-white/55">profile.toml → runtime_flags</span>).
                </p>
                <div className="flex flex-wrap gap-1.5">
                  {profile.runtimeFlags.map((f) => (
                    <span key={f} className="chip font-mono2 !text-[9.5px] !tracking-[0.06em] text-white/65">
                      {f}
                    </span>
                  ))}
                </div>
              </Panel>
            )}

            <Panel className="p-6">
              <SectionHeader icon={<Package className="h-4 w-4" />} title="Game Files" />
              {game.deployDir && game.exePath ? (
                <div className="flex flex-col gap-2.5">
                  <div className="panel-deep flex items-center gap-2.5 rounded-[10px] px-3 py-2.5">
                    <Package className="h-4 w-4 shrink-0 text-[var(--accent)]" />
                    <div className="min-w-0 flex-1">
                      <div className="text-[9px] font-bold tracking-[0.14em] text-white/35 uppercase">Game folder</div>
                      <div className="font-mono2 truncate text-[11.5px] text-white/70">{game.deployDir}</div>
                    </div>
                    <GhostBtn className="!h-9 shrink-0 !px-3" onClick={() => void openFolder(game.deployDir ?? "")}>
                      <FolderOpen className="h-3.5 w-3.5" /> Open
                    </GhostBtn>
                  </div>
                  <div className="panel-deep flex items-center gap-2.5 rounded-[10px] px-3 py-2.5">
                    <FileCode2 className="h-4 w-4 shrink-0 text-[var(--accent)]" />
                    <div className="min-w-0 flex-1">
                      <div className="text-[9px] font-bold tracking-[0.14em] text-white/35 uppercase">Executable</div>
                      <div className="font-mono2 truncate text-[11.5px] text-white/70">{game.exePath}</div>
                    </div>
                  </div>
                </div>
              ) : (
                <p className="rounded-xl border border-dashed border-white/12 p-4 text-center text-[12px] text-white/35">
                  No standalone build yet — run a recompile to produce one.
                </p>
              )}
            </Panel>

            <Panel className="p-6">
              <SectionHeader icon={<Save className="h-4 w-4" />} title="Runtime Data Locations" />
              {game.userDataDir && game.shaderCacheDir ? (
                <div className="flex flex-col gap-2.5">
                  <div className="panel-deep flex items-center gap-2.5 rounded-[10px] px-3 py-2.5">
                    <Save className="h-4 w-4 shrink-0 text-[var(--accent)]" />
                    <div className="min-w-0 flex-1">
                      <div className="text-[9px] font-bold tracking-[0.14em] text-white/35 uppercase">User data / saves</div>
                      <div className="font-mono2 truncate text-[11.5px] text-white/70">{game.userDataDir}</div>
                    </div>
                    <GhostBtn className="!h-9 shrink-0 !px-3" onClick={() => void openFolder(game.userDataDir ?? "")}>
                      <FolderOpen className="h-3.5 w-3.5" /> Open
                    </GhostBtn>
                  </div>
                  <div className="panel-deep flex items-center gap-2.5 rounded-[10px] px-3 py-2.5">
                    <Sparkles className="h-4 w-4 shrink-0 text-[var(--accent)]" />
                    <div className="min-w-0 flex-1">
                      <div className="text-[9px] font-bold tracking-[0.14em] text-white/35 uppercase">Shader cache</div>
                      <div className="font-mono2 truncate text-[11.5px] text-white/70">{game.shaderCacheDir}</div>
                    </div>
                    <GhostBtn className="!h-9 shrink-0 !px-3" onClick={() => void openFolder(game.shaderCacheDir ?? "")}>
                      <FolderOpen className="h-3.5 w-3.5" /> Open
                    </GhostBtn>
                  </div>
                  <p className="text-[10.5px] leading-relaxed text-white/30">
                    Shader files may be inside <span className="font-mono2 text-white/45">shareable\</span>. User data also contains runtime cache and profile data, not only saves.
                  </p>
                </div>
              ) : (
                <p className="rounded-xl border border-dashed border-white/12 p-4 text-center text-[12px] text-white/35">
                  These folders are created beside the game executable after deployment.
                </p>
              )}
            </Panel>
          </div>

          {/* right column */}
          <div className="flex min-w-0 flex-col gap-5">
            {profile && game.profileId && (
            <Panel className="p-6">
              <SectionHeader icon={<Cpu className="h-4 w-4" />} title="Build Config" />
              <div className="flex flex-col gap-4">
                <div>
                  <label className="mb-1.5 block text-[10px] font-bold tracking-[0.18em] text-white/40 uppercase">
                    Source Image
                  </label>
                  <div className="panel-deep flex items-center gap-2.5 rounded-[10px] px-3 py-2.5">
                    <Disc3 className="h-4 w-4 shrink-0 text-[var(--accent)]" />
                    <span className="font-mono2 truncate text-[11.5px] text-white/70">
                      {game.isoPath ?? "No ISO linked — pick one in the wizard"}
                    </span>
                  </div>
                </div>
                {profile && (
                  <div className="flex flex-wrap items-center gap-x-4 gap-y-1 text-[11px] text-white/45">
                    <span>{profile.cvarCount} forced cvars</span>
                    <span>{profile.runtimeFlags.length} runtime flags</span>
                    <span>SDK {profile.sdkVersion}</span>
                  </div>
                )}
                <PrimaryBtn onClick={() => openWizard(profile.id)} disabled={!!liveJob}>
                  <Cpu className="h-4 w-4" /> Run Recompile
                </PrimaryBtn>
              </div>
            </Panel>
            )}

            <Panel className="flex min-h-0 flex-1 flex-col p-6">
              <SectionHeader icon={<FolderOpen className="h-4 w-4" />} title="Build History" />
              <div className="flex flex-col gap-2 overflow-y-auto">
                {game.builds.length === 0 && (
                  <p className="rounded-xl border border-dashed border-white/12 p-4 text-center text-[12px] text-white/35">
                    {game.source === "imported"
                      ? "Imported package — build history starts when Glue360 creates a build."
                      : "No builds yet. Run a recompile to produce the first native package."}
                  </p>
                )}
                {game.builds.map((b) => (
                  <div key={b.id} className="panel-deep flex items-center gap-3 rounded-xl px-3.5 py-3">
                    {b.success ? (
                      <CheckCircle2 className="h-4 w-4 shrink-0 text-[var(--accent)]" />
                    ) : (
                      <XCircle className="h-4 w-4 shrink-0 text-red-400" />
                    )}
                    <div className="min-w-0 flex-1">
                      <div className="font-display truncate text-[13px] font-bold tracking-wider text-white/90">
                        {new Date(b.date).toLocaleDateString()}
                      </div>
                      <div className="truncate text-[10.5px] text-white/40">
                        {b.profileName} · {new Date(b.date).toLocaleTimeString()}
                      </div>
                    </div>
                    <span className="font-mono2 shrink-0 text-[11px] text-white/45">
                      {fmtDuration(b.durationSec)}
                    </span>
                  </div>
                ))}
              </div>
            </Panel>
          </div>
        </div>
      </motion.div>
    </AnimatePresence>
  );
}
