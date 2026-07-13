DarkWolfRTCW DXR CleanVisual RT Effects Lab - Game function table build fix
============================================================================

Этот hotfix заменяет scripts/build-windows-clean-release.ps1.

Причина ошибки:
  game.vcxproj на чистом GitHub Actions checkout запускает генератор таблицы
  game functions. Первый проход может вывести:
    Building game function table...
    Updated the function table, recompile required.
  После этого линковка может упасть на отсутствующем .\\Release\\g_save.obj.

Исправление:
  build script теперь повторяет сборку src\\game\\game.vcxproj один раз.
  Если это был штатный first-pass table update, второй проход должен собрать
  qagamex64.dll. Если ошибка настоящая, второй проход тоже упадет и workflow
  корректно остановится.

Как применить через GitHub Web:
  1. Распакуй архив.
  2. Открой папку dxr_cleanvisual_rt_effects_lab_game_buildfix.
  3. В репозитории нажми Add file -> Upload files.
  4. Перетащи папку scripts в корень репозитория с заменой файла.
  5. Commit changes.
  6. Снова запусти workflow DarkWolf DXR CleanVisual RT Effects Lab.
