#include <pmm.h>
#include <list.h>
#include <stdio.h>

/*
 * One intrusive free list for each power-of-two block size.  Page.property
 * stores order + 1 at both free and allocated block heads; interior pages
 * have property == 0.  PG_property is set ONLY on heads in the free lists.
 * Encoding the order avoids truncating a size_t page count into property.
 * All metadata lives here or in the existing Page descriptors.
 */
#define BUDDY_MAX_ORDER (sizeof(size_t) * 8 - 1)

static list_entry_t buddy_free_lists[BUDDY_MAX_ORDER + 1];
static size_t buddy_nr_free;

static size_t
buddy_block_pages(unsigned int order) {
    return (size_t)1 << order;
}

/* Zero and requests whose next power of two is unrepresentable fail. */
static bool
buddy_request_order(size_t n, unsigned int *order) {
    size_t block_pages = 1;
    unsigned int result = 0;

    if (n == 0) {
        return 0;
    }
    while (block_pages < n) {
        if (result == BUDDY_MAX_ORDER) {
            return 0;
        }
        block_pages <<= 1;
        result ++;
    }
    *order = result;
    return 1;
}

/* npage is the exclusive physical PPN limit, NOT the Page array length. */
static bool
buddy_block_in_range(size_t ppn, size_t block_pages) {
    return ppn >= nbase && ppn < npage && block_pages <= npage - ppn;
}

static void
buddy_add_block(struct Page *base, unsigned int order) {
    base->property = order + 1;
    SetPageProperty(base);
    list_add(&buddy_free_lists[order], &base->page_link);
}

static void
buddy_remove_block(struct Page *base) {
    list_del_init(&base->page_link);
    ClearPageProperty(base);
    base->property = 0;
}

/*
 * Insert an unlisted block, coalescing only with its exact free buddy.
 * Absolute physical page numbers define alignment for both registration
 * and merging.  A reserved hole or an unregistered range cannot be merged:
 * its pages are never heads of blocks in these free lists.
 * This helper does not change the total number of free pages.
 */
static void
buddy_release_block(struct Page *base, unsigned int order) {
    size_t ppn = page2ppn(base);

    while (order < BUDDY_MAX_ORDER) {
        size_t block_pages = buddy_block_pages(order);
        size_t buddy_ppn = ppn ^ block_pages;
        if (!buddy_block_in_range(buddy_ppn, block_pages)) {
            break;
        }

        struct Page *buddy = pages + (buddy_ppn - nbase);
        if (PageReserved(buddy) || !PageProperty(buddy) ||
            buddy->property != order + 1) {
            break;
        }

        buddy_remove_block(buddy);
        if (buddy_ppn < ppn) {
            base = buddy;
            ppn = buddy_ppn;
        }
        order ++;
    }
    buddy_add_block(base, order);
}

static void
buddy_init(void) {
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_init(&buddy_free_lists[order]);
    }
    buddy_nr_free = 0;
}

/*
 * As with default_pmm, the caller supplies a valid available range whose
 * descriptors are still marked reserved.  Register each page only once.
 * Separate calls may describe adjacent ranges or ranges separated by holes.
 */
static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(base != NULL && n > 0);
    assert(base >= pages);
    size_t ppn = page2ppn(base);
    assert(buddy_block_in_range(ppn, n));
    assert(buddy_nr_free <= npage - nbase);
    assert(n <= npage - nbase - buddy_nr_free);

    /* Validate the whole range before changing any descriptors. */
    for (size_t i = 0; i < n; i ++) {
        assert(PageReserved(base + i));
    }
    for (size_t i = 0; i < n; i ++) {
        struct Page *page = base + i;
        page->flags = 0;
        page->property = 0;
        set_page_ref(page, 0);
        list_init(&page->page_link);
    }

    size_t remaining = n;
    while (remaining > 0) {
        unsigned int order = 0;
        size_t block_pages = 1;

        /* Largest aligned block wholly contained in the remaining range. */
        while (order < BUDDY_MAX_ORDER && block_pages <= remaining / 2 &&
               (ppn & ((block_pages << 1) - 1)) == 0) {
            block_pages <<= 1;
            order ++;
        }
        buddy_release_block(base, order);
        base += block_pages;
        ppn += block_pages;
        remaining -= block_pages;
    }
    buddy_nr_free += n;
}

/*
 * Allocate the entire next-power-of-two block, including any padding pages.
 * A zero request, rounding overflow, or lack of a suitable block returns
 * NULL without changing descriptors, free lists, or the free-page count.
 */
