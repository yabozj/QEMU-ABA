/*
 *  ARM helper routines
 *
 *  Copyright (c) 2005-2007 CodeSourcery, LLC
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "internals.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "include/pku.h"

#define SIGNBIT (uint32_t)0x80000000
#define SIGNBIT64 ((uint64_t)1 << 63)

static CPUState *do_raise_exception(CPUARMState *env, uint32_t excp,
                                    uint32_t syndrome, uint32_t target_el)
{
    CPUState *cs = env_cpu(env);

    if (target_el == 1 && (arm_hcr_el2_eff(env) & HCR_TGE)) {
        /*
         * Redirect NS EL1 exceptions to NS EL2. These are reported with
         * their original syndrome register value, with the exception of
         * SIMD/FP access traps, which are reported as uncategorized
         * (see DDI0478C.a D1.10.4)
         */
        target_el = 2;
        if (syn_get_ec(syndrome) == EC_ADVSIMDFPACCESSTRAP) {
            syndrome = syn_uncategorized();
        }
    }

    assert(!excp_is_internal(excp));
    cs->exception_index = excp;
    env->exception.syndrome = syndrome;
    env->exception.target_el = target_el;

    return cs;
}

void raise_exception(CPUARMState *env, uint32_t excp,
                     uint32_t syndrome, uint32_t target_el)
{
    CPUState *cs = do_raise_exception(env, excp, syndrome, target_el);
    cpu_loop_exit(cs);
}

void raise_exception_ra(CPUARMState *env, uint32_t excp, uint32_t syndrome,
                        uint32_t target_el, uintptr_t ra)
{
    CPUState *cs = do_raise_exception(env, excp, syndrome, target_el);
    cpu_loop_exit_restore(cs, ra);
}

uint32_t HELPER(neon_tbl)(uint32_t ireg, uint32_t def, void *vn,
                          uint32_t maxindex)
{
    uint32_t val, shift;
    uint64_t *table = vn;

    val = 0;
    for (shift = 0; shift < 32; shift += 8) {
        uint32_t index = (ireg >> shift) & 0xff;
        if (index < maxindex) {
            uint32_t tmp = (table[index >> 3] >> ((index & 7) << 3)) & 0xff;
            val |= tmp << shift;
        } else {
            val |= def & (0xff << shift);
        }
    }
    return val;
}

void HELPER(v8m_stackcheck)(CPUARMState *env, uint32_t newvalue)
{
    /*
     * Perform the v8M stack limit check for SP updates from translated code,
     * raising an exception if the limit is breached.
     */
    if (newvalue < v7m_sp_limit(env)) {
        CPUState *cs = env_cpu(env);

        /*
         * Stack limit exceptions are a rare case, so rather than syncing
         * PC/condbits before the call, we use cpu_restore_state() to
         * get them right before raising the exception.
         */
        cpu_restore_state(cs, GETPC(), true);
        raise_exception(env, EXCP_STKOF, 0, 1);
    }
}

uint32_t HELPER(add_setq)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a + b;
    if (((res ^ a) & SIGNBIT) && !((a ^ b) & SIGNBIT))
        env->QF = 1;
    return res;
}

uint32_t HELPER(add_saturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a + b;
    if (((res ^ a) & SIGNBIT) && !((a ^ b) & SIGNBIT)) {
        env->QF = 1;
        res = ~(((int32_t)a >> 31) ^ SIGNBIT);
    }
    return res;
}

uint32_t HELPER(sub_saturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a - b;
    if (((res ^ a) & SIGNBIT) && ((a ^ b) & SIGNBIT)) {
        env->QF = 1;
        res = ~(((int32_t)a >> 31) ^ SIGNBIT);
    }
    return res;
}

uint32_t HELPER(double_saturate)(CPUARMState *env, int32_t val)
{
    uint32_t res;
    if (val >= 0x40000000) {
        res = ~SIGNBIT;
        env->QF = 1;
    } else if (val <= (int32_t)0xc0000000) {
        res = SIGNBIT;
        env->QF = 1;
    } else {
        res = val << 1;
    }
    return res;
}

uint32_t HELPER(add_usaturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a + b;
    if (res < a) {
        env->QF = 1;
        res = ~0;
    }
    return res;
}

uint32_t HELPER(sub_usaturate)(CPUARMState *env, uint32_t a, uint32_t b)
{
    uint32_t res = a - b;
    if (res > a) {
        env->QF = 1;
        res = 0;
    }
    return res;
}

