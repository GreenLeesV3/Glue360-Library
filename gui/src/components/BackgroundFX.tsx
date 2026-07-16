/* ---------- ambient backdrop: xbox dashboard curves/glow ---------- */

const NOISE_URI =
  "url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' width='140' height='140'%3E%3Cfilter id='n'%3E%3CfeTurbulence type='fractalNoise' baseFrequency='0.85' numOctaves='2'/%3E%3C/filter%3E%3Crect width='100%25' height='100%25' filter='url(%23n)' opacity='0.5'/%3E%3C/svg%3E\")";

export default function BackgroundFX() {
  return (
    <div className="pointer-events-none fixed inset-0 z-0 overflow-hidden" aria-hidden>
      {/* base wash */}
      <div
        className="absolute inset-0"
        style={{
          background:
            "radial-gradient(1200px 700px at 18% -10%, color-mix(in srgb, var(--accent) 13%, transparent), transparent 60%), radial-gradient(900px 600px at 110% 110%, rgba(70,120,30,0.12), transparent 55%), linear-gradient(180deg, #0b0f09 0%, #070907 55%, #050604 100%)",
        }}
      />

      {/* sweeping dashboard curves */}
      <svg
        className="anim-drift absolute -top-[15%] -left-[8%] h-[85%] w-[120%] opacity-70"
        viewBox="0 0 1600 900"
        fill="none"
        preserveAspectRatio="xMidYMin slice"
      >
        <defs>
          <linearGradient id="curv" x1="0" y1="0" x2="1" y2="1">
            <stop offset="0%" stopColor="var(--accent)" stopOpacity="0.55" />
            <stop offset="60%" stopColor="var(--accent)" stopOpacity="0.12" />
            <stop offset="100%" stopColor="var(--accent)" stopOpacity="0" />
          </linearGradient>
        </defs>
        <path
          d="M-100,720 C300,640 420,120 950,60 C1250,24 1420,90 1700,-40"
          stroke="url(#curv)"
          strokeWidth="2.5"
        />
        <path
          d="M-100,780 C360,700 500,200 1050,130 C1330,94 1440,150 1700,40"
          stroke="url(#curv)"
          strokeWidth="1.4"
          opacity="0.7"
        />
      </svg>

      {/* power-ring motif, bottom right */}
      <svg
        className="anim-drift absolute -right-[14%] -bottom-[28%] h-[70%] w-[70%] opacity-[0.10]"
        style={{ animationDelay: "-6s" }}
        viewBox="0 0 600 600"
        fill="none"
      >
        <circle cx="300" cy="300" r="290" stroke="var(--accent)" strokeWidth="1.5" />
        <circle cx="300" cy="300" r="210" stroke="var(--accent)" strokeWidth="1" strokeDasharray="4 10" />
        <circle cx="300" cy="300" r="130" stroke="white" strokeOpacity="0.5" strokeWidth="0.75" />
      </svg>

      {/* faint grid */}
      <div
        className="absolute inset-0 opacity-[0.05]"
        style={{
          backgroundImage:
            "linear-gradient(rgba(255,255,255,0.5) 1px, transparent 1px), linear-gradient(90deg, rgba(255,255,255,0.5) 1px, transparent 1px)",
          backgroundSize: "72px 72px",
          maskImage: "radial-gradient(80% 70% at 50% 30%, black, transparent)",
        }}
      />

      {/* film grain + vignette */}
      <div className="absolute inset-0 opacity-[0.05] mix-blend-overlay" style={{ backgroundImage: NOISE_URI }} />
      <div
        className="absolute inset-0"
        style={{ background: "radial-gradient(120% 90% at 50% 40%, transparent 55%, rgba(0,0,0,0.55))" }}
      />
    </div>
  );
}