static struct Page *
buddy_alloc_pages(size_t n) {
    unsigned int order;
    if (!buddy_request_order(n, &order)) {
        return NULL;
    }
    size_t block_pages = buddy_block_pages(order);
    if (block_pages > buddy_nr_free) {
        return NULL;
    }

    unsigned int found = order;
    while (found <= BUDDY_MAX_ORDER && list_empty(&buddy_free_lists[found])) {
        found ++;
    }
    if (found > BUDDY_MAX_ORDER) {
        return NULL;
    }

    struct Page *base = le2page(list_next(&buddy_free_lists[found]), page_link);
    buddy_remove_block(base);
    while (found > order) {
        found --;
        struct Page *right = base + buddy_block_pages(found);
        buddy_add_block(right, found);
    }
    base->property = order + 1;
    buddy_nr_free -= block_pages;
    return base;
}

/*
 * Free with the exact returned head and the ORIGINAL requested page count.
 * The whole rounded block is reclaimed.  Partial frees, zero-length frees,
 * double frees, and freeing an interior page are outside this interface.
 * The stored order checks the size class; it does not distinguish different
 * original requests that round to the same size class.
 */
static void
buddy_free_pages(struct Page *base, size_t n) {
    unsigned int order;
    assert(base != NULL);
    assert(buddy_request_order(n, &order));
    assert(base >= pages);
    size_t ppn = page2ppn(base);
    size_t block_pages = buddy_block_pages(order);
    assert(buddy_block_in_range(ppn, block_pages));
    assert((ppn & (block_pages - 1)) == 0);
    assert(!PageReserved(base) && !PageProperty(base));
    assert(base->property == order + 1);
    assert(buddy_nr_free <= npage - nbase);
    assert(block_pages <= npage - nbase - buddy_nr_free);

    for (size_t i = 0; i < block_pages; i ++) {
        struct Page *page = base + i;
        assert(!PageReserved(page) && !PageProperty(page));
        assert(i == 0 || page->property == 0);
    }
    for (size_t i = 0; i < block_pages; i ++) {
        struct Page *page = base + i;
        page->flags = 0;
        page->property = 0;
        set_page_ref(page, 0);
        list_init(&page->page_link);
    }

    buddy_release_block(base, order);
    buddy_nr_free += block_pages;
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

/*
 * Descriptor-only tests: no physical memory contents are accessed.  Using a
 * private arena avoids assuming that the live allocator has a large free
 * block.  Static scratch storage also keeps these checks off the small kernel
 * stack.  The independent masks include ALL rounded allocation pages.
 */
#define BUDDY_CHECK_PAGES 64

static struct Page buddy_check_pages[BUDDY_CHECK_PAGES];
static uint64_t buddy_check_available, buddy_check_busy;
static struct {
    size_t request;
    size_t count;
} buddy_check_allocations[BUDDY_CHECK_PAGES];

static uint64_t
buddy_check_mask(size_t start, size_t count) {
    assert(start < BUDDY_CHECK_PAGES);
    assert(count > 0 && count <= BUDDY_CHECK_PAGES - start);
    uint64_t mask = 0;
    for (size_t i = start; i < start + count; i ++) {
        mask |= (uint64_t)1 << i;
    }
    return mask;
}

/* Check every free/allocated/reserved descriptor against the independent map. */
static void
buddy_check_state(void) {
    uint64_t free_seen = 0, allocated_seen = 0;
    size_t total = 0;
    assert(pages == buddy_check_pages);
    assert(npage == nbase + BUDDY_CHECK_PAGES);
    assert((buddy_check_busy & ~buddy_check_available) == 0);

    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_entry_t *head = &buddy_free_lists[order];
        assert(head->next->prev == head && head->prev->next == head);
        size_t nodes = 0;
        for (list_entry_t *le = head->next; le != head; le = le->next) {
            assert(nodes ++ < BUDDY_CHECK_PAGES);  /* Detect list cycles. */
            assert(le->next->prev == le && le->prev->next == le);
            struct Page *base = le2page(le, page_link);
            assert(base >= pages && base < pages + BUDDY_CHECK_PAGES);
            size_t index = base - pages;
            size_t count = (size_t)1 << order;
            uint64_t mask = buddy_check_mask(index, count);
            assert(((nbase + index) & (count - 1)) == 0);
            assert((mask & buddy_check_available) == mask);
            assert((mask & (free_seen | buddy_check_busy)) == 0);
            for (size_t i = 0; i < count; i ++) {
                struct Page *page = base + i;
                assert(page_ref(page) == 0);
                assert(page->flags == (i == 0 ? (1UL << PG_property) : 0));
                assert(page->property == (i == 0 ? order + 1 : 0));
            }
            /* Every possible free buddy pair should already be coalesced. */
            size_t other = (nbase + index) ^ count;
            if (order < BUDDY_MAX_ORDER && other >= nbase && other < npage) {
                struct Page *buddy = pages + (other - nbase);
                assert(!PageProperty(buddy) || buddy->property != order + 1);
            }
            free_seen |= mask;
            total += count;
        }
    }

    for (size_t index = 0; index < BUDDY_CHECK_PAGES; index ++) {
        size_t count = buddy_check_allocations[index].count;
        if (count != 0) {
            uint64_t mask = buddy_check_mask(index, count);
            assert((mask & allocated_seen) == 0);
            assert((mask & buddy_check_busy) == mask);
            assert(((nbase + index) & (count - 1)) == 0);
            unsigned int order = 0;
            while (((size_t)1 << order) < count) {
                order ++;
            }
            assert(((size_t)1 << order) == count);
            for (size_t i = 0; i < count; i ++) {
                struct Page *page = pages + index + i;
                assert(page->flags == 0 && page_ref(page) == 0);
                assert(page->property == (i == 0 ? order + 1 : 0));
                assert(page->page_link.next == &page->page_link);
                assert(page->page_link.prev == &page->page_link);
            }
            allocated_seen |= mask;
        }
        if (!(buddy_check_available & ((uint64_t)1 << index))) {
            struct Page *page = pages + index;
            assert(page->flags == (1UL << PG_reserved));
            assert(page->property == 0 && page_ref(page) == -1);
            assert(page->page_link.next == &page->page_link);
            assert(page->page_link.prev == &page->page_link);
        }
    }
    assert(allocated_seen == buddy_check_busy);
    assert(free_seen == (buddy_check_available & ~buddy_check_busy));
    assert(total == buddy_nr_free_pages());
}

