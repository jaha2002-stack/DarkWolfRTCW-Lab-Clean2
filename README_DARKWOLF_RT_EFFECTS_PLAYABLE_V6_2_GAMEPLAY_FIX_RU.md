# DarkWolfRTCW RT Effects Playable v6.2

## Verified Gameplay Composite Fix

Этот небольшой source-патч применяется поверх **v6.1 Debug and Component
Pipeline Fix**. Тесты подтвердили нормальный прямой вывод режимов `12`, `8`,
`3` и `6`: цепочка cvar → CPU constant buffer → HLSL → UAV → игровой экран
работает, `gAlbedoTex` читается корректно, а sun и direct-light компоненты
вычисляются с нормальной яркостью.

v6.2 исправляет обычный режим `r_dxrDebugEffect 0`, где низкоэнергетические
RT-компоненты раньше почти исчезали на фоне уже освещённого RTCW framebuffer.

## Изменения исходного кода

- Direct light, specular, reflections и GI перед смешиванием переводятся в
  ограниченный perceptual range. Нулевой компонент остаётся строго нулевым,
  поэтому все существующие cvars продолжают работать как A/B-переключатели.
- У каждого компонента отдельный предел добавочной энергии.
- Чем ярче исходный RTCW lightmap, тем меньше дополнительная RT-энергия. Это
  сохраняет детали и снижает риск полностью белых комнат.
- RT shadows и AO сильнее, но безопасно модулируют оригинальный lightmap.
- HDR guard и highlight compression включаются до итогового tonemap.
- Основной Balanced-профиль усилен до заметного игрового уровня.
- Диагностический camera fallback light и debug views принудительно выключаются
  при запуске кампании.
- Сохранён подтверждённый стабильный режим:

```cfg
r_dxrAsyncSubmit 0
r_dxrCpuSync 1
```

- Стартового `vid_restart` нет.
- DebugEffect `1–12` из v6.1 сохранены и по-прежнему обходят denoiser,
  temporal, tonemap и bloom.
- При `r_dxrDebug 1` печатается `DXR v6.2 CPU CB:` с фактическими значениями
  direct/lightmap/AO/shadow/HDR и количеством выбранных lights.

## Установка через GitHub Web

1. Распакуйте ZIP патча.
2. Откройте папку `repo-overlay`.
3. В корне GitHub-репозитория выберите `Add file → Upload files`.
4. Перетащите **содержимое** `repo-overlay`, а не саму папку.
5. Папку `patches` загружать не нужно.
6. Создайте commit и запустите workflow:

```text
DarkWolf RTCW RT Effects Playable v6.2
```

Скачайте артефакт:

```text
DarkWolfRTCW-RT-Effects-Playable-v6.2-Release
```

Распакуйте его рядом с `WolfSP.exe` и запускайте:

```text
RUN_RT_ALL_BALANCED.bat
```

## Быстрая A/B-проверка

После загрузки карты переключайте один эффект и каждый раз сбрасывайте историю:

```cfg
r_dxrShadows 0
r_dxrShadows 1
r_dxrHistoryReset 1

r_dxrSun 0
r_dxrSun 1
r_dxrHistoryReset 1

r_dxrDynamicLights 0
r_dxrDynamicLights 1
r_dxrHistoryReset 1

r_dxrAO 0
r_dxrAO 1
r_dxrHistoryReset 1

r_dxrReflections 0
r_dxrReflections 1
r_dxrHistoryReset 1

r_dxrGI 0
r_dxrGI 1
r_dxrHistoryReset 1

r_dxrSpecular 0
r_dxrSpecular 1
r_dxrHistoryReset 1
```

Возврат к основному профилю:

```cfg
exec dxr_v6_all_balanced.cfg
```

При проблеме включите только логирование, не fallback-light:

```cfg
developer 1
logfile 2
r_dxrDebug 1
```