/* Signed saturation.  */
static inline uint32_t do_ssat(CPUARMState *env, int32_t val, int shift)
{
    int32_t top;
    uint32_t mask;

    top = val >> shift;
    mask = (1u << shift) - 1;
    if (top > 0) {
        env->QF = 1;
        return mask;
    } else if (top < -1) {
        env->QF = 1;
        return ~mask;
    }
    return val;
}

/* Unsigned saturation.  */
static inline uint32_t do_usat(CPUARMState *env, int32_t val, int shift)
{
    uint32_t max;

    max = (1u << shift) - 1;
    if (val < 0) {
        env->QF = 1;
        return 0;
    } else if (val > max) {
        env->QF = 1;
        return max;
    }
    return val;
}

/* Signed saturate.  */
uint32_t HELPER(ssat)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    return do_ssat(env, x, shift);
}

/* Dual halfword signed saturate.  */
uint32_t HELPER(ssat16)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    uint32_t res;

    res = (uint16_t)do_ssat(env, (int16_t)x, shift);
    res |= do_ssat(env, ((int32_t)x) >> 16, shift) << 16;
    return res;
}

/* Unsigned saturate.  */
uint32_t HELPER(usat)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    return do_usat(env, x, shift);
}

/* Dual halfword unsigned saturate.  */
uint32_t HELPER(usat16)(CPUARMState *env, uint32_t x, uint32_t shift)
{
    uint32_t res;

    res = (uint16_t)do_usat(env, (int16_t)x, shift);
    res |= do_usat(env, ((int32_t)x) >> 16, shift) << 16;
    return res;
}

void HELPER(setend)(CPUARMState *env)
{
    env->uncached_cpsr ^= CPSR_E;
}

/* Function checks whether WFx (WFI/WFE) instructions are set up to be trapped.
 * The function returns the target EL (1-3) if the instruction is to be trapped;
 * otherwise it returns 0 indicating it is not trapped.
 */
static inline int check_wfx_trap(CPUARMState *env, bool is_wfe)
{
    int cur_el = arm_current_el(env);
    uint64_t mask;

    if (arm_feature(env, ARM_FEATURE_M)) {
        /* M profile cores can never trap WFI/WFE. */
        return 0;
    }

    /* If we are currently in EL0 then we need to check if SCTLR is set up for
     * WFx instructions being trapped to EL1. These trap bits don't exist in v7.
     */
    if (cur_el < 1 && arm_feature(env, ARM_FEATURE_V8)) {
        int target_el;

        mask = is_wfe ? SCTLR_nTWE : SCTLR_nTWI;
        if (arm_is_secure_below_el3(env) && !arm_el_is_aa64(env, 3)) {
            /* Secure EL0 and Secure PL1 is at EL3 */
            target_el = 3;
        } else {
            target_el = 1;
        }

        if (!(env->cp15.sctlr_el[target_el] & mask)) {
            return target_el;
        }
    }

    /* We are not trapping to EL1; trap to EL2 if HCR_EL2 requires it
     * No need for ARM_FEATURE check as if HCR_EL2 doesn't exist the
     * bits will be zero indicating no trap.
     */
    if (cur_el < 2) {
        mask = is_wfe ? HCR_TWE : HCR_TWI;
        if (arm_hcr_el2_eff(env) & mask) {
            return 2;
        }
    }

    /* We are not trapping to EL1 or EL2; trap to EL3 if SCR_EL3 requires it */
    if (cur_el < 3) {
        mask = (is_wfe) ? SCR_TWE : SCR_TWI;
        if (env->cp15.scr_el3 & mask) {
            return 3;
        }
    }

    return 0;
}

void HELPER(wfi)(CPUARMState *env, uint32_t insn_len)
{
    CPUState *cs = env_cpu(env);
    int target_el = check_wfx_trap(env, false);

    if (cpu_has_work(cs)) {
        /* Don't bother to go into our "low power state" if
         * we would just wake up immediately.
         */
        return;
    }

    if (target_el) {
        env->pc -= insn_len;
        raise_exception(env, EXCP_UDEF, syn_wfx(1, 0xe, 0, insn_len == 2),
                        target_el);
    }

    cs->exception_index = EXCP_HLT;
    cs->halted = 1;
    cpu_loop_exit(cs);
}

