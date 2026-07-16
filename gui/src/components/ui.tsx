/* ---------- shared UI primitives (skin these here) ---------- */
import { cn } from "../utils/cn";
import type { ReactNode } from "react";
import type { GameStatus } from "../lib/types";

export function Panel({
  className,
  children,
}: {
  className?: string;
  children: ReactNode;
}) {
  return <div className={cn("panel rounded-2xl", className)}>{children}</div>;
}

export function SectionHeader({
  icon,
  title,
  right,
  className,
}: {
  icon?: ReactNode;
  title: string;
  right?: ReactNode;
  className?: string;
}) {
  return (
    <div className={cn("mb-4 flex items-center justify-between gap-3", className)}>
      <div className="flex items-center gap-2.5">
        {icon && <span className="text-[var(--accent)]">{icon}</span>}
        <h2 className="font-display text-[13px] font-bold tracking-[0.22em] text-white/85 uppercase">
          {title}
        </h2>
      </div>
      {right}
    </div>
  );
}

export function PrimaryBtn({
  className,
  ...props
}: React.ButtonHTMLAttributes<HTMLButtonElement>) {
  return (
    <button
      {...props}
      className={cn("btn btn-primary h-11 px-6 text-[13px]", className)}
    />
  );
}

export function GhostBtn({
  className,
  ...props
}: React.ButtonHTMLAttributes<HTMLButtonElement>) {
  return (
    <button {...props} className={cn("btn btn-ghost h-11 px-5 text-[12px]", className)} />
  );
}

const GAME_STATUS_META: Record<GameStatus, { label: string; cls: string; dot: string }> = {
  recompiled: { label: "Ready", cls: "text-[var(--accent)] border-[color-mix(in_srgb,var(--accent)_40%,transparent)] bg-[color-mix(in_srgb,var(--accent)_10%,transparent)]", dot: "bg-[var(--accent)]" },
  running: { label: "Running", cls: "text-sky-300 border-sky-400/40 bg-sky-400/10", dot: "bg-sky-400 anim-blink" },
  recompiling: { label: "Recompiling", cls: "text-amber-300 border-amber-400/40 bg-amber-400/10", dot: "bg-amber-400 anim-blink" },
  queued: { label: "Queued", cls: "text-amber-200 border-amber-300/30 bg-amber-300/10", dot: "bg-amber-300" },
  not_compiled: { label: "No Build", cls: "text-white/50 border-white/15 bg-white/5", dot: "bg-white/40" },
  failed: { label: "Failed", cls: "text-red-400 border-red-500/40 bg-red-500/10", dot: "bg-red-500" },
};

export function StatusChip({ status, className }: { status: GameStatus; className?: string }) {
  const meta = GAME_STATUS_META[status];
  return (
    <span className={cn("chip", meta.cls, className)}>
      <span className={cn("h-1.5 w-1.5 rounded-full", meta.dot)} />
      {meta.label}
    </span>
  );
}


export function ProgressBar({
  value,
  className,
  barClassName,
}: {
  value: number; // 0..100
  className?: string;
  barClassName?: string;
}) {
  return (
    <div className={cn("relative h-1.5 overflow-hidden rounded-full bg-white/8", className)}>
      <div
        className={cn("h-full rounded-full transition-[width] duration-200", barClassName)}
        style={{
          width: `${value}%`,
          background: "linear-gradient(90deg, color-mix(in srgb, var(--accent) 60%, transparent), var(--accent))",
          boxShadow: "0 0 12px color-mix(in srgb, var(--accent) 60%, transparent)",
        }}
      />
    </div>
  );
}

export function Toggle({
  checked,
  onChange,
  label,
}: {
  checked: boolean;
  onChange: (v: boolean) => void;
  label?: string;
}) {
  return (
    <button
      type="button"
      onClick={() => onChange(!checked)}
      className="group flex items-center gap-3 text-left"
    >
      <span
        className={cn(
          "relative h-5 w-9 shrink-0 rounded-full border transition-all duration-200",
          checked
            ? "border-transparent bg-[var(--accent)] shadow-[0_0_10px_color-mix(in_srgb,var(--accent)_50%,transparent)]"
            : "border-white/15 bg-white/8",
        )}
      >
        <span
          className={cn(
            "absolute top-1/2 h-3.5 w-3.5 -translate-y-1/2 rounded-full bg-white transition-all duration-200",
            checked ? "left-[calc(100%-18px)]" : "left-0.5",
          )}
        />
      </span>
      {label && <span className="text-[13px] text-white/70 group-hover:text-white/90">{label}</span>}
    </button>
  );
}

export function Stat({
  label,
  value,
  sub,
  className,
}: {
  label: string;
  value: string;
  sub?: string;
  className?: string;
}) {
  return (
    <div className={cn("min-w-0", className)}>
      <div className="text-[10px] font-semibold tracking-[0.18em] text-white/40 uppercase">
        {label}
      </div>
      <div className="font-display mt-0.5 truncate text-xl font-bold tracking-wide text-white">
        {value}
      </div>
      {sub && <div className="mt-0.5 text-[11px] text-white/40">{sub}</div>}
    </div>
  );
}
