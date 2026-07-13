RT Effects Lab v2 apply fix

Исправлено падение GitHub Actions на шаге:
Apply stable Performance v2 plus RT Effects Lab

Причина: patch 08 зависел от точного line context одного локального состояния исходников.
Решение: при несовпадении patch-контекста apply-скрипт автоматически копирует проверенные
source snapshots из source-overrides и затем запускает строгую проверку исходников.

Скопируйте всё содержимое папки dxr_cleanvisual_rt_effects_lab_kit_v2 в корень
репозитория с заменой файлов и повторно запустите workflow:
DarkWolf DXR CleanVisual RT Effects Lab
