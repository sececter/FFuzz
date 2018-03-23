#include <ntddk.h>
#include <ntimage.h>
#include <Aux_klib.h>
#include "s2e.h"
#include "hook.h"
#include "monitor.h"

#define WIN7_CHECKSUM64 0x55ce0c
#define WIN8_CHECKSUM64 0x6aa6c8
#define WIN8_CHECKSUM32 0x550165
#define WINXP_SP3_NTOSKRNL_FREE 0x2247c2
#define WINXP_SP3_NTOSKRNL_CHECKED 0x36b94a

static REGISTER_KERNEL_STRUCTS Win7Handler64;
static REGISTER_KERNEL_STRUCTS Win8Handler64;
static REGISTER_KERNEL_STRUCTS Win8Handler32;
static REGISTER_KERNEL_STRUCTS WindowsXPSP3FreeHandler;
static REGISTER_KERNEL_STRUCTS WindowsXPSP3CheckedHandler;

REGISTER_KERNEL_STRUCTS_HANDLERS g_KernelStructHandlers [] = {
#if defined(_AMD64_)
    {WIN8_CHECKSUM64, &Win8Handler64},
    {WIN7_CHECKSUM64, &Win7Handler64},
#endif
#if defined(_X86_)
    {WIN8_CHECKSUM32, &Win8Handler32},
#endif

    {WINXP_SP3_NTOSKRNL_FREE, &WindowsXPSP3FreeHandler},
    {WINXP_SP3_NTOSKRNL_CHECKED, &WindowsXPSP3CheckedHandler},
};


#if defined(_AMD64_)
static VOID Win7Handler64(UINT_PTR KernelLoadBase, UINT_PTR KernelNativeBase)
{
    KPCR *pKpcr;
    S2E_WINMON2_COMMAND Command;

    S2EMessage("Registering Windows 8 data structures (64-bits)\n");

    MonitorInitCommon(&Command);
    Command.Structs.KernelLoadBase = KernelLoadBase;
    Command.Structs.KernelNativeBase = KernelNativeBase;
    Command.Structs.LoadDriverPc = 0x14046557F;
    Command.Structs.UnloadDriverPc = 0x1404F68B0; //IopDeleteDriver;

    //KeBugCheckEx saves CPU registers in the right places
    //and then calls KeBugCheck2. It is easier to hook KeBugCheck2.
    Command.Structs.KeBugCheckEx = 0x140168460; //KeBugCheck2

    pKpcr = (KPCR*) __readmsr(IA32_GS_BASE);
    Command.Structs.KPCR = (UINT_PTR) pKpcr;

    Command.Structs.KPRCB = (UINT_PTR) pKpcr->CurrentPrcb;  //Command.Structs.KPCR + 0x180;

    //The KdVersionBlock pointer stored in KPCR is NULL.
    //Must compute it in some other way.
    //UPDATE: Don't care. Crash dump header is generated by Windows.
    Command.Structs.KdVersionBlock = 0; //Command.Structs.KPCR + 0x108;

    Command.Structs.KdDebuggerDataBlock = 0x1401F10A0;
    g_LfiKernelStructs.KdCopyDataBlock = 0x140108C00;
    g_LfiKernelStructs.KdpDataBlockEncoded = 0x1402194B2;

    /* Determine these by looking into KeBugCheckEx */
    g_LfiKernelStructs.PRCBProcessorStateOffset = (PVOID)(Command.Structs.KPRCB + 0x40);

    g_LfiKernelStructs.PsActiveProcessHead = (PLIST_ENTRY)(0x140227B90 - KernelNativeBase + KernelLoadBase);
    g_LfiKernelStructs.EProcessActiveProcessLinkOffset = 0x188;
    g_LfiKernelStructs.EProcessThreadListHeadOffset = 0x308;
    g_LfiKernelStructs.EThreadThreadListEntry = 0x420;
    g_LfiKernelStructs.ObpCreateHandle = 0x140378D40 - KernelNativeBase + KernelLoadBase;

    g_KernelStructs = Command.Structs;

    Command.Structs.EThreadSegment = R_GS;
    Command.Structs.EThreadSegmentOffset = 0x188;
    Command.Structs.EThreadStackBaseOffset = 0x278;
    Command.Structs.EThreadStackLimitOffset = 0x30;

    Command.Structs.KiRetireDpcCallSite = 0x14008BB15 - KernelNativeBase + KernelLoadBase;

    Command.Structs.DPCStackBasePtr = Command.Structs.KPRCB + 0x21c0;
    Command.Structs.DPCStackSize = 24 * 1024;

    Command.Structs.PsLoadedModuleList = (UINT_PTR)(0x140245E90 - KernelNativeBase + KernelLoadBase);

    Command.Structs.PerfLogImageUnload = (UINT_PTR)(0x14005FD10 - KernelNativeBase + KernelLoadBase);
    S2EInvokePlugin("WindowsMonitor2", &Command, sizeof(Command));
}

