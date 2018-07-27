/*
 * QEMU KVM support
 *
 * Copyright IBM, Corp. 2008
 *           Red Hat, Inc. 2008
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *  Glauber Costa     <gcosta@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>

#include <linux/kvm.h>

#include "qemu-common.h"
#include "qemu/atomic.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/hw.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/s390x/adapter.h"
#include "exec/gdbstub.h"
#include "sysemu/kvm_int.h"
#include "sysemu/cpus.h"
#include "qemu/bswap.h"
#include "exec/memory.h"
#include "exec/ram_addr.h"
#include "exec/address-spaces.h"
#include "qemu/event_notifier.h"
#include "trace.h"
#include "hw/irq.h"

#include "hw/boards.h"

#include "io/channel.h"
#include "io/channel-util.h"
#include "io/channel-buffer.h"
#include "io/channel-tls.h"
#include "io/channel-command.h"
#include "migration/savevm.h"
#include "migration/migration.h"
#include "migration/fd.h"
#include "migration/channel.h"
#include "migration/exec.h"
#include "migration/qemu-file.h"
#include "migration/ram.h"
#include "migration/colo.h"
#include "migration/global_state.h"
#include "migration/qemu-file-channel.h"
#include "migration/tls.h"
#include "migration/block.h"
#include "qemu/cutils.h"
#include "qmp-commands.h"
#include "exec/target_page.h"
#include "qom/object.h"
#define BUFFER_DELAY     100
#define XFER_LIMIT_RATIO (1000 / BUFFER_DELAY)
//#include <fcntl.h>
/* This check must be after config-host.h is included */
#ifdef CONFIG_EVENTFD
#include <sys/eventfd.h>
#endif

/* KVM uses PAGE_SIZE in its definition of KVM_COALESCED_MMIO_MAX. We
 * need to use the real host PAGE_SIZE, as that's what KVM will use.
 */
#define PAGE_SIZE getpagesize()

//#define DEBUG_KVM

#ifdef DEBUG_KVM
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define KVM_MSI_HASHTAB_SIZE    256

struct KVMParkedVcpu {
    unsigned long vcpu_id;
    int kvm_fd;
    QLIST_ENTRY(KVMParkedVcpu) node;
};

struct KVMState
{
    AccelState parent_obj;

    int nr_slots;
    int fd;
    int vmfd;
    int coalesced_mmio;
    struct kvm_coalesced_mmio_ring *coalesced_mmio_ring;
    bool coalesced_flush_in_progress;
    int vcpu_events;
    int robust_singlestep;
    int debugregs;
#ifdef KVM_CAP_SET_GUEST_DEBUG
    struct kvm_sw_breakpoint_head kvm_sw_breakpoints;
#endif
    int many_ioeventfds;
    int intx_set_mask;
    bool sync_mmu;
    /* The man page (and posix) say ioctl numbers are signed int, but
     * they're not.  Linux, glibc and *BSD all treat ioctl numbers as
     * unsigned, and treating them as signed here can break things */
    unsigned irq_set_ioctl;
    unsigned int sigmask_len;
    GHashTable *gsimap;
#ifdef KVM_CAP_IRQ_ROUTING
    struct kvm_irq_routing *irq_routes;
    int nr_allocated_irq_routes;
    unsigned long *used_gsi_bitmap;
    unsigned int gsi_count;
    QTAILQ_HEAD(msi_hashtab, KVMMSIRoute) msi_hashtab[KVM_MSI_HASHTAB_SIZE];
#endif
    KVMMemoryListener memory_listener;
    QLIST_HEAD(, KVMParkedVcpu) kvm_parked_vcpus;
};

KVMState *kvm_state;
bool kvm_kernel_irqchip;
bool kvm_split_irqchip;
bool kvm_async_interrupts_allowed;
bool kvm_halt_in_kernel_allowed;
bool kvm_eventfds_allowed;
bool kvm_irqfds_allowed;
bool kvm_resamplefds_allowed;
bool kvm_msi_via_irqfd_allowed;
bool kvm_gsi_routing_allowed;
bool kvm_gsi_direct_mapping;
bool kvm_allowed;
bool kvm_readonly_mem_allowed;
bool kvm_vm_attributes_allowed;
bool kvm_direct_msi_allowed;
bool kvm_ioeventfd_any_length_allowed;
bool kvm_msi_use_devid;
static bool kvm_immediate_exit;

static const KVMCapabilityInfo kvm_required_capabilites[] = {
    KVM_CAP_INFO(USER_MEMORY),
    KVM_CAP_INFO(DESTROY_MEMORY_REGION_WORKS),
    KVM_CAP_INFO(JOIN_MEMORY_REGIONS_WORKS),
    KVM_CAP_LAST_INFO
};

#define QEMU_BIN "/home/xenus/unikernels/paixnidi/fork/qemu-2.11.2/x86_64-softmmu/qemu-system-x86_64"

void my_fork(void *data, bool *check);
void my_start_migration(void *data);
void my_exec_start_outgoing_migration(MigrationState *s, const char *command, Error **errp);
void my_migration_channel_connect(MigrationState *s,
                               QIOChannel *ioc,
                               const char *hostname);
void my_migrate_fd_connect(MigrationState *s);
static void *my_migration_thread(void *opaque);
static int postcopy_start(MigrationState *ms, bool *old_vm_running);
static void migration_completion(MigrationState *s, int current_active_state,
                                 bool *old_vm_running,
                                 int64_t *start_time);
static int migration_maybe_pause(MigrationState *s,
                                 int *current_active_state,
                                 int new_state);
static int await_return_path_close_on_source(MigrationState *ms);
static NotifierList migration_state_notifiers =
    NOTIFIER_LIST_INITIALIZER(migration_state_notifiers);
static void migrate_fd_cleanup(void *opaque);
static int open_return_path_on_source(MigrationState *ms);
static void *source_return_path_thread(void *opaque);
static void block_cleanup_parameters(MigrationState *s);
static void mark_source_rp_bad(MigrationState *s);
static void migrate_set_block_incremental(MigrationState *s, bool value);
static bool migration_is_setup_or_active(int state);
/* Messages sent on the return path from destination to source */
enum mig_rp_message_type {
    MIG_RP_MSG_INVALID = 0,  /* Must be 0 */
    MIG_RP_MSG_SHUT,         /* sibling will not send any more RP messages */
    MIG_RP_MSG_PONG,         /* Response to a PING; data (seq: be32 ) */

    MIG_RP_MSG_REQ_PAGES_ID, /* data (start: be64, len: be32, id: string) */
    MIG_RP_MSG_REQ_PAGES,    /* data (start: be64, len: be32) */

    MIG_RP_MSG_MAX
};
static struct rp_cmd_args {
    ssize_t     len; /* -1 = variable */
    const char *name;
} rp_cmd_args[] = {
    [MIG_RP_MSG_INVALID]        = { .len = -1, .name = "INVALID" },
    [MIG_RP_MSG_SHUT]           = { .len =  4, .name = "SHUT" },
    [MIG_RP_MSG_PONG]           = { .len =  4, .name = "PONG" },
    [MIG_RP_MSG_REQ_PAGES]      = { .len = 12, .name = "REQ_PAGES" },
    [MIG_RP_MSG_REQ_PAGES_ID]   = { .len = -1, .name = "REQ_PAGES_ID" },
    [MIG_RP_MSG_MAX]            = { .len = -1, .name = "MAX" },
};
static void migrate_handle_rp_req_pages(MigrationState *ms, const char* rbname,
                                       ram_addr_t start, size_t len);
unsigned int my_cnt = 0;

int kvm_get_max_memslots(void)
{
    KVMState *s = KVM_STATE(current_machine->accelerator);

    return s->nr_slots;
}

static KVMSlot *kvm_get_free_slot(KVMMemoryListener *kml)
{
    KVMState *s = kvm_state;
    int i;

    for (i = 0; i < s->nr_slots; i++) {
        if (kml->slots[i].memory_size == 0) {
            return &kml->slots[i];
        }
    }

    return NULL;
}

bool kvm_has_free_slot(MachineState *ms)
{
    KVMState *s = KVM_STATE(ms->accelerator);

    return kvm_get_free_slot(&s->memory_listener);
}

static KVMSlot *kvm_alloc_slot(KVMMemoryListener *kml)
{
    KVMSlot *slot = kvm_get_free_slot(kml);

    if (slot) {
        return slot;
    }

    fprintf(stderr, "%s: no free slot available\n", __func__);
    abort();
}

static KVMSlot *kvm_lookup_matching_slot(KVMMemoryListener *kml,
                                         hwaddr start_addr,
                                         hwaddr size)
{
    KVMState *s = kvm_state;
    int i;

    for (i = 0; i < s->nr_slots; i++) {
        KVMSlot *mem = &kml->slots[i];

        if (start_addr == mem->start_addr && size == mem->memory_size) {
            return mem;
        }
    }

    return NULL;
}

/*
 * Calculate and align the start address and the size of the section.
 * Return the size. If the size is 0, the aligned section is empty.
 */
static hwaddr kvm_align_section(MemoryRegionSection *section,
                                hwaddr *start)
{
    hwaddr size = int128_get64(section->size);
    hwaddr delta, aligned;

    /* kvm works in page size chunks, but the function may be called
       with sub-page size and unaligned start address. Pad the start
       address to next and truncate size to previous page boundary. */
    aligned = ROUND_UP(section->offset_within_address_space,
                       qemu_real_host_page_size);
    delta = aligned - section->offset_within_address_space;
    *start = aligned;
    if (delta > size) {
        return 0;
    }

    return (size - delta) & qemu_real_host_page_mask;
}

int kvm_physical_memory_addr_from_host(KVMState *s, void *ram,
                                       hwaddr *phys_addr)
{
    KVMMemoryListener *kml = &s->memory_listener;
    int i;

    for (i = 0; i < s->nr_slots; i++) {
        KVMSlot *mem = &kml->slots[i];

        if (ram >= mem->ram && ram < mem->ram + mem->memory_size) {
            *phys_addr = mem->start_addr + (ram - mem->ram);
            return 1;
        }
    }

    return 0;
}

static int kvm_set_user_memory_region(KVMMemoryListener *kml, KVMSlot *slot)
{
    KVMState *s = kvm_state;
    struct kvm_userspace_memory_region mem;

    mem.slot = slot->slot | (kml->as_id << 16);
    mem.guest_phys_addr = slot->start_addr;
    mem.userspace_addr = (unsigned long)slot->ram;
    mem.flags = slot->flags;

    if (slot->memory_size && mem.flags & KVM_MEM_READONLY) {
        /* Set the slot size to 0 before setting the slot to the desired
         * value. This is needed based on KVM commit 75d61fbc. */
        mem.memory_size = 0;
        kvm_vm_ioctl(s, KVM_SET_USER_MEMORY_REGION, &mem);
    }
    mem.memory_size = slot->memory_size;
    return kvm_vm_ioctl(s, KVM_SET_USER_MEMORY_REGION, &mem);
}

int kvm_destroy_vcpu(CPUState *cpu)
{
    KVMState *s = kvm_state;
    long mmap_size;
    struct KVMParkedVcpu *vcpu = NULL;
    int ret = 0;

    DPRINTF("kvm_destroy_vcpu\n");

    mmap_size = kvm_ioctl(s, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) {
        ret = mmap_size;
        DPRINTF("KVM_GET_VCPU_MMAP_SIZE failed\n");
        goto err;
    }

    ret = munmap(cpu->kvm_run, mmap_size);
    if (ret < 0) {
        goto err;
    }

    vcpu = g_malloc0(sizeof(*vcpu));
    vcpu->vcpu_id = kvm_arch_vcpu_id(cpu);
    vcpu->kvm_fd = cpu->kvm_fd;
    QLIST_INSERT_HEAD(&kvm_state->kvm_parked_vcpus, vcpu, node);
err:
    return ret;
}

static int kvm_get_vcpu(KVMState *s, unsigned long vcpu_id)
{
    struct KVMParkedVcpu *cpu;

    QLIST_FOREACH(cpu, &s->kvm_parked_vcpus, node) {
        if (cpu->vcpu_id == vcpu_id) {
            int kvm_fd;

            QLIST_REMOVE(cpu, node);
            kvm_fd = cpu->kvm_fd;
            g_free(cpu);
            return kvm_fd;
        }
    }

    return kvm_vm_ioctl(s, KVM_CREATE_VCPU, (void *)vcpu_id);
}

int kvm_init_vcpu(CPUState *cpu)
{
    KVMState *s = kvm_state;
    long mmap_size;
    int ret;

    DPRINTF("kvm_init_vcpu\n");

    ret = kvm_get_vcpu(s, kvm_arch_vcpu_id(cpu));
    if (ret < 0) {
        DPRINTF("kvm_create_vcpu failed\n");
        goto err;
    }

    cpu->kvm_fd = ret;
    cpu->kvm_state = s;
    cpu->vcpu_dirty = true;

    mmap_size = kvm_ioctl(s, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0) {
        ret = mmap_size;
        DPRINTF("KVM_GET_VCPU_MMAP_SIZE failed\n");
        goto err;
    }

    cpu->kvm_run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        cpu->kvm_fd, 0);
    if (cpu->kvm_run == MAP_FAILED) {
        ret = -errno;
        DPRINTF("mmap'ing vcpu state failed\n");
        goto err;
    }

    if (s->coalesced_mmio && !s->coalesced_mmio_ring) {
        s->coalesced_mmio_ring =
            (void *)cpu->kvm_run + s->coalesced_mmio * PAGE_SIZE;
    }

    ret = kvm_arch_init_vcpu(cpu);
err:
    return ret;
}

/*
 * dirty pages logging control
 */

static int kvm_mem_flags(MemoryRegion *mr)
{
    bool readonly = mr->readonly || memory_region_is_romd(mr);
    int flags = 0;

    if (memory_region_get_dirty_log_mask(mr) != 0) {
        flags |= KVM_MEM_LOG_DIRTY_PAGES;
    }
    if (readonly && kvm_readonly_mem_allowed) {
        flags |= KVM_MEM_READONLY;
    }
    return flags;
}

