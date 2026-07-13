# DarkWolfRTCW DXR Clean Visual Stability Kit

Этот kit сделан специально для сценария: **сохранить вид старого `DarkWolfRTCW_DXR_Clean_Release_Build_Kit.zip`, но добавить стабильность**.

Он намеренно НЕ включает поздние ветки:

- HalfRes Performance Cache
- TDRSafe forced low resolution
- Playable smooth composite
- Final HighQuality smooth presets
- TrueRT M1/M2/M3 material-lite

## Что внутри

Patch stack:

1. `00-build-fix-Com_Printf.patch`
2. `02-dxr-stable-mode.patch`
3. `03-pvs-decompressvis-x64.patch`
4. `04-dxr-visibility-debug.patch`
5. `05-dxr-clean-visual-stability-only.patch`

Patch 05 не меняет визуальный стиль. Он добавляет только защиту:

- `r_dxrSafeMode`
- `r_dxrErrorLimit`
- `r_dxrFenceWaitMs`
- обработку `DXGI_ERROR_DEVICE_REMOVED / RESET / HUNG`
- timeout для D3D12 fence wait
- отключение дальнейшей DXR-работы после device lost вместо бесконечного зависания/краша

## Главный запуск

`RUN_CLEAN_VISUAL_RT_STABLE.bat`

Это старый Clean Release visual profile:

```text
r_dxr 1
r_dxrFallbackLight 1
r_dxrFallbackLightIntensity 8.0
r_dxrFallbackLightRadius 900
r_dxrAmbientIntensity 1.35
r_dxrLegacyBlend 0.65
r_dxrExposure 1.15
r_dxrShadowBias 0.05
```

## Workflow

`DarkWolf DXR Clean Visual Stability Build`

Artifact:

`DarkWolf-DXR-Clean-Visual-Stability-Release`

## Важное ограничение

Этот kit не делает магию с производительностью. Он сохраняет старый красивый Clean Release DXR path, поэтому может быть тяжелее поздних safe/smooth веток. Его задача — **не убить старую картинку**, а убрать фатальные зависания и device-lost краши насколько возможно без переписывания визуального path.
