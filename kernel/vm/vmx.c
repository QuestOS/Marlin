/*                    The Quest Operating System
 *  Copyright (C) 2005-2012  Richard West, Boston University
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifdef USE_VMX

#include "vm/vmx.h"
#include "vm/ept.h"
#include "kernel.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "util/cpuid.h"
#include "util/printf.h"
#include "smp/apic.h"
#include "arch/i386.h"
#include "arch/i386-mtrr.h"
#include "sched/sched.h"
#include "vm/shdr.h"
#include "vm/shm.h"
#include "drivers/pci/pci.h"
#ifdef USE_LINUX_SANDBOX
#include "vm/linux_boot.h"
#include "drivers/serial/pololu.h"
#endif

#define DEBUG_VMX 0
#define VMX_EPT
/* Disable XCR for guests */
//#define DISABLE_XCR

#if DEBUG_VMX > 2
#define DLOG(fmt,...) DLOG_PREFIX("vmx",fmt,##__VA_ARGS__)
#else
#define DLOG(fmt,...) ;
#endif

/* Exception will trigger VM-Exit if defined. */
//#define EXCEPTION_EXIT

#define com1_printf logger_printf

#define VMX_NUM_INSTR_ERRORS 29
#if DEBUG_VMX > 1
static char *vm_instruction_errors[] = {
  /* 00 */ "No error",
  /* 01 */ "VMCALL executed in VMX root operation",
  /* 02 */ "VMCLEAR with invalid physical address",
  /* 03 */ "VMCLEAR with VMXON pointer",
  /* 04 */ "VMLAUNCH with non-clear VMCS",
  /* 05 */ "VMRESUME with non-launched VMCS",
  /* 06 */ "VMRESUME with a corrupted VMCS (indicates corruption of the current VMCS)",
  /* 07 */ "VM entry with invalid control field(s)",
  /* 08 */ "VM entry with invalid host-state field(s)",
  /* 09 */ "VMPTRLD with invalid physical address",
  /* 10 */ "VMPTRLD with VMXON pointer",
  /* 11 */ "VMPTRLD with incorrect VMCS revision identifier",
  /* 12 */ "VMREAD/VMWRITE from/to unsupported VMCS component",
  /* 13 */ "VMWRITE to read-only VMCS component",
  /* 14 */ "unused code: 14",
  /* 15 */ "VMXON executed in VMX root operation",
  /* 16 */ "VM entry with invalid executive-VMCS pointer",
  /* 17 */ "VM entry with non-launched executive VMCS",
  /* 18 */ "VM entry with executive-VMCS pointer not VMXON pointer (when attempting to deactivate the dual-monitor treatment of SMIs and SMM)",
  /* 19 */ "VMCALL with non-clear VMCS (when attempting to activate the dual-monitor treatment of SMIs and SMM)",
  /* 20 */ "VMCALL with invalid VM-exit control fields",
  /* 21 */ "unused code: 21",
  /* 22 */ "VMCALL with incorrect MSEG revision identifier (when attempting to activate the dual-monitor treatment of SMIs and SMM)",
  /* 23 */ "VMXOFF under dual-monitor treatment of SMIs and SMM",
  /* 24 */ "VMCALL with invalid SMM-monitor features (when attempting to activate the dual-monitor treatment of SMIs and SMM)",
  /* 25 */ "VM entry with invalid VM-execution control fields in executive VMCS (when attempting to return from SMM)",
  /* 26 */ "VM entry with events blocked by MOV SS.",
  /* 27 */ "unused code: 27",
  /* 28 */ "Invalid operand to INVEPT/INVVPID."
};

#define VMX_NUM_EXIT_REASONS 56
static char *vm_exit_reasons[] = {
  /* 00 */ "Exception or non-maskable interrupt (NMI).",
  /* 01 */ "External interrupt.",
  /* 02 */ "Triple fault.",
  /* 03 */ "INIT signal.",
  /* 04 */ "Start-up IPI (SIPI).",
  /* 05 */ "I/O system-management interrupt (SMI).",
  /* 06 */ "Other SMI.",
  /* 07 */ "Interrupt window.",
  /* 08 */ "NMI window.",
  /* 09 */ "Task switch.",
  /* 10 */ "CPUID.",
  /* 11 */ "GETSEC.",
  /* 12 */ "HLT.",
  /* 13 */ "INVD.",
  /* 14 */ "INVLPG.",
  /* 15 */ "RDPMC.",
  /* 16 */ "RDTSC.",
  /* 17 */ "RSM.",
  /* 18 */ "VMCALL.",
  /* 19 */ "VMCLEAR.",
  /* 20 */ "VMLAUNCH.",
  /* 21 */ "VMPTRLD.",
  /* 22 */ "VMPTRST.",
  /* 23 */ "VMREAD.",
  /* 24 */ "VMRESUME.",
  /* 25 */ "VMWRITE.",
  /* 26 */ "VMXOFF.",
  /* 27 */ "VMXON.",
  /* 28 */ "Control-register accesses.",
  /* 29 */ "MOV DR.",
  /* 30 */ "I/O instruction.",
  /* 31 */ "RDMSR.",
  /* 32 */ "WRMSR.",
  /* 33 */ "VM-entry failure due to invalid guest state.",
  /* 34 */ "VM-entry failure due to MSR loading.",
  /* 35 */ "reserved (35)",
  /* 36 */ "MWAIT.",
  /* 37 */ "Monitor trap flag.",
  /* 38 */ "reserved (38)",
  /* 39 */ "MONITOR.",
  /* 40 */ "PAUSE.",
  /* 41 */ "VM-entry failure due to machine check.",
  /* 42 */ "reserved (42)",
  /* 43 */ "TPR below threshold.",
  /* 44 */ "APIC access.",
  /* 45 */ "reserved (45)",
  /* 46 */ "Access to GDTR or IDTR.",
  /* 47 */ "Access to LDTR or TR.",
  /* 48 */ "EPT violation.",
  /* 49 */ "EPT misconfiguration.",
  /* 50 */ "INVEPT.",
  /* 51 */ "RDTSCP.",
  /* 52 */ "VMX-preemption timer expired.",
  /* 53 */ "INVVPID.",
  /* 54 */ "WBINVD.",
  /* 55 */ "XSETBV.  ",
};
#endif

bool vmx_enabled = FALSE;
bool shared_driver_available = TRUE;

void
vmx_detect (void)
{
  if (cpuid_vmx_support ()) {
    print ("VMX support detected\n");
    vmx_enabled = TRUE;
  }
}

void
vmx_test_guest (void)
{
  for (;;)
    asm volatile ("int $0xE");
}

static char *vmx_cr_access_register_names[] = {
  "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"
};

void
vmx_vm_exit_reason (void)
{
  uint32 reason = vmread (VMXENC_EXIT_REASON);
  uint32 qualif = vmread (VMXENC_EXIT_QUAL);
  uint32 intinf = vmread (VMXENC_VM_EXIT_INTERRUPT_INFO);
  uint32 ercode = vmread (VMXENC_VM_EXIT_INTERRUPT_ERRCODE);
  /*******************************************************
   * uint32 inslen = vmread (VMXENC_VM_EXIT_INSTR_LEN);  *
   * uint32 insinf = vmread (VMXENC_VM_EXIT_INSTR_INFO); *
   *******************************************************/
  uint8 crnum, type, reg, vec;

  switch (reason) {
  case 0x0:
    /* Exception or NMI */
    if (intinf & 0x80000000) {
      char *cause;
      vec = intinf & 0xFF;
      type = (intinf & 0x700) >> 8;
      switch (type) {
      case 0: cause = "external interrupt"; break;
      case 2: cause = "NMI"; break;
      case 3: cause = "hardware exception"; break;
      case 6: cause = "software exception"; break;
      default: cause = "unknown"; break;
      }
      com1_printf ("  EXCEPTION: vector=%.2X code=%X cause=%s\n",
                   vec, (intinf & 0x800) ? ercode : 0, cause);
      if (vec == 0xE && type == 3) {
        /* Page fault */
        com1_printf ("    Page Fault at %.8X\n", qualif);
      }
    }
    break;
  case 0x1C:
    /* Control Register access */
    crnum = qualif & 0xF;
    type  = (qualif & 0x30) >> 4;
    reg   = (qualif & 0xF00) >> 8;
    switch (type) {
    case 0:
      com1_printf ("  CR WRITE: MOV %%%s, %%CR%d\n",
                   vmx_cr_access_register_names[reg],
                   crnum);
      break;
    case 1:
      com1_printf ("  CR READ: MOV %%CR%d, %%%s\n",
                   crnum,
                   vmx_cr_access_register_names[reg]);
      break;
    case 2:
      com1_printf ("  CLTS\n");
      break;
    case 3:
      com1_printf ("  LMSW\n");
      break;
    }
    break;
  }
}


static uint32 vmxon_frame[MAX_CPUS];
static u32 msr_bitmaps[1024] ALIGNED (0x1000);

int
vmx_load_monitor (virtual_machine *vm)
{
  uint32 phys_id = (uint32)LAPIC_get_physical_ID ();

  if (vm->loaded)
    return -1;

  vmptrld (vm->vmcs_frame);

  if (vmx_get_error () != 0) {
#if DEBUG_VMX > 0
    com1_printf ("VMPTRLD error\n");
#endif
    return -1;
  }

  vm->loaded = TRUE;
  vm->current_cpu = phys_id;
  return 0;
}

int
vmx_unload_monitor (virtual_machine *vm)
{
  vmclear (vm->vmcs_frame);

  if (!vm->loaded)
    return -1;

  if (vmx_get_error () != 0) {
#if DEBUG_VMX > 0
    com1_printf ("VMCLEAR error\n");
#endif
    return -1;
  }

  vm->loaded = FALSE;
  return 0;
}

int
vmx_destroy_monitor (virtual_machine *vm)
{
  if (vm->loaded)
    vmx_unload_monitor (vm);
  free_phys_frame (vm->vmcs_frame);
  vm->vmcs_frame = 0;
  return 0;
}

