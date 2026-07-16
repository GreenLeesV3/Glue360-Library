# Xenon Deck — Integration Guide

GUI shell for an xbox360recomp pipeline. The UI is fully driven by mocks so it demos
end-to-end; **all real wiring happens in one file**.

## The one file to edit: `src/services/recompService.ts`

Every function is documented inline. Keep the exported signatures, replace the bodies:

| Function            | Real implementation                                                |
| ------------------- | ------------------------------------------------------------------ |
| `listProfiles()`    | Read your profile dir (`.toml` configs), map to `GameProfile`.     |
| `browseDirectory()` | Real fs bridge — Tauri `invoke('read_dir')`, or Electron `fs`.     |
| `startRecompile()`  | Spawn `XenonRecomp <config> <in.xex> <out>` as a child process.    |
| `cancelRecompile()` | Kill the child process.                                            |
| `launchGame()`      | Spawn the produced native binary from `settings.outputDir`.        |
| `stopGame()`        | Kill it.                                                           |

### Streaming progress back to the UI

While the child process runs, forward stdout into the event bus:

```ts
jobEvents.publish({ type: "log", id: jobId, line: { t: elapsedMs, level: "info", msg: line } });
jobEvents.publish({ type: "progress", id: jobId, progress: pct, phaseIndex });
jobEvents.publish({ type: "status", id: jobId, status: "done" }); // or "failed" / "cancelled"
```

Parse `%`-progress and phase boundaries from your toolchain output any way you like;
use `RECOMP_PHASES` (9 phases) as the canonical phase list.

## Data flow

```
recompService ──jobEvents──▶ useStore (_applyJobEvent) ──▶ React components
```

- The store (`src/store/useStore.ts`) persists `games`, `profiles`, `settings` to
  localStorage under the key `xenon-deck`. Clear it to re-seed from `src/lib/mockData.ts`.
- On a successful job, `commitBuild()` in the store either patches the targeted game
  (wizard opened from a game page) or creates a new library entry.

## Adding content

- **Games / profiles**: copy the `TEMPLATE` blocks at the top of the seed arrays in
  `src/lib/mockData.ts`.
- **Cover art**: drop images in `src/assets/covers/`, import them in `mockData.ts`,
  add to `COVER_POOL`. (They're inlined into the single-file build.)
- **Reskin**: accent switcher in System blade sets `--accent` at runtime; every colored
  surface uses `color-mix()` against that var, or edit tokens in `src/index.css`.

## Types

`src/lib/types.ts` is the contract. Extend here first, then update shapes in the
service + seeds.
