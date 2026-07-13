# DarkWolfRTCW DXR Clean Visual Performance Kit

Цель этого kit-а: сохранить картинку `Clean Visual Stability` без изменений и убрать главные искусственные тормоза CPU/GPU.

## Что не меняется

- полный размер DXR lighting texture;
- старый Clean Release composite;
- оригинальные текстуры RTCW;
- видимые тени, specular и fallback lighting;
- значения ambient / legacy blend / exposure;
- никаких half-res, квадратов, smooth replacement или material-lite.

## Что оптимизировано

1. Удалён обязательный `glFinish()` перед каждым DXR pass.
2. Убран немедленный CPU fence wait после каждого DispatchRays; порядок сохраняется общей D3D12 queue.
3. Одинаковый RTCW surface больше не загружается и не помечает BLAS dirty несколько раз в одном кадре.
4. TLAS/BLAS scene update может выполняться раз в 2–3 кадра, при этом предыдущий валидный TLAS продолжает использоваться.
5. Опционально полный DXR output переиспользуется 1–2 кадра без снижения разрешения и без block upscale.

## Главные режимы

- `RUN_CLEAN_VISUAL_RT_BALANCED.bat`: DXR каждый кадр, scene update раз в 2 кадра.
- `RUN_CLEAN_VISUAL_RT_PERFORMANCE.bat`: основной игровой, DXR/scene update раз в 2 кадра.
- `RUN_CLEAN_VISUAL_RT_MAXFPS.bat`: обновление раз в 3 кадра.
- `RUN_CLEAN_VISUAL_RT_EXACT_ORIGINAL.bat`: старый синхронный эталон.

## Workflow

`DarkWolf DXR Clean Visual Performance Build`

Artifact:

`DarkWolf-DXR-Clean-Visual-Performance-Release`

## Важно

Этот patch не обещает одинаковый прирост на всех GPU. Он убирает подтверждённые полные синхронизации и повторную работу, не заменяя визуальный path. Если `BALANCED` уже даёт хороший FPS, используйте его: там DispatchRays остаётся каждый кадр.

## v2: Apply-step exit code fix

Исправлен ложный `Process completed with exit code 1` после успешного применения
validated source transformations. Скрипт теперь очищает ожидаемый ненулевой
код от `git apply --check` и явно завершает успешный apply-step кодом 0.
