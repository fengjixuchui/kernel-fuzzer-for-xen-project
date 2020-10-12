/*
 * Copyright (C) 2020 Intel Corporation
 * SPDX-License-Identifier: MIT
 */
#include "private.h"
#include "sink.h"

static const char *traptype[] = {
    [VMI_EVENT_SINGLESTEP] = "singlestep",
    [VMI_EVENT_CPUID] = "cpuid",
    [VMI_EVENT_INTERRUPT] = "int3",
    [VMI_EVENT_MEMORY] = "ept",
};
static vmi_event_t singlestep_event, int3_event, cpuid_event, ept_event;

extern int interrupted;
extern csh cs_handle;

/* doublefetch detection */
static addr_t doublefetch_check_va;
static addr_t reset_mem_permission;

/*
 * Control-flow tracing:
 * 1. start by disassembling code from the start address
 * 2. find next control-flow instruction and start monitoring it
 * 3. at control flow instruction remove monitor and create singlestep
 * 4. after a singlestep set start address to current RIP
 * 5. goto step 1
 */
static unsigned long tracer_counter;
static addr_t next_cf_vaddr;
static addr_t next_cf_paddr;
static uint8_t cc = 0xCC;
static uint8_t cf_backup;

static void breakpoint_next_cf(vmi_instance_t vmi)
{
    if ( VMI_SUCCESS == vmi_read_pa(vmi, next_cf_paddr, 1, &cf_backup, NULL) &&
         VMI_SUCCESS == vmi_write_pa(vmi, next_cf_paddr, 1, &cc, NULL) )
    {
        if ( debug ) printf("[TRACER] Next CF: 0x%lx -> 0x%lx\n", next_cf_vaddr, next_cf_paddr);
    }
}

static inline bool is_cf(unsigned int id)
{
    switch ( id )
    {
        case X86_INS_JA:
        case X86_INS_JAE:
        case X86_INS_JBE:
        case X86_INS_JB:
        case X86_INS_JCXZ:
        case X86_INS_JECXZ:
        case X86_INS_JE:
        case X86_INS_JGE:
        case X86_INS_JG:
        case X86_INS_JLE:
        case X86_INS_JL:
        case X86_INS_JMP:
        case X86_INS_LJMP:
        case X86_INS_JNE:
        case X86_INS_JNO:
        case X86_INS_JNP:
        case X86_INS_JNS:
        case X86_INS_JO:
        case X86_INS_JP:
        case X86_INS_JRCXZ:
        case X86_INS_JS:
        case X86_INS_CALL:
        case X86_INS_RET:
        case X86_INS_RETF:
        case X86_INS_RETFQ:
        case X86_INS_SYSCALL:
        case X86_INS_SYSRET:
            return true;
        default:
            break;
    }

    return false;
}

#define TRACER_CF_SEARCH_LIMIT 100u

