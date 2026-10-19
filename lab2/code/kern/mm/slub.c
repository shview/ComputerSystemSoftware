#include <pmm.h>
#include <list.h>
#include <stdio.h>

/*
 * A small, boot-time, single-hart object allocator layered over the PMM.
 * Call slub_init after the PMM and its kernel direct mapping are initialized.
 * Calls must be serialized; this is not an SMP or interrupt-context allocator.
 * Objects are not zeroed.  All returned addresses have 16-byte alignment.
 * Only the original, still-live returned pointer may be passed to slub_free.
 *
 * Page descriptors belong exclusively to the PMM.  Our headers and free-object
 * links live in the actual allocated physical pages, through their kernel VAs.
 */
#define SLUB_ALIGNMENT 16
#define SLUB_CLASS_COUNT 8
#define SLUB_LARGE_CLASS SLUB_CLASS_COUNT
#define SLUB_MAGIC ((uint64_t)0x534C55424C495645ULL)

static const size_t slub_sizes[SLUB_CLASS_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048,
};

struct slub_free_object {
    struct slub_free_object *next;  /* Stored only while the object is free. */
};

/* This header occupies the start of the first actual page of an allocation. */
struct slub_block {
    uint64_t magic;
    struct Page *page;
    size_t page_count;              /* ORIGINAL count passed to alloc_pages. */
    size_t class_index;             /* SLUB_LARGE_CLASS for a direct allocation. */
    size_t capacity;
    size_t inuse;
    struct slub_free_object *free_objects;
    list_entry_t partial_link;      /* Private link, never Page.page_link. */
};

#define SLUB_HEADER_SIZE \
    ((sizeof(struct slub_block) + SLUB_ALIGNMENT - 1) & \
     ~((size_t)SLUB_ALIGNMENT - 1))

/* Only nonempty slabs with available slots are kept in these lists. */
static list_entry_t slub_partial[SLUB_CLASS_COUNT];
static bool slub_initialized;

/*
 * pmm.h has no active KADDR/page2kva helper.  Validate the requested extent
 * against both the descriptor range and this project's kernel direct map.
 * Buddy's rounded-up extra pages are unused here and released by the PMM.
 * A NULL result must be followed by free_pages(page, original_count).
 */
static void *
slub_page_address(struct Page *page, size_t count) {
    size_t ppn = page2ppn(page);
    if (count == 0 || ppn < nbase || ppn >= npage || count > npage - ppn ||
        ppn > ((uintptr_t)-1 >> PGSHIFT)) {
        return NULL;
    }

    uintptr_t pa = page2pa(page);
    if (pa > (uintptr_t)-1 - va_pa_offset) {
        return NULL;
    }
    uintptr_t kva = pa + va_pa_offset;
    if ((kva & (PGSIZE - 1)) != 0 || kva < KERNBASE || kva >= KERNTOP ||
        count > (KERNTOP - kva) / PGSIZE) {
        return NULL;
    }
    return (void *)kva;
}

static void
slub_setup_block(struct slub_block *block, struct Page *page,
                 size_t page_count, size_t class_index) {
    block->magic = SLUB_MAGIC;
    block->page = page;
    block->page_count = page_count;
    block->class_index = class_index;
    block->capacity = 0;
    block->inuse = 0;
    block->free_objects = NULL;
    list_init(&block->partial_link);
}

static struct slub_block *
slub_new_slab(size_t class_index) {
    struct Page *page = alloc_pages(1);
    if (page == NULL) {
        return NULL;
    }
    struct slub_block *slab = slub_page_address(page, 1);
    if (slab == NULL) {
        free_pages(page, 1);
        return NULL;
    }

    slub_setup_block(slab, page, 1, class_index);
    size_t stride = slub_sizes[class_index];
    slab->capacity = (PGSIZE - SLUB_HEADER_SIZE) / stride;
    assert(slab->capacity > 0);
    char *objects = (char *)slab + SLUB_HEADER_SIZE;
    for (size_t i = slab->capacity; i > 0; i --) {
        struct slub_free_object *object =
            (struct slub_free_object *)(objects + (i - 1) * stride);
        object->next = slab->free_objects;
        slab->free_objects = object;
    }
    /* Publish only after the entire slab has been initialized. */
    list_add(&slub_partial[class_index], &slab->partial_link);
    return slab;
}

