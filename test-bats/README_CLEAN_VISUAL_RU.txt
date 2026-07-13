DarkWolf RTCW DXR Clean Visual Stability
========================================

Этот kit НЕ меняет визуальный стиль на поздние smooth/half-res/material-lite версии.
Он основан на старом Clean Release, где были заметны RT-тени, specular и блики.

Главный запуск:
  RUN_CLEAN_VISUAL_RT_STABLE.bat

Если хочешь максимально близко к старому поведению:
  RUN_CLEAN_VISUAL_RT_ORIGINAL_RISKY.bat

Сброс:
  RUN_RESET_SAFE_NO_DXR.bat

Что делает stability guard:
- не режет разрешение DXR;
- не включает half-res composite;
- не включает freeze scene;
- не меняет lighting/composite старого Clean Release;
- перехватывает DXGI_ERROR_DEVICE_REMOVED / RESET / HUNG;
- при потере D3D12 device отключает дальнейшую DXR-работу до vid_restart вместо бесконечного спама/краша;
- ограничивает ожидание fence, чтобы игра не висела бесконечно.
