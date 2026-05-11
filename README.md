# tagOnPlayer
![Анимация2](https://github.com/user-attachments/assets/ec4425f7-baf5-4470-ac13-84a490e06d0b)

ASI-плагин, который дорисовывает над локальным игроком:

- `CPlayerTags::DrawLabel` с текстом `Nick (ID)`
- `CPlayerTags::DrawHealthBar`
- зеркало исходящего чата: `MirrorOwnChatBubble=1` — **`CChatBubble::Add`** + обход **`CChatBubble::Draw`** (в `samp.dll` near-**`je`** после проверки флага видимости слота, в т.ч. локальный игрок); срок **`ChatBubbleLifeMs`**

Конфиг `tagOnPlayer.ini`, секция `[Settings]`:

- `Command`, `EnabledByDefault` — как раньше
- `MirrorOwnChatBubble` (0/1, по умолчанию 0) — свой текст чата над головой
- `ChatBubbleLifeMs` (по умолчанию **6000**, как типичный последний аргумент `SetPlayerChatBubble` на сервере в миллисекундах; допустимый диапазон 500–600000) — срок для зеркала чата

Проверенные адреса (в т.ч. RVA **`CChatBubble::Draw`**, **`Add`**, **`RefChatBubble`**, точка патча **`je`** в **`Draw`**):

- `R1`, `R2`, `R3`, `R3-1`, `R4`, `R4-2`, `R5-1`, `DL-R1` (`samp.dll` из набора `C:\Games\SAMP\main\…`, 2026-05)
