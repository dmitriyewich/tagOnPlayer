#pragma once

#include <cstddef>
#include <cstdint>

#include "samp_version.h"

namespace chat_bubble {

constexpr int kDefaultLifeMs = 6000;
constexpr int kDefaultOverlayLinePx = 257;
constexpr unsigned int kOverlayMaxRules = 16;
constexpr std::size_t kOverlayCommandMax = 64;
constexpr std::size_t kOverlayTemplateMax = 128;
constexpr std::size_t kOverlaySplitMax = 16;
constexpr std::size_t kLineMax = 256;

struct OverlayCommandRule {
    char cmd[kOverlayCommandMax]{};
    D3DCOLOR color = 0xFFFFFFFFu;
    bool forwardToServer = true;
    char bubbleTemplate[kOverlayTemplateMax]{};
    char bubbleSplit[kOverlaySplitMax]{};
    D3DCOLOR accentColor = 0xFFFFFFFFu;
    bool accentColorValid = false;
};

struct LocalPlayerContext {
    void* localPlayer = nullptr;
    std::uint16_t id = 0;
    float distanceToCamera = 0.0f;
};

using BuildLocalContextFn = bool(__cdecl*)(LocalPlayerContext& context);

struct RuntimeConfig {
    std::uintptr_t sampBase = 0;
    const SampVersionInfo* version = nullptr;
    bool mirrorOwnChatBubble = false;
    int lifeMs = kDefaultLifeMs;
    int overlayLinePx = kDefaultOverlayLinePx;
    /** Цвет текста зеркала исходящего чата без ведущего `/` (не оверлей-команды). */
    D3DCOLOR ownChatBubbleColor = 0xFFFFFFFFu;
    BuildLocalContextFn buildLocalContext = nullptr;
};

void Configure(const RuntimeConfig& config);
void Shutdown();

bool ParseColorString(const char* input, D3DCOLOR* outColor);
void BuildOverlayText(char* out, std::size_t cap, const OverlayCommandRule& rule, const char* rest);

bool WantLocalDrawPatches(bool mirrorOwnChatBubble, unsigned int overlayRuleCount);
bool InstallLocalDrawPatches();
bool InstallLocalPlayerChatHook();

void PushLocalPlayerBubble(
    const LocalPlayerContext& context,
    const char* text,
    D3DCOLOR color,
    int lifeMs);

}  // namespace chat_bubble
