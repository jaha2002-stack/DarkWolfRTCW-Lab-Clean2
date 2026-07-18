# Анализ DarkWolfRTCW_DXR_Clean_Release_Build_Kit

## Что фактически создало видимую картинку на приложенном скриншоте

Скриншот и исходный kit совпадают по параметрам:

- `fallback=1`
- `radius=900`
- `intensity=8.00`
- `ambient=1.35`
- `legacy=0.65`
- `exposure=1.15`
- `bias=0.0500`

Старый shader использовал простой линейный composite:

```hlsl
rtLitColor = albedo * lightingAccum + specularAccum;
finalColor = lerp(rtLitColor, baseAlbedo, legacyBlend) * exposure;
```

Именно этот путь делал RT-свет, RT-тени и specular заметными. Он не содержал поздних `authoredHeadroom`, микроскопических лимитов компонентов или многоступенчатого подавления RT-residual.

Ключевую роль в конкретном скриншоте играл тёплый camera-side fallback light. Старый patch добавлял его даже при наличии реальных map lights. Поэтому профиль `dxr_v7_screenshot_look.cfg` сохраняет этот режим явно и отдельно.

## Почему старый build тормозил

Старый build обрабатывал все найденные lights. На скриншоте видно `lights=66`. Каждый пиксель проверял большое число point/rect lights и shadow rays.

Кроме того, использовались:

- полное разрешение;
- синхронные BLAS/TLAS/dispatch waits;
- `glFinish()`;
- несколько samples у rect lights;
- отсутствие эффективного выбора ближайших/важных lights;
- отсутствие качественной фильтрации шума.

Это давало видимый RT, но высокую GPU/CPU стоимость и выраженный screen-door/grain pattern.

## Почему поздние v6-профили выглядели слабо

В поздней ветке были добавлены полезные механизмы стабильности, но одновременно энергия света была сильно уменьшена:

- emissive rect light переводился через `log2(1 + intensity) * 0.27`;
- point light ограничивался примерно `3.5`;
- rect light ограничивался примерно `2.2`;
- camera fill разрешался только при `r_dxrDebug 1`;
- gameplay composite неоднократно ослаблял RT-компоненты.

Диагностические режимы при этом работали, потому что обходили gameplay composite.

## Что делает v7

v7 соединяет две доказанные части:

1. Видимый Clean Release-подобный линейный relighting/composite.
2. Стабильный synchronous submission path:
   - `r_dxrAsyncSubmit 0`
   - `r_dxrCpuSync 1`
   - `r_dxrBuildInterval 1`
   - `r_dxrDispatchInterval 1`
   - timeout и safe device-lost handling.

Дополнительно сохранены:

- правильный G-buffer layout: `position.w = geometry flag`, `normal.w = roughness`;
- importance selection lights;
- AO, sun, sky, reflections, GI и specular;
- spatial denoiser без temporal ghosting;
- direct debug modes;
- CPU constant-buffer logging.

Энергия emissive lights восстановлена до видимого диапазона, но остаётся ограниченной:

```cpp
log2(1 + rawIntensity) * 0.55
```

Профили используют caps `8.0` для point lights и `4.5-5.5` для rect lights вместо слишком слабых поздних значений.

## Профили

- `RUN_RT_V7_FINAL_STABLE.bat` — основной стабильный красивый профиль с умеренным Clean Release fill light.
- `RUN_RT_V7_SCREENSHOT_LOOK.bat` — максимально близок к приложенному скриншоту: intensity 8, ambient 1.35, legacy 0.65, exposure 1.15.
- `RUN_RT_V7_REAL_LIGHTS_ONLY.bat` — тот же composite без camera-side fill light.
- `RUN_RT_V7_DEBUG.bat` — логирование `DXR v7` и `DXR v7 CPU CB`.

## Ограничение проверки

Статическая структура C++, embedded HLSL, профили и workflow проверяются до сборки. Настоящая компиляция MSVC/DXC выполняется GitHub Actions. Финальный визуальный результат и стабильность на конкретной DXR-видеокарте подтверждаются только игровым тестом.
