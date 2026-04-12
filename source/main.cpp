#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>

static_assert(sizeof(void*) == 4, "tagOnPlayer.asi must be built for Win32.");

using D3DCOLOR = std::uint32_t;
using Loop = void(__cdecl*)();
using SendCommand = void(__thiscall*)(std::uintptr_t, const char*);

extern "C" int _fltused = 0;
extern "C" void* __cdecl memcpy(void* destination, const void* source, std::size_t size);
extern "C" void* __cdecl memset(void* destination, int value, std::size_t size);

namespace {

constexpr DWORD kGtaLoadState = 0x00C8D4C0;
constexpr std::uintptr_t kCamera = 0x00B6F028;
constexpr std::size_t kDrawHookSize = 9;
constexpr std::size_t kSendHookSize = 13;
constexpr unsigned int kHeadBone = 8;
constexpr D3DCOLOR kWhite = 0xFFFFFFFFu;
constexpr char kIniName[] = "tagOnPlayer.ini";
constexpr char kIniSection[] = "main";
constexpr char kIniKeyCommand[] = "command";
constexpr char kIniKeyEnabled[] = "enabled_by_default";
constexpr char kDefaultCommand[] = "/tagon";

struct Vec3 { float x, y, z; };
struct Matrix { Vec3 right; std::uint32_t f0; Vec3 up; std::uint32_t f1; Vec3 at; std::uint32_t f2; Vec3 pos; std::uint32_t f3; void* attached; bool owned; char pad[3]; };
struct Placeable { void* vtable; struct { Vec3 pos; float heading; } simple; Matrix* matrix; };
struct Camera { Placeable placeable; };

struct Ver {
    DWORD ep;
    std::uint32_t net, tags, label_loop, bar_loop, send_command;
    std::uint32_t begin_label, end_label, draw_label, begin_bar, end_bar, draw_bar;
    std::uint32_t get_pool, get_local, get_name, get_color;
    std::uint32_t ped_on_screen, get_health, get_armour, get_bone, local_id, local_ped;
};

#define VER(...) { __VA_ARGS__ }
const Ver kVer[] = {
    // ep, net, tags, label_loop, bar_loop, send_command, begin_label, end_label, draw_label, begin_bar, end_bar, draw_bar, get_pool, get_local, get_name, get_color, ped_on_screen, get_health, get_armour, get_bone, local_id, local_ped
    VER(0x31DF13, 0x0021A0F8, 0x0021A0B0, 0x00070D40, 0x0006FC30, 0x00065C60, 0x000686A0, 0x000686B0, 0x000686C0, 0x00068FD0, 0x00068670, 0x000689C0, 0x00001160, 0x00001A30, 0x00013CE0, 0x00003D90, 0x000A65A0, 0x000A6610, 0x000A6650, 0x000A8D70, 0x00000004, 0x00000000), // R1
    VER(0x3195DD, 0x0021A100, 0x0021A0B8, 0x00070DE0, 0x0006FCD0, 0x00065D30, 0x00068770, 0x00068780, 0x00068790, 0x000690A0, 0x00068740, 0x00068A90, 0x00001170, 0x00001A40, 0x00013DA0, 0x00003DA0, 0x000A6770, 0x000A67E0, 0x000A6820, 0x000A8F40, 0x00000000, 0x00000000), // R2
    VER(0x0CC490, 0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB430, 0x000AB480, 0x000AB4C0, 0x000ADC00, 0x00002F1C, 0x00000000), // R3
    VER(0x0CC4D0, 0x0026E8DC, 0x0026E890, 0x00074C30, 0x00073B20, 0x00069190, 0x0006C610, 0x0006C620, 0x0006C630, 0x0006CF40, 0x0006C5E0, 0x0006C930, 0x00001160, 0x00001A30, 0x00016F00, 0x00003DA0, 0x000AB450, 0x000AB4C0, 0x000AB500, 0x000ADBF0, 0x00002F1C, 0x00000000), // R3-1
    VER(0x0CBCB0, 0x0026EA0C, 0x0026E9C0, 0x00075360, 0x00074240, 0x000698C0, 0x0006CD40, 0x0006CD50, 0x0006CD60, 0x0006D670, 0x0006CD10, 0x0006D060, 0x00001170, 0x00001A40, 0x00017570, 0x00003F10, 0x000ABCF0, 0x000ABD60, 0x000ABDA0, 0x000AE490, 0x0000000C, 0x00000104), // R4
    VER(0x0CBCD0, 0x0026EA0C, 0x0026E9C0, 0x00075390, 0x00074270, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABD20, 0x000ABD90, 0x000ABDD0, 0x000AE4C0, 0x00000004, 0x00000104), // R4-2
    VER(0x0CBC90, 0x0026EB94, 0x0026EB48, 0x00075330, 0x00074210, 0x00069900, 0x0006CD80, 0x0006CD90, 0x0006CDA0, 0x0006D6B0, 0x0006CD50, 0x0006D0A0, 0x00001170, 0x00001A40, 0x000175C0, 0x00003F20, 0x000ABCE0, 0x000ABD50, 0x000ABD90, 0x000AE480, 0x00000004, 0x00000104), // R5-1
    VER(0x0FDB60, 0x002ACA24, 0x002AC9D8, 0x00074DC0, 0x00073CB0, 0x00069340, 0x0006C7C0, 0x0006C7D0, 0x0006C7E0, 0x0006D0F0, 0x0006C790, 0x0006CAE0, 0x00001170, 0x00001A80, 0x000170D0, 0x00003E20, 0x000AB900, 0x000AB970, 0x000AB9B0, 0x000AE080, 0x00000000, 0x00000000), // DL-R1
};
#undef VER

struct Ctx { void* tags; Vec3 head; const char* name; std::uint16_t id; D3DCOLOR color; float health; float armour; float distance; };

HMODULE g_module = nullptr;
std::uintptr_t g_base = 0;
const Ver* g_ver = nullptr;
Loop g_old_label = nullptr;
Loop g_old_bar = nullptr;
SendCommand g_old_send = nullptr;
char g_ini[MAX_PATH] = {};
char g_cmd[32] = "/tagon";
bool g_render = true;

template <typename R, typename... A>
R call(void* self, std::uint32_t off, A... args) {
    return reinterpret_cast<R(__thiscall*)(void*, A...)>(g_base + off)(self, args...);
}

float fsqrt(float value) {
    if (!(value > 0.0f)) return 0.0f;
    float result = 0.0f;
    __asm {
        fld value
        fsqrt
        fstp result
    }
    return result;
}

void cpy(char* dst, std::size_t size, const char* src) {
    if (!dst || !size) return;
    std::size_t i = 0;
    while (src && src[i] && i + 1 < size) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

bool sp(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
char lo(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

void norm(char* dst, std::size_t size, const char* src) {
    char tmp[64];
    std::size_t i = 0;
    while (src && sp(*src)) ++src;
    if (src && *src && *src != '/' && i + 1 < sizeof(tmp)) tmp[i++] = '/';
    while (src && *src && !sp(*src) && i + 1 < sizeof(tmp)) tmp[i++] = *src++;
    tmp[i] = 0;
    cpy(dst, size, tmp[0] && !(tmp[0] == '/' && !tmp[1]) ? tmp : kDefaultCommand);
}

void cfg() {
    DWORD n = g_module ? GetModuleFileNameA(g_module, g_ini, MAX_PATH) : 0;
    if (!n || n >= MAX_PATH) cpy(g_ini, sizeof(g_ini), kIniName);
    else {
        while (n && g_ini[n - 1] != '\\' && g_ini[n - 1] != '/') --n;
        cpy(g_ini + n, sizeof(g_ini) - n, kIniName);
    }

    char raw[64] = {};
    norm(g_cmd, sizeof(g_cmd), kDefaultCommand);
    GetPrivateProfileStringA(kIniSection, kIniKeyCommand, kDefaultCommand, raw, sizeof(raw), g_ini);
    norm(g_cmd, sizeof(g_cmd), raw);
    g_render = GetPrivateProfileIntA(kIniSection, kIniKeyEnabled, 1, g_ini) != 0;
    WritePrivateProfileStringA(kIniSection, kIniKeyCommand, g_cmd, g_ini);
    WritePrivateProfileStringA(kIniSection, kIniKeyEnabled, g_render ? "1" : "0", g_ini);
}

bool patch(void* dst, const void* src, std::size_t size) {
    DWORD old = 0;
    if (!VirtualProtect(dst, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(dst, src, size);
    FlushInstructionCache(GetCurrentProcess(), dst, size);
    VirtualProtect(dst, size, old, &old);
    return true;
}

bool jmp(std::uint8_t* out, std::uintptr_t from, const void* to, std::size_t size) {
    memset(out, 0x90, size);
    out[0] = 0xE9;
    const auto delta = reinterpret_cast<std::intptr_t>(to) - static_cast<std::intptr_t>(from + 5);
    const auto rel = static_cast<std::int32_t>(delta);
    if (static_cast<std::intptr_t>(rel) != delta) return false;
    memcpy(out + 1, &rel, sizeof(rel));
    return true;
}

template <typename T>
bool hook(std::uint32_t off, const void* detour, std::size_t size, T& original) {
    auto* src = reinterpret_cast<std::uint8_t*>(g_base + off);
    auto* gate = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    std::uint8_t out[16];
    if (!gate || size > sizeof(out)) return false;
    memcpy(gate, src, size);
    if (!jmp(gate + size, reinterpret_cast<std::uintptr_t>(gate + size), src + size, 5)) { VirtualFree(gate, 0, MEM_RELEASE); return false; }
    if (!jmp(out, reinterpret_cast<std::uintptr_t>(src), detour, size)) { VirtualFree(gate, 0, MEM_RELEASE); return false; }
    if (!patch(src, out, size)) { VirtualFree(gate, 0, MEM_RELEASE); return false; }
    original = reinterpret_cast<T>(gate);
    return true;
}

const Ver* detect(HMODULE samp) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(samp);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const std::uint8_t*>(samp) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const DWORD ep = nt->OptionalHeader.AddressOfEntryPoint;
    for (const auto& ver : kVer) if (ver.ep == ep) return &ver;
    return nullptr;
}

Vec3 cam() {
    const auto* camera = reinterpret_cast<const Camera*>(kCamera);
    return camera->placeable.matrix ? camera->placeable.matrix->pos : camera->placeable.simple.pos;
}

float dist(const Vec3& a, const Vec3& b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return fsqrt(x * x + y * y + z * z);
}

void label(char* out, std::size_t size, const char* name, unsigned int id) {
    if (!out || !size) return;
    char* p = out;
    char* e = out + size - 1;
    while (name && *name && p < e) *p++ = *name++;
    if (p < e) *p++ = ' ';
    if (p < e) *p++ = '(';
    char rev[10];
    unsigned int n = 0;
    do { rev[n++] = static_cast<char>('0' + (id % 10)); id /= 10; } while (id && n < sizeof(rev));
    while (n && p < e) *p++ = rev[--n];
    if (p < e) *p++ = ')';
    *p = 0;
}

bool ctx(Ctx& c) {
    if (!g_ver || !g_base) return false;
    auto* net = *reinterpret_cast<void**>(g_base + g_ver->net);
    auto* tags = *reinterpret_cast<void**>(g_base + g_ver->tags);
    if (!net || !tags) return false;

    auto* pool = call<void*>(net, g_ver->get_pool);
    auto* local = pool ? call<void*>(pool, g_ver->get_local) : nullptr;
    if (!pool || !local) return false;

    c.id = *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::uint8_t*>(pool) + g_ver->local_id);
    c.name = call<const char*>(pool, g_ver->get_name, static_cast<unsigned int>(c.id));
    if (!c.name || !*c.name) return false;

    auto* ped = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(local) + g_ver->local_ped);
    if (!ped || !call<BOOL>(ped, g_ver->ped_on_screen)) return false;

    c.health = call<float>(ped, g_ver->get_health);
    if (!(c.health > 0.0f)) return false;

    c.tags = tags;
    call<void>(ped, g_ver->get_bone, kHeadBone, &c.head);
    c.color = call<D3DCOLOR>(local, g_ver->get_color);
    if (!c.color) c.color = kWhite;
    c.armour = call<float>(ped, g_ver->get_armour);
    if (c.armour < 0.0f) c.armour = 0.0f;
    c.distance = dist(c.head, cam());
    if (c.distance != c.distance) c.distance = 0.0f;
    return true;
}

void tag() {
    Ctx c;
    if (!ctx(c)) return;
    char text[320];
    label(text, sizeof(text), c.name, c.id);
    call<void>(c.tags, g_ver->begin_label);
    call<void>(c.tags, g_ver->draw_label, &c.head, text, c.color, c.distance, false, 0);
    call<void>(c.tags, g_ver->end_label);
}

void bar() {
    Ctx c;
    if (!ctx(c)) return;
    call<void>(c.tags, g_ver->begin_bar);
    call<void>(c.tags, g_ver->draw_bar, &c.head, c.health, c.armour, c.distance);
    call<void>(c.tags, g_ver->end_bar);
}

bool same(const char* text) {
    const char* cmd = g_cmd;
    if (!text || !*cmd) return false;
    while (sp(*text)) ++text;
    while (*text && *cmd && lo(*text) == lo(*cmd)) { ++text; ++cmd; }
    while (sp(*text)) ++text;
    return !*cmd && !*text;
}

void __cdecl hook_label() { if (g_old_label) g_old_label(); if (g_render) tag(); }
void __cdecl hook_bar() { if (g_old_bar) g_old_bar(); if (g_render) bar(); }

void __fastcall hook_send(std::uintptr_t self, void*, const char* text) {
    if (same(text)) { g_render = !g_render; return; }
    if (g_old_send) g_old_send(self, text);
}

DWORD WINAPI init(void*) {
    static const std::uint8_t sig[] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x6A, 0xFF, 0x68 };

    const auto* state = reinterpret_cast<volatile DWORD*>(kGtaLoadState);
    while (*state < 9) Sleep(10);
    cfg();

    HMODULE samp = nullptr;
    while ((samp = GetModuleHandleA("samp.dll")) == nullptr) Sleep(100);

    g_base = reinterpret_cast<std::uintptr_t>(samp);
    g_ver = detect(samp);
    if (!g_ver) return 0;

    const auto* send = reinterpret_cast<const std::uint8_t*>(g_base + g_ver->send_command);
    for (std::size_t i = 0; i != sizeof(sig); ++i) if (send[i] != sig[i]) return 0;

    if (!hook(g_ver->label_loop, reinterpret_cast<const void*>(&hook_label), kDrawHookSize, g_old_label)) return 0;
    if (!hook(g_ver->bar_loop, reinterpret_cast<const void*>(&hook_bar), kDrawHookSize, g_old_bar)) return 0;
    if (!hook(g_ver->send_command, reinterpret_cast<const void*>(&hook_send), kSendHookSize, g_old_send)) return 0;
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (HANDLE thread = CreateThread(nullptr, 0, &init, nullptr, 0, nullptr)) CloseHandle(thread);
    }
    return TRUE;
}