void HELPER(wfe)(CPUARMState *env)
{
    /* This is a hint instruction that is semantically different
     * from YIELD even though we currently implement it identically.
     * Don't actually halt the CPU, just yield back to top
     * level loop. This is not going into a "low power state"
     * (ie halting until some event occurs), so we never take
     * a configurable trap to a different exception level.
     */
    HELPER(yield)(env);
}

void HELPER(yield)(CPUARMState *env)
{
    CPUState *cs = env_cpu(env);

    /* This is a non-trappable hint instruction that generally indicates
     * that the guest is currently busy-looping. Yield control back to the
     * top level loop so that a more deserving VCPU has a chance to run.
     */
    cs->exception_index = EXCP_YIELD;
    cpu_loop_exit(cs);
}

/* Raise an internal-to-QEMU exception. This is limited to only
 * those EXCP values which are special cases for QEMU to interrupt
 * execution and not to be used for exceptions which are passed to
 * the guest (those must all have syndrome information and thus should
 * use exception_with_syndrome).
 */
void HELPER(exception_internal)(CPUARMState *env, uint32_t excp)
{
    CPUState *cs = env_cpu(env);

    assert(excp_is_internal(excp));
    cs->exception_index = excp;
    cpu_loop_exit(cs);
}

/* Raise an exception with the specified syndrome register value */
void HELPER(exception_with_syndrome)(CPUARMState *env, uint32_t excp,
                                     uint32_t syndrome, uint32_t target_el)
{
    raise_exception(env, excp, syndrome, target_el);
}

/* Raise an EXCP_BKPT with the specified syndrome register value,
 * targeting the correct exception level for debug exceptions.
 */
void HELPER(exception_bkpt_insn)(CPUARMState *env, uint32_t syndrome)
{
    int debug_el = arm_debug_target_el(env);
    int cur_el = arm_current_el(env);

    /* FSR will only be used if the debug target EL is AArch32. */
    env->exception.fsr = arm_debug_exception_fsr(env);
    /* FAR is UNKNOWN: clear vaddress to avoid potentially exposing
     * values to the guest that it shouldn't be able to see at its
     * exception/security level.
     */
    env->exception.vaddress = 0;
    /*
     * Other kinds of architectural debug exception are ignored if
     * they target an exception level below the current one (in QEMU
     * this is checked by arm_generate_debug_exceptions()). Breakpoint
     * instructions are special because they always generate an exception
     * to somewhere: if they can't go to the configured debug exception
     * level they are taken to the current exception level.
     */
    if (debug_el < cur_el) {
        debug_el = cur_el;
    }
    raise_exception(env, EXCP_BKPT, syndrome, debug_el);
}

uint32_t HELPER(cpsr_read)(CPUARMState *env)
{
    return cpsr_read(env) & ~(CPSR_EXEC | CPSR_RESERVED);
}

void HELPER(cpsr_write)(CPUARMState *env, uint32_t val, uint32_t mask)
{
    cpsr_write(env, val, mask, CPSRWriteByInstr);
}

/* Write the CPSR for a 32-bit exception return */
void HELPER(cpsr_write_eret)(CPUARMState *env, uint32_t val)
{
    qemu_mutex_lock_iothread();
    arm_call_pre_el_change_hook(env_archcpu(env));
    qemu_mutex_unlock_iothread();

    cpsr_write(env, val, CPSR_ERET_MASK, CPSRWriteExceptionReturn);

    /* Generated code has already stored the new PC value, but
     * without masking out its low bits, because which bits need
     * masking depends on whether we're returning to Thumb or ARM
     * state. Do the masking now.
     */
    env->regs[15] &= (env->thumb ? ~1 : ~3);

    qemu_mutex_lock_iothread();
    arm_call_el_change_hook(env_archcpu(env));
    qemu_mutex_unlock_iothread();
}

/* Access to user mode registers from privileged modes.  */
uint32_t HELPER(get_user_reg)(CPUARMState *env, uint32_t regno)
{
    uint32_t val;

    if (regno == 13) {
        val = env->banked_r13[BANK_USRSYS];
    } else if (regno == 14) {
        val = env->banked_r14[BANK_USRSYS];
    } else if (regno >= 8
               && (env->uncached_cpsr & 0x1f) == ARM_CPU_MODE_FIQ) {
        val = env->usr_regs[regno - 8];
    } else {
        val = env->regs[regno];
    }
    return val;
}