static void
buddy_check_reset(void) {
    assert(buddy_check_busy == 0);  /* Never discard an outstanding allocation. */
    buddy_init();
    buddy_check_available = 0;
    for (size_t i = 0; i < BUDDY_CHECK_PAGES; i ++) {
        struct Page *page = pages + i;
        page->flags = 1UL << PG_reserved;
        page->property = 0;
        set_page_ref(page, -1);  /* Untouched reserved-page guard. */
        list_init(&page->page_link);
        buddy_check_allocations[i].request = 0;
        buddy_check_allocations[i].count = 0;
    }
    buddy_check_state();
}

static void
buddy_check_register(size_t start, size_t count) {
    uint64_t mask = buddy_check_mask(start, count);
    assert((mask & buddy_check_available) == 0);
    size_t before = buddy_nr_free_pages();
    buddy_init_memmap(pages + start, count);
    buddy_check_available |= mask;
    assert(buddy_nr_free_pages() == before + count);
    buddy_check_state();
}

/* Expected sizes are supplied by the test, never by buddy_request_order. */
static struct Page *
buddy_check_alloc(size_t request, size_t expected_count) {
    size_t before = buddy_nr_free_pages();
    struct Page *base = buddy_alloc_pages(request);
    assert(base != NULL);
    assert(base >= pages && base < pages + BUDDY_CHECK_PAGES);
    size_t index = base - pages;
    uint64_t mask = buddy_check_mask(index, expected_count);
    assert((mask & buddy_check_available) == mask);
    assert((mask & buddy_check_busy) == 0);
    assert(before >= expected_count);
    assert(buddy_nr_free_pages() == before - expected_count);
    assert(buddy_check_allocations[index].count == 0);
    buddy_check_allocations[index].request = request;
    buddy_check_allocations[index].count = expected_count;
    buddy_check_busy |= mask;
    buddy_check_state();
    return base;
}

static void
buddy_check_free(struct Page *base) {
    assert(base >= pages && base < pages + BUDDY_CHECK_PAGES);
    size_t index = base - pages;
    size_t count = buddy_check_allocations[index].count;
    assert(count != 0);
    uint64_t mask = buddy_check_mask(index, count);
    assert((mask & buddy_check_busy) == mask);
    size_t before = buddy_nr_free_pages();
    buddy_free_pages(base, buddy_check_allocations[index].request);
    buddy_check_allocations[index].request = 0;
    buddy_check_allocations[index].count = 0;
    buddy_check_busy &= ~mask;
    assert(buddy_nr_free_pages() == before + count);
    buddy_check_state();
}

