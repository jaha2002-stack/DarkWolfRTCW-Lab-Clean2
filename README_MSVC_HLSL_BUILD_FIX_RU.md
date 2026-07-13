# RT Effects Lab v3 — MSVC embedded HLSL build fix

Эта версия исправляет ошибку Visual C++:

```
gl_d3d12raylight.cpp(...): error C2026: string too big, trailing characters truncated
```

Причина: весь HLSL RT Effects Lab (около 28.7 КБ) был помещён в один C++ raw string literal. MSVC ограничивает размер одного строкового литерала и обрывал его во время компиляции.

Исправление:

- HLSL разделён на 5 независимых raw-string chunks менее 8 КБ каждый;
- chunks объединяются в `std::string` только во время инициализации DXR pipeline;
- в DXC передаётся явный размер собранного исходника;
- содержимое HLSL не изменено ни на один символ;
- графические эффекты, cvars, разрешение и Performance v2 настройки не урезаны;
- workflow проверяет количество и размер chunks до MSBuild.

Новый патч:

```
patches/09-dxr-lab-msvc-split-embedded-hlsl.patch
```

Для репозитория, где Lab Kit v2 уже скопирован, достаточно заменить файлы из маленького BuildFix архива и повторно запустить workflow:

```
DarkWolf DXR CleanVisual RT Effects Lab
```