static int kvm_slot_update_flags(KVMMemoryListener *kml, KVMSlot *mem,
                                 MemoryRegion *mr)
{
    int old_flags;

    old_flags = mem->flags;
    mem->flags = kvm_mem_flags(mr);

    /* If nothing changed effectively, no need to issue ioctl */
    if (mem->flags == old_flags) {
        return 0;
    }

    return kvm_set_user_memory_region(kml, mem);
}

static int kvm_section_update_flags(KVMMemoryListener *kml,
                                    MemoryRegionSection *section)
{
    hwaddr start_addr, size;
    KVMSlot *mem;

    size = kvm_align_section(section, &start_addr);
    if (!size) {
        return 0;
    }

    mem = kvm_lookup_matching_slot(kml, start_addr, size);
    if (!mem) {
        /* We don't have a slot if we want to trap every access. */
        return 0;
    }

    return kvm_slot_update_flags(kml, mem, section->mr);
}

static void kvm_log_start(MemoryListener *listener,
                          MemoryRegionSection *section,
                          int old, int new)
{
    KVMMemoryListener *kml = container_of(listener, KVMMemoryListener, listener);
    int r;

    if (old != 0) {
        return;
    }

    r = kvm_section_update_flags(kml, section);
    if (r < 0) {
        abort();
    }
}

static void kvm_log_stop(MemoryListener *listener,
                          MemoryRegionSection *section,
                          int old, int new)
{
    KVMMemoryListener *kml = container_of(listener, KVMMemoryListener, listener);
    int r;

    if (new != 0) {
        return;
    }

    r = kvm_section_update_flags(kml, section);
    if (r < 0) {
        abort();
    }
}

/* get kvm's dirty pages bitmap and update qemu's */
static int kvm_get_dirty_pages_log_range(MemoryRegionSection *section,
                                         unsigned long *bitmap)
{
    ram_addr_t start = section->offset_within_region +
                       memory_region_get_ram_addr(section->mr);
    ram_addr_t pages = int128_get64(section->size) / getpagesize();

    cpu_physical_memory_set_dirty_lebitmap(bitmap, start, pages);
    return 0;
}

#define ALIGN(x, y)  (((x)+(y)-1) & ~((y)-1))

/**
 * kvm_physical_sync_dirty_bitmap - Grab dirty bitmap from kernel space
 * This function updates qemu's dirty bitmap using
 * memory_region_set_dirty().  This means all bits are set
 * to dirty.
 *
 * @start_add: start of logged region.
 * @end_addr: end of logged region.
 */
static int kvm_physical_sync_dirty_bitmap(KVMMemoryListener *kml,
                                          MemoryRegionSection *section)
{
    KVMState *s = kvm_state;
    struct kvm_dirty_log d = {};
    KVMSlot *mem;
    hwaddr start_addr, size;

    size = kvm_align_section(section, &start_addr);
    if (size) {
        mem = kvm_lookup_matching_slot(kml, start_addr, size);
        if (!mem) {
            /* We don't have a slot if we want to trap every access. */
            return 0;
        }

        /* XXX bad kernel interface alert
         * For dirty bitmap, kernel allocates array of size aligned to
         * bits-per-long.  But for case when the kernel is 64bits and
         * the userspace is 32bits, userspace can't align to the same
         * bits-per-long, since sizeof(long) is different between kernel
         * and user space.  This way, userspace will provide buffer which
         * may be 4 bytes less than the kernel will use, resulting in
         * userspace memory corruption (which is not detectable by valgrind
         * too, in most cases).
         * So for now, let's align to 64 instead of HOST_LONG_BITS here, in
         * a hope that sizeof(long) won't become >8 any time soon.
         */
        size = ALIGN(((mem->memory_size) >> TARGET_PAGE_BITS),
                     /*HOST_LONG_BITS*/ 64) / 8;
        d.dirty_bitmap = g_malloc0(size);

        d.slot = mem->slot | (kml->as_id << 16);
        if (kvm_vm_ioctl(s, KVM_GET_DIRTY_LOG, &d) == -1) {
            DPRINTF("ioctl failed %d\n", errno);
            g_free(d.dirty_bitmap);
            return -1;
        }

        kvm_get_dirty_pages_log_range(section, d.dirty_bitmap);
        g_free(d.dirty_bitmap);
    }

    return 0;
}

static void kvm_coalesce_mmio_region(MemoryListener *listener,
                                     MemoryRegionSection *secion,
                                     hwaddr start, hwaddr size)
{
    KVMState *s = kvm_state;

    if (s->coalesced_mmio) {
        struct kvm_coalesced_mmio_zone zone;

        zone.addr = start;
        zone.size = size;
        zone.pad = 0;

        (void)kvm_vm_ioctl(s, KVM_REGISTER_COALESCED_MMIO, &zone);
    }
}

static void kvm_uncoalesce_mmio_region(MemoryListener *listener,
                                       MemoryRegionSection *secion,
                                       hwaddr start, hwaddr size)
{
    KVMState *s = kvm_state;

    if (s->coalesced_mmio) {
        struct kvm_coalesced_mmio_zone zone;

        zone.addr = start;
        zone.size = size;
        zone.pad = 0;

        (void)kvm_vm_ioctl(s, KVM_UNREGISTER_COALESCED_MMIO, &zone);
    }
}

int kvm_check_extension(KVMState *s, unsigned int extension)
{
    int ret;

    ret = kvm_ioctl(s, KVM_CHECK_EXTENSION, extension);
    if (ret < 0) {
        ret = 0;
    }

    return ret;
}

int kvm_vm_check_extension(KVMState *s, unsigned int extension)
{
    int ret;

    ret = kvm_vm_ioctl(s, KVM_CHECK_EXTENSION, extension);
    if (ret < 0) {
        /* VM wide version not implemented, use global one instead */
        ret = kvm_check_extension(s, extension);
    }

    return ret;
}

static uint32_t adjust_ioeventfd_endianness(uint32_t val, uint32_t size)
{
#if defined(HOST_WORDS_BIGENDIAN) != defined(TARGET_WORDS_BIGENDIAN)
    /* The kernel expects ioeventfd values in HOST_WORDS_BIGENDIAN
     * endianness, but the memory core hands them in target endianness.
     * For example, PPC is always treated as big-endian even if running
     * on KVM and on PPC64LE.  Correct here.
     */
    switch (size) {
    case 2:
        val = bswap16(val);
        break;
    case 4:
        val = bswap32(val);
        break;
    }
#endif
    return val;
}

static int kvm_set_ioeventfd_mmio(int fd, hwaddr addr, uint32_t val,
                                  bool assign, uint32_t size, bool datamatch)
{
    int ret;
    struct kvm_ioeventfd iofd = {
        .datamatch = datamatch ? adjust_ioeventfd_endianness(val, size) : 0,
        .addr = addr,
        .len = size,
        .flags = 0,
        .fd = fd,
    };

    if (!kvm_enabled()) {
        return -ENOSYS;
    }

    if (datamatch) {
        iofd.flags |= KVM_IOEVENTFD_FLAG_DATAMATCH;
    }
    if (!assign) {
        iofd.flags |= KVM_IOEVENTFD_FLAG_DEASSIGN;
    }

    ret = kvm_vm_ioctl(kvm_state, KVM_IOEVENTFD, &iofd);

    if (ret < 0) {
        return -errno;
    }

    return 0;
}

static int kvm_set_ioeventfd_pio(int fd, uint16_t addr, uint16_t val,
                                 bool assign, uint32_t size, bool datamatch)
{
    struct kvm_ioeventfd kick = {
        .datamatch = datamatch ? adjust_ioeventfd_endianness(val, size) : 0,
        .addr = addr,
        .flags = KVM_IOEVENTFD_FLAG_PIO,
        .len = size,
        .fd = fd,
    };
    int r;
    if (!kvm_enabled()) {
        return -ENOSYS;
    }
    if (datamatch) {
        kick.flags |= KVM_IOEVENTFD_FLAG_DATAMATCH;
    }
    if (!assign) {
        kick.flags |= KVM_IOEVENTFD_FLAG_DEASSIGN;
    }
    r = kvm_vm_ioctl(kvm_state, KVM_IOEVENTFD, &kick);
    if (r < 0) {
        return r;
    }
    return 0;
}


static int kvm_check_many_ioeventfds(void)
{
    /* Userspace can use ioeventfd for io notification.  This requires a host
     * that supports eventfd(2) and an I/O thread; since eventfd does not
     * support SIGIO it cannot interrupt the vcpu.
     *
     * Older kernels have a 6 device limit on the KVM io bus.  Find out so we
     * can avoid creating too many ioeventfds.
     */
#if defined(CONFIG_EVENTFD)
    int ioeventfds[7];
    int i, ret = 0;
    for (i = 0; i < ARRAY_SIZE(ioeventfds); i++) {
        ioeventfds[i] = eventfd(0, EFD_CLOEXEC);
        if (ioeventfds[i] < 0) {
            break;
        }
        ret = kvm_set_ioeventfd_pio(ioeventfds[i], 0, i, true, 2, true);
        if (ret < 0) {
            close(ioeventfds[i]);
            break;
        }
    }

    /* Decide whether many devices are supported or not */
    ret = i == ARRAY_SIZE(ioeventfds);

    while (i-- > 0) {
        kvm_set_ioeventfd_pio(ioeventfds[i], 0, i, false, 2, true);
        close(ioeventfds[i]);
    }
    return ret;
#else
    return 0;
#endif
}

static const KVMCapabilityInfo *
kvm_check_extension_list(KVMState *s, const KVMCapabilityInfo *list)
{
    while (list->name) {
        if (!kvm_check_extension(s, list->value)) {
            return list;
        }
        list++;
    }
    return NULL;
}

static void kvm_set_phys_mem(KVMMemoryListener *kml,
                             MemoryRegionSection *section, bool add)
{
    KVMSlot *mem;
    int err;
    MemoryRegion *mr = section->mr;
    bool writeable = !mr->readonly && !mr->rom_device;
    hwaddr start_addr, size;
    void *ram;

    if (!memory_region_is_ram(mr)) {
        if (writeable || !kvm_readonly_mem_allowed) {
            return;
        } else if (!mr->romd_mode) {
            /* If the memory device is not in romd_mode, then we actually want
             * to remove the kvm memory slot so all accesses will trap. */
            add = false;
        }
    }

    size = kvm_align_section(section, &start_addr);
    if (!size) {
        return;
    }

    /* use aligned delta to align the ram address */
    ram = memory_region_get_ram_ptr(mr) + section->offset_within_region +
          (start_addr - section->offset_within_address_space);

    if (!add) {
        mem = kvm_lookup_matching_slot(kml, start_addr, size);
        if (!mem) {
            return;
        }
        if (mem->flags & KVM_MEM_LOG_DIRTY_PAGES) {
            kvm_physical_sync_dirty_bitmap(kml, section);
        }

        /* unregister the slot */
        mem->memory_size = 0;
        err = kvm_set_user_memory_region(kml, mem);
        if (err) {
            fprintf(stderr, "%s: error unregistering slot: %s\n",
                    __func__, strerror(-err));
            abort();
        }
        return;
    }

    /* register the new slot */
    mem = kvm_alloc_slot(kml);
    mem->memory_size = size;
    mem->start_addr = start_addr;
    mem->ram = ram;
    mem->flags = kvm_mem_flags(mr);

    err = kvm_set_user_memory_region(kml, mem);
    if (err) {
        fprintf(stderr, "%s: error registering slot: %s\n", __func__,
                strerror(-err));
        abort();
    }
}

static void kvm_region_add(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    KVMMemoryListener *kml = container_of(listener, KVMMemoryListener, listener);

    memory_region_ref(section->mr);
    kvm_set_phys_mem(kml, section, true);
}

static void kvm_region_del(MemoryListener *listener,
                           MemoryRegionSection *section)
{
    KVMMemoryListener *kml = container_of(listener, KVMMemoryListener, listener);

    kvm_set_phys_mem(kml, section, false);
    memory_region_unref(section->mr);
}

static void kvm_log_sync(MemoryListener *listener,
                         MemoryRegionSection *section)
{
    KVMMemoryListener *kml = container_of(listener, KVMMemoryListener, listener);
    int r;

    r = kvm_physical_sync_dirty_bitmap(kml, section);
    if (r < 0) {
        abort();
    }
}

static void kvm_mem_ioeventfd_add(MemoryListener *listener,
                                  MemoryRegionSection *section,
                                  bool match_data, uint64_t data,
                                  EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = kvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                               data, true, int128_get64(section->size),
                               match_data);
    if (r < 0) {
        fprintf(stderr, "%s: error adding ioeventfd: %s\n",
                __func__, strerror(-r));
        abort();
    }
}

static void kvm_mem_ioeventfd_del(MemoryListener *listener,
                                  MemoryRegionSection *section,
                                  bool match_data, uint64_t data,
                                  EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = kvm_set_ioeventfd_mmio(fd, section->offset_within_address_space,
                               data, false, int128_get64(section->size),
                               match_data);
    if (r < 0) {
        abort();
    }
}

static void kvm_io_ioeventfd_add(MemoryListener *listener,
                                 MemoryRegionSection *section,
                                 bool match_data, uint64_t data,
                                 EventNotifier *e)
{
    int fd = event_notifier_get_fd(e);
    int r;

    r = kvm_set_ioeventfd_pio(fd, section->offset_within_address_space,
                              data, true, int128_get64(section->size),
                              match_data);
    if (r < 0) {
        fprintf(stderr, "%s: error adding ioeventfd: %s\n",
                __func__, strerror(-r));
        abort();
    }
}

static void kvm_io_ioeventfd_del(MemoryListener *listener,
                                 MemoryRegionSection *section,
                                 bool match_data, uint64_t data,
                                 EventNotifier *e)

{
    int fd = event_notifier_get_fd(e);
    int r;

    r = kvm_set_ioeventfd_pio(fd, section->offset_within_address_space,
                              data, false, int128_get64(section->size),
                              match_data);
    if (r < 0) {
        abort();
    }
}

