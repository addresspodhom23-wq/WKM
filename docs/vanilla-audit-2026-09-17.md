# Vanilla renderer audit — 2026-09-17

Baseline: `869d4f18dfb627d2e4d8da07a4bcfa6cab673501`, branch `vanilla112-wmo-complete`.

This is a source and regression audit of stages 1–4 and the stage-5 build blocker. It is not a certification of pixel-identical WoW.exe behavior.

## Confirmed defects fixed

- Android Clang rejected an unused sequence-index variable in updateParticles after the cross-fade conversion. Remove the obsolete declaration; keep sequence lookup where discrete emitter tracks still need it.
- Portal particle emission and ribbon update/draw were disabled by model classification even in Vanilla mode. Restrict the suppression to the enhanced preset.
- Portal placement was still overwritten by synthetic rotation during update. Do not apply that rotation in Vanilla.
- Smoke-name classification replaced authored geometry with synthetic smoke/sparks. Disable replacement emission/draw in Vanilla and retain the authored mesh in CPU/GPU visibility and shadow selection.
- Lava received synthetic UV scrolling when the authored translation was zero. Preserve zero translation in Vanilla in both passes.
- Additive fire materials received synthetic lamp flicker. Restrict it to the enhanced preset and restore the authored tint before drawing, including after a preset switch.
- The fragment shader still forced opaque output from a name-derived black-color-key hint in Vanilla. Preserve the authored texture alpha.

## Review coverage

| Stage | Reviewed paths | Result / limit |
|---|---|---|
| 1: material/effect overrides | M2 loader, material setup, opaque/transparent draw, fragment shader | Additional override leaks above fixed; contract extended. |
| 2: vegetation/cloth and bones | authoredAnimationEnabled, cached animation lists, sway setup, billboard camera basis, weighted skinning | Vanilla preserves authored animation and disables procedural sway. Visual poses still require reference comparison. |
| 3: M2 and detail fade | world-doodad fade bands, authored bounds/scale, Vanilla LOD selection, detail fade shader, view-distance helper | Existing behavior retained. Distance helper tests pass; scene-level pop/fade needs device verification. |
| 4: emitters/ribbons | sequence sampling, emitter clocks, portal filters, blend/fog paths, ribbon history, draw ordering | Portal/smoke fixes above; do not call the entire stage fully verified. |

## Validation

- `test_vanilla_renderer_contract.cpp`: compiled with C++20, -Wall -Wextra -Werror and passed after extending regression coverage.
- Catch2 `test_m2_blend_mode.cpp`, `test_m2_view_distance.cpp`, `test_m2_glow_card.cpp`: 26 cases, 85 assertions passed.
- Full Android compilation, shader compilation, existing collision/WMO/placement contracts and APK signature verification run in the existing GitHub workflow on this commit.
- Source-contract tests guard wiring and known regressions; they do not execute a Vulkan frame or prove visual parity.

## Remaining evidence gaps

1. Particle AlphaKey still uses a luminance cutoff; ribbon output uses a universal low-alpha discard. Compare the exact WoW 1.12 material state before changing thresholds or opaque behavior.
2. Name/geometry-based invisible-trap and emitter-volume classification remains. Confirm reference treatment against actual models rather than removing potentially intentional invisibility.
3. Texture animation currently transports translation to the M2 shader. Full authored UV rotation/scale and final sequence selection belong to the unfinished animation work.
4. The stage-5 nextAnimation/variationNext, frequency and replay selection work is not declared complete or folded into this repair.
5. Portals already rotated in the enhanced preset do not recover their original placement merely by toggling presets; compare using a fresh Vanilla scene.
6. No controlled captures from the same camera, time, assets and scene on WoW.exe and the phone were available for this audit.

## Device comparison for this build

Start directly in Vanilla mode. Compare the same locations with the desktop reference: Tirisfal trees/cloth, Orgrimmar fire and smoke, one instance portal, lava, and spell/ribbon effects. Verify collision remains intact and check near/far fade while walking. Supply the installed build number and log with any differing scene.

