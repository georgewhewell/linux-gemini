#!/usr/bin/env python3
"""Exercise the actual seq reader and proc/stat emitter with host API seams.

Optional saved seq/stat sources run the same prefix acceptance against the old
production path. A rejected old optimization must compile and pass full output
before it fails on interrupt queries during the CPU-prefix read.
"""
from pathlib import Path
import os
import shlex
import subprocess
import sys
import tempfile


def block(source, signature, suffix=""):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + suffix


root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
seq = (Path(sys.argv[2]) if len(sys.argv) > 2 else root / "fs/seq_file.c").read_text()
stat = (Path(sys.argv[3]) if len(sys.argv) > 3 else root / "fs/proc/stat.c").read_text()
header = (root / "include/linux/seq_file.h").read_text()
has_one = "static ssize_t seq_read_iter_common(" in seq
seq_signatures = (
    "static void seq_set_overflow(", "static void *seq_buf_alloc(",
    "int seq_open(", "static int traverse(",
    *(("static ssize_t seq_read_iter_common(",) if has_one else ()),
    "ssize_t seq_read_iter(",
    *(("ssize_t seq_read_iter_one(",) if has_one else ()),
    "loff_t seq_lseek(", "int seq_release(",
    "void *single_start(", "static void *single_next(", "static void single_stop(",
    "int single_open(", "int single_open_size(", "int single_release(",
    "void seq_putc(", "int seq_write(", "void __seq_puts(",
)
seq_code = "\n".join(block(seq, name) for name in seq_signatures)
format_code = block(header, "static __always_inline void seq_puts(") + "\n"
format_code += "\n".join(block(seq, name) for name in (
    "void seq_vprintf(", "void seq_printf(", "void seq_put_decimal_ull_width(",
    "void seq_put_decimal_ull(",
))
stat_signatures = ["static void show_irq_gap(", "static void show_all_irqs("]
if "static int show_stat_cpus(" in stat:
    stat_signatures += ["static int show_stat_cpus(", "static int show_stat_tail(",
                        "static void *stat_start(", "static void *stat_next(",
                        "static void stat_stop(", "static int show_stat("]
else:
    stat_signatures += ["static int show_stat("]
stat_code = "\n".join(block(stat, name) for name in stat_signatures)
if "static const struct seq_operations stat_seq_ops" in stat:
    stat_code += "\n" + block(stat, "static const struct seq_operations stat_seq_ops", ";")
stat_code += "\n" + block(stat, "static int stat_open(")
stat_code += "\n" + block(stat, "static const struct proc_ops stat_proc_ops", ";")

