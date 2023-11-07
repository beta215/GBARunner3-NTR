#include "common.h"
#include <string.h>
#include <libtwl/gfx/gfxStatus.h>
#include "Emulator/IoRegisters.h"
#include "VirtualMachine/VMDtcm.h"
#include "MemCopy.h"
#include "GbaIoRegOffsets.h"
#include "SdCache/SdCache.h"
#include "MemoryEmulator/HiCodeCacheMapping.h"
#include "Peripherals/Sound/GbaSound9.h"
#include "VirtualMachine/VMNestedIrq.h"
#include "DmaTransfer.h"

DTCM_DATA dma_state_t dma_state;

void dma_immTransfer16(u32 src, u32 dst, u32 count, int srcStep, int dstStep);
void dma_immTransfer32(u32 src, u32 dst, u32 count, int srcStep, int dstStep);

extern void dma_immTransferSafe16(u32 src, u32 dst, u32 count, int srcStep, int dstStep);
extern void dma_immTransferSafe16BadSrc(u32 dst, u32 count, int dstStep);
extern void dma_immTransferSafe32(u32 src, u32 dst, u32 count, int srcStep, int dstStep);
extern void dma_immTransferSafe32BadSrc(u32 dst, u32 count, int dstStep);

extern u32 dma_transferRegister;
extern s8 dma_stepTable[4];

static inline void updateHBlankIrqForChannelStop(void)
{
    if (!(dma_state.dmaFlags & DMA_FLAG_HBLANK_MASK))
    {
        vm_forcedIrqMask &= ~(1 << 1);
        u32 gbaDispStat = *(u16*)&emu_ioRegisters[4];
        if (!(gbaDispStat & (1 << 4)))
        {
            gfx_setHBlankIrqEnabled(false);
        }
    }
}

ITCM_CODE static void dmaDummy(void)
{
}

static inline int getSrcStep(u32 control)
{
    return dma_stepTable[(control >> 7) & 3];
}

static inline int getDstStep(u32 control)
{
    return dma_stepTable[(control >> 5) & 3];
}

ITCM_CODE static void dmaTransfer(int channel, bool dma32)
{
    void* dmaIoBase = &emu_ioRegisters[0xB0 + channel * 0xC];
    u32 count = *(u16*)((u32)dmaIoBase + 8);
    if (channel == 3 && count == 0)
        count = 0x10000;
    u32 control = *(u16*)((u32)dmaIoBase + 0xA);
    int srcStep = getSrcStep(control);
    u32 src = dma_state.channels[channel].curSrc;
    dma_state.channels[channel].curSrc += srcStep * (count << (dma32 ? 2 : 1));
    int dstStep = getDstStep(control);
    u32 dst;
    if (((control >> 5) & 3) != 3)
    {
        dst = dma_state.channels[channel].curDst;
        dma_state.channels[channel].curDst += dstStep * (count << (dma32 ? 2 : 1));
    }
    else
    {
        // reload
        dst = *(u32*)((u32)dmaIoBase + 4);
        dma_state.channels[channel].curDst = dst;
    }
    if (control & (1 << 14))
    {
        vm_emulatedIfImeIe |= 1 << (8 + channel);
    }
    if (!(control & (1 << 9)))
    {
        dma_state.channels[channel].dmaFunction = (void*)dmaDummy;
        dma_state.dmaFlags &= ~DMA_FLAG_HBLANK(channel);
        updateHBlankIrqForChannelStop();
        *(u16*)((u32)dmaIoBase + 0xA) &= ~0x8000;
    }
    if (dma32)
        dma_immTransfer32(src, dst, count, srcStep, dstStep);
    else
        dma_immTransfer16(src, dst, count, srcStep, dstStep);
}

ITCM_CODE static void dma0Transfer16(void)
{
    dmaTransfer(0, false);
}

ITCM_CODE static void dma1Transfer16(void)
{
    dmaTransfer(1, false);
}

ITCM_CODE static void dma2Transfer16(void)
{
    dmaTransfer(2, false);
}

ITCM_CODE static void dma3Transfer16(void)
{
    dmaTransfer(3, false);
}

ITCM_CODE static void dma0Transfer32(void)
{
    dmaTransfer(0, true);
}

ITCM_CODE static void dma1Transfer32(void)
{
    dmaTransfer(1, true);
}

ITCM_CODE static void dma2Transfer32(void)
{
    dmaTransfer(2, true);
}

ITCM_CODE static void dma3Transfer32(void)
{
    dmaTransfer(3, true);
}