void kvm_memory_listener_register(KVMState *s, KVMMemoryListener *kml,
                                  AddressSpace *as, int as_id)
{
    int i;

    kml->slots = g_malloc0(s->nr_slots * sizeof(KVMSlot));
    kml->as_id = as_id;

    for (i = 0; i < s->nr_slots; i++) {
        kml->slots[i].slot = i;
    }

    kml->listener.region_add = kvm_region_add;
    kml->listener.region_del = kvm_region_del;
    kml->listener.log_start = kvm_log_start;
    kml->listener.log_stop = kvm_log_stop;
    kml->listener.log_sync = kvm_log_sync;
    kml->listener.priority = 10;

    memory_listener_register(&kml->listener, as);
}

static MemoryListener kvm_io_listener = {
    .eventfd_add = kvm_io_ioeventfd_add,
    .eventfd_del = kvm_io_ioeventfd_del,
    .priority = 10,
};

int kvm_set_irq(KVMState *s, int irq, int level)
{
    struct kvm_irq_level event;
    int ret;

    assert(kvm_async_interrupts_enabled());

    event.level = level;
    event.irq = irq;
    ret = kvm_vm_ioctl(s, s->irq_set_ioctl, &event);
    if (ret < 0) {
        perror("kvm_set_irq");
        abort();
    }

    return (s->irq_set_ioctl == KVM_IRQ_LINE) ? 1 : event.status;
}

#ifdef KVM_CAP_IRQ_ROUTING
typedef struct KVMMSIRoute {
    struct kvm_irq_routing_entry kroute;
    QTAILQ_ENTRY(KVMMSIRoute) entry;
} KVMMSIRoute;

static void set_gsi(KVMState *s, unsigned int gsi)
{
    set_bit(gsi, s->used_gsi_bitmap);
}

static void clear_gsi(KVMState *s, unsigned int gsi)
{
    clear_bit(gsi, s->used_gsi_bitmap);
}

void kvm_init_irq_routing(KVMState *s)
{
    int gsi_count, i;

    gsi_count = kvm_check_extension(s, KVM_CAP_IRQ_ROUTING) - 1;
    if (gsi_count > 0) {
        /* Round up so we can search ints using ffs */
        s->used_gsi_bitmap = bitmap_new(gsi_count);
        s->gsi_count = gsi_count;
    }

    s->irq_routes = g_malloc0(sizeof(*s->irq_routes));
    s->nr_allocated_irq_routes = 0;

    if (!kvm_direct_msi_allowed) {
        for (i = 0; i < KVM_MSI_HASHTAB_SIZE; i++) {
            QTAILQ_INIT(&s->msi_hashtab[i]);
        }
    }

    kvm_arch_init_irq_routing(s);
}

void kvm_irqchip_commit_routes(KVMState *s)
{
    int ret;

    if (kvm_gsi_direct_mapping()) {
        return;
    }

    if (!kvm_gsi_routing_enabled()) {
        return;
    }

    s->irq_routes->flags = 0;
    trace_kvm_irqchip_commit_routes();
    ret = kvm_vm_ioctl(s, KVM_SET_GSI_ROUTING, s->irq_routes);
    assert(ret == 0);
}

static void kvm_add_routing_entry(KVMState *s,
                                  struct kvm_irq_routing_entry *entry)
{
    struct kvm_irq_routing_entry *new;
    int n, size;

    if (s->irq_routes->nr == s->nr_allocated_irq_routes) {
        n = s->nr_allocated_irq_routes * 2;
        if (n < 64) {
            n = 64;
        }
        size = sizeof(struct kvm_irq_routing);
        size += n * sizeof(*new);
        s->irq_routes = g_realloc(s->irq_routes, size);
        s->nr_allocated_irq_routes = n;
    }
    n = s->irq_routes->nr++;
    new = &s->irq_routes->entries[n];

    *new = *entry;

    set_gsi(s, entry->gsi);
}

static int kvm_update_routing_entry(KVMState *s,
                                    struct kvm_irq_routing_entry *new_entry)
{
    struct kvm_irq_routing_entry *entry;
    int n;

    for (n = 0; n < s->irq_routes->nr; n++) {
        entry = &s->irq_routes->entries[n];
        if (entry->gsi != new_entry->gsi) {
            continue;
        }

        if(!memcmp(entry, new_entry, sizeof *entry)) {
            return 0;
        }

        *entry = *new_entry;

        return 0;
    }

    return -ESRCH;
}

void kvm_irqchip_add_irq_route(KVMState *s, int irq, int irqchip, int pin)
{
    struct kvm_irq_routing_entry e = {};

    assert(pin < s->gsi_count);

    e.gsi = irq;
    e.type = KVM_IRQ_ROUTING_IRQCHIP;
    e.flags = 0;
    e.u.irqchip.irqchip = irqchip;
    e.u.irqchip.pin = pin;
    kvm_add_routing_entry(s, &e);
}

void kvm_irqchip_release_virq(KVMState *s, int virq)
{
    struct kvm_irq_routing_entry *e;
    int i;

    if (kvm_gsi_direct_mapping()) {
        return;
    }

    for (i = 0; i < s->irq_routes->nr; i++) {
        e = &s->irq_routes->entries[i];
        if (e->gsi == virq) {
            s->irq_routes->nr--;
            *e = s->irq_routes->entries[s->irq_routes->nr];
        }
    }
    clear_gsi(s, virq);
    kvm_arch_release_virq_post(virq);
    trace_kvm_irqchip_release_virq(virq);
}

static unsigned int kvm_hash_msi(uint32_t data)
{
    /* This is optimized for IA32 MSI layout. However, no other arch shall
     * repeat the mistake of not providing a direct MSI injection API. */
    return data & 0xff;
}

static void kvm_flush_dynamic_msi_routes(KVMState *s)
{
    KVMMSIRoute *route, *next;
    unsigned int hash;

    for (hash = 0; hash < KVM_MSI_HASHTAB_SIZE; hash++) {
        QTAILQ_FOREACH_SAFE(route, &s->msi_hashtab[hash], entry, next) {
            kvm_irqchip_release_virq(s, route->kroute.gsi);
            QTAILQ_REMOVE(&s->msi_hashtab[hash], route, entry);
            g_free(route);
        }
    }
}

static int kvm_irqchip_get_virq(KVMState *s)
{
    int next_virq;

    /*
     * PIC and IOAPIC share the first 16 GSI numbers, thus the available
     * GSI numbers are more than the number of IRQ route. Allocating a GSI
     * number can succeed even though a new route entry cannot be added.
     * When this happens, flush dynamic MSI entries to free IRQ route entries.
     */
    if (!kvm_direct_msi_allowed && s->irq_routes->nr == s->gsi_count) {
        kvm_flush_dynamic_msi_routes(s);
    }

    /* Return the lowest unused GSI in the bitmap */
    next_virq = find_first_zero_bit(s->used_gsi_bitmap, s->gsi_count);
    if (next_virq >= s->gsi_count) {
        return -ENOSPC;
    } else {
        return next_virq;
    }
}

static KVMMSIRoute *kvm_lookup_msi_route(KVMState *s, MSIMessage msg)
{
    unsigned int hash = kvm_hash_msi(msg.data);
    KVMMSIRoute *route;

    QTAILQ_FOREACH(route, &s->msi_hashtab[hash], entry) {
        if (route->kroute.u.msi.address_lo == (uint32_t)msg.address &&
            route->kroute.u.msi.address_hi == (msg.address >> 32) &&
            route->kroute.u.msi.data == le32_to_cpu(msg.data)) {
            return route;
        }
    }
    return NULL;
}

int kvm_irqchip_send_msi(KVMState *s, MSIMessage msg)
{
    struct kvm_msi msi;
    KVMMSIRoute *route;

    if (kvm_direct_msi_allowed) {
        msi.address_lo = (uint32_t)msg.address;
        msi.address_hi = msg.address >> 32;
        msi.data = le32_to_cpu(msg.data);
        msi.flags = 0;
        memset(msi.pad, 0, sizeof(msi.pad));

        return kvm_vm_ioctl(s, KVM_SIGNAL_MSI, &msi);
    }

    route = kvm_lookup_msi_route(s, msg);
    if (!route) {
        int virq;

        virq = kvm_irqchip_get_virq(s);
        if (virq < 0) {
            return virq;
        }

        route = g_malloc0(sizeof(KVMMSIRoute));
        route->kroute.gsi = virq;
        route->kroute.type = KVM_IRQ_ROUTING_MSI;
        route->kroute.flags = 0;
        route->kroute.u.msi.address_lo = (uint32_t)msg.address;
        route->kroute.u.msi.address_hi = msg.address >> 32;
        route->kroute.u.msi.data = le32_to_cpu(msg.data);

        kvm_add_routing_entry(s, &route->kroute);
        kvm_irqchip_commit_routes(s);

        QTAILQ_INSERT_TAIL(&s->msi_hashtab[kvm_hash_msi(msg.data)], route,
                           entry);
    }

    assert(route->kroute.type == KVM_IRQ_ROUTING_MSI);

    return kvm_set_irq(s, route->kroute.gsi, 1);
}

int kvm_irqchip_add_msi_route(KVMState *s, int vector, PCIDevice *dev)
{
    struct kvm_irq_routing_entry kroute = {};
    int virq;
    MSIMessage msg = {0, 0};

    if (pci_available && dev) {
        msg = pci_get_msi_message(dev, vector);
    }

    if (kvm_gsi_direct_mapping()) {
        return kvm_arch_msi_data_to_gsi(msg.data);
    }

    if (!kvm_gsi_routing_enabled()) {
        return -ENOSYS;
    }

    virq = kvm_irqchip_get_virq(s);
    if (virq < 0) {
        return virq;
    }

    kroute.gsi = virq;
    kroute.type = KVM_IRQ_ROUTING_MSI;
    kroute.flags = 0;
    kroute.u.msi.address_lo = (uint32_t)msg.address;
    kroute.u.msi.address_hi = msg.address >> 32;
    kroute.u.msi.data = le32_to_cpu(msg.data);
    if (pci_available && kvm_msi_devid_required()) {
        kroute.flags = KVM_MSI_VALID_DEVID;
        kroute.u.msi.devid = pci_requester_id(dev);
    }
    if (kvm_arch_fixup_msi_route(&kroute, msg.address, msg.data, dev)) {
        kvm_irqchip_release_virq(s, virq);
        return -EINVAL;
    }

    trace_kvm_irqchip_add_msi_route(dev ? dev->name : (char *)"N/A",
                                    vector, virq);

    kvm_add_routing_entry(s, &kroute);
    kvm_arch_add_msi_route_post(&kroute, vector, dev);
    kvm_irqchip_commit_routes(s);

    return virq;
}

int kvm_irqchip_update_msi_route(KVMState *s, int virq, MSIMessage msg,
                                 PCIDevice *dev)
{
    struct kvm_irq_routing_entry kroute = {};

    if (kvm_gsi_direct_mapping()) {
        return 0;
    }

    if (!kvm_irqchip_in_kernel()) {
        return -ENOSYS;
    }

    kroute.gsi = virq;
    kroute.type = KVM_IRQ_ROUTING_MSI;
    kroute.flags = 0;
    kroute.u.msi.address_lo = (uint32_t)msg.address;
    kroute.u.msi.address_hi = msg.address >> 32;
    kroute.u.msi.data = le32_to_cpu(msg.data);
    if (pci_available && kvm_msi_devid_required()) {
        kroute.flags = KVM_MSI_VALID_DEVID;
        kroute.u.msi.devid = pci_requester_id(dev);
    }
    if (kvm_arch_fixup_msi_route(&kroute, msg.address, msg.data, dev)) {
        return -EINVAL;
    }

    trace_kvm_irqchip_update_msi_route(virq);

    return kvm_update_routing_entry(s, &kroute);
}

static int kvm_irqchip_assign_irqfd(KVMState *s, int fd, int rfd, int virq,
                                    bool assign)
{
    struct kvm_irqfd irqfd = {
        .fd = fd,
        .gsi = virq,
        .flags = assign ? 0 : KVM_IRQFD_FLAG_DEASSIGN,
    };

    if (rfd != -1) {
        irqfd.flags |= KVM_IRQFD_FLAG_RESAMPLE;
        irqfd.resamplefd = rfd;
    }

    if (!kvm_irqfds_enabled()) {
        return -ENOSYS;
    }

    return kvm_vm_ioctl(s, KVM_IRQFD, &irqfd);
}

int kvm_irqchip_add_adapter_route(KVMState *s, AdapterInfo *adapter)
{
    struct kvm_irq_routing_entry kroute = {};
    int virq;

    if (!kvm_gsi_routing_enabled()) {
        return -ENOSYS;
    }

    virq = kvm_irqchip_get_virq(s);
    if (virq < 0) {
        return virq;
    }

    kroute.gsi = virq;
    kroute.type = KVM_IRQ_ROUTING_S390_ADAPTER;
    kroute.flags = 0;
    kroute.u.adapter.summary_addr = adapter->summary_addr;
    kroute.u.adapter.ind_addr = adapter->ind_addr;
    kroute.u.adapter.summary_offset = adapter->summary_offset;
    kroute.u.adapter.ind_offset = adapter->ind_offset;
    kroute.u.adapter.adapter_id = adapter->adapter_id;

    kvm_add_routing_entry(s, &kroute);

    return virq;
}

int kvm_irqchip_add_hv_sint_route(KVMState *s, uint32_t vcpu, uint32_t sint)
{
    struct kvm_irq_routing_entry kroute = {};
    int virq;

    if (!kvm_gsi_routing_enabled()) {
        return -ENOSYS;
    }
    if (!kvm_check_extension(s, KVM_CAP_HYPERV_SYNIC)) {
        return -ENOSYS;
    }
    virq = kvm_irqchip_get_virq(s);
    if (virq < 0) {
        return virq;
    }

    kroute.gsi = virq;
    kroute.type = KVM_IRQ_ROUTING_HV_SINT;
    kroute.flags = 0;
    kroute.u.hv_sint.vcpu = vcpu;
    kroute.u.hv_sint.sint = sint;

    kvm_add_routing_entry(s, &kroute);
    kvm_irqchip_commit_routes(s);

    return virq;
}

#else /* !KVM_CAP_IRQ_ROUTING */

void kvm_init_irq_routing(KVMState *s)
{
}

void kvm_irqchip_release_virq(KVMState *s, int virq)
{
}

int kvm_irqchip_send_msi(KVMState *s, MSIMessage msg)
{
    abort();
}