void HELPER(set_user_reg)(CPUARMState *env, uint32_t regno, uint32_t val)
{
    if (regno == 13) {
        env->banked_r13[BANK_USRSYS] = val;
    } else if (regno == 14) {
        env->banked_r14[BANK_USRSYS] = val;
    } else if (regno >= 8
               && (env->uncached_cpsr & 0x1f) == ARM_CPU_MODE_FIQ) {
        env->usr_regs[regno - 8] = val;
    } else {
        env->regs[regno] = val;
    }
}

void HELPER(set_r13_banked)(CPUARMState *env, uint32_t mode, uint32_t val)
{
    if ((env->uncached_cpsr & CPSR_M) == mode) {
        env->regs[13] = val;
    } else {
        env->banked_r13[bank_number(mode)] = val;
    }
}

uint32_t HELPER(get_r13_banked)(CPUARMState *env, uint32_t mode)
{
    if ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_SYS) {
        /* SRS instruction is UNPREDICTABLE from System mode; we UNDEF.
         * Other UNPREDICTABLE and UNDEF cases were caught at translate time.
         */
        raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                        exception_target_el(env));
    }

    if ((env->uncached_cpsr & CPSR_M) == mode) {
        return env->regs[13];
    } else {
        return env->banked_r13[bank_number(mode)];
    }
}

static void msr_mrs_banked_exc_checks(CPUARMState *env, uint32_t tgtmode,
                                      uint32_t regno)
{
    /* Raise an exception if the requested access is one of the UNPREDICTABLE
     * cases; otherwise return. This broadly corresponds to the pseudocode
     * BankedRegisterAccessValid() and SPSRAccessValid(),
     * except that we have already handled some cases at translate time.
     */
    int curmode = env->uncached_cpsr & CPSR_M;

    if (regno == 17) {
        /* ELR_Hyp: a special case because access from tgtmode is OK */
        if (curmode != ARM_CPU_MODE_HYP && curmode != ARM_CPU_MODE_MON) {
            goto undef;
        }
        return;
    }

    if (curmode == tgtmode) {
        goto undef;
    }

    if (tgtmode == ARM_CPU_MODE_USR) {
        switch (regno) {
        case 8 ... 12:
            if (curmode != ARM_CPU_MODE_FIQ) {
                goto undef;
            }
            break;
        case 13:
            if (curmode == ARM_CPU_MODE_SYS) {
                goto undef;
            }
            break;
        case 14:
            if (curmode == ARM_CPU_MODE_HYP || curmode == ARM_CPU_MODE_SYS) {
                goto undef;
            }
            break;
        default:
            break;
        }
    }

    if (tgtmode == ARM_CPU_MODE_HYP) {
        /* SPSR_Hyp, r13_hyp: accessible from Monitor mode only */
        if (curmode != ARM_CPU_MODE_MON) {
            goto undef;
        }
    }

    return;

undef:
    raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                    exception_target_el(env));
}

void HELPER(msr_banked)(CPUARMState *env, uint32_t value, uint32_t tgtmode,
                        uint32_t regno)
{
    msr_mrs_banked_exc_checks(env, tgtmode, regno);

    switch (regno) {
    case 16: /* SPSRs */
        env->banked_spsr[bank_number(tgtmode)] = value;
        break;
    case 17: /* ELR_Hyp */
        env->elr_el[2] = value;
        break;
    case 13:
        env->banked_r13[bank_number(tgtmode)] = value;
        break;
    case 14:
        env->banked_r14[r14_bank_number(tgtmode)] = value;
        break;
    case 8 ... 12:
        switch (tgtmode) {
        case ARM_CPU_MODE_USR:
            env->usr_regs[regno - 8] = value;
            break;
        case ARM_CPU_MODE_FIQ:
            env->fiq_regs[regno - 8] = value;
            break;
        default:
            g_assert_not_reached();
        }
        break;
    default:
        g_assert_not_reached();
    }
}

