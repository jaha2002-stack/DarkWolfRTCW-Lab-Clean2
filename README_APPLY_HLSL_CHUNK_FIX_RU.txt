DarkWolfRTCW RT Effects Playable v6
HLSL embedded chunk 8000-byte build fix

Причина:
Один embedded HLSL raw-string chunk имел 7921 байт с LF, но 8096 байт
после CRLF-преобразования на Windows runner. Проверка v6 корректно остановила
сборку до MSVC/DXC.

Исправление:
Проблемный HLSL chunk разделен на два массива строк. Итоговый HLSL-код
байт-в-байт и семантически не изменен после конкатенации.

Применение через веб-интерфейс GitHub:
1. Откройте папку repo-overlay из этого архива.
2. Перетащите СОДЕРЖИМОЕ repo-overlay в Add file -> Upload files в корне репозитория.
3. Подтвердите замену двух файлов:
   source-overrides/src/opengl/gl_d3d12raylight.cpp
   src/opengl/gl_d3d12raylight.cpp
4. Commit message:
   Fix Playable v6 embedded HLSL chunk size on Windows
5. Повторно запустите workflow DarkWolf RTCW RT Effects Playable v6.

Проверено:
- конкатенированный HLSL до и после исправления имеет одинаковый SHA-256;
- число chunks: 7 -> 8;
- максимальный chunk с LF: 7537 байт;
- максимальный chunk при CRLF: 7823 байта;
- все chunks меньше лимита 8000 байт даже при CRLF.
