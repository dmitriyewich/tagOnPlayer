#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "chat_bubble.h"
#include "external/MinHook/include/MinHook.h"

extern "C" void ChatBubbleRemoteChainLoadGuardTrampoline();
extern "C" void ChatBubbleRemoteChainPedGuardTrampoline();
extern "C" void ChatBubblePoolNullTrampoline_EcxJe8();
extern "C" void ChatBubblePoolNullTrampoline_EdxJe8();
extern "C" void ChatBubblePoolNullTrampoline_EcxLeaJe11();

namespace {

constexpr std::size_t kLocalSkipJePatchBytes = 6;
constexpr std::size_t kPoolNullTrampPatchBytesMax = 11;
constexpr std::size_t kRemoteChainGuardPatchBytes = 5;

using LocalPlayerChatFn = void(__thiscall*)(void*, const char*);

chat_bubble::RuntimeConfig g_runtime;
LocalPlayerChatFn g_originalLocalPlayerChat = nullptr;

HDC g_measureDc = nullptr;
HFONT g_measureFont = nullptr;
HFONT g_measureOldFont = nullptr;

void* g_poolResume = nullptr;
void* g_poolResumeMid = nullptr;
void* g_poolSkip = nullptr;
void* g_remoteChainLoadResume = nullptr;
void* g_remoteChainPedResume = nullptr;

template <typename T>
T* ReadGlobalObject(std::uint32_t offset) {
    if (g_runtime.sampBase == 0) {
        return nullptr;
    }
    return *reinterpret_cast<T**>(g_runtime.sampBase + offset);
}

template <typename Ret, typename... Args>
Ret CallThis(void* thisPtr, std::uint32_t offset, Args... args) {
    using Function = Ret(__thiscall*)(void*, Args...);
    return reinterpret_cast<Function>(g_runtime.sampBase + offset)(thisPtr, args...);
}

std::uint16_t GetLocalPlayerId(void* playerPool) {
    if (playerPool == nullptr || g_runtime.version == nullptr) {
        return 0;
    }
    return *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const std::uint8_t*>(playerPool) + g_runtime.version->localPlayerIdOffset);
}

bool MeasureEnsure() {
    if (g_measureDc != nullptr) {
        return true;
    }

    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }

    g_measureDc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (g_measureDc == nullptr) {
        return false;
    }

    g_measureFont = CreateFontA(
        -11,
        0,
        0,
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        "Arial");
    if (g_measureFont == nullptr) {
        DeleteDC(g_measureDc);
        g_measureDc = nullptr;
        return false;
    }

    g_measureOldFont = static_cast<HFONT>(SelectObject(g_measureDc, g_measureFont));
    return true;
}

int MeasureCharWidthA(char c) {
    char s[2] = {c, '\0'};
    SIZE sz = {};
    if (g_measureDc == nullptr || !GetTextExtentPoint32A(g_measureDc, s, 1, &sz)) {
        return 7;
    }
    return static_cast<int>(sz.cx);
}

void AdvanceLineLayout(int maxLinePx, int& linePx, const char* s, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        const int w = MeasureCharWidthA(s[i]);
        if (linePx + w > maxLinePx) {
            linePx = w;
        } else {
            linePx += w;
        }
    }
}

bool ColorEmbedSplitsMidLine(int maxLinePx, int linePx, const char embed[9]) {
    int x = linePx;
    for (int i = 0; i < 8; ++i) {
        const int w = MeasureCharWidthA(embed[i]);
        if (x + w <= maxLinePx) {
            x += w;
            continue;
        }
        if (i > 0) {
            return true;
        }
        x = w;
    }
    return false;
}

