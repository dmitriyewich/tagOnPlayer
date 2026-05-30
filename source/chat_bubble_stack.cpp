#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "chat_bubble.h"
#include "external/MinHook/include/MinHook.h"

namespace {

// Зеркало CChatBubble::Player (stride 0x118). Массив таких лежит прямо по this
// объекта CChatBubble, поэтому reinterpret_cast<BubbleSlot*>(this) == m_player[].
struct BubbleSlot {
    BOOL m_bExists = FALSE;
    char m_szText[256]{};
    int m_creationTick = 0;
    int m_lifeSpan = 0;
    D3DCOLOR m_color = 0;
    float m_fDrawDistance = 0.0f;
    int m_nMaxLineLength = 0;
};

static_assert(sizeof(BubbleSlot) == 0x118, "BubbleSlot must match CChatBubble::Player stride");

// Снимок слота для очереди + собственная метка времени (GetTickCount): не зависит
// от внутренних часов SA:MP, истечение считаем по m_lifeSpan слота (длительность в мс).
struct QueuedBubble {
    BubbleSlot slot;
    DWORD addedTick = 0;
};

constexpr unsigned int kMaxPlayerSlots = 1004;
// Одна строка текста в единицах getChatBubbleHeight. Калибровка: при Δ0.088
// зазор был ~7 строк (rizin-скрин), т.е. строка ≈ 0.0125. Множитель distance в
// формуле бабла и в проекции сокращается, поэтому шаг постоянен в пикселях на
// любой дистанции/разрешении.
constexpr float kWorldLineStep = 0.0125f;
// Базовый подъём всего блока, чтобы новейший не заезжал на ник (в строках).
constexpr float kStackBaseLiftLines = 1.0f;

using ChatBubbleAddFn = std::int16_t(__thiscall*)(
    void*, unsigned int, const char*, D3DCOLOR, float, int);
using ChatBubbleDrawFn = void(__thiscall*)(void*);

chat_bubble::RuntimeConfig g_runtime;
ChatBubbleAddFn g_originalChatBubbleAdd = nullptr;
ChatBubbleDrawFn g_originalChatBubbleDraw = nullptr;

std::array<std::vector<QueuedBubble>, kMaxPlayerSlots> g_stackedBubbles;

float* HeightGlobalPtr() {
    if (g_runtime.version == nullptr || g_runtime.sampBase == 0
        || g_runtime.version->chatBubbleHeightGlobalRva == 0) {
        return nullptr;
    }
    return reinterpret_cast<float*>(
        g_runtime.sampBase + g_runtime.version->chatBubbleHeightGlobalRva);
}

unsigned int StackMax() {
    const int configured = g_runtime.stackMax;
    if (configured < 1) {
        return 1;
    }
    if (configured > static_cast<int>(chat_bubble::kMaxStackedBubblesHardMax)) {
        return chat_bubble::kMaxStackedBubblesHardMax;
    }
    return static_cast<unsigned int>(configured);
}

bool IsAlive(const QueuedBubble& entry, DWORD now) {
    if (entry.addedTick == 0 || !entry.slot.m_bExists) {
        return false;
    }
    if (entry.slot.m_lifeSpan <= 0) {
        return true;
    }
    return now - entry.addedTick < static_cast<DWORD>(entry.slot.m_lifeSpan);
}

// Снимок свежесозданного слота в очередь игрока: дедуп по тексту, иначе занимаем
// пустой/старейший. Очередь отсортирована новейшими вперёд (по addedTick).
void QueueSlotSnapshot(const BubbleSlot& fresh, unsigned int playerId, DWORD now) {
    auto& queue = g_stackedBubbles[playerId];
    const unsigned int cap = StackMax();
    if (queue.size() != cap) {
        queue.resize(cap);
    }

    QueuedBubble* dst = &queue[0];
    for (auto& candidate : queue) {
        if (candidate.addedTick != 0 && std::strcmp(candidate.slot.m_szText, fresh.m_szText) == 0) {
            dst = &candidate;
            break;
        }
        if (candidate.addedTick == 0) {
            dst = &candidate;
        } else if (dst->addedTick != 0 && candidate.addedTick < dst->addedTick) {
            dst = &candidate;
        }
    }

    dst->slot = fresh;
    dst->addedTick = now;
    std::sort(queue.begin(), queue.end(), [](const QueuedBubble& a, const QueuedBubble& b) {
        return a.addedTick > b.addedTick;
    });
}

// k-й живой бабл игрока в порядке «новейшие вперёд» (0 = новейший). nullptr — нет.
const BubbleSlot* NthAlive(const std::vector<QueuedBubble>& queue, unsigned int level, DWORD now) {
    unsigned int index = 0;
    for (const QueuedBubble& entry : queue) {
        if (!IsAlive(entry, now)) {
            continue;
        }
        if (index == level) {
            return &entry.slot;
        }
        ++index;
    }
    return nullptr;
}

// Число строк бабла с учётом переноса по ширине слота SA:MP.
int BubbleLineCount(const char* text) {
    const int wrapPx = g_runtime.version != nullptr
        ? static_cast<int>(g_runtime.version->sampBubbleDrawTextLinePx)
        : 0;
    const int lines = chat_bubble::MeasureBubbleLineCount(text, wrapPx);
    return lines > 0 ? lines : 1;
}

// Склейка живых баблов игрока в один многострочный текст: старые сверху, новый
// снизу (родной шрифт кладёт первую строку наверх). Цвет строки переносим через
// {RRGGBB} (m_color = 0xFFRRGGBB). При переполнении 256 байт отбрасываем старейшие.
// Возвращает число вошедших баблов.
int BuildCombined(const std::vector<QueuedBubble>& queue, DWORD now, char (&out)[256]) {
    char lines[chat_bubble::kMaxStackedBubblesHardMax][168];
    int lineLen[chat_bubble::kMaxStackedBubblesHardMax] = {};
    int count = 0;  // новейшие вперёд

    for (const QueuedBubble& entry : queue) {
        if (count >= static_cast<int>(chat_bubble::kMaxStackedBubblesHardMax)) {
            break;
        }
        if (!IsAlive(entry, now)) {
            continue;
        }
        const int written = std::snprintf(
            lines[count], sizeof(lines[count]), "{%06X}%s",
            entry.slot.m_color & 0x00FFFFFFu, entry.slot.m_szText);
        if (written <= 0) {
            continue;
        }
        lineLen[count] = (std::min)(written, static_cast<int>(sizeof(lines[count]) - 1));
        ++count;
    }

    if (count == 0) {
        out[0] = '\0';
        return 0;
    }

    int budget = static_cast<int>(sizeof(out) - 1);
    int keep = 0;  // сколько новейших баблов помещается
    for (int i = 0; i < count; ++i) {
        const int need = lineLen[i] + (i > 0 ? 1 : 0);
        if (budget - need < 0) {
            break;
        }
        budget -= need;
        ++keep;
    }
    if (keep == 0) {
        keep = 1;
    }

    int pos = 0;
    for (int i = keep - 1; i >= 0; --i) {  // от старейшего вошедшего к новейшему
        if (pos > 0 && pos < static_cast<int>(sizeof(out) - 1)) {
            out[pos++] = '\n';
        }
        const int copy = (std::min)(lineLen[i], static_cast<int>(sizeof(out) - 1) - pos);
        if (copy > 0) {
            std::memcpy(out + pos, lines[i], static_cast<size_t>(copy));
            pos += copy;
        }
    }
    out[pos] = '\0';
    return keep;
}

// Стек игрока рисуем ОДНИМ многострочным баблом — межстрочные интервалы и перенос
// делает сам SA:MP, поэтому внутренних наложений нет в принципе. Весь блок поднимаем
// так, чтобы новейший (нижние строки) оказался на штатной высоте бабла; калибровка
// kWorldLineStep влияет только на положение блока, не на интервалы внутри.
void DrawStacked(void* chatBubble) {
    BubbleSlot* const slots = reinterpret_cast<BubbleSlot*>(chatBubble);
    const DWORD now = GetTickCount();

    float* const heightGlobal = HeightGlobalPtr();

    static std::vector<unsigned int> stacked;
    stacked.clear();
    if (heightGlobal != nullptr) {
        for (unsigned int id = 0; id < kMaxPlayerSlots; ++id) {
            unsigned int alive = 0;
            for (const QueuedBubble& entry : g_stackedBubbles[id]) {
                if (IsAlive(entry, now) && ++alive >= 2) {
                    stacked.push_back(id);
                    break;
                }
            }
        }
    }

    if (stacked.empty()) {
        g_originalChatBubbleDraw(chatBubble);  // нет стеков — штатное поведение
        return;
    }

    static std::array<BubbleSlot, kMaxPlayerSlots> backup;
    std::memcpy(backup.data(), slots, sizeof(BubbleSlot) * kMaxPlayerSlots);

    // Уровень 0 без игроков со стеком (их слоты — одиночный новейший): остальные
    // игроки рисуются штатно, а стеки заменим многострочными баблами ниже.
    for (unsigned int id : stacked) {
        slots[id].m_bExists = FALSE;
    }
    g_originalChatBubbleDraw(chatBubble);

    const float origHeight = *heightGlobal;
    for (unsigned int id = 0; id < kMaxPlayerSlots; ++id) {
        slots[id].m_bExists = FALSE;  // чистый холст: по одному стеку за проход
    }

    for (unsigned int id : stacked) {
        const auto& queue = g_stackedBubbles[id];
        const BubbleSlot* const newest = NthAlive(queue, 0, now);
        if (newest == nullptr) {
            continue;
        }

        char combined[256];
        if (BuildCombined(queue, now, combined) < 2) {
            continue;
        }

        const int totalLines = BubbleLineCount(combined);
        const int newestLines = BubbleLineCount(newest->m_szText);
        const float raiseLines = static_cast<float>((std::max)(totalLines - newestLines, 0))
            + kStackBaseLiftLines + static_cast<float>(g_runtime.stackMarginPx) / 14.0f;

        slots[id] = *newest;  // цвет/дистанция/время от новейшего
        std::memcpy(slots[id].m_szText, combined, sizeof(slots[id].m_szText));
        slots[id].m_bExists = TRUE;

        *heightGlobal = origHeight + raiseLines * kWorldLineStep;
        g_originalChatBubbleDraw(chatBubble);
        slots[id].m_bExists = FALSE;
    }

    *heightGlobal = origHeight;
    std::memcpy(slots, backup.data(), sizeof(BubbleSlot) * kMaxPlayerSlots);
}

std::int16_t __fastcall ChatBubbleAddDetour(
    void* thisPtr,
    void* /*edx*/,
    unsigned int playerId,
    const char* text,
    D3DCOLOR color,
    float drawDistance,
    int lifeSpan) {
    if (g_originalChatBubbleAdd == nullptr) {
        return 0;
    }

    const std::int16_t result =
        g_originalChatBubbleAdd(thisPtr, playerId, text, color, drawDistance, lifeSpan);

    if (g_runtime.stackChatBubbles && thisPtr != nullptr && playerId < kMaxPlayerSlots) {
        const BubbleSlot& fresh = reinterpret_cast<const BubbleSlot*>(thisPtr)[playerId];
        if (fresh.m_bExists) {
            QueueSlotSnapshot(fresh, playerId, GetTickCount());
        }
    }

    return result;
}

void __fastcall ChatBubbleDrawDetour(void* thisPtr, void* /*edx*/) {
    if (g_originalChatBubbleDraw == nullptr) {
        return;
    }
    if (g_runtime.stackChatBubbles) {
        DrawStacked(thisPtr);
    } else {
        g_originalChatBubbleDraw(thisPtr);
    }
}

}  // namespace