void dma_init(void)
{
    memset(&dma_state, 0, sizeof(dma_state));
    dma_state.channels[0].dmaFunction = (void*)dmaDummy;
    dma_state.channels[1].dmaFunction = (void*)dmaDummy;
    dma_state.channels[2].dmaFunction = (void*)dmaDummy;
    dma_state.channels[3].dmaFunction = (void*)dmaDummy;
}

ITCM_CODE static u32 translateAddress(u32 address)
{
    switch (address >> 24)
    {
        case 2:
            address &= ~0x00FC0000;
            break;
        case 3:
            address &= ~0x00FF8000;
            break;
        case 5:
        case 7:
            address &= ~0x00FFFC00;
            break;
        case 6:
        {
            address &= ~0x00FE0000;
            if (!(address & 0x10000))
            {
                break;
            }

            address &= ~0x8000;
            if (address & 0x4000)
            {
                address += 0x003F0000;
            }
            else
            {
                u32 dispCnt = emu_ioRegisters[GBA_REG_OFFS_DISPCNT];
                if ((dispCnt & 7) < 3)
                {
                    address += 0x003F0000;
                }
            }
            break;
        }
    }
    return address;
}

ITCM_CODE static u32 dmaIoBaseToChannel(const void* dmaIoBase)
{
    u32 regOffset = (u32)dmaIoBase - (u32)emu_ioRegisters;
    return (regOffset - GBA_REG_OFFS_DMA0SAD) / (GBA_REG_OFFS_DMA1SAD - GBA_REG_OFFS_DMA0SAD);
}

static void dma_immTransfer16RomSrc(u32 src, u32 dst, u32 byteCount)
{
    u8* dstPtr = (u8*)dst;
    do
    {
        const void* cacheBlock = sdc_getRomBlock(src);
        u32 offset = src & SDC_BLOCK_MASK;
        u32 remainingInBlock = SDC_BLOCK_SIZE - offset;
        if (remainingInBlock > byteCount)
            remainingInBlock = byteCount;
        mem_copy16((const u8*)cacheBlock + offset, dstPtr, remainingInBlock);
        src += remainingInBlock;
        dstPtr += remainingInBlock;
        byteCount -= remainingInBlock;
    } while (byteCount > 0);
}

static void dma_immTransfer32RomSrc(u32 src, u32 dst, u32 byteCount)
{
    u8* dstPtr = (u8*)dst;
    do
    {
        const void* cacheBlock = sdc_getRomBlock(src);
        u32 offset = src & SDC_BLOCK_MASK;
        u32 remainingInBlock = SDC_BLOCK_SIZE - offset;
        if (remainingInBlock > byteCount)
            remainingInBlock = byteCount;
        mem_copy32((const u8*)cacheBlock + offset, dstPtr, remainingInBlock);
        src += remainingInBlock;
        dstPtr += remainingInBlock;
        byteCount -= remainingInBlock;
    } while (byteCount > 0);
}

static inline bool fastDmaSourceAllowed(u32 srcRegion)
{
    return 0b0011111111001100 & (1 << srcRegion);
}

static inline bool fastDmaDestinationAllowed(u32 dstRegion)
{
    return 0b0000000011001100 & (1 << dstRegion);
}

ITCM_CODE void dma_immTransfer16(u32 src, u32 dst, u32 count, int srcStep, int dstStep)
{
    src &= ~1;
    dst &= ~1;
    if (src < 0x02000000)
    {
        dma_immTransferSafe16BadSrc(dst, count, dstStep);
        return;
    }
    u32 srcRegion = src >> 24;
    u32 srcEnd = src + (count << 1);
    u32 srcEndRegion = srcEnd >> 24;
    u32 dstRegion = dst >> 24;
    int difference = dst - src;
    if (difference < 0)
        difference = -difference;
    u32 dsDst = translateAddress(dst);
    u32 dsDstEnd = translateAddress(dst + ((count - 1) << 1) * dstStep);
    if (srcStep <= 0 || dsDstEnd != dsDst + ((count - 1) << 1) ||
        !fastDmaSourceAllowed(srcRegion) || !fastDmaDestinationAllowed(dstRegion) ||
        srcRegion != srcEndRegion || difference < 32)
    {
        dma_immTransferSafe16(src, dst, count, srcStep, dstStep);
        return;
    }
    if (src >= 0x08000000)
    {
        dma_immTransfer16RomSrc(src, dsDst, count << 1);
    }
    else
    {
        src = translateAddress(src);
        mem_copy16((void*)src, (void*)dsDst, count << 1);
    }
    u32 last = ((u16*)dsDst)[count - 1];
    dma_transferRegister = last | (last << 16);
}

