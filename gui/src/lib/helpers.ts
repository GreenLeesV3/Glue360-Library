import type { GameStatus } from "./types";

/** sort priority for the library rail */
export const GAME_ORDER_HINT: Record<GameStatus, number> = {
  running: 0,
  recompiling: 1,
  queued: 2,
  recompiled: 3,
  failed: 4,
  not_compiled: 5,
};