static VOID Win8Handler64(UINT_PTR KernelLoadBase, UINT_PTR KernelNativeBase)
{
    KPCR *pKpcr;
    S2E_WINMON2_COMMAND Command;

    S2EMessage("Registering Windows 8 data structures (64-bits)\n");

    MonitorInitCommon(&Command);
    Command.Structs.KernelLoadBase = KernelLoadBase;
    Command.Structs.KernelNativeBase = KernelNativeBase;
    Command.Structs.LoadDriverPc = 0x140492769;
    Command.Structs.UnloadDriverPc = 0x14048dd40; //IopDeleteDriver;

    //KeBugCheckEx saves CPU registers in the right places
    //and then calls KeBugCheck2. It is easier to hook KeBugCheck2.
    Command.Structs.KeBugCheckEx = 0x0140174FA4; //KeBugCheck2

    pKpcr = (KPCR*) __readmsr(IA32_GS_BASE);
    Command.Structs.KPCR = (UINT_PTR) pKpcr;

    Command.Structs.KPRCB = (UINT_PTR) pKpcr->CurrentPrcb;  //Command.Structs.KPCR + 0x180;

    //The KdVersionBlock pointer stored in KPCR is NULL.
    //Must compute it in some other way.
    //UPDATE: Don't care. Crash dump header is generated by Windows.
    Command.Structs.KdVersionBlock = 0; //Command.Structs.KPCR + 0x108;

    Command.Structs.KdDebuggerDataBlock = 0x140273A90;
    g_LfiKernelStructs.KdCopyDataBlock = 0x140171EA0;
    g_LfiKernelStructs.KdpDataBlockEncoded = 0x1402808CB;

    /* Determine these by looking into KeBugCheckEx */
    g_LfiKernelStructs.PRCBProcessorStateOffset = (PVOID)(Command.Structs.KPRCB + 0x40);

    g_LfiKernelStructs.PsActiveProcessHead = (PLIST_ENTRY)(0x140296C10 - KernelNativeBase + KernelLoadBase);
    g_LfiKernelStructs.EProcessActiveProcessLinkOffset = 0x2e8;
    g_LfiKernelStructs.EProcessThreadListHeadOffset = 0x470;
    g_LfiKernelStructs.EThreadThreadListEntry = 0x400;

    g_KernelStructs = Command.Structs;

    Command.Structs.EThreadSegment = R_GS;
    Command.Structs.EThreadSegmentOffset = 0x188;
    Command.Structs.EThreadStackBaseOffset = 0x38;
    Command.Structs.EThreadStackLimitOffset = 0x30;

    Command.Structs.DPCStackBasePtr = Command.Structs.KPRCB + 0x2dc0;
    Command.Structs.DPCStackSize = 24 * 1024;

    Command.Structs.PsLoadedModuleList = (UINT_PTR)(0x1402CAA60 - KernelNativeBase + KernelLoadBase);

    Command.Structs.PerfLogImageUnload = (UINT_PTR)(0x1404D2A50 - KernelNativeBase + KernelLoadBase);

    S2EInvokePlugin("WindowsMonitor2", &Command, sizeof(Command));
}
#elif defined(_X86_)

static VOID Win8Handler32(UINT_PTR KernelLoadBase, UINT_PTR KernelNativeBase)
{
    KPCR *pKpcr;
    S2E_WINMON2_COMMAND Command;

    S2EMessage("Registering Windows 8 data structures (32-bits)\n");

    MonitorInitCommon(&Command);
    Command.Structs.KernelLoadBase = KernelLoadBase;
    Command.Structs.KernelNativeBase = KernelNativeBase;
    Command.Structs.LoadDriverPc = 0x6ABF61;
    Command.Structs.UnloadDriverPc = 0x6AE8E8; //IopDeleteDriver;

    //KeBugCheckEx saves CPU registers in the right places
    //and then calls KeBugCheck2. It is easier to hook KeBugCheck2.
    Command.Structs.KeBugCheckEx = 0x0051FF03; //KeBugCheck2

    pKpcr = (KPCR*) (0x00602000 - KernelNativeBase + KernelLoadBase); //_KiInitialPCR
    Command.Structs.KPCR = (UINT_PTR) pKpcr;

    Command.Structs.KPRCB = (UINT_PTR) pKpcr->Prcb;  //Command.Structs.KPCR + 0x180;

    //The KdVersionBlock pointer stored in KPCR is NULL.
    //Must compute it in some other way.
    //UPDATE: Don't care. Crash dump header is generated by Windows.
    Command.Structs.KdVersionBlock = 0; //Command.Structs.KPCR + 0x108;

    Command.Structs.KdDebuggerDataBlock = 0x005CD558;
    g_LfiKernelStructs.KdCopyDataBlock = 0x0051C9A5;
    g_LfiKernelStructs.KdpDataBlockEncoded =  0x00601174;

    /* Determine these by looking into KiBugCheck2 */
    g_LfiKernelStructs.PRCBProcessorStateOffset = (PVOID)(Command.Structs.KPRCB + 0x18);

    g_KernelStructs = Command.Structs;

    S2EInvokePlugin("WindowsMonitor2", &Command, sizeof(Command));
}

