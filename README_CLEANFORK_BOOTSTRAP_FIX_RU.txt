DarkWolfRTCW DXR CleanVisual RT Effects Lab - Clean Fork Bootstrap Fix

Назначение:
Этот маленький hotfix нужен, если вы сделали web Fork текущего jmarshall23/DarkWolf и workflow падает на:
Base patch cannot be applied cleanly: patches/02-dxr-stable-mode.patch

Причина:
Текущий upstream/fork имеет другое состояние исходников и line-context patch 02 больше не совпадает.
Этот fix заменяет apply-dxr-cleanvisual-rt-effects-lab.ps1 и устанавливает проверенные исходные snapshots напрямую.

Как применять через веб GitHub:
1. Откройте свой репозиторий DarkWolfRTCW-Lab-Clean2.
2. Нажмите Add file -> Upload files.
3. Перетащите содержимое папки dxr_cleanvisual_rt_effects_lab_cleanfork_bootstrap_fix в корень репозитория.
   Важно: не саму папку, а ее содержимое: scripts, source-overrides, README.
4. Commit changes.
5. Запустите workflow DarkWolf DXR CleanVisual RT Effects Lab снова.

Что заменяется:
- scripts/apply-dxr-cleanvisual-rt-effects-lab.ps1
- source-overrides/src/opengl/gl_d3d12raylight.cpp
- source-overrides/src/opengl/opengl.h
- source-overrides/src/opengl/gl_d3d12shim.cpp
- source-overrides/src/renderer/tr_backend.cpp
- source-overrides/src/renderer/tr_bsp.cpp
- source-overrides/src/renderer/tr_init.cpp
- source-overrides/src/renderer/tr_local.h
- source-overrides/src/botlib/be_aas_route.cpp

Этот fix не меняет BAT/CFG профили и не урезает RT Effects Lab настройки.
