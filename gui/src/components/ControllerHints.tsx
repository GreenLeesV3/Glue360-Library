/* ---------- xbox 360 controller face-button legend ---------- */
import { cn } from "../utils/cn";

const BUTTONS = [
  { key: "A", label: "Select", color: "#7ec234", ring: "ring-[#7ec234]/40" },
  { key: "B", label: "Back", color: "#e5484d", ring: "ring-[#e5484d]/40" },
  { key: "X", label: "Details", color: "#3e7bc4", ring: "ring-[#3e7bc4]/40" },
  { key: "Y", label: "Recompile", color: "#e8b340", ring: "ring-[#e8b340]/40" },
];

export default function ControllerHints() {
  return (
    <div className="pointer-events-none fixed right-6 bottom-5 z-30 flex items-center gap-4 rounded-full border border-white/10 bg-black/40 px-5 py-2.5 backdrop-blur-md">
      {BUTTONS.map((b) => (
        <span key={b.key} className="flex items-center gap-1.5">
          <span
            className={cn(
              "flex h-[18px] w-[18px] items-center justify-center rounded-full text-[10px] font-black text-black ring-2",
              b.ring,
            )}
            style={{ backgroundColor: b.color }}
          >
            {b.key}
          </span>
          <span className="text-[10px] font-semibold tracking-[0.14em] text-white/45 uppercase">
            {b.label}
          </span>
        </span>
      ))}
    </div>
  );
}