const char* SkipSpaces(const char* p) {
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

bool IsHexDigit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

std::uint8_t HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<std::uint8_t>(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<std::uint8_t>(10 + c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<std::uint8_t>(10 + c - 'a');
    }
    return 0;
}

/** Упаковка **RRGGBBAA** в `D3DCOLOR`, как ожидает аргумент `CChatBubble::Add` (samp: `(arg>>8)|0xFF000000` → AARRGGBB для Draw). */
bool PackD3dRrggbbaaFromRrggbb6(const char* digits, D3DCOLOR* outColor) {
    if (outColor == nullptr) {
        return false;
    }
    const std::uint32_t rr = (HexValue(digits[0]) << 4) | HexValue(digits[1]);
    const std::uint32_t gg = (HexValue(digits[2]) << 4) | HexValue(digits[3]);
    const std::uint32_t bb = (HexValue(digits[4]) << 4) | HexValue(digits[5]);
    *outColor = (rr << 24) | (gg << 16) | (bb << 8) | 0xFFu;
    return true;
}

bool PackD3dRrggbbaaFromRrggbbaa8(const char* digits, D3DCOLOR* outColor) {
    if (outColor == nullptr) {
        return false;
    }
    const std::uint32_t rr = (HexValue(digits[0]) << 4) | HexValue(digits[1]);
    const std::uint32_t gg = (HexValue(digits[2]) << 4) | HexValue(digits[3]);
    const std::uint32_t bb = (HexValue(digits[4]) << 4) | HexValue(digits[5]);
    const std::uint32_t aa = (HexValue(digits[6]) << 4) | HexValue(digits[7]);
    *outColor = (rr << 24) | (gg << 16) | (bb << 8) | aa;
    return true;
}

char HexDigitUpper(std::uint8_t nibble) {
    nibble &= 0x0Fu;
    return static_cast<char>(nibble < 10 ? ('0' + nibble) : ('A' + (nibble - 10)));
}

void FormatCanonicalColorEmbedInto(char embed[9], D3DCOLOR rrggbbaa) {
    const std::uint32_t u = static_cast<std::uint32_t>(rrggbbaa);
    const std::uint8_t r = static_cast<std::uint8_t>((u >> 24) & 0xFFu);
    const std::uint8_t g = static_cast<std::uint8_t>((u >> 16) & 0xFFu);
    const std::uint8_t b = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
    embed[0] = '{';
    embed[1] = HexDigitUpper(static_cast<std::uint8_t>(r >> 4));
    embed[2] = HexDigitUpper(r);
    embed[3] = HexDigitUpper(static_cast<std::uint8_t>(g >> 4));
    embed[4] = HexDigitUpper(g);
    embed[5] = HexDigitUpper(static_cast<std::uint8_t>(b >> 4));
    embed[6] = HexDigitUpper(b);
    embed[7] = '}';
    embed[8] = '\0';
}

bool AppendCStringTrunc(char* out, std::size_t cap, std::size_t& used, const char* s) {
    if (out == nullptr || cap == 0 || s == nullptr) {
        return false;
    }
    const std::size_t n = std::strlen(s);
    if (used + n + 1 > cap) {
        return false;
    }
    std::memcpy(out + used, s, n);
    used += n;
    out[used] = '\0';
    return true;
}

bool AppendChar(char* out, std::size_t cap, std::size_t& used, char c) {
    if (used + 2 > cap) {
        return false;
    }
    out[used] = c;
    ++used;
    out[used] = '\0';
    return true;
}

bool AppendColorEmbed(
    char* out,
    std::size_t cap,
    std::size_t& used,
    int maxLinePx,
    int& linePx,
    D3DCOLOR canonicalArgb) {
    if (out == nullptr || cap == 0) {
        return false;
    }

    char embed[9] = {};
    FormatCanonicalColorEmbedInto(embed, canonicalArgb);
    if (maxLinePx > 0 && MeasureEnsure() && ColorEmbedSplitsMidLine(maxLinePx, linePx, embed)) {
        if (!AppendChar(out, cap, used, '\n')) {
            return false;
        }
        linePx = 0;
    }

    if (used + 8 + 1 > cap) {
        return false;
    }
    std::memcpy(out + used, embed, 8);
    used += 8;
    out[used] = '\0';

    if (maxLinePx > 0 && MeasureEnsure()) {
        AdvanceLineLayout(maxLinePx, linePx, embed, 8);
    }
    return true;
}

void TrimCopyRange(const char* begin, const char* endExclusive, char* dest, std::size_t destCap) {
    if (dest == nullptr || destCap == 0) {
        return;
    }

    const char* a = begin;
    while (a < endExclusive && (*a == ' ' || *a == '\t')) {
        ++a;
    }

    const char* b = endExclusive;
    while (b > a && (b[-1] == ' ' || b[-1] == '\t')) {
        --b;
    }

    const std::size_t n = static_cast<std::size_t>(b - a);
    const std::size_t copyN = (n >= destCap) ? (destCap - 1) : n;
    if (copyN > 0) {
        std::memcpy(dest, a, copyN);
    }
    dest[copyN] = '\0';
}

void SplitRestIntoSegments(
    const char* rest,
    const char* splitSubstr,
    char* seg0,
    char* seg1,
    std::size_t segCap) {
    if (seg0 == nullptr || seg1 == nullptr || segCap == 0) {
        return;
    }

    seg0[0] = '\0';
    seg1[0] = '\0';
    if (rest == nullptr) {
        return;
    }

    if (splitSubstr == nullptr || splitSubstr[0] == '\0') {
        TrimCopyRange(rest, rest + std::strlen(rest), seg0, segCap);
        return;
    }

    const char* hit = std::strstr(rest, splitSubstr);
    if (hit == nullptr) {
        TrimCopyRange(rest, rest + std::strlen(rest), seg0, segCap);
        return;
    }

    TrimCopyRange(rest, hit, seg0, segCap);
    const char* after = hit + std::strlen(splitSubstr);
    TrimCopyRange(after, after + std::strlen(after), seg1, segCap);
}

bool WriteBytes(const void* address, const void* data, std::size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(const_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    std::memcpy(const_cast<void*>(address), data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD restoredProtect = 0;
    VirtualProtect(const_cast<void*>(address), size, oldProtect, &restoredProtect);
    return true;
}

bool BuildJumpPatch(std::uint8_t* buffer, std::size_t size, const void* source, const void* destination) {
    if (size < 5) {
        return false;
    }

    std::memset(buffer, 0x90, size);
    buffer[0] = 0xE9;

    const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source);
    const auto destinationAddress = reinterpret_cast<std::uintptr_t>(destination);
    const auto delta =
        static_cast<long long>(destinationAddress) - static_cast<long long>(sourceAddress + 5);

    if (delta < std::numeric_limits<std::int32_t>::min()
        || delta > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(buffer + 1, &relative, sizeof(relative));
    return true;
}

bool LikelyHeapDataPointer(const void* p) {
    const auto u = reinterpret_cast<std::uintptr_t>(p);
    return u >= 0x00010000u && u <= 0x7FFEFFFFu;
}

bool LooksLikeNearJeSkip(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x0F && bytes[1] == 0x84;
}

bool LooksLikePoolNullEcxJe8(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x85 && bytes[1] == 0xC9 && bytes[2] == 0x0F && bytes[3] == 0x84;
}

bool LooksLikePoolNullEdxJe8(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x85 && bytes[1] == 0xD2 && bytes[2] == 0x0F && bytes[3] == 0x84;
}

bool LooksLikePoolNullEcxLeaJe11(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x85 && bytes[1] == 0xC9 && bytes[2] == 0x8D && bytes[3] == 0x04
        && bytes[4] == 0x97 && bytes[5] == 0x0F && bytes[6] == 0x84;
}

bool LooksLikeRemoteChainLoad(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x8B && bytes[1] == 0x40 && bytes[2] == 0x08 && bytes[3] == 0xEB
        && bytes[4] == 0x02;
}

bool LooksLikeRemoteChainPed(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x8B && bytes[1] == 0x48 && bytes[2] == 0x04 && bytes[3] == 0x85
        && bytes[4] == 0xC9;
}

bool ApplyPoolNullTrampoline() {
    if (g_runtime.version == nullptr || g_runtime.sampBase == 0) {
        return true;
    }

    const std::uint8_t kind = g_runtime.version->chatBubblePoolNullTrampKind;
    const std::uint32_t patchRva = g_runtime.version->chatBubblePoolNullTrampPatchRva;
    if (kind == kChatBubblePoolTrampNone || patchRva == 0) {
        return true;
    }

    const std::size_t patchBytes =
        (kind == kChatBubblePoolTrampEcxLeaJe11) ? kPoolNullTrampPatchBytesMax : 8u;
    void* const patchAt = reinterpret_cast<void*>(g_runtime.sampBase + patchRva);

    bool signatureOk = false;
    if (kind == kChatBubblePoolTrampEcxJe8) {
        signatureOk = LooksLikePoolNullEcxJe8(patchAt);
    } else if (kind == kChatBubblePoolTrampEdxJe8) {
        signatureOk = LooksLikePoolNullEdxJe8(patchAt);
    } else if (kind == kChatBubblePoolTrampEcxLeaJe11) {
        signatureOk = LooksLikePoolNullEcxLeaJe11(patchAt);
    }
    if (!signatureOk) {
        return false;
    }

    g_poolResume =
        reinterpret_cast<void*>(g_runtime.sampBase + g_runtime.version->chatBubblePoolNullTrampResumeRva);
    if (g_runtime.version->chatBubblePoolNullTrampResumeMidRva != 0) {
        g_poolResumeMid =
            reinterpret_cast<void*>(g_runtime.sampBase + g_runtime.version->chatBubblePoolNullTrampResumeMidRva);
    } else {
        g_poolResumeMid = nullptr;
    }
    g_poolSkip =
        reinterpret_cast<void*>(g_runtime.sampBase + g_runtime.version->chatBubblePoolNullTrampSkipRva);

    if (g_poolResumeMid == nullptr) {
        return false;
    }

    const void* trampEntry = nullptr;
    if (kind == kChatBubblePoolTrampEcxJe8) {
        trampEntry = reinterpret_cast<const void*>(&ChatBubblePoolNullTrampoline_EcxJe8);
    } else if (kind == kChatBubblePoolTrampEdxJe8) {
        trampEntry = reinterpret_cast<const void*>(&ChatBubblePoolNullTrampoline_EdxJe8);
    } else {
        trampEntry = reinterpret_cast<const void*>(&ChatBubblePoolNullTrampoline_EcxLeaJe11);
    }

    std::uint8_t patchBuf[kPoolNullTrampPatchBytesMax]{};
    if (!BuildJumpPatch(patchBuf, patchBytes, patchAt, trampEntry)) {
        return false;
    }

    return WriteBytes(patchAt, patchBuf, patchBytes);
}

bool ApplyRemoteChainGuards() {
    if (g_runtime.version == nullptr || g_runtime.sampBase == 0) {
        return false;
    }

    const std::uint32_t loadRva = g_runtime.version->chatBubbleRemoteChainLoadGuardRva;
    const std::uint32_t pedRva = g_runtime.version->chatBubbleRemoteChainPedGuardRva;
    if (loadRva == 0 && pedRva == 0) {
        return true;
    }
    if (loadRva == 0 || pedRva == 0 || g_poolSkip == nullptr) {
        return false;
    }

    void* const loadAt = reinterpret_cast<void*>(g_runtime.sampBase + loadRva);
    void* const pedAt = reinterpret_cast<void*>(g_runtime.sampBase + pedRva);
    if (!LooksLikeRemoteChainLoad(loadAt) || !LooksLikeRemoteChainPed(pedAt)) {
        return false;
    }

    g_remoteChainLoadResume = pedAt;
    g_remoteChainPedResume =
        reinterpret_cast<void*>(g_runtime.sampBase + pedRva + kRemoteChainGuardPatchBytes);

    std::uint8_t loadPatch[kRemoteChainGuardPatchBytes]{};
    std::uint8_t pedPatch[kRemoteChainGuardPatchBytes]{};
    if (!BuildJumpPatch(
            loadPatch,
            sizeof(loadPatch),
            loadAt,
            reinterpret_cast<const void*>(&ChatBubbleRemoteChainLoadGuardTrampoline))) {
        return false;
    }
    if (!BuildJumpPatch(
            pedPatch,
            sizeof(pedPatch),
            pedAt,
            reinterpret_cast<const void*>(&ChatBubbleRemoteChainPedGuardTrampoline))) {
        return false;
    }

    if (!WriteBytes(loadAt, loadPatch, sizeof(loadPatch))) {
        return false;
    }
    return WriteBytes(pedAt, pedPatch, sizeof(pedPatch));
}

bool ApplyLocalDrawBypass() {
    if (g_runtime.version == nullptr || g_runtime.sampBase == 0) {
        return false;
    }
    if (g_runtime.version->chatBubbleLocalSkipJeRva == 0) {
        return false;
    }

    void* const patchAt =
        reinterpret_cast<void*>(g_runtime.sampBase + g_runtime.version->chatBubbleLocalSkipJeRva);
    if (!LooksLikeNearJeSkip(patchAt)) {
        return false;
    }

    static constexpr std::uint8_t kNops[kLocalSkipJePatchBytes] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    return WriteBytes(patchAt, kNops, sizeof(kNops));
}

}  // namespace

extern "C" void* __cdecl ResolveChatBubbleLocalPed(unsigned short slot) {
    if (g_runtime.version == nullptr || g_runtime.sampBase == 0) {
        return nullptr;
    }

    void* const netGame = ReadGlobalObject<void>(g_runtime.version->refNetGameOffset);
    if (netGame == nullptr) {
        return nullptr;
    }

    void* const pool = CallThis<void*>(netGame, g_runtime.version->getPlayerPoolOffset);
    if (pool == nullptr) {
        return nullptr;
    }

    if (slot != GetLocalPlayerId(pool)) {
        return nullptr;
    }

    void* const localPlayer = CallThis<void*>(pool, g_runtime.version->getLocalPlayerOffset);
    if (localPlayer == nullptr) {
        return nullptr;
    }

    __try {
        void* const ped = *reinterpret_cast<void**>(
            reinterpret_cast<std::uint8_t*>(localPlayer) + g_runtime.version->localPedOffset);
        if (!LikelyHeapDataPointer(ped)) {
            return nullptr;
        }
        return ped;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

extern "C" int __cdecl ChatBubbleLea11RowChainOk(const void* leaSlotBase) {
    if (leaSlotBase == nullptr || g_runtime.version == nullptr) {
        return 0;
    }

    __try {
        const auto* const b = static_cast<const std::uint8_t*>(leaSlotBase);
        const void* r = nullptr;
        const void* r2 = nullptr;
        switch (g_runtime.version->version) {
            case SampVersion::R2: {
                r = *reinterpret_cast<const void* const*>(b + 0x26);
                if (!LikelyHeapDataPointer(r)) {
                    return 0;
                }
                r2 = *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(r) + 0x0c);
                if (!LikelyHeapDataPointer(r2)) {
                    return 0;
                }
                (void)*reinterpret_cast<const volatile std::uint32_t*>(
                    static_cast<const std::uint8_t*>(r2) + 0x1c);
                (void)*reinterpret_cast<const volatile std::uint8_t*>(
                    static_cast<const std::uint8_t*>(r2) + 0x12);
                return 1;
            }
            case SampVersion::R4: {
                r = *reinterpret_cast<const void* const*>(b + 0x2e);
                if (!LikelyHeapDataPointer(r)) {
                    return 0;
                }
                r2 = *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(r) + 0x10);
                if (!LikelyHeapDataPointer(r2)) {
                    return 0;
                }
                (void)*reinterpret_cast<const volatile std::uint32_t*>(
                    static_cast<const std::uint8_t*>(r2) + 0x1dd);
                (void)*reinterpret_cast<const volatile std::uint8_t*>(
                    static_cast<const std::uint8_t*>(r2) + 0x10a);
                return 1;
            }
            case SampVersion::R4_2:
            case SampVersion::R5_1: {
                r = *reinterpret_cast<const void* const*>(b + 0x1f8a);
                if (!LikelyHeapDataPointer(r)) {
                    return 0;
                }
                r2 = *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(r) + 0x10);
                if (!LikelyHeapDataPointer(r2)) {
                    return 0;
                }
                (void)*reinterpret_cast<const volatile std::uint32_t*>(
                    static_cast<const std::uint8_t*>(r2) + 0x1dd);
                (void)*reinterpret_cast<const volatile std::uint8_t*>(
                    static_cast<const std::uint8_t*>(r2) + 0x10a);
                return 1;
            }
            case SampVersion::DL_R1: {
                r = *reinterpret_cast<const void* const*>(b + 0x26);
                if (!LikelyHeapDataPointer(r)) {
                    return 0;
                }
                r2 = *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(r) + 8);
                if (!LikelyHeapDataPointer(r2)) {
                    return 0;
                }
                const void* const ped =
                    *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(r2) + 4);
                if (!LikelyHeapDataPointer(ped)) {
                    return 0;
                }
                (void)*reinterpret_cast<const volatile std::uint8_t*>(
                    static_cast<const std::uint8_t*>(r2) + 0x1a);
                return 1;
            }
            default:
                return 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" int __cdecl ChatBubbleCanReadLikelyPointerAt(const void* base, unsigned int offset) {
    if (!LikelyHeapDataPointer(base) || offset > 0x2000u) {
        return 0;
    }

    __try {
        const void* const value =
            *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(base) + offset);
        return LikelyHeapDataPointer(value) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" int __cdecl ChatBubbleR1RowChainOk(const void* rowBase) {
    if (!LikelyHeapDataPointer(rowBase)) {
        return 0;
    }

    __try {
        const auto* const row = static_cast<const std::uint8_t*>(rowBase);
        const void* const pedSlot = *reinterpret_cast<const void* const*>(row + 0x2e);
        if (!LikelyHeapDataPointer(pedSlot)) {
            return 0;
        }

        const void* const ped = *reinterpret_cast<const void* const*>(pedSlot);
        if (!LikelyHeapDataPointer(ped)) {
            return 0;
        }

        const void* const vtable = *reinterpret_cast<const void* const*>(ped);
        if (!LikelyHeapDataPointer(vtable)) {
            return 0;
        }

        return (*reinterpret_cast<const volatile std::uint8_t*>(
            static_cast<const std::uint8_t*>(ped) + 0x09) != 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" int __cdecl ChatBubbleR3RowChainOk(const void* poolBase, unsigned int slot) {
    if (!LikelyHeapDataPointer(poolBase) || slot >= 1004u) {
        return 0;
    }

    __try {
        const auto* const pool = static_cast<const std::uint8_t*>(poolBase);
        const void* const pedSlot =
            *reinterpret_cast<const void* const*>(pool + (slot * 4u) + 0x04);
        if (!LikelyHeapDataPointer(pedSlot)) {
            return 0;
        }

        const void* const ped = *reinterpret_cast<const void* const*>(pedSlot);
        if (!LikelyHeapDataPointer(ped)) {
            return 0;
        }

        const void* const vtable = *reinterpret_cast<const void* const*>(ped);
        if (!LikelyHeapDataPointer(vtable)) {
            return 0;
        }

        return (*reinterpret_cast<const volatile std::uint8_t*>(
            static_cast<const std::uint8_t*>(ped) + 0x10a) != 0) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

extern "C" __declspec(naked) void ChatBubbleRemoteChainLoadGuardTrampoline() {
    __asm {
        push ecx
        push edx
        push eax
        push 8
        push eax
        call ChatBubbleCanReadLikelyPointerAt
        add esp, 8
        test eax, eax
        pop eax
        pop edx
        pop ecx
        je L_skip
        mov eax, dword ptr [eax + 8]
        jmp dword ptr [g_remoteChainLoadResume]
L_skip:
        jmp dword ptr [g_poolSkip]
    }
}

extern "C" __declspec(naked) void ChatBubbleRemoteChainPedGuardTrampoline() {
    __asm {
        push edx
        push eax
        push 4
        push eax
        call ChatBubbleCanReadLikelyPointerAt
        add esp, 8
        test eax, eax
        pop eax
        pop edx
        je L_skip
        mov ecx, dword ptr [eax + 4]
        test ecx, ecx
        jmp dword ptr [g_remoteChainPedResume]
L_skip:
        jmp dword ptr [g_poolSkip]
    }
}

extern "C" __declspec(naked) void ChatBubblePoolNullTrampoline_EcxJe8() {
    __asm {
        push eax
        push ecx
        push edx
        push esi
        push edi
        movzx eax, bp
        push eax
        call ResolveChatBubbleLocalPed
        add esp, 4
        test eax, eax
        je L_restore_original
        add esp, 8
        mov edi, eax
        pop edx
        pop ecx
        pop eax
        jmp dword ptr [g_poolResumeMid]
L_restore_original:
        pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        test ecx, ecx
        jnz L_validate_row
        jmp dword ptr [g_poolSkip]
L_validate_row:
        push eax
        push ecx
        push edx
        push esi
        push edi
        push eax
        call ChatBubbleR1RowChainOk
        add esp, 4
        test eax, eax
        pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        je L_skip_row
        jmp dword ptr [g_poolResume]
L_skip_row:
        jmp dword ptr [g_poolSkip]
    }
}

extern "C" __declspec(naked) void ChatBubblePoolNullTrampoline_EdxJe8() {
    __asm {
        push eax
        push ecx
        push edx
        push esi
        push edi
        movzx eax, bp
        push eax
        call ResolveChatBubbleLocalPed
        add esp, 4
        test eax, eax
        je L_restore_original
        add esp, 8
        mov esi, eax
        pop edx
        pop ecx
        pop eax
        jmp dword ptr [g_poolResumeMid]
L_restore_original:
        pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        test edx, edx
        jnz L_validate_edx
        jmp dword ptr [g_poolSkip]
L_validate_edx:
        push eax
        push ecx
        push edx
        push esi
        push edi
        push eax
        push ecx
        call ChatBubbleR3RowChainOk
        add esp, 8
        test eax, eax
        pop edi
        pop esi
        pop edx
        pop ecx
        pop eax
        je L_skip_edx
        jmp dword ptr [g_poolResume]
L_skip_edx:
        jmp dword ptr [g_poolSkip]
    }
}

extern "C" __declspec(naked) void ChatBubblePoolNullTrampoline_EcxLeaJe11() {
    __asm {
        push ecx
        push edx
        push esi
        push edi
        movzx eax, bp
        push eax
        call ResolveChatBubbleLocalPed
        add esp, 4
        test eax, eax
        je L_restore_original
        add esp, 8
        mov edi, eax
        pop edx
        pop ecx
        jmp dword ptr [g_poolResumeMid]
L_restore_original:
        pop edi
        pop esi
        pop edx
        pop ecx
        test ecx, ecx
        jnz L_nonempty_pool
        jmp dword ptr [g_poolSkip]
L_nonempty_pool:
        push edx
        lea eax, [edi + edx * 4]
        push eax
        call ChatBubbleLea11RowChainOk
        add esp, 4
        pop edx
        test eax, eax
        je L_nonempty_pool_chain_bad
        lea eax, [edi + edx * 4]
        jmp dword ptr [g_poolResume]
L_nonempty_pool_chain_bad:
        jmp dword ptr [g_poolSkip]
    }
}

namespace {

void __fastcall LocalPlayerChatDetour(void* thisPtr, void* /*edx*/, const char* text) {
    if (g_originalLocalPlayerChat != nullptr) {
        g_originalLocalPlayerChat(thisPtr, text);
    }

    if (!g_runtime.mirrorOwnChatBubble) {
        return;
    }
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    if (text[0] == '/') {
        return;
    }
    if (g_runtime.buildLocalContext == nullptr) {
        return;
    }

    chat_bubble::LocalPlayerContext context{};
    if (!g_runtime.buildLocalContext(context)) {
        return;
    }
    if (thisPtr != context.localPlayer || context.id >= 1004u) {
        return;
    }

    chat_bubble::PushLocalPlayerBubble(context, text, g_runtime.ownChatBubbleColor, g_runtime.lifeMs);
}

}  // namespace

namespace chat_bubble {

void Configure(const RuntimeConfig& config) {
    g_runtime = config;
    SyncStackRuntime(config);
}

void Shutdown() {
    if (g_measureDc == nullptr) {
        return;
    }

    if (g_measureFont != nullptr) {
        SelectObject(g_measureDc, g_measureOldFont);
        DeleteObject(g_measureFont);
        g_measureFont = nullptr;
        g_measureOldFont = nullptr;
    }
    DeleteDC(g_measureDc);
    g_measureDc = nullptr;
}

bool ParseColorString(const char* input, D3DCOLOR* outColor) {
    if (input == nullptr || outColor == nullptr) {
        return false;
    }

    const char* s = SkipSpaces(input);
    if (*s == '\0') {
        return false;
    }

    if (*s == '{') {
        ++s;
        std::size_t n = 0;
        while (IsHexDigit(s[n])) {
            ++n;
        }
        if (s[n] != '}' || (n != 6 && n != 8)) {
            return false;
        }
        if (*SkipSpaces(s + n + 1) != '\0') {
            return false;
        }
        if (n == 6) {
            return PackD3dRrggbbaaFromRrggbb6(s, outColor);
        }
        return PackD3dRrggbbaaFromRrggbbaa8(s, outColor);
    }

    const char* t = s;
    std::size_t hexLen = 0;
    while (IsHexDigit(*t)) {
        ++t;
        ++hexLen;
    }
    if (*SkipSpaces(t) == '\0' && (hexLen == 6 || hexLen == 8)) {
        if (hexLen == 6) {
            return PackD3dRrggbbaaFromRrggbb6(s, outColor);
        }
        return PackD3dRrggbbaaFromRrggbbaa8(s, outColor);
    }

    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *SkipSpaces(end) != '\0') {
        return false;
    }
    *outColor = static_cast<D3DCOLOR>(static_cast<std::int32_t>(v));
    return true;
}

void BuildOverlayText(char* out, std::size_t cap, const OverlayCommandRule& rule, const char* rest) {
    if (out == nullptr || cap == 0) {
        return;
    }

    out[0] = '\0';
    if (rest == nullptr) {
        return;
    }
    if (rule.bubbleTemplate[0] == '\0') {
        strncpy_s(out, cap, rest, _TRUNCATE);
        return;
    }

    char msgWhole[kLineMax] = {};
    TrimCopyRange(rest, rest + std::strlen(rest), msgWhole, sizeof(msgWhole));

    char seg0[kLineMax] = {};
    char seg1[kLineMax] = {};
    SplitRestIntoSegments(rest, rule.bubbleSplit, seg0, seg1, sizeof(seg0));

    std::size_t used = 0;
    int linePx = 0;
    const int maxLine = g_runtime.overlayLinePx;
    const D3DCOLOR c2 = rule.accentColorValid ? rule.accentColor : rule.color;

    for (const char* p = rule.bubbleTemplate; *p != '\0';) {
        if (p[0] == '[') {
            if (std::strncmp(p, "[msg]", 5) == 0) {
                if (!AppendCStringTrunc(out, cap, used, msgWhole)) {
                    return;
                }
                if (maxLine > 0 && MeasureEnsure()) {
                    AdvanceLineLayout(maxLine, linePx, msgWhole, std::strlen(msgWhole));
                }
                p += 5;
                continue;
            }
            if (std::strncmp(p, "[a]", 3) == 0) {
                if (!AppendCStringTrunc(out, cap, used, seg0)) {
                    return;
                }
                if (maxLine > 0 && MeasureEnsure()) {
                    AdvanceLineLayout(maxLine, linePx, seg0, std::strlen(seg0));
                }
                p += 3;
                continue;
            }
            if (std::strncmp(p, "[b]", 3) == 0) {
                if (!AppendCStringTrunc(out, cap, used, seg1)) {
                    return;
                }
                if (maxLine > 0 && MeasureEnsure()) {
                    AdvanceLineLayout(maxLine, linePx, seg1, std::strlen(seg1));
                }
                p += 3;
                continue;
            }
            if (std::strncmp(p, "[c]", 3) == 0) {
                if (!AppendColorEmbed(out, cap, used, maxLine, linePx, rule.color)) {
                    return;
                }
                p += 3;
                continue;
            }
            if (std::strncmp(p, "[d]", 3) == 0) {
                if (!AppendColorEmbed(out, cap, used, maxLine, linePx, c2)) {
                    return;
                }
                p += 3;
                continue;
            }
        }

        if (!AppendChar(out, cap, used, *p)) {
            return;
        }
        if (maxLine > 0 && MeasureEnsure()) {
            if (*p == '\n') {
                linePx = 0;
            } else {
                AdvanceLineLayout(maxLine, linePx, p, 1);
            }
        }
        ++p;
    }
}

bool WantLocalDrawPatches(bool mirrorOwnChatBubble, unsigned int overlayRuleCount, bool stackChatBubbles) {
    return mirrorOwnChatBubble || overlayRuleCount > 0 || stackChatBubbles;
}

bool InstallLocalDrawPatches() {
    if (!ApplyPoolNullTrampoline()) {
        return false;
    }
    if (!ApplyRemoteChainGuards()) {
        return false;
    }
    return ApplyLocalDrawBypass();
}

bool InstallLocalPlayerChatHook() {
    if (g_runtime.version == nullptr || g_runtime.sampBase == 0) {
        return false;
    }
    if (!g_runtime.mirrorOwnChatBubble || g_runtime.version->localPlayerChatOffset == 0) {
        return true;
    }

    void* chatTarget =
        reinterpret_cast<void*>(g_runtime.sampBase + g_runtime.version->localPlayerChatOffset);
    const MH_STATUS status = MH_CreateHook(
        chatTarget,
        reinterpret_cast<void*>(&LocalPlayerChatDetour),
        reinterpret_cast<void**>(&g_originalLocalPlayerChat));
    if (status != MH_OK) {
        return false;
    }
    return MH_EnableHook(chatTarget) == MH_OK;
}

void PushLocalPlayerBubble(
    const LocalPlayerContext& context,
    const char* text,
    D3DCOLOR color,
    int lifeMs) {
    if (g_runtime.version == nullptr || context.id >= 1004u) {
        return;
    }

    void* const bubble = ReadGlobalObject<void>(g_runtime.version->refChatBubbleOffset);
    if (bubble == nullptr) {
        return;
    }

    char line[kLineMax] = {};
    if (text != nullptr) {
        strncpy_s(line, sizeof(line), text, _TRUNCATE);
    }

    CallThis<void>(
        bubble,
        g_runtime.version->chatBubbleAddOffset,
        static_cast<unsigned int>(context.id),
        line,
        color,
        context.distanceToCamera,
        lifeMs);
}

int MeasureBubbleLineCount(const char* text, int maxLinePx) {
    if (text == nullptr || text[0] == '\0') {
        return 1;
    }

    int lines = 1;
    if (maxLinePx <= 0 || !MeasureEnsure()) {
        for (const char* p = text; *p != '\0'; ++p) {
            if (*p == '\n') {
                ++lines;
            }
        }
        return lines;
    }

    int linePx = 0;
    for (const char* p = text; *p != '\0';) {
        if (*p == '\n') {
            ++lines;
            linePx = 0;
            ++p;
            continue;
        }
        if (*p == '{' && IsHexDigit(p[1]) && IsHexDigit(p[2]) && IsHexDigit(p[3])
            && IsHexDigit(p[4]) && IsHexDigit(p[5]) && IsHexDigit(p[6]) && p[7] == '}') {
            p += 8;  // {RRGGBB} не занимает ширину
            continue;
        }
        const int w = MeasureCharWidthA(*p);
        if (linePx + w > maxLinePx && linePx > 0) {
            ++lines;
            linePx = w;
        } else {
            linePx += w;
        }
        ++p;
    }
    return lines;
}

}  // namespace chat_bubble