int kvm_irqchip_add_msi_route(KVMState *s, int vector, PCIDevice *dev)
{
    return -ENOSYS;
}

int kvm_irqchip_add_adapter_route(KVMState *s, AdapterInfo *adapter)
{
    return -ENOSYS;
}

int kvm_irqchip_add_hv_sint_route(KVMState *s, uint32_t vcpu, uint32_t sint)
{
    return -ENOSYS;
}

static int kvm_irqchip_assign_irqfd(KVMState *s, int fd, int virq, bool assign)
{
    abort();
}

int kvm_irqchip_update_msi_route(KVMState *s, int virq, MSIMessage msg)
{
    return -ENOSYS;
}
#endif /* !KVM_CAP_IRQ_ROUTING */

int kvm_irqchip_add_irqfd_notifier_gsi(KVMState *s, EventNotifier *n,
                                       EventNotifier *rn, int virq)
{
    return kvm_irqchip_assign_irqfd(s, event_notifier_get_fd(n),
           rn ? event_notifier_get_fd(rn) : -1, virq, true);
}

int kvm_irqchip_remove_irqfd_notifier_gsi(KVMState *s, EventNotifier *n,
                                          int virq)
{
    return kvm_irqchip_assign_irqfd(s, event_notifier_get_fd(n), -1, virq,
           false);
}

int kvm_irqchip_add_irqfd_notifier(KVMState *s, EventNotifier *n,
                                   EventNotifier *rn, qemu_irq irq)
{
    gpointer key, gsi;
    gboolean found = g_hash_table_lookup_extended(s->gsimap, irq, &key, &gsi);

    if (!found) {
        return -ENXIO;
    }
    return kvm_irqchip_add_irqfd_notifier_gsi(s, n, rn, GPOINTER_TO_INT(gsi));
}

int kvm_irqchip_remove_irqfd_notifier(KVMState *s, EventNotifier *n,
                                      qemu_irq irq)
{
    gpointer key, gsi;
    gboolean found = g_hash_table_lookup_extended(s->gsimap, irq, &key, &gsi);

    if (!found) {
        return -ENXIO;
    }
    return kvm_irqchip_remove_irqfd_notifier_gsi(s, n, GPOINTER_TO_INT(gsi));
}

void kvm_irqchip_set_qemuirq_gsi(KVMState *s, qemu_irq irq, int gsi)
{
    g_hash_table_insert(s->gsimap, irq, GINT_TO_POINTER(gsi));
}

static void kvm_irqchip_create(MachineState *machine, KVMState *s)
{
    int ret;

    if (kvm_check_extension(s, KVM_CAP_IRQCHIP)) {
        ;
    } else if (kvm_check_extension(s, KVM_CAP_S390_IRQCHIP)) {
        ret = kvm_vm_enable_cap(s, KVM_CAP_S390_IRQCHIP, 0);
        if (ret < 0) {
            fprintf(stderr, "Enable kernel irqchip failed: %s\n", strerror(-ret));
            exit(1);
        }
    } else {
        return;
    }

    /* First probe and see if there's a arch-specific hook to create the
     * in-kernel irqchip for us */
    ret = kvm_arch_irqchip_create(machine, s);
    if (ret == 0) {
        if (machine_kernel_irqchip_split(machine)) {
            perror("Split IRQ chip mode not supported.");
            exit(1);
        } else {
            ret = kvm_vm_ioctl(s, KVM_CREATE_IRQCHIP);
        }
    }
    if (ret < 0) {
        fprintf(stderr, "Create kernel irqchip failed: %s\n", strerror(-ret));
        exit(1);
    }

    kvm_kernel_irqchip = true;
    /* If we have an in-kernel IRQ chip then we must have asynchronous
     * interrupt delivery (though the reverse is not necessarily true)
     */
    kvm_async_interrupts_allowed = true;
    kvm_halt_in_kernel_allowed = true;

    kvm_init_irq_routing(s);

    s->gsimap = g_hash_table_new(g_direct_hash, g_direct_equal);
}

/* Find number of supported CPUs using the recommended
 * procedure from the kernel API documentation to cope with
 * older kernels that may be missing capabilities.
 */
static int kvm_recommended_vcpus(KVMState *s)
{
    int ret = kvm_vm_check_extension(s, KVM_CAP_NR_VCPUS);
    return (ret) ? ret : 4;
}

static int kvm_max_vcpus(KVMState *s)
{
    int ret = kvm_check_extension(s, KVM_CAP_MAX_VCPUS);
    return (ret) ? ret : kvm_recommended_vcpus(s);
}

static int kvm_max_vcpu_id(KVMState *s)
{
    int ret = kvm_check_extension(s, KVM_CAP_MAX_VCPU_ID);
    return (ret) ? ret : kvm_max_vcpus(s);
}

bool kvm_vcpu_id_is_valid(int vcpu_id)
{
    KVMState *s = KVM_STATE(current_machine->accelerator);
    return vcpu_id >= 0 && vcpu_id < kvm_max_vcpu_id(s);
}

static int kvm_init(MachineState *ms)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    static const char upgrade_note[] =
        "Please upgrade to at least kernel 2.6.29 or recent kvm-kmod\n"
        "(see http://sourceforge.net/projects/kvm).\n";
    struct {
        const char *name;
        int num;
    } num_cpus[] = {
        { "SMP",          smp_cpus },
        { "hotpluggable", max_cpus },
        { NULL, }
    }, *nc = num_cpus;
    int soft_vcpus_limit, hard_vcpus_limit;
    KVMState *s;
    const KVMCapabilityInfo *missing_cap;
    int ret;
    int type = 0;
    const char *kvm_type;

    s = KVM_STATE(ms->accelerator);

    /*
     * On systems where the kernel can support different base page
     * sizes, host page size may be different from TARGET_PAGE_SIZE,
     * even with KVM.  TARGET_PAGE_SIZE is assumed to be the minimum
     * page size for the system though.
     */
    assert(TARGET_PAGE_SIZE <= getpagesize());

    s->sigmask_len = 8;

#ifdef KVM_CAP_SET_GUEST_DEBUG
    QTAILQ_INIT(&s->kvm_sw_breakpoints);
#endif
    QLIST_INIT(&s->kvm_parked_vcpus);
    s->vmfd = -1;
    s->fd = qemu_open("/dev/kvm", O_RDWR);
    if (s->fd == -1) {
        fprintf(stderr, "Could not access KVM kernel module: %m\n");
        ret = -errno;
        goto err;
    }

    ret = kvm_ioctl(s, KVM_GET_API_VERSION, 0);
    if (ret < KVM_API_VERSION) {
        if (ret >= 0) {
            ret = -EINVAL;
        }
        fprintf(stderr, "kvm version too old\n");
        goto err;
    }

    if (ret > KVM_API_VERSION) {
        ret = -EINVAL;
        fprintf(stderr, "kvm version not supported\n");
        goto err;
    }

    kvm_immediate_exit = kvm_check_extension(s, KVM_CAP_IMMEDIATE_EXIT);
    s->nr_slots = kvm_check_extension(s, KVM_CAP_NR_MEMSLOTS);

    /* If unspecified, use the default value */
    if (!s->nr_slots) {
        s->nr_slots = 32;
    }

    kvm_type = qemu_opt_get(qemu_get_machine_opts(), "kvm-type");
    if (mc->kvm_type) {
        type = mc->kvm_type(kvm_type);
    } else if (kvm_type) {
        ret = -EINVAL;
        fprintf(stderr, "Invalid argument kvm-type=%s\n", kvm_type);
        goto err;
    }

    do {
        ret = kvm_ioctl(s, KVM_CREATE_VM, type);
    } while (ret == -EINTR);

    if (ret < 0) {
        fprintf(stderr, "ioctl(KVM_CREATE_VM) failed: %d %s\n", -ret,
                strerror(-ret));

#ifdef TARGET_S390X
        if (ret == -EINVAL) {
            fprintf(stderr,
                    "Host kernel setup problem detected. Please verify:\n");
            fprintf(stderr, "- for kernels supporting the switch_amode or"
                    " user_mode parameters, whether\n");
            fprintf(stderr,
                    "  user space is running in primary address space\n");
            fprintf(stderr,
                    "- for kernels supporting the vm.allocate_pgste sysctl, "
                    "whether it is enabled\n");
        }
#endif
        goto err;
    }

    s->vmfd = ret;

    /* check the vcpu limits */
    soft_vcpus_limit = kvm_recommended_vcpus(s);
    hard_vcpus_limit = kvm_max_vcpus(s);

    while (nc->name) {
        if (nc->num > soft_vcpus_limit) {
            warn_report("Number of %s cpus requested (%d) exceeds "
                        "the recommended cpus supported by KVM (%d)",
                        nc->name, nc->num, soft_vcpus_limit);

            if (nc->num > hard_vcpus_limit) {
                fprintf(stderr, "Number of %s cpus requested (%d) exceeds "
                        "the maximum cpus supported by KVM (%d)\n",
                        nc->name, nc->num, hard_vcpus_limit);
                exit(1);
            }
        }
        nc++;
    }

    missing_cap = kvm_check_extension_list(s, kvm_required_capabilites);
    if (!missing_cap) {
        missing_cap =
            kvm_check_extension_list(s, kvm_arch_required_capabilities);
    }
    if (missing_cap) {
        ret = -EINVAL;
        fprintf(stderr, "kvm does not support %s\n%s",
                missing_cap->name, upgrade_note);
        goto err;
    }

    s->coalesced_mmio = kvm_check_extension(s, KVM_CAP_COALESCED_MMIO);

#ifdef KVM_CAP_VCPU_EVENTS
    s->vcpu_events = kvm_check_extension(s, KVM_CAP_VCPU_EVENTS);
#endif

    s->robust_singlestep =
        kvm_check_extension(s, KVM_CAP_X86_ROBUST_SINGLESTEP);

#ifdef KVM_CAP_DEBUGREGS
    s->debugregs = kvm_check_extension(s, KVM_CAP_DEBUGREGS);
#endif

#ifdef KVM_CAP_IRQ_ROUTING
    kvm_direct_msi_allowed = (kvm_check_extension(s, KVM_CAP_SIGNAL_MSI) > 0);
#endif

    s->intx_set_mask = kvm_check_extension(s, KVM_CAP_PCI_2_3);

    s->irq_set_ioctl = KVM_IRQ_LINE;
    if (kvm_check_extension(s, KVM_CAP_IRQ_INJECT_STATUS)) {
        s->irq_set_ioctl = KVM_IRQ_LINE_STATUS;
    }

#ifdef KVM_CAP_READONLY_MEM
    kvm_readonly_mem_allowed =
        (kvm_check_extension(s, KVM_CAP_READONLY_MEM) > 0);
#endif

    kvm_eventfds_allowed =
        (kvm_check_extension(s, KVM_CAP_IOEVENTFD) > 0);

    kvm_irqfds_allowed =
        (kvm_check_extension(s, KVM_CAP_IRQFD) > 0);

    kvm_resamplefds_allowed =
        (kvm_check_extension(s, KVM_CAP_IRQFD_RESAMPLE) > 0);

    kvm_vm_attributes_allowed =
        (kvm_check_extension(s, KVM_CAP_VM_ATTRIBUTES) > 0);

    kvm_ioeventfd_any_length_allowed =
        (kvm_check_extension(s, KVM_CAP_IOEVENTFD_ANY_LENGTH) > 0);

    kvm_state = s;

    ret = kvm_arch_init(ms, s);
    if (ret < 0) {
        goto err;
    }

    if (machine_kernel_irqchip_allowed(ms)) {
        kvm_irqchip_create(ms, s);
    }

    if (kvm_eventfds_allowed) {
        s->memory_listener.listener.eventfd_add = kvm_mem_ioeventfd_add;
        s->memory_listener.listener.eventfd_del = kvm_mem_ioeventfd_del;
    }
    s->memory_listener.listener.coalesced_mmio_add = kvm_coalesce_mmio_region;
    s->memory_listener.listener.coalesced_mmio_del = kvm_uncoalesce_mmio_region;

    kvm_memory_listener_register(s, &s->memory_listener,
                                 &address_space_memory, 0);
    memory_listener_register(&kvm_io_listener,
                             &address_space_io);

    s->many_ioeventfds = kvm_check_many_ioeventfds();

    s->sync_mmu = !!kvm_vm_check_extension(kvm_state, KVM_CAP_SYNC_MMU);

    return 0;

err:
    assert(ret < 0);
    if (s->vmfd >= 0) {
        close(s->vmfd);
    }
    if (s->fd != -1) {
        close(s->fd);
    }
    g_free(s->memory_listener.slots);

    return ret;
}

void kvm_set_sigmask_len(KVMState *s, unsigned int sigmask_len)
{
    s->sigmask_len = sigmask_len;
}

static void kvm_handle_io(uint16_t port, MemTxAttrs attrs, void *data, int direction,
                          int size, uint32_t count)
{
    int i;
    uint8_t *ptr = data;

    for (i = 0; i < count; i++) {
        address_space_rw(&address_space_io, port, attrs,
                         ptr, size,
                         direction == KVM_EXIT_IO_OUT);
        ptr += size;
    }
}

static int kvm_handle_internal_error(CPUState *cpu, struct kvm_run *run)
{
    fprintf(stderr, "KVM internal error. Suberror: %d\n",
            run->internal.suberror);

    if (kvm_check_extension(kvm_state, KVM_CAP_INTERNAL_ERROR_DATA)) {
        int i;

        for (i = 0; i < run->internal.ndata; ++i) {
            fprintf(stderr, "extra data[%d]: %"PRIx64"\n",
                    i, (uint64_t)run->internal.data[i]);
        }
    }
    if (run->internal.suberror == KVM_INTERNAL_ERROR_EMULATION) {
        fprintf(stderr, "emulation failure\n");
        if (!kvm_arch_stop_on_emulation_error(cpu)) {
            cpu_dump_state(cpu, stderr, fprintf, CPU_DUMP_CODE);
            return EXCP_INTERRUPT;
        }
    }
    /* FIXME: Should trigger a qmp message to let management know
     * something went wrong.
     */
    return -1;
}