uint32_t HELPER(mrs_banked)(CPUARMState *env, uint32_t tgtmode, uint32_t regno)
{
    msr_mrs_banked_exc_checks(env, tgtmode, regno);

    switch (regno) {
    case 16: /* SPSRs */
        return env->banked_spsr[bank_number(tgtmode)];
    case 17: /* ELR_Hyp */
        return env->elr_el[2];
    case 13:
        return env->banked_r13[bank_number(tgtmode)];
    case 14:
        return env->banked_r14[r14_bank_number(tgtmode)];
    case 8 ... 12:
        switch (tgtmode) {
        case ARM_CPU_MODE_USR:
            return env->usr_regs[regno - 8];
        case ARM_CPU_MODE_FIQ:
            return env->fiq_regs[regno - 8];
        default:
            g_assert_not_reached();
        }
    default:
        g_assert_not_reached();
    }
}

void HELPER(access_check_cp_reg)(CPUARMState *env, void *rip, uint32_t syndrome,
                                 uint32_t isread)
{
    const ARMCPRegInfo *ri = rip;
    int target_el;

    if (arm_feature(env, ARM_FEATURE_XSCALE) && ri->cp < 14
        && extract32(env->cp15.c15_cpar, ri->cp, 1) == 0) {
        raise_exception(env, EXCP_UDEF, syndrome, exception_target_el(env));
    }

    if (!ri->accessfn) {
        return;
    }

    switch (ri->accessfn(env, ri, isread)) {
    case CP_ACCESS_OK:
        return;
    case CP_ACCESS_TRAP:
        target_el = exception_target_el(env);
        break;
    case CP_ACCESS_TRAP_EL2:
        /* Requesting a trap to EL2 when we're in EL3 or S-EL0/1 is
         * a bug in the access function.
         */
        assert(!arm_is_secure(env) && arm_current_el(env) != 3);
        target_el = 2;
        break;
    case CP_ACCESS_TRAP_EL3:
        target_el = 3;
        break;
    case CP_ACCESS_TRAP_UNCATEGORIZED:
        target_el = exception_target_el(env);
        syndrome = syn_uncategorized();
        break;
    case CP_ACCESS_TRAP_UNCATEGORIZED_EL2:
        target_el = 2;
        syndrome = syn_uncategorized();
        break;
    case CP_ACCESS_TRAP_UNCATEGORIZED_EL3:
        target_el = 3;
        syndrome = syn_uncategorized();
        break;
    case CP_ACCESS_TRAP_FP_EL2:
        target_el = 2;
        /* Since we are an implementation that takes exceptions on a trapped
         * conditional insn only if the insn has passed its condition code
         * check, we take the IMPDEF choice to always report CV=1 COND=0xe
         * (which is also the required value for AArch64 traps).
         */
        syndrome = syn_fp_access_trap(1, 0xe, false);
        break;
    case CP_ACCESS_TRAP_FP_EL3:
        target_el = 3;
        syndrome = syn_fp_access_trap(1, 0xe, false);
        break;
    default:
        g_assert_not_reached();
    }

    raise_exception(env, EXCP_UDEF, syndrome, target_el);
}

void HELPER(set_cp_reg)(CPUARMState *env, void *rip, uint32_t value)
{
    const ARMCPRegInfo *ri = rip;

    if (ri->type & ARM_CP_IO) {
        qemu_mutex_lock_iothread();
        ri->writefn(env, ri, value);
        qemu_mutex_unlock_iothread();
    } else {
        ri->writefn(env, ri, value);
    }
}

uint32_t HELPER(get_cp_reg)(CPUARMState *env, void *rip)
{
    const ARMCPRegInfo *ri = rip;
    uint32_t res;

    if (ri->type & ARM_CP_IO) {
        qemu_mutex_lock_iothread();
        res = ri->readfn(env, ri);
        qemu_mutex_unlock_iothread();
    } else {
        res = ri->readfn(env, ri);
    }

    return res;
}

void HELPER(set_cp_reg64)(CPUARMState *env, void *rip, uint64_t value)
{
    const ARMCPRegInfo *ri = rip;

    if (ri->type & ARM_CP_IO) {
        qemu_mutex_lock_iothread();
        ri->writefn(env, ri, value);
        qemu_mutex_unlock_iothread();
    } else {
        ri->writefn(env, ri, value);
    }
}

uint64_t HELPER(get_cp_reg64)(CPUARMState *env, void *rip)
{
    const ARMCPRegInfo *ri = rip;
    uint64_t res;

    if (ri->type & ARM_CP_IO) {
        qemu_mutex_lock_iothread();
        res = ri->readfn(env, ri);
        qemu_mutex_unlock_iothread();
    } else {
        res = ri->readfn(env, ri);
    }

    return res;
}

