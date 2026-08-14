# FreeInk SDK Modernization Audit (re-verified, Phase 1)

Status: **audit and candidate selection only. Nothing cherry-picked, nothing
implemented.** Supersedes Milestone 1's earlier SDK-diff research where they
disagree — this pass re-walked the actual current commit range and file
contents directly, rather than reusing that summary.

## Pin state, re-confirmed live (not assumed from prior notes)

- Midad's pin: `a485dc46ef5fb2283e4bdb674002ddbef97a9268` (unchanged since
  Milestone 1)
- CrossPoint `develop`'s freeink-sdk pin, read directly via `git ls-tree` on a
  fresh shallow clone of `crosspoint-reader/crosspoint-reader` (HEAD
  `a757b9d1`, itself unchanged since the SettingsList-contribution work):
  `57f05e80c0f72ab7bdda16a3d8cdf9a9bbaa7b6f` — **unchanged since Milestone 1**.
- freeink-sdk's own `main` has moved further, to `873a2638` — commits beyond
  `57f05e80` are **not** in scope here: Midad's thin-fork policy tracks what
  CrossPoint itself has adopted, not freeink-sdk's bleeding edge, so going past
  CrossPoint's own pin would create new compatibility gaps rather than close
  them.
- **Net result: the comparison range (`a485dc46..57f05e80`, 47 commits) is
  identical to Milestone 1's — but every commit below was re-examined by file
  content, not reused from that summary**, per your instruction that the
  earlier 7 fixes are candidates, not pre-approved.

## Which driver files Midad's build actually compiles (confirmed, not assumed)

`freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp:21`: *"X3 + X4
both link in the generic C3 build; setDisplayX3() picks at runtime."*
Confirmed via `#include` list: `Ssd1677Driver`, `Uc8253X3Driver` (X3),
`Uc8179Driver`, `Uc8279X4Driver` (X4) are **all** compiled into every Midad
binary (`default`/`gh_release*` envs define both `FREEINK_DEVICE_X3=1` and
`FREEINK_DEVICE_X4=1`). `Ssd1683Driver`/`PaperMonoDriver` and `M5*` board
headers are **not** included anywhere in `FreeInkDisplay.cpp` — genuinely
irrelevant to Midad, confirmed by absence, not by commit-title guess.

## Candidates

### SAFE NOW (isolated, real fixes, touch only Midad-compiled files)

| SHA | Commit | Files | Bug fixed | Why Midad needs it | X3/X4 | Battery/display/power | Newer-API dependency | Clean cherry-pick? | Build risk | Hardware validation |
|---|---|---|---|---|---|---|---|---|---|---|
| `b17beee` | fix(x3): one forced full sync after begin(), not two | `Uc8253X3Driver.cpp` only (9 lines) | X3-only: an extra redundant full-refresh at boot | Real, measured boot-time win previously cited (7634ms→4569ms) | X3 only | Faster boot, no other effect | None visible | Yes, single small isolated file | Low | Yes — boot-time timing, needs an X3 to confirm |
| `4bcf068` | fix: correct voltage-to-percentage conversion for battery | `BatteryMonitor.h/.cpp` (shared component, not board-specific) | Non-monotonic voltage curve below 3.22V reporting false 100% | Real user-facing bug: wrong battery percentage near empty | Shared (X3+X4 both use BatteryMonitor) | Battery display correctness only, no power-consumption change | None visible from diff scope | Yes, 2-file isolated change | Low-medium — verify the new curve doesn't regress mid-range accuracy | Yes — needs real battery discharge behavior to confirm, not just code review |
| `65d9eba` | Fix frontlight precision loss at low brightness | `FrontlightManager.cpp` only | PWM precision issue at low brightness levels | **Confirmed irrelevant to current hardware**: `BoardConfig.h`'s `XTEINK_X4`/`XTEINK_X3` profiles both set `NO_FRONTLIGHT` — only `XteinkX4Pro` (future ESP32-S3 hardware, not yet shipping, Phase 5/6 territory) has a frontlight. Moving this to REJECT/IRRELEVANT for *current* Midad hardware — revisit when Phase 5 (S3/Pro) ships | X3/X4: none (no frontlight); X4Pro: yes, future | N/A today | None visible | N/A — not applicable yet | None needed now |
| `0425477` | Hold display RESET pin high during deep sleep | `EpdBus.cpp` + `PowerManager.cpp` (14 lines total) | Real, previously-cited ~36-hour battery drain on UC8179 (X4) panels from RESET pin floating during sleep | Direct battery-life fix on shared bus code | X3+X4 (EpdBus is shared) | Real battery-life improvement — the actual mechanism (not just a claim): explicitly documented as a floating-RESET fix | None visible | Yes, small and isolated, but touches shared power-sequencing code — read the full diff before applying, not just the stat | Medium — power-sequencing changes deserve care even when small | Yes — multi-hour/day sleep-current measurement, can't be confirmed by code review alone |
| `61f0b2b` | Fix display RESET hold logic for gated power rails | `EpdBus.cpp`, `PowerManager.h/.cpp`, `XteinkDetect.cpp` | Refines the same RESET-pin/power-rail area `0425477` touches — chronologically **after** it in this range | Likely a necessary follow-on to `0425477`, not a standalone pick — **cherry-pick both together, in order, or not at all** | X3+X4 | Same subsystem as `0425477` | Depends on `0425477`'s changes to the same file being present first | Only clean if `0425477` is applied first | Medium, same reasoning as `0425477` | Yes, same as `0425477` |
| `cd2bcc5` | Remove unused OEM grayscale LUT banks and fix CDI bug | `Uc8253X3Driver` (X3) + `Uc8279X4Driver` (X4) + shared `Uc8253X3Luts.h` | A real CDI (command) bug plus dead LUT removal | Touches both compiled targets directly, described as a genuine bug fix, not just cleanup | X3+X4 | Display-refresh correctness (previously cited as a border-ghosting-adjacent fix) | None visible | Yes, but review the "CDI bug" portion specifically — mixed cleanup+fix commit, verify the fix survives isolated from the cleanup, or take both together | Low-medium | Yes — visual ghosting is a screen-observation check |
| `477ac31` | Fix grayscale residue accumulation on rapid page turns | `Uc8179Driver` + `Uc8279X4Driver` | Residue buildup after fast page turns (X4 family) | Real, previously-cited visual bug on X4 | X4 | Display-refresh correctness | None visible | Yes, isolated to the two X4-side driver files | Low-medium | Yes — needs rapid-page-turn visual observation |

