# DarkWolfRTCW RT Final Stable Visible v7

Это полный source-overlay для текущего репозитория DarkWolfRTCW. Он не является набором одних CFG: архив заменяет C++/embedded HLSL snapshots и добавляет отдельный GitHub Actions workflow.

## Установка через GitHub Web

1. Распакуйте архив.
2. Откройте папку `repo-overlay`.
3. Загрузите **содержимое** `repo-overlay` в корень репозитория.
4. Папку `patches` загружать не требуется.
5. Commit message:

```text
Apply DarkWolfRTCW RT Final Stable Visible v7
```

6. В Actions запустите:

```text
DarkWolf RTCW RT Final Stable Visible v7
```

7. Выберите `Release`.
8. Скачайте artifact:

```text
DarkWolfRTCW-RT-Final-Stable-Visible-v7-Release
```

9. Скопируйте artifact поверх установленной игры.

## Первый запуск

Сначала запустите:

```text
RUN_RT_V7_SCREENSHOT_LOOK.bat
```

Это профиль, восстановленный по значениям с присланного скриншота, но с spatial denoiser и стабильным synchronous path.

Основной более умеренный вариант:

```text
RUN_RT_V7_FINAL_STABLE.bat
```

Вариант без camera-side fill:

```text
RUN_RT_V7_REAL_LIGHTS_ONLY.bat
```

## Проверка версии

Запустите `RUN_RT_V7_DEBUG.bat` или введите:

```cfg
developer 1
logfile 2
r_dxrDebug 1
```

В `rtcwconsole.log` должны быть строки:

```text
DXR v7:
DXR v7 CPU CB: composite=clean-release-visible
```

Если печатается v6.x, старый `WolfSP.exe` не был заменён.

## Важно

Не выполняйте `vid_restart` после запуска. Все игровые RT-параметры профиля применяются без перезапуска renderer; latched-параметры передаются BAT-файлом до запуска.
