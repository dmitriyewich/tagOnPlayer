#pragma once

#include <cstdint>

using D3DCOLOR = std::uint32_t;

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

constexpr std::uint8_t kChatBubblePoolTrampNone = 0;
// R1: test ecx + near-je, 8-byte patch.
constexpr std::uint8_t kChatBubblePoolTrampEcxJe8 = 1;
// R3/R3-1: test edx + near-je, 8-byte patch.
constexpr std::uint8_t kChatBubblePoolTrampEdxJe8 = 2;
// R2/R4/R4-2/R5-1/DL-R1: test ecx + lea eax,[edi+edx*4] + near-je, 11-byte patch.
constexpr std::uint8_t kChatBubblePoolTrampEcxLeaJe11 = 3;

struct SampVersionInfo {
    std::uint32_t entryPointRva;
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
    // Patch point that redirects the null-pool/slot branch in CChatBubble::Draw.
    std::uint32_t chatBubblePoolNullTrampPatchRva;
    std::uint32_t chatBubblePoolNullTrampResumeRva;
    // Continuation after local-player ped substitution.
    std::uint32_t chatBubblePoolNullTrampResumeMidRva;
    std::uint32_t chatBubblePoolNullTrampSkipRva;
    // Optional guarded remote-chain dereferences; 0 disables.
    std::uint32_t chatBubbleRemoteChainLoadGuardRva;
    std::uint32_t chatBubbleRemoteChainPedGuardRva;
    std::uint8_t chatBubblePoolNullTrampKind;
    // Аддитивная глобаль getChatBubbleHeight: Z += distanceToCamera * (A + height).
    // Временный сдвиг этой константы поднимает баббл на distance*Δ — рычаг стопки вверх.
    std::uint32_t chatBubbleHeightGlobalRva;
    std::uint16_t sampBubbleDrawTextLinePx;
};
