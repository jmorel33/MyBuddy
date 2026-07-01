# MyBuddy Allocator Consistency Plan

**Status:** In progress  
**Triggered by:** MyBuddy integration (v2.4.266). First test run surfaced `STATUS_HEAP_CORRUPTION`
from a mixed-allocator call in `situation_impl_etc.h`. Broader audit found two categories of
remaining inconsistency.

---

## Background

Situation's allocator abstraction (`SIT_MALLOC`/`SIT_CALLOC`/`SIT_REALLOC`/`SIT_FREE`) is now
backed by MyBuddy. Any pointer allocated with the CRT `malloc`/`calloc`/`realloc` and freed with
MyBuddy's `mbd_free` — or vice versa — is heap corruption. This plan documents all remaining
inconsistency sites and prescribes fixes.

## Audit Summary

All `sit/situation_impl_*.h` files: **clean** — only comment-text mentions of `free()`.
`sit/aud/` and `sit/aud/fx/`, `sit/aud/polysonix/`: **two categories of raw allocator usage**
described below.

`sit/ext/` remains out of scope (third-party amalgamation).

**`sit/kfs/` (updated M7, 2026-06-30):** KFS is **MyBuddy-ready** as a standalone library.
All KFS + SQLite heap traffic routes through `kfs_mem` (`KFS_*` macros + full
`sqlite3_mem_methods` vtable). Default build uses CRT in the sole backend island;
`-DKFS_MEM_USE_MYBUDDY` links MyBuddy in `kfs.c` and delegates the backend island
plus **SQLite `mem_methods`** to `mbd_*` — **no** `SIT_MALLOC` / Situation headers.
Production config: **profile C** (`BUDDY_LARGE`, 256 MiB pool). H7 bench on KFS
(2026-06-30): ~25–40% faster reads vs CRT; ingest parity — see `sit/kfs/doc/memory_alloc_plan.md` §10.
Situation integration may later share the same process-wide MyBuddy heap via `kfs_mem_init`.

---

## Category 1 — `#ifndef SIT_MALLOC` Fallback Definitions (Already Correct)

Several `aud/` files define their own CRT fallbacks for use when compiled standalone:

| File | Pattern |
|------|---------|
| `sit/aud/pcm_input.h` | `#ifndef SIT_MALLOC ... #define SIT_MALLOC(s) malloc(s) ... #endif` |
| `sit/aud/node_graph_impl.h` | same |
| `sit/aud/node_graph_serialization_impl.h` | same |
| `sit/aud/fx/chorus_4stage.h` | same |
| `sit/aud/fx/spectrum_analyzer.h` | same |
| `sit/aud/fx/studio_reverb.h` | same |

**Status: No action needed.** These use `#ifndef` guards. When compiled as part of Situation
(where `situation_impl_deps.h` has already defined `SIT_MALLOC` = `mbd_alloc`), the guards
prevent the CRT fallback from firing. They're already routing through MyBuddy correctly.

---

## Category 2 — Internally Consistent CRT Islands (Bypass MyBuddy, No Cross-Contamination)

These subsystems allocate with raw `malloc`/`calloc`/`realloc` and free with raw `free` — every
allocation and free within the island is CRT-only. There is no cross-contamination with
`SIT_MALLOC`-allocated pointers at the boundaries. They currently bypass MyBuddy entirely.

### 2a — `sit/aud/aud/midi.h` and `sit/aud/midi_device.h` and `sit/aud/midi_learn.h`

The MIDI subsystem (PortMidi wrapper, processor, device, learn state, JSON buffers, virtual
device buffers, stream structs) uses raw CRT throughout. Allocations and frees are strictly
paired within the MIDI code. No `SIT_MALLOC`-allocated pointer crosses into this island.

### 2b — `sit/aud/sound_source.h`

`sound_source_load_buffer`, `sound_source_resize_buffer`, `sound_source_cleanup` use
`malloc`/`realloc`/`free` for `src->buffer`. The `SituationSoundSource*` struct itself is
allocated by the higher-level audio layer via `SIT_CALLOC`, but `src->buffer` is a separately
allocated sub-buffer that is always freed by `sound_source_cleanup` — never via `SIT_FREE`.
**Currently safe** but fragile: if the cleanup path is ever routed through `SIT_FREE` by
accident, it becomes Class A corruption.

### 2c — `sit/aud/tone_synth_graph.h`

`_SituationToneSynthSumLimiterFree` / `_SituationToneSynthSumLimiterInit` use
`calloc`/`free` for `delay_line_l` / `delay_line_r`. Self-contained pair.

### 2d — `sit/aud/fx/dynamics.h`

`dynamics_init` / `dynamics_cleanup` use `calloc`/`free` for `delay_line_l`/`delay_line_r`.
Self-contained pair.

### 2e — `sit/aud/fx/deafmax.h`

`deafmax_create` / `deafmax_destroy` use `calloc`/`free` for the `DeafMax` struct itself.
Self-contained pair.

### 2f — `sit/aud/fx/maximizer.h`