ITCM_CODE void dma_immTransfer32(u32 src, u32 dst, u32 count, int srcStep, int dstStep)
{
    src &= ~3;
    dst &= ~3;
    if (src < 0x02000000)
    {
        dma_immTransferSafe32BadSrc(dst, count, dstStep);
        return;
    }
    u32 srcRegion = src >> 24;
    u32 srcEnd = src + (count << 2);
    u32 srcEndRegion = srcEnd >> 24;
    u32 dstRegion = dst >> 24;
    int difference = dst - src;
    if (difference < 0)
        difference = -difference;
    u32 dsDst = translateAddress(dst);
    u32 dsDstEnd = translateAddress(dst + ((count - 1) << 2) * dstStep);
    if (srcStep <= 0 || dsDstEnd != dsDst + ((count - 1) << 2) ||
        !fastDmaSourceAllowed(srcRegion) || !fastDmaDestinationAllowed(dstRegion) ||
        srcRegion != srcEndRegion || difference < 32)
    {
        dma_immTransferSafe32(src, dst, count, srcStep, dstStep);
        return;
    }
    if (src >= 0x08000000)
    {
        dma_immTransfer32RomSrc(src, dsDst, count << 2);
    }
    else
    {
        src = translateAddress(src);
        mem_copy32((void*)src, (void*)dsDst, count << 2);
    }
    dma_transferRegister = ((u32*)dsDst)[count - 1];
}

ITCM_CODE static void dmaStop(int channel, void* dmaIoBase)
{
    dma_state.dmaFlags &= ~DMA_FLAG_HBLANK(channel);
    dma_state.dmaFlags &= ~DMA_FLAG_SOUND(channel);
    dma_state.channels[channel].dmaFunction = (void*)dmaDummy;
    updateHBlankIrqForChannelStop();
    if (!(dma_state.dmaFlags & DMA_FLAG_SOUND_MASK))
    {
        vm_forcedIrqMask &= ~(1 << 16); // arm7 irq
    }
}

ITCM_CODE static void dmaStartHBlank(int channel, void* dmaIoBase, u32 value)
{
    u32 src = *(u32*)dmaIoBase;
    if ((src >= 0x02200000 && src < 0x02400000) || src >= 0x08000000)
        return;
    *(u16*)(dmaIoBase + 0xA) = value;
    dma_state.dmaFlags |= DMA_FLAG_HBLANK(channel);
    vm_forcedIrqMask |= 1 << 1; // hblank irq
    gfx_setHBlankIrqEnabled(true);
    dma_state.channels[channel].curSrc = src;
    dma_state.channels[channel].curDst = *(u32*)(dmaIoBase + 4);
    if (value & (1 << 10))
    {
        switch (channel)
        {
            case 0:
                dma_state.channels[0].dmaFunction = (void*)dma0Transfer32;
                break;
            case 1:
                dma_state.channels[1].dmaFunction = (void*)dma1Transfer32;
                break;
            case 2:
                dma_state.channels[2].dmaFunction = (void*)dma2Transfer32;
                break;
            case 3:
                dma_state.channels[3].dmaFunction = (void*)dma3Transfer32;
                break;
        }
    }
    else
    {
        switch (channel)
        {
            case 0:
                dma_state.channels[0].dmaFunction = (void*)dma0Transfer16;
                break;
            case 1:
                dma_state.channels[1].dmaFunction = (void*)dma1Transfer16;
                break;
            case 2:
                dma_state.channels[2].dmaFunction = (void*)dma2Transfer16;
                break;
            case 3:
                dma_state.channels[3].dmaFunction = (void*)dma3Transfer16;
                break;
        }        
    }
}

