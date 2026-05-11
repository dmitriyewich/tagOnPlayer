#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "external/MinHook/include/MinHook.h"

static_assert(sizeof(void*) == 4, "tagOnPlayer.asi must be built for Win32.");

using D3DCOLOR = std::uint32_t;

namespace {

constexpr DWORD kGtaLoadStateAddress = 0x00C8D4C0;
constexpr std::uintptr_t kTheCameraAddress = 0x00B6F028;
constexpr std::size_t kHookPatchSize = 9;
constexpr unsigned int kHeadBoneId = 8;
constexpr D3DCOLOR kFallbackLabelColor = 0xFFFFFFFFu;

constexpr char kConfigSection[] = "Settings";
constexpr char kConfigKeyCommand[] = "Command";
constexpr char kConfigKeyEnabled[] = "EnabledByDefault";
constexpr char kConfigKeyMirrorOwnChatBubble[] = "MirrorOwnChatBubble";
constexpr char kConfigKeyChatBubbleLifeMs[] = "ChatBubbleLifeMs";
/** Дефолт ширины строки бабла (px), если INI без ключа; по rizin во всех строках `kSupportedVersions` — **257** (`push 0x101` перед внутренним `DrawText`). */
constexpr char kConfigKeyOverlayBubbleLinePx[] = "OverlayBubbleLinePx";
constexpr char kDefaultCommand[] = "/tagon";
constexpr int kDefaultChatBubbleLifeMs = 6000;
constexpr int kDefaultOverlayBubbleLinePx = 257;
constexpr char kOverlaySection[] = "OverlayCommands";
constexpr unsigned int kOverlayMaxRules = 16;
constexpr std::size_t kOverlayCmdMax = 64;
constexpr std::size_t kOverlayBubbleTemplateMax = 128;
constexpr std::size_t kOverlayBubbleSplitMax = 16;
constexpr std::size_t kChatBubbleLineMax = 256;
constexpr std::size_t kChatBubbleLocalSkipJePatchBytes = 6;
constexpr std::size_t kChatBubblePoolNullTrampPatchBytesMax = 12;

/** 0 — нет патча; иначе см. `chatBubblePoolNullTrampKind`. */
constexpr std::uint8_t kChatBubblePoolTrampNone = 0;
/** R1: `test ecx` + near-`je` (8 байт); `resume` / `resumeMid` как сейчас. */
constexpr std::uint8_t kChatBubblePoolTrampEcxJe8 = 1;
/** R3 / R3-1: `test edx` + near-`je` (8); подстановка — `esi = *(void**)((uint8_t*)lp + localPedOffset)`. */
constexpr std::uint8_t kChatBubblePoolTrampEdxJe8 = 2;
/** R2, R4, R4-2, R5-1, DL-R1: `test ecx` + `lea eax,[edi+edx*4]` + near-`je` (12). */
constexpr std::uint8_t kChatBubblePoolTrampEcxLeaJe12 = 3;

struct CVector {
    float x;
    float y;
    float z;
};

struct GameSimpleTransform {
    CVector position;
    float heading;
};

struct GameMatrix {
    CVector right;
    std::uint32_t flags0;
    CVector up;
    std::uint32_t flags1;
    CVector at;
    std::uint32_t flags2;
    CVector pos;
    std::uint32_t flags3;
    void* attachedMatrix;
    bool ownsAttachedMatrix;
    char pad[3];
};

struct GamePlaceable {
    void* vtable;
    GameSimpleTransform placement;
    GameMatrix* matrix;
};

struct GameCamera {
    GamePlaceable placeable;
};

enum class SampVersion {
    R1,
    R2,
    R3,
    R3_1,
    R4,
    R4_2,
    R5_1,
    DL_R1,
};

struct SampVersionInfo {
    DWORD entryPointRva;
    SampVersion version;
    const char* name;
    std::uint32_t refNetGameOffset;
    std::uint32_t refPlayerTagsOffset;
    std::uint32_t labelLoopOffset;
    std::uint32_t healthLoopOffset;
    std::uint32_t sendCommandOffset;
    std::uint32_t beginLabelOffset;
    std::uint32_t endLabelOffset;
    std::uint32_t drawLabelOffset;
    std::uint32_t beginHealthBarOffset;
    std::uint32_t endHealthBarOffset;
    std::uint32_t drawHealthBarOffset;
    std::uint32_t getPlayerPoolOffset;
    std::uint32_t getLocalPlayerOffset;
    std::uint32_t getNameByIdOffset;
    std::uint32_t getLocalPlayerColorArgbOffset;
    std::uint32_t pedIsOnScreenOffset;
    std::uint32_t pedGetHealthOffset;
    std::uint32_t pedGetArmourOffset;
    std::uint32_t pedGetBonePositionOffset;
    std::uint32_t localPlayerIdOffset;
    std::uint32_t localPedOffset;
    std::uint32_t localPlayerChatOffset;
    std::uint32_t refChatBubbleOffset;
    std::uint32_t chatBubbleAddOffset;
    std::uint32_t chatBubbleDrawOffset;
    std::uint32_t chatBubbleLocalSkipJeRva;
    /** R1: после mov ecx,[row+0xfde]; test ecx; near-je на конец итерации — для локального слота ecx==0. */
    std::uint32_t chatBubblePoolNullTrampPatchRva;
    std::uint32_t chatBubblePoolNullTrampResumeRva;
    /** После подстановки локального слота: R1 — `mov ecx,edi`/кость; R3* — `mov ecx,esi`/кость; R2+ — `push ebp`/`mov ecx,edi`/GetPlayer. */
    std::uint32_t chatBubblePoolNullTrampResumeMidRva;
    std::uint32_t chatBubblePoolNullTrampSkipRva;
    /** `kChatBubblePoolTramp*`; размер патча 8 при 1–2, 12 при 3. */
    std::uint8_t chatBubblePoolNullTrampKind;
    /** Ширина строки текста бабла в клиенте (px), для дефолта `OverlayBubbleLinePx` после детекта версии. */
    std::uint16_t sampBubbleDrawTextLinePx;
};

constexpr std::array<SampVersionInfo, 8> kSupportedVersions{{
    // chatBubbleDrawOffset — RVA CChatBubble::Draw (канон SAMP-API / rizin); chatBubbleLocalSkipJeRva — начало near-je
    // «пропуск слота», если флаг видимости бабла нулевой (в т.ч. локальный игрок); патч: 6×NOP поверх 0F 84 …
    {0x31DF13, SampVersion::R1,    "R1",    0x0021A0F8, 0x0021A0B0, 0x00070D40, 0x0006FC30, 0x00065C60, 0x000686A0, 0x000686B0, 0x000686C0, 0x00068FD0, 0x00068670, 0x000689C0, 0x00001160, 0x00001A30, 0x00013CE0, 0x00003D90, 0x000A65A0, 0x000A6610, 0x000A6650, 0x000A8D70, 0x00000004, 0x00000000, 0x000057F0, 0x0021A0DC, 0x00063250, 0x00063310, 0x000633DA, 0x000633B7, 0x000633BF, 0x000633F4, 0x00063495, kChatBubblePoolTrampEcxJe8, 257},
    {0x3195DD, SampVersion::R2,    "R2",    0x0021A100, 0x0021A0B8, 0x00070DE0, 0x0006FCD0, 0x00065D30, 0x00068770, 0x00068780, 0x00068790, 0x000690A0, 0x00068740, 0x00068A90, 0x00001170, 0x00001A40, 0x00013DA0, 0x00003DA0, 0x000A6770, 0x000A67E0, 0x000A6820, 0x000A8F40, 0x00000000, 0x00000000, 0x000058C0, 0x0021A0E4, 0x00063320, 0x000633E0, 0x000634AA, 0x00063481, 0x0006348C, 0x000634B0, 0x00063564, kChatBubblePoolTrampEcxLeaJe12, 257},
    {0x0CC490, SampVersion::R3,    "R3",    0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB430, 0x000AB480, 0x000AB4C0, 0x000ADC00, 0x00002F1C, 0x00000000, 0x00005820, 0x0026E8C0, 0x000666A0, 0x00066760, 0x0006682C, 0x00066805, 0x0006680D, 0x00066846, 0x000668E7, kChatBubblePoolTrampEdxJe8, 257},
    {0x0CC4D0, SampVersion::R3_1,  "R3-1",  0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB450, 0x000AB4C0, 0x000AB500, 0x000ADBF0, 0x00002F1C, 0x00000000, 0x00005820, 0x0026E8C0, 0x000666A0, 0x00066760, 0x0006682C, 0x00066805, 0x0006680D, 0x00066846, 0x000668E7, kChatBubblePoolTrampEdxJe8, 257},
    {0x0CBCB0, SampVersion::R4,    "R4",    0x0026EA0C, 0x0026E9C0, 0x00075360, 0x00074240, 0x000698C0, 0x0006CD40, 0x0006CD50, 0x0006CD60, 0x0006D670, 0x0006CD10, 0x0006D060, 0x00001170, 0x00001A40, 0x00017570, 0x00003F10, 0x000ABCF0, 0x000ABD60, 0x000ABDA0, 0x000AE490, 0x0000000C, 0x00000104, 0x00005918, 0x0026E9F0, 0x00066DD0, 0x00066E90, 0x00066F60, 0x00066F31, 0x00066F3C, 0x00066F66, 0x0006701D, kChatBubblePoolTrampEcxLeaJe12, 257},
    {0x0CBCD0, SampVersion::R4_2,  "R4-2",  0x0026EA0C, 0x0026E9C0, 0x00075390, 0x00074270, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABD20, 0x000ABD90, 0x000ABDD0, 0x000AE4C0, 0x00000004, 0x00000104, 0x00005A10, 0x0026E9F0, 0x00066E10, 0x00066ED0, 0x00066FA0, 0x00066F6E, 0x00066F79, 0x00066FA6, 0x0006705D, kChatBubblePoolTrampEcxLeaJe12, 257},
    {0x0CBC90, SampVersion::R5_1,  "R5-1",  0x0026EB94, 0x0026EB48, 0x00075330, 0x00074210, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABCE0, 0x000ABD50, 0x000ABD90, 0x000AE480, 0x00000004, 0x00000104, 0x00005A10, 0x0026EB78, 0x00066E10, 0x00066ED0, 0x00066FA0, 0x00066F6E, 0x00066F79, 0x00066FA6, 0x0006705D, kChatBubblePoolTrampEcxLeaJe12, 257},
    {0x0FDB60, SampVersion::DL_R1, "DL-R1", 0x002ACA24, 0x002AC9D8, 0x00074DC0, 0x00073CB0, 0x00069340, 0x0006C7C0, 0x0006C7D0, 0x0006C7E0, 0x0006D0F0, 0x0006C790, 0x0006CAE0, 0x00001170, 0x00001A80, 0x000170D0, 0x00003E20, 0x000AB900, 0x000AB970, 0x000AB9B0, 0x000AE080, 0x00000000, 0x00000000, 0x00005860, 0x002ACA08, 0x00066890, 0x00066950, 0x00066A1A, 0x000669F1, 0x000669FC, 0x00066A20, 0x00066AD4, kChatBubblePoolTrampEcxLeaJe12, 257},
}};

using RenderLoopFn = void(__cdecl*)();
using SendCommandFn = void(__thiscall*)(void*, const char*);
using LocalPlayerChatFn = void(__thiscall*)(void*, const char*);

struct OverlayCommandRule {
    char cmd[kOverlayCmdMax]{};
    D3DCOLOR color = kFallbackLabelColor;
    bool forwardToServer = true;
    char bubbleTemplate[kOverlayBubbleTemplateMax]{};
    char bubbleSplit[kOverlayBubbleSplitMax]{};
    D3DCOLOR accentColor = kFallbackLabelColor;
    bool accentColorValid = false;
};

struct PluginState {
    HMODULE pluginModule = nullptr;
    HMODULE sampModule = nullptr;
    std::uintptr_t sampBase = 0;
    const SampVersionInfo* version = nullptr;
    RenderLoopFn originalLabelLoop = nullptr;
    RenderLoopFn originalHealthLoop = nullptr;
    bool renderEnabled = true;
    bool mirrorOwnChatBubble = false;
    int chatBubbleLifeMs = kDefaultChatBubbleLifeMs;
    /** `0` — не вставлять `\n` перед `{RRGGBB}`; иначе макс. ширина строки (px), R1 из rizin. */
    int overlayBubbleLinePx = kDefaultOverlayBubbleLinePx;
    char toggleCommand[64] = "/tagon";
    std::array<OverlayCommandRule, kOverlayMaxRules> overlayRules{};
    unsigned int overlayRuleCount = 0;
};

PluginState g_state;
SendCommandFn g_originalSendCommand = nullptr;
LocalPlayerChatFn g_originalLocalPlayerChat = nullptr;

static HDC g_overlayBubbleMeasureDc = nullptr;
static HFONT g_overlayBubbleMeasureFont = nullptr;
static HFONT g_overlayBubbleMeasureOldFont = nullptr;

void OverlayBubbleMeasureShutdown() {
    if (g_overlayBubbleMeasureDc != nullptr) {
        if (g_overlayBubbleMeasureFont != nullptr) {
            SelectObject(g_overlayBubbleMeasureDc, g_overlayBubbleMeasureOldFont);
            DeleteObject(g_overlayBubbleMeasureFont);
            g_overlayBubbleMeasureFont = nullptr;
            g_overlayBubbleMeasureOldFont = nullptr;
        }
        DeleteDC(g_overlayBubbleMeasureDc);
        g_overlayBubbleMeasureDc = nullptr;
    }
}

bool OverlayBubbleMeasureEnsure() {
    if (g_overlayBubbleMeasureDc != nullptr) {
        return true;
    }
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return false;
    }
    g_overlayBubbleMeasureDc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (g_overlayBubbleMeasureDc == nullptr) {
        return false;
    }
    /* R1 rizin: `ps @ 0x100d8028` → Arial; у `0x1006739b` — Height 10, Weight 700 для D3DXCreateFont. */
    g_overlayBubbleMeasureFont = CreateFontA(
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
    if (g_overlayBubbleMeasureFont == nullptr) {
        DeleteDC(g_overlayBubbleMeasureDc);
        g_overlayBubbleMeasureDc = nullptr;
        return false;
    }
    g_overlayBubbleMeasureOldFont =
        static_cast<HFONT>(SelectObject(g_overlayBubbleMeasureDc, g_overlayBubbleMeasureFont));
    return true;
}

int OverlayBubbleMeasureCharWidthA(char c) {
    char s[2] = {c, '\0'};
    SIZE sz = {};
    if (g_overlayBubbleMeasureDc == nullptr || !GetTextExtentPoint32A(g_overlayBubbleMeasureDc, s, 1, &sz)) {
        return 7;
    }
    return static_cast<int>(sz.cx);
}

void OverlayBubbleAdvanceLineLayout(int maxLinePx, int& linePx, const char* s, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
        const int w = OverlayBubbleMeasureCharWidthA(s[i]);
        if (linePx + w > maxLinePx) {
            linePx = w;
        } else {
            linePx += w;
        }
    }
}