void HELPER(pre_hvc)(CPUARMState *env)
{
    ARMCPU *cpu = env_archcpu(env);
    int cur_el = arm_current_el(env);
    /* FIXME: Use actual secure state.  */
    bool secure = false;
    bool undef;

    if (arm_is_psci_call(cpu, EXCP_HVC)) {
        /* If PSCI is enabled and this looks like a valid PSCI call then
         * that overrides the architecturally mandated HVC behaviour.
         */
        return;
    }

    if (!arm_feature(env, ARM_FEATURE_EL2)) {
        /* If EL2 doesn't exist, HVC always UNDEFs */
        undef = true;
    } else if (arm_feature(env, ARM_FEATURE_EL3)) {
        /* EL3.HCE has priority over EL2.HCD. */
        undef = !(env->cp15.scr_el3 & SCR_HCE);
    } else {
        undef = env->cp15.hcr_el2 & HCR_HCD;
    }

    /* In ARMv7 and ARMv8/AArch32, HVC is undef in secure state.
     * For ARMv8/AArch64, HVC is allowed in EL3.
     * Note that we've already trapped HVC from EL0 at translation
     * time.
     */
    if (secure && (!is_a64(env) || cur_el == 1)) {
        undef = true;
    }

    if (undef) {
        raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                        exception_target_el(env));
    }
}

void HELPER(pre_smc)(CPUARMState *env, uint32_t syndrome)
{
    ARMCPU *cpu = env_archcpu(env);
    int cur_el = arm_current_el(env);
    bool secure = arm_is_secure(env);
    bool smd_flag = env->cp15.scr_el3 & SCR_SMD;

    /*
     * SMC behaviour is summarized in the following table.
     * This helper handles the "Trap to EL2" and "Undef insn" cases.
     * The "Trap to EL3" and "PSCI call" cases are handled in the exception
     * helper.
     *
     *  -> ARM_FEATURE_EL3 and !SMD
     *                           HCR_TSC && NS EL1   !HCR_TSC || !NS EL1
     *
     *  Conduit SMC, valid call  Trap to EL2         PSCI Call
     *  Conduit SMC, inval call  Trap to EL2         Trap to EL3
     *  Conduit not SMC          Trap to EL2         Trap to EL3
     *
     *
     *  -> ARM_FEATURE_EL3 and SMD
     *                           HCR_TSC && NS EL1   !HCR_TSC || !NS EL1
     *
     *  Conduit SMC, valid call  Trap to EL2         PSCI Call
     *  Conduit SMC, inval call  Trap to EL2         Undef insn
     *  Conduit not SMC          Trap to EL2         Undef insn
     *
     *
     *  -> !ARM_FEATURE_EL3
     *                           HCR_TSC && NS EL1   !HCR_TSC || !NS EL1
     *
     *  Conduit SMC, valid call  Trap to EL2         PSCI Call
     *  Conduit SMC, inval call  Trap to EL2         Undef insn
     *  Conduit not SMC          Undef insn          Undef insn
     */

    /* On ARMv8 with EL3 AArch64, SMD applies to both S and NS state.
     * On ARMv8 with EL3 AArch32, or ARMv7 with the Virtualization
     *  extensions, SMD only applies to NS state.
     * On ARMv7 without the Virtualization extensions, the SMD bit
     * doesn't exist, but we forbid the guest to set it to 1 in scr_write(),
     * so we need not special case this here.
     */
    bool smd = arm_feature(env, ARM_FEATURE_AARCH64) ? smd_flag
                                                     : smd_flag && !secure;

    if (!arm_feature(env, ARM_FEATURE_EL3) &&
        cpu->psci_conduit != QEMU_PSCI_CONDUIT_SMC) {
        /* If we have no EL3 then SMC always UNDEFs and can't be
         * trapped to EL2. PSCI-via-SMC is a sort of ersatz EL3
         * firmware within QEMU, and we want an EL2 guest to be able
         * to forbid its EL1 from making PSCI calls into QEMU's
         * "firmware" via HCR.TSC, so for these purposes treat
         * PSCI-via-SMC as implying an EL3.
         * This handles the very last line of the previous table.
         */
        raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                        exception_target_el(env));
    }

    if (cur_el == 1 && (arm_hcr_el2_eff(env) & HCR_TSC)) {
        /* In NS EL1, HCR controlled routing to EL2 has priority over SMD.
         * We also want an EL2 guest to be able to forbid its EL1 from
         * making PSCI calls into QEMU's "firmware" via HCR.TSC.
         * This handles all the "Trap to EL2" cases of the previous table.
         */
        raise_exception(env, EXCP_HYP_TRAP, syndrome, 2);
    }

    /* Catch the two remaining "Undef insn" cases of the previous table:
     *    - PSCI conduit is SMC but we don't have a valid PCSI call,
     *    - We don't have EL3 or SMD is set.
     */
    if (!arm_is_psci_call(cpu, EXCP_SMC) &&
        (smd || !arm_feature(env, ARM_FEATURE_EL3))) {
        raise_exception(env, EXCP_UDEF, syn_uncategorized(),
                        exception_target_el(env));
    }
}