#if 0
int
vmx_create_monitor_raw (virtual_machine *vm)
{
  uint32 phys_id = (uint32)LAPIC_get_physical_ID ();
  uint32 *vmcs_virt;
  uint32 cr;
  descriptor ad;

  vm->realmode = TRUE;
  vm->launched = vm->loaded = FALSE;
  vm->current_cpu = phys_id;
  vm->guest_regs.eax = vm->guest_regs.ebx = vm->guest_regs.ecx =
    vm->guest_regs.edx = vm->guest_regs.esi = vm->guest_regs.edi =
    vm->guest_regs.ebp = 0;

  /* Setup the Virtual Machine Control Section */
  vm->vmcs_frame = alloc_phys_frame ();
  vmcs_virt  = map_virtual_page (vm->vmcs_frame | 3);
  vmcs_virt[0] = rdmsr (IA32_VMX_BASIC);
  vmcs_virt[1] = 0;
  unmap_virtual_page (vmcs_virt);

  vmclear (vm->vmcs_frame);

  if (vmx_load_monitor (vm) != 0)
    goto abort_load_VM;

  /* Setup Guest State */
  vmwrite ((1<<1)  |           /* Reserved bit set */
           (1<<17) |           /* VM86 */
           0,
           VMXENC_GUEST_RFLAGS);
  asm volatile ("movl %%cr0, %0":"=r" (cr));
  vmwrite (cr, VMXENC_GUEST_CR0);
  asm volatile ("movl %%cr3, %0":"=r" (cr));
  vmwrite (cr, VMXENC_GUEST_CR3);
  asm volatile ("movl %%cr4, %0":"=r" (cr));
  vmwrite (cr, VMXENC_GUEST_CR4);
  vmwrite (0x0, VMXENC_GUEST_DR7);
  vmwrite (0x0, VMXENC_GUEST_CS_SEL);
  vmwrite (VMX_VM86_START_SS_SEL, VMXENC_GUEST_SS_SEL);
  vmwrite (0x0, VMXENC_GUEST_DS_SEL);
  vmwrite (0x0, VMXENC_GUEST_ES_SEL);
  vmwrite (0x0, VMXENC_GUEST_FS_SEL);
  vmwrite (0x0, VMXENC_GUEST_GS_SEL);
  vmwrite (str (), VMXENC_GUEST_TR_SEL);
  vmwrite ((uint32) lookup_TSS (str ()), VMXENC_GUEST_TR_BASE);
  vmwrite (0x0, VMXENC_GUEST_CS_BASE);
  vmwrite (VMX_VM86_START_SS_SEL << 4, VMXENC_GUEST_SS_BASE);
  vmwrite (0x0, VMXENC_GUEST_DS_BASE);
  vmwrite (0x0, VMXENC_GUEST_ES_BASE);
  vmwrite (0x0, VMXENC_GUEST_FS_BASE);
  vmwrite (0x0, VMXENC_GUEST_GS_BASE);
  vmwrite ((uint32) sgdtr (), VMXENC_GUEST_GDTR_BASE);
  vmwrite ((uint32) sidtr (), VMXENC_GUEST_IDTR_BASE);
  vmwrite (0xFFFF, VMXENC_GUEST_CS_LIMIT);
  vmwrite (0xFFFF, VMXENC_GUEST_DS_LIMIT);
  vmwrite (0xFFFF, VMXENC_GUEST_ES_LIMIT);
  vmwrite (0xFFFF, VMXENC_GUEST_FS_LIMIT);
  vmwrite (0xFFFF, VMXENC_GUEST_GS_LIMIT);
  vmwrite (0xFFFF, VMXENC_GUEST_SS_LIMIT);
#define ACCESS(ad)                              \
  (( 0x01            << 0x00 ) |                \
   ( ad.uType        << 0x00 ) |                \
   ( ad.uDPL         << 0x05 ) |                \
   ( ad.fPresent     << 0x07 ) |                \
   ( ad.f            << 0x0C ) |                \
   ( ad.f0           << 0x0D ) |                \
   ( ad.fX           << 0x0E ) |                \
   ( ad.fGranularity << 0x0F ))
  vmwrite (0xF3, VMXENC_GUEST_CS_ACCESS);
  vmwrite (0xF3, VMXENC_GUEST_DS_ACCESS);
  vmwrite (0xF3, VMXENC_GUEST_ES_ACCESS);
  vmwrite (0xF3, VMXENC_GUEST_FS_ACCESS);
  vmwrite (0xF3, VMXENC_GUEST_GS_ACCESS);
  vmwrite (0xF3, VMXENC_GUEST_SS_ACCESS);
  vmwrite (0x8B, VMXENC_GUEST_TR_ACCESS);
#undef ACCESS
  vmwrite ((uint32) sgdtr (), VMXENC_GUEST_GDTR_BASE);
  vmwrite ((uint32) sidtr (), VMXENC_GUEST_IDTR_BASE);
  vmwrite (sgdtr_limit (), VMXENC_GUEST_GDTR_LIMIT);
  vmwrite (sidtr_limit (), VMXENC_GUEST_IDTR_LIMIT);
  get_GDT_descriptor (str (), &ad);
  vmwrite (ad.uLimit0 | (ad.uLimit1 << 16), VMXENC_GUEST_TR_LIMIT);
  //vmwrite ((uint32) vmx_code16_entry, VMXENC_GUEST_RIP);
  vmwrite ((uint32) VMX_VM86_START_SP, VMXENC_GUEST_RSP);
  vmwrite (0, VMXENC_GUEST_LDTR_SEL);
  vmwrite (0, VMXENC_GUEST_LDTR_BASE);
  vmwrite (0, VMXENC_GUEST_LDTR_LIMIT);
  vmwrite (0x82, VMXENC_GUEST_LDTR_ACCESS);
  vmwrite (0, VMXENC_GUEST_IA32_SYSENTER_CS);
  vmwrite (0, VMXENC_GUEST_IA32_SYSENTER_ESP);
  vmwrite (0, VMXENC_GUEST_IA32_SYSENTER_EIP);
  vmwrite64 (0xFFFFFFFFFFFFFFFFLL, VMXENC_VMCS_LINK_PTR);
  vmwrite (0, VMXENC_GUEST_PENDING_DEBUG_EXCEPTIONS);
  vmwrite (0, VMXENC_GUEST_ACTIVITY);
  vmwrite (0, VMXENC_GUEST_INTERRUPTIBILITY);
  vmwrite (~0, VMXENC_EXCEPTION_BITMAP);
  vmwrite (0, VMXENC_PAGE_FAULT_ERRCODE_MASK);
  vmwrite (0, VMXENC_PAGE_FAULT_ERRCODE_MATCH);
  /* Mask the PG and PE bits. */
  vmwrite (0x80000001, VMXENC_CR0_GUEST_HOST_MASK);
  /* Although we start in real-mode, this read-shadow is not used
   * until the VM86 simulation of real-mode is disabled.  At which
   * point, we are simulating prot-mode.  Therefore, leave PE set in
   * the read-shadow. */
  vmwrite (0x00000001, VMXENC_CR0_READ_SHADOW);

  return 0;

 abort_load_VM:
  vmx_destroy_monitor (vm);
  return -1;
}
#endif

