#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <buddy_pmm.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;

#if defined(USE_DEFAULT_PMM) && defined(USE_BUDDY_PMM)
#error "USE_DEFAULT_PMM and USE_BUDDY_PMM are mutually exclusive"
#endif


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
#if defined(USE_DEFAULT_PMM)
    pmm_manager = &default_pmm_manager;
#elif defined(USE_BUDDY_PMM)
    pmm_manager = &buddy_pmm_manager;
#else
    pmm_manager = &best_fit_pmm_manager;
#endif
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 2511102
static void page_init(void) {
    extern char end[];

    uint64_t mem_base = get_memory_base();
    uint64_t mem_size = get_memory_size();
    if (mem_base == 0 || mem_size == 0 ||
        mem_base > (uint64_t)-1 - mem_size) {
        panic("invalid physical memory range from DTB");
    }

    uint64_t mem_end = mem_base + mem_size;
    if (mem_base != DRAM_BASE || mem_end > PHYSICAL_MEMORY_END) {
        panic("unsupported physical memory range: [0x%016lx, 0x%016lx)",
              mem_base, mem_end);
    }

    va_pa_offset = PHYSICAL_MEMORY_OFFSET;
    npage = PPN(mem_end);
    if (npage <= nbase) {
        panic("physical memory contains no manageable pages");
    }

    pages = (struct Page *)ROUNDUP((uintptr_t)end, PGSIZE);
    size_t descriptor_count = npage - nbase;
    if (descriptor_count > ((size_t)-1) / sizeof(struct Page) ||
        (uintptr_t)pages > (uintptr_t)-1 -
                               descriptor_count * sizeof(struct Page)) {
        panic("page descriptor array size overflow");
    }

    for (size_t i = 0; i < descriptor_count; i ++) {
        struct Page *page = pages + i;
        page->flags = 0;
        page->property = 0;
        set_page_ref(page, 0);
        list_init(&page->page_link);
        SetPageReserved(page);
    }

    uintptr_t descriptor_end =
        (uintptr_t)pages + descriptor_count * sizeof(struct Page);
    if (descriptor_end > (uintptr_t)-1 - (PGSIZE - 1)) {
        panic("page descriptor end address overflow");
    }
    uintptr_t free_start = ROUNDUP(PADDR(descriptor_end), PGSIZE);
    uintptr_t free_end = ROUNDDOWN(mem_end, PGSIZE);
    if (free_start >= free_end) {
        panic("no complete physical pages remain after kernel metadata");
    }

    size_t free_count = (free_end - free_start) / PGSIZE;
    init_memmap(pa2page(free_start), free_count);

    uintptr_t kernel_end_pa = PADDR((uintptr_t)end);
    uintptr_t pages_pa = PADDR((uintptr_t)pages);
    if (va_pa_offset != PHYSICAL_MEMORY_OFFSET ||
        npage != PPN(mem_end) || pages == NULL ||
        ((uintptr_t)pages & (PGSIZE - 1)) != 0 ||
        (uintptr_t)pages < ROUNDUP((uintptr_t)end, PGSIZE) ||
        PPN(kernel_end_pa) >= npage || PPN(pages_pa) >= npage ||
        !PageReserved(pa2page(kernel_end_pa)) ||
        !PageReserved(pa2page(pages_pa)) ||
        nr_free_pages() != free_count ||
        pa2page(page2pa(pa2page(kernel_end_pa))) !=
            pa2page(kernel_end_pa)) {
        panic("physical page initialization verification failed");
    }

    cprintf("physical memory map: [0x%016lx, 0x%016lx)\n",
            mem_base, mem_end);
    cprintf("page descriptors:  0x%016lx, count: %lu\n",
            (uintptr_t)pages, descriptor_count);
    cprintf("free physical memory: [0x%016lx, 0x%016lx), pages: %lu\n",
            free_start, free_end, free_count);
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
