# DarkWolf RTCW DXR CleanVisual RT Effects Lab

## Назначение

Этот kit построен строго поверх стабильной базы `DarkWolfRTCW_DXR_Clean_Visual_Performance_Kit_v2`.
Он не заменяет классический Clean Release composite, не переводит игру на low-resolution block rendering и не добавляет PBR.
Цель — сохранить исходный вид RTCW, specular, стабильность и быстрый async-путь, но дать отдельные переключатели для реальных DXR visibility rays, солнца, динамического света, AO, приближённых reflections/GI и очистки изображения.

## Ключевое исправление отсутствующих теней

В Performance v2 одинаковые посещения сцены отсекались на уровне всего `trDXRMesh_t`. Один brush model содержит несколько поверхностей, поэтому после первой поверхности остальные могли не попасть в RT-геометрию. Это особенно опасно для железных решёток, дверей и кронштейнов факелов.

Lab переносит кэш на `trDXRSurface_t`: каждая поверхность загружается один раз, но ни одна соседняя поверхность модели не теряется. Для shadow rays по умолчанию используется `r_dxrShadowCullMode 0`, то есть без front/back-face culling — это помогает тонкой и двусторонней геометрии.

Ограничение: если решётка сделана не геометрией, а прозрачными отверстиями в alpha-текстуре, текущий DXR path всё ещё видит треугольник как opaque. Полностью правильные alpha-tested тени потребуют any-hit shader и texture lookup; этот kit этого не выдаёт за готовую функцию.

## Удаление пиксельности и зернистости

- `r_dxrNativeResolution 1` отключает внутренний upscaler и сохраняет full-resolution raster/G-buffer и текстуры.
- `r_picmip 0`, `r_picmip2 0`, trilinear filtering и detail textures сохраняют исходное разрешение обычных и будущих HD texture packs.
- Новый edge-aware denoiser фильтрует только RT/light residual. Центральный full-resolution albedo добавляется обратно без размытия, поэтому текстурная детализация не должна превращаться в крупные блоки.
- Temporal accumulation имеет clamp и автоматический reset при заметном изменении матрицы камеры.

Temporal path пока не имеет motion vectors и полноценной reprojection. Он выключен в основном `BALANCED` профиле; отдельный профиль предназначен для тестирования. При быстром движении возможен ghosting — тогда уменьшайте `r_dxrTemporalWeight` или выключайте `r_dxrTemporal`.

## Сборка через GitHub Actions

1. Скопируйте содержимое папки `dxr_cleanvisual_rt_effects_lab_kit` в корень своего репозитория.
2. Commit changes.
3. Откройте **Actions**.
4. Запустите workflow **DarkWolf DXR CleanVisual RT Effects Lab**.
5. Скачайте artifact **DarkWolf-DXR-CleanVisual-RT-Effects-Lab-Release**.

Workflow выполняет четыре проверки до упаковки:

1. применяет/проверяет Performance v2;
2. применяет изолированный patch 08;
3. проверяет обязательные source markers и layout constant buffer;
4. извлекает встроенный HLSL и компилирует его `dxc.exe -T lib_6_3` до запуска MSBuild.

## Первый порядок тестирования

1. `RUN_RESET_SAFE_NO_DXR.bat`, затем полностью закрыть игру.
2. `RUN_RT_EFFECTS_BASELINE.bat` — контроль оригинальной картинки.
3. `RUN_RT_DEBUG_SHADOW_CASTERS.bat` — проверить геометрию решётчатой двери/кронштейнов.
4. `RUN_RT_SHADOWS_ONLY.bat`.
5. `RUN_RT_DYNAMIC_LIGHTS_ONLY.bat` — проверить факелы и игровые dynamic lights.
6. `RUN_RT_DENOISE_TEMPORAL.bat` — проверить сетчатую вуаль и зернистость.
7. `RUN_RT_ALL_BALANCED.bat` — рекомендуемый основной профиль.
8. `RUN_RT_ALL_HIGH.bat` — только после стабильного Balanced.

## Профили

- `BASELINE`: минимум новых эффектов, full-resolution, fallback test light.
- `SHADOWS_ONLY`: прямые visibility rays, мягкие тени и лёгкий denoiser.
- `SUN_ONLY`: directional sun с RT-тенями.
- `DYNAMIC_LIGHTS_ONLY`: реальные игровые dynamic lights/факелы без fallback light.
- `AO_ONLY`: отдельный RT AO.
- `REFLECTIONS_ONLY`: visibility/environment approximation; это не material/PBR closest-hit reflection.
- `GI_ONLY`: sky/GI visibility approximation; не multi-bounce path tracing.
- `DENOISE_TEMPORAL`: отдельная проверка spatial + temporal очистки.
- `ALL_BALANCED`: классический tonemap, умеренные эффекты, temporal off.
- `ALL_HIGH`: больше samples, temporal, HDR tonemap и слабый bloom; экспериментальный и тяжелее.
- `DEBUG_SHADOW_CASTERS`: чёрно-белая видимость RT-теней и гарантированный fallback light.

## Основные cvars

### Тени