static void *
slub_alloc_large(size_t size) {
    if (size > (size_t)-1 - SLUB_HEADER_SIZE) {
        return NULL;
    }
    size_t bytes = size + SLUB_HEADER_SIZE;
    /* Division and remainder avoid overflow in bytes + PGSIZE - 1. */
    size_t count = bytes / PGSIZE + (bytes % PGSIZE != 0);
    if (count > (size_t)-1 / PGSIZE || count > KMEMSIZE / PGSIZE) {
        return NULL;
    }

    struct Page *page = alloc_pages(count);
    if (page == NULL) {
        return NULL;
    }
    struct slub_block *block = slub_page_address(page, count);
    if (block == NULL) {
        free_pages(page, count);
        return NULL;
    }
    slub_setup_block(block, page, count, SLUB_LARGE_CLASS);
    block->capacity = block->inuse = 1;
    /* The returned pointer is always inside the FIRST page, even for large
     * allocations.  Page-aligning that exact pointer recovers this header. */
    return (char *)block + SLUB_HEADER_SIZE;
}

void
slub_init(void) {
    static_assert((SLUB_ALIGNMENT & (SLUB_ALIGNMENT - 1)) == 0);
    static_assert(PGSIZE % SLUB_ALIGNMENT == 0);
    static_assert(sizeof(struct slub_free_object) <= SLUB_ALIGNMENT);
    static_assert(__alignof__(struct slub_block) <= SLUB_ALIGNMENT);
    static_assert(SLUB_HEADER_SIZE + 2048 <= PGSIZE);

    /* Repeated initialization must not orphan any live slabs or objects. */
    if (slub_initialized) {
        return;
    }
    for (size_t i = 0; i < SLUB_CLASS_COUNT; i ++) {
        list_init(&slub_partial[i]);
    }
    slub_initialized = 1;
}

void *
slub_alloc(size_t size) {
    if (size == 0 || !slub_initialized) {
        return NULL;
    }

    size_t class_index = 0;
    while (class_index < SLUB_CLASS_COUNT && slub_sizes[class_index] < size) {
        class_index ++;
    }
    if (class_index == SLUB_CLASS_COUNT) {
        return slub_alloc_large(size);
    }

    struct slub_block *slab;
    if (list_empty(&slub_partial[class_index])) {
        slab = slub_new_slab(class_index);
        if (slab == NULL) {
            return NULL;
        }
    } else {
        slab = to_struct(list_next(&slub_partial[class_index]),
                         struct slub_block, partial_link);
    }
    assert(slab->magic == SLUB_MAGIC && slab->class_index == class_index);
    assert(slab->inuse < slab->capacity && slab->free_objects != NULL);
    struct slub_free_object *object = slab->free_objects;
    slab->free_objects = object->next;
    slab->inuse ++;
    if (slab->inuse == slab->capacity) {
        assert(slab->free_objects == NULL);
        list_del_init(&slab->partial_link);
    }
    return object;
}

void
slub_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    assert(slub_initialized);
    uintptr_t address = (uintptr_t)ptr;
    assert((address & (SLUB_ALIGNMENT - 1)) == 0);
    assert(address >= KERNBASE && address < KERNTOP);
    struct slub_block *block =
        (struct slub_block *)(address & ~((uintptr_t)PGSIZE - 1));
    assert(block->magic == SLUB_MAGIC);
    assert(block->page != NULL && block->page_count > 0);
    uintptr_t first_object = (uintptr_t)block + SLUB_HEADER_SIZE;

    if (block->class_index == SLUB_LARGE_CLASS) {
        assert(address == first_object);
        assert(block->capacity == 1 && block->inuse == 1);
        struct Page *page = block->page;
        size_t count = block->page_count;
        block->magic = 0;
        /* Passing the saved original count lets Buddy release its full block. */
        free_pages(page, count);
        return;  /* Never access the header after returning its pages. */
    }

    assert(block->class_index < SLUB_CLASS_COUNT && block->page_count == 1);
    size_t stride = slub_sizes[block->class_index];
    assert(block->capacity == (PGSIZE - SLUB_HEADER_SIZE) / stride);
    assert(address >= first_object);
    size_t offset = address - first_object;
    assert(offset < block->capacity * stride && offset % stride == 0);
    assert(block->inuse > 0 && block->inuse <= block->capacity);
    bool was_full = (block->inuse == block->capacity);
    assert((block->free_objects == NULL) == was_full);

    struct slub_free_object *object = ptr;
    object->next = block->free_objects;
    block->free_objects = object;
    block->inuse --;
    if (block->inuse == 0) {
        /* A full one-slot slab was already absent from the partial list. */
        if (!was_full) {
            list_del_init(&block->partial_link);
        }
        struct Page *page = block->page;
        block->magic = 0;
        free_pages(page, 1);
    } else if (was_full) {
        list_add(&slub_partial[block->class_index], &block->partial_link);
    }
}

