#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
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
constexpr char kDefaultCommand[] = "/tagon";

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
};

constexpr std::array<SampVersionInfo, 8> kSupportedVersions{{
    {0x31DF13, SampVersion::R1,    "R1",    0x0021A0F8, 0x0021A0B0, 0x00070D40, 0x0006FC30, 0x00065C60, 0x000686A0, 0x000686B0, 0x000686C0, 0x00068FD0, 0x00068670, 0x000689C0, 0x00001160, 0x00001A30, 0x00013CE0, 0x00003D90, 0x000A65A0, 0x000A6610, 0x000A6650, 0x000A8D70, 0x00000004, 0x00000000},
    {0x3195DD, SampVersion::R2,    "R2",    0x0021A100, 0x0021A0B8, 0x00070DE0, 0x0006FCD0, 0x00065D30, 0x00068770, 0x00068780, 0x00068790, 0x000690A0, 0x00068740, 0x00068A90, 0x00001170, 0x00001A40, 0x00013DA0, 0x00003DA0, 0x000A6770, 0x000A67E0, 0x000A6820, 0x000A8F40, 0x00000000, 0x00000000},
    {0x0CC490, SampVersion::R3,    "R3",    0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB430, 0x000AB480, 0x000AB4C0, 0x000ADC00, 0x00002F1C, 0x00000000},
    {0x0CC4D0, SampVersion::R3_1,  "R3-1",  0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB450, 0x000AB4C0, 0x000AB500, 0x000ADBF0, 0x00002F1C, 0x00000000},
    {0x0CBCB0, SampVersion::R4,    "R4",    0x0026EA0C, 0x0026E9C0, 0x00075360, 0x00074240, 0x000698C0, 0x0006CD40, 0x0006CD50, 0x0006CD60, 0x0006D670, 0x0006CD10, 0x0006D060, 0x00001170, 0x00001A40, 0x00017570, 0x00003F10, 0x000ABCF0, 0x000ABD60, 0x000ABDA0, 0x000AE490, 0x0000000C, 0x00000104},
    {0x0CBCD0, SampVersion::R4_2,  "R4-2",  0x0026EA0C, 0x0026E9C0, 0x00075390, 0x00074270, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABD20, 0x000ABD90, 0x000ABDD0, 0x000AE4C0, 0x00000004, 0x00000104},
    {0x0CBC90, SampVersion::R5_1,  "R5-1",  0x0026EB94, 0x0026EB48, 0x00075330, 0x00074210, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABCE0, 0x000ABD50, 0x000ABD90, 0x000AE480, 0x00000004, 0x00000104},
    {0x0FDB60, SampVersion::DL_R1, "DL-R1", 0x002ACA24, 0x002AC9D8, 0x00074DC0, 0x00073CB0, 0x00069340, 0x0006C7C0, 0x0006C7D0, 0x0006C7E0, 0x0006D0F0, 0x0006C790, 0x0006CAE0, 0x00001170, 0x00001A80, 0x000170D0, 0x00003E20, 0x000AB900, 0x000AB970, 0x000AB9B0, 0x000AE080, 0x00000000, 0x00000000},
}};

using RenderLoopFn = void(__cdecl*)();
using SendCommandFn = void(__thiscall*)(void*, const char*);

struct PluginState {
    HMODULE pluginModule = nullptr;
    HMODULE sampModule = nullptr;
    std::uintptr_t sampBase = 0;
    const SampVersionInfo* version = nullptr;
    RenderLoopFn originalLabelLoop = nullptr;
    RenderLoopFn originalHealthLoop = nullptr;
    bool renderEnabled = true;
    char toggleCommand[64] = "/tagon";
};

PluginState g_state;
SendCommandFn g_originalSendCommand = nullptr;

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

void __fastcall SendCommandDetour(void* thisPtr, void* /*edx*/, const char* text) {
    if (text && StrEqualNoCase(text, g_state.toggleCommand)) {
        g_state.renderEnabled = !g_state.renderEnabled;
        return;
    }
    if (g_originalSendCommand) {
        g_originalSendCommand(thisPtr, text);
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

    return true;
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

void LoadConfig() {
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

    char commandBuf[64] = {};
    GetPrivateProfileStringA(kConfigSection, kConfigKeyCommand, "", commandBuf, sizeof(commandBuf), iniPath);
    if (commandBuf[0] == '\0') {
        std::memcpy(commandBuf, kDefaultCommand, sizeof(kDefaultCommand));
    }
    std::memcpy(g_state.toggleCommand, commandBuf, sizeof(g_state.toggleCommand));

    int enabled = GetPrivateProfileIntA(kConfigSection, kConfigKeyEnabled, 1, iniPath);
    g_state.renderEnabled = (enabled != 0);

    WritePrivateProfileStringA(kConfigSection, kConfigKeyCommand, g_state.toggleCommand, iniPath);
    char enabledStr[4] = {};
    _snprintf_s(enabledStr, _TRUNCATE, "%d", g_state.renderEnabled ? 1 : 0);
    WritePrivateProfileStringA(kConfigSection, kConfigKeyEnabled, enabledStr, iniPath);
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

            InstallHooks();
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
    }

    return TRUE;
}
