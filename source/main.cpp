#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "chat_bubble.h"
#include "external/MinHook/include/MinHook.h"

static_assert(sizeof(void*) == 4, "tagOnPlayer.asi must be built for Win32.");

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
constexpr char kConfigKeyOwnChatBubbleColor[] = "OwnChatBubbleColor";
/** Дефолт ширины строки бабла (px), если INI без ключа; по rizin во всех строках `kSupportedVersions` — **257** (`push 0x101` перед внутренним `DrawText`). */
constexpr char kConfigKeyOverlayBubbleLinePx[] = "OverlayBubbleLinePx";
constexpr char kConfigKeyStackChatBubbles[] = "StackChatBubbles";
constexpr char kConfigKeyStackedChatBubbleMax[] = "StackedChatBubbleMax";
constexpr char kConfigKeyStackedChatBubbleMarginPx[] = "StackedChatBubbleMarginPx";
constexpr char kDefaultCommand[] = "/tagon";
constexpr std::size_t kToggleCommandMax = 64;
constexpr char kOverlaySection[] = "OverlayCommands";

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

constexpr std::array<SampVersionInfo, 8> kSupportedVersions{{
    // chatBubbleDrawOffset — RVA CChatBubble::Draw (канон SAMP-API / rizin); chatBubbleLocalSkipJeRva — начало near-je
    // «пропуск слота», если флаг видимости бабла нулевой (в т.ч. локальный игрок); патч: 6×NOP поверх 0F 84 …
    {0x31DF13, SampVersion::R1,    "R1",    0x0021A0F8, 0x0021A0B0, 0x00070D40, 0x0006FC30, 0x00065C60, 0x000686A0, 0x000686B0, 0x000686C0, 0x00068FD0, 0x00068670, 0x000689C0, 0x00001160, 0x00001A30, 0x00013CE0, 0x00003D90, 0x000A65A0, 0x000A6610, 0x000A6650, 0x000A8D70, 0x00000004, 0x00000000, 0x000057F0, 0x0021A0DC, 0x00063250, 0x00063310, 0x000633DA, 0x000633B7, 0x000633BF, 0x000633F4, 0x00063495, 0x00000000, 0x00000000, kChatBubblePoolTrampEcxJe8, 0x000D795C, 257},
    {0x3195DD, SampVersion::R2,    "R2",    0x0021A100, 0x0021A0B8, 0x00070DE0, 0x0006FCD0, 0x00065D30, 0x00068770, 0x00068780, 0x00068790, 0x000690A0, 0x00068740, 0x00068A90, 0x00001170, 0x00001A40, 0x00013DA0, 0x00003DA0, 0x000A6770, 0x000A67E0, 0x000A6820, 0x000A8F40, 0x00000000, 0x00000000, 0x000058C0, 0x0021A0E4, 0x00063320, 0x000633E0, 0x000634AA, 0x00063481, 0x0006348C, 0x000634C3, 0x00063564, 0x00000000, 0x00000000, kChatBubblePoolTrampEcxLeaJe11, 0x000D796C, 257},
    {0x0CC490, SampVersion::R3,    "R3",    0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB430, 0x000AB480, 0x000AB4C0, 0x000ADC00, 0x00002F1C, 0x00000000, 0x00005820, 0x0026E8C0, 0x000666A0, 0x00066760, 0x0006682C, 0x00066805, 0x0006680D, 0x00066846, 0x000668E7, 0x00000000, 0x00000000, kChatBubblePoolTrampEdxJe8, 0x000E9C3C, 257},
    {0x0CC4D0, SampVersion::R3_1,  "R3-1",  0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB450, 0x000AB4C0, 0x000AB500, 0x000ADBF0, 0x00002F1C, 0x00000000, 0x00005820, 0x0026E8C0, 0x000666A0, 0x00066760, 0x0006682C, 0x00066805, 0x0006680D, 0x00066846, 0x000668E7, 0x00000000, 0x00000000, kChatBubblePoolTrampEdxJe8, 0x000E9C3C, 257},
    {0x0CBCB0, SampVersion::R4,    "R4",    0x0026EA0C, 0x0026E9C0, 0x00075360, 0x00074240, 0x000698C0, 0x0006CD40, 0x0006CD50, 0x0006CD60, 0x0006D670, 0x0006CD10, 0x0006D060, 0x00001170, 0x00001A40, 0x00017570, 0x00003F10, 0x000ABCF0, 0x000ABD60, 0x000ABDA0, 0x000AE490, 0x0000000C, 0x00000104, 0x00005918, 0x0026E9F0, 0x00066DD0, 0x00066E90, 0x00066F60, 0x00066F31, 0x00066F3C, 0x00066F7C, 0x0006701D, 0x00000000, 0x00000000, kChatBubblePoolTrampEcxLeaJe11, 0x000E9C94, 257},
    {0x0CBCD0, SampVersion::R4_2,  "R4-2",  0x0026EA0C, 0x0026E9C0, 0x00075390, 0x00074270, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABD20, 0x000ABD90, 0x000ABDD0, 0x000AE4C0, 0x00000004, 0x00000104, 0x00005A10, 0x0026E9F0, 0x00066E10, 0x00066ED0, 0x00066FA0, 0x00066F6E, 0x00066F79, 0x00066FBC, 0x0006705D, 0x00000000, 0x00000000, kChatBubblePoolTrampEcxLeaJe11, 0x000E9C94, 257},
    {0x0CBC90, SampVersion::R5_1,  "R5-1",  0x0026EB94, 0x0026EB48, 0x00075330, 0x00074210, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABCE0, 0x000ABD50, 0x000ABD90, 0x000AE480, 0x00000004, 0x00000104, 0x00005A10, 0x0026EB78, 0x00066E10, 0x00066ED0, 0x00066FA0, 0x00066F6E, 0x00066F79, 0x00066FBC, 0x0006705D, 0x00000000, 0x00000000, kChatBubblePoolTrampEcxLeaJe11, 0x000E9C94, 257},
    {0x0FDB60, SampVersion::DL_R1, "DL-R1", 0x002ACA24, 0x002AC9D8, 0x00074DC0, 0x00073CB0, 0x00069340, 0x0006C7C0, 0x0006C7D0, 0x0006C7E0, 0x0006D0F0, 0x0006C790, 0x0006CAE0, 0x00001170, 0x00001A80, 0x000170D0, 0x00003E20, 0x000AB900, 0x000AB970, 0x000AB9B0, 0x000AE080, 0x00000000, 0x00000000, 0x00005860, 0x002ACA08, 0x00066890, 0x00066950, 0x00066A1A, 0x000669F1, 0x000669FC, 0x00066A33, 0x00066AD4, 0x00066A03, 0x00066A0A, kChatBubblePoolTrampEcxLeaJe11, 0x0011BCCC, 257},
}};

