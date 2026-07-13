DarkWolfRTCW DXR CleanVisual RT Effects Lab Runtime DLL Packaging Fix
====================================================================

Исправляет упаковку релиза: build script теперь копирует в dist не только
OpenAL32.dll / dxcompiler.dll / dxil.dll, но и обязательные Streamline/DLSS
runtime DLL, включая sl.interposer.dll.

Причина ошибки:
  WolfSP.exe напрямую импортирует sl.interposer.dll. Если этот файл отсутствует
  рядом с WolfSP.exe, Windows завершает запуск ещё до старта игры с сообщением:
  "Не удается продолжить выполнение кода, поскольку система не обнаружила
  sl.interposer.dll".

Через GitHub Web:
  1. Распаковать архив.
  2. Открыть папку dxr_cleanvisual_rt_effects_lab_runtime_dll_packaging_fix.
  3. Загрузить содержимое папки в корень репозитория через Add file -> Upload files.
  4. Commit changes.
  5. Запустить workflow DarkWolf DXR CleanVisual RT Effects Lab заново.

После исправления artifact должен содержать как минимум:
  WolfSP.exe
  OpenAL32.dll
  dxcompiler.dll
  dxil.dll
  sl.interposer.dll

Также при наличии в корне репозитория будут упакованы:
  sl.common.dll, sl.dlss.dll, sl.reflex.dll, sl.nis.dll, sl.nvperf.dll и др.
  nvngx_dlss.dll, nvngx_dlssd.dll, nvngx_dlssg.dll, nvngx_deepdvc.dll
  NvLowLatencyVk.dll
