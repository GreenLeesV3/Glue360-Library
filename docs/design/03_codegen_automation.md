# Codegen Automation Design — RexGlue360 Recompiler Pipeline

**Scope:** Automate the pipeline from a user-supplied Spider-Man 3 (Xbox 360) ISO
to a CMake project ready for `cmake --build`. This covers: ISO extraction, rexglue
`init`/`codegen`, **funcid rename re-application**, hand-authored `src/` file
preservation, incremental rebuild detection, and game identification. The
downstream custom-runtime build, SDK source patches, ThinLTO, and TOML config
generation are covered by the PatchSystem agent's separate design.

**Status:** Design only. No code written.

**Grounded in:** Actual project files at `C:\tmp\Workspace 1\RexGlue360Recomp\`,
`DOCS/BUILD_GUIDE.md`, `DOCS/FILE_INVENTORY.md`, `DOCS/LESSONS_LEARNED.md`, the
`funcid/` rename system, and verified `rexglue --help` output.

---

## 0. The True Pipeline (Corrected)

The manual pipeline is **not** "codegen then patch." It is:

```
ISO → extract → rexglue init → rexglue codegen → funcid rename → preserve/inject src/ → build
                                        │              │
                                   writes generated/    rewrites generated/
                                   (sub_XXXXXXXX)       (semantic names)