/* Compare fields, not struct padding, and include allocated/reserved pages. */
static void
buddy_check_failure(size_t request) {
    static struct Page saved_pages[BUDDY_CHECK_PAGES];
    static list_entry_t saved_lists[BUDDY_MAX_ORDER + 1];
    size_t saved_count = buddy_nr_free_pages();
    for (size_t i = 0; i < BUDDY_CHECK_PAGES; i ++) {
        saved_pages[i] = pages[i];
    }
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        saved_lists[order] = buddy_free_lists[order];
    }
    assert(buddy_alloc_pages(request) == NULL);
    assert(buddy_nr_free_pages() == saved_count);
    for (size_t i = 0; i < BUDDY_CHECK_PAGES; i ++) {
        assert(pages[i].ref == saved_pages[i].ref);
        assert(pages[i].flags == saved_pages[i].flags);
        assert(pages[i].property == saved_pages[i].property);
        assert(pages[i].page_link.prev == saved_pages[i].page_link.prev);
        assert(pages[i].page_link.next == saved_pages[i].page_link.next);
    }
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        assert(buddy_free_lists[order].prev == saved_lists[order].prev);
        assert(buddy_free_lists[order].next == saved_lists[order].next);
    }
    buddy_check_state();
}

static void
buddy_check_only_block(struct Page *base, unsigned int expected_order) {
    assert(buddy_nr_free_pages() == ((size_t)1 << expected_order));
    assert(PageProperty(base) && base->property == expected_order + 1);
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_entry_t *head = &buddy_free_lists[order];
        if (order == expected_order) {
            assert(head->next == &base->page_link && head->prev == &base->page_link);
        } else {
            assert(list_empty(head));
        }
    }
}

static void
buddy_check_mixed(size_t start) {
    static const size_t requests[] = {1, 2, 3, 5, 9, 1};
    static const size_t counts[] = {1, 2, 4, 8, 16, 1};
    static const unsigned int release_order[] = {5, 1, 4, 0, 3, 2};
    struct Page *blocks[6];
    buddy_check_reset();
    buddy_check_register(start, 32);
    buddy_check_only_block(pages + start, 5);

    buddy_check_failure(0);
    buddy_check_failure(33);
    size_t high_bit = (size_t)1 << (sizeof(size_t) * 8 - 1);
    buddy_check_failure(high_bit);       /* Representable, but too large. */
    buddy_check_failure(high_bit + 1);   /* Rounding would overflow. */
    buddy_check_failure((size_t)-1);

    for (size_t i = 0; i < 6; i ++) {
        blocks[i] = buddy_check_alloc(requests[i], counts[i]);
    }
    assert(buddy_nr_free_pages() == 0);
    buddy_check_failure(1);
    buddy_check_free(blocks[2]);  /* free(head, 3) must return all FOUR pages. */
    assert(buddy_nr_free_pages() == 4);
    buddy_check_failure(5);
    struct Page *reused = buddy_check_alloc(3, 4);
    assert(reused == blocks[2]);  /* This is the only available block. */
    for (size_t i = 0; i < 6; i ++) {
        buddy_check_free(blocks[release_order[i]]);
    }
    buddy_check_only_block(pages + start, 5);
    reused = buddy_check_alloc(32, 32);
    assert(reused == pages + start);
    buddy_check_free(reused);
    buddy_check_only_block(pages + start, 5);
}

static void
buddy_check_split_merge(size_t start) {
    buddy_check_reset();
    buddy_check_register(start, 32);
    buddy_check_alloc(1, 1);
    /* A split from order 5 down to 0 leaves one block at each lower order. */
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        list_entry_t *head = &buddy_free_lists[order];
        if (order < 5) {
            assert(!list_empty(head) && head->next == head->prev);
        } else {
            assert(list_empty(head));
        }
    }
    for (size_t i = 1; i < 32; i ++) {
        buddy_check_alloc(1, 1);
    }
    assert(buddy_check_busy == buddy_check_available);
    buddy_check_failure(1);

    struct Page *base = pages + start;
    buddy_check_free(base + 1);
    buddy_check_free(base + 2);
    assert(buddy_nr_free_pages() == 2);
    /* Adjacent [1,2) and [2,3) belong to different order-1 parents. */
    buddy_check_failure(2);
    buddy_check_free(base);
    assert(PageProperty(base) && base->property == 2);
    assert(PageProperty(base + 2) && base[2].property == 1);
    buddy_check_free(base + 3);
    buddy_check_only_block(base, 2);
    for (unsigned int order = 3; order <= 5; order ++) {
        for (size_t i = (size_t)1 << (order - 1); i < ((size_t)1 << order); i ++) {
            buddy_check_free(base + i);
        }
        buddy_check_only_block(base, order);
    }
    assert(buddy_check_busy == 0);
}