```
r_dxrShadows 0/1
r_dxrShadowStrength 0..2
r_dxrShadowSamples 1..16
r_dxrShadowSoftness 0..4
r_dxrShadowMaxDistance
r_dxrShadowCullMode 0/1/2   // none/front/back
r_dxrShadowMinVisibility 0..1
r_dxrContactShadows 0/1
r_dxrContactShadowLength
```

### Солнце

```
r_dxrSun 0/1
r_dxrSunIntensity
r_dxrSunDirX / Y / Z
r_dxrSunColorR / G / B
r_dxrSunAngularRadius
r_dxrSunShadowSamples
```

### Динамические источники

```
r_dxrDynamicLights 0/1
r_dxrDynamicLightShadows 0/1
r_dxrMaxLights
r_dxrDynamicLightIntensityScale
r_dxrDynamicLightRadiusScale
```

### AO, reflections, sky/GI, specular

```
r_dxrAO, r_dxrAOSamples, r_dxrAORadius, r_dxrAOStrength
r_dxrReflections, r_dxrReflectionSamples, r_dxrReflectionStrength
r_dxrReflectionMaxDistance, r_dxrReflectionRoughness
r_dxrSky, r_dxrSkySamples, r_dxrSkyStrength, r_dxrSkyMaxDistance
r_dxrGI, r_dxrGISamples, r_dxrGIStrength, r_dxrGIMaxDistance
r_dxrSpecular, r_dxrSpecularStrength, r_dxrSpecularPower
```

### Denoiser и temporal

```
r_dxrDenoiser 0/1
r_dxrDenoiserRadius 0..3
r_dxrDenoiserStrength 0..1
r_dxrDenoiserDepthSigma
r_dxrDenoiserNormalSigma
r_dxrTemporal 0/1
r_dxrTemporalWeight 0..0.95
r_dxrTemporalClamp
r_dxrTemporalResetThreshold
r_dxrHistoryReset 1
```

### HDR, tonemap, bloom и цвет

```
r_dxrTonemap 0/1/2       // classic / Reinhard-like / ACES-like
r_dxrHDRWhitePoint
r_dxrBloom 0/1
r_dxrBloomStrength
r_dxrBloomThreshold
r_dxrSaturation
r_dxrContrast
r_dxrOutputGamma
```

### Разрешение и совместимый upscaler

```
r_dxrNativeResolution 1  // основной вариант, latched
r_dxrUpscalerBackend
r_dxrUpscalerQuality
r_dxrUpscalerSharpness
r_dxrRayAIDenoise
r_dxrDLSSRayReconstruction
r_dxrFSRRayRegeneration
```

При изменении latched cvars нужен `vid_restart` или полный перезапуск игры.

### Debug

```
r_dxrDebug 1
r_dxrDebugEffect 0 normal
r_dxrDebugEffect 1 shadow visibility
r_dxrDebugEffect 2 AO
r_dxrDebugEffect 3 sun
r_dxrDebugEffect 4 reflection approximation
r_dxrDebugEffect 5 GI approximation
```

## Производительность и стабильность

Сохранены проверенные настройки Performance v2:

```
r_dxrSafeMode 1
r_dxrAsyncSubmit 1
r_dxrCpuSync 0
r_dxrBuildInterval 2
r_dxrDispatchInterval 1
```

Кроме того, ожидание предыдущего DXR command list перенесено перед перезаписью upload constant/light buffers и shader-visible descriptors. Это закрывает гонку lifetime данных без добавления второго fence wait и без возврата к `glFinish()` на каждом кадре.

Full-resolution denoiser использует второй resolve-dispatch без дополнительных `TraceRay` в этом проходе, но всё равно расходует GPU-время и две full-resolution history/raw textures. AO, GI, sky, reflections и высокое число shadow samples увеличивают количество rays. Поэтому `ALL_BALANCED` является основным профилем, а `ALL_HIGH` — экспериментальным.

## Честные границы этого этапа

- PBR и bindless material texture lookup не добавлены.
- Reflections и GI — управляемые RT visibility/environment approximations, а не full-color material closest-hit path tracing.
- Alpha-tested cutout shadows пока не реализованы.
- Temporal accumulation без motion vectors.
- Реальный FPS и стабильность новых эффектов должны быть проверены на вашей видеокарте; базовая Performance v2 архитектура сохранена, но новые rays не бесплатны.

---

## Исправление v2: надёжное применение к рабочему репозиторию

В первой версии patch 08 был создан против одного точного состояния исходников Performance v2. Если в репозитории уже находились файлы предыдущих DarkWolf kits, `git apply` мог отклонить patch из-за несовпадения контекста, хотя базовый Performance v2 успешно применялся.

В v2 apply-скрипт сначала пытается применить обычный patch. При несовпадении контекста он автоматически устанавливает шесть проверенных source snapshots из `source-overrides/`, после чего обязательный verify-step проверяет cvars, shader markers, per-surface shadow-caster cache и структуру constant buffer. Это исправляет ошибку `Patch cannot be applied and is not already present` без отключения проверки исходников.

## Исправление v3 для MSVC C2026

В v3 встроенный HLSL разделён на несколько строковых chunks. Это устраняет `error C2026: string too big, trailing characters truncated`, не меняя сам shader и визуальные настройки. См. `README_MSVC_HLSL_BUILD_FIX_RU.md`.