/** Перенос посередине 8-символьного `{RRGGBB}` при той же эвристике ширины, что в плагине. */
bool OverlayBubbleColorEmbedSplitsMidLine(int maxLinePx, int linePx, const char embed[9]) {
    int x = linePx;
    for (int i = 0; i < 8; ++i) {
        const int w = OverlayBubbleMeasureCharWidthA(embed[i]);
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

std::uint8_t g_chatBubbleJeOrig[kChatBubbleLocalSkipJePatchBytes] = {};
std::uint8_t g_chatBubblePoolEarlyOrig[kChatBubblePoolNullTrampPatchBytesMax] = {};

static void* g_poolNullRowEax = nullptr;
static void* g_poolNullSaveEcx = nullptr;
static void* g_poolNullSaveEax = nullptr;
static void* g_poolNullLeaSaveEdx = nullptr;
static void* g_poolNullLeaSaveEdi = nullptr;
static DWORD g_chatBubbleLocalPedOffsetDw = 0;
static void* g_chatBubblePoolResume = nullptr;
static void* g_chatBubblePoolResumeMid = nullptr;
static void* g_chatBubblePoolSkip = nullptr;
static char g_tagOnPlayerIniPath[MAX_PATH] = {};

bool StrEqualNoCase(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

const char* SkipSpaces(const char* p) {
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    return p;
}

bool FirstTokenEqualsNoCase(const char* line, const char* cmd) {
    const char* p = SkipSpaces(line);
    for (; *cmd; ++cmd, ++p) {
        char c = *p;
        char d = *cmd;
        if (d >= 'A' && d <= 'Z') {
            d += 32;
        }
        if (c >= 'A' && c <= 'Z') {
            c += 32;
        }
        if (c != d) {
            return false;
        }
    }
    return *p == '\0' || *p == ' ' || *p == '\t';
}

const char* RestAfterFirstToken(const char* line) {
    const char* p = SkipSpaces(line);
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        ++p;
    }
    return SkipSpaces(p);
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

/** Ровно 6 hex-цифр: `RRGGBB`, альфа в dword — `FF`. */
bool PackD3dArgbFromRrggbb6(const char* digits, D3DCOLOR* outColor) {
    if (outColor == nullptr) {
        return false;
    }
    const std::uint32_t rr = (HexValue(digits[0]) << 4) | HexValue(digits[1]);
    const std::uint32_t gg = (HexValue(digits[2]) << 4) | HexValue(digits[3]);
    const std::uint32_t bb = (HexValue(digits[4]) << 4) | HexValue(digits[5]);
    *outColor = (0xFFu << 24) | (rr << 16) | (gg << 8) | bb;
    return true;
}

/** Ровно 8 hex-цифр: `AARRGGBB` (как dword D3DCOLOR). */
bool PackD3dArgbFromAarrggbb8(const char* digits, D3DCOLOR* outColor) {
    if (outColor == nullptr) {
        return false;
    }
    const std::uint32_t aa = (HexValue(digits[0]) << 4) | HexValue(digits[1]);
    const std::uint32_t rr = (HexValue(digits[2]) << 4) | HexValue(digits[3]);
    const std::uint32_t gg = (HexValue(digits[4]) << 4) | HexValue(digits[5]);
    const std::uint32_t bb = (HexValue(digits[6]) << 4) | HexValue(digits[7]);
    *outColor = (aa << 24) | (rr << 16) | (gg << 8) | bb;
    return true;
}

/** INI `ColorN`: только `{RRGGBB}`, `{AARRGGBB}` или десятичный signed D3DCOLOR ([Color list](https://sampwiki.blast.hk/wiki/Color_list)). */
bool ParseOverlayColorString(const char* input, D3DCOLOR* outColor) {
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
            return PackD3dArgbFromRrggbb6(s, outColor);
        }
        return PackD3dArgbFromAarrggbb8(s, outColor);
    }

    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *SkipSpaces(end) != '\0') {
        return false;
    }
    *outColor = static_cast<D3DCOLOR>(static_cast<std::int32_t>(v));
    return true;
}

