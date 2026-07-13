# Cvars DarkWolfRTCW RT Effects Playable v6

Все перечисленные ниже игровые cvars применяются во время работы. `vid_restart` не нужен. Исключение: старые latched-параметры разрешения/upscaler задаются BAT-файлом до инициализации renderer.

## Главные переключатели

| Cvar | Значение | Назначение |
|---|---:|---|
| `r_dxr` | `0/1` | Весь DXR lighting pass |
| `r_dxrShadows` | `0/1` | RT-тени |
| `r_dxrDynamicLights` | `0/1` | Реальные игровые point/rect lights |
| `r_dxrSun` | `0/1` | Направленный солнечный источник |
| `r_dxrAO` | `0/1` | Ray-traced ambient occlusion |
| `r_dxrReflections` | `0/1` | Ограниченные RT sky/reflection rays |
| `r_dxrSky` | `0/1` | Sky visibility lighting |
| `r_dxrGI` | `0/1` | Ограниченный one-bounce GI approximation |
| `r_dxrSpecular` | `0/1` | Specular от sun и выбранных lights |
| `r_dxrDenoiser` | `0/1` | Edge-aware spatial denoiser |
| `r_dxrTemporal` | `0/1` | Recursive static-camera temporal stabilization |
| `r_dxrBloom` | `0/1` | Мягкий highlight bloom |

После пакетных переключений: `r_dxrHistoryReset 1`.

## Стабильность и submission

| Cvar | Balanced | Комментарий |
|---|---:|---|
| `r_dxrAsyncSubmit` | `0` | Проверенный стабильный режим; `1` пока экспериментальный |
| `r_dxrCpuSync` | `1` | Явная синхронизация CPU/GPU |
| `r_dxrBuildInterval` | `1` | Обновление AS каждый кадр |
| `r_dxrDispatchInterval` | `1` | RT pass каждый кадр |
| `r_dxrSafeMode` | `1` | Защитная обработка ошибок |
| `r_dxrFallbackLight` | `0` | Камерная diagnostic lamp; в gameplay код ее блокирует |

## Свет и выбор источников

| Cvar | Balanced | Диапазон/смысл |
|---|---:|---|
| `r_dxrMaxLights` | `20` | Максимум источников после importance selection |
| `r_dxrLightSelectionMode` | `1` | `0` — source order, `1` — importance sort |
| `r_dxrLightSelectionHysteresis` | `0.20` | Удерживает ранее выбранные lights, снижает popping |
| `r_dxrLightSelectionMinScore` | `0.0005` | Отбрасывает практически невидимые lights |
| `r_dxrPointLightIntensityCap` | `3.00` | Защита от неограниченной яркости point lights |
| `r_dxrRectLightIntensityCap` | `1.90` | Защита для emissive BSP rect lights |
| `r_dxrLightRadiusMin` | `48` | Нижний предел influence radius |
| `r_dxrLightRadiusMax` | `2048` | Верхний предел influence radius |
| `r_dxrDynamicLightIntensityScale` | `0.65` | Общий scale выбранных lights |
| `r_dxrDynamicLightRadiusScale` | `0.88` | Общий radius scale |

При `r_dxrDebug 1` лог раз в секунду выводит `lights`, `selected`, `rejected`.

## Смешивание и HDR

| Cvar | Balanced | Назначение |
|---|---:|---|
| `r_dxrLightmapStrength` | `1.00` | Оригинальная RTCW lighting base |
| `r_dxrLegacyBlend` | `1.00` | Доля оригинального lightmap |
| `r_dxrDirectLightingStrength` | `0.18` | RT direct diffuse |
| `r_dxrAOLightmapStrength` | `0.18` | Насколько AO модулирует lightmap |
| `r_dxrShadowLightmapStrength` | `0.14` | Насколько RT shadow модулирует lightmap |
| `r_dxrExposure` | `0.97` | Общая pre-tonemap экспозиция |
| `r_dxrRadianceClamp` | `3.00` | Начало мягкого HDR guard |
| `r_dxrHighlightCompression` | `1.60` | Сжатие превышения над clamp |
| `r_dxrTonemap` | `2` | `0` off, `1` Reinhard, `2` ACES approximation |
| `r_dxrHDRWhitePoint` | `2.60` | White point для mode 1 |
| `r_dxrBloomStrength` | `0.018` | Очень слабый bloom |
| `r_dxrBloomThreshold` | `1.45` | Порог bloom |

## Тени

| Cvar | Balanced | Назначение |
|---|---:|---|
| `r_dxrShadowStrength` | `0.74` | Общая сила shadow mask |
| `r_dxrShadowSamples` | `1` | Главный ray budget; Quality использует 2 |
| `r_dxrShadowSoftness` | `0.42` | Размер area sampling |
| `r_dxrShadowBias` | `0.075` | Защита от self-shadow grid/acne |
| `r_dxrShadowMaxDistance` | `3072` | Дальность shadow rays |
| `r_dxrShadowMinVisibility` | `0.16` | Не допускает полностью черных провалов |
| `r_dxrContactShadows` | `1` | Короткие contact rays |
| `r_dxrContactShadowLength` | `80` | Длина contact rays |

## Spatial и temporal

| Cvar | Balanced | Назначение |
|---|---:|---|
| `r_dxrDenoiserRadius` | `1` | Радиус edge-aware filter, максимум 3 |
| `r_dxrDenoiserStrength` | `0.82` | Сила spatial filter |
| `r_dxrDenoiserDepthSigma` | `180` | Защита границ по depth |
| `r_dxrDenoiserNormalSigma` | `30` | Защита границ по normal |
| `r_dxrTemporalWeight` | `0.52` | Вес прошлой resolved history |
| `r_dxrTemporalClamp` | `0.075` | Neighborhood clamp против ghosting |
| `r_dxrTemporalPositionThreshold` | `0.10` | Reset при перемещении камеры |
| `r_dxrTemporalRotationThreshold` | `0.0015` | Reset при повороте камеры |
| `r_dxrTemporalMaxFrames` | `8` | Периодический reset против stale history |
| `r_dxrHistoryReset` | `1` | Ручной one-shot reset |

## Debug views

Сначала `r_dxrDebug 1`, затем:

| `r_dxrDebugEffect` | Вид |
|---:|---|
| `0` | Финальный composite |
| `1` | Shadow visibility |
| `2` | AO |
| `3` | Sun component |
| `4` | Reflection component |
| `5` | GI component |
| `6` | RT direct diffuse |
| `7` | Specular |
| `8` | Original lightmap base |
| `9` | Shadowed/unshadowed direct ratio |

Для возврата: `r_dxrDebugEffect 0; r_dxrDebug 0`.