/* ??? Flag setting arithmetic is awkward because we need to do comparisons.
   The only way to do that in TCG is a conditional branch, which clobbers
   all our temporaries.  For now implement these as helper functions.  */

/* Similarly for variable shift instructions.  */

uint32_t HELPER(shl_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift = i & 0xff;
    if (shift >= 32) {
        if (shift == 32)
            env->CF = x & 1;
        else
            env->CF = 0;
        return 0;
    } else if (shift != 0) {
        env->CF = (x >> (32 - shift)) & 1;
        return x << shift;
    }
    return x;
}

uint32_t HELPER(shr_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift = i & 0xff;
    if (shift >= 32) {
        if (shift == 32)
            env->CF = (x >> 31) & 1;
        else
            env->CF = 0;
        return 0;
    } else if (shift != 0) {
        env->CF = (x >> (shift - 1)) & 1;
        return x >> shift;
    }
    return x;
}

uint32_t HELPER(sar_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift = i & 0xff;
    if (shift >= 32) {
        env->CF = (x >> 31) & 1;
        return (int32_t)x >> 31;
    } else if (shift != 0) {
        env->CF = (x >> (shift - 1)) & 1;
        return (int32_t)x >> shift;
    }
    return x;
}

uint32_t HELPER(ror_cc)(CPUARMState *env, uint32_t x, uint32_t i)
{
    int shift1, shift;
    shift1 = i & 0xff;
    shift = shift1 & 0x1f;
    if (shift == 0) {
        if (shift1 != 0)
            env->CF = (x >> 31) & 1;
        return x;
    } else {
        env->CF = (x >> (shift - 1)) & 1;
        return ((uint32_t)x >> shift) | (x << (32 - shift));
    }
}