void kvm_flush_coalesced_mmio_buffer(void)
{
    KVMState *s = kvm_state;

    if (s->coalesced_flush_in_progress) {
        return;
    }

    s->coalesced_flush_in_progress = true;

    if (s->coalesced_mmio_ring) {
        struct kvm_coalesced_mmio_ring *ring = s->coalesced_mmio_ring;
        while (ring->first != ring->last) {
            struct kvm_coalesced_mmio *ent;

            ent = &ring->coalesced_mmio[ring->first];

            cpu_physical_memory_write(ent->phys_addr, ent->data, ent->len);
            smp_wmb();
            ring->first = (ring->first + 1) % KVM_COALESCED_MMIO_MAX;
        }
    }

    s->coalesced_flush_in_progress = false;
}

static void do_kvm_cpu_synchronize_state(CPUState *cpu, run_on_cpu_data arg)
{
    if (!cpu->vcpu_dirty) {
        kvm_arch_get_registers(cpu);
        cpu->vcpu_dirty = true;
    }
}

void kvm_cpu_synchronize_state(CPUState *cpu)
{
    if (!cpu->vcpu_dirty) {
        run_on_cpu(cpu, do_kvm_cpu_synchronize_state, RUN_ON_CPU_NULL);
    }
}

static void do_kvm_cpu_synchronize_post_reset(CPUState *cpu, run_on_cpu_data arg)
{
    kvm_arch_put_registers(cpu, KVM_PUT_RESET_STATE);
    cpu->vcpu_dirty = false;
}

void kvm_cpu_synchronize_post_reset(CPUState *cpu)
{
    run_on_cpu(cpu, do_kvm_cpu_synchronize_post_reset, RUN_ON_CPU_NULL);
}

static void do_kvm_cpu_synchronize_post_init(CPUState *cpu, run_on_cpu_data arg)
{
    kvm_arch_put_registers(cpu, KVM_PUT_FULL_STATE);
    cpu->vcpu_dirty = false;
}

void kvm_cpu_synchronize_post_init(CPUState *cpu)
{
    run_on_cpu(cpu, do_kvm_cpu_synchronize_post_init, RUN_ON_CPU_NULL);
}

static void do_kvm_cpu_synchronize_pre_loadvm(CPUState *cpu, run_on_cpu_data arg)
{
    cpu->vcpu_dirty = true;
}

void kvm_cpu_synchronize_pre_loadvm(CPUState *cpu)
{
    run_on_cpu(cpu, do_kvm_cpu_synchronize_pre_loadvm, RUN_ON_CPU_NULL);
}

#ifdef KVM_HAVE_MCE_INJECTION
static __thread void *pending_sigbus_addr;
static __thread int pending_sigbus_code;
static __thread bool have_sigbus_pending;
#endif

static void kvm_cpu_kick(CPUState *cpu)
{
    atomic_set(&cpu->kvm_run->immediate_exit, 1);
}

static void kvm_cpu_kick_self(void)
{
    if (kvm_immediate_exit) {
        kvm_cpu_kick(current_cpu);
    } else {
        qemu_cpu_kick_self();
    }
}

static void kvm_eat_signals(CPUState *cpu)
{
    struct timespec ts = { 0, 0 };
    siginfo_t siginfo;
    sigset_t waitset;
    sigset_t chkset;
    int r;

    if (kvm_immediate_exit) {
        atomic_set(&cpu->kvm_run->immediate_exit, 0);
        /* Write kvm_run->immediate_exit before the cpu->exit_request
         * write in kvm_cpu_exec.
         */
        smp_wmb();
        return;
    }

    sigemptyset(&waitset);
    sigaddset(&waitset, SIG_IPI);

    do {
        r = sigtimedwait(&waitset, &siginfo, &ts);
        if (r == -1 && !(errno == EAGAIN || errno == EINTR)) {
            perror("sigtimedwait");
            exit(1);
        }

        r = sigpending(&chkset);
        if (r == -1) {
            perror("sigpending");
            exit(1);
        }
    } while (sigismember(&chkset, SIG_IPI));
}

/*
 * Process a request for pages received on the return path,
 * We're allowed to send more than requested (e.g. to round to our page size)
 * and we don't need to send pages that have already been sent.
 */
static void migrate_handle_rp_req_pages(MigrationState *ms, const char* rbname,
                                       ram_addr_t start, size_t len)
{
    long our_host_ps = getpagesize();

    //trace_migrate_handle_rp_req_pages(rbname, start, len);

    /*
     * Since we currently insist on matching page sizes, just sanity check
     * we're being asked for whole host pages.
     */
    if (start & (our_host_ps-1) ||
       (len & (our_host_ps-1))) {
        error_report("%s: Misaligned page request, start: " RAM_ADDR_FMT
                     " len: %zd", __func__, start, len);
        mark_source_rp_bad(ms);
        return;
    }

    if (ram_save_queue_pages(rbname, start, len)) {
        mark_source_rp_bad(ms);
    }
}

/*
 * Return true if we're already in the middle of a migration
 * (i.e. any of the active or setup states)
 */
static bool migration_is_setup_or_active(int state)
{
    switch (state) {
    case MIGRATION_STATUS_ACTIVE:
    case MIGRATION_STATUS_POSTCOPY_ACTIVE:
    case MIGRATION_STATUS_SETUP:
    case MIGRATION_STATUS_PRE_SWITCHOVER:
    case MIGRATION_STATUS_DEVICE:
        return true;

    default:
        return false;

    }
}

static void migrate_set_block_incremental(MigrationState *s, bool value)
{
    s->parameters.block_incremental = value;
}

/* migration thread support */
/*
 * Something bad happened to the RP stream, mark an error
 * The caller shall print or trace something to indicate why
 */
static void mark_source_rp_bad(MigrationState *s)
{
    s->rp_state.error = true;
}

static void block_cleanup_parameters(MigrationState *s)
{
    if (s->must_remove_block_options) {
        /* setting to false can never fail */
        migrate_set_block_enabled(false, &error_abort);
        migrate_set_block_incremental(s, false);
        s->must_remove_block_options = false;
    }
}

/*
 * Handles messages sent on the return path towards the source VM
 *
 */
static void *source_return_path_thread(void *opaque)
{
    MigrationState *ms = opaque;
    QEMUFile *rp = ms->rp_state.from_dst_file;
    uint16_t header_len, header_type;
    uint8_t buf[512];
    uint32_t tmp32, sibling_error;
    ram_addr_t start = 0; /* =0 to silence warning */
    size_t  len = 0, expected_len;
    int res;

    //trace_source_return_path_thread_entry();
    while (!ms->rp_state.error && !qemu_file_get_error(rp) &&
           migration_is_setup_or_active(ms->state)) {
        //trace_source_return_path_thread_loop_top();
        header_type = qemu_get_be16(rp);
        header_len = qemu_get_be16(rp);

        if (header_type >= MIG_RP_MSG_MAX ||
            header_type == MIG_RP_MSG_INVALID) {
            error_report("RP: Received invalid message 0x%04x length 0x%04x",
                    header_type, header_len);
            mark_source_rp_bad(ms);
            goto out;
        }

        if ((rp_cmd_args[header_type].len != -1 &&
            header_len != rp_cmd_args[header_type].len) ||
            header_len > sizeof(buf)) {
            error_report("RP: Received '%s' message (0x%04x) with"
                    "incorrect length %d expecting %zu",
                    rp_cmd_args[header_type].name, header_type, header_len,
                    (size_t)rp_cmd_args[header_type].len);
            mark_source_rp_bad(ms);
            goto out;
        }

        /* We know we've got a valid header by this point */
        res = qemu_get_buffer(rp, buf, header_len);
        if (res != header_len) {
            error_report("RP: Failed reading data for message 0x%04x"
                         " read %d expected %d",
                         header_type, res, header_len);
            mark_source_rp_bad(ms);
            goto out;
        }

        /* OK, we have the message and the data */
        switch (header_type) {
        case MIG_RP_MSG_SHUT:
            sibling_error = ldl_be_p(buf);
            //trace_source_return_path_thread_shut(sibling_error);
            if (sibling_error) {
                error_report("RP: Sibling indicated error %d", sibling_error);
                mark_source_rp_bad(ms);
            }
            /*
             * We'll let the main thread deal with closing the RP
             * we could do a shutdown(2) on it, but we're the only user
             * anyway, so there's nothing gained.
             */
            goto out;

        case MIG_RP_MSG_PONG:
            tmp32 = ldl_be_p(buf);
            //trace_source_return_path_thread_pong(tmp32);
            break;

        case MIG_RP_MSG_REQ_PAGES:
            start = ldq_be_p(buf);
            len = ldl_be_p(buf + 8);
            migrate_handle_rp_req_pages(ms, NULL, start, len);
            break;

        case MIG_RP_MSG_REQ_PAGES_ID:
            expected_len = 12 + 1; /* header + termination */

            if (header_len >= expected_len) {
                start = ldq_be_p(buf);
                len = ldl_be_p(buf + 8);
                /* Now we expect an idstr */
                tmp32 = buf[12]; /* Length of the following idstr */
                buf[13 + tmp32] = '\0';
                expected_len += tmp32;
            }
            if (header_len != expected_len) {
                error_report("RP: Req_Page_id with length %d expecting %zd",
                        header_len, expected_len);
                mark_source_rp_bad(ms);
                goto out;
            }
            migrate_handle_rp_req_pages(ms, (char *)&buf[13], start, len);
            break;

        default:
            break;
        }
    }
    if (qemu_file_get_error(rp)) {
        //trace_source_return_path_thread_bad_end();
        mark_source_rp_bad(ms);
    }

    //trace_source_return_path_thread_end();
out:
    ms->rp_state.from_dst_file = NULL;
    qemu_fclose(rp);
    return NULL;
}

static int open_return_path_on_source(MigrationState *ms)
{

    ms->rp_state.from_dst_file = qemu_file_get_return_path(ms->to_dst_file);
    if (!ms->rp_state.from_dst_file) {
        return -1;
    }

    //trace_open_return_path_on_source();
    qemu_thread_create(&ms->rp_state.rp_thread, "return path",
                       source_return_path_thread, ms, QEMU_THREAD_JOINABLE);

    //trace_open_return_path_on_source_continue();

    return 0;
}

static void migrate_fd_cleanup(void *opaque)
{
    MigrationState *s = opaque;

    qemu_bh_delete(s->cleanup_bh);
    s->cleanup_bh = NULL;

    if (s->to_dst_file) {
        Error *local_err = NULL;

        //trace_migrate_fd_cleanup();
        qemu_mutex_unlock_iothread();
        if (s->migration_thread_running) {
            qemu_thread_join(&s->thread);
            s->migration_thread_running = false;
        }
        qemu_mutex_lock_iothread();

        if (multifd_save_cleanup(&local_err) != 0) {
            error_report_err(local_err);
        }
        qemu_fclose(s->to_dst_file);
        s->to_dst_file = NULL;
    }

    assert((s->state != MIGRATION_STATUS_ACTIVE) &&
           (s->state != MIGRATION_STATUS_POSTCOPY_ACTIVE));

    if (s->state == MIGRATION_STATUS_CANCELLING) {
        migrate_set_state(&s->state, MIGRATION_STATUS_CANCELLING,
                          MIGRATION_STATUS_CANCELLED);
    }

    if (s->error) {
        /* It is used on info migrate.  We can't free it */
        error_report_err(error_copy(s->error));
    }
    notifier_list_notify(&migration_state_notifiers, s);
    block_cleanup_parameters(s);
}

/* Returns 0 if the RP was ok, otherwise there was an error on the RP */
static int await_return_path_close_on_source(MigrationState *ms)
{
    /*
     * If this is a normal exit then the destination will send a SHUT and the
     * rp_thread will exit, however if there's an error we need to cause
     * it to exit.
     */
    if (qemu_file_get_error(ms->to_dst_file) && ms->rp_state.from_dst_file) {
        /*
         * shutdown(2), if we have it, will cause it to unblock if it's stuck
         * waiting for the destination.
         */
        qemu_file_shutdown(ms->rp_state.from_dst_file);
        mark_source_rp_bad(ms);
    }
    //trace_await_return_path_close_on_source_joining();
    qemu_thread_join(&ms->rp_state.rp_thread);
    //trace_await_return_path_close_on_source_close();
    return ms->rp_state.error;
}

/**
 * migration_maybe_pause: Pause if required to by
 * migrate_pause_before_switchover called with the iothread locked
 * Returns: 0 on success
 */
static int migration_maybe_pause(MigrationState *s,
                                 int *current_active_state,
                                 int new_state)
{
    if (!migrate_pause_before_switchover()) {
        return 0;
    }

    /* Since leaving this state is not atomic with posting the semaphore
     * it's possible that someone could have issued multiple migrate_continue
     * and the semaphore is incorrectly positive at this point;
     * the docs say it's undefined to reinit a semaphore that's already
     * init'd, so use timedwait to eat up any existing posts.
     */
    while (qemu_sem_timedwait(&s->pause_sem, 1) == 0) {
        /* This block intentionally left blank */
    }

    qemu_mutex_unlock_iothread();
    migrate_set_state(&s->state, *current_active_state,
                      MIGRATION_STATUS_PRE_SWITCHOVER);
    qemu_sem_wait(&s->pause_sem);
    migrate_set_state(&s->state, MIGRATION_STATUS_PRE_SWITCHOVER,
                      new_state);
    *current_active_state = new_state;
    qemu_mutex_lock_iothread();

    return s->state == new_state ? 0 : -EINVAL;
}

/**
 * migration_completion: Used by migration_thread when there's not much left.
 *   The caller 'breaks' the loop when this returns.
 *
 * @s: Current migration state
 * @current_active_state: The migration state we expect to be in
 * @*old_vm_running: Pointer to old_vm_running flag
 * @*start_time: Pointer to time to update
 */