/** Циклы тегов — `CPlayerTags::__thiscall` (`this` в `ECX`). Хук — `__fastcall(void*,void*)`: тот же `ECX`, второй аргумент в `EDX` не используется (совместимо с точкой входа). */
using RenderLoopFn = void(__thiscall*)(void* thisPtr);
using SendCommandFn = void(__thiscall*)(void*, const char*);

struct PluginState {
    HMODULE pluginModule = nullptr;
    HMODULE sampModule = nullptr;
    std::uintptr_t sampBase = 0;
    const SampVersionInfo* version = nullptr;
    RenderLoopFn originalLabelLoop = nullptr;
    RenderLoopFn originalHealthLoop = nullptr;
    bool renderEnabled = true;
    bool mirrorOwnChatBubble = false;
    int chatBubbleLifeMs = chat_bubble::kDefaultLifeMs;
    D3DCOLOR ownChatBubbleColor = kFallbackLabelColor;
    /** `0` — не вставлять `\n` перед `{RRGGBB}`; иначе макс. ширина строки (px), R1 из rizin. */
    int overlayBubbleLinePx = chat_bubble::kDefaultOverlayLinePx;
    bool stackChatBubbles = false;
    int stackedChatBubbleMax = chat_bubble::kDefaultStackMax;
    int stackedChatBubbleMarginPx = chat_bubble::kDefaultStackMarginPx;
    char toggleCommand[kToggleCommandMax] = "/tagon";
    std::array<chat_bubble::OverlayCommandRule, chat_bubble::kOverlayMaxRules> overlayRules{};
    unsigned int overlayRuleCount = 0;
};

