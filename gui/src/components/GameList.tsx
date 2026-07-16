/* ---------- steam-style library rail (left column) ---------- */
import { AnimatePresence, motion } from "framer-motion";
import { Import, Search } from "lucide-react";
import { useMemo, useState } from "react";
import { cn } from "../utils/cn";
import { useStore } from "../store/useStore";
import { GAME_ORDER_HINT } from "../lib/helpers";
import { StatusChip } from "./ui";
import ImportCompiledGameDialog from "./ImportCompiledGameDialog";

export default function GameList() {
  const games = useStore((s) => s.games);
  const selectedId = useStore((s) => s.selectedGameId);
  const selectGame = useStore((s) => s.selectGame);
  const [query, setQuery] = useState("");
  const [importOpen, setImportOpen] = useState(false);

  const filtered = useMemo(() => {
    const sorted = [...games].sort(
      (a, b) => GAME_ORDER_HINT[a.status] - GAME_ORDER_HINT[b.status] || b.addedAt - a.addedAt,
    );
    if (!query.trim()) return sorted;
    return sorted.filter((g) => g.title.toLowerCase().includes(query.toLowerCase()));
  }, [games, query]);

  return (
    <>
    <aside className="panel flex w-[300px] shrink-0 flex-col overflow-hidden rounded-2xl">
      <div className="border-b border-white/8 p-3">
        <div className="relative">
          <Search className="absolute top-1/2 left-3 h-4 w-4 -translate-y-1/2 text-white/35" />
          <input
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            placeholder="Search library…"
            className="field !py-2 !pl-9 text-[13px]"
          />
        </div>
        <div className="mt-2.5 flex items-center justify-between px-1">
          <span className="text-[10px] font-bold tracking-[0.2em] text-white/40 uppercase">
            {games.length} Titles
          </span>
          <span className="text-[10px] font-bold tracking-[0.2em] text-[var(--accent)] uppercase">
            {games.filter((g) => g.status === "recompiled" || g.status === "running").length} Ready
          </span>
        </div>
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto p-2">
        {filtered.map((g, i) => {
          const active = g.id === selectedId;
          return (
            <motion.button
              key={g.id}
              initial={{ opacity: 0, x: -18 }}
              animate={{ opacity: 1, x: 0 }}
              transition={{ delay: i * 0.035, duration: 0.25 }}
              onClick={() => selectGame(g.id)}
              className={cn(
                "group relative mb-1 flex w-full items-center gap-3 overflow-hidden rounded-xl px-3 py-2.5 text-left transition-colors",
                active ? "bg-white/8" : "hover:bg-white/4",
              )}
            >
              {/* selection edge */}
              {active && (
                <motion.span
                  layoutId="rail-edge"
                  className="absolute top-2 bottom-2 left-0 w-[3px] rounded-full bg-[var(--accent)] shadow-[0_0_12px_color-mix(in_srgb,var(--accent)_70%,transparent)]"
                />
              )}
              {/* sheen */}
              <span className="pointer-events-none absolute inset-y-0 w-16 bg-gradient-to-r from-transparent via-white/10 to-transparent opacity-0 transition-opacity group-hover:opacity-100" style={{ animation: "shimmer 2.6s linear infinite" }} />

              <img
                src={g.cover}
                alt=""
                className={cn(
                  "h-11 w-8 shrink-0 rounded-md object-cover ring-1 ring-white/10 transition",
                  active ? "shadow-[0_4px_14px_rgba(0,0,0,0.5)]" : "opacity-80 group-hover:opacity-100",
                )}
              />
              <span className="min-w-0 flex-1">
                <span
                  className={cn(
                    "font-display block truncate text-[13px] font-bold tracking-[0.1em] uppercase",
                    active ? "text-white" : "text-white/72 group-hover:text-white/95",
                  )}
                >
                  {g.title}
                </span>
                <span className="mt-0.5 block">
                  <StatusChip status={g.status} className="!px-2 !py-[1px] !text-[9px]" />
                </span>
              </span>
            </motion.button>
          );
        })}
        {filtered.length === 0 && (
          <div className="p-6 text-center text-xs text-white/35">No titles match “{query}”.</div>
        )}
      </div>
      <div className="border-t border-white/8 p-2">
        <button
          onClick={() => setImportOpen(true)}
          className="flex w-full items-center justify-center gap-2 rounded-xl border border-white/8 px-3 py-2.5 text-[10px] font-bold tracking-[0.16em] text-white/50 uppercase transition-colors hover:border-white/15 hover:bg-white/5 hover:text-white/80"
        >
          <Import className="h-3.5 w-3.5 text-[var(--accent)]" /> Import Compiled Game
        </button>
      </div>
    </aside>
      <AnimatePresence>
        {importOpen && <ImportCompiledGameDialog onClose={() => setImportOpen(false)} />}
      </AnimatePresence>
    </>
  );
}