```

**Three critical facts that shape the design:**

1. **`src/` is never touched by codegen.** `rexglue init` creates `src/main.cpp`
   and `src/roundevenf.cpp` as a skeleton. Codegen writes only to
   `generated/default/`. The hand-authored files (`spiderman3_app.h`,
   `xmp_bypass.cpp`, `particle_perf.cpp`) are added to `src/` once and must be
   **preserved** across re-codegen — they are not "patches" applied to generated
   code. (LESSONS_LEARNED §3.5: "All game-specific behavior goes in `src/` via
   hooks, or in the rename map for naming. Treat `generated/` as a build
   artifact.")

2. **Funcid rename is a mandatory post-codegen step.** Codegen emits
   `sub_XXXXXXXX` symbol names (where XXXXXXXX is the original PPC address). The
   funcid pipeline (`funcid_06_rename.py` + `funcid_06_rename_map_fixed.csv`)
   applies 18,871 semantic renames — 129,634 text replacements across 95 files.
   **The hook targets in `xmp_bypass.cpp` are themselves renamed symbols:**
   `XMPGetStatus_Wrapper` (was `sub_829BA2C0`),
   `XamShowDeviceSelectorUI_Wrapper` (was `sub_82A18160`),
   `XamContentGetDeviceState_Wrapper` (was `sub_82A183B0`).
   `REX_HOOK_RAW(XMPGetStatus_Wrapper)` resolves against a symbol that **only
   exists after** `funcid_06_rename.py` runs. Re-codegen regenerates
   `generated/*.cpp` + `spiderman3_init.h` with `sub_XXXXXXXX` names, orphaning
   every hook — the game silently hangs at boot (XMP/storage/content calls
   unhandled). **Any re-codegen REQUIRES re-applying the rename map before
   build.**

3. **The `spiderman3_codegen` CMake target is `add_custom_target` (always-runs,
   no `OUTPUT`/`DEPENDS`).** CMake gives no incremental skip for free. The
   automation must detect re-codegen necessity via a stored hash/mtime of
   `extracted/default.xex` against a stamp file.

---

## 1. ISO Extraction

### 1.1 Format Detection

Xbox 360 ISOs use the **XDVDFS** (aka `XTAF`) filesystem, not ISO9660. XGD2/XGD3
ISOs have a video partition at offset 0 (the "security sector" ring) that is
all-zero bytes, followed by the **game partition** at a fixed offset.

**Observed in our ISO** (`Spider-Man 3 (USA, Europe).iso`, 7,835,492,352 bytes = 7.3 GiB):

| Offset | Content | Detection |
|--------|---------|-----------|
| `0x00000` | All zeros (video partition placeholder) | First 32KB is zero → likely XGD |
| `0x20800` | XDVDFS volume descriptor | Root dir entry (`0x08 0x00 0x02 0x00`) |
| `0xFD00000` | XGD3 video layer (absent in XGD2) | — |

**Detection algorithm:**

```
function detectXboxIso(path):
  size = fileSize(path)
  head = read(path, 0, 0x10000)
  if head is not all-zeros:
    // Could be a stripped ISO (extract-xiso output) or GOD container
    if isXdvdfsVolumeDescriptor(read(path, 0x10000, 0x800)):
      return { format: "XDVDFS-stripped", gamePartitionOffset: 0x10000 }
    return { format: "unknown" }

  // All-zero head → XGD2 or XGD3. Probe known game partition offsets.
  for offset in [0x20800, 0x10000, 0xFD00000]:
    if isXdvdfsVolumeDescriptor(read(path, offset, 0x800)):
      return { format: "XGD2" if offset < 0x1000000 else "XGD3",
               gamePartitionOffset: offset }

  // Fallback: scan in 0x800 strides for XDVDFS magic
  for offset in range(0x10000, min(size, 0x2000000), 0x800):
    if isXdvdfsVolumeDescriptor(read(path, offset, 0x800)):
      return { format: "XGD2", gamePartitionOffset: offset }

  return { format: "unknown" }
```

**XDVDFS volume descriptor validation:**
- Bytes `0x00–0x03`: root directory entry — sector number (big-endian, shifted left 11 bits to get byte offset)
- Byte `0x04`: attributes (directory flag = `0x80`)
- Validated by checking the root entry points to a readable directory table

### 1.2 Extraction Tool

**Recommended: `extract-xiso`** (https://github.com/XboxDev/extract-xiso)

- **License:** BSD-3-Clause — redistributable, can be bundled
- **Prebuilt binaries:** Windows, Linux, macOS builds on GitHub Releases (latest: `build-202505152050`, 8 assets including Windows x64)
- **Handles XGD2/XGD3 natively:** `-x` flag auto-detects the game partition
- **Single binary, no dependencies** — ideal for bundling

**Command:**
```
extract-xiso -x "Spider-Man 3 (USA, Europe).iso" -d "extracted/"
```

**Bundling strategy:** Ship `extract-xiso.exe` in the app's `tools/` directory (~200KB). BSD-3-Clause licensed, legal to redistribute in a GitHub release.

### 1.3 Extraction Output Location

The project uses a **fixed relative layout** (verified from manifest and BUILD_GUIDE):

```
<workspace>/
  extracted/              ← ISO extraction target (BUILD_GUIDE: "<SDK_ROOT>\extracted\")
    default.xex           ← entrypoint (14,069,760 bytes)
    amalga.toc            ← asset table of contents (103,400 bytes)
    $SystemUpdate/        ← system update partition (ignored by game)
    movies/               ← FMV video files (.wmv/.wma)
    packs/                ← 1,228 .XEPACK asset packs
    sound/                ← 28 .xessb + en/ voice packs
  spiderman3/             ← rexglue project (created by init)
    spiderman3_manifest.toml
    CMakeLists.txt
    generated/default/
    src/
```

The manifest's `game_root = "../extracted"` and `entrypoint.file_path = "../extracted/default.xex"` are **relative paths from the project directory**. The automation must:
1. Extract ISO → `<workspace>/extracted/`
2. Run `rexglue init` with `--project-root <workspace>/spiderman3` so `../extracted` resolves correctly
3. Verify `extracted/default.xex` exists post-extraction before proceeding

### 1.4 Extraction Failure Modes

| Failure | Detection | Recovery |
|---------|-----------|----------|
| Not an Xbox ISO | Format detection returns `unknown` | Error: "This does not appear to be an Xbox 360 ISO" |
| Corrupted ISO | extract-xiso exits non-zero or `default.xex` missing | Error: report which files failed |
| XGD3 (wrong format) | Game partition at `0xFD00000` | Warn: "XGD3 detected — Spider-Man 3 is XGD2. Wrong game?" |
| Partial extraction | File count < expected | Re-run or report missing files |
| Disk space | Pre-check: free space >= 8GB | Error before starting |

---

## 2. RexGlue Init

### 2.1 Command (Verified Flags)

**IMPORTANT:** The BUILD_GUIDE shows `rexglue init --name spiderman3 --xex extracted\default.xex` — this is **stale**. The actual CLI (verified from `rexglue init --help`) uses `--project-name` and `--xex-path`:

```
rexglue init \
  --project-name spiderman3 \
  --xex-path "../extracted/default.xex" \
  --game-root "../extracted" \
  --project-root "<workspace>/spiderman3"
```

**Parameters (from `rexglue init --help`):**
- `--project-name` (REQUIRED): Becomes `[project].name` in manifest, CMake target name
- `--xex-path` (REQUIRED): Path to entrypoint XEX — parsed to extract function list, image base, code bounds
- `--game-root` (OPTIONAL): Game asset root for DLL guest-path derivation
- `--project-root` (OPTIONAL): Where to create the project (defaults to cwd)
- `--scan-dll` (OPTIONAL): Scans `--game-root` for `.dll` files, adds each as a `[[modules]]` manifest entry
- `--template-dir` (OPTIONAL): Custom template directory for overrides

**For Spider-Man 3:** No DLL modules (single `default.xex` entrypoint). Do NOT pass `--scan-dll`.

### 2.2 Manifest File Generated

`rexglue init` produces `<project-root>/<project-name>_manifest.toml`:

```toml
# Generated by ReXGlue v0.8.0 on 2026-07-06 04:25:15 UTC.
[project]
name = "spiderman3"
sdk_version = "0.8.0"
game_root = "../extracted"

[entrypoint]
file_path = "../extracted/default.xex"
out_directory_path = "generated/default"
includes = []

[entrypoint.functions.0x82967D40]
name = "sub_82967D40"
# ... (9 entrypoint function hints)
```

**Key observations from the actual manifest (38 lines):**
- `[project]` section: name, SDK version, game_root (relative path)
- `[entrypoint]` section: XEX path, output directory, includes list
- `[entrypoint.functions.<addr>]` sections: **9 entrypoint hints** — PPC virtual addresses in the XEX's code section (`0x82280000`–`0x82BECE1C`) that mark functions the recompiler should treat as analysis roots. Only 9 entries — the bulk of 43,676 functions are auto-discovered during codegen, not listed in the manifest.
- The manifest is small (38 lines) and stable

### 2.3 Title ID Detection

The XEX header contains the Xbox 360 title ID. Parsing from `default.xex`:

**XEX2 header structure** (verified from our `default.xex`):
```
0x00: magic "XEX2" (4 bytes)
0x04: version (uint32 BE) = 1
0x08: flags (uint32 BE) = 0x3000
0x0C: reserved = 0
0x10: header_size (uint32 BE) = 0x90 (144 bytes)
0x14: optional_header_count (uint32 BE) = 15 (0xF)
0x18+: optional header entries (8 bytes each: 4-byte ID + 4-byte value/offset)
```

**Title ID location:** The `LICENCE_INFO` optional header (ID `0x00040006`, value = offset `0x1C48`). At that offset, the title ID appears at bytes +12:

```
0x1C48: 5e 75 39 88 00 00 00 01 00 00 00 01 41 56 07 e2
                                                  ^^^^^^^^^^
                                                  Title ID = 415607E2
```

This is the **PAL** Spider-Man 3 title ID. Also confirmed as the Tracy trace filename (`415607E2_stream.xtr`).

**Parsing code:**
```python
def extract_title_id(xex_path):
    with open(xex_path, 'rb') as f:
        data = f.read(0x2000)
    count = struct.unpack('>I', data[0x14:0x18])[0]
    off = 0x18
    for i in range(count):
        hdr_id = struct.unpack('>I', data[off:off+4])[0]
        hdr_val = struct.unpack('>I', data[off+4:off+8])[0]
        if hdr_id == 0x00040006:  # LICENCE_INFO
            title_id = struct.unpack('>I', data[hdr_val+12:hdr_val+16])[0]
            return f"{title_id:08X}"
        off += 8
    return None
```

**Known Spider-Man 3 title IDs:**

| Region | Title ID | Notes |
|--------|----------|-------|
| NTSC (USA) | `415607D2` | Most common |
| PAL (Europe) | `415607E2` | Our ISO |
| PAL (Asia) | `415607F2` | Rare |

### 2.4 Media ID (Secondary Fingerprint)

The `MEDIA_ID` optional header (ID `0x00040404`, value = offset `0x1CA0`) provides a per-pressing fingerprint:

```
Media ID: b3cca11553bbcb95405cb413045d0c40000006c8
```

This distinguishes different pressings of the same title ID.

---

## 3. RexGlue Codegen

### 3.1 Command

```
rexglue codegen spiderman3_manifest.toml
```

Or, if the manifest is in the current directory:
```
rexglue codegen
```

**Parameters (from `rexglue codegen --help`):**
- `config` (POSITIONAL, optional): Path to manifest TOML. Auto-discovered in cwd if omitted
- `--target NAME` (repeatable): DLL target to build. Entrypoint always included. Not needed for Spider-Man 3

**CMake integration:** The generated `rexglue.cmake` defines a custom target:
```cmake
add_custom_target(spiderman3_codegen
    COMMAND $<TARGET_FILE:rex::rexglue> codegen ${CMAKE_CURRENT_SOURCE_DIR}/spiderman3_manifest.toml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating recompiled code for spiderman3"
    VERBATIM
)
```

**⚠️ This is `add_custom_target`, not `add_custom_command`.** It has no `OUTPUT` or `DEPENDS` — CMake runs it every time `cmake --build . --target spiderman3_codegen` is invoked. There is no built-in incremental skip. The automation must implement its own skip logic (§6).

### 3.2 Inputs and Outputs

**Inputs:**
- `spiderman3_manifest.toml` (38 lines — project config + 9 entrypoint hints)
- `../extracted/default.xex` (14 MB — the binary being recompiled)
- RexGlue SDK (provides `rex::rexglue` executable and template files)

**Outputs (verified from actual generated project):**

| File | Size | Purpose |
|------|------|---------|
| `generated/default/spiderman3_init.h` | 1.7 MB (44,026 lines) | Config macros, memory access macros, function declarations — **ALL `sub_XXXXXXXX` names** |
| `generated/default/spiderman3_init.cpp` | 1.6 MB | `PPCImageConfig` struct, `PPCFuncMappings[]` table — **ALL `sub_XXXXXXXX` names** |
| `generated/default/spiderman3_register.cpp` | ~500 KB | `spiderman3_RegisterFunctions()` — maps 43,676 addresses to `sub_XXXXXXXX` functions |
| `generated/default/spiderman3_recomp.0.cpp` ... `.91.cpp` | ~2 MB each | 92 files containing recompiled PPC functions — **ALL `sub_XXXXXXXX` names** |
| `generated/default/sources.cmake` | 5.3 KB (102 lines) | CMake list of all generated `.cpp` files |
| `generated/rexglue.cmake` | 2.5 KB (69 lines) | SDK boilerplate: `find_package`, `rexglue_setup_target` macro, `spiderman3_codegen` target |
| `CMakeLists.txt` | 29 lines | Root build file — includes rexglue.cmake, defines source list |
| `CMakePresets.json` | 141 lines | Build presets for win/linux × amd64/arm64 × debug/release/relwithdebinfo |
| `spiderman3_manifest.toml` | 38 lines | Manifest (written by `init`, read by `codegen`) |

**Total generated code:** ~180 MB of C++ source (92 recomp files × ~2 MB), 43,676 functions. **All symbols are `sub_XXXXXXXX` at this stage — no semantic names.**

### 3.3 Duration and Progress

**Expected duration:** 30–90 seconds on a modern CPU (Z1 Extreme: ~60 sec). Single-threaded analysis pass over the XEX binary followed by parallel code emission.

**Progress reporting:** `rexglue codegen` does not natively emit progress percentages. Options:
1. **Parse stderr:** `rexglue --log-level info codegen` emits log lines. Parse for phase transitions (loading, analyzing, generating, writing).
2. **File-count proxy:** Poll `generated/default/` for `spiderman3_recomp.*.cpp` files. Target = 92. Progress = `count / 92`.
3. **CMake target:** `COMMENT` shows "Generating recompiled code for spiderman3" — no progress, just a busy indicator.

**Recommended:** Run as subprocess, capture stdout+stderr, parse log lines for phase transitions, use file-count polling as secondary signal.

---

## 4. Funcid Rename Re-Application (CRITICAL POST-CODEGEN STEP)

### 4.1 Why This Step Exists

Codegen emits `sub_XXXXXXXX` symbol names. The funcid pipeline identified 18,871 of 43,676 functions (43.21%) through static analysis (RTTI vtables, import call sites, string references, code patterns, call graph). The rename map (`funcid_06_rename_map_fixed.csv`) maps each `sub_XXXXXXXX` to a semantic name.

**The hook targets in `xmp_bypass.cpp` are renamed symbols — they do NOT exist in raw codegen output:**

| Hook in `xmp_bypass.cpp` | Raw codegen name | Rename source |
|--------------------------|------------------|---------------|
| `REX_HOOK_RAW(XMPGetStatus_Wrapper)` | `sub_829BA2C0` | manual (high confidence) |
| `REX_HOOK_RAW(XamContentGetDeviceState_Wrapper)` | `sub_82A183B0` | manual (high confidence) |
| `REX_HOOK_RAW(XamShowDeviceSelectorUI_Wrapper)` | `sub_82A18160` | manual (high confidence) |
| `REX_HOOK_RAW(FrameDelta_Compute)` (in dynamic_resolution.cpp) | `sub_8284E6B8` | manual (high confidence) |

Without the rename, `REX_HOOK_RAW(XMPGetStatus_Wrapper)` references an undeclared
symbol. The linker either fails (if the symbol is truly absent) or — worse —
silently resolves nothing and the game hangs at boot because XMP/storage/content
calls are unhandled.

### 4.2 The Rename Tool

**Script:** `funcid/funcid_06_rename.py`
**Map:** `funcid/funcid_06_rename_map_fixed.csv` (18,871 entries)

```
python funcid_06_rename.py --apply --root spiderman3/ --csv funcid/funcid_06_rename_map_fixed.csv
```

**What it does:**
- Loads the CSV: `sub_XXXXXXXX,new_name,confidence,source`
- Builds a single regex matching any `sub_XXXXXXXX` as a whole token (word-boundary guarded)
- Replaces every occurrence across all `.cpp` and `.h` files under the root
- Backs up original files to `<file>.bak` before in-place editing
- Writes a manifest to `rename_manifest.txt`

**Verified stats (from `rename_manifest.txt`):**
- Total replacements: 129,634
- Entries: 18,871
- Files touched: 95 (93 in `generated/default/` + `spiderman3_register.cpp` + `xmp_bypass.cpp`)
- Top file: `spiderman3_init.cpp` / `spiderman3_register.cpp` (18,697 replacements each)

**The 95 `.bak` files are the only rollback path** (FILE_INVENTORY §3.2: "Only rollback path for 129,634 renames across 18,871 functions"). Do NOT delete them until the build is confirmed green.

### 4.3 Rename Re-Application on Re-Codegen

**When re-codegen runs, it overwrites `generated/default/*.cpp` and `*.h` with fresh `sub_XXXXXXXX` names.** The `.bak` files are NOT overwritten (codegen doesn't touch them), but they are now stale (they back up the *previous* post-rename state, not the new pre-rename state).

**Correct re-codegen sequence:**
1. Run `rexglue codegen` → fresh `sub_XXXXXXXX` output in `generated/default/`
2. Run `funcid_06_rename.py --apply` → re-applies all 18,871 renames
3. **Gate check:** Verify `DECLARE_REX_FUNC(XMPGetStatus_Wrapper)` exists in `spiderman3_init.h`
4. Only then proceed to build

**Gate check implementation:**
```python
def verify_rename_applied(init_h_path):
    """Assert that hook target symbols exist in the generated header."""
    content = read(init_h_path)
    required_symbols = [
        "XMPGetStatus_Wrapper",
        "XamContentGetDeviceState_Wrapper",
        "XamShowDeviceSelectorUI_Wrapper",
        "FrameDelta_Compute",  # if dynamic_resolution.cpp is active
    ]
    missing = [s for s in required_symbols if s not in content]
    if missing:
        raise RuntimeError(
            f"Rename map not applied. Missing symbols in {init_h_path}: {missing}. "
            f"Run: python funcid_06_rename.py --apply --root spiderman3/ --csv funcid/funcid_06_rename_map_fixed.csv"
        )
```

### 4.4 Rename Idempotency

`funcid_06_rename.py` is **NOT idempotent** if run twice on already-renamed files — it would try to rename `sub_XXXXXXXX` tokens that no longer exist (0 replacements, harmless) but it would create fresh `.bak` files overwriting the originals. 

**Safe re-application:**
1. Check if rename was already applied (gate check above)
2. If already applied AND generated files haven't changed → skip
3. If generated files changed (re-codegen) → re-apply (the fresh `sub_XXXXXXXX` output needs renaming)

### 4.5 The Rename Map as a Versioned Asset

The rename map (`funcid_06_rename_map_fixed.csv`) is an **irreplaceable asset** (FILE_INVENTORY §2.6: "IRREPLACEABLE — DO NOT DELETE"). It represents months of static analysis work. For the automation tool:

- **Ship the rename map** as part of the release (1.2 MB CSV file)
- **Ship the rename script** (`funcid_06_rename.py`, 7.1 KB)
- **Version it** — if the community improves identification, update the map and release a new version
- **Never auto-generate it** — the analysis pipeline (vtable, import, string, pattern, callgraph passes) is out of scope for the automation tool. The tool consumes the map; it does not produce it.

---

## 5. Hand-Authored `src/` File Preservation

### 5.1 The Generated vs. Authored Split

| Location | Touched by codegen? | Touched by rename? | Owner |
|----------|---------------------|---------------------|-------|
| `generated/default/*.cpp` | ✅ (overwritten on re-codegen) | ✅ (renamed) | Build artifact |
| `generated/default/*.h` | ✅ (overwritten on re-codegen) | ✅ (renamed) | Build artifact |
| `generated/rexglue.cmake` | ✅ (written by init) | ❌ | Build artifact |
| `CMakeLists.txt` | ✅ (written by init) | ❌ | **Modified once** (add sources) |
| `src/main.cpp` | ✅ (skeleton from init) | ❌ | **Overwritten once** (Spiderman3App) |
| `src/roundevenf.cpp` | ✅ (skeleton from init) | ❌ | Kept as-is |
| `src/spiderman3_app.h` | ❌ | ❌ | **Hand-authored, preserved** |
| `src/xmp_bypass.cpp` | ❌ | ✅ (10 replacements) | **Hand-authored, preserved** |
| `src/particle_perf.cpp` | ❌ | ❌ | **Hand-authored, preserved** |
| `src/dynamic_resolution.cpp` | ❌ | ❌ | **Hand-authored, preserved** (not in CMakeLists currently) |

### 5.2 What `rexglue init` Creates in `src/`

`rexglue init` creates a minimal skeleton:
- `src/main.cpp` — generic app entrypoint (uses `rex::ReXApp`, not `Spiderman3App`)
- `src/roundevenf.cpp` — CRT math shim (this one is fine as-is)

### 5.3 Files to Inject/Overwrite After Init

| File | Action | Source |
|------|--------|--------|
| `src/main.cpp` | **Overwrite** init skeleton | Pre-authored version with `#include "spiderman3_app.h"` and `Spiderman3App::Create` |
| `src/spiderman3_app.h` | **Create** (init doesn't make it) | Pre-authored: `Spiderman3App` class with `OnConfigurePaths`, `OnPreSetup` (17 cvars), `OnPostLoadXexImage` |
| `src/xmp_bypass.cpp` | **Create** | Pre-authored: 3 `REX_HOOK_RAW` hooks (XMP/XAM bypass) |
| `src/particle_perf.cpp` | **Create** | Pre-authored: placeholder for future particle hooks |
| `src/dynamic_resolution.cpp` | **Create** (optional) | Pre-authored: DRS hook (not currently in CMakeLists) |
| `CMakeLists.txt` | **Overwrite** init skeleton | Pre-authored version with all sources wired |

### 5.4 Actual CMakeLists.txt Source List (Verified)

```cmake
set(SPIDERMAN3_SOURCES
    src/main.cpp
    src/roundevenf.cpp
    src/xmp_bypass.cpp
    src/particle_perf.cpp
)
```

**4 sources.** The BUILD_GUIDE shows only 3 (missing `particle_perf.cpp`) — it is stale. The actual `CMakeLists.txt` on disk has 4. `dynamic_resolution.cpp` exists in `src/` but is NOT in CMakeLists (it's a future feature, not yet wired).

### 5.5 The xmp_bypass.cpp Hook Count (Verified)

The FILE_INVENTORY says "5 system call hooks" — this is **stale**. The actual `xmp_bypass.cpp` has **3 hooks** (verified by reading the file):

1. `XMPGetStatus_Wrapper` → writes 0 to status ptr, returns 0 (idle)
2. `XamContentGetDeviceState_Wrapper` → returns 0 (success)
3. `XamShowDeviceSelectorUI_Wrapper` → synchronous SUCCESS + device_id=1 + async XN_SYS_UI=false broadcast (200ms delayed)

The earlier "5 hooks" version (with `XamContentCreateEnumerator_Wrapper` and `XamEnumerate_Wrapper`) was **superseded** — those hooks masked the real runtime enumeration bug and were removed. The BUILD_GUIDE §4d documents this supersession.

### 5.6 Preservation Strategy

**These files are hand-authored, version-controlled assets.** They are NOT generated. The automation must:

1. **Ship them in a `patches/spiderman3/src/` overlay directory** as part of the release
2. **After `rexglue init`**, copy the overlay files into `spiderman3/src/`:
   - Overwrite `src/main.cpp` (replace generic skeleton with Spiderman3App version)
   - Create `src/spiderman3_app.h`
   - Create `src/xmp_bypass.cpp`
   - Create `src/particle_perf.cpp`
3. **Overwrite `CMakeLists.txt`** with the pre-authored version (4 sources + `/EHa`)
4. **On re-codegen: DO NOT touch `src/`.** Codegen doesn't overwrite `src/`, so these files persist. Only re-apply if the overlay version changed (git hash comparison).

**IMPORTANT (LESSONS_LEARNED §3.5, FILE_INVENTORY §3.2):** `spiderman3_app.h` and `xmp_bypass.cpp` are marked "DO NOT rewrite." The automation must use the exact pre-authored files — it must NOT generate or modify them programmatically. The overlay files are the source of truth.

### 5.7 Preservation Order

```
rexglue init  → creates src/main.cpp (generic), src/roundevenf.cpp, CMakeLists.txt (skeleton)
                ↓
Apply src/ overlay:
  1. Overwrite src/main.cpp         (Spiderman3App entrypoint)
  2. Create   src/spiderman3_app.h  (cvars + paths + data dump)
  3. Create   src/xmp_bypass.cpp    (3 hooks)
  4. Create   src/particle_perf.cpp (placeholder)
  5. Overwrite CMakeLists.txt       (4 sources + /EHa)
                ↓
rexglue codegen  → writes generated/default/ (sub_XXXXXXXX)
                ↓
funcid rename    → rewrites generated/default/ (semantic names)
                 → also rewrites src/xmp_bypass.cpp (10 replacements)
                ↓
Ready for build
```

**Note:** `funcid_06_rename.py` also processes `src/xmp_bypass.cpp` (10 replacements, per `rename_manifest.txt`). This means `xmp_bypass.cpp` in the overlay must be the **pre-rename** version (with `sub_XXXXXXXX` references) OR the rename must be idempotent for already-renamed files. Since the rename script does whole-token matching on `sub_XXXXXXXX`, and the authored `xmp_bypass.cpp` already uses semantic names (`XMPGetStatus_Wrapper`), the rename script will find 0 `sub_` tokens to replace in it — **unless** the overlay ships the pre-rename version. 

**Decision:** Ship the **post-rename** version of `xmp_bypass.cpp` in the overlay (with semantic names like `XMPGetStatus_Wrapper`). The rename script will process it but find nothing to replace (harmless). This is correct because the hook targets (`XMPGetStatus_Wrapper`) are defined in the post-rename `spiderman3_init.h`, so `xmp_bypass.cpp` must use those names to compile.

---

## 6. Incremental Rebuild Detection

### 6.1 What Can Be Skipped

| Step | Skippable? | Cache Key |
|------|-----------|-----------|
| ISO extraction | Yes | ISO file hash + size |
| `rexglue init` | Yes | XEX file hash + SDK version + project name |
| `rexglue codegen` | Yes | XEX hash + SDK version + manifest hash |
| **Funcid rename** | **Yes** (but only if codegen was skipped) | Rename map hash + generated file hash |
| `src/` overlay | Yes | Overlay version (git commit hash) |
| CMake configure | Yes | CMakeLists hash + SDK version + compiler version |
| CMake build | Yes (incremental) | Source file mtimes + compiler version |

### 6.2 Why CMake Can't Help with Codegen Skipping

The `spiderman3_codegen` target is `add_custom_target` (always-runs, no `OUTPUT`/`DEPENDS`). CMake provides no incremental skip. The automation must implement its own detection.

### 6.3 State File

**`<workspace>/.recomp_state.json`:**

```json
{
  "iso": {
    "path": "C:/path/to/Spider-Man 3.iso",
    "sha256": "abc123...",
    "size": 7835492352,
    "extracted_at": "2026-07-07T12:00:00Z"
  },
  "xex": {
    "sha256": "def456...",
    "size": 14069760,
    "title_id": "415607E2",
    "media_id": "b3cca11553bbcb95405cb413045d0c40000006c8"
  },
  "manifest": {
    "sha256": "ghi789...",
    "sdk_version": "0.8.0"
  },
  "codegen": {
    "completed_at": "2026-07-07T12:01:00Z",
    "function_count": 43676,
    "file_count": 92,
    "rexglue_version": "0.8.0"
  },
  "rename": {
    "map_sha256": "jkl012...",
    "applied_at": "2026-07-07T12:01:30Z",
    "replacements": 129634,
    "entries": 18871,
    "gate_check_passed": true
  },
  "src_overlay": {
    "version": "a1b2c3d...",
    "applied_at": "2026-07-07T12:01:35Z"
  }
}
```

### 6.4 Skip Logic

```
function shouldSkipExtraction(state, iso_path):
  if not exists(state): return false
  if state.iso.path != iso_path: return false
  if state.iso.size != fileSize(iso_path): return false
  if state.iso.sha256 != sha256(iso_path): return false
  if not exists("extracted/default.xex"): return false
  return true

function shouldSkipInit(state, xex_path):
  if not exists(state): return false
  if state.xex.sha256 != sha256(xex_path): return false
  if not exists("spiderman3/spiderman3_manifest.toml"): return false
  return true

function shouldSkipCodegen(state):
  if not exists(state): return false
  if not state.codegen.completed_at: return false
  if state.codegen.rexglue_version != rexglueVersion(): return false
  # Verify generated files exist
  if not exists("spiderman3/generated/default/sources.cmake"): return false
  if not exists("spiderman3/generated/default/spiderman3_init.h"): return false
  recomp_files = glob("spiderman3/generated/default/spiderman3_recomp.*.cpp")
  if len(recomp_files) != state.codegen.file_count: return false
  # CRITICAL: verify rename was applied (generated files have semantic names, not sub_)
  if not state.rename.gate_check_passed: return false
  return true

function shouldSkipRename(state):
  if not exists(state): return false
  if not state.rename.applied_at: return false
  # Verify gate check: hook symbols exist in init.h
  if not verify_rename_applied("spiderman3/generated/default/spiderman3_init.h"):
    return false
  # Verify rename map hasn't been updated
  current_map_hash = sha256("funcid/funcid_06_rename_map_fixed.csv")
  if state.rename.map_sha256 != current_map_hash: return false
  return true

function shouldSkipSrcOverlay(state, overlay_dir):
  if not exists(state): return false
  overlay_hash = gitCommitHash(overlay_dir) or directoryHash(overlay_dir)
  if state.src_overlay.version != overlay_hash: return false
  for file in REQUIRED_SRC_FILES:
    if not exists(f"spiderman3/src/{file}"): return false
  return true
```

### 6.5 The Codegen → Rename Dependency

**The critical dependency chain:**

```
codegen skipped?  →  rename must also be skipped (generated files already have semantic names)
codegen re-run?   →  rename MUST be re-run (generated files have fresh sub_XXXXXXXX names)
```

**Logic:**
```python
if shouldSkipCodegen(state):
    # Generated files already renamed — skip rename too
    pass
else:
    runCodegen()
    runRename()  # MUST run after every codegen
    verifyRenameGate()  # MUST pass before proceeding
```

### 6.6 Hash Performance

- **ISO SHA-256:** 7.3 GB → ~20 seconds. Cache the result.
- **XEX SHA-256:** 14 MB → <50ms. Always recompute.
- **Manifest hash:** 38 lines → instant.
- **Rename map hash:** 1.2 MB → <10ms.

**Optimization:** For ISO hash, use `(size, mtime, first_4KB_hash, last_4KB_hash)` as a fast cache key. Only compute full SHA-256 if the fast key matches. Reduces re-hash from 20s to <100ms for unchanged ISOs.

### 6.7 Force Rebuild

Provide `--force` / `--clean` flags:
- `--force-extract`: Delete `extracted/`, re-extract
- `--force-codegen`: Delete `generated/default/*.cpp` + `*.h` (keep `.bak`), re-codegen, re-rename
- `--force-rename`: Re-apply rename map (even if gate check passes)
- `--force-all`: Clear `.recomp_state.json`, full pipeline

**⚠️ `--force-codegen` MUST always re-run rename.** There is no `--force-codegen-only` — codegen without rename produces uncompilable output.

---

## 7. Game Identification

### 7.1 Identification Signals

| Signal | Source | Reliability | Spider-Man 3 Value |
|--------|--------|-------------|---------------------|
| Title ID | XEX LICENCE_INFO header | High | `415607E2` (PAL), `415607D2` (NTSC), `415607F2` (PAL-Asia) |
| Media ID | XEX MEDIA_ID header | High (per-pressing) | `b3cca11553bbcb95405cb413045d0c40000006c8` |
| XEX SHA-256 | Computed from `default.xex` | Perfect (per-build) | `44e736ee3fc77b21db3f00c7330952718b653fbc` |
| PE name | XEX RESOURCE_INFO | Low | `Spidermanxenon_SHIP_IT_final.pe` |
| ISO size | File system | Medium | 7,835,492,352 bytes |

### 7.2 Identification Strategy

**Primary: Title ID** (from XEX header)

```
function identifyGame(xex_path):
  title_id = extract_title_id(xex_path)
  match title_id:
    case "415607D2": return { game: "Spider-Man 3", region: "NTSC", supported: true }
    case "415607E2": return { game: "Spider-Man 3", region: "PAL", supported: true }
    case "415607F2": return { game: "Spider-Man 3", region: "PAL-Asia", supported: true }
    default: return { game: "unknown", supported: false, title_id: title_id }
```

**Secondary: XEX SHA-256** (for exact build matching)

### 7.3 Wrong Game Handling

| Detection | Behavior |
|-----------|----------|
| Not XDVDFS | Hard stop: "This is not an Xbox 360 ISO" |
| `default.xex` missing | Hard stop: "Extraction failed or wrong format" |
| Title ID not in supported list | Hard stop: "This is [title_id], not Spider-Man 3" |
| Title ID matches, XEX hash unknown | Warning: "Unverified build. Patches may not apply. Continue?" |
| Title ID matches, unexpected `REX_IMAGE_BASE` | Hard stop: "Wrong game version — patches won't work" |

**Post-codegen sanity check:** Verify `REX_IMAGE_BASE = 0x82000000` and `REX_CODE_BASE = 0x82280000` in `spiderman3_init.h`. These are game-specific constants from the XEX header. If they differ, the wrong game was recompiled.

### 7.4 Post-Rename Gate as Game Validation

The rename gate check (§4.3) doubles as game validation: if `XMPGetStatus_Wrapper` doesn't exist in `spiderman3_init.h` after rename, either (a) the rename wasn't applied, or (b) the wrong game was recompiled (the hook target addresses `0x829BA2C0`, `0x82A18160`, `0x82A183B0` are Spider-Man 3 specific).

---

## 8. End-to-End Pipeline Flow (Corrected)

```
User provides ISO path
        │
        ▼
┌─────────────────────┐
│ 1. Game ID & Format │  detectXboxIso() + extract_title_id()
│    Detection        │  → Validate: XGD2 + title ID 415607D2/E2/F2
└────────┬────────────┘
         │ valid
         ▼
┌─────────────────────┐
│ 2. ISO Extraction   │  extract-xiso -x iso -d extracted/
│    (skip if cached) │  → Verify extracted/default.xex exists
└────────┬────────────┘
         │ success
         ▼
┌─────────────────────┐
│ 3. RexGlue Init     │  rexglue init --project-name spiderman3
│    (skip if cached) │    --xex-path ../extracted/default.xex
│                     │    --game-root ../extracted
│                     │    --project-root spiderman3/
│                     │  → Creates skeleton: CMakeLists, manifest, src/main.cpp
└────────┬────────────┘
         │ success
         ▼
┌─────────────────────┐
│ 4. Apply src/       │  Copy overlay files into spiderman3/src/:
│    Overlay          │    - Overwrite main.cpp (Spiderman3App)
│    (skip if cached) │    - Create spiderman3_app.h, xmp_bypass.cpp,
│                     │      particle_perf.cpp
│                     │  - Overwrite CMakeLists.txt (4 sources + /EHa)
└────────┬────────────┘
         │ success
         ▼
┌─────────────────────┐
│ 5. RexGlue Codegen  │  rexglue codegen spiderman3_manifest.toml
│    (skip if cached) │  → 92 C++ files with sub_XXXXXXXX names
│                     │  → Progress via log parsing + file count polling
└────────┬────────────┘
         │ success
         ▼
┌─────────────────────┐
│ 6. Funcid Rename    │  python funcid_06_rename.py --apply
│    (MUST run after  │    --root spiderman3/
│     every codegen)  │    --csv funcid/funcid_06_rename_map_fixed.csv
│                     │  → 129,634 replacements across 95 files
│                     │  → sub_XXXXXXXX → semantic names
└────────┬────────────┘
         │ success
         ▼
┌─────────────────────┐
│ 7. Rename Gate      │  Verify DECLARE_REX_FUNC(XMPGetStatus_Wrapper)
│    Check            │  exists in spiderman3_init.h
│                     │  → FAIL if missing → game hangs at boot
└────────┬────────────┘
         │ pass
         ▼
┌─────────────────────┐
│ 8. State Save       │  Write .recomp_state.json with all cache keys
└────────┬────────────┘
         │
         ▼
   Ready for build (custom runtime + game build — separate design)
```

### 8.1 Error Recovery

| Failed Step | Cleanup | Resume |
|-------------|---------|--------|
| 1 (ID/format) | None | User provides correct ISO |
| 2 (extraction) | Delete partial `extracted/` | Re-run from step 2 |
| 3 (init) | Delete partial `spiderman3/` | Re-run from step 3 |
| 4 (src overlay) | Re-apply overlay (idempotent) | Re-run step 4 |
| 5 (codegen) | Delete `generated/default/*.cpp/.h` (keep `.bak`) | Re-run from step 5 |
| 6 (rename) | Restore from `.bak` files, re-run rename | Re-run step 6 |
| 7 (gate check) | Rename not applied or wrong game | Re-run step 6 or halt with "wrong game" |

**Critical:** Step 5 failure or re-run **always** triggers step 6 re-run. There is no scenario where codegen runs and rename doesn't.

---

## 9. Tool Dependencies

| Tool | Source | Bundling | License |
|------|--------|----------|---------|
| `extract-xiso.exe` | https://github.com/XboxDev/extract-xiso/releases | Ship in `tools/` (~200KB) | BSD-3-Clause |
| `rexglue.exe` | RexGlue360 SDK 0.8.0 | Rely on SDK install | SDK license |
| `funcid_06_rename.py` | Project `funcid/` | Ship in `tools/funcid/` (7KB) | Project license |
| `funcid_06_rename_map_fixed.csv` | Project `funcid/` | Ship in `tools/funcid/` (1.2MB) | Project license |
| Python 3.10+ | User install or bundled | For rename script + automation | PSF |
| CMake 3.25+ | User install | Required for build | BSD-3-Clause |
| clang-cl/LLVM 22+ | User install | Required for build | Apache-2.0 |
| MSVC build tools | User install | Required for build | MSVC license |
| Ninja | User install | Required for build | Apache-2.0 |

### 9.1 Minimal Release Bundle

```
spiderman3-recompiler/
├── tools/
│   ├── extract-xiso.exe                    # ISO extraction
│   └── funcid/
│       ├── funcid_06_rename.py             # Rename script
│       └── funcid_06_rename_map_fixed.csv  # 18,871 rename entries
├── patches/
│   └── spiderman3/
│       ├── CMakeLists.txt                  # Pre-authored (4 sources + /EHa)
│       └── src/
│           ├── main.cpp                    # Spiderman3App entrypoint
│           ├── spiderman3_app.h            # 17 cvars + paths + data dump
│           ├── xmp_bypass.cpp              # 3 XMP/XAM hooks
│           └── particle_perf.cpp           # Placeholder
├── game_profiles.json                      # Title ID → game profile
├── recompiler.py                           # Main automation script
└── README.md
```

**Not bundled (prerequisites):**
- RexGlue360 SDK (~500MB with source) — user installs separately
- Build toolchain (CMake, clang-cl, MSVC, Ninja) — user installs

---

## 10. Discrepancies in Existing Documentation

During this design work, I identified stale information in the existing docs. The automation must use the **verified values**, not the docs:

| Source | Stale Claim | Verified Reality |
|--------|-------------|------------------|
| BUILD_GUIDE §2 | `rexglue init --name spiderman3 --xex extracted\default.xex` | Actual flags: `--project-name`, `--xex-path` |
| BUILD_GUIDE §4 | CMakeLists has 3 sources (no particle_perf) | Actual: 4 sources (includes particle_perf.cpp) |
| FILE_INVENTORY §2.1 | xmp_bypass.cpp has "5 system call hooks" | Actual: 3 hooks (superseded version removed) |
| BUILD_GUIDE §4 | `src/spiderman3_app.h` has `OnPostLoadXexImage` | Actual file (94 lines) has `OnConfigurePaths` + `OnPreSetup` only — no `OnPostLoadXexImage`. (The data dump code may have been in an earlier version.) |

---

## 11. Open Questions for User Review

1. **Python vs compiled binary:** Python script for v1 (easy to develop/modify), PyInstaller-compiled exe for release?

2. **extract-xiso bundling:** Ship prebuilt binary, or build from source? Recommend: prebuilt.

3. **Rename map versioning:** Ship a fixed rename map, or allow community updates? Recommend: ship fixed, version it, allow override via `--rename-map` flag.

4. **SDK distribution:** Require users to install RexGlue SDK separately? Recommend: yes (500MB, license constraints).

5. **Multi-region:** Support all three title IDs from day one? Recommend: yes (same patches, same codegen, same rename map — only the XEX binary differs, and the rename map is address-based so it works for all regions).

6. **dynamic_resolution.cpp:** It exists in `src/` but is NOT in CMakeLists. Should the overlay include it and wire it? Or leave it as a future feature? Recommend: leave out of CMakeLists for v1 (it's an experimental DRS hook, not part of the verified working build).

7. **OnPostLoadXexImage:** BUILD_GUIDE describes a data-dump method in `spiderman3_app.h`, but the actual file doesn't have it. Was it removed? Should the overlay include it? Recommend: exclude from overlay unless the user specifically wants data dumps for analysis.
