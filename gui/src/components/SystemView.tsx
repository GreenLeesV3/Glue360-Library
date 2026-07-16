/* ---------- system blade: real toolchain readout, paths, interface ---------- */
import { useState } from "react";
import {
  FolderCog, Wrench, MonitorCog, FolderOpen, RefreshCcw, CheckCircle2, XCircle,
} from "lucide-react";
import { useStore } from "../store/useStore";
import { hasBridge, openFolder, pickDir } from "../services/recompService";
import { cn } from "../utils/cn";
import { Panel, SectionHeader, GhostBtn, Toggle } from "./ui";

const ACCENTS = [
  { id: "green", label: "Xbox Green", hex: "#9ce318" },
  { id: "crimson", label: "Crimson", hex: "#ff4757" },
  { id: "frost", label: "Frost", hex: "#38c2ff" },
  { id: "violet", label: "Violet", hex: "#a78bfa" },
] as const;

export default function SystemView() {
  const settings = useStore((s) => s.settings);
  const updateSettings = useStore((s) => s.updateSettings);
  const profiles = useStore((s) => s.profiles);
  const toolchain = useStore((s) => s.toolchain);
  const refreshToolchain = useStore((s) => s.refreshToolchain);
  const [scanning, setScanning] = useState(false);

  const rescan = async () => {
    setScanning(true);
    try {
      await refreshToolchain();
    } finally {
      setScanning(false);
    }
  };

  const browsePath = async (key: "sdkPath" | "outputRoot" | "sdkSourcePath", title: string) => {
    const dir = await pickDir(title);
    if (dir) {
      updateSettings({ [key]: dir });
      if (key === "sdkPath") await rescan();
    }
  };

  return (
    <div className="grid min-h-0 flex-1 grid-cols-1 gap-5 overflow-y-auto pb-6 lg:grid-cols-2 xl:grid-cols-3">
      {/* toolchain — real dependency checker readout */}
      <Panel className="h-fit p-6">
        <SectionHeader
          icon={<Wrench className="h-4 w-4" />}
          title="Toolchain"
          right={
            <GhostBtn className="!h-8 !px-3 !text-[10px]" onClick={() => void rescan()} disabled={scanning}>
              <RefreshCcw className={cn("h-3.5 w-3.5", scanning && "anim-spin-slow")} /> Re-scan
            </GhostBtn>
          }
        />
        {toolchain ? (
          <div className="flex flex-col gap-2">
            <ToolRow name="RexGlue SDK" value={toolchain.sdkVersion || "not found"} ok={!!toolchain.sdkVersion} sub={toolchain.sdkRoot} />
            <ToolRow name="clang-cl" value={toolchain.clangVersion || "not found"} ok={!!toolchain.clangVersion} />
            <ToolRow name="CMake" value={toolchain.cmakeVersion || "not found"} ok={!!toolchain.cmakeVersion} />
            <ToolRow name="Ninja" value={toolchain.ninjaVersion || "not found"} ok={!!toolchain.ninjaVersion} />
            <ToolRow name="MSVC" value={toolchain.msvcVersion || "not found"} ok={!!toolchain.msvcVersion} />
            {toolchain.issues.length > 0 && (
              <div className="mt-1 rounded-xl border border-red-500/25 bg-red-500/8 p-3">
                {toolchain.issues.map((issue, i) => (
                  <p key={i} className="text-[11px] leading-relaxed text-red-300/85">
                    {issue}
                  </p>
                ))}
              </div>
            )}
            <p className="mt-1 text-[10.5px] text-white/30">
              Glue360 {toolchain.appVersion || "dev"} · pipeline shells out to these tools — nothing
              is bundled into the exe except the SDK.
            </p>
          </div>
        ) : (
          <p className="rounded-xl border border-dashed border-white/12 p-4 text-center text-[12px] text-white/35">
            {hasBridge ? "Scanning toolchain…" : "No host bridge (dev mode) — toolchain readout unavailable."}
          </p>
        )}
      </Panel>

      {/* paths + build defaults */}
      <Panel className="h-fit p-6">
        <SectionHeader icon={<FolderCog className="h-4 w-4" />} title="Paths & Defaults" />
        <div className="flex flex-col gap-4">
          <PathField
            label="RexGlue SDK Root"
            value={settings.sdkPath || "(auto-detect)"}
            onBrowse={() => void browsePath("sdkPath", "Select the RexGlue360 SDK root")}
            onOpen={settings.sdkPath ? () => void openFolder(settings.sdkPath) : undefined}
          />
          <PathField
            label="Default Output Root"
            value={settings.outputRoot || "(ask every time)"}
            onBrowse={() => void browsePath("outputRoot", "Select the default output root")}
            onOpen={settings.outputRoot ? () => void openFolder(settings.outputRoot) : undefined}
          />
          <PathField
            label="SDK Source Tree (optional)"
            value={settings.sdkSourcePath || "(not set — bundled runtime DLLs are used)"}
            onBrowse={() => void browsePath("sdkSourcePath", "Select the RexGlue SDK source tree")}
          />
          <div className="flex flex-col gap-3 border-t border-white/8 pt-4">
            <div>
              <label className="mb-1.5 block text-[10px] font-bold tracking-[0.18em] text-white/40 uppercase">
                Default Profile
              </label>
              <select
                className="field font-mono2 text-[12px]"
                value={settings.defaultProfileId ?? ""}
                onChange={(e) => updateSettings({ defaultProfileId: e.target.value || null })}
              >
                <option value="">— none —</option>
                {profiles.map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.name}
                  </option>
                ))}
              </select>
            </div>
            <Toggle
              label="Clean build by default (wipe stage outputs each run)"
              checked={settings.cleanBuild}
              onChange={(v) => updateSettings({ cleanBuild: v })}
            />
          </div>
        </div>
      </Panel>

      {/* interface */}
      <Panel className="h-fit p-6">
        <SectionHeader icon={<MonitorCog className="h-4 w-4" />} title="Interface" />
        <div>
          <label className="mb-2 block text-[10px] font-bold tracking-[0.18em] text-white/40 uppercase">
            Dashboard Accent
          </label>
          <div className="grid grid-cols-2 gap-2">
            {ACCENTS.map((a) => {
              const on = settings.accent === a.id;
              return (
                <button
                  key={a.id}
                  onClick={() => updateSettings({ accent: a.id })}
                  className={cn(
                    "flex items-center gap-2.5 rounded-xl border p-3 text-left transition-all",
                    on ? "border-white/30 bg-white/8" : "border-white/8 bg-white/2 hover:bg-white/5",
                  )}
                >
                  <span
                    className="h-5 w-5 rounded-full ring-2 ring-offset-2 ring-offset-black"
                    style={{ backgroundColor: a.hex, ...(on ? { ["--tw-ring-color" as string]: a.hex } : {}) }}
                  />
                  <span className="text-[12px] font-semibold text-white/75">{a.label}</span>
                </button>
              );
            })}
          </div>
        </div>
      </Panel>
    </div>
  );
}