static void migration_completion(MigrationState *s, int current_active_state,
                                 bool *old_vm_running,
                                 int64_t *start_time)
{
    int ret;

    if (s->state == MIGRATION_STATUS_ACTIVE) {
        qemu_mutex_lock_iothread();
        *start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER);
        *old_vm_running = runstate_is_running();
        ret = global_state_store();

        if (!ret) {
            bool inactivate = !migrate_colo_enabled();
            ret = vm_stop_force_state(RUN_STATE_FINISH_MIGRATE);
            if (ret >= 0) {
                ret = migration_maybe_pause(s, &current_active_state,
                                            MIGRATION_STATUS_DEVICE);
            }
            if (ret >= 0) {
                qemu_file_set_rate_limit(s->to_dst_file, INT64_MAX);
                ret = qemu_savevm_state_complete_precopy(s->to_dst_file, false,
                                                         inactivate);
            }
            if (inactivate && ret >= 0) {
                s->block_inactive = true;
            }
        }
        qemu_mutex_unlock_iothread();

        if (ret < 0) {
            goto fail;
        }
    } else if (s->state == MIGRATION_STATUS_POSTCOPY_ACTIVE) {
        //trace_migration_completion_postcopy_end();

        qemu_savevm_state_complete_postcopy(s->to_dst_file);
        //trace_migration_completion_postcopy_end_after_complete();
    }

    /*
     * If rp was opened we must clean up the thread before
     * cleaning everything else up (since if there are no failures
     * it will wait for the destination to send it's status in
     * a SHUT command).
     */
    if (s->rp_state.from_dst_file) {
        int rp_error;
        //trace_migration_return_path_end_before();
        rp_error = await_return_path_close_on_source(s);
        //trace_migration_return_path_end_after(rp_error);
        if (rp_error) {
            goto fail_invalidate;
        }
    }

    if (qemu_file_get_error(s->to_dst_file)) {
        //trace_migration_completion_file_err();
        goto fail_invalidate;
    }

    if (!migrate_colo_enabled()) {
        migrate_set_state(&s->state, current_active_state,
                          MIGRATION_STATUS_COMPLETED);
    }

    return;

fail_invalidate:
    /* If not doing postcopy, vm_start() will be called: let's regain
     * control on images.
     */
    if (s->state == MIGRATION_STATUS_ACTIVE ||
        s->state == MIGRATION_STATUS_DEVICE) {
        Error *local_err = NULL;

        qemu_mutex_lock_iothread();
        bdrv_invalidate_cache_all(&local_err);
        if (local_err) {
            error_report_err(local_err);
        } else {
            s->block_inactive = false;
        }
        qemu_mutex_unlock_iothread();
    }

fail:
    migrate_set_state(&s->state, current_active_state,
                      MIGRATION_STATUS_FAILED);
}

/*
 * Switch from normal iteration to postcopy
 * Returns non-0 on error
 */
static int postcopy_start(MigrationState *ms, bool *old_vm_running)
{
    int ret;
    QIOChannelBuffer *bioc;
    QEMUFile *fb;
    int64_t time_at_stop = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    bool restart_block = false;
    int cur_state = MIGRATION_STATUS_ACTIVE;
    if (!migrate_pause_before_switchover()) {
        migrate_set_state(&ms->state, MIGRATION_STATUS_ACTIVE,
                          MIGRATION_STATUS_POSTCOPY_ACTIVE);
    }

    //trace_postcopy_start();
    qemu_mutex_lock_iothread();
    //trace_postcopy_start_set_run();

    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_OTHER);
    *old_vm_running = runstate_is_running();
    global_state_store();
    ret = vm_stop_force_state(RUN_STATE_FINISH_MIGRATE);
    if (ret < 0) {
        goto fail;
    }

    ret = migration_maybe_pause(ms, &cur_state,
                                MIGRATION_STATUS_POSTCOPY_ACTIVE);
    if (ret < 0) {
        goto fail;
    }

    ret = bdrv_inactivate_all();
    if (ret < 0) {
        goto fail;
    }
    restart_block = true;

    /*
     * Cause any non-postcopiable, but iterative devices to
     * send out their final data.
     */
    qemu_savevm_state_complete_precopy(ms->to_dst_file, true, false);

    /*
     * in Finish migrate and with the io-lock held everything should
     * be quiet, but we've potentially still got dirty pages and we
     * need to tell the destination to throw any pages it's already received
     * that are dirty
     */
    if (migrate_postcopy_ram()) {
        if (ram_postcopy_send_discard_bitmap(ms)) {
            error_report("postcopy send discard bitmap failed");
            goto fail;
        }
    }

    /*
     * send rest of state - note things that are doing postcopy
     * will notice we're in POSTCOPY_ACTIVE and not actually
     * wrap their state up here
     */
    qemu_file_set_rate_limit(ms->to_dst_file, INT64_MAX);
    if (migrate_postcopy_ram()) {
        /* Ping just for debugging, helps line traces up */
        qemu_savevm_send_ping(ms->to_dst_file, 2);
    }

    /*
     * While loading the device state we may trigger page transfer
     * requests and the fd must be free to process those, and thus
     * the destination must read the whole device state off the fd before
     * it starts processing it.  Unfortunately the ad-hoc migration format
     * doesn't allow the destination to know the size to read without fully
     * parsing it through each devices load-state code (especially the open
     * coded devices that use get/put).
     * So we wrap the device state up in a package with a length at the start;
     * to do this we use a qemu_buf to hold the whole of the device state.
     */
    bioc = qio_channel_buffer_new(4096);
    qio_channel_set_name(QIO_CHANNEL(bioc), "migration-postcopy-buffer");
    fb = qemu_fopen_channel_output(QIO_CHANNEL(bioc));
    object_unref(OBJECT(bioc));

    /*
     * Make sure the receiver can get incoming pages before we send the rest
     * of the state
     */
    qemu_savevm_send_postcopy_listen(fb);

    qemu_savevm_state_complete_precopy(fb, false, false);
    if (migrate_postcopy_ram()) {
        qemu_savevm_send_ping(fb, 3);
    }

    qemu_savevm_send_postcopy_run(fb);

    /* <><> end of stuff going into the package */

    /* Last point of recovery; as soon as we send the package the destination
     * can open devices and potentially start running.
     * Lets just check again we've not got any errors.
     */
    ret = qemu_file_get_error(ms->to_dst_file);
    if (ret) {
        error_report("postcopy_start: Migration stream errored (pre package)");
        goto fail_closefb;
    }

    restart_block = false;

    /* Now send that blob */
    if (qemu_savevm_send_packaged(ms->to_dst_file, bioc->data, bioc->usage)) {
        goto fail_closefb;
    }
    qemu_fclose(fb);

    /* Send a notify to give a chance for anything that needs to happen
     * at the transition to postcopy and after the device state; in particular
     * spice needs to trigger a transition now
     */
    ms->postcopy_after_devices = true;
    notifier_list_notify(&migration_state_notifiers, ms);

    ms->downtime =  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - time_at_stop;

    qemu_mutex_unlock_iothread();

    if (migrate_postcopy_ram()) {
        /*
         * Although this ping is just for debug, it could potentially be
         * used for getting a better measurement of downtime at the source.
         */
        qemu_savevm_send_ping(ms->to_dst_file, 4);
    }

    if (migrate_release_ram()) {
        ram_postcopy_migrated_memory_release(ms);
    }

    ret = qemu_file_get_error(ms->to_dst_file);
    if (ret) {
        error_report("postcopy_start: Migration stream errored");
        migrate_set_state(&ms->state, MIGRATION_STATUS_POSTCOPY_ACTIVE,
                              MIGRATION_STATUS_FAILED);
    }

    return ret;

fail_closefb:
    qemu_fclose(fb);
fail:
    migrate_set_state(&ms->state, MIGRATION_STATUS_POSTCOPY_ACTIVE,
                          MIGRATION_STATUS_FAILED);
    if (restart_block) {
        /* A failure happened early enough that we know the destination hasn't
         * accessed block devices, so we're safe to recover.
         */
        Error *local_err = NULL;

        bdrv_invalidate_cache_all(&local_err);
        if (local_err) {
            error_report_err(local_err);
        }
    }
    qemu_mutex_unlock_iothread();
    return -1;
}

/*
 * Master migration thread on the source VM.
 * It drives the migration and pumps the data down the outgoing channel.
 */
static void *my_migration_thread(void *opaque)
{
    MigrationState *s = opaque;
    /* Used by the bandwidth calcs, updated later */
    int64_t initial_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t setup_start = qemu_clock_get_ms(QEMU_CLOCK_HOST);
    int64_t initial_bytes = 0;
    /*
     * The final stage happens when the remaining data is smaller than
     * this threshold; it's calculated from the requested downtime and
     * measured bandwidth
     */
    int64_t threshold_size = 0;
    int64_t start_time = initial_time;
    int64_t end_time;
    bool old_vm_running = false;
    bool entered_postcopy = false;
    /* The active state we expect to be in; ACTIVE or POSTCOPY_ACTIVE */
    enum MigrationStatus current_active_state = MIGRATION_STATUS_ACTIVE;
    bool enable_colo = migrate_colo_enabled();

    FILE *fp = fopen("/tmp/migration_thread", "w+");
    if (runstate_is_running())
	    fprintf(fp, "ma trexw\n");
    else
	    fprintf(fp, "den trexw\n");
    rcu_register_thread();

    qemu_savevm_state_header(s->to_dst_file);

    /*
     * If we opened the return path, we need to make sure dst has it
     * opened as well.
     */
    if (s->rp_state.from_dst_file) {
        /* Now tell the dest that it should open its end so it can reply */
        qemu_savevm_send_open_return_path(s->to_dst_file);

        /* And do a ping that will make stuff easier to debug */
        qemu_savevm_send_ping(s->to_dst_file, 1);
    }

    if (migrate_postcopy()) {
	    fprintf(fp, "ti postcopy re tsrlatane?\n");
        /*
         * Tell the destination that we *might* want to do postcopy later;
         * if the other end can't do postcopy it should fail now, nice and
         * early.
         */
        qemu_savevm_send_postcopy_advise(s->to_dst_file);
    }

    qemu_savevm_state_setup(s->to_dst_file);

    s->setup_time = qemu_clock_get_ms(QEMU_CLOCK_HOST) - setup_start;
    migrate_set_state(&s->state, MIGRATION_STATUS_SETUP,
                      MIGRATION_STATUS_ACTIVE);

    //trace_migration_thread_setup_complete();

    if (runstate_is_running())
	    fprintf(fp, "ma trexw 1\n");
    else
	    fprintf(fp, "den trexw 1\n");
    while (s->state == MIGRATION_STATUS_ACTIVE ||
           s->state == MIGRATION_STATUS_POSTCOPY_ACTIVE) {
        int64_t current_time;
        uint64_t pending_size;

        if (!qemu_file_rate_limit(s->to_dst_file)) {
            uint64_t pend_post, pend_nonpost;

            qemu_savevm_state_pending(s->to_dst_file, threshold_size,
                                      &pend_nonpost, &pend_post);
            pending_size = pend_nonpost + pend_post;
            //trace_migrate_pending(pending_size, threshold_size,
                                  //pend_post, pend_nonpost);
            if (pending_size && pending_size >= threshold_size) {
                /* Still a significant amount to transfer */

                if (migrate_postcopy() &&
                    s->state != MIGRATION_STATUS_POSTCOPY_ACTIVE &&
                    pend_nonpost <= threshold_size &&
                    atomic_read(&s->start_postcopy)) {
			fprintf(fp, "ti postcopy re tsrlatane?\n");

                    if (!postcopy_start(s, &old_vm_running)) {
                        current_active_state = MIGRATION_STATUS_POSTCOPY_ACTIVE;
                        entered_postcopy = true;
			fprintf(fp, "ti postcopy re tsrlatane?\n");
                    }

                    continue;
                }
                /* Just another iteration step */
                qemu_savevm_state_iterate(s->to_dst_file, entered_postcopy);
            } else {
                //trace_migration_thread_low_pending(pending_size);
                migration_completion(s, current_active_state,
                                     &old_vm_running, &start_time);
                break;
            }
        }

        if (qemu_file_get_error(s->to_dst_file)) {
            migrate_set_state(&s->state, current_active_state,
                              MIGRATION_STATUS_FAILED);
            //trace_migration_thread_file_err();
            break;
        }
        current_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        if (current_time >= initial_time + BUFFER_DELAY) {
            uint64_t transferred_bytes = qemu_ftell(s->to_dst_file) -
                                         initial_bytes;
            uint64_t time_spent = current_time - initial_time;
            double bandwidth = (double)transferred_bytes / time_spent;
            threshold_size = bandwidth * s->parameters.downtime_limit;

            s->mbps = (((double) transferred_bytes * 8.0) /
                    ((double) time_spent / 1000.0)) / 1000.0 / 1000.0;

            //trace_migrate_transferred(transferred_bytes, time_spent,
                                      //bandwidth, threshold_size);
            /* if we haven't sent anything, we don't want to recalculate
               10000 is a small enough number for our purposes */
            if (ram_counters.dirty_pages_rate && transferred_bytes > 10000) {
                s->expected_downtime = ram_counters.dirty_pages_rate *
                    qemu_target_page_size() / bandwidth;
            }

            qemu_file_reset_rate_limit(s->to_dst_file);
            initial_time = current_time;
            initial_bytes = qemu_ftell(s->to_dst_file);
        }
        if (qemu_file_rate_limit(s->to_dst_file)) {
            /* usleep expects microseconds */
            g_usleep((initial_time + BUFFER_DELAY - current_time)*1000);
        }
    }
    if (runstate_is_running())
	    fprintf(fp, "ma trexw 2\n");
    else
	    fprintf(fp, "den trexw 2\n");

    //trace_migration_thread_after_loop();
    /* If we enabled cpu throttling for auto-converge, turn it off. */
    cpu_throttle_stop();
    end_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    qemu_mutex_lock_iothread();
    /*
     * The resource has been allocated by migration will be reused in COLO
     * process, so don't release them.
     */
    if (!enable_colo) {
        qemu_savevm_state_cleanup();
    }
    if (s->state == MIGRATION_STATUS_COMPLETED) {
        uint64_t transferred_bytes = qemu_ftell(s->to_dst_file);
        s->total_time = end_time - s->total_time;
        if (!entered_postcopy) {
            s->downtime = end_time - start_time;
        }
        if (s->total_time) {
            s->mbps = (((double) transferred_bytes * 8.0) /
                       ((double) s->total_time)) / 1000;
        }
        runstate_set(RUN_STATE_POSTMIGRATE);
    } else {
        if (s->state == MIGRATION_STATUS_ACTIVE && enable_colo) {
            migrate_start_colo_process(s);
            qemu_savevm_state_cleanup();
            /*
            * Fixme: we will run VM in COLO no matter its old running state.
            * After exited COLO, we will keep running.
            */
            old_vm_running = true;
        }
        if (old_vm_running && !entered_postcopy) {
            vm_start();
        } else {
            if (runstate_check(RUN_STATE_FINISH_MIGRATE)) {
                runstate_set(RUN_STATE_POSTMIGRATE);
            }
        }
    }
    Error *err = NULL;
    qmp_cont(&err);
    qemu_bh_schedule(s->cleanup_bh);
    qemu_mutex_unlock_iothread();

    rcu_unregister_thread();
    if (runstate_is_running())
	    fprintf(fp, "ma trexw\n");
    else
	    fprintf(fp, "den trexw\n");
    fclose(fp);
    return NULL;
}