static bool next_cf_insn(vmi_instance_t vmi, addr_t dtb, addr_t start)
{
    cs_insn *insn;
    size_t count;

    size_t read, search = 0;
    unsigned char buff[15];
    bool found = false;
    access_context_t ctx = {
        .translate_mechanism = VMI_TM_PROCESS_DTB,
        .dtb = dtb,
        .addr = start
    };

    while ( !found && search < TRACER_CF_SEARCH_LIMIT )
    {
        memset(buff, 0, 15);

        if ( VMI_FAILURE == vmi_read(vmi, &ctx, 15, buff, &read) && !read )
        {
            if ( debug ) printf("Failed to grab memory from 0x%lx with PT 0x%lx\n", start, dtb);
            goto done;
        }

        count = cs_disasm(cs_handle, buff, read, ctx.addr, 0, &insn);
        if ( !count )
        {
            if ( debug ) printf("No instruction was found at 0x%lx with PT 0x%lx\n", ctx.addr, dtb);
            goto done;
        }

        size_t j;
        for ( j=0; j<count; j++) {

            ctx.addr = insn[j].address + insn[j].size;

            if ( debug ) printf("Next instruction @ 0x%lx: %s, size %i!\n", insn[j].address, insn[j].mnemonic, insn[j].size);

            if ( is_cf(insn[j].id) )
            {
                next_cf_vaddr = insn[j].address;
                if ( VMI_FAILURE == vmi_pagetable_lookup(vmi, dtb, next_cf_vaddr, &next_cf_paddr) )
                {
                    if ( debug ) printf("Failed to lookup next instruction PA for 0x%lx with PT 0x%lx\n", next_cf_vaddr, dtb);
                    break;
                }

                found = true;

                if ( debug ) printf("Found next control flow instruction @ 0x%lx: %s!\n", next_cf_vaddr, insn[j].mnemonic);
                break;
            }
        }
        cs_free(insn, count);
    }

    if ( !found && debug )
        printf("Didn't find a control flow instruction starting from 0x%lx with a search limit %u! Counter: %lu\n",
               start, TRACER_CF_SEARCH_LIMIT, tracer_counter);

done:
    return found;
}

static event_response_t tracer_cb(vmi_instance_t vmi, vmi_event_t *event)
{
    if ( debug )
    {
        printf("[TRACER %s] RIP: 0x%lx", traptype[event->type], event->x86_regs->rip);

        if ( !nocov )
            printf(" CF limit: %lu/%lu\n", tracer_counter, limit);
    }

    /* Check if RIP is right now at any of the sink points */
    int c;
    for (c=0; c < __SINK_MAX; c++)
    {
        if ( sink_vaddr[c] == event->x86_regs->rip )
        {
            vmi_pause_vm(vmi);
            interrupted = 1;
            crash = 1;

            if ( debug ) printf("\t Sink %s! Tracer counter: %lu. Crash: %i.\n", sinks[c], tracer_counter, crash);

            if ( VMI_EVENT_INTERRUPT == event->type )
                event->interrupt_event.reinject = 0;

            if ( VMI_EVENT_SINGLESTEP == event->type )
                return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP;

            return 0;
        }
    }

    /* Check if we hit the end harness */
    if ( VMI_EVENT_CPUID == event->type )
    {
        if ( debug ) printf("CPUID leaf %x\n", event->cpuid_event.leaf);
        if ( event->cpuid_event.leaf == 0x13371337 )
        {
            // Harness signal on finish
            vmi_pause_vm(vmi);
            interrupted = 1;
            if ( debug ) printf("\t Harness signal on finish\n");
            return 0;
        }

        event->x86_regs->rip += event->cpuid_event.insn_length;
        return VMI_EVENT_RESPONSE_SET_REGISTERS;
    }

    /* We are still running */
    afl_instrument_location(event->x86_regs->rip);

    switch ( event->type )
    {

    /*
     * Singlestep (MTF) callbacks happen after:
     * - EPT violation used for doublefetch detection where we need to reset page permissions afterwards
     * - Stepping over a breakpoint CF where we need to find the next CF and breakpoint it
     *
     * In both cases we toggle the MTF off as the response
     */
    case VMI_EVENT_SINGLESTEP:
    {
        if ( reset_mem_permission )
        {
            vmi_set_mem_event(vmi, doublefetch, VMI_MEMACCESS_RWX, 0);
            reset_mem_permission = 0;
        } else if ( next_cf_insn(vmi, event->x86_regs->cr3, event->x86_regs->rip) )
            breakpoint_next_cf(vmi);

        return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP;
    }

    /*
     * Interrupt (int3) callbacks can happen when:
     * - 1) Tracing the control flow
     * - 2) End-harness with non-cpuid harnessing
     * - 3) The code we are fuzzing itself may have an int3
     */
    case VMI_EVENT_INTERRUPT:
    {
        /* Reinject needs to be set in all cases */
        event->interrupt_event.reinject = 0;

        if ( event->x86_regs->rip != next_cf_vaddr )
        {
            /* This is not the next CF address */
            if ( harness_cpuid )
            {
                /* This is case 3) expecting breakpoints only at the sinks so reinject this to the VM */
                if ( debug ) printf("\t Reinjecting unexpected breakpoint at 0x%lx\n", event->x86_regs->rip);
                event->interrupt_event.reinject = 1;
                return 0;
            }

            /*
             * This is case 2)
             * We are using breakpoints as a harness. We can't tell the difference if this is the end harness
             * or just a spurious int3 in the code being fuzzed so we treat it as the end harness. Should be
             * very rare.
             */
            vmi_pause_vm(vmi);
            interrupted = 1;
            if ( debug ) printf("\t Harness signal on finish\n");
            return 0;
        }

        /* This is case 1), we are at the expected breakpointed CF instruction */
        vmi_write_pa(vmi, next_cf_paddr, 1, &cf_backup, NULL);

        tracer_counter++;

        /* Turn on MTF if we are under the limit and continue */
        if ( limit == ~0ul || tracer_counter < limit )
            return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP;

        if ( debug ) printf("Hit the tracer limit: %lu\n", tracer_counter);
        vmi_pause_vm(vmi);
        interrupted = 1;
        break;
    }

    /* Used only when checking for doublefetches */
    case VMI_EVENT_MEMORY:
    {
        if ( debug ) printf("Mem access @ 0x%lx %c%c%c\n", event->mem_event.gla,
                            (event->mem_event.out_access & VMI_MEMACCESS_R) ? 'r' : '-',
                            (event->mem_event.out_access & VMI_MEMACCESS_W) ? 'w' : '-',
                            (event->mem_event.out_access & VMI_MEMACCESS_X) ? 'x' : '-');

        /* Only care about data-fetches */
        if ( event->mem_event.out_access & VMI_MEMACCESS_R )
        {
            /* If fetch happened at the address we just saw, doublefetch detected! */
            if ( event->mem_event.gla == doublefetch_check_va )
            {
                if ( debug ) printf("Doublefetch detected at 0x%lx\n", event->mem_event.gla);
                vmi_pause_vm(vmi);
                interrupted = 1;
                crash = 1;
                break;
            }

            /* Store address currently fetched for future reference */
            doublefetch_check_va = event->mem_event.gla;
        }

        /* Allow access through but mark that permissions need to be reset after singlestep */
        vmi_set_mem_event(vmi, doublefetch, VMI_MEMACCESS_N, 0);
        reset_mem_permission = 1;

        return VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP;
    }
    };

    return 0;
}

