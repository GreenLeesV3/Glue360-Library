/* ---------- live recompile console (shared by wizard + jobs) ---------- */
import { useEffect, useRef } from "react";
import { TerminalSquare } from "lucide-react";
import type { JobLog } from "../lib/types";
import { fmtLogTime } from "../lib/format";
import { cn } from "../utils/cn";

const LEVEL_CLS: Record<JobLog["level"], string> = {
  info: "text-white/55",
  ok: "text-[var(--accent)]",
  warn: "text-amber-300",
  err: "text-red-400",
};

export default function ConsoleStream({
  logs,
  className,
  title = "toolchain stdout",
}: {
  logs: JobLog[];
  className?: string;
  title?: string;
}) {
  const endRef = useRef<HTMLDivElement>(null);
  const scrollRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    const el = scrollRef.current;
    if (!el) return;
    // only autoscroll when near the bottom (don't fight the user)
    const nearBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 120;
    if (nearBottom) endRef.current?.scrollIntoView({ behavior: "smooth", block: "end" });
  }, [logs.length]);

  return (
    <div className={cn("flex min-h-0 flex-col overflow-hidden rounded-xl border border-white/10 bg-black/55", className)}>
      <div className="flex shrink-0 items-center gap-2 border-b border-white/8 px-3.5 py-2">
        <TerminalSquare className="h-3.5 w-3.5 text-[var(--accent)]" />
        <span className="font-mono2 text-[10.5px] tracking-wider text-white/45">{title}</span>
        <span className="ml-auto flex gap-1.5">
          <span className="h-2 w-2 rounded-full bg-red-500/50" />
          <span className="h-2 w-2 rounded-full bg-amber-400/50" />
          <span className="h-2 w-2 rounded-full bg-[color-mix(in_srgb,var(--accent)_60%,transparent)]" />
        </span>
      </div>
      <div ref={scrollRef} className="min-h-0 flex-1 overflow-y-auto px-3.5 py-2.5">
        {logs.map((l, i) => (
          <div key={i} className="font-mono2 flex gap-2.5 py-[1.5px] text-[11px] leading-[1.5]">
            <span className="shrink-0 text-white/25 tabular-nums">{fmtLogTime(l.t)}</span>
            <span className={cn("break-all", LEVEL_CLS[l.level])}>{l.msg}</span>
          </div>
        ))}
        {logs.length > 0 && (
          <span className="anim-blink font-mono2 mt-1 inline-block h-3.5 w-2 bg-[var(--accent)]" />
        )}
        {logs.length === 0 && (
          <span className="font-mono2 text-[11px] text-white/30">waiting for toolchain output…</span>
        )}
        <div ref={endRef} />
      </div>
    </div>
  );
}