#endif

static VOID WindowsXPInitDbg(S2E_WINMON2_COMMAND *Command)
{
    UINT_PTR pKprcb;
    Command->Structs.KPCR = 0xffdff000;
    pKprcb = *(UINT_PTR*)(UINT_PTR)(Command->Structs.KPCR + 0x20);
    if (pKprcb != Command->Structs.KPCR + 0x120) {
        S2EMessageFmt("WindowsXPInitDbg: KPRCB invalid (found %p, expected %p)\n",
            pKprcb, (PVOID)(UINT_PTR)(Command->Structs.KPCR + 0x120));
    }

    Command->Structs.KPRCB = Command->Structs.KPCR + 0x120;
    Command->Structs.KdVersionBlock = *(UINT_PTR*)(UINT_PTR)(Command->Structs.KPCR + 0x34);
}

static VOID WindowsXPSP3FreeHandler(UINT_PTR KernelLoadBase, UINT_PTR KernelNativeBase)
{
    S2E_WINMON2_COMMAND Command;

    S2EMessage("Registering Windows XP SP3 Free Build data structures\n");

    MonitorInitCommon(&Command);
    Command.Structs.KernelLoadBase = KernelLoadBase;
    Command.Structs.KernelNativeBase = KernelNativeBase;
    Command.Structs.LoadDriverPc = 0x004cc99a;
    Command.Structs.UnloadDriverPc = 0x004EB33F;//IopDeleteDriver;
    Command.Structs.KdDebuggerDataBlock = 0x00475DE0;
    Command.Structs.KeBugCheckEx = (UINT_PTR) 0x0045BCAA; //KeBugCheck2

    Command.Structs.EThreadSegment = R_FS;
    Command.Structs.EThreadSegmentOffset = 0x124;
    Command.Structs.EThreadStackBaseOffset = 0x168;
    Command.Structs.EThreadStackLimitOffset = 0x1C;

    WindowsXPInitDbg(&Command);

    Command.Structs.DPCStackBasePtr = Command.Structs.KPRCB + 0x868;
    Command.Structs.DPCStackSize = 0x3000;

    Command.Structs.PsLoadedModuleList = (UINT_PTR)(0x4841C0 - KernelNativeBase + KernelLoadBase);

    g_LfiKernelStructs.PsActiveProcessHead = (PLIST_ENTRY)(0x48A358 - KernelNativeBase + KernelLoadBase);
    g_LfiKernelStructs.EProcessActiveProcessLinkOffset = 0x88;
    g_LfiKernelStructs.EProcessThreadListHeadOffset = 0x190;
    g_LfiKernelStructs.EThreadThreadListEntry = 0x22c;

    g_KernelStructs = Command.Structs;
    S2EInvokePlugin("WindowsMonitor2", &Command, sizeof(Command));
}

static VOID WindowsXPSP3CheckedHandler(UINT_PTR KernelLoadBase, UINT_PTR KernelNativeBase)
{
    S2E_WINMON2_COMMAND Command;

    S2EMessage("Registering Windows XP SP3 Checked Build data structures\n");

    MonitorInitCommon(&Command);
    Command.Structs.KernelLoadBase = KernelLoadBase;
    Command.Structs.KernelNativeBase = KernelNativeBase;
    Command.Structs.LoadDriverPc = 0x0052E5D6;
    Command.Structs.UnloadDriverPc = 0x00531A72;//IopDeleteDriver;
    Command.Structs.KdDebuggerDataBlock = 0x004E03F0;
    Command.Structs.KeBugCheckEx = (UINT_PTR) 0x0042F3E8; //KeBugCheck2

    Command.Structs.EThreadSegment = R_FS;
    Command.Structs.EThreadSegmentOffset = 0x124;
    Command.Structs.EThreadStackBaseOffset = 0x168;
    Command.Structs.EThreadStackLimitOffset = 0x1C;

    WindowsXPInitDbg(&Command);

    Command.Structs.DPCStackBasePtr = Command.Structs.KPRCB + 0x868;
    Command.Structs.DPCStackSize = 0x3000;

    Command.Structs.PsLoadedModuleList = (UINT_PTR)(0x4F3708 - KernelNativeBase + KernelLoadBase);

    g_LfiKernelStructs.PsActiveProcessHead = (PLIST_ENTRY)(0x503FD8 - KernelNativeBase + KernelLoadBase);
    g_LfiKernelStructs.EProcessActiveProcessLinkOffset = 0x88;
    g_LfiKernelStructs.EProcessThreadListHeadOffset = 0x190;
    g_LfiKernelStructs.EThreadThreadListEntry = 0x22c;

    g_KernelStructs = Command.Structs;
    S2EInvokePlugin("WindowsMonitor2", &Command, sizeof(Command));
}