/*
 * Self-tests use real objects, never substitute Page arrays or reset the PMM.
 * The independent records contain requested sizes, not slab capacities.  All
 * payload accesses stay within those sizes, including the exhaustion list.
 * Static bookkeeping avoids consuming the small kernel stack or the heap
 * whose exhaustion/reclamation is being tested.
 */
#define SLUB_CHECK_OBJECTS 64
#define SLUB_CHECK_HELD_SIZE 2048
#define SLUB_CHECK_PAGE_WORDS ((KMEMSIZE / PGSIZE + 63) / 64)

struct slub_check_object {
    void *ptr;
    size_t size;
    size_t tag;
};

static struct slub_check_object slub_check_objects[SLUB_CHECK_OBJECTS];
static size_t slub_check_tag;
static uint64_t slub_check_held_pages[SLUB_CHECK_PAGE_WORDS];

/* This list node lives INSIDE a requested 2048-byte test object. */
struct slub_check_held {
    struct slub_check_held *next;
    size_t serial;
};

static uint8_t
slub_check_pattern(size_t tag, size_t offset) {
    return (uint8_t)((tag * 37 + offset * 13 + (offset >> 8) * 7) ^
                     (offset >> 3));
}

static void
slub_check_write(void *ptr, size_t size, size_t tag, size_t first) {
    assert(first <= size);
    volatile uint8_t *bytes = ptr;
    for (size_t i = first; i < size; i ++) {
        bytes[i] = slub_check_pattern(tag, i);
    }
}

static void
slub_check_read(const void *ptr, size_t size, size_t tag, size_t first) {
    assert(first <= size);
    const volatile uint8_t *bytes = ptr;
    for (size_t i = first; i < size; i ++) {
        assert(bytes[i] == slub_check_pattern(tag, i));
    }
}

/* Check user-visible extents before writing, without consulting slab headers. */
static void
slub_check_address(const void *ptr, size_t size) {
    uintptr_t address = (uintptr_t)ptr;
    assert(ptr != NULL && size != 0);
    assert(address % 16 == 0);  /* The advertised alignment, independently. */
    assert(address >= KERNBASE && address < KERNTOP);
    assert(size <= KERNTOP - address);
}

static void
slub_check_disjoint(const void *a, size_t a_size, const void *b, size_t b_size) {
    /* Both ranges have already been checked for address overflow. */
    uintptr_t a_start = (uintptr_t)a, b_start = (uintptr_t)b;
    assert(a_start + a_size <= b_start || b_start + b_size <= a_start);
}

static void
slub_check_contents(void) {
    for (size_t i = 0; i < SLUB_CHECK_OBJECTS; i ++) {
        struct slub_check_object *object = &slub_check_objects[i];
        if (object->ptr == NULL) {
            continue;
        }
        slub_check_address(object->ptr, object->size);
        for (size_t j = 0; j < i; j ++) {
            struct slub_check_object *other = &slub_check_objects[j];
            if (other->ptr != NULL) {
                slub_check_disjoint(object->ptr, object->size,
                                    other->ptr, other->size);
            }
        }
        slub_check_read(object->ptr, object->size, object->tag, 0);
    }
}

