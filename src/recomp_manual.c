/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
/* The host controller layer (xboxrecomp src/input) behind the XInput
 * overrides below. Included before recomp_funcs.h: it pulls in windows.h, and
 * the register-alias macros that header defines (eax, esp, ...) must not be in
 * force while the Windows headers are parsed. Same arrangement as the
 * Breakdown control's recomp_manual.c. */
#include "xinput_xbox.h"
/* The eax/esp/... -> g_eax/g_esp/... register aliases in recomp_types.h are
 * gated behind this define, as in every generated chunk. */
#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state ─────────────────────────────────────────── */
/* g_eax, g_esp and g_xbox_mem_offset come from the recomp_funcs.h include
 * above. Redeclaring them here without RECOMP_TLS conflicts with that
 * (storage-class mismatch), as it did on Breakdown. */

/* ── XInput: the host controller in place of the XDK's USB stack ── */

/*
 * XInput on the Xbox is XDK library code in the title's XPP section, and it
 * drives a USB host controller the runtime doesn't emulate. Its device tables
 * never change, so XGetDeviceChanges never reports a pad and the splash
 * screen shows "Please connect a controller" (operator, 2026-10-08).
 *
 * These replace the six entry points Burnout's pad module (sub_000714BF,
 * sub_000718BB, sub_00071565, sub_000717CA) calls, with xboxrecomp's
 * src/input: a real controller through XInput, or on port 0 the keyboard
 * stand-in or an XBOXRECOMP_PAD_SCRIPT when none is attached. They are a port
 * of the Breakdown control's overrides (Breakdown-Launcher fb7af5f), which
 * work. Burnout links the same XDK XPP code: each entry point was matched to
 * Breakdown's by instruction sequence (XGetDeviceChanges, XInputOpen and
 * XInputClose 100%, XInputGetState 93%, XInputGetCapabilities and
 * XInputSetState 82%), with Open/Close/GetCapabilities at Breakdown's exact
 * spacing. Each stdcall argument size was checked against the original's
 * `ret`, or its argument count in Ghidra (/mnt/dev/Burnout-Ghidra), and the
 * gamepad device-type table 0x00175214 is what the pad module pushes at the
 * XGetDeviceChanges and XInputOpen call sites (0x000714D2, 0x0007152D).
 *
 *   Breakdown     Burnout       entry point           ret
 *   0x00204CDD    0x00175FD6    XGetDeviceChanges     0xC
 *   0x002051FF    0x001766D6    XInputOpen            0x10
 *   0x00205255    0x0017672C    XInputClose           4
 *   0x00205261    0x00176738    XInputGetCapabilities 8
 *   0x00205439    0x00176915    XInputGetState        8
 *   0x002054AC    0x00176984    XInputSetState        8
 *
 * Layouts (XDK), as on Breakdown:
 *   XINPUT_STATE        DWORD dwPacketNumber; XINPUT_GAMEPAD (WORD wButtons,
 *                       BYTE bAnalogButtons[8], SHORT sThumbLX/LY/RX/RY)
 *   XINPUT_CAPABILITIES BYTE SubType; WORD Reserved; XINPUT_GAMEPAD In at +4;
 *                       XINPUT_RUMBLE Out at +22
 *   XINPUT_FEEDBACK     DWORD dwStatus; HANDLE hEvent; BYTE Reserved[58];
 *                       XINPUT_RUMBLE at +66
 */

#define BO_XDEVICE_TYPE_GAMEPAD  0x00175214u    /* XPP device-type table */
#define BO_PAD_HANDLE_BASE       0x58490000u    /* handle = base + port; only
                                                 * these overrides look inside */
#define BO_ERROR_INVALID_HANDLE        6u
#define BO_ERROR_DEVICE_NOT_CONNECTED  0x48Fu
#define BO_ERROR_IO_PENDING            0x3E5u

static int bo_pad_port(uint32_t handle)
{
    uint32_t port = handle - BO_PAD_HANDLE_BASE;
    return port < XBOX_MAX_CONTROLLERS ? (int)port : -1;
}

/* BOOL XGetDeviceChanges(PXPP_DEVICE_TYPE, PDWORD pdwInsertions,
 *                        PDWORD pdwRemovals) -- ret 0xC */
void sub_00175FD6(void)
{
    uint32_t type = MEM32(esp + 4), p_ins = MEM32(esp + 8), p_rem = MEM32(esp + 12);
    uint32_t ins = 0, rem = 0;

    if (type == BO_XDEVICE_TYPE_GAMEPAD) {
        /* Ports already reported present. A pad attached at boot is an
         * insertion on the first call, as on the console. */
        static uint32_t reported;
        uint32_t now = (uint32_t)xbox_InputConnectedMask() & 0xF;
        ins = now & ~reported;
        rem = reported & ~now;
        reported = now;
        if (ins | rem)
            fprintf(stderr, "[INPUT] XGetDeviceChanges: gamepad ports inserted 0x%X "
                    "removed 0x%X\n", ins, rem);
    }
    MEM32(p_ins) = ins;
    MEM32(p_rem) = rem;
    eax = (ins | rem) != 0;
    esp += 4 + 12;
}

