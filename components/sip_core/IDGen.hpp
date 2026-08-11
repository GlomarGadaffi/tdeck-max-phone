#ifndef IDGEN_HPP
#define IDGEN_HPP

#include <string>
#include <cstdint>

// NOTE (PoC fix): the upstream pocket-dial IDGen used `thread_local std::mt19937`.
// On ESP-IDF the ~2.5 KB Mersenne-Twister TLS block is copied into EVERY task's
// stack, which OVERFLOWS the small system tasks (idle ~1.5 KB, IPC ~1.3 KB) the
// instant they are created -> heap/stack corruption -> the scheduler then derefs
// a 0xa5a5a5a5 stack-fill pointer and panics at boot. We use the hardware RNG
// instead: thread-safe, stateless, no TLS.
#if defined(ESP_PLATFORM)
#include "esp_random.h"
#else
#include <random>
#endif

class IDGen
{
public:
    IDGen() = delete;

    static std::string GenerateID(int len)
    {
        std::string id;
        id.reserve(len);
        constexpr int N = static_cast<int>(sizeof(alphanum) - 1);  // exclude NUL
        for (int i = 0; i < len; ++i)
        {
#if defined(ESP_PLATFORM)
            uint32_t r = esp_random();                 // HW RNG, no TLS, thread-safe
#else
            thread_local std::mt19937 rng{std::random_device{}()};
            uint32_t r = rng();
#endif
            id += alphanum[r % N];
        }
        return id;
    }

private:
    static constexpr char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
};

#endif
