# Changelog — DarkWolfRTCW RT Effects Playable v6

## v6.0 source patch

- Удален camera-follow fallback light из игрового пути.
- Добавлена нормализация и cap legacy emissive/point light intensity.
- Добавлен deterministic importance selection с hysteresis.
- Расширен C/HLSL effects cbuffer с 64 до 80 32-bit scalars.
- Добавлен отдельный component mixer: authored lightmap, AO modulation, shadow modulation, RT direct diffuse, specular, reflections, GI.
- Добавлен мягкий pre-composite radiance guard.
- Улучшен angle-aware shadow bias и ограничены shadow budgets.
- Spatial denoiser фильтрует только RT residual, сохраняя детали исходной картинки.
- Temporal history переведена в recursive post-tonemap domain и сбрасывается при camera/map/resize changes.
- Добавлены debug views `6-9` для direct/specular/lightmap/shadow ratio.
- Все RT effect toggles доступны как runtime cvars.
- Gameplay defaults переведены на `AsyncSubmit 0`, `CpuSync 1`, без startup `vid_restart`.
- Добавлены Balanced, Quality, Performance и Disable профили.
- Добавлен отдельный GitHub Actions workflow с static verification и DXC compile-check.
