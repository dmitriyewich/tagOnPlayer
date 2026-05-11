# tagOnPlayer

<img width="480" height="270" alt="tagOnPlayer" src="https://github.com/user-attachments/assets/ec4425f7-baf5-4470-ac13-84a490e06d0b" />

ASI-плагин для **GTA San Andreas 1.0 US** + **SA:MP** (`samp.dll`, Win32): после штатных циклов **`CPlayerTags`** дорисовывает над **локальным** игроком те же элементы, что клиент рисует для удалённых — **`DrawLabel`** (ник и ID) и **`DrawHealthBar`**. Опционально — зеркало исходящего чата в **`CChatBubble`**.

**Исходники и релизы:** [dmitriyewich/tagOnPlayer](https://github.com/dmitriyewich/tagOnPlayer)

---

## Возможности

- Дорисовка тега **`Nick (ID)`** и полос **HP / броня** над локальным игроком (тот же API, что для удалённых).
- Переключение дорисовки командой в чат SA:MP (по умолчанию **`/tagon`**, настраивается в INI).
- **Зеркало чата** (`MirrorOwnChatBubble=1`): **`CChatBubble::Add`** + те же правки **`CChatBubble::Draw`**, что ниже (только хук на чат и зеркалирование строки).
- **Текст по команде** (секция **`[OverlayCommands]`**): тот же путь, что зеркало — **`CChatBubble::Add`** с цветом из правила и **`ChatBubbleLifeMs`**; хвост после команды в бабл; пустая команда — короткий **`Add`** для гашения. Патчи **`Draw`** включаются, если **`MirrorOwnChatBubble=1`** **или** в INI задан **`Count` > 0** для оверлея.

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
3. При первом запуске рядом с ASI используется/создаётся **`tagOnPlayer.ini`**, секция **`[Settings]`** (и опционально **`[OverlayCommands]`**).

---

## Настройки (`tagOnPlayer.ini`, секция `[Settings]`)

| Ключ | По умолчанию | Описание |
|------|----------------|----------|
| **`Command`** | `/tagon` | Команда в чат для вкл/выкл дорисовки (на сервер не уходит). |
| **`EnabledByDefault`** | `1` | `1` — дорисовка включена при старте; `0` — выкл. до первой команды. |
| **`MirrorOwnChatBubble`** | `0` | `1` — дублировать свой текст чата в бабл над головой. |
| **`ChatBubbleLifeMs`** | `6000` | Длительность зеркала (мс), диапазон **500–600000**. |

Пустой **`Command`** в файле заменяется значением по умолчанию; при старте ключи перезаписываются актуальными строками (как в коде). Секция **`[OverlayCommands]`** плагин **не** перезаписывает — правьте вручную.

### Текст над головой по команде (`[OverlayCommands]`)

До **16** правил. Ключ **`Count`** — сколько записей читать (**`Cmd1`…`CmdN`**, **`Color1`…`ColorN`**, **`Forward1`…`ForwardN`**). Сравнение команды с вводом — по **первому токену** без учёта регистра (после команды должен быть пробел или конец строки, чтобы **`/ame`** не совпало с **`/american`**).

| Ключ | Описание |
|------|-----------|
| **`Count`** | Число правил `1…N` (пусто или `0` — оверлей по командам отключён). |
| **`CmdN`** | Строка команды, например **`/ame`**. |
| **`ColorN`** | Цвет для **`CChatBubble::Add`** (см. ниже). |
| **`ForwardN`** | **`1`** (по умолчанию) — строка уходит в штатный **`SendCommand`** (выполняется **до** нашего **`CChatBubble::Add`**, иначе клиент перезапишет бабл своим цветом, напр. для **`/me`**); **`0`** — перехват без отправки на сервер. |

**Форматы `ColorN`** (dword **D3DCOLOR** в порядке **AARRGGBB**; см. [SA:MP wiki](https://sampwiki.blast.hk/wiki/Color_list) / [open.mp](https://open.mp/docs/scripting/resources/colorslist)):

- Целое **со знаком в десятичной записи**, например **`-6684673`** — то же 32-битное значение, что в скриптах.
- **`{RRGGBB}`** — шесть hex-цифр: красный, зелёный, синий; альфа в dword задаётся как **`FF`**.
- **`{AARRGGBB}`** — восемь hex-цифр: полный dword **D3DCOLOR** (альфа, красный, зелёный, синий).

Запись в INI — в **каноническом** порядке **AARRGGBB** (как у тегов и в wiki); перед **`CChatBubble::Add`** плагин перекодирует dword: у клиента разбор цвета бабла не совпадает с прямым **AARRGGBB** для **`DrawLabel`**.

Команда **без текста** (только токен или пробелы) — сброс бабла через **`Add`** с пустой строкой и минимальным сроком (как при зеркале — поведение клиента SA:MP).

Пример:

```ini
[OverlayCommands]
Count=2
Cmd1=/ame
Color1=-6684673
Forward1=1
Cmd2=/do
Color2={ff99ff}
Forward2=0
```

### Зеркало чата и локальный бабл (`CChatBubble`)

1. **MinHook** на **`localPlayerChatOffset`** — только при **`MirrorOwnChatBubble=1`** (исходящий текст чата без ведущего **`/`**). Цвет бабла зеркала — **белый** (**`0xFFFFFFFF`**, непрозрачный ARGB). Если оффсет **`0`** (у **R2** в таблице), хук не ставится.
2. При **`MirrorOwnChatBubble=1`** **или** при **`[OverlayCommands]`** с **`Count` > 0**: в **`CChatBubble::Draw`** — трамплин пустого слота пула для локального ID (патч **8** или **12** байт) и **6×NOP** на near-**`je`** у проверки видимости слота — иначе локальный **`Add`** не рисуется.

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