void my_migrate_fd_connect(MigrationState *s)
{
    s->expected_downtime = s->parameters.downtime_limit;
    s->cleanup_bh = qemu_bh_new(migrate_fd_cleanup, s);

    qemu_file_set_blocking(s->to_dst_file, true);
    qemu_file_set_rate_limit(s->to_dst_file,
                             s->parameters.max_bandwidth / XFER_LIMIT_RATIO);

    /* Notify before starting migration thread */
    notifier_list_notify(&migration_state_notifiers, s);

    /*
     * Open the return path. For postcopy, it is used exclusively. For
     * precopy, only if user specified "return-path" capability would
     * QEMU uses the return path.
     */
    if (migrate_postcopy_ram() || migrate_use_return_path()) {
        if (open_return_path_on_source(s)) {
            error_report("Unable to open return-path for postcopy");
            migrate_set_state(&s->state, MIGRATION_STATUS_SETUP,
                              MIGRATION_STATUS_FAILED);
            migrate_fd_cleanup(s);
            return;
        }
    }

    if (multifd_save_setup() != 0) {
        migrate_set_state(&s->state, MIGRATION_STATUS_SETUP,
                          MIGRATION_STATUS_FAILED);
        migrate_fd_cleanup(s);
        return;
    }
    qemu_thread_create(&s->thread, "live_migration", my_migration_thread, s,
                       QEMU_THREAD_JOINABLE);
    s->migration_thread_running = true;
}

/**
 * @my_migration_channel_connect - Create new outgoing migration channel
 *
 * @s: Current migration state
 * @ioc: Channel to which we are connecting
 * @hostname: Where we want to connect
 */
void my_migration_channel_connect(MigrationState *s,
                               QIOChannel *ioc,
                               const char *hostname)
{
    //trace_migration_set_outgoing_channel(
        //ioc, object_get_typename(OBJECT(ioc)), hostname);

    if (s->parameters.tls_creds &&
        *s->parameters.tls_creds &&
        !object_dynamic_cast(OBJECT(ioc),
                             TYPE_QIO_CHANNEL_TLS)) {
        Error *local_err = NULL;
        migration_tls_channel_connect(s, ioc, hostname, &local_err);
        if (local_err) {
            migrate_fd_error(s, local_err);
            error_free(local_err);
        }
    } else {
        QEMUFile *f = qemu_fopen_channel_output(ioc);

        s->to_dst_file = f;

        my_migrate_fd_connect(s);
    }
}

void my_exec_start_outgoing_migration(MigrationState *s, const char *command, Error **errp)
{
    QIOChannel *ioc;
    const char *argv[] = { "/bin/sh", "-c", command, NULL };

    //trace_migration_exec_outgoing(command);
    ioc = QIO_CHANNEL(qio_channel_command_new_spawn(argv,
                                                    O_RDWR,
                                                    errp));
    if (!ioc) {
        return;
    }

    qio_channel_set_name(ioc, "migration-exec-outgoing");
    my_migration_channel_connect(s, ioc, NULL);
    object_unref(OBJECT(ioc));
}

void my_start_migration(void *data)
{
	uint8_t *ptr = data;
	pid_t p = 0;
	stl_p(ptr,p);
	const char uri[] = "exec:cat > /tmp/vm_migration.out", *pa;
	Error *errp = NULL;
	MigrationState *s;
	s = migrate_init();
	strstart(uri, "exec:", &pa);
	my_exec_start_outgoing_migration(s, pa, &errp);
}

/* fork hypercall from guest, spawns a new qemu process that starts server */
void my_fork(void *data, bool *check)
{
	uint8_t *ptr = data;
	pid_t p = 0;
	
	MigrationState *s = migrate_get_current();
	FILE *fp = fopen("/tmp/xamos11", "a+");
	if(s->migration_thread_running == true) 
		fprintf(fp, "trexeiiiii\n");
	else
		fprintf(fp, "den trexeiiiii\n");
	fclose(fp);
	//if (*check == true) {
	//	FILE *fp = fopen("/tmp/xamos", "a+");
	//	fprintf(fp, "eleos: %d\n", my_cnt);
	//	fclose(fp);
	//	my_cnt++;
	//	if(s->migration_thread_running == true) {
	//		return;
	//	}
	//	*check = false;
	//	stl_p(ptr,1);
	//	return;
	//}
	FILE *fp1 = fopen("/tmp/xamos", "a+");
	fprintf(fp1, "eleos: %d\n", my_cnt);
	fclose(fp1);
	if(s->migration_thread_running == true) {
		my_cnt = 1;
		stl_p(ptr,p);
		return;
	}
	if(my_cnt > 0) {
		if (*check == true) {
			stl_p(ptr,1);
			*check = false;
			return;
		}
	} else {
		stl_p(ptr,2);
		return;
	}

	/* migration code */
	//static char temp_file[] = "/tmp/migrate.XXXXXX";
	//Error *errp = NULL;
	//MigrationState *s = migrate_get_current();
	//s = migrate_init();
	//FILE *fp = fopen("/tmp/xamos", "w+");
	//fprintf(fp, "eleos\n");
	//if (runstate_is_running())
	//	fprintf(fp, "ma trexw\n");
	//else
	//	fprintf(fp, "den trexw\n");
	//fclose(fp);
	//QIOChannel *ioc;
	////int fd = mkstemp(temp_file);
	//int fd = open (temp_file, O_RDWR | O_CREAT | O_EXCL | O_BINARY, S_IREAD | S_IWRITE);
	//int fd_dp = dup(fd);
	//ioc = qio_channel_new_fd(fd_dp, &errp);
	//if (!ioc) {
	//	close(fd);
	//	exit(1);
	//}
	//qio_channel_set_name(QIO_CHANNEL(ioc), "my-migration-fd-outgoing");
	//migration_channel_connect(s, ioc, NULL);
	//object_unref(OBJECT(ioc));
	/* prepei na allaksw to running state giati to migration kanei pause
	 * to vm */

	//fd_start_outgoing_migration(s, "/tmp/migration", &local_err);
	//fp = fopen("/tmp/xamos", "w+");
	//fprintf(fp, "eleos 2\n");
	//fclose(fp);
	p = fork();
	//p = 2;
	if (p == 0) {
		/* child */
		const char *argv[] = {QEMU_BIN, "-net", "none", "-enable-kvm", "-cpu", "host", "-m", "64", "-vga", "none", "-nographic", "-device", "ivshmem-plain,memdev=hostmem,master=on", "-object", "memory-backend-file,size=1M,share,mem-path=/dev/shm/ivshmem,id=hostmem", "-kernel", "server-rumprun.bin", "-incoming", "exec: cat /tmp/vm_migration.out",(char *)0};
		const char *envp[] = {(char *)0};
		/* redirect output of child in a special file 
		 * therefore child and parent will not fight over stdout */
		int fd = open("/tmp/my_server.out", O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
		dup2(fd, 1);   // make stdout go to file
		dup2(fd, 2);   // make stderr go to file
		close(fd);
		execve(argv[0], &argv[0], envp);
		/* control should not reach this code */
		perror("execve");
		exit(1);
	} else if (p == -1) {
		/* error */
		perror("fork");
		exit(1);
	} else {
		/* parent */
		stl_p(ptr,p);
		return;
	}
}

int kvm_cpu_exec(CPUState *cpu)
{
    struct kvm_run *run = cpu->kvm_run;
    int ret, run_ret;
    bool check = true;

    DPRINTF("kvm_cpu_exec()\n");

    if (kvm_arch_process_async_events(cpu)) {
        atomic_set(&cpu->exit_request, 0);
        return EXCP_HLT;
    }

    qemu_mutex_unlock_iothread();
    cpu_exec_start(cpu);

    do {
        MemTxAttrs attrs;

        if (cpu->vcpu_dirty) {
            kvm_arch_put_registers(cpu, KVM_PUT_RUNTIME_STATE);
            cpu->vcpu_dirty = false;
        }

        kvm_arch_pre_run(cpu, run);
        if (atomic_read(&cpu->exit_request)) {
            DPRINTF("interrupt exit requested\n");
            /*
             * KVM requires us to reenter the kernel after IO exits to complete
             * instruction emulation. This self-signal will ensure that we
             * leave ASAP again.
             */
            kvm_cpu_kick_self();
        }

        /* Read cpu->exit_request before KVM_RUN reads run->immediate_exit.
         * Matching barrier in kvm_eat_signals.
         */
        smp_rmb();

        run_ret = kvm_vcpu_ioctl(cpu, KVM_RUN, 0);

        attrs = kvm_arch_post_run(cpu, run);

#ifdef KVM_HAVE_MCE_INJECTION
        if (unlikely(have_sigbus_pending)) {
            qemu_mutex_lock_iothread();
            kvm_arch_on_sigbus_vcpu(cpu, pending_sigbus_code,
                                    pending_sigbus_addr);
            have_sigbus_pending = false;
            qemu_mutex_unlock_iothread();
        }
#endif

        if (run_ret < 0) {
            if (run_ret == -EINTR || run_ret == -EAGAIN) {
                DPRINTF("io window exit\n");
                kvm_eat_signals(cpu);
                ret = EXCP_INTERRUPT;
                break;
            }
            fprintf(stderr, "error: kvm run failed %s\n",
                    strerror(-run_ret));
#ifdef TARGET_PPC
            if (run_ret == -EBUSY) {
                fprintf(stderr,
                        "This is probably because your SMT is enabled.\n"
                        "VCPU can only run on primary threads with all "
                        "secondary threads offline.\n");
            }
#endif
            ret = -1;
            break;
        }

	FILE *fp;
	fp = fopen("/tmp/mpla", "a+");
	fprintf(fp, "exit reason %x\n", run->exit_reason);
	fclose(fp);
        trace_kvm_run_exit(cpu->cpu_index, run->exit_reason);
        switch (run->exit_reason) {
        case KVM_EXIT_IO:
		fp = fopen("/tmp/mpla", "a+");
		fprintf(fp, "io port %x\n", run->io.port);
		fclose(fp);
            DPRINTF("handle_io\n");
	    if (run->io.port == 0xffdc && run->io.direction == KVM_EXIT_IO_IN ) {
	//    if (run->io.port == 0xffdc && *(uint8_t *)((char *) (run) + 
	//			    run->io.data_offset) == 77) {
		    my_fork((uint8_t *)run + run->io.data_offset, &check);
		    ret = 0;
		    break;
	    }
	    if (run->io.port == 0xffdd && run->io.direction == KVM_EXIT_IO_IN ) {
	//    if (run->io.port == 0xffdc && *(uint8_t *)((char *) (run) + 
	//			    run->io.data_offset) == 77) {
		    my_start_migration((uint8_t *)run + run->io.data_offset);
		    ret = 0;
		    break;
	    }
            /* Called outside BQL */
            kvm_handle_io(run->io.port, attrs,
                          (uint8_t *)run + run->io.data_offset,
                          run->io.direction,
                          run->io.size,
                          run->io.count);
            ret = 0;
            break;
        case KVM_EXIT_MMIO:
            DPRINTF("handle_mmio\n");
            /* Called outside BQL */
            address_space_rw(&address_space_memory,
                             run->mmio.phys_addr, attrs,
                             run->mmio.data,
                             run->mmio.len,
                             run->mmio.is_write);
            ret = 0;
            break;
        case KVM_EXIT_IRQ_WINDOW_OPEN:
            DPRINTF("irq_window_open\n");
            ret = EXCP_INTERRUPT;
            break;
        case KVM_EXIT_SHUTDOWN:
            DPRINTF("shutdown\n");
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            ret = EXCP_INTERRUPT;
            break;
        case KVM_EXIT_UNKNOWN:
            fprintf(stderr, "KVM: unknown exit, hardware reason %" PRIx64 "\n",
                    (uint64_t)run->hw.hardware_exit_reason);
            ret = -1;
            break;
        case KVM_EXIT_INTERNAL_ERROR:
            ret = kvm_handle_internal_error(cpu, run);
            break;
        case KVM_EXIT_SYSTEM_EVENT:
            switch (run->system_event.type) {
            case KVM_SYSTEM_EVENT_SHUTDOWN:
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_GUEST_SHUTDOWN);
                ret = EXCP_INTERRUPT;
                break;
            case KVM_SYSTEM_EVENT_RESET:
                qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
                ret = EXCP_INTERRUPT;
                break;
            case KVM_SYSTEM_EVENT_CRASH:
                kvm_cpu_synchronize_state(cpu);
                qemu_mutex_lock_iothread();
                qemu_system_guest_panicked(cpu_get_crash_info(cpu));
                qemu_mutex_unlock_iothread();
                ret = 0;
                break;
            default:
                DPRINTF("kvm_arch_handle_exit\n");
                ret = kvm_arch_handle_exit(cpu, run);
                break;
            }
            break;
        default:
            DPRINTF("kvm_arch_handle_exit\n");
            ret = kvm_arch_handle_exit(cpu, run);
            break;
        }
    } while (ret == 0);

    cpu_exec_end(cpu);
    qemu_mutex_lock_iothread();

    if (ret < 0) {
        cpu_dump_state(cpu, stderr, fprintf, CPU_DUMP_CODE);
        vm_stop(RUN_STATE_INTERNAL_ERROR);
    }

    atomic_set(&cpu->exit_request, 0);
    return ret;
}