### DEFER TO FULL SDK BUMP (real but too broad, risky, or dependent on other changes to cherry-pick narrowly)

| SHA | Commit | Files | Why deferred |
|---|---|---|---|
| `c60987a` | Update x4pro drivers according to documentation | `EpdBus`, `Ssd1677Driver`, `Uc8179Driver`, `Uc8279X4Driver` | Re-confirmed still present: replaces a fixed-timeout BUSY wait with an **unbounded** wait-until-HIGH loop — a misbehaving/miswired panel could hang indefinitely. Needs explicit soak-testing on real hardware before acceptance, not a narrow pick |
| `72529a0` | Implement stock-compatible grayscale rendering for UC8179 | `Uc8179Driver.cpp/.h`, 216 lines | Large rewrite with a PSRAM-gated fallback path; X3/X4 have no PSRAM — must verify the non-PSRAM path is correctly exercised, not silently degraded. Too large/behavior-changing for a narrow pick |
| `41f2a7f` | Add UC8279 display driver support for X4 Pro | `FreeInkDisplay.cpp` (the runtime driver-selection dispatcher itself), `PanelDriver.h` (base interface), `Uc8179Driver.cpp`, plus a driver rename | Touches the **shared controller-detection/dispatch code** every device (including X3/X4) runs through — a detection regression here could misroute the wrong init/LUT sequence to real glass. Highest-blast-radius commit in the whole range; defer to the full bump where it gets full regression testing across all device types at once |
| `f50b1ab` | Add grayscale preconditioning for e-paper display | `Uc8179Driver.cpp/.h`, 75 lines added | Rendering-behavior change, additive but substantial; bundle with the full bump's grayscale-path regression pass rather than isolating |
| `a7bb60b` | Fix grayscale-to-BW transition in e-paper driver | `Uc8179Driver.cpp/.h`, 117 lines | Same reasoning — real fix, but a large enough behavior change to want the full bump's broader validation pass, not a solo pick |
| `e52d480` | Add reset phase to e-paper grey level waveforms | `Uc8179Driver.cpp`, 45 lines | Waveform-timing behavior change; low file-count but needs visual regression testing alongside the other grayscale-cluster commits, not in isolation |
| `a479e21` | Fix grayscale waveform LUTs for CrossPoint rendering | `Uc8179Driver.cpp`, 33 lines | Same cluster, same reasoning — bundle with the grayscale-path validation pass |
| `1f0a314` | Add inverted content support for EPD drivers | 23 files across nearly every driver | A real feature addition spanning the whole driver layer, not an isolated fix — squarely full-bump scope |
| `8b8337b` | Add memory pressure levels and task stack types | `MemoryManager.h/.cpp/library.json`, ~275 lines, all new API surface | New infrastructure, not a bug fix — adopting it only matters once something in Midad actually calls the new API, which is bigger scope than a fix cherry-pick |
| `fdf246d` | perf: cut per-page controller and glyph overhead | `TtfFont` (shared, relevant) + `FreeInkDisplay`/`PanelDriver` (shared, relevant) + `Ssd1683Driver` (Paper-Mono-only, irrelevant) | **Mixed commit** — part of it is relevant, part isn't. Not a clean single cherry-pick; would need manual splitting to take only the TtfFont/FreeInkDisplay portion. Worth doing eventually (real perf win on shared font-rendering code) but not a "safe now" one-command pick |
| Touch/gesture cluster: `f94bf05`, `ef9f70d`, `cd1c076`, `74d0695`, `9144ad6` | Swipe/long-press/tap-slop/ListNav work | `keyboard.h`, new touch-gesture files | Midad has no shipping touch-input consumer yet (task #8, "Phase 6: touch input," is still in-progress, separately tracked). Not urgent now — re-evaluate when that phase resumes, since adopting these without a consumer is dead code today |
| FreeInkUICore/FreeInkApp race-fix cluster: `57f05e8`, `83d4934`, `05b7267`/`8c78ef6`/`99b9074`/`c4bb075`, `e70baa2`, `d1109c5` | List-label height, render/loop-task race fixes, shared-theme-mode, text-rendering fast path | `FreeInkUICore.h`, `FreeInkApp.h`, `list.h`, `keyboard.h`, `FreeInkUIGfxRenderer.h` | Midad's own `UiAppHost.h`/`UiAppHelpers.h` **do** already touch `FreeInkApp.h`/`FreeInkUICore` at the integration-glue level (confirmed via grep — this is more relevant than "no consumer yet" like the touch cluster), but no actual Activity uses `list()`/`tabBar()` in production yet. Worth adopting together with the UI-convergence milestone (Phase 1 plan §9), not as a narrow pre-emptive pick |

### REJECT / IRRELEVANT (confirmed not compiled into any Midad target)

| SHA(s) | Why |
|---|---|
| `f9c60ae`, `3c20447`, `b6931b7`, `ba4f1d6`, `e165846`, `032e7f6`, `71040ea`, `1b6a09a`, `8b3c556`, `1d37286`, `629512f`, `24fbab7` | All Paper Mono / M5Stack-specific (`PaperMonoBoard`, `M5Ioe1.h`, `M5Pm1.h`, `Ssd1683Driver`/`PaperMonoDriver`, FT6336 touch controller for that board). None of these files are included by `FreeInkDisplay.cpp` or referenced anywhere in Midad's `src/`/`lib/` — confirmed by grep, not assumed from commit titles. Some of these ("x4pro"-adjacent titles) were exactly the kind of misleading-by-title commits Milestone 1 flagged as a risk of misjudging by title alone — re-confirmed here by actually checking file paths, and they are genuinely irrelevant |
| `1ff0202` | Merge commit wrapping `b17beee` — no independent content |
| `c1d96ba`, `7d2396e` | `ViewableInsets`/bezel-overlap config additions to `BoardConfig.h` — additive, no consumer in Midad yet (Midad's own `UIThemeTokens.h` workaround exists specifically because this doesn't exist at Midad's *current* pin — relevant to the compatibility-workaround-removal milestone, not a standalone fix pick) |
| `2fea991`, `723af4e` | Battery-bolt icon redraw, tab-bar layout tweak — cosmetic FreeInkUI changes with no current Midad consumer (same reasoning as the FreeInkUICore cluster, but purely cosmetic rather than a race fix, so lower priority even than that cluster) |

## Recommended action
Cherry-pick the SAFE NOW set — `b17beee`, `4bcf068`, `0425477`+`61f0b2b`
together, `cd2bcc5`, `477ac31` (five commits/six SHAs, `65d9eba` dropped:
confirmed irrelevant to current X3/X4 hardware, no frontlight) — as a narrow,
isolated commit set once a dedicated test device is available for the
hardware-validation column above — **not implemented yet, per instruction.**
Everything else stays parked behind the eventual full SDK bump or a specific
later milestone (UI convergence, touch-input Phase 6, S3/Pro hardware).
