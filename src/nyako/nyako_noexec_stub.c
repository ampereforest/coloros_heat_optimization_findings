#include <stdint.h>
#include <stddef.h>

/*
 * Zero-execmem / Zero-SELinux helper stub embedded in libnyako_core.so
 * at vaddr 0x190000 (PT_LOAD R E, offset 0x178000, size 0x44000).
 *
 * Fixed layout:
 *   0x18c100 : uint64_t g_pool_off   (RW .bss tail in PT_LOAD[3])
 *   0x18c108 : uint32_t g_dump_lock  (RW .bss tail in PT_LOAD[3])
 *   0x190000 : entry jump table (4 fixed 4-byte entrypoints):
 *              +0x00: b nyako_codepatch_entry   (from 0x105214)
 *              +0x04: b nyako_dobby_alloc_entry (from 0x1049ac)
 *              +0x08: b nyako_lsplant_alloc_entry (from 0xd25c4)
 *              +0x0c: b nyako_lsplant_write20_entry (from 0xd2708)
 *   0x194000 : 256 KiB (0x40000) pre-mapped R-X private COW trampoline pool
 */

#define POOL_SIZE        0x40000ULL
#define AT_FDCWD         (-100L)
#define O_RDWR           2
#define O_CLOEXEC        0x80000
#define PR_GET_DUMPABLE  3
#define PR_SET_DUMPABLE  4
#define SYS_openat       56L
#define SYS_close        57L
#define SYS_pwrite64     68L
#define SYS_prctl        167L
#define EINTR            4L

static inline long sys4(long nr, long a0, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
    return x0;
}

static inline long sys5(long nr, long a0, long a1, long a2, long a3, long a4) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                     : "memory", "cc");
    return x0;
}

static inline long sys1(long nr, long a0) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8)
                     : "memory", "cc");
    return x0;
}

/* Compute exact runtime base of libnyako_core.so via PC-relative ADR on _start (0x190000). */
extern char _stub_start[];
static inline uintptr_t get_lib_base(void) {
    uintptr_t p;
    __asm__("adrp %0, _stub_start\n\tadd %0, %0, :lo12:_stub_start" : "=r"(p));
    return p - 0x190000ULL;
}

static void clear_icache(uintptr_t start, uintptr_t end) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if (!(ctr & (1ULL << 28))) { /* IDC == 0 -> clean D-cache to PoU */
        uint64_t dline = 4ULL << ((ctr >> 16) & 0xf);
        for (uintptr_t p = start & ~(dline - 1); p < end; p += dline) {
            __asm__ volatile("dc cvau, %0" :: "r"(p) : "memory");
        }
    }
    __asm__ volatile("dsb ish" ::: "memory");
    if (!(ctr & (1ULL << 29))) { /* DIC == 0 -> invalidate I-cache to PoU */
        uint64_t iline = 4ULL << (ctr & 0xf);
        for (uintptr_t p = start & ~(iline - 1); p < end; p += iline) {
            __asm__ volatile("ic ivau, %0" :: "r"(p) : "memory");
        }
        __asm__ volatile("dsb ish" ::: "memory");
    }
    __asm__ volatile("isb" ::: "memory");
}

static void spin_lock(volatile uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile("yield" ::: "memory");
    }
}

static void spin_unlock(volatile uint32_t *lock) {
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}

static int open_self_mem(uintptr_t base) {
    static const char path[] = "/proc/self/mem";
    long fd = sys4(SYS_openat, AT_FDCWD, (long)path, O_RDWR | O_CLOEXEC, 0);
    if (fd >= 0) return (int)fd;

    volatile uint32_t *lock = (volatile uint32_t *)(base + 0x18c108ULL);
    spin_lock(lock);
    long old_d = sys5(SYS_prctl, PR_GET_DUMPABLE, 0, 0, 0, 0);
    if (old_d == 0) {
        sys5(SYS_prctl, PR_SET_DUMPABLE, 1, 0, 0, 0);
    }
    fd = sys4(SYS_openat, AT_FDCWD, (long)path, O_RDWR | O_CLOEXEC, 0);
    if (old_d == 0) {
        sys5(SYS_prctl, PR_SET_DUMPABLE, 0, 0, 0, 0);
    }
    spin_unlock(lock);
    return (int)fd;
}

static int codepatch_impl(void *addr, const void *buf, uint32_t size) {
    if (!addr || !buf || size == 0) return 0;
    uintptr_t base = get_lib_base();
    int fd = open_self_mem(base);
    if (fd < 0) return -1;

    /* Untag destination VA (clear top byte TBI) for positive off_t in pwrite64 */
    uint64_t dst = ((uint64_t)(uintptr_t)addr) & 0x00ffffffffffffffULL;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;
    while (done < size) {
        long n = sys4(SYS_pwrite64, (long)fd, (long)(src + done), (long)(size - done), (long)(dst + done));
        if (n <= 0) {
            if (n == -EINTR) continue;
            sys1(SYS_close, (long)fd);
            return -1;
        }
        done += (uint32_t)n;
    }
    sys1(SYS_close, (long)fd);
    clear_icache(dst, dst + size);
    return 0;
}