/* HANDLE XInputOpen(PXPP_DEVICE_TYPE, DWORD dwPort, DWORD dwSlot,
 *                   PXINPUT_POLLING_PARAMETERS) -- ret 0x10 */
void sub_001766D6(void)
{
    uint32_t type = MEM32(esp + 4), port = MEM32(esp + 8), slot = MEM32(esp + 12);

    eax = 0;   /* NULL: no device there */
    if (type == BO_XDEVICE_TYPE_GAMEPAD && port < XBOX_MAX_CONTROLLERS
            && slot == 0 /* XDEVICE_NO_SLOT */
            && ((xbox_InputConnectedMask() >> port) & 1))
        eax = BO_PAD_HANDLE_BASE + port;
    fprintf(stderr, "[INPUT] XInputOpen(port %u) -> 0x%08X\n", port, eax);
    esp += 4 + 16;
}

/* VOID XInputClose(HANDLE) -- ret 4 */
void sub_0017672C(void)
{
    esp += 4 + 4;
}

/* DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES) -- ret 8 */
void sub_00176738(void)
{
    uint32_t handle = MEM32(esp + 4), p = MEM32(esp + 8), i;
    int port = bo_pad_port(handle);

    if (port < 0) {
        eax = BO_ERROR_INVALID_HANDLE;
    } else if (!((xbox_InputConnectedMask() >> port) & 1)) {
        eax = BO_ERROR_DEVICE_NOT_CONNECTED;
    } else {
        for (i = 0; i < 26; i++)
            MEM8(p + i) = 0;
        MEM8(p) = 1;                       /* XINPUT_DEVSUBTYPE_GC_GAMEPAD */
        MEM16(p + 4) = 0x00FF;             /* all eight digital buttons */
        for (i = 0; i < 8; i++)
            MEM8(p + 6 + i) = 0xFF;        /* 8-bit analog buttons */
        for (i = 0; i < 4; i++)
            MEM16(p + 14 + 2 * i) = 0xFFFF;  /* full-resolution sticks */
        MEM16(p + 22) = 0xFFFF;            /* both rumble motors */
        MEM16(p + 24) = 0xFFFF;
        eax = 0;
    }
    esp += 4 + 8;
}

/* DWORD XInputGetState(HANDLE, PXINPUT_STATE) -- ret 8 */
void sub_00176915(void)
{
    uint32_t handle = MEM32(esp + 4), p = MEM32(esp + 8), i;
    int port = bo_pad_port(handle);
    XBOX_INPUT_STATE state;

    if (port < 0) {
        eax = BO_ERROR_INVALID_HANDLE;
    } else if (xbox_InputGetState((DWORD)port, &state) != 0) {
        eax = BO_ERROR_DEVICE_NOT_CONNECTED;
    } else {
        MEM32(p) = state.dwPacketNumber;
        MEM16(p + 4) = state.Gamepad.wButtons;
        for (i = 0; i < 8; i++)
            MEM8(p + 6 + i) = state.Gamepad.bAnalogButtons[i];
        MEM16(p + 14) = (uint16_t)state.Gamepad.sThumbLX;
        MEM16(p + 16) = (uint16_t)state.Gamepad.sThumbLY;
        MEM16(p + 18) = (uint16_t)state.Gamepad.sThumbRX;
        MEM16(p + 20) = (uint16_t)state.Gamepad.sThumbRY;
        eax = 0;
    }
    esp += 4 + 8;
}

/* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK) -- ret 8
 *
 * Asynchronous on the console: it returns ERROR_IO_PENDING and fills
 * Header.dwStatus in when the motors have been set. Here the motors are set
 * at once, so dwStatus reads done straight away. A caller waiting on
 * Header.hEvent would need that event signalled, and nothing here does. */
void sub_00176984(void)
{
    uint32_t handle = MEM32(esp + 4), p = MEM32(esp + 8);
    int port = bo_pad_port(handle);

    if (port < 0) {
        eax = BO_ERROR_INVALID_HANDLE;
    } else {
        XBOX_VIBRATION vibration;
        static int warned_event;
        vibration.wLeftMotorSpeed = MEM16(p + 66);
        vibration.wRightMotorSpeed = MEM16(p + 68);
        xbox_InputSetState((DWORD)port, &vibration);   /* no-op without a pad */
        if (MEM32(p + 4) && !warned_event) {
            warned_event = 1;
            fprintf(stderr, "[INPUT] XInputSetState: feedback carries an event "
                    "handle this override never signals\n");
        }
        MEM32(p) = 0;                      /* Header.dwStatus = ERROR_SUCCESS */
        eax = BO_ERROR_IO_PENDING;
    }
    esp += 4 + 8;
}

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    /* Once a function is defined here, its callers reach it through this
     * lookup, so each override is registered (as on Breakdown). */
    if (xbox_va == 0x00175FD6) return sub_00175FD6;   /* XGetDeviceChanges */
    if (xbox_va == 0x001766D6) return sub_001766D6;   /* XInputOpen */
    if (xbox_va == 0x0017672C) return sub_0017672C;   /* XInputClose */
    if (xbox_va == 0x00176738) return sub_00176738;   /* XInputGetCapabilities */
    if (xbox_va == 0x00176915) return sub_00176915;   /* XInputGetState */
    if (xbox_va == 0x00176984) return sub_00176984;   /* XInputSetState */

    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}