bool setup_trace(vmi_instance_t vmi)
{
    if ( debug ) printf("Setup trace\n");

    SETUP_SINGLESTEP_EVENT(&singlestep_event, 1, tracer_cb, 0);
    SETUP_INTERRUPT_EVENT(&int3_event, tracer_cb);

    if ( VMI_FAILURE == vmi_register_event(vmi, &singlestep_event) )
        return false;
    if ( VMI_FAILURE == vmi_register_event(vmi, &int3_event) )
        return false;

    if ( harness_cpuid )
    {
        cpuid_event.version = VMI_EVENTS_VERSION;
        cpuid_event.type = VMI_EVENT_CPUID;
        cpuid_event.callback = tracer_cb;

        if ( VMI_FAILURE == vmi_register_event(vmi, &cpuid_event) )
            return false;
    }

    if ( doublefetch )
    {
        // convert doublefetch page VA -> PA
        if ( VMI_FAILURE == vmi_pagetable_lookup(vmi, target_pagetable, doublefetch, &doublefetch) )
            return false;

        if ( debug ) printf("Doublefetch PA 0x%lx\n", doublefetch);

        // convert doublefetch page PA -> GFN
        doublefetch >>= 12;

        if ( debug ) printf("Doublefetch GFN 0x%lx\n", doublefetch);

        /* Register catch-all callback for all EPT events (most flexible LibVMI API) */
        SETUP_MEM_EVENT(&ept_event, ~0ULL, VMI_MEMACCESS_RWX, tracer_cb, 1);

        if ( VMI_FAILURE == vmi_register_event(vmi, &ept_event) )
            return false;
    }

    if ( debug ) printf("Setup trace finished\n");
    return true;
}

