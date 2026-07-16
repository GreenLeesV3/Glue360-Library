/* ---------- Xbox "blades" tab bar + identity cluster ---------- */
import { useEffect, useState } from "react";
import { motion } from "framer-motion";
import { Gamepad2, Cpu, ListChecks, Settings2, Package, Plus } from "lucide-react";
import { cn } from "../utils/cn";
import { useStore } from "../store/useStore";
import type { View } from "../lib/types";

function XboxMark({ className }: { className?: string }) {
  return (
    <svg viewBox="0 0 32 32" className={className} aria-hidden>
      <circle cx="16" cy="16" r="14.5" fill="none" stroke="currentColor" strokeWidth="2" />
      <path
        d="M7.5 8.5c3-1.6 4.6 4.2 8.5 8.5 3.9-4.3 5.5-10.1 8.5-8.5 2.6 1.4 4.3 4.9 4.6 8.4-2.9-2-5.8-.4-8.4 2.2-3 .3-6.4.2-9.4-.2-2.6-2.6-5.5-4.2-8.4-2.2.3-3.5 2-7 4.6-8.4Z"
        fill="currentColor"
        opacity="0.95"
      />
    </svg>
  );
}

const BLADES: { id: View | "wizard"; label: string; icon: typeof Gamepad2 }[] = [
  { id: "library", label: "Games", icon: Gamepad2 },
  { id: "wizard", label: "Recompiler", icon: Cpu },
  { id: "queue", label: "Jobs", icon: ListChecks },
  { id: "system", label: "System", icon: Settings2 },
];

function Clock() {
  const [now, setNow] = useState(new Date());
  useEffect(() => {
    const t = setInterval(() => setNow(new Date()), 10_000);
    return () => clearInterval(t);
  }, []);
  return (
    <span className="font-display text-sm font-semibold tracking-[0.15em] text-white/70 tabular-nums">
      {now.getHours().toString().padStart(2, "0")}:{now.getMinutes().toString().padStart(2, "0")}
    </span>
  );
}

export default function TopBar() {
  const view = useStore((s) => s.view);
  const setView = useStore((s) => s.setView);
  const openWizard = useStore((s) => s.openWizard);
  const jobs = useStore((s) => s.jobs);
  const toolchain = useStore((s) => s.toolchain);
  const runningJobs = Object.values(jobs).filter((j) => j.status === "running").length;

  return (
    <header className="relative z-20 flex h-[72px] shrink-0 items-center gap-6 px-6">
      {/* identity */}
      <div className="flex items-center gap-3">
        <div className="anim-ringpulse flex h-10 w-10 items-center justify-center rounded-full text-[var(--accent)]">
          <XboxMark className="h-8 w-8" />
        </div>
        <div className="leading-none">
          <div className="font-display text-lg font-bold tracking-[0.28em] text-white">
            GLUE<span className="text-[var(--accent)]">360</span>
          </div>
          <div className="mt-1 text-[9px] font-semibold tracking-[0.34em] text-white/35 uppercase">
            Xbox 360 Recompile Station{toolchain?.appVersion ? ` · v${toolchain.appVersion}` : ""}
          </div>
        </div>
      </div>

      {/* blades */}
      <nav className="mx-auto flex items-stretch gap-1" aria-label="Primary">
        {BLADES.map((b, i) => {
          const isOn = b.id !== "wizard" && view === b.id;
          const Icon = b.icon;
          return (
            <motion.button
              key={b.id}
              initial={{ opacity: 0, y: -14 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ delay: 0.06 * i + 0.1, type: "spring", stiffness: 200, damping: 20 }}
              onClick={() => (b.id === "wizard" ? openWizard() : setView(b.id as View))}
              className={cn(
                "blade relative flex items-center gap-2 px-7 py-2.5",
                "font-display text-[13px] font-bold tracking-[0.24em] uppercase",
                isOn ? "blade-on" : "blade-off",
              )}
            >
              <Icon className="h-4 w-4" strokeWidth={2.4} />
              {b.label}
              {b.id === "queue" && runningJobs > 0 && (
                <span className="absolute -top-1.5 -right-1 flex h-5 min-w-5 items-center justify-center rounded-full bg-[var(--accent)] px-1 text-[10px] font-bold text-black shadow-[0_0_10px_color-mix(in_srgb,var(--accent)_60%,transparent)]">
                  {runningJobs}
                </span>
              )}
            </motion.button>
          );
        })}
      </nav>

      {/* right cluster */}
      <div className="flex items-center gap-4">
        <button
          onClick={() => openWizard()}
          className="btn btn-primary hidden h-9 px-4 text-[11px] md:inline-flex"
        >
          <Plus className="h-4 w-4" strokeWidth={3} />
          Recompile
        </button>
        <div className="hidden items-center gap-4 border-l border-white/10 pl-4 lg:flex">
          <span
            className={cn(
              "flex items-center gap-1.5 text-[11px] font-medium",
              toolchain?.sdkVersion ? "text-white/45" : "text-red-300/80",
            )}
            title={toolchain?.sdkRoot || "SDK not detected"}
          >
            <Package className={cn("h-3.5 w-3.5", toolchain?.sdkVersion ? "text-[var(--accent)]" : "text-red-400")} />
            {toolchain?.sdkVersion ? `SDK ${toolchain.sdkVersion}` : "SDK MISSING"}
          </span>
          <Clock />
        </div>
      </div>
    </header>
  );
}
