# sinew-libc

Sinew libc: musl 1.2.6 with the full Sinew patch series applied.
This repository is the home of Sinew libc work. The musl name,
soname, target tuple, build layout, and ABI compatibility are
unchanged; the loader identifies itself as `sinew libc` and credits
musl-libc (see `README.musl`, the original upstream readme).

## Contents

The tree is musl 1.2.6 plus the applied series (see the Saphira
recipes tree, `musl/files/*.patch` and `musl/PATCHES.md`, for the
patch files and per-patch rationale):

- Base Saphira policy: 1 MiB default pthread stack, kernel-UAPI
  coordination, utmp/wtmp paths (0001–0003).
- F-series arch work: SSE2/SSE4.1 string and math paths (0004–0012,
  0014; 0013 was proposed and never wired).
- Sinew clone3 program: raw primitive, launch trampoline and
  dispatch (0015, 0038), fork/spawn/pthread/legacy-clone migration
  (0039–0041), dispatch diagnostics (0042).
- Correctness series: wordexp, powf, strptime, scanf, regex,
  fnmatch, floatscan, allocator, timer, ftw/nftw (0016–0037).
- Reviewed upstream backports (0101+).
- Loader branding with musl credit (0043).

## Policy

Self-maintained: issues are patched here as found. Upstream musl is
tracked for compatibility through human-reviewed backports only —
nothing in this project fetches, clones, or syncs from
musl-libc.org. The release path (build, sign, publish, install)
remains the Saphira recipes tree (`musl/`); this repository holds
the code.

## License

musl files remain under their MIT license (see `COPYRIGHT`).
Sinew additions follow the file headers.