static void *
slub_check_new(size_t slot, size_t size) {
    assert(slot < SLUB_CHECK_OBJECTS && slub_check_objects[slot].ptr == NULL);
    void *ptr = slub_alloc(size);
    slub_check_address(ptr, size);  /* Success cases must actually succeed. */
    for (size_t i = 0; i < SLUB_CHECK_OBJECTS; i ++) {
        struct slub_check_object *other = &slub_check_objects[i];
        if (other->ptr != NULL) {
            slub_check_disjoint(ptr, size, other->ptr, other->size);
        }
    }
    struct slub_check_object *object = &slub_check_objects[slot];
    object->ptr = ptr;
    object->size = size;
    object->tag = ++ slub_check_tag;
    slub_check_write(ptr, size, object->tag, 0);
    slub_check_contents();
    return ptr;
}

static void
slub_check_delete(size_t slot) {
    assert(slot < SLUB_CHECK_OBJECTS && slub_check_objects[slot].ptr != NULL);
    slub_check_contents();
    slub_free(slub_check_objects[slot].ptr);
    slub_check_objects[slot].ptr = NULL;
    slub_check_objects[slot].size = 0;
    slub_check_objects[slot].tag = 0;
    slub_check_contents();  /* Freeing one object must not damage another. */
}

static void
slub_check_clean(size_t expected_free) {
    for (size_t i = 0; i < SLUB_CHECK_OBJECTS; i ++) {
        assert(slub_check_objects[i].ptr == NULL);
    }
    for (size_t i = 0; i < SLUB_CLASS_COUNT; i ++) {
        assert(list_empty(&slub_partial[i]));
    }
    assert(nr_free_pages() == expected_free);
}

/* A failed allocation (or NULL free) may not change lists, pages, or data. */
static void
slub_check_no_effect(size_t request, bool free_null) {
    list_entry_t saved_lists[SLUB_CLASS_COUNT];
    size_t before = nr_free_pages();
    slub_check_contents();
    for (size_t i = 0; i < SLUB_CLASS_COUNT; i ++) {
        saved_lists[i] = slub_partial[i];
    }
    if (free_null) {
        slub_free(NULL);
    } else {
        assert(slub_alloc(request) == NULL);
    }
    assert(nr_free_pages() == before);
    for (size_t i = 0; i < SLUB_CLASS_COUNT; i ++) {
        assert(slub_partial[i].prev == saved_lists[i].prev);
        assert(slub_partial[i].next == saved_lists[i].next);
    }
    slub_check_contents();
}

static void
slub_check_boundaries(size_t baseline) {
    /* Explicit requests on both sides of every published size-class boundary. */
    static const size_t requests[] = {
        1, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 511, 512, 513,
        1023, 1024, 1025, 2047, 2048, 2049,
        4095, 4096, 4097, 8191, 8192, 8193, 12288,
    };
    for (size_t i = 0; i < sizeof(requests) / sizeof(requests[0]); i ++) {
        slub_check_new(0, requests[i]);
        slub_check_new(1, requests[i]);
        slub_check_new(2, 7);
        slub_check_no_effect(0, 0);
        slub_check_no_effect(0, 1);
        slub_check_delete(1);
        slub_check_new(1, requests[i]);
        slub_check_delete(0);
        slub_check_delete(2);
        slub_check_delete(1);
        slub_check_clean(baseline);
    }
}

static void
slub_check_expansion(size_t baseline) {
    void *first = slub_check_new(0, 128);
    uintptr_t first_page = (uintptr_t)first & ~((uintptr_t)PGSIZE - 1);
    assert(nr_free_pages() == baseline - 1);
    size_t second = 0;
    /* Even without metadata, a page cannot hold more than PGSIZE/128 objects.
     * Observe the real transition; do not copy the allocator's capacity math. */
    for (size_t i = 1; i <= PGSIZE / 128; i ++) {
        size_t before = nr_free_pages();
        void *ptr = slub_check_new(i, 128);
        uintptr_t owner = (uintptr_t)ptr & ~((uintptr_t)PGSIZE - 1);
        if (owner != first_page) {
            assert(nr_free_pages() == before - 1);
            second = i;
            break;
        }
        assert(nr_free_pages() == before);
    }
    assert(second > 1 && second <= PGSIZE / 128);
    assert(nr_free_pages() == baseline - 2);

    slub_check_delete(second);  /* The second page now has no live objects. */
    assert(nr_free_pages() == baseline - 1);
    slub_check_delete(0);       /* One vacancy in the otherwise full first page. */
    assert(nr_free_pages() == baseline - 1);
    assert(slub_check_new(0, 128) == first);  /* The only available slot. */
    assert(nr_free_pages() == baseline - 1);
    slub_check_new(second, 128);  /* Expand again after refilling the vacancy. */
    assert(nr_free_pages() == baseline - 2);

    for (size_t i = 0; i + 1 < second; i ++) {
        slub_check_delete(i);
        assert(nr_free_pages() == baseline - 2);  /* Still one live object. */
    }
    slub_check_delete(second - 1);
    assert(nr_free_pages() == baseline - 1);
    slub_check_delete(second);
    slub_check_clean(baseline);
}

