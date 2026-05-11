# tagOnPlayer

<img width="480" height="270" alt="tagOnPlayer" src="https://github.com/user-attachments/assets/ec4425f7-baf5-4470-ac13-84a490e06d0b" />

ASI-плагин для **GTA San Andreas 1.0 US** + **SA:MP** (`samp.dll`, Win32): после штатных циклов **`CPlayerTags`** дорисовывает над **локальным** игроком те же элементы, что клиент рисует для удалённых — **`DrawLabel`** (ник и ID) и **`DrawHealthBar`**. Опционально — зеркало исходящего чата в **`CChatBubble`**.

**Исходники и релизы:** [dmitriyewich/tagOnPlayer](https://github.com/dmitriyewich/tagOnPlayer)

---

## Возможности

- Дорисовка тега **`Nick (ID)`** и полос **HP / броня** над локальным игроком (тот же API, что для удалённых).
- Переключение дорисовки командой в чат SA:MP (по умолчанию **`/tagon`**, настраивается в INI).
- **Зеркало чата** (`MirrorOwnChatBubble=1`): **`CChatBubble::Add`** + правки в **`CChatBubble::Draw`** в `samp.dll` (трамплин пустого слота пула для локального ID и обход `je` по флагу видимости слота); срок бабла — **`ChatBubbleLifeMs`**.

---

## Требования и ограничения

| Параметр | Значение |
|----------|----------|
| Игра | GTA San Andreas **1.0 US** |
| Архитектура | **Win32 (x86)** |
| SA:MP | Поддерживаемые `samp.dll` (по entry point): **R1, R2, R3, R3-1, R4, R4-2, R5-1, DL-R1** (таблица **`kSupportedVersions`** в **`source/main.cpp`**) |
| Неизвестный клиент | Хуки **не** ставятся |
| Ограничение | Не заменяйте **`tagOnPlayer.asi`** при запущенной игре |

**Канон RVA/оффсетов** — только код; в README числа не дублируются.

---

## Установка

1. Получите **`tagOnPlayer.asi`** ([релиз](https://github.com/dmitriyewich/tagOnPlayer/releases) или локальная [сборка](#сборка)).
2. Поместите **`tagOnPlayer.asi`** в каталог с **`gta_sa.exe`** (рядом с **`samp.dll`**, как принято для ASI).
3. При первом запуске рядом с ASI используется/создаётся **`tagOnPlayer.ini`**, секция **`[Settings]`**.

---

## Настройки (`tagOnPlayer.ini`, секция `[Settings]`)

| Ключ | По умолчанию | Описание |
|------|----------------|----------|
| **`Command`** | `/tagon` | Команда в чат для вкл/выкл дорисовки (на сервер не уходит). |
| **`EnabledByDefault`** | `1` | `1` — дорисовка включена при старте; `0` — выкл. до первой команды. |
| **`MirrorOwnChatBubble`** | `0` | `1` — дублировать свой текст чата в бабл над головой. |
| **`ChatBubbleLifeMs`** | `6000` | Длительность зеркала (мс), диапазон **500–600000**. |

Пустой **`Command`** в файле заменяется значением по умолчанию; при старте ключи перезаписываются актуальными строками (как в коде).

### Зеркало чата (`MirrorOwnChatBubble=1`)

1. **MinHook** на функцию по RVA **`localPlayerChatOffset`** (вывод исходящего текста в чат). Если оффсет **`0`** для вашей строки таблицы (у **R2** так), этот путь не подключается.
2. В **`CChatBubble::Draw`**: трамплин «пустой слот пула» для локального ID (патч **8** или **12** байт по версии) и **6×NOP** на near-**`je`** после проверки флага видимости слота.

---

## Сборка

- Visual Studio, конфигурация **`Release`**, платформа **`Win32`**.
- Из корня репозитория (путь к MSBuild подправьте под свою VS):

```text
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" tagOnPlayer.sln /p:Configuration=Release /p:Platform=Win32
```

- Артефакт: **`build\Release\Win32\tagOnPlayer.asi`**.

CI: **`.github/workflows/build-release-win32.yml`**.

---

## Автор

**[@dmitriyewich](https://github.com/dmitriyewich)** — [tagOnPlayer](https://github.com/dmitriyewich/tagOnPlayer)