static void *pool_alloc_impl(size_t size) {
    uintptr_t base = get_lib_base();
    volatile uint64_t *offp = (volatile uint64_t *)(base + 0x18c100ULL);
    uint64_t aligned = (size == 0) ? 0x1000ULL : ((size + 0xfffULL) & ~0xfffULL);
    while (1) {
        uint64_t cur = __atomic_load_n(offp, __ATOMIC_ACQUIRE);
        if (cur + aligned > POOL_SIZE) return (void *)0;
        if (__atomic_compare_exchange_n(offp, &cur, cur + aligned, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return (void *)(base + 0x194000ULL + cur);
        }
    }
}

/*
 * Entry 0 (+0x00): replaces 0x105214 (Dobby CodePatch(addr, buf, size) -> int).
 * Tries zero-execmem /proc/self/mem codepatch; if it ever fails, executes the
 * displaced prologue at 0x105214 and falls back to 0x105218.
 */
int nyako_codepatch_c(void *addr, const void *buf, uint32_t size) {
    return codepatch_impl(addr, buf, size);
}

/*
 * Entry 1 (+0x04): called from 0x1049ac (Dobby NearMemoryArena page alloc).
 * x0 = size. Returns r-xp page addr; falls back to original 0x1051b8 + 0x1051f0 if pool full.
 */
void *nyako_dobby_alloc_c(size_t size) {
    void *p = pool_alloc_impl(size);
    if (p) return p;
    uintptr_t base = get_lib_base();
    void *(*orig_alloc)(size_t, int) = (void *(*)(size_t, int))(base + 0x1051b8ULL);
    int (*orig_prot)(void *, size_t, int) = (int (*)(void *, size_t, int))(base + 0x1051f0ULL);
    p = orig_alloc(size, 0);
    if (p) orig_prot(p, size, 4);
    return p;
}

/*
 * Entry 2 (+0x08): called from 0xd25c4 (LSPlant trampoline page alloc).
 * x0 = page_size. Returns r-xp page addr (or MAP_FAILED (-1) on complete failure).
 */
void *nyako_lsplant_alloc_c(size_t size) {
    void *p = pool_alloc_impl(size);
    if (p) return p;
    /* Fallback: original mmap(NULL, size, RWX, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    uintptr_t base = get_lib_base();
    void *(*orig_mmap)(void *, size_t, int, int, int, long) =
        (void *(*)(void *, size_t, int, int, int, long))(base + 0x16a270ULL);
    return orig_mmap((void *)0, size, 7, 0x22, -1, 0);
}

/*
 * Entry 3 (+0x0c): called from 0xd2708 (LSPlant 20-byte trampoline writer).
 * x0 = rx_dst, x1 = stack_src (20 bytes), w2 = 20.
 */
void nyako_lsplant_write20_c(void *rx_dst, const void *src, uint32_t size) {
    if (codepatch_impl(rx_dst, src, size) != 0) {
        /* Fallback direct write if somehow RWX page was used */
        const uint8_t *s = (const uint8_t *)src;
        uint8_t *d = (uint8_t *)rx_dst;
        for (uint32_t i = 0; i < size; i++) d[i] = s[i];
        uintptr_t dst = ((uintptr_t)rx_dst) & 0x00ffffffffffffffULL;
        clear_icache(dst, dst + size);
    }
}

/*
 * Fixed jump table at 0x190000 (_stub_start) plus fallback trampoline for 0x105214.
 */
__asm__(
    ".section .text.entry, \"ax\", @progbits\n"
    ".global _stub_start\n"
    "_stub_start:\n"
    "  b nyako_codepatch_tramp\n"     /* +0x00: from 0x105214 */
    "  b nyako_dobby_alloc_c\n"       /* +0x04: from 0x1049ac */
    "  b nyako_lsplant_alloc_c\n"     /* +0x08: from 0xd25c4  */
    "  b nyako_lsplant_write20_c\n"   /* +0x0c: from 0xd2708  */
    "\n"
    "nyako_codepatch_tramp:\n"
    "  stp x29, x30, [sp, #-0x30]!\n"
    "  stp x0, x1, [sp, #0x10]\n"
    "  str x2, [sp, #0x20]\n"
    "  bl nyako_codepatch_c\n"
    "  cbnz w0, 1f\n"
    "  ldp x29, x30, [sp], #0x30\n"
    "  mov w0, #0\n"
    "  ret\n"
    "1:\n"
    "  ldr x2, [sp, #0x20]\n"
    "  ldp x0, x1, [sp, #0x10]\n"
    "  ldp x29, x30, [sp], #0x30\n"
    "  stp x30, x25, [sp, #-0x40]!\n" /* displaced insn at 0x105214 */
    "  adrp x16, _stub_start\n"
    "  add x16, x16, :lo12:_stub_start\n"
    "  sub x16, x16, #0x8a, lsl #12\n"
    "  sub x16, x16, #0xde8\n"
    "  br x16\n"
);
