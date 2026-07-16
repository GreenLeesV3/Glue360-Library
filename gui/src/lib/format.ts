/* tiny formatting helpers shared across the UI */

export function fmtBytes(bytes?: number): string {
  if (bytes == null) return "—";
  const gb = bytes / 1_073_741_824;
  if (gb >= 0.95) return `${gb.toFixed(2)} GB`;
  return `${(bytes / 1_048_576).toFixed(1)} MB`;
}

export function fmtDuration(sec: number): string {
  if (sec < 60) return `${Math.floor(sec)}s`;
  const m = Math.floor(sec / 60);
  const s = Math.floor(sec % 60);
  return `${m}m ${s.toString().padStart(2, "0")}s`;
}

export function fmtClock(ts: number): string {
  const d = new Date(ts);
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`;
}

export function fmtLogTime(ms: number): string {
  const s = Math.floor(ms / 1000);
  const m = Math.floor(s / 60);
  return `${String(m).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}.${String(
    Math.floor((ms % 1000) / 100),
  )}`;
}

export function timeAgo(ts: number | null): string {
  if (!ts) return "Never";
  const diff = Date.now() - ts;
  const h = diff / 3_600_000;
  if (h < 1) return "Just now";
  if (h < 24) return `${Math.floor(h)}h ago`;
  const d = Math.floor(h / 24);
  if (d === 1) return "Yesterday";
  if (d < 30) return `${d}d ago`;
  return new Date(ts).toLocaleDateString();
}

export function fmtPlayTime(hours: number): string {
  if (hours === 0) return "—";
  if (hours < 1) return `${Math.round(hours * 60)}m`;
  return `${hours.toFixed(1)}h`;
}

/** short stable id */
export function uid(prefix = "id"): string {
  return `${prefix}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
}

/** Remove characters Windows forbids in directory names. */
export function safeWindowsFolderName(name: string): string {
  const cleaned = name
    .replace(/[<>:"/\\|?*\u0000-\u001f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/[. ]+$/, "");
  const fallback = cleaned || "Xbox 360 Game";
  const base = fallback.split(".")[0]?.toUpperCase();
  return /^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])$/.test(base) ? `_${fallback}` : fallback;
}

export function joinWindowsPath(parent: string, child: string): string {
  const root = parent.replace(/[\\/]+$/, "");
  return root ? `${root}\\${child}` : child;
}

export function parentWindowsPath(path: string): string {
  const normalized = path.replace(/\//g, "\\").replace(/\\+$/, "");
  const split = normalized.lastIndexOf("\\");
  return split > 0 ? normalized.slice(0, split) : "";
}

export function windowsFileStem(path: string): string {
  const normalized = path.replace(/\//g, "\\");
  const name = normalized.slice(normalized.lastIndexOf("\\") + 1);
  return name.replace(/\.exe$/i, "") || "Imported Game";
}