int kvm_ioctl(KVMState *s, int type, ...)
{
    int ret;
    void *arg;
    va_list ap;

    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);

    trace_kvm_ioctl(type, arg);
    ret = ioctl(s->fd, type, arg);
    if (ret == -1) {
        ret = -errno;
    }
    return ret;
}

int kvm_vm_ioctl(KVMState *s, int type, ...)
{
    int ret;
    void *arg;
    va_list ap;

    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);

    trace_kvm_vm_ioctl(type, arg);
    ret = ioctl(s->vmfd, type, arg);
    if (ret == -1) {
        ret = -errno;
    }
    return ret;
}

int kvm_vcpu_ioctl(CPUState *cpu, int type, ...)
{
    int ret;
    void *arg;
    va_list ap;

    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);

    trace_kvm_vcpu_ioctl(cpu->cpu_index, type, arg);
    ret = ioctl(cpu->kvm_fd, type, arg);
    if (ret == -1) {
        ret = -errno;
    }
    return ret;
}

int kvm_device_ioctl(int fd, int type, ...)
{
    int ret;
    void *arg;
    va_list ap;

    va_start(ap, type);
    arg = va_arg(ap, void *);
    va_end(ap);

    trace_kvm_device_ioctl(fd, type, arg);
    ret = ioctl(fd, type, arg);
    if (ret == -1) {
        ret = -errno;
    }
    return ret;
}

int kvm_vm_check_attr(KVMState *s, uint32_t group, uint64_t attr)
{
    int ret;
    struct kvm_device_attr attribute = {
        .group = group,
        .attr = attr,
    };

    if (!kvm_vm_attributes_allowed) {
        return 0;
    }

    ret = kvm_vm_ioctl(s, KVM_HAS_DEVICE_ATTR, &attribute);
    /* kvm returns 0 on success for HAS_DEVICE_ATTR */
    return ret ? 0 : 1;
}

int kvm_device_check_attr(int dev_fd, uint32_t group, uint64_t attr)
{
    struct kvm_device_attr attribute = {
        .group = group,
        .attr = attr,
        .flags = 0,
    };

    return kvm_device_ioctl(dev_fd, KVM_HAS_DEVICE_ATTR, &attribute) ? 0 : 1;
}

int kvm_device_access(int fd, int group, uint64_t attr,
                      void *val, bool write, Error **errp)
{
    struct kvm_device_attr kvmattr;
    int err;

    kvmattr.flags = 0;
    kvmattr.group = group;
    kvmattr.attr = attr;
    kvmattr.addr = (uintptr_t)val;

    err = kvm_device_ioctl(fd,
                           write ? KVM_SET_DEVICE_ATTR : KVM_GET_DEVICE_ATTR,
                           &kvmattr);
    if (err < 0) {
        error_setg_errno(errp, -err,
                         "KVM_%s_DEVICE_ATTR failed: Group %d "
                         "attr 0x%016" PRIx64,
                         write ? "SET" : "GET", group, attr);
    }
    return err;
}

bool kvm_has_sync_mmu(void)
{
    return kvm_state->sync_mmu;
}

int kvm_has_vcpu_events(void)
{
    return kvm_state->vcpu_events;
}

int kvm_has_robust_singlestep(void)
{
    return kvm_state->robust_singlestep;
}

int kvm_has_debugregs(void)
{
    return kvm_state->debugregs;
}

int kvm_has_many_ioeventfds(void)
{
    if (!kvm_enabled()) {
        return 0;
    }
    return kvm_state->many_ioeventfds;
}

int kvm_has_gsi_routing(void)
{
#ifdef KVM_CAP_IRQ_ROUTING
    return kvm_check_extension(kvm_state, KVM_CAP_IRQ_ROUTING);
#else
    return false;
#endif
}

int kvm_has_intx_set_mask(void)
{
    return kvm_state->intx_set_mask;
}

bool kvm_arm_supports_user_irq(void)
{
    return kvm_check_extension(kvm_state, KVM_CAP_ARM_USER_IRQ);
}

#ifdef KVM_CAP_SET_GUEST_DEBUG
struct kvm_sw_breakpoint *kvm_find_sw_breakpoint(CPUState *cpu,
                                                 target_ulong pc)
{
    struct kvm_sw_breakpoint *bp;

    QTAILQ_FOREACH(bp, &cpu->kvm_state->kvm_sw_breakpoints, entry) {
        if (bp->pc == pc) {
            return bp;
        }
    }
    return NULL;
}

int kvm_sw_breakpoints_active(CPUState *cpu)
{
    return !QTAILQ_EMPTY(&cpu->kvm_state->kvm_sw_breakpoints);
}

struct kvm_set_guest_debug_data {
    struct kvm_guest_debug dbg;
    int err;
};

static void kvm_invoke_set_guest_debug(CPUState *cpu, run_on_cpu_data data)
{
    struct kvm_set_guest_debug_data *dbg_data =
        (struct kvm_set_guest_debug_data *) data.host_ptr;

    dbg_data->err = kvm_vcpu_ioctl(cpu, KVM_SET_GUEST_DEBUG,
                                   &dbg_data->dbg);
}

int kvm_update_guest_debug(CPUState *cpu, unsigned long reinject_trap)
{
    struct kvm_set_guest_debug_data data;

    data.dbg.control = reinject_trap;

    if (cpu->singlestep_enabled) {
        data.dbg.control |= KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
    }
    kvm_arch_update_guest_debug(cpu, &data.dbg);

    run_on_cpu(cpu, kvm_invoke_set_guest_debug,
               RUN_ON_CPU_HOST_PTR(&data));
    return data.err;
}

int kvm_insert_breakpoint(CPUState *cpu, target_ulong addr,
                          target_ulong len, int type)
{
    struct kvm_sw_breakpoint *bp;
    int err;

    if (type == GDB_BREAKPOINT_SW) {
        bp = kvm_find_sw_breakpoint(cpu, addr);
        if (bp) {
            bp->use_count++;
            return 0;
        }

        bp = g_malloc(sizeof(struct kvm_sw_breakpoint));
        bp->pc = addr;
        bp->use_count = 1;
        err = kvm_arch_insert_sw_breakpoint(cpu, bp);
        if (err) {
            g_free(bp);
            return err;
        }

        QTAILQ_INSERT_HEAD(&cpu->kvm_state->kvm_sw_breakpoints, bp, entry);
    } else {
        err = kvm_arch_insert_hw_breakpoint(addr, len, type);
        if (err) {
            return err;
        }
    }

    CPU_FOREACH(cpu) {
        err = kvm_update_guest_debug(cpu, 0);
        if (err) {
            return err;
        }
    }
    return 0;
}

int kvm_remove_breakpoint(CPUState *cpu, target_ulong addr,
                          target_ulong len, int type)
{
    struct kvm_sw_breakpoint *bp;
    int err;

    if (type == GDB_BREAKPOINT_SW) {
        bp = kvm_find_sw_breakpoint(cpu, addr);
        if (!bp) {
            return -ENOENT;
        }

        if (bp->use_count > 1) {
            bp->use_count--;
            return 0;
        }

        err = kvm_arch_remove_sw_breakpoint(cpu, bp);
        if (err) {
            return err;
        }

        QTAILQ_REMOVE(&cpu->kvm_state->kvm_sw_breakpoints, bp, entry);
        g_free(bp);
    } else {
        err = kvm_arch_remove_hw_breakpoint(addr, len, type);
        if (err) {
            return err;
        }
    }

    CPU_FOREACH(cpu) {
        err = kvm_update_guest_debug(cpu, 0);
        if (err) {
            return err;
        }
    }
    return 0;
}

void kvm_remove_all_breakpoints(CPUState *cpu)
{
    struct kvm_sw_breakpoint *bp, *next;
    KVMState *s = cpu->kvm_state;
    CPUState *tmpcpu;

    QTAILQ_FOREACH_SAFE(bp, &s->kvm_sw_breakpoints, entry, next) {
        if (kvm_arch_remove_sw_breakpoint(cpu, bp) != 0) {
            /* Try harder to find a CPU that currently sees the breakpoint. */
            CPU_FOREACH(tmpcpu) {
                if (kvm_arch_remove_sw_breakpoint(tmpcpu, bp) == 0) {
                    break;
                }
            }
        }
        QTAILQ_REMOVE(&s->kvm_sw_breakpoints, bp, entry);
        g_free(bp);
    }
    kvm_arch_remove_all_hw_breakpoints();

    CPU_FOREACH(cpu) {
        kvm_update_guest_debug(cpu, 0);
    }
}

#else /* !KVM_CAP_SET_GUEST_DEBUG */

int kvm_update_guest_debug(CPUState *cpu, unsigned long reinject_trap)
{
    return -EINVAL;
}

int kvm_insert_breakpoint(CPUState *cpu, target_ulong addr,
                          target_ulong len, int type)
{
    return -EINVAL;
}

int kvm_remove_breakpoint(CPUState *cpu, target_ulong addr,
                          target_ulong len, int type)
{
    return -EINVAL;
}

void kvm_remove_all_breakpoints(CPUState *cpu)
{
}
#endif /* !KVM_CAP_SET_GUEST_DEBUG */

static int kvm_set_signal_mask(CPUState *cpu, const sigset_t *sigset)
{
    KVMState *s = kvm_state;
    struct kvm_signal_mask *sigmask;
    int r;

    sigmask = g_malloc(sizeof(*sigmask) + sizeof(*sigset));

    sigmask->len = s->sigmask_len;
    memcpy(sigmask->sigset, sigset, sizeof(*sigset));
    r = kvm_vcpu_ioctl(cpu, KVM_SET_SIGNAL_MASK, sigmask);
    g_free(sigmask);

    return r;
}

static void kvm_ipi_signal(int sig)
{
    if (current_cpu) {
        assert(kvm_immediate_exit);
        kvm_cpu_kick(current_cpu);
    }
}

void kvm_init_cpu_signals(CPUState *cpu)
{
    int r;
    sigset_t set;
    struct sigaction sigact;

    memset(&sigact, 0, sizeof(sigact));
    sigact.sa_handler = kvm_ipi_signal;
    sigaction(SIG_IPI, &sigact, NULL);

    pthread_sigmask(SIG_BLOCK, NULL, &set);
#if defined KVM_HAVE_MCE_INJECTION
    sigdelset(&set, SIGBUS);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
#endif
    sigdelset(&set, SIG_IPI);
    if (kvm_immediate_exit) {
        r = pthread_sigmask(SIG_SETMASK, &set, NULL);
    } else {
        r = kvm_set_signal_mask(cpu, &set);
    }
    if (r) {
        fprintf(stderr, "kvm_set_signal_mask: %s\n", strerror(-r));
        exit(1);
    }
}

/* Called asynchronously in VCPU thread.  */
int kvm_on_sigbus_vcpu(CPUState *cpu, int code, void *addr)
{
#ifdef KVM_HAVE_MCE_INJECTION
    if (have_sigbus_pending) {
        return 1;
    }
    have_sigbus_pending = true;
    pending_sigbus_addr = addr;
    pending_sigbus_code = code;
    atomic_set(&cpu->exit_request, 1);
    return 0;
#else
    return 1;
#endif
}

/* Called synchronously (via signalfd) in main thread.  */
int kvm_on_sigbus(int code, void *addr)
{
#ifdef KVM_HAVE_MCE_INJECTION
    /* Action required MCE kills the process if SIGBUS is blocked.  Because
     * that's what happens in the I/O thread, where we handle MCE via signalfd,
     * we can only get action optional here.
     */
    assert(code != BUS_MCEERR_AR);
    kvm_arch_on_sigbus_vcpu(first_cpu, code, addr);
    return 0;
#else
    return 1;
#endif
}

int kvm_create_device(KVMState *s, uint64_t type, bool test)
{
    int ret;
    struct kvm_create_device create_dev;

    create_dev.type = type;
    create_dev.fd = -1;
    create_dev.flags = test ? KVM_CREATE_DEVICE_TEST : 0;

    if (!kvm_check_extension(s, KVM_CAP_DEVICE_CTRL)) {
        return -ENOTSUP;
    }

    ret = kvm_vm_ioctl(s, KVM_CREATE_DEVICE, &create_dev);
    if (ret) {
        return ret;
    }

    return test ? 0 : create_dev.fd;
}

bool kvm_device_supported(int vmfd, uint64_t type)
{
    struct kvm_create_device create_dev = {
        .type = type,
        .fd = -1,
        .flags = KVM_CREATE_DEVICE_TEST,
    };

    if (ioctl(vmfd, KVM_CHECK_EXTENSION, KVM_CAP_DEVICE_CTRL) <= 0) {
        return false;
    }

    return (ioctl(vmfd, KVM_CREATE_DEVICE, &create_dev) >= 0);
}

int kvm_set_one_reg(CPUState *cs, uint64_t id, void *source)
{
    struct kvm_one_reg reg;
    int r;

    reg.id = id;
    reg.addr = (uintptr_t) source;
    r = kvm_vcpu_ioctl(cs, KVM_SET_ONE_REG, &reg);
    if (r) {
        trace_kvm_failed_reg_set(id, strerror(-r));
    }
    return r;
}

int kvm_get_one_reg(CPUState *cs, uint64_t id, void *target)
{
    struct kvm_one_reg reg;
    int r;

    reg.id = id;
    reg.addr = (uintptr_t) target;
    r = kvm_vcpu_ioctl(cs, KVM_GET_ONE_REG, &reg);
    if (r) {
        trace_kvm_failed_reg_get(id, strerror(-r));
    }
    return r;
}

static void kvm_accel_class_init(ObjectClass *oc, void *data)
{
    AccelClass *ac = ACCEL_CLASS(oc);
    ac->name = "KVM";
    ac->init_machine = kvm_init;
    ac->allowed = &kvm_allowed;
}

static const TypeInfo kvm_accel_type = {
    .name = TYPE_KVM_ACCEL,
    .parent = TYPE_ACCEL,
    .class_init = kvm_accel_class_init,
    .instance_size = sizeof(KVMState),
};

static void kvm_type_init(void)
{
    type_register_static(&kvm_accel_type);
}

type_init(kvm_type_init);