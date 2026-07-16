/* ---------- jobs / build queue blade ---------- */
import { useState } from "react";
import { AnimatePresence, motion } from "framer-motion";
import {
  ListChecks, Cpu, CheckCircle2, XCircle, Ban, ChevronDown, Trash2, Timer,
} from "lucide-react";
import { useStore } from "../store/useStore";
import { RECOMP_PHASES } from "../services/recompService";
import { fmtDuration } from "../lib/format";
import { cn } from "../utils/cn";
import { Panel, SectionHeader, ProgressBar, GhostBtn } from "./ui";
import ConsoleStream from "./ConsoleStream";

const STATUS_META = {
  running: { icon: Cpu, cls: "text-amber-300", label: "Running" },
  done: { icon: CheckCircle2, cls: "text-[var(--accent)]", label: "Complete" },
  failed: { icon: XCircle, cls: "text-red-400", label: "Failed" },
  cancelled: { icon: Ban, cls: "text-white/45", label: "Cancelled" },
} as const;

export default function QueueView() {
  const jobs = useStore((s) => s.jobs);
  const jobOrder = useStore((s) => s.jobOrder);
  const cancelJob = useStore((s) => s.cancelJob);
  const clearFinishedJobs = useStore((s) => s.clearFinishedJobs);
  const profiles = useStore((s) => s.profiles);
  const [expanded, setExpanded] = useState<string | null>(null);

  const ordered = jobOrder.map((id) => jobs[id]).filter(Boolean);
  const active = ordered.filter((j) => j.status === "running");
  const history = ordered.filter((j) => j.status !== "running");

  return (
    <div className="flex min-h-0 flex-1 flex-col gap-5 overflow-y-auto pb-6">
      {/* active */}
      <Panel className="shrink-0 p-5">
        <SectionHeader
          icon={<Cpu className="h-4 w-4" />}
          title={`Active Builds · ${active.length}`}
          right={
            history.length > 0 && (
              <GhostBtn className="!h-8 !px-3 !text-[10px]" onClick={clearFinishedJobs}>
                <Trash2 className="h-3.5 w-3.5" /> Clear History
              </GhostBtn>
            )
          }
        />
        {active.length === 0 ? (
          <p className="rounded-xl border border-dashed border-white/12 p-6 text-center text-[12.5px] text-white/35">
            Queue is idle. Start a recompile from the Recompiler blade or any game page.
          </p>
        ) : (
          <div className="flex flex-col gap-3">
            {active.map((j) => (
              <JobRow key={j.id} jobId={j.id} expanded={expanded === j.id} onToggle={() => setExpanded(expanded === j.id ? null : j.id)} onCancel={() => cancelJob(j.id)} profileName={profiles.find((p) => p.id === j.profileId)?.name} />
            ))}
          </div>
        )}
      </Panel>

      {/* history */}
      <Panel className="p-5">
        <SectionHeader icon={<ListChecks className="h-4 w-4" />} title={`History · ${history.length}`} />
        {history.length === 0 ? (
          <p className="text-[12.5px] text-white/30">Nothing here yet — completed builds will be logged.</p>
        ) : (
          <div className="flex flex-col gap-2">
            {history.map((j) => (
              <JobRow key={j.id} jobId={j.id} expanded={expanded === j.id} onToggle={() => setExpanded(expanded === j.id ? null : j.id)} profileName={profiles.find((p) => p.id === j.profileId)?.name} />
            ))}
          </div>
        )}
      </Panel>
    </div>
  );
}

function JobRow({
  jobId,
  expanded,
  onToggle,
  onCancel,
  profileName,
}: {
  jobId: string;
  expanded: boolean;
  onToggle: () => void;
  onCancel?: () => void;
  profileName?: string;
}) {
  const job = useStore((s) => s.jobs[jobId]);
  if (!job) return null;
  const meta = STATUS_META[job.status];
  const Icon = meta.icon;
  const dur = ((job.endedAt ?? Date.now()) - job.startedAt) / 1000;

  return (
    <motion.div layout initial={{ opacity: 0, y: 10 }} animate={{ opacity: 1, y: 0 }} className="panel-deep overflow-hidden rounded-xl">
      <button onClick={onToggle} className="flex w-full items-center gap-4 px-4 py-3.5 text-left">
        {job.cover ? (
          <img src={job.cover} alt="" className="h-12 w-9 rounded-md object-cover ring-1 ring-white/10" />
        ) : (
          <span className="flex h-12 w-9 items-center justify-center rounded-md bg-white/8 ring-1 ring-white/10">
            <Cpu className="h-4 w-4 text-white/30" />
          </span>
        )}
        <span className={cn("flex h-8 w-8 shrink-0 items-center justify-center rounded-lg bg-white/6", meta.cls)}>
          <Icon className={cn("h-4 w-4", job.status === "running" && "anim-spin-slow")} />
        </span>
        <span className="min-w-0 flex-1">
          <span className="font-display block truncate text-[13px] font-bold tracking-[0.1em] text-white/90 uppercase">
            {job.title}
          </span>
          <span className="mt-0.5 flex items-center gap-3 text-[11px] text-white/40">
            <span className={cn("font-semibold", meta.cls)}>{meta.label}</span>
            {profileName && <span>{profileName}</span>}
            <span className="flex items-center gap-1"><Timer className="h-3 w-3" />{fmtDuration(dur)}</span>
          </span>
        </span>
        {job.status === "running" && (
          <span className="w-48 shrink-0">
            <span className="mb-1 flex justify-between text-[10px] text-white/40">
              <span className="truncate">{RECOMP_PHASES[job.phaseIndex]}</span>
              <span className="font-mono2 text-[var(--accent)]">{Math.floor(job.progress)}%</span>
            </span>
            <ProgressBar value={job.progress} />
          </span>
        )}
        {onCancel && (
          <GhostBtn
            className="!h-8 shrink-0 !px-3 !text-[10px] !text-red-300 hover:!bg-red-500/10"
            onClick={(e) => {
              e.stopPropagation();
              onCancel();
            }}
          >
            Abort
          </GhostBtn>
        )}
        <ChevronDown className={cn("h-4 w-4 shrink-0 text-white/30 transition-transform", expanded && "rotate-180")} />
      </button>
      <AnimatePresence>
        {expanded && (
          <motion.div
            initial={{ height: 0, opacity: 0 }}
            animate={{ height: "auto", opacity: 1 }}
            exit={{ height: 0, opacity: 0 }}
            transition={{ duration: 0.22 }}
            className="overflow-hidden"
          >
            <div className="px-4 pb-4">
              <ConsoleStream logs={job.logs} className="h-52" title={`session ${job.id.slice(-5)} · ${job.isoPath}`} />
            </div>
          </motion.div>
        )}
      </AnimatePresence>
    </motion.div>
  );
}