[[gnu::noinline]]
ITCM_CODE static void dmaSound(u32 channel)
{
    dc_drainWriteBuffer();
    dc_invalidateRange(&gGbaSoundShared.directChannels[channel - 1].dmaRequest, 1);
    if (!gGbaSoundShared.directChannels[channel - 1].dmaRequest)
        return;

    void* dmaIoBase = &emu_ioRegisters[0xB0 + channel * 0xC];
    u32 control = *(u16*)((u32)dmaIoBase + 0xA);
    u32 src = dma_state.channels[channel].curSrc;
    int srcStep = getSrcStep(control);
    if (src >= 0x02000000)
    {
        dma_state.channels[channel].curSrc += srcStep * 16;
        u32 dst = dma_state.channels[channel].curDst;
        dma_immTransferSafe32(src, dst, 4, srcStep, 0);
    }

    gGbaSoundShared.directChannels[channel - 1].dmaRequest = false;
    dc_drainWriteBuffer();

    if (control & (1 << 14))
    {
        vm_emulatedIfImeIe |= 1 << (8 + channel);
    }
    if (!(control & (1 << 9)))
    {
        dma_state.dmaFlags &= ~DMA_FLAG_SOUND(channel);
        if (!(dma_state.dmaFlags & DMA_FLAG_SOUND_MASK))
        {
            vm_forcedIrqMask &= ~(1 << 16); // arm7 irq
        }
        *(u16*)((u32)dmaIoBase + 0xA) &= ~0x8000;
    }
}

ITCM_CODE void dma_dmaSound1(void)
{
    dmaSound(1);
}

ITCM_CODE void dma_dmaSound2(void)
{
    dmaSound(2);
}

ITCM_CODE static void dmaStartSound(int channel, void* dmaIoBase, u32 value)
{
    *(u16*)(dmaIoBase + 0xA) = value;
    dma_state.dmaFlags |= DMA_FLAG_SOUND(channel);
    vm_forcedIrqMask |= 1 << 16; // arm7 irq
    dma_state.channels[channel].curSrc = *(u32*)dmaIoBase;
    dma_state.channels[channel].curDst = *(u32*)(dmaIoBase + 4);
    gGbaSoundShared.directChannels[channel - 1].dmaRequest = false;
    dc_drainWriteBuffer();
}

ITCM_CODE static void dmaStartSpecial(int channel, void* dmaIoBase, u32 value)
{
    u32 src = *(u32*)dmaIoBase;
    if ((src >= 0x02200000 && src < 0x02400000) || src >= 0x08000000)
        return;
    switch (channel)
    {
        case 0:
            break;
        case 1:
        case 2:
            dmaStartSound(channel, dmaIoBase, value);
            break;
        case 3:
            break;
    }
}

ITCM_CODE static void dmaStart(int channel, void* dmaIoBase, u32 value)
{
    *(u16*)(dmaIoBase + 0xA) = value & ~0x8000;
    if (value & (1 << 11))
        return; // rom dreq
    switch ((value >> 12) & 3)
    {
        case 0:
            break;
        case 1: // vblank
            return;
        case 2: // hblank
            dmaStartHBlank(channel, dmaIoBase, value);
            return;
        case 3: // special
            dmaStartSpecial(channel, dmaIoBase, value);
            return;
    }
    u32 count = *(u16*)(dmaIoBase + 8);
    if (count == 0)
        count = 0x10000;
    u32 src = *(u32*)dmaIoBase;
    if (src >= 0x02200000 && src < 0x02400000)
    {
        // assume this is a pc-relative rom address
        src = src + 0x08000000 - 0x02200000;
    }
    u32 dst = *(u32*)(dmaIoBase + 4);
    if (value & (1 << 14))
    {
        vm_emulatedIfImeIe |= 1 << (8 + channel);
    }
    if (channel == 3)
    {
        vm_enableNestedIrqs();
    }
    int srcStep = getSrcStep(value);
    int dstStep = getDstStep(value);
    if (value & (1 << 10))
        dma_immTransfer32(src, dst, count, srcStep, dstStep);
    else
        dma_immTransfer16(src, dst, count, srcStep, dstStep);
    if (channel == 3)
    {
        vm_disableNestedIrqs();
    }
}

ITCM_CODE void dma_CntHStore16(void* dmaIoBase, u32 value)
{
#ifdef GBAR3_HICODE_CACHE_MAPPING
    hic_unmapRomBlock();
#endif
    ic_invalidateAll();
    int channel = dmaIoBaseToChannel(dmaIoBase);
    u32 oldCnt = *(u16*)(dmaIoBase + 0xA);
    if (!((oldCnt ^ value) & 0x8000))
    {
        // no change in start/stop
        *(u16*)(dmaIoBase + 0xA) = value;
    }
    else if (!(value & 0x8000))
    {
        // dma was stopped
        *(u16*)(dmaIoBase + 0xA) = value;
        dmaStop(channel, dmaIoBase);
    }
    else
    {
        // dma was started
        dmaStart(channel, dmaIoBase, value);
    }
}