PluginState g_state;
SendCommandFn g_originalSendCommand = nullptr;
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

chat_bubble::LocalPlayerContext ToChatBubbleContext(const LocalDrawContext& context) {
    chat_bubble::LocalPlayerContext result{};
    result.localPlayer = context.localPlayer;
    result.id = context.id;
    result.distanceToCamera = context.distanceToCamera;
    return result;
}

bool BuildLocalChatBubbleContext(chat_bubble::LocalPlayerContext& context) {
    LocalDrawContext drawContext{};
    if (!BuildLocalDrawContext(drawContext)) {
        return false;
    }
    context = ToChatBubbleContext(drawContext);
    return true;
}

void __fastcall SendCommandDetour(void* thisPtr, void* /*edx*/, const char* text) {
    if (text && StrEqualNoCase(text, g_state.toggleCommand)) {
        g_state.renderEnabled = !g_state.renderEnabled;
        return;
    }

    if (text != nullptr && g_state.overlayRuleCount > 0) {
        for (unsigned int i = 0; i < g_state.overlayRuleCount; ++i) {
            const chat_bubble::OverlayCommandRule& rule = g_state.overlayRules[i];
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
                const chat_bubble::LocalPlayerContext bubbleContext = ToChatBubbleContext(bubbleCtx);
                if (*rest == '\0') {
                    chat_bubble::PushLocalPlayerBubble(bubbleContext, "", rule.color, 1);
                } else {
                    char bubbleText[chat_bubble::kLineMax] = {};
                    chat_bubble::BuildOverlayText(bubbleText, sizeof(bubbleText), rule, rest);
                    chat_bubble::PushLocalPlayerBubble(
                        bubbleContext, bubbleText, rule.color, g_state.chatBubbleLifeMs);
                }
            }
            return;
        }
    }

    if (g_originalSendCommand) {
        g_originalSendCommand(thisPtr, text);
    }
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

void __fastcall HookLabelLoop(void* thisPtr, void* /*edxUnused*/) {
    if (g_state.originalLabelLoop != nullptr) {
        g_state.originalLabelLoop(thisPtr);
    }

    if (g_state.renderEnabled) {
        DrawLocalLabel();
    }
}

void __fastcall HookHealthLoop(void* thisPtr, void* /*edxUnused*/) {
    if (g_state.originalHealthLoop != nullptr) {
        g_state.originalHealthLoop(thisPtr);
    }

    if (g_state.renderEnabled) {
        DrawLocalHealthBar();
    }
}

