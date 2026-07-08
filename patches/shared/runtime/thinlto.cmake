# ThinLTO / IPO CMake include fragment for the rexruntime target.
#
# This is NOT a source-tree edit. It is a CMake property injection: the
# apply_patches stage arranges for the runtime's src/system/CMakeLists.txt
# to `include()` this file after `add_library(rexruntime SHARED ...)`, which
# sets INTERPROCEDURAL_OPTIMIZATION on the Release and RelWithDebInfo
# configurations. Pairs with `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON` at
# configure time as belt-and-suspenders (the -D is the redundant safety
# net; this include() is the authoritative, version-independent mechanism).
#
# The shipped SDK source already sets these properties at
# src/system/CMakeLists.txt lines 54-57. This fragment re-applies them so
# the optimization is guaranteed regardless of SDK version, and it is
# idempotent (set_target_properties overwrites the same values).
#
# Effect: 5-15% runtime performance gain via cross-module inlining of the
# recompiled PPC translation layer. ThinLTO is the shipped configuration;
# PGO is a mutually-exclusive experimental path (clang rejects
# -flto=thin + -fprofile-instr-generate).
#
# Patch id: runtime/thinlto-ipo
# Category: general-runtime (always-on build-config, not a source patch)

# === ThinLTO / IPO for rexruntime.dll ===
# Re-apply regardless of SDK version. set_target_properties is idempotent.
set_target_properties(rexruntime PROPERTIES
    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
)

# If the host compiler is clang, prefer ThinLTO specifically (not full LTO).
# Full LTO is incompatible with lld-link on Windows for this project's link
# configuration; ThinLTO ships instead. Only emit the flag when IPO is
# actually on for the current config to avoid polluting Debug builds.
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    target_compile_options(rexruntime PRIVATE
        $<$<CONFIG:Release>:-flto=thin>
        $<$<CONFIG:RelWithDebInfo>:-flto=thin>
    )
endif()