static void
slub_check_bad_requests(size_t baseline) {
    slub_check_new(0, 1);
    slub_check_new(1, 129);
    slub_check_new(2, 8192);  /* Spans pages; Buddy must reclaim its whole block. */
    slub_check_no_effect(0, 0);
    slub_check_no_effect(0, 1);
    slub_check_no_effect((size_t)-1, 0);        /* Header addition overflows. */
    slub_check_no_effect((size_t)-16, 0);
    slub_check_no_effect((size_t)-PGSIZE, 0);  /* Page rounding exceeds size_t. */
    slub_check_no_effect((size_t)1 << (sizeof(size_t) * 8 - 1), 0);
    slub_check_no_effect((size_t)KMEMSIZE, 0); /* No room for allocation metadata. */
    slub_check_no_effect((size_t)KMEMSIZE + PGSIZE, 0);
    slub_check_delete(1);
    slub_check_new(1, 129);  /* A rejected request must leave the allocator usable. */
    slub_check_delete(2);
    slub_check_delete(0);
    slub_check_delete(1);
    slub_check_clean(baseline);
}

static size_t
slub_check_held_index(const struct slub_check_held *object) {
    slub_check_address(object, SLUB_CHECK_HELD_SIZE);
    uintptr_t address = (uintptr_t)object;
    assert((address & (PGSIZE - 1)) + SLUB_CHECK_HELD_SIZE <= PGSIZE);
    return (address - KERNBASE) / PGSIZE;
}

static void
slub_check_held_contents(const struct slub_check_held *head, size_t count) {
    while (count > 0) {
        size_t index = slub_check_held_index(head);
        uint64_t bit = (uint64_t)1 << (index % 64);
        assert(slub_check_held_pages[index / 64] & bit);
        assert(head->serial == count);
        slub_check_read(head, SLUB_CHECK_HELD_SIZE, count, sizeof(*head));
        head = head->next;
        count --;
    }
    assert(head == NULL);  /* Detect cycles, omissions and damaged next links. */
}

static struct slub_check_held *
slub_check_release_held(struct slub_check_held *head, size_t count) {
    size_t index = slub_check_held_index(head);
    uint64_t bit = (uint64_t)1 << (index % 64);
    assert(slub_check_held_pages[index / 64] & bit);
    assert(head->serial == count);
    slub_check_read(head, SLUB_CHECK_HELD_SIZE, count, sizeof(*head));
    struct slub_check_held *next = head->next;
    size_t before = nr_free_pages();
    slub_free(head);
    slub_check_held_pages[index / 64] &= ~bit;
    assert(nr_free_pages() == before + 1);
    return next;
}