static void
vmx_guest_state_dup (u32 rip0, u32 rsp0)
{
  uint32 cr, sel, base, limit, access;
  descriptor ad;

  asm volatile ("pushfl; pop %0":"=r" (cr));
  vmwrite (cr, VMXENC_GUEST_RFLAGS);
  asm volatile ("movl %%cr0, %0":"=r" (cr));
  vmwrite (cr, VMXENC_GUEST_CR0);
  asm volatile ("movl %%cr3, %0":"=r" (cr));
  vmwrite (cr, VMXENC_GUEST_CR3);
  asm volatile ("movl %%cr4, %0":"=r" (cr));
  vmwrite (cr, VMXENC_GUEST_CR4);
  vmwrite (0x0, VMXENC_GUEST_DR7);
  logger_printf ("GUEST-STATE: FLAGS=0x%p CR0=0x%p CR3=0x%p CR4=0x%p\n",
                 vmread (VMXENC_GUEST_RFLAGS),
                 vmread (VMXENC_GUEST_CR0),
                 vmread (VMXENC_GUEST_CR3),
                 vmread (VMXENC_GUEST_CR4));

#define ACCESS(ad)                              \
  (( 0x01            << 0x00 ) |                \
   ( ad.uType        << 0x00 ) |                \
   ( ad.uDPL         << 0x05 ) |                \
   ( ad.fPresent     << 0x07 ) |                \
   ( ad.f            << 0x0C ) |                \
   ( ad.f0           << 0x0D ) |                \
   ( ad.fX           << 0x0E ) |                \
   ( ad.fGranularity << 0x0F ))

  /* Setup segment selector/base/limit/access entries */
#define SETUPSEG(seg) do {                                              \
    asm volatile ("movl %%" __stringify(seg) ", %0":"=r" (sel));        \
    get_GDT_descriptor (sel, &ad);                                      \
    base = (ad.pBase0 | (ad.pBase1 << 16) | (ad.pBase2 << 24));         \
    limit = ad.uLimit0 | (ad.uLimit1 << 16);                            \
    if (ad.fGranularity) { limit <<= 12; limit |= 0xFFF; }              \
    access = ACCESS (ad);                                               \
    vmwrite (sel, VMXENC_GUEST_##seg##_SEL);                            \
    vmwrite (base, VMXENC_GUEST_##seg##_BASE);                          \
    vmwrite (limit, VMXENC_GUEST_##seg##_LIMIT);                        \
    vmwrite (access, VMXENC_GUEST_##seg##_ACCESS);                      \
    logger_printf ("GUEST-STATE: %s=0x%.02X base=0x%p limit=0x%p access=0x%.02X\n", \
                   __stringify(seg), sel, base, limit, access);         \
  } while (0)

  SETUPSEG (CS);
  SETUPSEG (SS);
  SETUPSEG (DS);
  SETUPSEG (ES);
  SETUPSEG (FS);
  SETUPSEG (GS);

  /* TR */
  sel = hw_str ();
  get_GDT_descriptor (sel, &ad);
  base = (ad.pBase0 | (ad.pBase1 << 16) | (ad.pBase2 << 24));
  limit = ad.uLimit0 | (ad.uLimit1 << 16);
  if (ad.fGranularity) { limit <<= 12; limit |= 0xFFF; }
  access = ACCESS (ad);
  vmwrite (sel, VMXENC_GUEST_TR_SEL);
  vmwrite (base, VMXENC_GUEST_TR_BASE);
  vmwrite (limit, VMXENC_GUEST_TR_LIMIT);
  vmwrite (access, VMXENC_GUEST_TR_ACCESS);
  logger_printf ("GUEST-STATE: %s=0x%.02X base=0x%p limit=0x%p access=0x%.02X\n",
                 "TR", sel, base, limit, access);

#undef ACCESS

  /* LDTR */
  vmwrite (0, VMXENC_GUEST_LDTR_SEL);
  vmwrite (0, VMXENC_GUEST_LDTR_BASE);
  vmwrite (0, VMXENC_GUEST_LDTR_LIMIT);
  vmwrite (0x10082, VMXENC_GUEST_LDTR_ACCESS);

  /* GDTR */
  vmwrite ((uint32) sgdtr (), VMXENC_GUEST_GDTR_BASE);
  vmwrite (sgdtr_limit (), VMXENC_GUEST_GDTR_LIMIT);

  /* IDTR */
  vmwrite ((uint32) sidtr (), VMXENC_GUEST_IDTR_BASE);
  vmwrite (sidtr_limit (), VMXENC_GUEST_IDTR_LIMIT);

  /* RIP/RSP */
  vmwrite ((uint32) rip0, VMXENC_GUEST_RIP);
  vmwrite ((uint32) rsp0, VMXENC_GUEST_RSP);
  logger_printf ("GUEST-STATE: RIP=0x%p RSP=0x%p\n", rip0, rsp0);

  /* SYSENTER MSRs */
  vmwrite (0, VMXENC_GUEST_IA32_SYSENTER_CS);
  vmwrite (0, VMXENC_GUEST_IA32_SYSENTER_ESP);
  vmwrite (0, VMXENC_GUEST_IA32_SYSENTER_EIP);

  vmwrite64 (0xFFFFFFFFFFFFFFFFLL, VMXENC_VMCS_LINK_PTR);
  vmwrite (0, VMXENC_GUEST_PENDING_DEBUG_EXCEPTIONS);
  vmwrite (0, VMXENC_GUEST_ACTIVITY);
  vmwrite (0, VMXENC_GUEST_INTERRUPTIBILITY);

  return;
}

int
vmx_create_monitor (virtual_machine *vm, u32 rip0, u32 rsp0)
{
  uint32 phys_id = (uint32)LAPIC_get_physical_ID ();
  uint32 *vmcs_virt;

  vm->realmode = FALSE;
  vm->launched = vm->loaded = FALSE;
  vm->current_cpu = phys_id;
  vm->guest_regs.eax = vm->guest_regs.ebx = vm->guest_regs.ecx =
    vm->guest_regs.edx = vm->guest_regs.esi = vm->guest_regs.edi =
    vm->guest_regs.ebp = 0;

  /* Setup the Virtual Machine Control Section */
  vm->vmcs_frame = alloc_phys_frame ();
  vmcs_virt  = map_virtual_page (vm->vmcs_frame | 3);
  vmcs_virt[0] = rdmsr (IA32_VMX_BASIC);
  vmcs_virt[1] = 0;
  unmap_virtual_page (vmcs_virt);

  vmclear (vm->vmcs_frame);

  if (vmx_load_monitor (vm) != 0)
    goto abort_load_VM;

  /* Setup Guest State */
  vmx_guest_state_dup (rip0, rsp0);

  return 0;

 abort_load_VM:
  vmx_destroy_monitor (vm);
  return -1;
}

/* 8KB memory region for IO bitmaps. It contains bits for ports 0x0000 to 0xFFFF. */
static uint32 * io_bitmaps = NULL;

#define IOBITMAP_SET(table,index) ((table)[(index)>>5] |= (1 << ((index) & 31)))
#define IOBITMAP_CLR(table,index) ((table)[(index)>>5] &= ~(1 << ((index) & 31)))
#define IOBITMAP_TST(table,index) ((table)[(index)>>5] & (1 << ((index) & 31)))

static uint32 last_reason = 0;
static uint32 last_port = 0;
static uint32 last_gphys = 0;
static uint8 last_permission = EPT_NO_ACCESS;

typedef struct {
  uint8 sandbox_id;
  uint16 vendorID;
  uint16 deviceID;
} pci_dev_blacklist_t;

static pci_dev_blacklist_t pci_dev_blacklist[MAX_PCI_BLACKLIST_LEN];

bool
pci_dev_in_blacklist (uint16 vendid, uint16 devid, uint8 sb)
{
  int i = 0;

  for (i = 0; i < MAX_PCI_BLACKLIST_LEN; i++) {
    if (pci_dev_blacklist[i].vendorID == 0xFFFF) {
      return FALSE;
    } else if ((pci_dev_blacklist[i].vendorID == vendid) &&
               (pci_dev_blacklist[i].deviceID == devid) &&
               (pci_dev_blacklist[i].sandbox_id == sb)) {
      return TRUE;
    }
  }

  return FALSE;
}

bool
pci_add_dev_blacklist (uint16 vendid, uint16 devid, uint8 sb)
{
  int i = 0;

  for (i = 0; i < MAX_PCI_BLACKLIST_LEN; i++) {
    if (pci_dev_blacklist[i].vendorID == 0xFFFF) {
      pci_dev_blacklist[i].vendorID = vendid;
      pci_dev_blacklist[i].deviceID = devid;
      pci_dev_blacklist[i].sandbox_id = sb;
      return TRUE;
    }
  }

  return FALSE;
}

static bool
dev_access_allowed (uint16 vendid, uint16 devid)
{
  if (pci_dev_in_blacklist (vendid, devid, (uint8) get_pcpu_id ())) {
    return FALSE;
  }

  return TRUE;
}

#define IOAPIC_ADDR_DEFAULT 0xFEC00000uL
#define MP_IOAPIC_READ(x)   (*((volatile uint32 *) (IOAPIC_ADDR_DEFAULT+(x))))
#define MP_IOAPIC_WRITE(x,y) (*((volatile uint32 *) (IOAPIC_ADDR_DEFAULT+(x))) = (y))

typedef struct {
  uint8 reg;
  uint8 sandbox_id;
} PACKED ioapic_blacklist_t;

static ioapic_blacklist_t ioapic_blacklist[MAX_IOAPIC_BLACKLIST_LEN];

bool
ioapic_reg_in_blacklist (uint8 reg, uint8 sb)
{
  int i = 0;

  for (i = 0; i < MAX_IOAPIC_BLACKLIST_LEN; i++) {
    if (ioapic_blacklist[i].reg == 0xFF) {
      return FALSE;
    } else if ((ioapic_blacklist[i].reg == reg) &&
               (ioapic_blacklist[i].sandbox_id == sb)) {
      return TRUE;
    }
  }

  return FALSE;
}

bool
ioapic_add_reg_blacklist (uint8 reg, uint8 sb)
{
  int i = 0;

  for (i = 0; i < MAX_IOAPIC_BLACKLIST_LEN; i++) {
    if (ioapic_blacklist[i].reg == 0xFF) {
      /* Empty block */
      ioapic_blacklist[i].reg = reg;
      ioapic_blacklist[i].sandbox_id = sb;
      return TRUE;
    }
  }

  return FALSE;
}

bool
vmx_io_blacklist_init ()
{
  int i = 0;

  for (i = 0; i < MAX_IOAPIC_BLACKLIST_LEN; i++) {
    ioapic_blacklist[i].reg = 0xFF;
  }

  for (i = 0; i < MAX_PCI_BLACKLIST_LEN; i++) {
    pci_dev_blacklist[i].vendorID = 0xFFFF;
  }

#ifdef USE_LINUX_SANDBOX
  /* --YL-- Hard coded IOAPIC redirection table entry for Realtek NIC */
  /* We will add facility to make this more configurable later. */
  //ioapic_add_reg_blacklist (0x30, LINUX_SANDBOX);
  //ioapic_add_reg_blacklist (0x31, LINUX_SANDBOX);
  //pci_add_dev_blacklist (0x10EC, 0x8168, LINUX_SANDBOX);
  /* MosChip 9922 Serial Card */
  pci_add_dev_blacklist (0x9710, 0x9922, LINUX_SANDBOX);
#endif
  
  return TRUE;
}

/* Returns 0 to skip the access or 1 to allow access */
static int
gphys_access_type (uint32 gphys_addr)
{
  uint64 reg = 0;

  if ((gphys_addr >> 12) == 0xFEC00) {
    /* Access to IOAPIC */
    if (gphys_addr == 0xFEC00010ul) {
      /* Write to register data */
      /* Find out the register */
      reg = MP_IOAPIC_READ (IOAPIC_REGSEL + 1);
      reg = MP_IOAPIC_READ (IOAPIC_REGSEL);
      reg &= 0xFFull;
      
      if (ioapic_reg_in_blacklist ((uint8) reg, (uint8) get_pcpu_id ())) {
        return 0;
      }
    }

    return 1;
  }

  return -1;
}

/*
 * This function process VM-Exit. If -1 is returned, monitor will panic.
 * To skip the exiting instruction, 0 should be returned. To re-execute
 * the exiting instruction, return 1. Notice though for different situations,
 * the definition of "exiting instruction" could be different (in terms of
 * whether the exit happens before executing the instruction or after).
 * Please refer to the manual for details.
 * 
 * Basically, returning 1 will cause the instruction pointed to by guest
 * EIP to be executed again and returning 0 will skip the instruction
 * pointed to by guest EIP on VM entry.
 */
int
vmx_process_exit (virtual_machine *vm, uint32 reason)
{
  bool need_ecx_fix = FALSE;

  switch (reason) {
    case 0x00 :
    {
      /* VM-Exit due to Exception and NMI */
      uint32 intinf = vmread (VMXENC_VM_EXIT_INTERRUPT_INFO);
      uint8 vec = intinf & 0xFF;
#if 0
      uint32 eip = vmread (VMXENC_GUEST_RIP);
      uint32 ercode = vmread (VMXENC_VM_EXIT_INTERRUPT_ERRCODE);

      logger_printf ("Exception/NMI:\n");
      logger_printf ("  VM_EXIT_INTERRUPT_INFO: 0x%X\n", intinf);
      logger_printf ("  VM_EXIT_INTERRUPT_ERRCODE: 0x%X\n", ercode);
      logger_printf ("  Guest EIP: 0x%X\n", eip);
#endif

      switch (vec) {
        case 0x01 :
          /* Debug Exception #DB. Quest use this for I/O partitioning purpose. */
          //logger_printf ("Debug Exception, debug reason (0x%X)\n", last_reason);
          switch (last_reason) {
            case 0x1E :
            {
              uint64 proc_msr = 0;
              /* Clear TF flag */
              vmwrite (vmread (VMXENC_GUEST_RFLAGS) & (~0x100ul), VMXENC_GUEST_RFLAGS);
              last_reason = 0;
              last_port = 0;
              proc_msr = vmread (VMXENC_PROCBASED_VM_EXEC_CTRLS);
              proc_msr |= (1ul << 25);
              vmwrite (proc_msr, VMXENC_PROCBASED_VM_EXEC_CTRLS);
              return 1;
            }
            case 0x30 :
            {
              /* EPT Violation */
              /* Clear TF flag */
              vmwrite (vmread (VMXENC_GUEST_RFLAGS) & (~0x100ul), VMXENC_GUEST_RFLAGS);
              set_ept_page_permission (last_gphys, last_permission);
              last_reason = 0;
              last_gphys = 0;
              last_permission = EPT_NO_ACCESS;
              return 1;
            }
            default :
              logger_printf ("Unknown reason triggered debug exception!\n");
          }
          break;
        default :
          logger_printf ("Unexpected Exception/NMI!\n");
      }
      return -1;
    }
    case 0x0A :
      /* We use CPUID as a way of intentional VM-Exit in Guest too */
      if (vm->guest_regs.eax == 0xFFFFFFFF) {
        vmx_process_hypercall (vm->guest_regs.ecx, vm);
        return 0;
      }
      if (vm->guest_regs.eax == 0x1) {
        need_ecx_fix = TRUE;
      }
      /* CPUID -- unconditional VM-EXIT -- perform in monitor */
      logger_printf ("VM: performing CPUID (0x%p, 0x%p) => ",
                     vm->guest_regs.eax, vm->guest_regs.ecx);
      cpuid (vm->guest_regs.eax, vm->guest_regs.ecx,
             &vm->guest_regs.eax, &vm->guest_regs.ebx,
             &vm->guest_regs.ecx, &vm->guest_regs.edx);
      logger_printf ("(0x%p, 0x%p, 0x%p, 0x%p)\n",
                     vm->guest_regs.eax, vm->guest_regs.ebx,
                     vm->guest_regs.ecx, vm->guest_regs.edx);
      if (need_ecx_fix) {
        /*
         * We have several hardware features disabled in guest kernel.
         * These include VM extension, TXT, XSAVE/XSTOR, OSXSAVE and
         * AVX. Some more features can be disabled but these seems
         * enough to maintain the consistency of our monitor and make
         * Linux boot.
         */
        vm->guest_regs.ecx &= ~(1ul << 5);   /* Clear VMX */
        vm->guest_regs.ecx &= ~(1ul << 6);   /* Clear SMX (TXT) */
#ifdef DISABLE_XCR
        vm->guest_regs.ecx &= ~(1ul << 26);  /* Clear XSAVE */
        vm->guest_regs.ecx &= ~(1ul << 27);  /* Clear OSXSAVE */
        vm->guest_regs.ecx &= ~(1ul << 28);  /* Clear AVX */
#endif
        logger_printf ("Fixed ECX: 0x%p\n", vm->guest_regs.ecx);
      }

      return 0;
    case 0x12 :
      /* VM Exit through vmcall instruction */
      vmx_process_hypercall (vm->guest_regs.ecx, vm);
      return 0;
    case 0x1E :
    {
      /* VM Exit due to IO instruction */
      uint32 qualif = vmread (VMXENC_EXIT_QUAL);
      uint64 proc_msr = 0;
      uint32 portn = (qualif >> 16) & 0xFFFF;
      int sflag = (qualif >> 4) & 0x01; /* 0 - Not string, 1 - String */
      int dir_flag = (qualif >> 3) & 0x1; /* 0 - Out, 1 - In */
      int rep_flag = (qualif >> 5) & 0x01; /* 0 - No rep, 1 - rep */
      static uint32 cur_config_addr = 0;
      static int deny_flag = 0;
      int bus = 0, slot = 0, func = 0;
      uint16 vendorID = 0, deviceID = 0;

#if 0
      int size = qualif & 0x7; /* 0 - 1B, 1 - 2B, 3 - 4B */
      int enc_flag = (qualif >> 6) & 0x01; /* 0 - DX, 1 - imm */
      int df_flag = (vmread (VMXENC_GUEST_RFLAGS) >> 10) & 0x01; /* 0 - CLD, 1 - STD */
      logger_printf ("IO instruction:\n");
      logger_printf ("  ");
      if (df_flag)
        logger_printf ("std; ");
      else
        logger_printf ("cld; ");
      if (rep_flag) logger_printf ("rep ");
      if (dir_flag) {
        logger_printf ("in");
      } else {
        logger_printf ("out");
      }
      if (sflag) logger_printf ("s");
      switch (size) {
        case 0 :
          logger_printf ("b\n");
          break;
        case 1 :
          logger_printf ("w\n");
          break;
        case 3 :
          logger_printf ("l\n");
          break;
      }
      if (enc_flag)
        logger_printf ("  Encoding using imm, port number is: 0x%X\n", portn);
      else
        logger_printf ("  Encoding using DX, port number is: 0x%X\n", portn);
      logger_printf ("  EAX=0x%X, EDX=0x%X, ECX=0x%X\n", vm->guest_regs.eax,
                     vm->guest_regs.edx, vm->guest_regs.ecx);
#endif

      /* Trying to access serial port */
      if ((portn >= serial_port1) && (portn <= (serial_port1 + 7))) {
        if (sflag == 0) {
          if ((portn == (serial_port1 + 5)) && (dir_flag == 1)) {
            /* Set line status to rescue polling drivers */
            vm->guest_regs.eax = 0x20;
          } else if (dir_flag == 1) {
            vm->guest_regs.eax = 0;
          }
        }
        return 0;
      }

      /* Trying to access PCI_CONFIG_ADDRESS */
      if (portn == PCI_CONFIG_ADDRESS) {
        if (sflag == 1) {
          logger_printf ("Write to PCI_CONFIG_ADDRESS with string instruction\n");
          return -1;
        }
        if (rep_flag == 1) {
          logger_printf ("Write to PCI_CONFIG_ADDRESS with rep refix\n");
          return -1;
        }
        if (dir_flag == 0) {
          /* Trying to do out on PCI_CONFIG_ADDRESS */
          cur_config_addr = vm->guest_regs.eax;
          bus = ((cur_config_addr >> 16) & 0xFF);
          slot = ((cur_config_addr >> 11) & 0x1F);
          func = ((cur_config_addr >> 8) & 0x07);
          vendorID = pci_read_word (pci_addr (bus, slot, func, 0x00));
          deviceID = pci_read_word (pci_addr (bus, slot, func, 0x02));
          if (dev_access_allowed (vendorID, deviceID)) {
            deny_flag = 0;
          } else {
            deny_flag = 1;
          }
        } else {
          logger_printf ("Guest reading PCI_CONFIG_ADDRESS!\n");
        }
        /* Allow access */
        goto allow_access;
      }

      /* Trying to access PCI_CONFIG_DATA */
      if (portn == PCI_CONFIG_DATA) {
        if (cur_config_addr == 0) {
          logger_printf ("Accessing PCI DATA without setting ADDRESS\n");
          return -1;
        }

        /* Do we need to block access? */
        if (deny_flag) {
          /* Need to block access */
          if ((dir_flag == 1) && (sflag == 0)) {
            /* If this is a non-string IN, fix EAX. */
            vm->guest_regs.eax = 0xFFFFFFFF;
          }
          /* If this is an OUT, OUTS, INS or IN/OUT with rep prefix, we do nothing. */
          return 0; /* Skip instruction */
        }

        goto allow_access;
      }

allow_access:
      /* Set TF flag in rflags for guest to enable single step debug */
      vmwrite (vmread (VMXENC_GUEST_RFLAGS) | 0x100, VMXENC_GUEST_RFLAGS);
      last_reason = 0x1E;
      last_port = portn;
      /* Allow access to port */
      proc_msr = vmread (VMXENC_PROCBASED_VM_EXEC_CTRLS);
      proc_msr &= (~(1ul << 25));
      vmwrite (proc_msr, VMXENC_PROCBASED_VM_EXEC_CTRLS);

      return 1;
    }
    case 0x1F :
      /* RDMSR / WRMSR -- conditional on MSR bitmap -- else perform in monitor */
      logger_printf ("VM: use MSR bitmaps=%d MSR_BITMAPS=0x%p bitmap[0x%X]=%d\n",
                     !!(vmread (VMXENC_PROCBASED_VM_EXEC_CTRLS) & (1<<28)),
                     vmread (VMXENC_MSR_BITMAPS),
                     vm->guest_regs.ecx,
                     !!(BITMAP_TST (msr_bitmaps, vm->guest_regs.ecx)));
      logger_printf ("VM: performing RDMSR (0x%p) => ", vm->guest_regs.ecx);
      asm volatile ("rdmsr"
                    :"=d" (vm->guest_regs.edx),
                     "=a" (vm->guest_regs.eax)
                    :"c" (vm->guest_regs.ecx));
      logger_printf ("0x%p %p\n", vm->guest_regs.edx, vm->guest_regs.eax);
      return 0;
    case 0x20 :
      /* RDMSR / WRMSR -- conditional on MSR bitmap -- else perform in monitor */
      logger_printf ("VM: use MSR bitmaps=%d MSR_BITMAPS=0x%p bitmap[0x%X]=%d\n",
                     !!(vmread (VMXENC_PROCBASED_VM_EXEC_CTRLS) & (1<<28)),
                     vmread (VMXENC_MSR_BITMAPS),
                     vm->guest_regs.ecx,
                     !!(BITMAP_TST (msr_bitmaps, vm->guest_regs.ecx)));
      logger_printf ("VM: performing WRMSR (0x%p %p,0x%p)\n",
                      vm->guest_regs.edx, vm->guest_regs.eax, vm->guest_regs.ecx);
      asm volatile ("wrmsr":
                    :"d" (vm->guest_regs.edx),
                     "a" (vm->guest_regs.eax),
                     "c" (vm->guest_regs.ecx));
      return 0;
#ifdef VMX_EPT
    case 0x30 :
    {
      uint32 gphys = vmread (VMXENC_GUEST_PHYS_ADDR);

#if 0
      uint32 glinear = vmread (VMXENC_GUEST_LINEAR_ADDR);
      uint32 qualif = vmread (VMXENC_EXIT_QUAL);

      /* EPT Violation */
      logger_printf ("EPT violation:\n");
      logger_printf ("  Guest Physical Address is: 0x%X\n", gphys);
      logger_printf ("  Guest Linear Address is: 0x%X\n", glinear);

      if (!(qualif & (0x1 << 0x7))) {
        logger_printf ("  Warning: Guest linear address is not valid!\n");
        logger_printf ("  Attempt to load guest PDPTE through MOV CR\n");
      } else {
        /* Bit 7 is set */
        if (qualif & (0x1 << 0x8)) {
          logger_printf ("  Violation happened during normal address translation\n");
        } else {
          logger_printf ("  Violation happened during page walk\n");
        }
      }

      logger_printf ("  Caused by:");
      if (qualif & (0x1 << 0x0)) logger_printf (" READ");
      if (qualif & (0x1 << 0x1)) logger_printf (" WRITE");
      if (qualif & (0x1 << 0x2)) logger_printf (" INST FETCH");
      logger_printf ("\n");

      logger_printf ("  Guest Physical Address Permission:");
      if (qualif & (0x1 << 0x3)) logger_printf (" READABLE");
      if (qualif & (0x1 << 0x4)) logger_printf (" WRITEABLE");
      if (qualif & (0x1 << 0x5)) logger_printf (" EXECUTABLE");
      logger_printf ("\n");

      if (qualif & (0x1 << 0xC)) logger_printf ("  NMI unblocking due to IRET\n");

      logger_printf ("  Guest Registers:\n");
      logger_printf ("    EAX=0x%X, EBX=0x%X\n", vm->guest_regs.eax, vm->guest_regs.ebx);
      logger_printf ("    ECX=0x%X, EDX=0x%X\n", vm->guest_regs.ecx, vm->guest_regs.edx);
      logger_printf ("    EIP=0x%X\n", vmread (VMXENC_GUEST_RIP));
#endif

      switch (gphys_access_type (gphys)) {
        case 0 :
          return 0;
        case 1 :
          /* Set TF flag in rflags for guest to enable single step debug */
          vmwrite (vmread (VMXENC_GUEST_RFLAGS) | 0x100, VMXENC_GUEST_RFLAGS);
          last_reason = 0x30;
          last_gphys = gphys;
          last_permission = EPT_READ_ACCESS;
          set_ept_page_permission (gphys, EPT_ALL_ACCESS);
          return 1;
        default :
          break;
      }

      return -1;
    }
    case 0x31 :
      /* EPT misconfiguration */
      logger_printf ("EPT misconfiguration:\n  VMXENC_EPT_PTR=0x%p\n",
                     vmread (VMXENC_EPT_PTR));
      return -1;
#endif
    case 0x37 :
    {
      logger_printf ("Guest trying to execute XSETBV instruction\n");
#ifdef DISABLE_XCR
      return -1;
#else
      uint32 cr4 = 0;
      /* Trying to execute XSETBV instruction */
      logger_printf ("EDX=0x%X, EAX=0x%X, ECX=0x%X\n",
                     vm->guest_regs.edx, vm->guest_regs.eax,
                     vm->guest_regs.ecx);
      asm volatile ("movl %%cr4, %0\r\n"
                    : "=r" (cr4):);
      logger_printf ("CR4=0x%X\n", cr4);
      logger_printf ("Guest CR4=0x%X\n", vmread (VMXENC_GUEST_CR4));
      cr4 = cr4 | (0x1 << 18);
      asm volatile ("movl %0, %%cr4\r\n"
                    :: "r" (cr4));
      logger_printf ("New CR4=0x%X\n", cr4);
      /* XSETBV writes contents of EDX:EAX into XCR[n]. */
      /* n is specified by ECX. Currently, only XCR0 is available. */
      asm volatile ("xsetbv\r\n":
                    : "d" (vm->guest_regs.edx),
                      "a" (vm->guest_regs.eax),
                      "c" (vm->guest_regs.ecx));
      return 0;
#endif
    }
    case 0x1C :
      /* MOV to control register */
      logger_printf ("GUEST-STATE: EAX=0x%X, EBX=0x%X, ECX=0x%X, EDX=0x%X\n",
                     vm->guest_regs.eax, vm->guest_regs.ebx, vm->guest_regs.ecx,
                     vm->guest_regs.edx);
      return 0;
    default :
#if DEBUG_VMX > 1
    {
      uint32 inslen = vmread (VMXENC_VM_EXIT_INSTR_LEN);
      uint32 intinf = vmread (VMXENC_VM_EXIT_INTERRUPT_INFO);
      uint32 qualif = vmread (VMXENC_EXIT_QUAL);
      uint32 ercode = vmread (VMXENC_VM_EXIT_INTERRUPT_ERRCODE);
      uint32 insinf = vmread (VMXENC_VM_EXIT_INSTR_INFO);
      uint32 exit_eflags = 0;
      asm volatile ("pushfl; pop %0\n":"=r" (exit_eflags):);

      logger_printf ("VM-EXIT: %s\n  reason=%.8X qualif=%.8X\n  intinf=%.8X \
                      ercode=%.8X\n  inslen=%.8X insinf=%.8X\n",
                     (reason < VMX_NUM_EXIT_REASONS ?
                      vm_exit_reasons[reason] : "invalid exit-reason"),
                     reason, qualif, intinf, ercode, inslen, insinf);
      logger_printf ("VM-EXIT: EFLAGS=0x%.8X\n", exit_eflags);
      vmx_vm_exit_reason ();
    }
#endif
      return -1;     
  }
}

static void
setup_io_bitmaps ()
{
#ifdef USE_LINUX_SANDBOX
  int i = 0;

  if (get_pcpu_id () == LINUX_SANDBOX) {
    /* Restrict serial port access for Linux front end */
    IOBITMAP_SET (io_bitmaps, serial_port1 + 0);
    IOBITMAP_SET (io_bitmaps, serial_port1 + 1);
    IOBITMAP_SET (io_bitmaps, serial_port1 + 2);
    IOBITMAP_SET (io_bitmaps, serial_port1 + 3);
    IOBITMAP_SET (io_bitmaps, serial_port1 + 4);
    IOBITMAP_SET (io_bitmaps, serial_port1 + 5);
    IOBITMAP_SET (io_bitmaps, serial_port1 + 6);
    IOBITMAP_SET (io_bitmaps, serial_port1 + 7);

    /* Restrict access to Pololu serial ports */
    for (i = 0; i < NUM_POLOLU_PORTS; i++) {
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 0);
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 1);
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 2);
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 3);
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 4);
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 5);
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 6);
      IOBITMAP_SET (io_bitmaps, pololu_ports[i] + 7);
    }
  }
