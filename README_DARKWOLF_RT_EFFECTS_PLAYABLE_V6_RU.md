> **Обновление v6.2:** применён подтверждённый игровым тестом component pipeline и новый видимый bounded gameplay composite. См. `README_DARKWOLF_RT_EFFECTS_PLAYABLE_V6_2_GAMEPLAY_FIX_RU.md`.

# DarkWolfRTCW RT Effects Playable v6

Это исходный патч и игровой профиль для прохождения кампании RTCW с одновременно включенными RT-эффектами. Он основан на стабильном результате тестов 1-5: синхронный DXR-путь работал без зависаний и `DXGI_ERROR_DEVICE_REMOVED`, а трассируемые тени от факелов, кронштейнов и решеток были видимы.

## Что изменено в исходном коде

- Камерный fallback-light заблокирован в обычной игре и разрешен только при `r_dxrDebug 1`.
- Реальные point/rect lights нормализуются и ограничиваются до безопасной HDR-интенсивности.
- Каждый кадр выбираются наиболее важные источники по яркости, радиусу, площади и расстоянию до камеры. Добавлен hysteresis, чтобы набор источников не дрожал на границе лимита.
- Оригинальный RTCW lightmap сохраняется как стабильная база. RT direct light, specular, AO, shadow modulation, reflections и GI смешиваются отдельно.
- Перед composite применяется мягкий HDR guard, затем tonemap. Это устраняет внезапные полностью белые комнаты из теста 5.
- Shadow bias зависит от угла луча; поднят безопасный базовый bias. Одно-сэмпловые тени проходят edge-aware spatial denoiser.
- Temporal-фильтр стал рекурсивным в едином post-tonemap color domain. История сбрасывается при перемещении/повороте камеры, загрузке карты, изменении размера, ручном reset и периодически по лимиту кадров.
- Все эффекты имеют cvars и переключаются в консоли без `vid_restart`.
- По умолчанию оставлен подтвержденный стабильный путь `r_dxrAsyncSubmit 0`, `r_dxrCpuSync 1`.

## Основной профиль

После сборки распакуйте артефакт GitHub Actions прямо в папку игры рядом с `WolfSP.exe`. Сначала запустите `VERIFY_RT_PLAYABLE_V6_INSTALL.bat`, затем:

```text
RUN_RT_ALL_BALANCED.bat
```

Профиль включает одновременно:

- RT shadows и contact shadows;
- реальные динамические и emissive rect lights;
- directional sun;
- RT AO;
- bounded sky visibility/reflections;
- low-cost one-bounce GI approximation;
- specular;
- spatial denoiser;
- temporal stabilization;
- HDR guard, ACES tonemap и очень мягкий bloom.

Дополнительные профили:

```text
RUN_RT_ALL_PERFORMANCE.bat
RUN_RT_ALL_QUALITY.bat
RUN_RT_ALL_BALANCED_ESCAPE1_TEST.bat
RUN_RT_DISABLE_V6.bat
```

## Сборка через GitHub Actions

1. Замените содержимое репозитория содержимым полного v6 source-архива или примените patch/overlay.
2. Commit и push в GitHub.
3. Откройте **Actions**.
4. Запустите workflow **DarkWolf RTCW RT Effects Playable v6**.
5. Выберите `Release`.
6. Скачайте артефакт `DarkWolfRTCW-RT-Effects-Playable-v6-Release`.
7. Скопируйте его содержимое в папку игры.

Workflow сначала устанавливает deterministic source snapshots, затем проверяет layout C/HLSL, cvars, отсутствие `vid_restart`, компилирует embedded HLSL через DXC и только после этого собирает игру.

## Быстрые переключатели в консоли

```text
r_dxrShadows 0/1
r_dxrSun 0/1
r_dxrDynamicLights 0/1
r_dxrAO 0/1
r_dxrReflections 0/1
r_dxrSky 0/1
r_dxrGI 0/1
r_dxrSpecular 0/1
r_dxrDenoiser 0/1
r_dxrTemporal 0/1
r_dxrBloom 0/1
```

После изменения temporal/denoiser или нескольких эффектов сразу выполните:

```text
r_dxrHistoryReset 1
```

Для визуальной изоляции компонентов:

```text
r_dxrDebug 1
r_dxrDebugEffect 1   // shadow mask
r_dxrDebugEffect 2   // AO
r_dxrDebugEffect 3   // sun
r_dxrDebugEffect 4   // reflections
r_dxrDebugEffect 5   // GI
r_dxrDebugEffect 6   // direct diffuse
r_dxrDebugEffect 7   // specular
r_dxrDebugEffect 8   // original lightmap base
r_dxrDebugEffect 9   // shadow ratio
r_dxrDebugEffect 0   // final image
```

Полная таблица: `DARKWOLF_RT_EFFECTS_PLAYABLE_V6_CVARS_RU.md`.

## Важные границы текущей архитектуры

RTCW не предоставляет полноценный modern PBR G-buffer с roughness/metalness/emissive textures. Поэтому v6 сохраняет оригинальный lightmap и добавляет ограниченные RT-компоненты поверх него. Reflections и GI являются безопасными visibility-based приближениями, а не path tracing. Это осознанный выбор для стабильной кампании и сохранения художественного освещения оригинала.

Temporal не выполняет motion-vector reprojection. Поэтому история намеренно сбрасывается при движении камеры, чтобы не появлялись длинные шлейфы. При остановке она накапливает стабильный результат; в движении основную очистку выполняет spatial denoiser.