namespace chat_bubble {

void SyncStackRuntime(const RuntimeConfig& config) {
    g_runtime = config;
    const unsigned int cap = StackMax();
    for (auto& queue : g_stackedBubbles) {
        queue.resize(cap);
    }
}

bool InstallStackHooks() {
    if (!g_runtime.stackChatBubbles || g_runtime.version == nullptr || g_runtime.sampBase == 0) {
        return true;
    }

    if (float* const heightGlobal = HeightGlobalPtr()) {
        DWORD oldProtect = 0;
        VirtualProtect(heightGlobal, sizeof(float), PAGE_EXECUTE_READWRITE, &oldProtect);
    }

    void* const addTarget =
        reinterpret_cast<void*>(g_runtime.sampBase + g_runtime.version->chatBubbleAddOffset);
    void* const drawTarget =
        reinterpret_cast<void*>(g_runtime.sampBase + g_runtime.version->chatBubbleDrawOffset);

    if (MH_CreateHook(
            addTarget,
            reinterpret_cast<void*>(&ChatBubbleAddDetour),
            reinterpret_cast<void**>(&g_originalChatBubbleAdd))
        != MH_OK) {
        return false;
    }
    if (MH_EnableHook(addTarget) != MH_OK) {
        return false;
    }

    if (MH_CreateHook(
            drawTarget,
            reinterpret_cast<void*>(&ChatBubbleDrawDetour),
            reinterpret_cast<void**>(&g_originalChatBubbleDraw))
        != MH_OK) {
        return false;
    }
    return MH_EnableHook(drawTarget) == MH_OK;
}

}  // namespace chat_bubble
