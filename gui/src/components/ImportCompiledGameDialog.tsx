import { useEffect, useState, type ReactNode } from "react";
import { motion } from "framer-motion";
import { FolderOpen, Gamepad2, HardDrive, Import, Save, Sparkles, X } from "lucide-react";
import { useStore } from "../store/useStore";
import { hasBridge, pickDir, pickExe } from "../services/recompService";
import { joinWindowsPath, parentWindowsPath, windowsFileStem } from "../lib/format";
import { GhostBtn, PrimaryBtn } from "./ui";

interface Props {
  onClose: () => void;
}

export default function ImportCompiledGameDialog({ onClose }: Props) {
  const importCompiledGame = useStore((s) => s.importCompiledGame);
  const [title, setTitle] = useState("");
  const [gameDir, setGameDir] = useState("");
  const [exePath, setExePath] = useState("");
  const exeDir = parentWindowsPath(exePath) || gameDir;
  const userDataDir = exeDir ? joinWindowsPath(exeDir, "user_data") : "";
  const shaderCacheDir = userDataDir ? joinWindowsPath(userDataDir, "cache\\shaders") : "";
  const canImport = title.trim().length > 0 && gameDir.length > 0 && exePath.length > 0;

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape") onClose();
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, [onClose]);

  const browseFolder = async () => {
    const path = await pickDir("Select the compiled game's folder");
    if (path) setGameDir(path);
  };

  const browseExecutable = async () => {
    const path = await pickExe();
    if (!path) return;
    setExePath(path);
    const parent = parentWindowsPath(path);
    if (parent) setGameDir(parent);
    if (!title.trim()) setTitle(windowsFileStem(path));
  };

  const submit = () => {
    if (!canImport) return;
    importCompiledGame({ title: title.trim(), gameDir, exePath });
    onClose();
  };

  return (
    <motion.div
      initial={{ opacity: 0 }}
      animate={{ opacity: 1 }}
      exit={{ opacity: 0 }}
      className="fixed inset-0 z-50 flex items-center justify-center p-6"
    >
      <button aria-label="Close import dialog" className="absolute inset-0 bg-black/75 backdrop-blur-sm" onClick={onClose} />
      <motion.div
        initial={{ opacity: 0, y: 24, scale: 0.97 }}
        animate={{ opacity: 1, y: 0, scale: 1 }}
        exit={{ opacity: 0, y: 16, scale: 0.98 }}
        className="panel relative w-[min(720px,96vw)] overflow-hidden rounded-2xl shadow-[0_40px_120px_-20px_rgba(0,0,0,0.9)]"
      >
        <div className="flex items-center justify-between border-b border-white/8 px-6 py-4">
          <div className="flex items-center gap-3">
            <span className="flex h-8 w-8 items-center justify-center rounded-lg bg-[var(--accent)] text-black">
              <Import className="h-4 w-4" strokeWidth={2.5} />
            </span>
            <div>
              <h2 className="font-display text-[15px] font-bold tracking-[0.2em] text-white uppercase">
                Import Compiled Game
              </h2>
              <p className="text-[10.5px] tracking-[0.12em] text-white/40 uppercase">
                Add an existing native build to this library
              </p>
            </div>
          </div>
          <GhostBtn className="!h-9 !px-2.5" onClick={onClose}><X className="h-4 w-4" /></GhostBtn>
        </div>

        <div className="flex flex-col gap-5 p-6">
          <PathInput
            icon={<Gamepad2 className="h-4 w-4" />}
            label="Library Name"
            value={title}
            placeholder="e.g. Spider-Man 3"
            onChange={setTitle}
          />
          <PathInput
            icon={<HardDrive className="h-4 w-4" />}
            label="Game Folder"
            value={gameDir}
            placeholder="Folder used as the game's working directory"
            onChange={setGameDir}
            onBrowse={hasBridge ? browseFolder : undefined}
          />
          <PathInput
            icon={<Gamepad2 className="h-4 w-4" />}
            label="Game Executable"
            value={exePath}
            placeholder="Full path to the compiled .exe"
            onChange={setExePath}
            onBrowse={hasBridge ? browseExecutable : undefined}
          />

          <div className="rounded-xl border border-white/8 bg-white/3 p-4">
            <div className="mb-3 text-[10px] font-bold tracking-[0.2em] text-white/40 uppercase">
              Portable runtime locations
            </div>
            <LocationRow icon={<Save className="h-3.5 w-3.5" />} label="User data / saves" value={userDataDir || "Select an executable"} />
            <LocationRow icon={<Sparkles className="h-3.5 w-3.5" />} label="Shader cache" value={shaderCacheDir || "Select an executable"} />
            <p className="mt-3 text-[10.5px] leading-relaxed text-white/30">
              RexGlue stores both beside the executable. Shader files may appear under the cache&apos;s shareable folder.
            </p>
          </div>
        </div>

        <div className="flex items-center justify-end gap-3 border-t border-white/8 px-6 py-4">
          <GhostBtn onClick={onClose}>Cancel</GhostBtn>
          <PrimaryBtn onClick={submit} disabled={!canImport}>
            <Import className="h-4 w-4" /> Add to Library
          </PrimaryBtn>
        </div>
      </motion.div>
    </motion.div>
  );
}

function PathInput({
  icon,
  label,
  value,
  placeholder,
  onChange,
  onBrowse,
}: {
  icon: ReactNode;
  label: string;
  value: string;
  placeholder: string;
  onChange: (value: string) => void;
  onBrowse?: () => Promise<void>;
}) {
  return (
    <div>
      <label className="mb-1.5 flex items-center gap-2 text-[10px] font-bold tracking-[0.18em] text-white/45 uppercase">
        <span className="text-[var(--accent)]">{icon}</span> {label}
      </label>
      <div className="flex min-w-0 items-center gap-2">
        <input
          value={value}
          onChange={(event) => onChange(event.target.value)}
          placeholder={placeholder}
          className="field font-mono2 min-w-0 flex-1 !text-[11.5px]"
          spellCheck={false}
        />
        {onBrowse && (
          <GhostBtn className="!h-11 shrink-0 !px-4" onClick={() => void onBrowse()}>
            <FolderOpen className="h-4 w-4" /> Browse
          </GhostBtn>
        )}
      </div>
    </div>
  );
}

function LocationRow({ icon, label, value }: { icon: ReactNode; label: string; value: string }) {
  return (
    <div className="mt-2 flex items-center gap-2.5 rounded-lg bg-black/20 px-3 py-2">
      <span className="text-[var(--accent)]">{icon}</span>
      <span className="w-32 shrink-0 text-[10px] font-bold tracking-[0.12em] text-white/40 uppercase">{label}</span>
      <span className="font-mono2 min-w-0 flex-1 truncate text-[10.5px] text-white/65">{value}</span>
    </div>
  );
}