bool InstallHooks() {
    if (g_state.version == nullptr || g_state.sampBase == 0) {
        return false;
    }

    chat_bubble::RuntimeConfig bubbleConfig{};
    bubbleConfig.sampBase = g_state.sampBase;
    bubbleConfig.version = g_state.version;
    bubbleConfig.mirrorOwnChatBubble = g_state.mirrorOwnChatBubble;
    bubbleConfig.lifeMs = g_state.chatBubbleLifeMs;
    bubbleConfig.overlayLinePx = g_state.overlayBubbleLinePx;
    bubbleConfig.ownChatBubbleColor = g_state.ownChatBubbleColor;
    bubbleConfig.buildLocalContext = &BuildLocalChatBubbleContext;
    bubbleConfig.stackChatBubbles = g_state.stackChatBubbles;
    bubbleConfig.stackMax = g_state.stackedChatBubbleMax;
    bubbleConfig.stackMarginPx = g_state.stackedChatBubbleMarginPx;
    chat_bubble::Configure(bubbleConfig);

    if (chat_bubble::WantLocalDrawPatches(
            g_state.mirrorOwnChatBubble, g_state.overlayRuleCount, g_state.stackChatBubbles)) {
        if (!chat_bubble::InstallLocalDrawPatches()) {
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
    const MH_STATUS sc0 = MH_CreateHook(sendCommandTarget, reinterpret_cast<void*>(&SendCommandDetour),
        reinterpret_cast<void**>(&g_originalSendCommand));
    if (sc0 != MH_OK) {
        return false;
    }
    if (MH_EnableHook(sendCommandTarget) != MH_OK) {
        return false;
    }

    if (!chat_bubble::InstallLocalPlayerChatHook()) {
        return false;
    }

    if (!chat_bubble::InstallStackHooks()) {
        return false;
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
    if (n > chat_bubble::kOverlayMaxRules) {
        n = chat_bubble::kOverlayMaxRules;
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

        chat_bubble::OverlayCommandRule rule{};
        GetPrivateProfileStringA(kOverlaySection, keyCmd, "", rule.cmd, static_cast<DWORD>(sizeof(rule.cmd)), iniPath);
        if (rule.cmd[0] == '\0') {
            continue;
        }

        char colorBuf[96] = {};
        GetPrivateProfileStringA(
            kOverlaySection, keyColor, "", colorBuf, static_cast<DWORD>(sizeof(colorBuf)), iniPath);
        if (!chat_bubble::ParseColorString(colorBuf, &rule.color)) {
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
        if (accentBuf[0] != '\0' && chat_bubble::ParseColorString(accentBuf, &rule.accentColor)) {
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
        "; OwnChatBubbleColor: mirror bubble for outgoing chat without leading / — same formats as ColorN "
        "(see README).\r\n"
        "OwnChatBubbleColor={FFFFFF}\r\n"
        "; OverlayBubbleLinePx: max bubble text line width (px) for overlay bubble wrap heuristic (GDI vs D3DX). "
        "Per supported samp.dll (rizin): 257 (`push 0x101` before inner DrawText); 0=off.\r\n"
        "OverlayBubbleLinePx=257\r\n"
        "; StackChatBubbles: 1 = stack recent chat bubbles above players (native CLabel draw).\r\n"
        "StackChatBubbles=0\r\n"
        "StackedChatBubbleMax=5\r\n"
        "StackedChatBubbleMarginPx=5\r\n"
        "\r\n"
        "; [OverlayCommands] ColorN: {RRGGBB}, {RRGGBBAA}, bare 6/8 hex (RRGGBB / RRGGBBAA), or signed decimal "
        "D3DCOLOR (RRGGBBAA dword).\r\n"
        "; BubbleTemplateN: placeholders [msg] full tail, [a]/[b] before/after BubbleSplitN, [c]/[d] color embeds "
        "({RRGGBB}) from ColorN / AccentColorN.\r\n"
        "; IC speech without /cmd is not matched here.\r\n"
        "[OverlayCommands]\r\n"
        "Count=3\r\n"
        "Cmd1=/me\r\n"
        "Color1={ff90ff}\r\n"
        "Forward1=1\r\n"
        "BubbleTemplate1=[c]* [a]\r\n"
        "Cmd2=/do\r\n"
        "Color2={D6A2E8}\r\n"
        "Forward2=1\r\n"
        "Cmd3=/todo\r\n"
        "Color3={FFFFFF}\r\n"
        "Forward3=1\r\n"
        "BubbleSplit3=*\r\n"
        "AccentColor3={FF99FF}\r\n"
        "BubbleTemplate3=[c][a]*[d][b]";

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
        kConfigSection, kConfigKeyChatBubbleLifeMs, chat_bubble::kDefaultLifeMs, iniPath);
    if (life < 500) {
        life = 500;
    }
    if (life > 600000) {
        life = 600000;
    }
    g_state.chatBubbleLifeMs = life;

    char ownBubbleColorBuf[48] = {};
    GetPrivateProfileStringA(
        kConfigSection, kConfigKeyOwnChatBubbleColor, "", ownBubbleColorBuf, sizeof(ownBubbleColorBuf), iniPath);
    if (ownBubbleColorBuf[0] == '\0'
        || !chat_bubble::ParseColorString(ownBubbleColorBuf, &g_state.ownChatBubbleColor)) {
        g_state.ownChatBubbleColor = kFallbackLabelColor;
    }

    LoadOverlayBubbleLinePxFromIni(iniPath, chat_bubble::kDefaultOverlayLinePx);

    int stack = GetPrivateProfileIntA(kConfigSection, kConfigKeyStackChatBubbles, 0, iniPath);
    g_state.stackChatBubbles = (stack != 0);

    int stackMax = GetPrivateProfileIntA(
        kConfigSection, kConfigKeyStackedChatBubbleMax, chat_bubble::kDefaultStackMax, iniPath);
    if (stackMax < 1) {
        stackMax = 1;
    }
    if (stackMax > static_cast<int>(chat_bubble::kMaxStackedBubblesHardMax)) {
        stackMax = static_cast<int>(chat_bubble::kMaxStackedBubblesHardMax);
    }
    g_state.stackedChatBubbleMax = stackMax;

    int stackMargin = GetPrivateProfileIntA(
        kConfigSection, kConfigKeyStackedChatBubbleMarginPx, chat_bubble::kDefaultStackMarginPx, iniPath);
    if (stackMargin < 0) {
        stackMargin = 0;
    }
    if (stackMargin > 64) {
        stackMargin = 64;
    }
    g_state.stackedChatBubbleMarginPx = stackMargin;

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
    char ownColorStr[20] = {};
    const D3DCOLOR oc = g_state.ownChatBubbleColor;
    const unsigned r = static_cast<unsigned>((oc >> 24) & 0xFFu);
    const unsigned g = static_cast<unsigned>((oc >> 16) & 0xFFu);
    const unsigned b = static_cast<unsigned>((oc >> 8) & 0xFFu);
    const unsigned a = static_cast<unsigned>(oc & 0xFFu);
    if (a != 0xFFu) {
        _snprintf_s(ownColorStr, _TRUNCATE, "{%02X%02X%02X%02X}", r, g, b, a);
    } else {
        _snprintf_s(ownColorStr, _TRUNCATE, "{%02X%02X%02X}", r, g, b);
    }
    WritePrivateProfileStringA(kConfigSection, kConfigKeyOwnChatBubbleColor, ownColorStr, iniPath);
    char linePxStr[16] = {};
    _snprintf_s(linePxStr, _TRUNCATE, "%d", g_state.overlayBubbleLinePx);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyOverlayBubbleLinePx, linePxStr, iniPath);
    char stackStr[4] = {};
    _snprintf_s(stackStr, _TRUNCATE, "%d", g_state.stackChatBubbles ? 1 : 0);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyStackChatBubbles, stackStr, iniPath);
    char stackMaxStr[8] = {};
    _snprintf_s(stackMaxStr, _TRUNCATE, "%d", g_state.stackedChatBubbleMax);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyStackedChatBubbleMax, stackMaxStr, iniPath);
    char stackMarginStr[8] = {};
    _snprintf_s(stackMarginStr, _TRUNCATE, "%d", g_state.stackedChatBubbleMarginPx);
    WritePrivateProfileStringA(
        kConfigSection, kConfigKeyStackedChatBubbleMarginPx, stackMarginStr, iniPath);
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
        chat_bubble::Shutdown();
    }

    return TRUE;
}
