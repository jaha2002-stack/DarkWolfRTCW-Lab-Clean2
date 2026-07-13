# DarkWolfRTCW RT Effects Playable v6.1 — Debug and Component Pipeline Fix

Это небольшой source-overlay поверх Playable v6. Он не меняет игровой Balanced-профиль.

## Что исправлено

- `r_dxrDebugEffect != 0` выводится прямо в конечную UAV-текстуру без spatial denoiser, temporal, HDR clamp, bloom, tonemap, saturation, contrast и gamma.
- История автоматически сбрасывается при каждом изменении `r_dxrDebugEffect`.
- Режим `8` показывает точный `gAlbedoTex`.
- Режимы `4/5/6/7` диагностически нормализованы, но игровое смешивание не изменено.
- Добавлены проверки всей цепочки: `10=красный`, `11=зелёный`, `12=синий`.
- При `r_dxrDebug 1` раз в секунду печатается строка `DXR v6.1 CPU CB:` с фактическими значениями CPU constant buffer и текущим количеством выбранных источников.
- Для fallback-теста используется правильный `r_dxrDynamicLights`; диагностический fallback сам гарантирует включённую dynamic-light ветку, не меняя обычный игровой режим.
- Для каждого поля `glRaytracingEffectsOptions_t` и внешнего `glRaytracingLightingConstants_t` добавлен отдельный `static_assert(offsetof(...))`.

## Быстрый тест

Запустите `RUN_RT_V61_DEBUG_COMPONENTS.bat`. Первым должен появиться полностью красный кадр. Затем в консоли:

```cfg
r_dxrDebugEffect 11 // полностью зелёный
r_dxrDebugEffect 12 // полностью синий
r_dxrDebugEffect 8  // точный входной albedo/scene texture
r_dxrSun 1
r_dxrSunIntensity 1
r_dxrDebugEffect 3  // прямой цвет sun constant buffer
r_dxrDebugEffect 6  // нормализованный direct light + fallback probe
```

Для возврата к игре:

```cfg
r_dxrFallbackLight 0
r_dxrDebugEffect 0
r_dxrDebug 0
exec dxr_v6_all_balanced.cfg
```