void HELPER(dc_zva)(CPUARMState *env, uint64_t vaddr_in)
{
    /*
     * Implement DC ZVA, which zeroes a fixed-length block of memory.
     * Note that we do not implement the (architecturally mandated)
     * alignment fault for attempts to use this on Device memory
     * (which matches the usual QEMU behaviour of not implementing either
     * alignment faults or any memory attribute handling).
     */

    ARMCPU *cpu = env_archcpu(env);
    uint64_t blocklen = 4 << cpu->dcz_blocksize;
    uint64_t vaddr = vaddr_in & ~(blocklen - 1);

#ifndef CONFIG_USER_ONLY
    {
        /*
         * Slightly awkwardly, QEMU's TARGET_PAGE_SIZE may be less than
         * the block size so we might have to do more than one TLB lookup.
         * We know that in fact for any v8 CPU the page size is at least 4K
         * and the block size must be 2K or less, but TARGET_PAGE_SIZE is only
         * 1K as an artefact of legacy v5 subpage support being present in the
         * same QEMU executable. So in practice the hostaddr[] array has
         * two entries, given the current setting of TARGET_PAGE_BITS_MIN.
         */
        int maxidx = DIV_ROUND_UP(blocklen, TARGET_PAGE_SIZE);
        void *hostaddr[DIV_ROUND_UP(2 * KiB, 1 << TARGET_PAGE_BITS_MIN)];
        int try, i;
        unsigned mmu_idx = cpu_mmu_index(env, false);
        TCGMemOpIdx oi = make_memop_idx(MO_UB, mmu_idx);

        assert(maxidx <= ARRAY_SIZE(hostaddr));

        for (try = 0; try < 2; try++) {

            for (i = 0; i < maxidx; i++) {
                hostaddr[i] = tlb_vaddr_to_host(env,
                                                vaddr + TARGET_PAGE_SIZE * i,
                                                1, mmu_idx);
                if (!hostaddr[i]) {
                    break;
                }
            }
            if (i == maxidx) {
                /*
                 * If it's all in the TLB it's fair game for just writing to;
                 * we know we don't need to update dirty status, etc.
                 */
                for (i = 0; i < maxidx - 1; i++) {
                    memset(hostaddr[i], 0, TARGET_PAGE_SIZE);
                }
                memset(hostaddr[i], 0, blocklen - (i * TARGET_PAGE_SIZE));
                return;
            }
            /*
             * OK, try a store and see if we can populate the tlb. This
             * might cause an exception if the memory isn't writable,
             * in which case we will longjmp out of here. We must for
             * this purpose use the actual register value passed to us
             * so that we get the fault address right.
             */
            helper_ret_stb_mmu(env, vaddr_in, 0, oi, GETPC());
            /* Now we can populate the other TLB entries, if any */
            for (i = 0; i < maxidx; i++) {
                uint64_t va = vaddr + TARGET_PAGE_SIZE * i;
                if (va != (vaddr_in & TARGET_PAGE_MASK)) {
                    helper_ret_stb_mmu(env, va, 0, oi, GETPC());
                }
            }
        }

        /*
         * Slow path (probably attempt to do this to an I/O device or
         * similar, or clearing of a block of code we have translations
         * cached for). Just do a series of byte writes as the architecture
         * demands. It's not worth trying to use a cpu_physical_memory_map(),
         * memset(), unmap() sequence here because:
         *  + we'd need to account for the blocksize being larger than a page
         *  + the direct-RAM access case is almost always going to be dealt
         *    with in the fastpath code above, so there's no speed benefit
         *  + we would have to deal with the map returning NULL because the
         *    bounce buffer was in use
         */
        for (i = 0; i < blocklen; i++) {
            helper_ret_stb_mmu(env, vaddr + i, 0, oi, GETPC());
        }
    }
#else
    memset(g2h(vaddr), 0, blocklen);
#endif
}

extern int ldex_count;
extern long long llsc_single;
extern long long llsc_multi;
extern int is_multi;
void HELPER(offload_load_exclusive_count)(uint32_t addr)
{
	if (!is_multi)
		__sync_fetch_and_add(&llsc_single, 1);
	else
		__sync_fetch_and_add(&llsc_multi, 1);

    //fprintf(stderr, "helper_offload_load_exclusive\taddr %p, value %p\n", addr, *(int*)(g2h(addr)));
}

void HELPER(offload_store_exclusive_count)(uint32_t addr)
{
    //extern int stex_count;
	//__sync_fetch_and_add(&stex_count, 1);
}

void HELPER(print_aa32_addr)(uint32_t addr)
{
    fprintf(stderr, "[print_aa32_addr]\taa32 addr = %x\n", addr);
}

extern int x_monitor_set_exclusive_addr(void* p_node, uint32_t addr);
extern int target_mprotect(abi_ulong, abi_ulong, int);
void HELPER(mpk_llsc_add)(uint32_t addr, uint32_t tid)
{
	target_ulong page_addr = addr & 0xfffff000;
    // fprintf(stderr, "[pf_llsc_add]\taddr = %x, node_addr = %lx\n", page_addr, node_addr);
    //用一个链表串起来即可
	x_monitor_set_exclusive_addr(tid, addr);
    add_pku_protect(tid, addr);
    //wait for count == tcount
    // env_archcpu->pkey = 0;
    // env_cpu->pkey = 0;
    //替换成pkey_mprotect
    //pkey_mprotect用于关联
    // pkey_mprotect(page_addr, 0x1000, PROT_READ | PROT_WRITE, pkey);
    target_mprotect(page_addr, 0x1000, PROT_READ);
}

void HELPER(mpk_llsc_remove)(uint32_t addr, uint32_t tid)
{
    remove_pku_protect(tid, addr);
}

extern bool find_key_in_global2(unsigned addr, uint32_t tid);
extern __thread CPUState *thread_cpu;

void HELPER(mpk_llsc_checkflag)(uint32_t addr, uint32_t tid)
{
    if(!find_key_in_global2(addr))
    {
        CPUArchState *env = thread_cpu->env_ptr;
        CPUState *cpu = env_cpu(env);
        if((CPUARMState *)cpu->exclusive_addr == addr){
            (CPUARMState *)cpu->exclusive_addr = 0;
        }
    }
}