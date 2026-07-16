/* ============================================================
   XENON DECK — Xbox 360 Recompile Station
   Layout:  BackgroundFX < TopBar (blades) < View < ControllerHints
   Views:   library | queue | system   (+ wizard overlay)
   ============================================================ */
import { useEffect } from "react";
import { AnimatePresence, motion } from "framer-motion";
import { Square } from "lucide-react";
import { useStore } from "./store/useStore";
import BackgroundFX from "./components/BackgroundFX";
import TopBar from "./components/TopBar";
import ControllerHints from "./components/ControllerHints";
import GameList from "./components/GameList";
import GameDetail from "./components/GameDetail";
import QueueView from "./components/QueueView";
import SystemView from "./components/SystemView";
import AddGameWizard from "./components/AddGameWizard";

const ACCENT_HEX: Record<string, string> = {
  green: "#9ce318",
  crimson: "#ff4757",
  frost: "#38c2ff",
  violet: "#a78bfa",
};

export default function App() {
  const view = useStore((s) => s.view);
  const accent = useStore((s) => s.settings.accent);
  const boot = useStore((s) => s.boot);

  // sync profiles/settings/toolchain from the C++ host (or dev seeds)
  useEffect(() => {
    void boot();
  }, [boot]);

  // runtime re-skin: one CSS var drives every accent surface
  useEffect(() => {
    document.documentElement.style.setProperty("--accent", ACCENT_HEX[accent] ?? ACCENT_HEX.green);
  }, [accent]);

  return (
    <div className="relative flex h-full w-full flex-col overflow-hidden">
      <BackgroundFX />
      <TopBar />

      <main className="relative z-10 flex min-h-0 flex-1 px-6 pb-6">
        <AnimatePresence mode="wait">
          <motion.div
            key={view}
            initial={{ opacity: 0, y: 16 }}
            animate={{ opacity: 1, y: 0 }}
            exit={{ opacity: 0, y: -10 }}
            transition={{ duration: 0.24, ease: [0.2, 0.8, 0.2, 1] }}
            className="flex min-h-0 flex-1 gap-5"
          >
            {view === "library" && (
              <>
                <GameList />
                <GameDetail />
              </>
            )}
            {view === "queue" && <QueueView />}
            {view === "system" && <SystemView />}
          </motion.div>
        </AnimatePresence>
      </main>

      <NowPlayingDock />
      <ControllerHints />
      <AddGameWizard />
    </div>
  );
}

/* floating "now playing" indicator, bottom-left */
function NowPlayingDock() {
  const games = useStore((s) => s.games);
  const stopGameById = useStore((s) => s.stopGameById);
  const running = games.find((g) => g.status === "running");

  if (!running) return null;
  return (
    <motion.div
      initial={{ opacity: 0, y: 20, x: -8 }}
      animate={{ opacity: 1, y: 0, x: 0 }}
      exit={{ opacity: 0, y: 20 }}
      className="panel fixed bottom-5 left-6 z-30 flex items-center gap-3 rounded-full py-2 pr-2 pl-2.5 shadow-[0_16px_40px_rgba(0,0,0,0.6)]"
    >
      <img src={running.cover} alt="" className="h-9 w-7 rounded-md object-cover ring-1 ring-white/15" />
      <span className="mr-1">
        <span className="flex items-center gap-1.5 text-[9px] font-bold tracking-[0.22em] text-sky-300 uppercase">
          <span className="anim-blink h-1.5 w-1.5 rounded-full bg-sky-400" /> Now Playing
        </span>
        <span className="font-display block max-w-40 truncate text-[12px] font-bold tracking-[0.1em] text-white uppercase">
          {running.title}
        </span>
      </span>
      <button
        onClick={() => stopGameById(running.id)}
        className="btn btn-danger h-8 px-3 text-[10px]"
        title="Stop game"
      >
        <Square className="h-3 w-3 fill-current" />
        Quit
      </button>
    </motion.div>
  );
}