program = r'''
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/uio.h>
typedef uint64_t u64;
typedef uint8_t u8;
#undef __always_inline
#define __always_inline inline __attribute__((always_inline))
#define unlikely(v) (v)
#define fallthrough __attribute__((__fallthrough__))
#define GFP_KERNEL 0
#define GFP_KERNEL_ACCOUNT 0
#define PAGE_SIZE 64 /* Force real production overflow/retry paths. */
#define MAX_RW_COUNT INT_MAX
#define FMODE_PWRITE 1
#define PROC_ENTRY_PERMANENT 1
#define SEQ_START_TOKEN ((void *)1)
#define SEQ_SKIP 1
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define ERR_PTR(v) ((void *)(intptr_t)(v))
#define PTR_ERR(v) ((intptr_t)(v))
#define IS_ERR(v) ((uintptr_t)(v) >= (uintptr_t)-4095)
#define WARN_ON(v) assert(!(v))
#define BUG_ON(v) assert(!(v))
#define pr_info_ratelimited(...) abort()
struct mutex { bool held; };
static void mutex_init(struct mutex *m) { m->held = false; }
static void mutex_lock(struct mutex *m) { assert(!m->held); m->held = true; }
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
struct file { void *private_data; unsigned f_mode; loff_t f_pos; };
struct inode { unsigned unused; };
struct kiocb { struct file *ki_filp; loff_t ki_pos; };
SEQ_STRUCTS
static size_t allocations, live_allocations, allocation_fail;
static void *host_alloc(size_t n, bool zero)
{
    if (++allocations == allocation_fail) return NULL;
    void *p = zero ? calloc(1, n) : malloc(n);
    if (p) live_allocations++;
    return p;
}
static void host_free(const void *p) { if (p) { assert(live_allocations); live_allocations--; free((void *)p); } }
#define kvmalloc(n, flags) host_alloc(n, false)
#define kvfree(p) host_free(p)
#define kfree(p) host_free(p)
#define kmalloc_obj(obj, flags) host_alloc(sizeof(obj), false)
#define kmem_cache_zalloc(cache, flags) host_alloc(sizeof(struct seq_file), true)
#define kmem_cache_free(cache, p) host_free(p)
struct iov_iter {
    struct iovec *iov;
    size_t nr, index, offset, count, fault_after, copied;
};
static size_t iov_iter_count(const struct iov_iter *i) { return i->count; }
static size_t copy_to_iter(const void *buf, size_t n, struct iov_iter *i)
{
    size_t done = 0;
    if (n > i->count) n = i->count;
    if (n > i->fault_after - i->copied) n = i->fault_after - i->copied;
    while (done < n) {
        assert(i->index < i->nr);
        size_t available = i->iov[i->index].iov_len - i->offset;
        if (!available) { i->index++; i->offset = 0; continue; }
        size_t part = n - done < available ? n - done : available;
        memcpy((char *)i->iov[i->index].iov_base + i->offset, (const char *)buf + done, part);
        i->offset += part; i->count -= part; i->copied += part; done += part;
    }
    return done;
}
static int num_to_str(char *buf, int size, unsigned long long v, unsigned width)
{
    char local[64];
    int n = snprintf(local, sizeof(local), "%*llu", (int)width, v);
    assert(n > 0 && n < (int)sizeof(local));
    if (n > size) return 0;
    memcpy(buf, local, (size_t)n);
    return n;
}
static inline bool seq_has_overflowed(struct seq_file *m) { return m->count == m->size; }
SEQ_CODE
FORMAT_CODE
struct proc_ops {
    unsigned proc_flags;
    int (*proc_open)(struct inode *, struct file *);
    ssize_t (*proc_read_iter)(struct kiocb *, struct iov_iter *);
    loff_t (*proc_lseek)(struct file *, loff_t, int);
    int (*proc_release)(struct inode *, struct file *);
};
enum { CPUTIME_USER, CPUTIME_NICE, CPUTIME_SYSTEM, CPUTIME_IDLE, CPUTIME_IOWAIT,
       CPUTIME_IRQ, CPUTIME_SOFTIRQ, CPUTIME_STEAL, CPUTIME_GUEST, CPUTIME_GUEST_NICE };
#define NR_SOFTIRQS 10
struct kernel_cpustat { u64 cpustat[10]; };
struct timespec64 { int64_t tv_sec; };
static struct kernel_cpustat cpu_values[4];
static unsigned online_mask = 11;
static unsigned irq_queries, irq_cpu_queries, softirq_queries, boot_queries, fetches;
static unsigned long total_forks = 88;
#define for_each_possible_cpu(i) for ((i) = 0; (i) < 4; (i)++)
#define for_each_online_cpu(i) for ((i) = 0; (i) < 4; (i)++) if (online_mask & (1u << (i)))
#define for_each_active_irq(i) for ((i) = 0; (i) < 8; (i)++) if ((i) == 1 || (i) == 3 || (i) == 4)
static unsigned irq_get_nr_irqs(void) { return 8; }
#define num_online_cpus() ((unsigned)__builtin_popcount(online_mask))
static void kcpustat_cpu_fetch(struct kernel_cpustat *p, int cpu)
{ *p = cpu_values[cpu]; p->cpustat[CPUTIME_IDLE] += 10u * (unsigned)cpu;
  p->cpustat[CPUTIME_IOWAIT] += (unsigned)cpu; fetches++; }
static u64 nsec_to_clock_t(u64 n) { return n; }
static unsigned kstat_cpu_irqs_sum(int cpu) { irq_cpu_queries++; return 10u * ((unsigned)cpu + 1); }
#define arch_irq_stat_cpu(cpu) ((unsigned)(cpu) + 1)
static unsigned kstat_softirqs_cpu(int j, int cpu) { softirq_queries++; return 10u * (unsigned)cpu + (unsigned)j; }
static unsigned kstat_irqs_usr(unsigned irq) { irq_queries++; return irq * 100 + 1; }
static void getboottime64(struct timespec64 *t) { boot_queries++; t->tv_sec = 1234567; }
static void timens_sub_boottime(struct timespec64 *t) { t->tv_sec -= 33; }
static unsigned long long nr_context_switches(void) { return 77; }
static unsigned nr_running(void) { return 9; }
static unsigned nr_iowait(void) { return 2; }
STAT_CODE
static const char expected[] =
 "cpu  406 10 86 2066 24 22 26 30 34 38\n"
 "cpu0 100 1 20 500 3 4 5 6 7 8\n"
 "cpu1 101 2 21 511 5 5 6 7 8 9\n"
 "cpu3 103 4 23 533 9 7 8 9 10 11\n"
 "intr 110 0 101 0 301 401 0 0 0\nctxt 77\nbtime 1234534\nprocesses 88\n"
 "procs_running 9\nprocs_blocked 2\nsoftirq 780 60 64 68 72 76 80 84 88 92 96\n";
static void reset_stats(void)
{
    for (unsigned i = 0; i < 4; i++) {
        u64 initial[] = {100+i, 1+i, 20+i, 500+i, 3+i, 4+i, 5+i, 6+i, 7+i, 8+i};
        memcpy(cpu_values[i].cpustat, initial, sizeof(initial));
    }
    online_mask = 11; irq_queries = irq_cpu_queries = softirq_queries = boot_queries = fetches = 0;
}
static struct file open_stat(void)
{
    struct file f = {0};
    assert(stat_proc_ops.proc_open(NULL, &f) == 0);
    return f;
}
static void release_stat(struct file *f) { assert(stat_proc_ops.proc_release(NULL, f) == 0); assert(!live_allocations); }
static ssize_t read_iov(struct file *f, struct iovec *iov, size_t nr, size_t fault,
                        bool greedy, loff_t position, bool pread)
{
    struct iov_iter iter = {.iov=iov, .nr=nr, .fault_after=fault};
    struct kiocb k = {.ki_filp=f, .ki_pos=position};
    for (size_t j = 0; j < nr; j++) iter.count += iov[j].iov_len;
    ssize_t n = greedy ? seq_read_iter(&k, &iter) : stat_proc_ops.proc_read_iter(&k, &iter);
    if (!pread) f->f_pos = k.ki_pos;
    assert(!((struct seq_file *)f->private_data)->lock.held);
    return n;
}
static ssize_t read_piece(struct file *f, char *buf, size_t n, bool greedy)
{ struct iovec iov = {.iov_base=buf, .iov_len=n}; return read_iov(f, &iov, 1, SIZE_MAX, greedy, f->f_pos, false); }
static size_t read_rest(struct file *f, char *buf, size_t chunk, bool greedy)
{
    size_t used = 0;
    for (;;) {
        assert(used + chunk < 4096);
        ssize_t n = read_piece(f, buf + used, chunk, greedy);
        assert(n >= 0);
        if (!n) break;
        used += (size_t)n;
    }
    buf[used] = 0;
    return used;
}
static void full_output(void)
{
    const size_t chunks[] = {1, 2, 3, 7, 64, 127, 1024};
    for (unsigned greedy = 0; greedy < 2; greedy++) {
        for (size_t j = 0; j < ARRAY_SIZE(chunks); j++) {
            reset_stats(); struct file f = open_stat(); char buf[4096];
            assert(read_rest(&f, buf, chunks[j], greedy) == strlen(expected));
            assert(!strcmp(buf, expected));
            assert(irq_queries >= 3 && irq_cpu_queries >= 4 && softirq_queries >= 40 && boot_queries);
            assert(fetches >= 7);
            release_stat(&f);
        }
    }
}
static void prefix_only(void)
{
    const size_t head = (size_t)(strstr(expected, "\nintr ") - expected) + 6;
    reset_stats(); struct file f = open_stat(); char buf[4096];
    ssize_t n = read_piece(&f, buf, sizeof(buf), false);
    if (irq_queries || irq_cpu_queries || softirq_queries || boot_queries) {
        fprintf(stderr, "PREFIX_IRQ_WORK: irq_queries=%u irq_cpu_queries=%u softirq_queries=%u boot_queries=%u\n",
                irq_queries, irq_cpu_queries, softirq_queries, boot_queries);
        release_stat(&f);
        exit(1);
    }
    assert(n == (ssize_t)head && !memcmp(buf, expected, head));
    assert(read_rest(&f, buf, 1024, false) == strlen(expected) - head);
    assert(!strcmp(buf, expected + head)); release_stat(&f);
    /* Draining the last buffered byte must not prefetch the tail, even with
       no space left in the destination iterator. */
    reset_stats(); f = open_stat();
    assert(read_piece(&f, buf, head - 1, false) == (ssize_t)head - 1);
    assert(read_piece(&f, buf + head - 1, 1, false) == 1);
    assert(!memcmp(buf, expected, head) && !irq_queries && !irq_cpu_queries && !boot_queries);
    assert(read_piece(&f, buf, 0, false) == 0 && !irq_queries);
    size_t allocated = allocations;
    assert(seq_lseek(&f, 0, SEEK_SET) == 0);
    online_mask = 5; cpu_values[0].cpustat[0] += 5;
    n = read_piece(&f, buf, sizeof(buf)-1, false); assert(n > 0); buf[n] = 0;
    assert(strstr(buf, "cpu0 105 ") && strstr(buf, "cpu2 102 ") && !strstr(buf, "cpu1 "));
    assert(allocations == allocated && !irq_queries);
    release_stat(&f);
    /* The unchanged greedy reader still fills through the tail. */
    reset_stats(); f = open_stat();
    n = read_piece(&f, buf, 1024, true);
    assert(n >= (ssize_t)head && n <= (ssize_t)strlen(expected));
    assert(irq_queries >= 3 && !memcmp(buf, expected, (size_t)n));
    assert(read_rest(&f, buf+(size_t)n, 1024, true) == strlen(expected)-(size_t)n);
    assert(!strcmp(buf, expected)); release_stat(&f);
}
static void seeks_and_iovecs(void)
{
    for (size_t offset = 0; offset < strlen(expected) + 4; offset++) {
        reset_stats(); struct file f = open_stat(); char buf[4096];
        assert(seq_lseek(&f, (loff_t)offset, SEEK_SET) == (loff_t)offset);
        size_t n = read_rest(&f, buf, 7, false);
        size_t rest = offset < strlen(expected) ? strlen(expected)-offset : 0;
        assert(n == rest && !memcmp(buf, expected + (offset < strlen(expected) ? offset : strlen(expected)), rest));
        assert(seq_lseek(&f, -1, SEEK_SET) == -EINVAL);
        assert(seq_lseek(&f, 0, SEEK_END) == -EINVAL);
        assert(seq_lseek(&f, -(loff_t)f.f_pos, SEEK_CUR) == 0);
        assert(read_piece(&f, buf, 3, false) == 3 && !memcmp(buf, expected, 3));
        release_stat(&f);
    }
    reset_stats(); struct file f = open_stat(); char a[3], b[11], c[4096];
    struct iovec iov[] = {{a, 0}, {a, sizeof(a)}, {b, sizeof(b)}, {c, 5}};
    assert(read_iov(&f, iov, ARRAY_SIZE(iov), SIZE_MAX, false, 0, false) == 19);
    assert(!memcmp(a, expected, 3) && !memcmp(b, expected+3, 11) && !memcmp(c, expected+14, 5));
    loff_t original = f.f_pos;
    struct iovec pv = {c, 4};
    assert(read_iov(&f, &pv, 1, SIZE_MAX, false, 1, true) == 4);
    assert(f.f_pos == original && !memcmp(c, expected+1, 4));
    assert(read_piece(&f, c, 5, false) == 5 && !memcmp(c, expected+original, 5));
    release_stat(&f);
}
static unsigned mode;
static void *error_start(struct seq_file *m, loff_t *pos)
{
    if (mode == 1) return ERR_PTR(-EIO);
    if (mode == 5 && *pos == 1) return ERR_PTR(-ENXIO);
    return *pos < 2 ? pos : NULL;
}
static void *error_next(struct seq_file *m, void *v, loff_t *pos)
{ ++*pos; return mode == 5 && *pos == 1 ? ERR_PTR(-ENXIO) : error_start(m, pos); }
static int error_show(struct seq_file *m, void *v)
{
    if (*(loff_t *)v == 0) {
        if (mode == 2) return -EIO;
        if (mode == 3) { seq_puts(m, "discarded"); return SEQ_SKIP; }
        if (mode == 4) return 0;
    }
    seq_puts(m, "ok\n"); return 0;
}
static const struct seq_operations error_ops = {
    .start=error_start, .next=error_next, .stop=single_stop, .show=error_show,
};
static void failures(void)
{
    for (mode = 1; mode <= 5; mode++) {
        struct file f = {0}; char buf[64]; assert(seq_open(&f, &error_ops) == 0);
        size_t allocated = allocations;
        assert(read_piece(&f, buf, 0, false) == 0 && allocations == allocated);
        ssize_t n = read_piece(&f, buf, sizeof(buf), false);
        if (mode <= 2) {
            assert(n == -EIO && !f.f_pos);
            assert(seq_lseek(&f, 1, SEEK_SET) == -EIO && !f.f_pos);
        }
        else { assert(n == 3 && !memcmp(buf, "ok\n", 3));
               if (mode == 5) assert(read_piece(&f, buf, sizeof(buf), false) == -ENXIO); }
        assert(seq_release(NULL, &f) == 0 && !live_allocations);
    }
    reset_stats(); struct file f = open_stat(); char buf[4096];
    struct iovec iov = {buf, sizeof(buf)};
    assert(read_iov(&f, &iov, 1, 0, false, 0, false) == -EFAULT && f.f_pos == 0);
    assert(!irq_queries && !irq_cpu_queries);
    assert(read_iov(&f, &iov, 1, 5, false, 0, false) == 5 && f.f_pos == 5);
    assert(read_piece(&f, buf, 8, false) == 8 && !memcmp(buf, expected+5, 8));
    release_stat(&f);
    reset_stats(); f = open_stat(); allocation_fail = allocations+1;
    assert(read_piece(&f, buf, sizeof(buf), false) == -ENOMEM && f.f_pos == 0);
    allocation_fail = 0; assert(read_piece(&f, buf, 2, false) == 2); release_stat(&f);
    reset_stats(); f = open_stat(); allocation_fail = allocations+2;
    assert(read_piece(&f, buf, sizeof(buf), false) == -ENOMEM && f.f_pos == 0);
    allocation_fail = 0; assert(read_piece(&f, buf, 2, false) == 2); release_stat(&f);
}
int main(void)
{
    full_output(); /* Old production must pass before the intended rejection. */
    prefix_only(); seeks_and_iovecs(); failures();
    puts("proc-stat-host: PASS prefix-no-IRQ/full-bytes/greedy/fragment/iovec/seek/rewind/overflow/errors");
    return 0;
}
'''
structs = block(header, "struct seq_file {", ";") + "\n" + block(header, "struct seq_operations {", ";")
for marker, value in {"SEQ_STRUCTS": structs, "SEQ_CODE": seq_code,
                      "FORMAT_CODE": format_code, "STAT_CODE": stat_code}.items():
    program = program.replace(marker, value)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "proc-stat.c"
    binary = Path(tmp) / "proc-stat"
    source.write_text(program)
    command = shlex.split(os.environ.get("HOSTCC", os.environ.get("CC", "cc")))
    flags = shlex.split(os.environ.get("HOSTCFLAGS", ""))
    # Native W=1 also suppresses signed comparisons and unused parameters;
    # traverse's existing loff_t/size_t comparison is kept exactly as shipped.
    subprocess.run(command + ["-std=gnu11", "-Wall", "-Wextra", "-Werror",
        "-Wno-unused-parameter", "-Wno-sign-compare", "-O2", "-g",
        "-fno-pie", "-no-pie", "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer", *flags,
        str(source), "-o", str(binary)], check=True)
    print("proc-stat-host: compiled actual seq/stat sources", flush=True)
    subprocess.run([str(binary)], check=True)
