/* ============================================================
   GLUE360 — APP DATA
   Real pipeline stages + presentation metadata for known titles.
   Profiles themselves come from the C++ host (profile.toml is
   authoritative); GAME_META only decorates known profile ids
   with covers/descriptions. Unknown profiles get generated
   fallbacks, so new profiles Just Work.
   ============================================================ */
import type { Game, GameProfile } from "./types";

/* ------ the real pipeline: 7 stages, ids match the orchestrator ------ */
export const STAGES = [
  { id: "iso_extract", label: "Extract ISO" },
  { id: "rexglue_init", label: "Init Project" },
  { id: "rexglue_codegen", label: "PPC → C++ Codegen" },
  { id: "apply_patches", label: "Apply Patches" },
  { id: "build_runtime", label: "Runtime DLL" },
  { id: "build_game", label: "Compile Native" },
  { id: "deploy", label: "Deploy Standalone" },
] as const;

/** label list — used by progress UIs */
export const RECOMP_PHASES = STAGES.map((s) => s.label);

export function phaseIndexForStage(stageId: string): number {
  const i = STAGES.findIndex((s) => s.id === stageId);
  return i < 0 ? 0 : i;
}

/* ------ generated SVG covers (2:3). Honest placeholders — no fake box art. ------ */
function svgCover(lines: string[], hueA: number, hueB: number, mark: string): string {
  const text = lines
    .map(
      (l, i) =>
        `<text x="30" y="${430 + i * 34}" font-family="Segoe UI, Arial, sans-serif" font-size="${lines.length > 1 ? 26 : 30}" font-weight="800" letter-spacing="3" fill="white" opacity="0.96">${l}</text>`,
    )
    .join("");
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="360" height="540" viewBox="0 0 360 540">
<defs>
<linearGradient id="bg" x1="0" y1="0" x2="1" y2="1">
<stop offset="0" stop-color="hsl(${hueA},60%,14%)"/>
<stop offset="1" stop-color="hsl(${hueB},70%,7%)"/>
</linearGradient>
<radialGradient id="glow" cx="0.8" cy="0.15" r="1">
<stop offset="0" stop-color="hsl(${hueA},85%,45%)" stop-opacity="0.35"/>
<stop offset="1" stop-color="hsl(${hueA},85%,45%)" stop-opacity="0"/>
</radialGradient>
</defs>
<rect width="360" height="540" fill="url(#bg)"/>
<rect width="360" height="540" fill="url(#glow)"/>
<text x="182" y="330" font-family="Segoe UI Black, Arial Black, sans-serif" font-size="290" font-weight="900" fill="hsl(${hueA},80%,55%)" opacity="0.16" text-anchor="middle">${mark}</text>
<rect x="0" y="0" width="360" height="44" fill="black" opacity="0.35"/>
<text x="30" y="29" font-family="Segoe UI, Arial, sans-serif" font-size="13" font-weight="700" letter-spacing="4" fill="hsl(88,75%,55%)">XBOX 360</text>
<text x="252" y="29" font-family="Segoe UI, Arial, sans-serif" font-size="13" font-weight="700" letter-spacing="2" fill="white" opacity="0.5">RECOMP</text>
${text}
<rect x="30" y="${445 + lines.length * 34 - 30}" width="52" height="3" fill="hsl(${hueA},80%,55%)"/>
<rect x="0.5" y="0.5" width="359" height="539" fill="none" stroke="white" stroke-opacity="0.14"/>
</svg>`;
  return `data:image/svg+xml,${encodeURIComponent(svg)}`;
}

interface GameMeta {
  title: string;
  cover: string;
  description: string;
  tags: string[];
}

/** Presentation for known profile ids. Everything factual (RTV, FSR, 60 FPS…)
    matches the shipped profile cvars. */
export const GAME_META: Record<string, GameMeta> = {
  spiderman3: {
    title: "SPIDER-MAN 3",
    cover: svgCover(["SPIDER-MAN 3"], 0, 350, "S3"),
    description:
      "Treyarch's open-world web-slinger, statically recompiled to native x64. Runs the RTV render path with unclipped-draw CPU execution (50-60 FPS on AMD APUs), a custom runtime DLL with FSR upscaling and seven save-system fixes, 60 FPS unlock, FXAA, and max anisotropic filtering.",
    tags: ["D3D12", "RTV", "60 FPS", "FSR", "Custom Runtime"],
  },
  jurassic_hunted: {
    title: "JURASSIC: THE HUNTED",
    cover: svgCover(["JURASSIC:", "THE HUNTED"], 145, 100, "J"),
    description:
      "Cauldron's dinosaur survival shooter, recompiled to native x64. RTV render path preserves the gamma pipeline (ROV forces gamma off and renders too dark), 60 FPS unlock via 120 Hz vblank, D3D12 only.",
    tags: ["D3D12", "RTV", "60 FPS"],
  },
  spiderman_wos: {
    title: "SPIDER-MAN: WEB OF SHADOWS",
    cover: svgCover(["WEB OF", "SHADOWS"], 260, 320, "W"),
    description:
      "Shaba Games' symbiote-invasion brawler, recompiled to native x64 with 148 recovered function hints. RTV + unclipped-draw CPU execution fixes the EDRAM black-screen corruption; Skate3-style cvar set.",
    tags: ["D3D12", "RTV", "148 Fn Hints"],
  },
};

export function metaFor(profileId: string, profileName?: string): GameMeta {
  const known = GAME_META[profileId];
  if (known) return known;
  // Unknown profile → generated cover from the name, neutral copy.
  const name = (profileName || profileId).toUpperCase();
  let hash = 0;
  for (const c of profileId) hash = (hash * 31 + c.charCodeAt(0)) >>> 0;
  const hue = hash % 360;
  const words = name.split(/\s+/);
  const lines = words.length > 2 ? [words.slice(0, 2).join(" "), words.slice(2).join(" ")] : [name];
  return {
    title: name,
    cover: svgCover(lines, hue, (hue + 40) % 360, name[0] ?? "?"),
    description: `Recompilation profile "${profileId}". Run a build to produce a native standalone.`,
    tags: ["D3D12"],
  };
}

/** Library entry scaffold for a host profile that isn't in the library yet. */
export function gameFromProfile(p: GameProfile): Game {
  const meta = metaFor(p.id, p.name);
  return {
    id: p.id,
    source: "profile",
    title: meta.title,
    cover: meta.cover,
    isoPath: null,
    profileId: p.id,
    status: "not_compiled",
    lastPlayed: null,
    description: meta.description,
    tags: meta.tags,
    builds: [],
    deployDir: null,
    exePath: null,
    userDataDir: null,
    shaderCacheDir: null,
    launchError: null,
    titleId: p.titleId,
    addedAt: Date.now(),
  };
}

/* ------ dev-simulator fallback profiles (used ONLY when no C++ bridge) ------
   These mirror the real profiles/<id>/profile.toml files so `npm run dev`
   in a browser stays demoable. Production always uses host data. */
export const FALLBACK_PROFILES: GameProfile[] = [
  {
    id: "spiderman3",
    name: "Spider-Man 3 (Xbox 360)",
    titleId: "415607E2",
    sdkVersion: "0.8.0",
    runtimeFlags: [
      "REX_GAME_SAVE_SYSTEM_FIX",
      "REX_ROV_BARRIER_SKIP",
      "REX_QUEUEFRAMES_2",
      "REX_XAM_DISPATCH_HEADLESS",
      "REX_XAM_ENUM_IO_PENDING",
      "REX_XENUMERATOR_WRITEITEMS_ZERO",
      "REX_THINLTO",
    ],
    customRuntime: true,
    cvarCount: 22,
  },
  {
    id: "jurassic_hunted",
    name: "Jurassic: The Hunted (Xbox 360)",
    titleId: "41560870",
    sdkVersion: "0.8.0",
    runtimeFlags: [],
    customRuntime: false,
    cvarCount: 12,
  },
  {
    id: "spiderman_wos",
    name: "Spider-Man: Web of Shadows (Xbox 360)",
    titleId: "41560815",
    sdkVersion: "0.8.0",
    runtimeFlags: [],
    customRuntime: false,
    cvarCount: 14,
  },
];