/* The maximal aligned partition of the 13-page interval [start+1,start+14). */
static void
buddy_check_irregular_shape(size_t start) {
    static const size_t offsets[] = {1, 2, 4, 8, 12};
    static const unsigned int orders[] = {0, 1, 2, 2, 1};
    assert(buddy_check_busy == 0 && buddy_nr_free_pages() == 13);
    for (size_t i = 0; i < 5; i ++) {
        struct Page *page = pages + start + offsets[i];
        assert(PageProperty(page) && page->property == orders[i] + 1);
    }
    buddy_check_state();
}

static void
buddy_check_irregular(size_t start) {
    buddy_check_reset();
    buddy_check_register(start + 1, 13);
    buddy_check_irregular_shape(start);
    buddy_check_failure(8);  /* 13 free pages, but no aligned 8-page block. */
    for (size_t i = 0; i < 13; i ++) {
        buddy_check_alloc(1, 1);
    }
    assert(buddy_check_busy == buddy_check_mask(start + 1, 13));
    buddy_check_failure(1);
    for (size_t i = 0; i < 13; i ++) {
        /* A permutation, independent of free-list allocation order. */
        buddy_check_free(pages + start + 1 + (i * 5) % 13);
    }
    buddy_check_irregular_shape(start);

    buddy_check_reset();
    buddy_check_register(start + 1, 5);
    buddy_check_register(start + 8, 6);
    buddy_check_failure(8);  /* The still-reserved hole [6,8) must survive. */
    for (size_t i = 0; i < 11; i ++) {
        buddy_check_alloc(1, 1);
    }
    assert(buddy_check_busy == buddy_check_available);
    buddy_check_failure(1);
    for (size_t i = 0; i < BUDDY_CHECK_PAGES; i ++) {
        if (buddy_check_allocations[i].count != 0) {
            buddy_check_free(pages + i);
        }
    }
    assert(PageProperty(pages + start + 4));
    assert(pages[start + 4].property == 2);  /* Free [4,6). */
    buddy_check_register(start + 6, 2);     /* Joins [4,6) across calls. */
    buddy_check_irregular_shape(start);
    assert(!PageProperty(pages + start + 6));
    assert(pages[start + 6].property == 0);
    struct Page *block = buddy_check_alloc(3, 4);
    buddy_check_free(block);
    buddy_check_irregular_shape(start);
}

/*
 * Call during exclusive, single-hart boot-time PMM checking, as in pmm.c.
 * Disable local interrupts while pages/npage refer to the synthetic arena.
 * Real descriptors and physical page contents are never modified; their list
 * nodes keep pointing to the original global sentinel addresses throughout.
 */
static void
buddy_check(void) {
    static list_entry_t saved_lists[BUDDY_MAX_ORDER + 1];
    unsigned long saved_status = clear_csr(sstatus, SSTATUS_SIE);
    asm volatile ("" ::: "memory");
    struct Page *saved_pages = pages;
    size_t saved_npage = npage;
    size_t saved_count = buddy_nr_free_pages();
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        saved_lists[order] = buddy_free_lists[order];
    }

    assert(nbase <= (size_t)-1 - BUDDY_CHECK_PAGES);
    pages = buddy_check_pages;
    npage = nbase + BUDDY_CHECK_PAGES;
    /* Leave room for a physically aligned 32-page arena even for odd nbase. */
    size_t start = ((size_t)0 - nbase) & 31;
    buddy_check_mixed(start);
    buddy_check_split_merge(start);
    buddy_check_irregular(start);
    assert(buddy_check_busy == 0);
    buddy_check_state();

    pages = saved_pages;
    npage = saved_npage;
    buddy_nr_free = saved_count;
    for (unsigned int order = 0; order <= BUDDY_MAX_ORDER; order ++) {
        buddy_free_lists[order] = saved_lists[order];
        list_entry_t *head = &buddy_free_lists[order];
        assert(head->next == saved_lists[order].next);
        assert(head->prev == saved_lists[order].prev);
        assert(head->next->prev == head && head->prev->next == head);
    }
    assert(pages == saved_pages && npage == saved_npage);
    assert(buddy_nr_free_pages() == saved_count);
    asm volatile ("" ::: "memory");
    if (saved_status & SSTATUS_SIE) {
        set_csr(sstatus, SSTATUS_SIE);
    }
    cprintf("buddy_check() succeeded!\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