static void
slub_check_exhaustion(size_t baseline) {
    slub_check_new(0, 16);    /* Keep a partially occupied small-object slab. */
    slub_check_new(1, 4096);  /* Keep a live object crossing a page boundary. */
    size_t available = nr_free_pages();
    assert(available > 0);
    for (size_t i = 0; i < SLUB_CHECK_PAGE_WORDS; i ++) {
        assert(slub_check_held_pages[i] == 0);
    }

    struct slub_check_held *head = NULL;
    size_t count = 0;
    for (;;) {
        size_t before = nr_free_pages();
        struct slub_check_held *object = slub_alloc(SLUB_CHECK_HELD_SIZE);
        if (object == NULL) {
            /* A NULL alone is insufficient evidence of genuine exhaustion. */
            assert(before == 0 && nr_free_pages() == 0);
            break;
        }
        /* Two 2048-byte objects cannot share a page that also has metadata. */
        assert(before > 0 && nr_free_pages() == before - 1);
        assert(count < available);
        size_t index = slub_check_held_index(object);
        uint64_t bit = (uint64_t)1 << (index % 64);
        assert((slub_check_held_pages[index / 64] & bit) == 0);
        slub_check_held_pages[index / 64] |= bit;
        for (size_t i = 0; i < SLUB_CHECK_OBJECTS; i ++) {
            struct slub_check_object *other = &slub_check_objects[i];
            if (other->ptr != NULL) {
                slub_check_disjoint(object, SLUB_CHECK_HELD_SIZE,
                                    other->ptr, other->size);
            }
        }
        object->next = head;
        object->serial = ++ count;
        slub_check_write(object, SLUB_CHECK_HELD_SIZE, count, sizeof(*object));
        head = object;
    }
    assert(count == available);
    slub_check_contents();
    slub_check_held_contents(head, count);
    slub_check_no_effect(128, 0);   /* A new small slab needs a page. */
    slub_check_no_effect(2048, 0);
    slub_check_no_effect(4096, 0);  /* Direct multi-page allocation also fails. */
    slub_check_no_effect(8192, 0);
    slub_check_no_effect(0, 0);
    slub_check_no_effect((size_t)-1, 0);
    slub_check_no_effect(0, 1);

    /* Existing slots remain usable even though the PMM has zero free pages. */
    slub_check_new(2, 16);
    assert(nr_free_pages() == 0);
    slub_check_delete(2);
    assert(nr_free_pages() == 0);
    slub_check_new(2, 16);
    assert(nr_free_pages() == 0);
    slub_check_delete(2);
    slub_check_held_contents(head, count);

    /* Return one page, then prove both a failed size class and a new slab can
     * allocate again.  Remaining exhaustion objects must retain their data. */
    head = slub_check_release_held(head, count --);
    assert(nr_free_pages() == 1);
    slub_check_new(2, 2048);
    assert(nr_free_pages() == 0);
    slub_check_delete(2);
    assert(nr_free_pages() == 1);
    slub_check_new(2, 128);
    assert(nr_free_pages() == 0);
    slub_check_delete(2);
    assert(nr_free_pages() == 1);
    while (count > 0) {
        head = slub_check_release_held(head, count --);
    }
    assert(head == NULL && nr_free_pages() == available);
    for (size_t i = 0; i < SLUB_CHECK_PAGE_WORDS; i ++) {
        assert(slub_check_held_pages[i] == 0);
    }
    slub_check_contents();
    slub_check_delete(1);
    slub_check_delete(0);
    slub_check_clean(baseline);
}

/*
 * Invoke after PMM/SLUB initialization, with no outstanding SLUB objects,
 * during exclusive single-hart boot.  Successful cases need a small working
 * set (at least nine free pages, including two allocatable four-page blocks).
 * Insufficient test resources assert; mandatory cases are never skipped.
 */
void
slub_check(void) {
    unsigned long saved_status = clear_csr(sstatus, SSTATUS_SIE);
    asm volatile ("" ::: "memory");
    assert(slub_initialized);
    size_t baseline = nr_free_pages();
    assert(baseline >= 9);
    slub_check_clean(baseline);
    struct Page *saved_pages = pages;
    size_t saved_npage = npage;
    uint64_t saved_offset = va_pa_offset;
    const struct pmm_manager *saved_manager = pmm_manager;

    slub_check_no_effect(0, 0);
    slub_check_no_effect(0, 1);
    slub_check_boundaries(baseline);
    slub_check_expansion(baseline);
    slub_check_bad_requests(baseline);
    slub_check_exhaustion(baseline);

    slub_check_clean(baseline);
    assert(pages == saved_pages && npage == saved_npage);
    assert(va_pa_offset == saved_offset && pmm_manager == saved_manager);
    asm volatile ("" ::: "memory");
    if (saved_status & SSTATUS_SIE) {
        set_csr(sstatus, SSTATUS_SIE);
    }
    cprintf("slub_check() succeeded!\n");
}
