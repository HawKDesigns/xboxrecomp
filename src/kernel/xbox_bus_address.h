#ifndef XBOX_BUS_ADDRESS_H
#define XBOX_BUS_ADDRESS_H
#include <stdint.h>

/* This host stores low Xbox RAM and the contiguous window separately.
 * Their equal offsets cannot denote the same bus page. Reserve bit 26 for
 * ordinary RAM returned by MmGetPhysicalAddress; the bus masters strip it
 * before accessing low RAM. Contiguous offsets retain their existing values.
 * These addresses are host-runtime tokens, not retail hardware addresses. */
#define XBOX_BUS_RAM_TAG 0x04000000u
#define XBOX_BUS_RAM_SIZE 0x04000000u

static inline int xbox_bus_is_ram(uint32_t address)
{
    return address >= XBOX_BUS_RAM_TAG && address < 2u * XBOX_BUS_RAM_TAG;
}

static inline uint32_t xbox_bus_ram_address(uint32_t address)
{
    return address & (XBOX_BUS_RAM_SIZE - 1u);
}

static inline uint32_t xbox_bus_encode(uint32_t va)
{
    if (va >= 0x80000000u && va < 0x84000000u) return va - 0x80000000u;
    if (va && va < XBOX_BUS_RAM_SIZE) return va | XBOX_BUS_RAM_TAG;
    return va;
}
#endif