#endif
  /* Trap on access to PCI configuration space address port */
  IOBITMAP_SET (io_bitmaps, PCI_CONFIG_ADDRESS);
  /* Trap on access to PCI configuration space data port */
  IOBITMAP_SET (io_bitmaps, PCI_CONFIG_DATA);
  return;
}

static void
vmx_host_state_save ()
{
  u64 proc_msr = 0;
  uint32 cr = 0;
  uint16 fs = 0;
  uint32 phys_io_bitmaps = 0;

  phys_io_bitmaps = alloc_phys_frames (2);

  if (phys_io_bitmaps != -1) {
    io_bitmaps = map_contiguous_virtual_pages (phys_io_bitmaps | 3, 2);
    if (!io_bitmaps) free_phys_frames (phys_io_bitmaps, 2);
  }

  if (io_bitmaps) {
    memset (io_bitmaps, 0, 0x2000);
  } else {
    com1_printf ("WARNING: Could not allocate memory for IO Bitmaps!!!\n");
  }

#ifdef EXCEPTION_EXIT
  vmwrite (~0, VMXENC_EXCEPTION_BITMAP);
#else
  /*
   * Force Debug Exception to trap into monitor. All others will be sent directly to
   * the guest IDT. #DB (Vector 1) is used to do single-step debugging. Quest-V uses
   * this to support resource (mostly I/O) partitioning. For details, see VM Exit
   * handling in vm/vmx.c.
   */
  vmwrite ((0x1ul << 1), VMXENC_EXCEPTION_BITMAP);
#endif
  vmwrite (0, VMXENC_PAGE_FAULT_ERRCODE_MASK);
  vmwrite (0, VMXENC_PAGE_FAULT_ERRCODE_MATCH);
  vmwrite (0, VMXENC_CR0_GUEST_HOST_MASK); /* all bits "owned" by guest */
  vmwrite (0, VMXENC_CR0_READ_SHADOW);
#ifdef DISABLE_XCR
  vmwrite (0x42000, VMXENC_CR4_GUEST_HOST_MASK); /* VMXE and OSXSAVE "owned" by host */
#else
  vmwrite (0x2000, VMXENC_CR4_GUEST_HOST_MASK); /* VMXE "owned" by host */
#endif
  vmwrite (0, VMXENC_CR4_READ_SHADOW);

  vmwrite (rdmsr (IA32_VMX_PINBASED_CTLS), VMXENC_PINBASED_VM_EXEC_CTRLS);
  proc_msr = rdmsr (IA32_VMX_PROCBASED_CTLS) | rdmsr (IA32_VMX_TRUE_PROCBASED_CTLS);
  /* allow CR3 load/store, RDTSC, RDPMC */
  proc_msr &= ~((1 << 15) | (1 << 16) | (1 << 12) | (1 << 11));
  /* use MSR bitmaps */
  proc_msr |= (1 << 28);
  /* use IO bitmaps */
  if (io_bitmaps) {
    proc_msr |= (1 << 25);
    setup_io_bitmaps ();
    vmwrite ((uint32) get_phys_addr (io_bitmaps), VMXENC_IO_BITMAP_A);
    vmwrite ((uint32) get_phys_addr (((uint8 *) io_bitmaps) + 0x1000), VMXENC_IO_BITMAP_B);
  }
  /* secondary controls */
  proc_msr |= (1 << 31);
  vmwrite (proc_msr, VMXENC_PROCBASED_VM_EXEC_CTRLS);
  vmwrite (0, VMXENC_PROCBASED_VM_EXEC_CTRLS2);
  vmwrite (0, VMXENC_CR3_TARGET_COUNT);
  vmwrite (rdmsr (IA32_VMX_EXIT_CTLS), VMXENC_VM_EXIT_CTRLS);
  vmwrite (0, VMXENC_VM_EXIT_MSR_STORE_COUNT);
  vmwrite (0, VMXENC_VM_EXIT_MSR_LOAD_COUNT);
  /* Do not load debug controls. We have to check the capability regs before doing this. */
  vmwrite (rdmsr (IA32_VMX_ENTRY_CTLS) & (~(1 << 2)), VMXENC_VM_ENTRY_CTRLS);
  vmwrite (0, VMXENC_VM_ENTRY_MSR_LOAD_COUNT);
  vmwrite (0, VMXENC_MSR_BITMAPS_HI);
  /* clear MSR bitmaps */
  memset (msr_bitmaps, 0, 0x1000);
  vmwrite ((u32) get_phys_addr (msr_bitmaps), VMXENC_MSR_BITMAPS);
  asm volatile ("movl %%cr0, %0":"=r" (cr));
  vmwrite (cr, VMXENC_HOST_CR0);
  asm volatile ("movl %%cr3, %0":"=r" (cr));
  vmwrite (cr, VMXENC_HOST_CR3);
  asm volatile ("movl %%cr4, %0":"=r" (cr));
  vmwrite (cr, VMXENC_HOST_CR4);
  vmwrite (0x08, VMXENC_HOST_CS_SEL);
  vmwrite (0x10, VMXENC_HOST_SS_SEL);
  vmwrite (0x10, VMXENC_HOST_DS_SEL);
  vmwrite (0x10, VMXENC_HOST_ES_SEL);
  asm volatile ("movw %%fs, %0":"=r" (fs));
  vmwrite (fs, VMXENC_HOST_FS_SEL);
  vmwrite (0x10, VMXENC_HOST_GS_SEL);
  vmwrite (hw_str (), VMXENC_HOST_TR_SEL);
  vmwrite ((uint32) lookup_TSS (hw_str ()), VMXENC_HOST_TR_BASE);
  vmwrite ((uint32) lookup_GDT_selector (fs), VMXENC_HOST_FS_BASE);
  vmwrite ((uint32) lookup_GDT_selector (0x10), VMXENC_HOST_GS_BASE);
  vmwrite ((uint32) sgdtr (), VMXENC_HOST_GDTR_BASE);
  vmwrite ((uint32) sidtr (), VMXENC_HOST_IDTR_BASE);
  vmwrite (0, VMXENC_VM_ENTRY_INTERRUPT_INFO);
  vmwrite (rdmsr (IA32_SYSENTER_CS), VMXENC_HOST_IA32_SYSENTER_CS);
  vmwrite (rdmsr (IA32_SYSENTER_ESP), VMXENC_HOST_IA32_SYSENTER_ESP);
  vmwrite (rdmsr (IA32_SYSENTER_EIP), VMXENC_HOST_IA32_SYSENTER_EIP);
  vmwrite (vmread (VMXENC_GUEST_CS_ACCESS) | 0x1, VMXENC_GUEST_CS_ACCESS);

  return;
}

