#include "private.h"

extern int interrupted;
extern bool parent_ready;

static vmi_event_t cpuid_event, singlestep_event, cc_event;

static event_response_t start_cpuid_cb(vmi_instance_t vmi, vmi_event_t *event)
{
    event->x86_regs->rip += event->cpuid_event.insn_length;

    if ( event->cpuid_event.leaf == 0x13371337 )
    {
        printf("Got start cpuid callback with leaf: 0x%x @ 0x%lx\n", event->cpuid_event.leaf, event->x86_regs->rip);

        vmi_clear_event(vmi, event, NULL);
        return VMI_EVENT_RESPONSE_SET_REGISTERS | VMI_EVENT_RESPONSE_TOGGLE_SINGLESTEP;
    }

    return VMI_EVENT_RESPONSE_SET_REGISTERS;
}

static event_response_t singlestep_cb(vmi_instance_t vmi, vmi_event_t *event)
{
    vmi_pause_vm(vmi);
    parent_ready = 1;
    interrupted = 1;

    printf("Parent VM is paused right after the harness CPUID @ 0x%lx\n", event->x86_regs->rip);

    vmi_clear_event(vmi, event, NULL);

    return 0;
}

static event_response_t start_cc_cb(vmi_instance_t vmi, vmi_event_t *event)
{
    access_context_t ctx = {
        .translate_mechanism = VMI_TM_PROCESS_DTB,
        .dtb = event->x86_regs->cr3,
        .addr = event->x86_regs->rip
    };

    if ( VMI_SUCCESS == vmi_write_8(vmi, &ctx, &start_byte) )
        parent_ready = 1;

    vmi_pause_vm(vmi);
    interrupted = 1;
    vmi_clear_event(vmi, event, NULL);

    printf("Parent VM is paused at the breakpoint location\n");

    return 0;
}

static void waitfor_start(vmi_instance_t vmi)
{
    SETUP_SINGLESTEP_EVENT(&singlestep_event, 1, singlestep_cb, 0);

    if ( harness_cpuid )
    {
        cpuid_event.version = VMI_EVENTS_VERSION;
        cpuid_event.type = VMI_EVENT_CPUID;
        cpuid_event.callback = start_cpuid_cb;

        if ( VMI_FAILURE == vmi_register_event(vmi, &cpuid_event) )
            return;
        if ( VMI_FAILURE == vmi_register_event(vmi, &singlestep_event) )
            return;

        printf("Waiting for harness start (cpuid with leaf 0x13371337)\n");

    } else {
        SETUP_INTERRUPT_EVENT(&cc_event, start_cc_cb);

        if ( VMI_FAILURE == vmi_register_event(vmi, &cc_event) )
            return;

        printf("Waiting for harness start (software breakpoint, 0xCC)\n");
    }

    loop(vmi);

    interrupted = 0;

    return;
}

bool make_parent_ready(void)
{
    bool ret = false;

    if ( !setup_vmi(&vmi, domain, domid, json, setup, true) )
    {
        fprintf(stderr, "Unable to start VMI on domain\n");
        goto done;
    }

    vcpus = vmi_get_num_vcpus(vmi);

    if ( vcpus > 1 )
    {
        fprintf(stderr, "The target domain has more then 1 vCPUs, not supported\n");
        return false;
    }

    if ( !domain )
        domain = vmi_get_name(vmi);
    if ( !domid )
        domid = vmi_get_vmid(vmi);
    if ( setup )
        waitfor_start(vmi);
    else
        parent_ready = true;

    if ( !parent_ready )
    {
        fprintf(stderr, "Unable to make domain fork ready\n");
        goto done;
    }

    setup_sinks(vmi);

    printf("Parent ready\n");
    ret = true;

done:
    vmi_destroy(vmi);
    vmi = NULL;
    return ret;
}
