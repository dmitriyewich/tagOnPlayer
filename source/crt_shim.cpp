#include <cstddef>
#include <cstdint>

extern "C" void* __cdecl memset(void* destination, int value, std::size_t size);
extern "C" void* __cdecl memcpy(void* destination, const void* source, std::size_t size);

#pragma function(memset)
#pragma function(memcpy)

extern "C" void* __cdecl memset(void* destination, int value, std::size_t size) {
    auto* destinationBytes = static_cast<std::uint8_t*>(destination);
    const auto fillValue = static_cast<std::uint8_t>(value);

    for (std::size_t index = 0; index < size; ++index) {
        destinationBytes[index] = fillValue;
    }

    return destination;
}

extern "C" void* __cdecl memcpy(void* destination, const void* source, std::size_t size) {
    auto* destinationBytes = static_cast<std::uint8_t*>(destination);
    const auto* sourceBytes = static_cast<const std::uint8_t*>(source);

    for (std::size_t index = 0; index < size; ++index) {
        destinationBytes[index] = sourceBytes[index];
    }

    return destination;
}