bool start_trace(vmi_instance_t vmi, addr_t address) {
    if ( debug ) printf("Starting trace from 0x%lx.\n", address);

    /* Permission needs to be removed for each iteration */
    if ( doublefetch )
    {
        doublefetch_check_va = 0;
        reset_mem_permission = 0;

        if ( VMI_FAILURE == vmi_set_mem_event(vmi, doublefetch, VMI_MEMACCESS_RW, 0) )
            return false;

        if ( debug ) printf("EPT access permissions removed from GFN 0x%lx\n", doublefetch);
    }

    if ( !nocov && !ptcov )
    {
        next_cf_vaddr = 0;
        next_cf_paddr = 0;
        tracer_counter = 0;

        if ( !next_cf_insn(vmi, target_pagetable, address) )
        {
            if ( debug ) printf("Failed starting trace from 0x%lx\n", address);
            return false;
        }

        breakpoint_next_cf(vmi);
    }

    return true;
}

void close_trace(vmi_instance_t vmi) {
    vmi_clear_event(vmi, &singlestep_event, NULL);
    vmi_clear_event(vmi, &int3_event, NULL);

    if ( harness_cpuid )
        vmi_clear_event(vmi, &cpuid_event, NULL);

    if ( doublefetch )
    {
        vmi_set_mem_event(vmi, doublefetch, VMI_MEMACCESS_N, 0);
        vmi_clear_event(vmi, &ept_event, NULL);
    }

    if ( debug ) printf("Closing tracer\n");
}

/* Inject breakpoints into the sink points in the sink VM */
bool make_sink_ready(void)
{
    vmi_instance_t sink_vmi = NULL;
    bool ret = false;

    if ( !setup_vmi(&sink_vmi, NULL, sinkdomid, json, false, true) )
        return ret;

    int c;
    for(c=0; c < __SINK_MAX; c++)
    {
        /*
         * Note that we assume all sink points defined by name are standard kernel functions and thus LibVMI
         * can just look up their address in the provided json. We can use LibVMI to look up functions in other
         * jsons as well but for that use vmi_get_symbol_addr_from_json. The reason we don't use that API for
         * the kernel here is to get the addresses with KASLR adjustment automatically. For kernel modules
         * the address would need to be added to the module's base address.
         */
        if ( !sink_vaddr[c] && VMI_FAILURE == vmi_translate_ksym2v(sink_vmi, sinks[c], &sink_vaddr[c]) )
        {
            fprintf(stderr, "Failed to find address for sink %s in the JSON\n", sinks[c]);
            continue;
        }

        if ( !sink_paddr[c] && VMI_FAILURE == vmi_pagetable_lookup(sink_vmi, target_pagetable, sink_vaddr[c], &sink_paddr[c]) )
        {
            if ( debug ) printf("Failed to translate %s V2P 0x%lx\n", sinks[c], sink_vaddr[c]);
            goto done;
        }
        if ( VMI_FAILURE == vmi_write_pa(sink_vmi, sink_paddr[c], 1, &cc, NULL) )
        {
            if ( debug ) printf("Failed to write %s PA 0x%lx\n", sinks[c], sink_paddr[c]);
            goto done;
        }

        if ( debug )
            printf("Setting breakpoint on sink %s 0x%lx -> 0x%lx\n",
                   sinks[c], sink_vaddr[c], sink_paddr[c]);
    }

    if ( debug ) printf("Sinks are ready\n");
    ret = true;

done:
    vmi_destroy(sink_vmi);
    return ret;
}