int
vmx_start_monitor (virtual_machine *vm)
{
  uint32 phys_id = (uint32)LAPIC_get_physical_ID ();
  u64 start, finish;
  uint32 eip = 0, err = 0, state = 0;
  int process_ret = 0;

  if (!vm->loaded || vm->current_cpu != phys_id)
    goto not_loaded;

  /* Save Host State */
  vmx_host_state_save ();

  u32 cpu = get_pcpu_id ();

  /* 
   * vmx_vm_fork will essentially do a VM fork. After this point
   * we will be in a new sandbox kernel and with shared memory enabled.
   */
  vmx_vm_fork (cpu);

#ifdef VMX_EPT
  vmx_init_ept (cpu);
#endif

  logger_printf ("vmx_start_monitor: GUEST-STATE: RIP=0x%p RSP=0x%p RBP=0x%p CR3=0x%p\n",
                 vmread (VMXENC_GUEST_RIP), vmread (VMXENC_GUEST_RSP), vm->guest_regs.ebp,
                 vmread (VMXENC_GUEST_CR3));

//#define VMX_DUMP_GUEST_STATE
//#define VMX_DUMP_CONTROLS

#ifdef VMX_DUMP_CONTROLS
  logger_printf ("-------------------\n");
  logger_printf ("|VMX CONTROLS DUMP|\n");
  logger_printf ("-------------------\n");
  logger_printf ("VMXENC_PINBASED_VM_EXEC_CTRLS=0x%X\n",
                 vmread (VMXENC_PINBASED_VM_EXEC_CTRLS));
  logger_printf ("VMXENC_PROCBASED_VM_EXEC_CTRLS=0x%X\n",
                 vmread (VMXENC_PROCBASED_VM_EXEC_CTRLS));
  logger_printf ("VMXENC_PROCBASED_VM_EXEC_CTRLS2=0x%X\n",
                 vmread (VMXENC_PROCBASED_VM_EXEC_CTRLS2));
  logger_printf ("VMXENC_CR3_TARGET_COUNT=0x%X\n", vmread (VMXENC_CR3_TARGET_COUNT));
  logger_printf ("VMXENC_VM_EXIT_CTRLS=0x%X\n", vmread (VMXENC_VM_EXIT_CTRLS));
  logger_printf ("VMXENC_VM_EXIT_MSR_STORE_COUNT=0x%X\n",
                 vmread (VMXENC_VM_EXIT_MSR_STORE_COUNT));
  logger_printf ("VMXENC_VM_EXIT_MSR_LOAD_COUNT=0x%X\n",
                 vmread (VMXENC_VM_EXIT_MSR_LOAD_COUNT));
  logger_printf ("VMXENC_VM_ENTRY_CTRLS=0x%X\n", vmread (VMXENC_VM_ENTRY_CTRLS));
  logger_printf ("VMXENC_VM_ENTRY_MSR_LOAD_COUNT=0x%X\n",
                 vmread (VMXENC_VM_ENTRY_MSR_LOAD_COUNT));
  logger_printf ("------------------------\n");
  logger_printf ("|VMX CONTROLS DUMP DONE|\n");
  logger_printf ("------------------------\n");
#endif

#ifdef VMX_DUMP_GUEST_STATE
  logger_printf ("----------------------\n");
  logger_printf ("|VMX GUEST STATE DUMP|\n");
  logger_printf ("----------------------\n");
  logger_printf ("VMXENC_GUEST_RFLAGS=0x%X\n", vmread (VMXENC_GUEST_RFLAGS));
  logger_printf ("VMXENC_GUEST_CR0=0x%X\n", vmread (VMXENC_GUEST_CR0));
  logger_printf ("VMXENC_GUEST_CR3=0x%X\n", vmread (VMXENC_GUEST_CR3));
  logger_printf ("VMXENC_GUEST_CR4=0x%X\n", vmread (VMXENC_GUEST_CR4));
  logger_printf ("VMXENC_GUEST_DR7=0x%X\n", vmread (VMXENC_GUEST_DR7));
  logger_printf ("VMXENC_GUEST_CS_SEL=0x%X\n", vmread (VMXENC_GUEST_CS_SEL));
  logger_printf ("VMXENC_GUEST_CS_BASE=0x%X\n", vmread (VMXENC_GUEST_CS_BASE));
  logger_printf ("VMXENC_GUEST_CS_LIMIT=0x%X\n", vmread (VMXENC_GUEST_CS_LIMIT));
  logger_printf ("VMXENC_GUEST_CS_ACCESS=0x%X\n", vmread (VMXENC_GUEST_CS_ACCESS));
  logger_printf ("VMXENC_GUEST_SS_SEL=0x%X\n", vmread (VMXENC_GUEST_SS_SEL));
  logger_printf ("VMXENC_GUEST_SS_BASE=0x%X\n", vmread (VMXENC_GUEST_SS_BASE));
  logger_printf ("VMXENC_GUEST_SS_LIMIT=0x%X\n", vmread (VMXENC_GUEST_SS_LIMIT));
  logger_printf ("VMXENC_GUEST_SS_ACCESS=0x%X\n", vmread (VMXENC_GUEST_SS_ACCESS));
  logger_printf ("VMXENC_GUEST_DS_SEL=0x%X\n", vmread (VMXENC_GUEST_DS_SEL));
  logger_printf ("VMXENC_GUEST_DS_BASE=0x%X\n", vmread (VMXENC_GUEST_DS_BASE));
  logger_printf ("VMXENC_GUEST_DS_LIMIT=0x%X\n", vmread (VMXENC_GUEST_DS_LIMIT));
  logger_printf ("VMXENC_GUEST_DS_ACCESS=0x%X\n", vmread (VMXENC_GUEST_DS_ACCESS));
  logger_printf ("VMXENC_GUEST_ES_SEL=0x%X\n", vmread (VMXENC_GUEST_ES_SEL));
  logger_printf ("VMXENC_GUEST_ES_BASE=0x%X\n", vmread (VMXENC_GUEST_ES_BASE));
  logger_printf ("VMXENC_GUEST_ES_LIMIT=0x%X\n", vmread (VMXENC_GUEST_ES_LIMIT));
  logger_printf ("VMXENC_GUEST_ES_ACCESS=0x%X\n", vmread (VMXENC_GUEST_ES_ACCESS));
  logger_printf ("VMXENC_GUEST_FS_SEL=0x%X\n", vmread (VMXENC_GUEST_FS_SEL));
  logger_printf ("VMXENC_GUEST_FS_BASE=0x%X\n", vmread (VMXENC_GUEST_FS_BASE));
  logger_printf ("VMXENC_GUEST_FS_LIMIT=0x%X\n", vmread (VMXENC_GUEST_FS_LIMIT));
  logger_printf ("VMXENC_GUEST_FS_ACCESS=0x%X\n", vmread (VMXENC_GUEST_FS_ACCESS));
  logger_printf ("VMXENC_GUEST_GS_SEL=0x%X\n", vmread (VMXENC_GUEST_GS_SEL));
  logger_printf ("VMXENC_GUEST_GS_BASE=0x%X\n", vmread (VMXENC_GUEST_GS_BASE));
  logger_printf ("VMXENC_GUEST_GS_LIMIT=0x%X\n", vmread (VMXENC_GUEST_GS_LIMIT));
  logger_printf ("VMXENC_GUEST_GS_ACCESS=0x%X\n", vmread (VMXENC_GUEST_GS_ACCESS));
  logger_printf ("VMXENC_GUEST_TR_SEL=0x%X\n", vmread (VMXENC_GUEST_TR_SEL));
  logger_printf ("VMXENC_GUEST_TR_BASE=0x%X\n", vmread (VMXENC_GUEST_TR_BASE));
  logger_printf ("VMXENC_GUEST_TR_LIMIT=0x%X\n", vmread (VMXENC_GUEST_TR_LIMIT));
  logger_printf ("VMXENC_GUEST_TR_ACCESS=0x%X\n", vmread (VMXENC_GUEST_TR_ACCESS));
  logger_printf ("VMXENC_GUEST_LDTR_SEL=0x%X\n", vmread (VMXENC_GUEST_LDTR_SEL));
  logger_printf ("VMXENC_GUEST_LDTR_BASE=0x%X\n", vmread (VMXENC_GUEST_LDTR_BASE));
  logger_printf ("VMXENC_GUEST_LDTR_LIMIT=0x%X\n", vmread (VMXENC_GUEST_LDTR_LIMIT));
  logger_printf ("VMXENC_GUEST_LDTR_ACCESS=0x%X\n", vmread (VMXENC_GUEST_LDTR_ACCESS));
  logger_printf ("VMXENC_GUEST_GDTR_BASE=0x%X\n", vmread (VMXENC_GUEST_GDTR_BASE));
  logger_printf ("VMXENC_GUEST_GDTR_LIMIT=0x%X\n", vmread (VMXENC_GUEST_GDTR_LIMIT));
  logger_printf ("VMXENC_GUEST_IDTR_BASE=0x%X\n", vmread (VMXENC_GUEST_IDTR_BASE));
  logger_printf ("VMXENC_GUEST_IDTR_LIMIT=0x%X\n", vmread (VMXENC_GUEST_IDTR_LIMIT));
  logger_printf ("VMXENC_GUEST_RIP=0x%X\n", vmread (VMXENC_GUEST_RIP));
  logger_printf ("VMXENC_GUEST_RSP=0x%X\n", vmread (VMXENC_GUEST_RSP));
  logger_printf ("VMXENC_GUEST_IA32_SYSENTER_CS=0x%X\n",
                 vmread (VMXENC_GUEST_IA32_SYSENTER_CS));
  logger_printf ("VMXENC_GUEST_IA32_SYSENTER_ESP=0x%X\n",
                 vmread (VMXENC_GUEST_IA32_SYSENTER_ESP));
  logger_printf ("VMXENC_GUEST_IA32_SYSENTER_EIP=0x%X\n",
                 vmread (VMXENC_GUEST_IA32_SYSENTER_EIP));
  logger_printf ("VMXENC_VMCS_LINK_PTR_HIGH=0x%X\n", vmread (VMXENC_VMCS_LINK_PTR));
  logger_printf ("VMXENC_VMCS_LINK_PTR_LOW=0x%X\n", vmread (VMXENC_VMCS_LINK_PTR + 1));
  logger_printf ("VMXENC_GUEST_PENDING_DEBUG_EXCEPTIONS=0x%X\n",
                 vmread (VMXENC_GUEST_PENDING_DEBUG_EXCEPTIONS));
  logger_printf ("VMXENC_GUEST_ACTIVITY=0x%X\n", vmread (VMXENC_GUEST_ACTIVITY));
  logger_printf ("VMXENC_GUEST_INTERRUPTIBILITY=0x%X\n",
                 vmread (VMXENC_GUEST_INTERRUPTIBILITY));
  logger_printf ("VMXENC_EXCEPTION_BITMAP=0x%X\n", vmread (VMXENC_EXCEPTION_BITMAP));
  logger_printf ("VMXENC_PAGE_FAULT_ERRCODE_MASK=0x%X\n",
                 vmread (VMXENC_PAGE_FAULT_ERRCODE_MASK));
  logger_printf ("VMXENC_PAGE_FAULT_ERRCODE_MATCH=0x%X\n",
                 vmread (VMXENC_PAGE_FAULT_ERRCODE_MATCH));
  logger_printf ("VMXENC_CR0_GUEST_HOST_MASK=0x%X\n", vmread (VMXENC_CR0_GUEST_HOST_MASK));
  logger_printf ("VMXENC_CR0_READ_SHADOW=0x%X\n", vmread (VMXENC_CR0_READ_SHADOW));
  logger_printf ("---------------------------\n");
  logger_printf ("|VMX GUEST STATE DUMP DONE|\n");
  logger_printf ("---------------------------\n");
#endif

 enter:
  RDTSC (start);

  /* clobber-list is not necessary here because "pusha" below saves
   * HOST registers */
  asm volatile (/* save HOST registers on stack and ESP in VMCS */
                "pusha\n"
                "vmwrite %%esp, %2\n"
                /* Do trick to get current EIP and differentiate between the first
                 * and second time this code is invoked. */
                "call 1f\n"
                /* On VM-EXIT, resume Host here: */
                "pusha\n"       /* quickly snapshot guest registers to stack */
                "addl $0x20, %%esp\n"
                "popa\n"        /* temporarily restore host registers */
                "lea %1, %%edi\n"
                "movl $8, %%ecx\n"
                "lea -0x40(%%esp), %%esi\n"
                "cld; rep movsd\n" /* save guest registers to memory */
                "subl $0x20, %%esp\n"
                "popa\n"        /* permanently restore host registers */
                "xor %0, %0\n"
                "jmp 2f\n"
                "1:\n"
                "pop %0\n"
                "2:\n"
                :"=r" (eip):"m" (vm->guest_regs),"r" (VMXENC_HOST_RSP));

  /* VM-ENTER */
  if (eip) {
    DLOG ("Entering VM! host EIP=0x%p", eip);
    vmwrite (eip, VMXENC_HOST_RIP);
    /* Allow shared driver access in sandboxes */
    shared_driver_available = TRUE;
    if (vm->launched) {
      asm volatile ("movl $1, %0\n"
                    "movl $2, %1\n"
                    /* Restore Guest registers using POPA */
                    "subl $0x20, %%esp\n"
                    "movl %%esp, %%edi\n"
                    "cld; rep movsd\n"
                    "popa\n"
                    "vmresume"
                    :"=m" (vm->launched), "=m"(state)
                    :"c" (8), "S" (&vm->guest_regs):"edi","cc","memory");
    } else {
      asm volatile ("movl $1, %0\n"
                    "movl $1, %1\n"
                    /* Restore Guest registers using POPA */
                    "subl $0x20, %%esp\n"
                    "movl %%esp, %%edi\n"
                    "cld; rep movsd\n"
                    "popa\n"
                    "vmlaunch"
                    :"=m" (vm->launched), "=m"(state)
                    :"c" (8), "S" (&vm->guest_regs):"edi","cc","memory");
    }

    shared_driver_available = FALSE;

    /* Must check if CF=1 or ZF=1 before doing anything else.
     * However, ESP is wiped out.  To restore stack requires a VMREAD.
     * However that would clobber flags.  Therefore, we must check
     * condition codes using "JBE" first.  Then we can restore stack,
     * and also host registers. */

    /* This may be unnecessary, should not reach this point except on error. */
    asm volatile ("xorl %%edi, %%edi; jbe 1f; jmp 2f\n"
                  "1: movl $1, %%edi\n"
                  "2: vmread %1, %%esp; pushl %%edi; addl $4, %%esp\n"
                  "popa\n"
                  /* alt. could modify EDI on stack.. oh well. */
                  "subl $0x24, %%esp\npopl %%edi\naddl $0x20, %%esp"
                  :"=D" (err)
                  :"r" (VMXENC_HOST_RSP));
    if (err) {
#if DEBUG_VMX > 1
      uint32 error = vmread (VMXENC_VM_INSTR_ERROR);
#endif
      uint32 reason = vmread (VMXENC_EXIT_REASON);

      if (state == 1)
        /* Failure to VMLAUNCH */
        vm->launched = FALSE;

#if DEBUG_VMX > 1
      logger_printf ("VM-ENTRY: %d error: %.8X (%s)\n  reason: %.8X qual: %.8X\n",
                     err,
                     error,
                     (error < VMX_NUM_INSTR_ERRORS ? vm_instruction_errors[error] : "n/a"),
                     reason,
                     vmread (VMXENC_EXIT_QUAL));
#endif
      if (reason & 0x80000000) {
#if DEBUG_VMX > 0
        logger_printf ("  VM-ENTRY failure, code: %d\n", reason & 0xFF);
#endif
      }
      goto abort_vmentry;
    }
  }

  if (!eip) {
    /* VM-exited */
    uint32 reason = vmread (VMXENC_EXIT_REASON);
    uint32 inslen = vmread (VMXENC_VM_EXIT_INSTR_LEN);

#if DEBUG_VMX > 2
    uint32 intinf = vmread (VMXENC_VM_EXIT_INTERRUPT_INFO);
    uint32 qualif = vmread (VMXENC_EXIT_QUAL);
    uint32 ercode = vmread (VMXENC_VM_EXIT_INTERRUPT_ERRCODE);
    uint32 insinf = vmread (VMXENC_VM_EXIT_INSTR_INFO);
#endif

    /* Cannot use shared driver in monitor */
    shared_driver_available = FALSE;

    if (reason & (1 << 31)) {
      /* VM-exit was due to failure during checking of Guest state
       * during VM-entry */
      reason &= ~(1 << 31);
      if (state == 1)
        /* Failure to VMLAUNCH */
        vm->launched = FALSE;
    }

    RDTSC (finish);

#if DEBUG_VMX > 2
    logger_printf ("VM-EXIT: %s\n  reason=%.8X qualif=%.8X\n  intinf=%.8X ercode=%.8X\n  inslen=%.8X insinf=%.8X\n  guestphys=0x%llX guestlinear=0x%llX\n  cycles=0x%llX cpu=%d\n",
                   (reason < VMX_NUM_EXIT_REASONS ?
                    vm_exit_reasons[reason] : "invalid exit-reason"),
                   reason, qualif, intinf, ercode, inslen, insinf,
                   (u64) vmread (VMXENC_GUEST_PHYS_ADDR),
                   (u64) vmread (VMXENC_GUEST_LINEAR_ADDR),
                   finish - start, get_pcpu_id ());
    vmx_vm_exit_reason ();
    u32 rip = vmread (VMXENC_GUEST_RIP), rsp = vmread (VMXENC_GUEST_RSP);
    logger_printf ("VM-EXIT: GUEST-STATE: RIP=0x%p RSP=0x%p\n", rip, rsp);
    logger_printf ("VM-EXIT: GUEST-STATE: FLAGS=0x%p CR0=0x%p CR3=0x%p CR4=0x%p\n",
                   vmread (VMXENC_GUEST_RFLAGS),
                   vmread (VMXENC_GUEST_CR0),
                   vmread (VMXENC_GUEST_CR3),
                   vmread (VMXENC_GUEST_CR4));
#define SHOWSEG(seg) do {                                               \
      logger_printf ("VM-EXIT: GUEST-STATE: %s=0x%.02X base=0x%p limit=0x%p access=0x%p\n", \
                     __stringify (seg),                                 \
                     vmread (VMXENC_GUEST_##seg##_SEL),                 \
                     vmread (VMXENC_GUEST_##seg##_BASE),                \
                     vmread (VMXENC_GUEST_##seg##_LIMIT),               \
                     vmread (VMXENC_GUEST_##seg##_ACCESS)               \
                     );                                                 \
    } while (0)
#define SHOWDTR(seg) do {                                               \
      logger_printf ("VM-EXIT: GUEST-STATE: %s base=0x%p limit=0x%p\n", \
                     __stringify (seg),                                 \
                     vmread (VMXENC_GUEST_##seg##_BASE),                \
                     vmread (VMXENC_GUEST_##seg##_LIMIT)                \
                     );                                                 \
    } while (0)

    SHOWSEG (CS);
    SHOWSEG (SS);
    SHOWSEG (DS);
    SHOWSEG (ES);
    SHOWSEG (FS);
    SHOWSEG (GS);
    SHOWSEG (TR);
    SHOWSEG (LDTR);
    SHOWDTR (GDTR);
    SHOWDTR (IDTR);

#endif

    process_ret = vmx_process_exit (vm, reason);

    if (process_ret == 0) {
      /* Return to guest, skip exiting instruction. */
      vmwrite (vmread (VMXENC_GUEST_RIP) + inslen, VMXENC_GUEST_RIP); /* skip instruction */
      goto enter;
    } else if (process_ret == 1) {
      /* Return to guest, repeat exiting instruction. */
      vmwrite (vmread (VMXENC_GUEST_RIP), VMXENC_GUEST_RIP); /* skip instruction */
      goto enter;
    }
  }

  DLOG ("start_VM: return 0 -- giving up on virtual machine (cpu#%d)", get_pcpu_id ());
  crash_debug ("stack is probably corrupt now");
  /* control could be resumed where the VM failed.  maybe do this later. */

  return 0;
 abort_vmentry:
 not_loaded:
  return -1;
}

/* start VM guest with state derived from host state */
int
vmx_enter_guest (virtual_machine *vm)
{
  u32 guest_eip = 0, esp, ebp, cr3;
  u32 hyperstack_frame = alloc_phys_frame ();
  if (hyperstack_frame == (u32) -1) return -1;
  u32 *hyperstack = map_virtual_page (hyperstack_frame | 3);
  if (hyperstack == 0) return -1;

  asm volatile ("call 1f\n"
                /* RESUME POINT */
                "xorl %0, %0\n"
                "jmp 2f\n"
                "1: pop %0; movl %%esp, %1\n"
                "2:":"=r" (guest_eip), "=r" (esp));

  if (guest_eip == 0) {
    /* inside VM  */
    asm volatile ("movl %%esp, %0; movl %%ebp, %1":"=r" (esp), "=r" (ebp));
    asm volatile ("movl %%cr3, %0":"=r" (cr3));
    DLOG ("vmx_enter_guest: entry success ESP=0x%p EBP=0x%p CR3=0x%p", esp, ebp, cr3);
    //dump_page ((u8 *) (esp & (~0xFFF)));

#if 1
    uint32 cpu = get_pcpu_id ();
    switch (cpu) {
      case 0 :
        print ("Welcome to Quest-V Sandbox 0!\n");
        break;
      case 1 :
        print ("Welcome to Quest-V Sandbox 1!\n");
        break;
      case 2 :
        print ("Welcome to Quest-V Sandbox 2!\n");
        break;
      case 3 :
        print ("Welcome to Quest-V Sandbox 3!\n");
        break;
      default:
        print ("Unknown Sandbox!\n");
    }
#endif

    return 0;
  }

  /* save general registers for guest */
  asm volatile ("pusha; movl %%esp, %%esi; movl $0x20, %%ecx; rep movsb; popa"
                ::"D" (&vm->guest_regs));

  /* copy stack */
  memcpy (hyperstack, (void *) (esp & (~0xFFF)), 0x1000);
  /* change frame pointer in host to hypervisor stack */
  asm volatile ("movl %%ebp, %0":"=r" (ebp));
  ebp = (((u32) &hyperstack) & (~0xFFF)) | (ebp & 0xFFF);
  asm volatile ("movl %0, %%ebp"::"r" (ebp));
  /* switch host stack to hypervisor stack */
  asm volatile ("movl %0, %%esp"::"r" (&hyperstack[(esp & 0xFFF) >> 2]));

  /* hypervisor stack now in effect */

  /* set guest to continue from resume point above */
  vmwrite (guest_eip, VMXENC_GUEST_RIP);
  /* guest takes over original stack */
  vmwrite (esp, VMXENC_GUEST_RSP);

  logger_printf ("vmx_enter_guest: GUEST-STATE: RIP=0x%p RSP=0x%p RBP=0x%p\n",
                 vmread (VMXENC_GUEST_RIP), vmread (VMXENC_GUEST_RSP), vm->guest_regs.ebp);
  return vmx_start_monitor (vm);
}

void
test_pmode_vm (void)
{
  logger_printf ("INSIDE PMODE VM -- going into infinite loop\n");
  for (;;);
}

static virtual_machine VMs[MAX_CPUS] ALIGNED (0x1000);
static int num_VMs = 0;
DEF_PER_CPU (virtual_machine *, cpu_vm);

#ifdef USE_LINUX_SANDBOX
static uint32 vmx_bios_pgt[1024] __attribute__ ((aligned(0x1000)));

static void
vmx_map_bios ()
{
  uint32 phys_pgt = (uint32) get_phys_addr (vmx_bios_pgt);
  uint32 phys_pgd = (uint32) get_pdbr ();
  uint32 *virt_pgd = map_virtual_page (phys_pgd | 3);
  uint32 i;

  memset (vmx_bios_pgt, 0, 1024 * sizeof (uint32));
  virt_pgd[0] = (uint32) phys_pgt | 7; /* so it is usable in PL=3 */
  unmap_virtual_page (virt_pgd);

  /* identity map the first megabyte */
  for (i=0; i<256; i++) vmx_bios_pgt[i] = (i << 12) | 7;
}
#endif

static void
vmx_percpu_init (void)
{
  uint8 phys_id = get_pcpu_id ();
  DLOG ("processor_init pcpu_id=%d", phys_id);
  uint32 cr0, cr4;
  uint32 *vmxon_virt;
  virtual_machine *vm = &VMs[phys_id];

  if (!vmx_enabled)
    return;

#ifdef USE_LINUX_SANDBOX
  /* Maps BIOS region for Linux sandbox */
  if (phys_id == LINUX_SANDBOX) vmx_map_bios ();
#endif

  /* Set the NE bit to satisfy CR0_FIXED0 */
  asm volatile ("movl %%cr0, %0\n"
                "orl $0x20, %0\n"
                "movl %0, %%cr0":"=r" (cr0));

#if DEBUG_VMX > 1
  com1_printf ("IA32_FEATURE_CONTROL: 0x%.8X\n", (uint32) rdmsr (IA32_FEATURE_CONTROL));
  com1_printf ("IA32_VMX_BASIC: 0x%.16llX\n",
               rdmsr (IA32_VMX_BASIC));
  com1_printf ("IA32_VMX_CR0_FIXED0: 0x%.8X\n", (uint32) rdmsr (IA32_VMX_CR0_FIXED0));
  com1_printf ("IA32_VMX_CR0_FIXED1: 0x%.8X\n", (uint32) rdmsr (IA32_VMX_CR0_FIXED1));
  com1_printf ("IA32_VMX_CR4_FIXED0: 0x%.8X\n", (uint32) rdmsr (IA32_VMX_CR4_FIXED0));
  com1_printf ("IA32_VMX_CR4_FIXED1: 0x%.8X\n", (uint32) rdmsr (IA32_VMX_CR4_FIXED1));

  com1_printf ("IA32_VMX_PINBASED_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_PINBASED_CTLS));
  com1_printf ("IA32_VMX_TRUE_PINBASED_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_TRUE_PINBASED_CTLS));
  com1_printf ("IA32_VMX_PROCBASED_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_PROCBASED_CTLS));
  com1_printf ("IA32_VMX_TRUE_PROCBASED_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_TRUE_PROCBASED_CTLS));
  com1_printf ("IA32_VMX_PROCBASED_CTLS2: 0x%.16llX\n",
               rdmsr (IA32_VMX_PROCBASED_CTLS2));

  com1_printf ("IA32_VMX_EXIT_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_EXIT_CTLS));
  com1_printf ("IA32_VMX_ENTRY_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_ENTRY_CTLS));
  com1_printf ("IA32_VMX_TRUE_EXIT_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_TRUE_EXIT_CTLS));
  com1_printf ("IA32_VMX_TRUE_ENTRY_CTLS: 0x%.16llX\n",
               rdmsr (IA32_VMX_TRUE_ENTRY_CTLS));
  com1_printf ("IA32_VMX_MISC: 0x%.16llX\n",
               rdmsr (IA32_VMX_MISC));
#endif

  /* Enable VMX */
  asm volatile ("movl %%cr4, %0\n"
                "orl $0x2000, %0\n"
                "movl %0, %%cr4":"=r" (cr4));

  /* Allocate a VMXON memory area */
  vmxon_frame[phys_id] = alloc_phys_frame ();
  vmxon_virt  = map_virtual_page (vmxon_frame[phys_id] | 3);
  *vmxon_virt = rdmsr (IA32_VMX_BASIC);
  unmap_virtual_page (vmxon_virt);

  vmxon (vmxon_frame[phys_id]);

  if (vmx_get_error () != 0) {
#if DEBUG_VMX > 0
    com1_printf ("VMXON error\n");
#endif
    goto abort_vmxon;
  }

  percpu_write (cpu_vm, vm);

  if (vmx_create_monitor (vm, 0, 0) != 0)
    goto vm_error;

  if (vmx_enter_guest (vm) != 0)
    goto vm_error;

  num_VMs++;

  return;

vm_error:
  vmxoff ();
abort_vmxon:
  free_phys_frame (vmxon_frame[phys_id]);
}

bool
vmx_init (void)
{
  int cpu = get_pcpu_id ();
  
  if (cpu == 0) {
    vmx_detect ();
    if (!vmx_enabled) {
      DLOG ("VMX not enabled");
      goto vm_error;
    }
  }

  vmx_percpu_init ();

#if 0
  if (vmx_unload_monitor (&first_vm) != 0)
    goto vm_error;
  vmx_destroy_monitor (&first_vm);
#endif

  return TRUE;
vm_error:
  return FALSE;
}

#include "module/header.h"

static const struct module_ops mod_ops = {
  .init = vmx_init
};

#ifdef USE_VMX
//DEF_MODULE (vm___vmx, "VMX hardware virtualization driver", &mod_ops, {});
#endif

#endif /* USE_VMX */

/*
 * Local Variables:
 * indent-tabs-mode: nil
 * mode: C
 * c-file-style: "gnu"
 * c-basic-offset: 2
 * End:
 */

/* vi: set et sw=2 sts=2: */
