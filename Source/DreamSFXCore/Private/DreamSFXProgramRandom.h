#pragma once
#include "CoreTypes.h"

namespace DreamSFX::V2
{
    enum class EProgramRandom : uint8 { Lcg32, Mulberry32 };

    inline uint32 DrawRandomBits(uint32& State, EProgramRandom Algorithm)
    {
        if (Algorithm == EProgramRandom::Mulberry32)
        {
            State += 0x6D2B79F5u;
            uint32 V = (State ^ (State >> 15)) * (State | 1u);
            V ^= V + (V ^ (V >> 7)) * (V | 61u);
            return V ^ (V >> 14);
        }
        State = State * 1664525u + 1013904223u;
        return State;
    }
}