static char HexDigitUpper(std::uint8_t nibble) {
    nibble &= 0x0Fu;
    return static_cast<char>(nibble < 10 ? ('0' + nibble) : ('A' + (nibble - 10)));
}

void FormatCanonicalColorEmbedInto(char embed[9], D3DCOLOR canonicalArgb) {
    const std::uint32_t u = static_cast<std::uint32_t>(canonicalArgb);
    const std::uint8_t r = static_cast<std::uint8_t>((u >> 16) & 0xFFu);
    const std::uint8_t g = static_cast<std::uint8_t>((u >> 8) & 0xFFu);
    const std::uint8_t b = static_cast<std::uint8_t>(u & 0xFFu);
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

/** Вставка `{RRGGBB}`: при `maxLinePx>0` и симуляции разрыва токена — один `\n` перед ним, иначе без лишних переносов. */
bool AppendOverlayBubbleColorEmbed(
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
    if (maxLinePx > 0 && OverlayBubbleMeasureEnsure()
        && OverlayBubbleColorEmbedSplitsMidLine(maxLinePx, linePx, embed)) {
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
    if (maxLinePx > 0 && OverlayBubbleMeasureEnsure()) {
        OverlayBubbleAdvanceLineLayout(maxLinePx, linePx, embed, 8);
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

void BuildOverlayBubbleText(char* out, std::size_t cap, const OverlayCommandRule& rule, const char* rest) {
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

    char seg0[kChatBubbleLineMax] = {};
    char seg1[kChatBubbleLineMax] = {};
    SplitRestIntoSegments(rest, rule.bubbleSplit, seg0, seg1, sizeof(seg0));

    std::size_t used = 0;
    int linePx = 0;
    const int maxLine = g_state.overlayBubbleLinePx;
    const D3DCOLOR c2 = rule.accentColorValid ? rule.accentColor : rule.color;

    for (const char* p = rule.bubbleTemplate; *p != '\0';) {
        if (p[0] == '{' && p[1] == '0' && p[2] == '}') {
            if (!AppendCStringTrunc(out, cap, used, seg0)) {
                return;
            }
            if (maxLine > 0 && OverlayBubbleMeasureEnsure()) {
                OverlayBubbleAdvanceLineLayout(maxLine, linePx, seg0, std::strlen(seg0));
            }
            p += 3;
            continue;
        }
        if (p[0] == '{' && p[1] == '1' && p[2] == '}') {
            if (!AppendCStringTrunc(out, cap, used, seg1)) {
                return;
            }
            if (maxLine > 0 && OverlayBubbleMeasureEnsure()) {
                OverlayBubbleAdvanceLineLayout(maxLine, linePx, seg1, std::strlen(seg1));
            }
            p += 3;
            continue;
        }
        if (p[0] == '{' && p[1] == 'c' && p[2] == '1' && p[3] == '}') {
            if (!AppendOverlayBubbleColorEmbed(out, cap, used, maxLine, linePx, rule.color)) {
                return;
            }
            p += 4;
            continue;
        }
        if (p[0] == '{' && p[1] == 'c' && p[2] == '2' && p[3] == '}') {
            if (!AppendOverlayBubbleColorEmbed(out, cap, used, maxLine, linePx, c2)) {
                return;
            }
            p += 4;
            continue;
        }
        if (!AppendChar(out, cap, used, *p)) {
            return;
        }
        if (maxLine > 0 && OverlayBubbleMeasureEnsure()) {
            if (*p == '\n') {
                linePx = 0;
            } else {
                OverlayBubbleAdvanceLineLayout(maxLine, linePx, p, 1);
            }
        }
        ++p;
    }
}

template <typename T>
bool WriteBytes(void* address, const T* data, std::size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD restoredProtect = 0;
    VirtualProtect(address, size, oldProtect, &restoredProtect);
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
    const auto delta = static_cast<long long>(destinationAddress)
        - static_cast<long long>(sourceAddress + 5);

    if (delta < std::numeric_limits<std::int32_t>::min()
        || delta > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }

    const auto relative = static_cast<std::int32_t>(delta);
    std::memcpy(buffer + 1, &relative, sizeof(relative));
    return true;
}

bool InstallDetour(void* target, const void* hook, RenderLoopFn& original) {
    auto* targetBytes = static_cast<std::uint8_t*>(target);
    auto* trampoline = static_cast<std::uint8_t*>(VirtualAlloc(
        nullptr,
        kHookPatchSize + 5,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) {
        return false;
    }

    std::memcpy(trampoline, targetBytes, kHookPatchSize);

    std::uint8_t trampolineJump[5] = {};
    if (!BuildJumpPatch(
            trampolineJump,
            sizeof(trampolineJump),
            trampoline + kHookPatchSize,
            targetBytes + kHookPatchSize)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    std::memcpy(trampoline + kHookPatchSize, trampolineJump, sizeof(trampolineJump));

    std::uint8_t patch[kHookPatchSize] = {};
    if (!BuildJumpPatch(patch, sizeof(patch), targetBytes, hook)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    if (!WriteBytes(targetBytes, patch, sizeof(patch))) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    original = reinterpret_cast<RenderLoopFn>(trampoline);
    return true;
}

const SampVersionInfo* DetectSampVersion(HMODULE sampModule) {
    if (sampModule == nullptr) {
        return nullptr;
    }

    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(sampModule);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const std::uint8_t*>(sampModule) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }

    const DWORD entryPoint = ntHeaders->OptionalHeader.AddressOfEntryPoint;
    for (const auto& version : kSupportedVersions) {
        if (version.entryPointRva == entryPoint) {
            return &version;
        }
    }

    return nullptr;
}

template <typename T>
T* ReadGlobalObject(std::uint32_t offset) {
    if (g_state.sampBase == 0) {
        return nullptr;
    }

    return *reinterpret_cast<T**>(g_state.sampBase + offset);
}

template <typename Ret, typename... Args>
Ret CallThis(void* thisPtr, std::uint32_t offset, Args... args) {
    using Function = Ret(__thiscall*)(void*, Args...);
    return reinterpret_cast<Function>(g_state.sampBase + offset)(thisPtr, args...);
}

CVector GetCameraPosition() {
    const auto* camera = reinterpret_cast<const GameCamera*>(kTheCameraAddress);
    if (camera->placeable.matrix != nullptr) {
        return camera->placeable.matrix->pos;
    }
    return camera->placeable.placement.position;
}

float CalculateDistance(const CVector& from, const CVector& to) {
    const float dx = from.x - to.x;
    const float dy = from.y - to.y;
    const float dz = from.z - to.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

std::uint16_t GetLocalPlayerId(void* playerPool) {
    if (playerPool == nullptr || g_state.version == nullptr) {
        return 0;
    }

    return *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<const std::uint8_t*>(playerPool) + g_state.version->localPlayerIdOffset);
}

extern "C" void* __cdecl ResolveChatBubblePoolNull_EcxSubstitute(unsigned short slot) {
    if (g_state.version == nullptr || g_state.sampBase == 0) {
        return nullptr;
    }

    void* const netGame = ReadGlobalObject<void>(g_state.version->refNetGameOffset);
    if (netGame == nullptr) {
        return nullptr;
    }

    void* const pool = CallThis<void*>(netGame, g_state.version->getPlayerPoolOffset);
    if (pool == nullptr) {
        return nullptr;
    }

    const std::uint16_t localId = GetLocalPlayerId(pool);
    if (slot != localId) {
        return nullptr;
    }

    void* const lp = CallThis<void*>(pool, g_state.version->getLocalPlayerOffset);
    if (lp == nullptr) {
        return nullptr;
    }

    return lp;
}

extern "C" __declspec(naked) void ChatBubblePoolNullTrampoline_EcxJe8() {
    __asm {
        test ecx, ecx
        jnz L_resume_row
        mov g_poolNullRowEax, eax
        movzx eax, bp
        push eax
        call ResolveChatBubblePoolNull_EcxSubstitute
        add esp, 4
        mov ecx, eax
        mov eax, g_poolNullRowEax
        test ecx, ecx
        je L_skip
        push edx
        mov edx, g_chatBubbleLocalPedOffsetDw
        mov edi, dword ptr [ecx + edx]
        pop edx
        test edi, edi
        je L_skip
        jmp dword ptr [g_chatBubblePoolResumeMid]
L_resume_row:
        jmp dword ptr [g_chatBubblePoolResume]
L_skip:
        jmp dword ptr [g_chatBubblePoolSkip]
    }
}

extern "C" __declspec(naked) void ChatBubblePoolNullTrampoline_EdxJe8() {
    __asm {
        test edx, edx
        jnz L_resume_edx
        mov g_poolNullSaveEcx, ecx
        mov g_poolNullSaveEax, eax
        movzx eax, bp
        push eax
        call ResolveChatBubblePoolNull_EcxSubstitute
        add esp, 4
        test eax, eax
        je L_fail_edx
        mov ecx, g_chatBubbleLocalPedOffsetDw
        mov esi, dword ptr [eax + ecx]
        test esi, esi
        je L_fail_edx
        mov ecx, g_poolNullSaveEcx
        mov eax, g_poolNullSaveEax
        jmp dword ptr [g_chatBubblePoolResumeMid]
L_fail_edx:
        mov ecx, g_poolNullSaveEcx
        mov eax, g_poolNullSaveEax
        jmp dword ptr [g_chatBubblePoolSkip]
L_resume_edx:
        jmp dword ptr [g_chatBubblePoolResume]
    }
}

extern "C" __declspec(naked) void ChatBubblePoolNullTrampoline_EcxLeaJe12() {
    __asm {
        test ecx, ecx
        jnz L_nonempty_pool
        mov g_poolNullLeaSaveEdx, edx
        mov g_poolNullLeaSaveEdi, edi
        movzx eax, bp
        push eax
        call ResolveChatBubblePoolNull_EcxSubstitute
        add esp, 4
        test eax, eax
        je L_restore_skip
        mov ecx, g_chatBubbleLocalPedOffsetDw
        mov edi, dword ptr [eax + ecx]
        test edi, edi
        je L_restore_skip
        mov edx, g_poolNullLeaSaveEdx
        jmp dword ptr [g_chatBubblePoolResumeMid]
L_restore_skip:
        mov edi, g_poolNullLeaSaveEdi
        mov edx, g_poolNullLeaSaveEdx
        jmp dword ptr [g_chatBubblePoolSkip]
L_nonempty_pool:
        lea eax, [edi + edx * 4]
        jmp dword ptr [g_chatBubblePoolResume]
    }
}

struct LocalDrawContext {
    void* playerTags = nullptr;
    void* localPlayer = nullptr;
    void* ped = nullptr;
    CVector headPosition{};
    const char* name = nullptr;
    std::uint16_t id = 0;
    D3DCOLOR color = kFallbackLabelColor;
    float health = 0.0f;
    float armour = 0.0f;
    float distanceToCamera = 0.0f;
};

bool BuildLocalDrawContext(LocalDrawContext& context) {
    if (g_state.version == nullptr) {
        return false;
    }

    void* const netGame = ReadGlobalObject<void>(g_state.version->refNetGameOffset);
    void* const playerTags = ReadGlobalObject<void>(g_state.version->refPlayerTagsOffset);
    if (netGame == nullptr || playerTags == nullptr) {
        return false;
    }

    void* const playerPool = CallThis<void*>(netGame, g_state.version->getPlayerPoolOffset);
    if (playerPool == nullptr) {
        return false;
    }

    void* const localPlayer = CallThis<void*>(playerPool, g_state.version->getLocalPlayerOffset);
    if (localPlayer == nullptr) {
        return false;
    }

    const std::uint16_t localId = GetLocalPlayerId(playerPool);

    const char* const localName =
        CallThis<const char*>(playerPool, g_state.version->getNameByIdOffset, static_cast<unsigned int>(localId));
    if (localName == nullptr || localName[0] == '\0') {
        return false;
    }

    void* const ped = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(localPlayer) + g_state.version->localPedOffset);
    if (ped == nullptr) {
        return false;
    }

    if (!CallThis<BOOL>(ped, g_state.version->pedIsOnScreenOffset)) {
        return false;
    }

    const float health = CallThis<float>(ped, g_state.version->pedGetHealthOffset);
    if (!(health > 0.0f)) {
        return false;
    }

    CVector headPosition{};
    CallThis<void>(ped, g_state.version->pedGetBonePositionOffset, kHeadBoneId, &headPosition);

    D3DCOLOR color = CallThis<D3DCOLOR>(localPlayer, g_state.version->getLocalPlayerColorArgbOffset);
    if (color == 0) {
        color = kFallbackLabelColor;
    }

    context.playerTags = playerTags;
    context.localPlayer = localPlayer;
    context.ped = ped;
    context.headPosition = headPosition;
    context.name = localName;
    context.id = localId;
    context.color = color;
    context.health = health;
    context.armour = CallThis<float>(ped, g_state.version->pedGetArmourOffset);
    if (context.armour < 0.0f) {
        context.armour = 0.0f;
    }
    context.distanceToCamera = CalculateDistance(headPosition, GetCameraPosition());
    if (!std::isfinite(context.distanceToCamera)) {
        context.distanceToCamera = 0.0f;
    }

    return true;
}

bool WantChatBubbleLocalDrawPatches() {
    return g_state.mirrorOwnChatBubble || g_state.overlayRuleCount > 0;
}

/** Канонический AARRGGBB (как у `DrawLabel` / wiki) → dword для `CChatBubble::Add`: иначе {FF90FF} рисуется как FFFF90. */
D3DCOLOR ChatBubbleAddColorFromCanonicalArgb(D3DCOLOR canonical) {
    const std::uint32_t u = static_cast<std::uint32_t>(canonical);
    const std::uint32_t a = (u >> 24) & 0xFFu;
    const std::uint32_t r = (u >> 16) & 0xFFu;
    const std::uint32_t g = (u >> 8) & 0xFFu;
    const std::uint32_t b = u & 0xFFu;
    return static_cast<D3DCOLOR>((a << 24) | (g << 16) | (b << 8) | r);
}

void PushLocalPlayerChatBubble(
    const LocalDrawContext& context, const char* text, D3DCOLOR color, int lifeMs) {
    if (g_state.version == nullptr || context.id == 0) {
        return;
    }

    void* const bubble = ReadGlobalObject<void>(g_state.version->refChatBubbleOffset);
    if (bubble == nullptr) {
        return;
    }

    char line[kChatBubbleLineMax] = {};
    if (text != nullptr) {
        strncpy_s(line, sizeof(line), text, _TRUNCATE);
    }

    const D3DCOLOR addColor = ChatBubbleAddColorFromCanonicalArgb(color);

    CallThis<void>(
        bubble,
        g_state.version->chatBubbleAddOffset,
        static_cast<unsigned int>(context.id),
        line,
        addColor,
        context.distanceToCamera,
        lifeMs);
}

void __fastcall SendCommandDetour(void* thisPtr, void* /*edx*/, const char* text) {
    if (text && StrEqualNoCase(text, g_state.toggleCommand)) {
        g_state.renderEnabled = !g_state.renderEnabled;
        return;
    }

    if (text != nullptr && g_state.overlayRuleCount > 0) {
        for (unsigned int i = 0; i < g_state.overlayRuleCount; ++i) {
            const OverlayCommandRule& rule = g_state.overlayRules[i];
            if (rule.cmd[0] == '\0') {
                continue;
            }
            if (!FirstTokenEqualsNoCase(text, rule.cmd)) {
                continue;
            }

            const char* rest = RestAfterFirstToken(text);
            LocalDrawContext bubbleCtx{};
            const bool ctxOk = BuildLocalDrawContext(bubbleCtx);

            // Сначала штатный `SendCommand`, иначе SA:MP снова кладёт бабл (напр. /me с цветом по умолчанию)
            // в тот же слот и перезаписывает наш `Add` с `ColorN`.
            if (rule.forwardToServer) {
                if (g_originalSendCommand) {
                    g_originalSendCommand(thisPtr, text);
                }
            }

            if (ctxOk) {
                if (*rest == '\0') {
                    PushLocalPlayerChatBubble(bubbleCtx, "", rule.color, 1);
                } else {
                    char bubbleText[kChatBubbleLineMax] = {};
                    BuildOverlayBubbleText(bubbleText, sizeof(bubbleText), rule, rest);
                    PushLocalPlayerChatBubble(
                        bubbleCtx, bubbleText, rule.color, g_state.chatBubbleLifeMs);
                }
            }
            return;
        }
    }

    if (g_originalSendCommand) {
        g_originalSendCommand(thisPtr, text);
    }
}

bool LooksLikeNearJeSkip(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x0F && bytes[1] == 0x84;
}

bool LooksLikeChatBubblePoolNullEcxJe8(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x85 && bytes[1] == 0xC9 && bytes[2] == 0x0F && bytes[3] == 0x84;
}

bool LooksLikeChatBubblePoolNullEdxJe8(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x85 && bytes[1] == 0xD2 && bytes[2] == 0x0F && bytes[3] == 0x84;
}

bool LooksLikeChatBubblePoolNullEcxLeaJe12(const void* address) {
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    return bytes[0] == 0x85 && bytes[1] == 0xC9 && bytes[2] == 0x8D && bytes[3] == 0x04 && bytes[4] == 0x97
        && bytes[5] == 0x0F && bytes[6] == 0x84;
}

bool ApplyChatBubblePoolNullTrampoline() {
    if (g_state.version == nullptr || g_state.sampBase == 0) {
        return true;
    }

    const std::uint8_t kind = g_state.version->chatBubblePoolNullTrampKind;
    const std::uint32_t patchRva = g_state.version->chatBubblePoolNullTrampPatchRva;
    if (kind == kChatBubblePoolTrampNone || patchRva == 0) {
        return true;
    }

    const std::size_t patchBytes = (kind == kChatBubblePoolTrampEcxLeaJe12) ? 12u : 8u;
    void* const patchAt = reinterpret_cast<void*>(g_state.sampBase + patchRva);

    bool signatureOk = false;
    if (kind == kChatBubblePoolTrampEcxJe8) {
        signatureOk = LooksLikeChatBubblePoolNullEcxJe8(patchAt);
    } else if (kind == kChatBubblePoolTrampEdxJe8) {
        signatureOk = LooksLikeChatBubblePoolNullEdxJe8(patchAt);
    } else if (kind == kChatBubblePoolTrampEcxLeaJe12) {
        signatureOk = LooksLikeChatBubblePoolNullEcxLeaJe12(patchAt);
    }
    if (!signatureOk) {
        return false;
    }

    g_chatBubblePoolResume = reinterpret_cast<void*>(
        g_state.sampBase + g_state.version->chatBubblePoolNullTrampResumeRva);
    if (g_state.version->chatBubblePoolNullTrampResumeMidRva != 0) {
        g_chatBubblePoolResumeMid = reinterpret_cast<void*>(
            g_state.sampBase + g_state.version->chatBubblePoolNullTrampResumeMidRva);
    } else {
        g_chatBubblePoolResumeMid = nullptr;
    }
    g_chatBubblePoolSkip =
        reinterpret_cast<void*>(g_state.sampBase + g_state.version->chatBubblePoolNullTrampSkipRva);

    if (g_chatBubblePoolResumeMid == nullptr) {
        return false;
    }

    g_chatBubbleLocalPedOffsetDw = g_state.version->localPedOffset;

    const void* trampEntry = nullptr;
    if (kind == kChatBubblePoolTrampEcxJe8) {
        trampEntry = reinterpret_cast<const void*>(&ChatBubblePoolNullTrampoline_EcxJe8);
    } else if (kind == kChatBubblePoolTrampEdxJe8) {
        trampEntry = reinterpret_cast<const void*>(&ChatBubblePoolNullTrampoline_EdxJe8);
    } else {
        trampEntry = reinterpret_cast<const void*>(&ChatBubblePoolNullTrampoline_EcxLeaJe12);
    }

    std::memcpy(g_chatBubblePoolEarlyOrig, patchAt, patchBytes);

    std::uint8_t patchBuf[kChatBubblePoolNullTrampPatchBytesMax]{};
    if (!BuildJumpPatch(patchBuf, patchBytes, patchAt, trampEntry)) {
        return false;
    }

    if (!WriteBytes(patchAt, patchBuf, patchBytes)) {
        return false;
    }

    return true;
}

bool ApplyChatBubbleLocalDrawBypass() {
    if (g_state.version == nullptr || g_state.sampBase == 0) {
        return false;
    }
    if (g_state.version->chatBubbleLocalSkipJeRva == 0) {
        return false;
    }

    void* const patchAt =
        reinterpret_cast<void*>(g_state.sampBase + g_state.version->chatBubbleLocalSkipJeRva);
    if (!LooksLikeNearJeSkip(patchAt)) {
        return false;
    }

    std::memcpy(g_chatBubbleJeOrig, patchAt, kChatBubbleLocalSkipJePatchBytes);
    static constexpr std::uint8_t kNops[kChatBubbleLocalSkipJePatchBytes] = {
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    if (!WriteBytes(patchAt, kNops, sizeof(kNops))) {
        return false;
    }
    return true;
}

void __fastcall LocalPlayerChatDetour(void* thisPtr, void* /*edx*/, const char* text) {
    if (g_originalLocalPlayerChat != nullptr) {
        g_originalLocalPlayerChat(thisPtr, text);
    }

    if (!g_state.mirrorOwnChatBubble) {
        return;
    }
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    if (text[0] == '/') {
        return;
    }

    LocalDrawContext context{};
    if (!BuildLocalDrawContext(context)) {
        return;
    }

    if (thisPtr != context.localPlayer) {
        return;
    }

    if (context.id == 0) {
        return;
    }

    PushLocalPlayerChatBubble(context, text, kFallbackLabelColor, g_state.chatBubbleLifeMs);
}

void DrawLocalLabel() {
    LocalDrawContext context{};
    if (!BuildLocalDrawContext(context)) {
        return;
    }

    char text[320] = {};
    _snprintf_s(text, _TRUNCATE, "%s (%u)", context.name, static_cast<unsigned int>(context.id));

    CallThis<void>(context.playerTags, g_state.version->beginLabelOffset);
    CallThis<void>(
        context.playerTags,
        g_state.version->drawLabelOffset,
        &context.headPosition,
        text,
        context.color,
        context.distanceToCamera,
        false,
        0);
    CallThis<void>(context.playerTags, g_state.version->endLabelOffset);
}

void DrawLocalHealthBar() {
    LocalDrawContext context{};
    if (!BuildLocalDrawContext(context)) {
        return;
    }

    CallThis<void>(context.playerTags, g_state.version->beginHealthBarOffset);
    CallThis<void>(
        context.playerTags,
        g_state.version->drawHealthBarOffset,
        &context.headPosition,
        context.health,
        context.armour,
        context.distanceToCamera);
    CallThis<void>(context.playerTags, g_state.version->endHealthBarOffset);
}

void __cdecl HookLabelLoop() {
    if (g_state.originalLabelLoop != nullptr) {
        g_state.originalLabelLoop();
    }

    if (g_state.renderEnabled) {
        DrawLocalLabel();
    }
}

void __cdecl HookHealthLoop() {
    if (g_state.originalHealthLoop != nullptr) {
        g_state.originalHealthLoop();
    }

    if (g_state.renderEnabled) {
        DrawLocalHealthBar();
    }
}

bool InstallHooks() {
    if (g_state.version == nullptr || g_state.sampBase == 0) {
        return false;
    }

    if (WantChatBubbleLocalDrawPatches()) {
        if (!ApplyChatBubblePoolNullTrampoline()) {
            return false;
        }
        if (!ApplyChatBubbleLocalDrawBypass()) {
            return false;
        }
    }

    if (!InstallDetour(
            reinterpret_cast<void*>(g_state.sampBase + g_state.version->labelLoopOffset),
            reinterpret_cast<const void*>(&HookLabelLoop),
            g_state.originalLabelLoop)) {
        return false;
    }

    if (!InstallDetour(
            reinterpret_cast<void*>(g_state.sampBase + g_state.version->healthLoopOffset),
            reinterpret_cast<const void*>(&HookHealthLoop),
            g_state.originalHealthLoop)) {
        return false;
    }

    void* sendCommandTarget = reinterpret_cast<void*>(g_state.sampBase + g_state.version->sendCommandOffset);
    if (MH_CreateHook(sendCommandTarget, reinterpret_cast<void*>(&SendCommandDetour),
                       reinterpret_cast<void**>(&g_originalSendCommand)) != MH_OK) {
        return false;
    }
    if (MH_EnableHook(sendCommandTarget) != MH_OK) {
        return false;
    }

    if (g_state.mirrorOwnChatBubble && g_state.version->localPlayerChatOffset != 0) {
        void* chatTarget =
            reinterpret_cast<void*>(g_state.sampBase + g_state.version->localPlayerChatOffset);
        if (MH_CreateHook(
                chatTarget,
                reinterpret_cast<void*>(&LocalPlayerChatDetour),
                reinterpret_cast<void**>(&g_originalLocalPlayerChat))
            != MH_OK) {
            return false;
        }
        if (MH_EnableHook(chatTarget) != MH_OK) {
            return false;
        }
    }

    return true;
}

void LoadOverlayCommands(const char* iniPath) {
    g_state.overlayRuleCount = 0;
    g_state.overlayRules = {};

    int count = GetPrivateProfileIntA(kOverlaySection, "Count", 0, iniPath);
    if (count <= 0) {
        return;
    }
    unsigned int n = static_cast<unsigned int>(count);
    if (n > kOverlayMaxRules) {
        n = kOverlayMaxRules;
    }

    for (unsigned int i = 1; i <= n; ++i) {
        char keyCmd[24] = {};
        char keyColor[24] = {};
        char keyFwd[24] = {};
        char keyTpl[32] = {};
        char keySplit[32] = {};
        char keyAccent[32] = {};
        _snprintf_s(keyCmd, _TRUNCATE, "Cmd%u", i);
        _snprintf_s(keyColor, _TRUNCATE, "Color%u", i);
        _snprintf_s(keyFwd, _TRUNCATE, "Forward%u", i);
        _snprintf_s(keyTpl, _TRUNCATE, "BubbleTemplate%u", i);
        _snprintf_s(keySplit, _TRUNCATE, "BubbleSplit%u", i);
        _snprintf_s(keyAccent, _TRUNCATE, "AccentColor%u", i);

        OverlayCommandRule rule{};
        GetPrivateProfileStringA(kOverlaySection, keyCmd, "", rule.cmd, static_cast<DWORD>(sizeof(rule.cmd)), iniPath);
        if (rule.cmd[0] == '\0') {
            continue;
        }

        char colorBuf[96] = {};
        GetPrivateProfileStringA(
            kOverlaySection, keyColor, "", colorBuf, static_cast<DWORD>(sizeof(colorBuf)), iniPath);
        if (!ParseOverlayColorString(colorBuf, &rule.color)) {
            rule.color = kFallbackLabelColor;
        }

        const int fwd = GetPrivateProfileIntA(kOverlaySection, keyFwd, 1, iniPath);
        rule.forwardToServer = (fwd != 0);

        GetPrivateProfileStringA(
            kOverlaySection,
            keyTpl,
            "",
            rule.bubbleTemplate,
            static_cast<DWORD>(sizeof(rule.bubbleTemplate)),
            iniPath);
        GetPrivateProfileStringA(
            kOverlaySection,
            keySplit,
            "",
            rule.bubbleSplit,
            static_cast<DWORD>(sizeof(rule.bubbleSplit)),
            iniPath);

        char accentBuf[96] = {};
        GetPrivateProfileStringA(
            kOverlaySection, keyAccent, "", accentBuf, static_cast<DWORD>(sizeof(accentBuf)), iniPath);
        if (accentBuf[0] != '\0' && ParseOverlayColorString(accentBuf, &rule.accentColor)) {
            rule.accentColorValid = true;
        }

        g_state.overlayRules[g_state.overlayRuleCount] = rule;
        ++g_state.overlayRuleCount;
    }
}

void BuildDirFromPath(char* dirOut, const char* filePath) {
    std::memcpy(dirOut, filePath, MAX_PATH);
    char* lastSlash = nullptr;
    for (char* p = dirOut; *p; ++p) {
        if (*p == '\\' || *p == '/') lastSlash = p;
    }
    if (lastSlash) {
        *(lastSlash + 1) = '\0';
    } else {
        dirOut[0] = '\0';
    }
}

bool TagOnPlayerIniFileExists(const char* iniPath) {
    const DWORD a = GetFileAttributesA(iniPath);
    if (a == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/** Первый запуск: нет **`tagOnPlayer.ini`** — создаём файл с дефолтным содержимым (как эталонный `tagOnPlayer.ini`: CRLF). */
void WriteDefaultTagOnPlayerIniIfAbsent(const char* iniPath) {
    if (TagOnPlayerIniFileExists(iniPath)) {
        return;
    }
    static const char kBody[] =
        "[Settings]\r\n"
        "Command=/tagon\r\n"
        "EnabledByDefault=1\r\n"
        "MirrorOwnChatBubble=1\r\n"
        "ChatBubbleLifeMs=6000\r\n"
        "; OverlayBubbleLinePx: max bubble text line width (px) for overlay bubble wrap heuristic (GDI vs D3DX). "
        "Per supported samp.dll (rizin): 257 (`push 0x101` before inner DrawText); 0=off.\r\n"
        "OverlayBubbleLinePx=257\r\n"
        "\r\n"
        "; [OverlayCommands] ColorN: only {RRGGBB}, {AARRGGBB} (dword ARGB), or signed decimal D3DCOLOR.\r\n"
        "; Optional: BubbleTemplateN, BubbleSplitN, AccentColorN\r\n"
        "; Placeholders: {0} first segment, {1} after first BubbleSplit, {c1}/{c2} embed {RRGGBB} from ColorN / "
        "AccentColorN\r\n"
        "; IC speech without /cmd is not matched here.\r\n"
        "[OverlayCommands]\r\n"
        "Count=14\r\n"
        "Cmd1=/me\r\n"
        "Color1={ff90ff}\r\n"
        "Forward1=1\r\n"
        "BubbleTemplate1={c1}* {0}\r\n"
        "Cmd2=/do\r\n"
        "Color2={D6A2E8}\r\n"
        "Forward2=1\r\n"
        "Cmd3=/todo\r\n"
        "Color3={FFFFFF}\r\n"
        "Forward3=1\r\n"
        "BubbleSplit3=*\r\n"
        "AccentColor3={FF99FF}\r\n"
        "BubbleTemplate3={c1}{0}*{c2}{1}";

    HANDLE h = CreateFileA(
        iniPath,
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    const DWORD n = static_cast<DWORD>(sizeof(kBody) - 1u);
    DWORD written = 0;
    WriteFile(h, kBody, n, &written, nullptr);
    CloseHandle(h);
}

void LoadOverlayBubbleLinePxFromIni(const char* iniPath, int defaultPx) {
    int bubbleLinePx =
        GetPrivateProfileIntA(kConfigSection, kConfigKeyOverlayBubbleLinePx, defaultPx, iniPath);
    if (bubbleLinePx < 0) {
        bubbleLinePx = 0;
    }
    if (bubbleLinePx > 2048) {
        bubbleLinePx = 2048;
    }
    g_state.overlayBubbleLinePx = bubbleLinePx;
}

/** После `DetectSampVersion`: дефолт `GetPrivateProfileInt` для отсутствующего ключа — ширина из `samp.dll` для этой строки `kSupportedVersions`. */
void RefreshOverlayBubbleLinePxAfterVersionDetect() {
    if (g_state.version == nullptr || g_tagOnPlayerIniPath[0] == '\0') {
        return;
    }
    LoadOverlayBubbleLinePxFromIni(
        g_tagOnPlayerIniPath, static_cast<int>(g_state.version->sampBubbleDrawTextLinePx));
}

void LoadConfig() {
    g_tagOnPlayerIniPath[0] = '\0';
    char modulePath[MAX_PATH] = {};
    bool found = false;

    if (GetModuleFileNameA(g_state.pluginModule, modulePath, MAX_PATH) != 0) {
        found = true;
    }

    if (!found) {
        HMODULE resolvedModule = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&LoadConfig),
                &resolvedModule)
            && resolvedModule != nullptr
            && GetModuleFileNameA(resolvedModule, modulePath, MAX_PATH) != 0) {
            found = true;
        }
    }

    if (!found) {
        if (GetModuleFileNameA(nullptr, modulePath, MAX_PATH) == 0) {
            return;
        }
    }

    char dirPath[MAX_PATH] = {};
    BuildDirFromPath(dirPath, modulePath);

    char iniPath[MAX_PATH] = {};
    _snprintf_s(iniPath, _TRUNCATE, "%stagOnPlayer.ini", dirPath);
    strcpy_s(g_tagOnPlayerIniPath, iniPath);

    WriteDefaultTagOnPlayerIniIfAbsent(iniPath);

    char commandBuf[64] = {};
    GetPrivateProfileStringA(kConfigSection, kConfigKeyCommand, "", commandBuf, sizeof(commandBuf), iniPath);
    if (commandBuf[0] == '\0') {
        std::memcpy(commandBuf, kDefaultCommand, sizeof(kDefaultCommand));
    }
    std::memcpy(g_state.toggleCommand, commandBuf, sizeof(g_state.toggleCommand));

    int enabled = GetPrivateProfileIntA(kConfigSection, kConfigKeyEnabled, 1, iniPath);
    g_state.renderEnabled = (enabled != 0);

    int mirror = GetPrivateProfileIntA(kConfigSection, kConfigKeyMirrorOwnChatBubble, 1, iniPath);
    g_state.mirrorOwnChatBubble = (mirror != 0);

    int life = GetPrivateProfileIntA(
        kConfigSection, kConfigKeyChatBubbleLifeMs, kDefaultChatBubbleLifeMs, iniPath);
    if (life < 500) {
        life = 500;
    }
    if (life > 600000) {
        life = 600000;
    }
    g_state.chatBubbleLifeMs = life;

    LoadOverlayBubbleLinePxFromIni(iniPath, kDefaultOverlayBubbleLinePx);

    LoadOverlayCommands(iniPath);

    WritePrivateProfileStringA(kConfigSection, kConfigKeyCommand, g_state.toggleCommand, iniPath);
    char enabledStr[4] = {};
    _snprintf_s(enabledStr, _TRUNCATE, "%d", g_state.renderEnabled ? 1 : 0);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyEnabled, enabledStr, iniPath);
    char mirrorStr[4] = {};
    _snprintf_s(mirrorStr, _TRUNCATE, "%d", g_state.mirrorOwnChatBubble ? 1 : 0);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyMirrorOwnChatBubble, mirrorStr, iniPath);
    char lifeStr[16] = {};
    _snprintf_s(lifeStr, _TRUNCATE, "%d", g_state.chatBubbleLifeMs);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyChatBubbleLifeMs, lifeStr, iniPath);
    char linePxStr[16] = {};
    _snprintf_s(linePxStr, _TRUNCATE, "%d", g_state.overlayBubbleLinePx);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyOverlayBubbleLinePx, linePxStr, iniPath);
}

DWORD WINAPI InitializePlugin(void*) {
    const auto* gtaLoadState = reinterpret_cast<volatile DWORD*>(kGtaLoadStateAddress);
    while (*gtaLoadState < 9) {
        Sleep(10);
    }

    LoadConfig();
    MH_Initialize();

    for (;;) {
        HMODULE sampModule = GetModuleHandleA("samp.dll");
        if (sampModule != nullptr) {
            g_state.sampModule = sampModule;
            g_state.sampBase = reinterpret_cast<std::uintptr_t>(sampModule);
            g_state.version = DetectSampVersion(sampModule);

            if (g_state.version == nullptr) {
                return 0;
            }

            RefreshOverlayBubbleLinePxAfterVersionDetect();

            if (!InstallHooks()) {
                return 0;
            }
            return 0;
        }

        Sleep(100);
    }
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_state.pluginModule = module;
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, &InitializePlugin, nullptr, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        OverlayBubbleMeasureShutdown();
    }

    return TRUE;
}