`sit_fft_init` / `sit_fft_free` use raw `malloc`/`free` for FFT plan buffers. The large
aligned allocations use `_aligned_malloc`/`_aligned_free` on Windows and `posix_memalign`/`free`
on POSIX — these **cannot** be replaced with `SIT_MALLOC` directly because they require
alignment guarantees. They must stay as-is or migrate to `mbd_memalign`.

### 2g — `sit/aud/polysonix/polysonix.h` and `sit/aud/polysonix/px_patching.h` and `sit/aud/polysonix/dsp_math.h`

The entire Polysonix synth engine uses raw `calloc`/`free` throughout — voices, ADSRs, LFOs,
patch banks, FFT twiddle tables, preset buffers. All allocations and frees are internal.
No pointer from this island crosses into `SIT_MALLOC` territory.

---

## Fix Strategy

### Phase A — Migrate Category 2 to `SIT_MALLOC` (recommended, non-urgent)

The safest long-term state is for all of Situation's audio subsystem to route through
`SIT_MALLOC`/`SIT_FREE` so MyBuddy manages everything consistently. The Category 2 islands
are currently safe but will silently bypass MyBuddy's stats, profiling, and cache-warming.

Migration is mechanical for most sites — replace `malloc(` → `SIT_MALLOC(`, `calloc(` →
`SIT_CALLOC(`, `realloc(` → `SIT_REALLOC(`, `free(` → `SIT_FREE(` within each island.

**Exceptions that need care:**
- `maximizer.h` aligned allocations: replace `_aligned_malloc`/`posix_memalign` with
  `mbd_memalign(64, ...)` (available in MyBuddy's public API) and the matching `free`/`_aligned_free`
  with `mbd_free`. Note: `mbd_memalign` is exposed via `mybuddy_api.h`, not via `SIT_MALLOC` —
  these sites need a direct `mbd_memalign` call.
- `midi.h` `dev->info.name`: allocated with `malloc` and cast to `const char*`. Replace with
  `SIT_MALLOC`, ensure the matching `free((void*)dev->info.name)` becomes `SIT_FREE(dev->info.name)`.

### Phase B — Harden the Boundary at `sound_source.h` (urgent)

`src->buffer` is the most dangerous site: the struct owning it is `SIT_CALLOC`-allocated, but
the buffer sub-field is `malloc`-allocated. If future refactoring introduces a code path that
calls `SIT_FREE(src->buffer)`, that is instant corruption. Fix now:

```c
// sound_source.h — replace all three sites:
src->buffer = (float*)SIT_MALLOC(frames * channels * sizeof(float));
float* nb = (float*)SIT_REALLOC(src->buffer, need_bytes);
SIT_FREE(src->buffer);
```

### Phase C — Add a Lint Rule

Add a note to the steering file (`situation-project.md`) and the architecture doc that raw
`malloc`/`calloc`/`realloc`/`free` are forbidden in `sit/` code. All allocation must go through
`SIT_MALLOC`/`SIT_CALLOC`/`SIT_REALLOC`/`SIT_FREE`. The only exceptions are:
- `mbd_memalign` for alignment requirements
- Code inside `sit/kfs/` (its own domain)
- Third-party code in `ext/`

---

## Checklist

### Phase A — Migrate aud/ CRT islands to SIT_MALLOC
- [ ] A1 `sit/aud/midi.h` — replace ~25 raw allocator calls
- [ ] A2 `sit/aud/midi_device.h` — replace ~8 raw allocator calls
- [ ] A3 `sit/aud/midi_learn.h` — replace ~10 raw allocator calls
- [ ] A4 `sit/aud/tone_synth_graph.h` — replace 4 raw allocator calls
- [ ] A5 `sit/aud/dynamics.h` — replace 4 raw allocator calls
- [ ] A6 `sit/aud/fx/deafmax.h` — replace 2 raw allocator calls
- [ ] A7 `sit/aud/fx/maximizer.h` — replace non-aligned calls with `SIT_MALLOC`/`SIT_FREE`; replace aligned calls with `mbd_memalign`/`mbd_free`
- [ ] A8 `sit/aud/polysonix/polysonix.h` — replace ~35 raw allocator calls
- [ ] A9 `sit/aud/polysonix/px_patching.h` — replace ~15 raw allocator calls
- [ ] A10 `sit/aud/polysonix/dsp_math.h` — replace ~4 raw allocator calls
- [ ] A11 `sit/aud/sound_source.h` — replace 3 raw allocator calls (see Phase B note)

### Phase B — Harden sound_source.h boundary
- [x] B1 `sit/aud/sound_source.h` — migrate `src->buffer` to `SIT_MALLOC`/`SIT_REALLOC`/`SIT_FREE` *(merge with A11)*

### Phase C — Policy
- [ ] C1 Add "no raw malloc/free in sit/" rule to `situation-project.md` steering file
- [ ] C2 Update `doc/architecture.md` allocator section to document the rule and exceptions

### On completion
- [ ] Rebuild `static-opengl` — zero new warnings
- [ ] Run full test harness — all previously-passing tests still pass
- [ ] Bump Situation patch version and log in `doc/UPDATELOG.md`
