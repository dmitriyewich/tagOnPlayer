# Context

## Назначение проекта

- `tagOnPlayer` это нативный `ASI`-плагин для `GTA San Andreas / SA:MP`.
- Плагин дорисовывает над локальным игроком те же элементы, которые обычно рисуются только для удалённых игроков:
  - `Nick (ID)`
  - полоску `health`
  - полоску `armour`
- Выходной артефакт проекта:
  - `tagOnPlayer.asi`

## Текущая рабочая логика

- Плагин ждёт, пока игра полностью догрузится, затем ждёт появления `samp.dll`.
- Версия `samp.dll` определяется по `AddressOfEntryPoint`.
- После детекта версии ставятся два detour-хука:
  - на цикл отрисовки label
  - на цикл отрисовки health bar
- После штатной отрисовки SA:MP плагин отдельно дорисовывает label и полоски уже для локального игрока.

## Важные файлы

- Основной код:
  - `C:\Games\CODEX\tagOnPlayer\source\main.cpp`
- Низкоуровневый release-shim:
  - `C:\Games\CODEX\tagOnPlayer\source\crt_shim.cpp`
- Проект Visual Studio:
  - `C:\Games\CODEX\tagOnPlayer\tagOnPlayer.vcxproj`
- Solution:
  - `C:\Games\CODEX\tagOnPlayer\tagOnPlayer.sln`
- Основной `README`:
  - `C:\Games\CODEX\tagOnPlayer\README.md`
- Этот контекст:
  - `C:\Games\CODEX\tagOnPlayer\context.md`

## Поддерживаемые версии SA:MP

- Текущий набор версий, зашитый в `source\main.cpp`:
  - `R1`
  - `R2`
  - `R3`
  - `R3-1`
  - `R4`
  - `R4-2`
  - `R5-1`
  - `DL-R1`

### Проверенные entry point и ключевые offsets

- `R1`
  - entry point: `0x31DF13`
  - label loop: `0x00070D40`
  - health loop: `0x0006FC30`
- `R2`
  - entry point: `0x3195DD`
  - label loop: `0x00070DE0`
  - health loop: `0x0006FCD0`
- `R3`
  - entry point: `0x0CC490`
  - label loop: `0x00074C30`
  - health loop: `0x00073B20`
  - ped_on_screen: `0x000AB430`
  - get_health: `0x000AB480`
  - get_armour: `0x000AB4C0`
  - get_bone: `0x000ADC00`
- `R3-1`
  - entry point: `0x0CC4D0`
  - label loop: `0x00074C30`
  - health loop: `0x00073B20`
  - ped_on_screen: `0x000AB450`
  - get_health: `0x000AB4C0`
  - get_armour: `0x000AB500`
  - get_bone: `0x000ADBF0`
- `R4`
  - entry point: `0x0CBCB0`
  - label loop: `0x00075360`
  - health loop: `0x00074240`
- `R4-2`
  - entry point: `0x0CBCD0`
  - label loop: `0x00075390`
  - health loop: `0x00074270`
- `R5-1`
  - entry point: `0x0CBC90`
  - label loop: `0x00075330`
  - health loop: `0x00074210`
- `DL-R1`
  - entry point: `0x0FDB60`
  - label loop: `0x00074DC0`
  - health loop: `0x00073CB0`

## Правила сборки

- Проект только `Win32`.
- Основной сценарий работы только `build -> release`.
- Использовать `MSBuild` только по этому пути:
  - `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe`
- Целевая конфигурация:
  - `Release | Win32`
- Ожидаемый выходной файл:
  - `C:\Games\CODEX\tagOnPlayer\build\Release\Win32\tagOnPlayer.asi`
- Никаких автоматических копирований сборки в папку игры.

### Текущий release-профиль

- `Release | Win32` должен оставаться в aggressive-профиле уменьшения размера.
- Для `Release` отключены default CRT libraries.
- DLL использует собственный entrypoint:
  - `DllMain@12`
- В `Release` оставлены только нужные импорты из `KERNEL32.dll`.
- `source\crt_shim.cpp` используется как локальный no-CRT shim для `memset/memcpy`, чтобы основной модуль оставался совместимым с aggressive-link.

### Результат по размеру

- До aggressive-профиля:
  - `tagOnPlayer.asi` = `113664` байт
- После aggressive-профиля:
  - `tagOnPlayer.asi` = `5120` байт

## Правила публикации

- В проекте может существовать разовая папка публикации:
  - `C:\Games\CODEX\tagOnPlayer\publish`
- Архивы публикации:
  - `C:\Games\CODEX\tagOnPlayer\publish\source.zip`
  - `C:\Games\CODEX\tagOnPlayer\publish\release.zip`
- Обновлять или пересобирать публикацию можно только по прямой команде пользователя.
- Основной рабочий сценарий проекта всё равно только `build -> release`.

## Правила `reference`

- Папка `C:\Games\CODEX\tagOnPlayer\reference` зарезервирована под:
  - проверочные `samp.dll`
  - дизассемблерные заметки
  - вспомогательные референсные артефакты по offsets
- Такие материалы не нужно складывать в корень проекта.

## Правила `.codex`

- В проекте должна существовать папка:
  - `C:\Games\CODEX\tagOnPlayer\.codex`
- Мои действия и краткий рабочий журнал нужно сохранять в `.codex`.
- После заметных изменений по проекту журнал в `.codex` нужно обновлять, чтобы в следующей сессии было видно, что уже делалось.

## Чистота проекта

- Корень проекта должен оставаться простым и читаемым.
- Исходники держать в `source/`.
- Release-артефакты держать в `build\Release\Win32`.
- Промежуточные файлы держать в `build\obj`.
- Временные reference-материалы и архивы публикации не должны захламлять корень проекта.