function ToolRow({ name, value, ok, sub }: { name: string; value: string; ok: boolean; sub?: string }) {
  return (
    <div className="panel-deep flex items-center gap-3 rounded-xl px-3.5 py-2.5">
      {ok ? (
        <CheckCircle2 className="h-4 w-4 shrink-0 text-[var(--accent)]" />
      ) : (
        <XCircle className="h-4 w-4 shrink-0 text-red-400" />
      )}
      <div className="min-w-0 flex-1">
        <div className="flex items-baseline justify-between gap-3">
          <span className="font-display text-[12px] font-bold tracking-[0.12em] text-white/85 uppercase">
            {name}
          </span>
          <span className="font-mono2 shrink-0 text-[11px] text-white/55">{value}</span>
        </div>
        {sub && <div className="font-mono2 mt-0.5 truncate text-[10px] text-white/30">{sub}</div>}
      </div>
    </div>
  );
}

function PathField({
  label,
  value,
  onBrowse,
  onOpen,
}: {
  label: string;
  value: string;
  onBrowse: () => void;
  onOpen?: () => void;
}) {
  return (
    <div>
      <label className="mb-1.5 block text-[10px] font-bold tracking-[0.18em] text-white/40 uppercase">
        {label}
      </label>
      <div className="flex items-center gap-2">
        <div className="panel-deep min-w-0 flex-1 rounded-lg px-3 py-2.5">
          <span className="font-mono2 block truncate text-[11.5px] text-white/70">{value}</span>
        </div>
        {onOpen && (
          <GhostBtn className="!h-9 shrink-0 !px-3" onClick={onOpen} title="Open in Explorer">
            <FolderOpen className="h-3.5 w-3.5" />
          </GhostBtn>
        )}
        {hasBridge && (
          <GhostBtn className="!h-9 shrink-0 !px-3" onClick={onBrowse}>
            Browse
          </GhostBtn>
        )}
      </div>
    </div>
  );
}
