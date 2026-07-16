/* ---------- virtual filesystem browser (dev-sim mode only) ----------
   In the native app, ISO/folder picking uses Windows dialogs via the
   bridge (pick_iso / pick_dir). This component only serves `npm run dev`
   in a browser, backed by the service's simulated filesystem.          */
import { useEffect, useMemo, useState } from "react";
import {
  ArrowUp, Disc3, FileCode2, File as FileIcon, Folder, HardDrive, RefreshCw,
} from "lucide-react";
import { browseDirectory, normalizePath, parentPath } from "../services/recompService";
import type { FSEntry } from "../lib/types";
import { fmtBytes } from "../lib/format";
import { cn } from "../utils/cn";
import { GhostBtn } from "./ui";

const ISO_EXTENSIONS = [".iso"];

const DRIVES = [
  { label: "C:\\", path: "C:/" },
  { label: "D:\\", path: "D:/" },
];

export default function FileBrowser({
  initialPath = "D:/Games/XBOX360",
  mode = "file", // 'file' = pick an iso/xex, 'dir' = pick a folder
  onSelect,
  className,
}: {
  initialPath?: string;
  mode?: "file" | "dir";
  onSelect: (path: string, entry?: FSEntry) => void;
  className?: string;
}) {
  const [path, setPath] = useState(normalizePath(initialPath));
  const [entries, setEntries] = useState<FSEntry[]>([]);
  const [loading, setLoading] = useState(false);
  const [selected, setSelected] = useState<string | null>(null);

  useEffect(() => {
    let alive = true;
    setLoading(true);
    browseDirectory(path).then((list) => {
      if (!alive) return;
      setEntries(list);
      setLoading(false);
    });
    return () => {
      alive = false;
    };
  }, [path]);

  const visible = useMemo(() => {
    const dirs = entries.filter((e) => e.type === "dir");
    const files = entries.filter((e) => e.type === "file");
    return [...dirs, ...files];
  }, [entries]);

  const crumbs = useMemo(() => {
    const n = normalizePath(path).replace(/\/$/, "");
    const segs = n.split("/").filter(Boolean);
    let acc = "";
    return segs.map((s, i) => {
      acc += s + (i === 0 ? ":/" : "/");
      return { label: i === 0 ? `${s}:\\` : s, path: normalizePath(acc) };
    });
  }, [path]);

  const isIsoLike = (name: string) =>
    ISO_EXTENSIONS.some((ext) => name.toLowerCase().endsWith(ext));

  const entryPath = (e: FSEntry) =>
    normalizePath(path.endsWith("/") ? path + e.name : `${path}/${e.name}`);

  const activate = (e: FSEntry) => {
    const p = entryPath(e);
    if (e.type === "dir") {
      setPath(p);
      setSelected(null);
      if (mode === "dir") onSelect(p, e);
    } else {
      setSelected(p);
      if (mode === "file") onSelect(p, e);
    }
  };

  return (
    <div className={cn("flex min-h-0 flex-col", className)}>
      {/* toolbar */}
      <div className="mb-2 flex items-center gap-2">
        {DRIVES.map((d) => (
          <button
            key={d.path}
            onClick={() => {
              setPath(d.path);
              setSelected(null);
            }}
            className={cn(
              "btn h-8 gap-1.5 px-3 text-[10px]",
              path.startsWith(d.path) ? "btn-primary" : "btn-ghost",
            )}
          >
            <HardDrive className="h-3.5 w-3.5" />
            {d.label}
          </button>
        ))}
        <GhostBtn
          className="!h-8 !px-2.5"
          onClick={() => setPath(parentPath(path))}
          disabled={path === "C:/" || path === "D:/"}
          title="Up one level"
        >
          <ArrowUp className="h-3.5 w-3.5" />
        </GhostBtn>
        {/* breadcrumb */}
        <div className="panel-deep ml-1 flex min-w-0 flex-1 items-center gap-1 overflow-x-auto rounded-lg px-2.5 py-1.5">
          {crumbs.map((c, i) => (
            <span key={c.path} className="flex shrink-0 items-center gap-1">
              <button
                onClick={() => {
                  setPath(c.path);
                  setSelected(null);
                }}
                className={cn(
                  "font-mono2 rounded px-1 text-[11px] hover:bg-white/10",
                  i === crumbs.length - 1 ? "text-[var(--accent)]" : "text-white/50",
                )}
              >
                {c.label}
              </button>
              {i < crumbs.length - 1 && <span className="text-white/25">›</span>}
            </span>
          ))}
        </div>
      </div>

      {/* listing */}
      <div className="panel-deep min-h-0 flex-1 overflow-y-auto rounded-xl">
        {loading ? (
          <div className="flex h-full items-center justify-center gap-2 text-white/40">
            <RefreshCw className="anim-spin-slow h-4 w-4" />
            <span className="text-xs">reading directory…</span>
          </div>
        ) : visible.length === 0 ? (
          <div className="flex h-full items-center justify-center text-xs text-white/30">
            empty folder
          </div>
        ) : (
          <table className="w-full text-left">
            <thead className="sticky top-0 bg-black/60 backdrop-blur">
              <tr className="text-[9px] font-bold tracking-[0.2em] text-white/30 uppercase">
                <th className="px-3.5 py-2 font-semibold">Name</th>
                <th className="w-24 px-2 py-2 font-semibold">Size</th>
                <th className="w-32 px-2 py-2 font-semibold">Modified</th>
              </tr>
            </thead>
            <tbody>
              {visible.map((e) => {
                const p = entryPath(e);
                const isSel = selected === p;
                return (
                  <tr
                    key={p}
                    onClick={() => activate(e)}
                    onDoubleClick={() => e.type === "file" && isIsoLike(e.name) && activate(e)}
                    className={cn(
                      "group cursor-pointer border-t border-white/4 transition-colors",
                      isSel
                        ? "bg-[color-mix(in_srgb,var(--accent)_14%,transparent)]"
                        : "hover:bg-white/4",
                    )}
                  >
                    <td className="px-3.5 py-2">
                      <span className="flex items-center gap-2.5">
                        {e.type === "dir" ? (
                          <Folder className="h-4 w-4 shrink-0 text-amber-300/80" />
                        ) : isIsoLike(e.name) ? (
                          <Disc3 className={cn("h-4 w-4 shrink-0", isSel ? "text-[var(--accent)]" : "text-[var(--accent)]/70")} />
                        ) : e.name.endsWith(".toml") ? (
                          <FileCode2 className="h-4 w-4 shrink-0 text-white/40" />
                        ) : (
                          <FileIcon className="h-4 w-4 shrink-0 text-white/35" />
                        )}
                        <span
                          className={cn(
                            "font-mono2 truncate text-[12px]",
                            isSel ? "text-white" : "text-white/70 group-hover:text-white/90",
                          )}
                        >
                          {e.name}
                        </span>
                        {e.type === "file" && isIsoLike(e.name) && (
                          <span className="chip !px-1.5 !py-px !text-[8px] text-[var(--accent)]">
                            {e.name.split(".").pop()?.toUpperCase()}
                          </span>
                        )}
                      </span>
                    </td>
                    <td className="font-mono2 px-2 py-2 text-[11px] whitespace-nowrap text-white/40">
                      {e.type === "dir" ? "—" : fmtBytes(e.size)}
                    </td>
                    <td className="font-mono2 px-2 py-2 text-[11px] whitespace-nowrap text-white/40">
                      {e.modified ?? "—"}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        )}
      </div>

      {/* selection footer */}
      <div className="mt-2 flex items-center gap-2.5 rounded-lg border border-white/8 bg-black/40 px-3 py-2">
        <Disc3 className="h-4 w-4 shrink-0 text-[var(--accent)]" />
        <span className="font-mono2 truncate text-[11px] text-white/60">
          {selected ?? (mode === "dir" ? normalizePath(path) : "no file selected")}
        </span>
        {selected && mode === "file" && (
          <span className="chip ml-auto shrink-0 border-[color-mix(in_srgb,var(--accent)_40%,transparent)] bg-[color-mix(in_srgb,var(--accent)_10%,transparent)] text-[var(--accent)]">
            Container OK
          </span>
        )}
      </div>
    </div>
  );
}
