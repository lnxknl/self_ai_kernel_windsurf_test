#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cacheflush.h"

/* Global cache configuration */
static struct cache_info cache_info;

/* Cache statistics */
static struct cache_stats {
    unsigned long icache_hits;
    unsigned long icache_misses;
    unsigned long dcache_hits;
    unsigned long dcache_misses;
} stats = {0};

/* Simulated cache lines */
struct cache_line {
    unsigned long tag;
    unsigned char valid;
    unsigned char dirty;
    unsigned char *data;
};

/* Simulated cache arrays */
static struct cache_line *icache = NULL;
static struct cache_line *dcache = NULL;

void init_cache_info(unsigned long icache_size, unsigned long dcache_size,
                    unsigned long icache_line_size, unsigned long dcache_line_size) {
    cache_info.icache_size = icache_size;
    cache_info.dcache_size = dcache_size;
    cache_info.icache_line_size = icache_line_size;
    cache_info.dcache_line_size = dcache_line_size;
    cache_info.has_icache = (icache_size > 0);
    cache_info.has_dcache = (dcache_size > 0);

    /* Allocate simulated cache arrays */
    if (cache_info.has_icache) {
        icache = calloc(icache_size / icache_line_size, sizeof(struct cache_line));
        for (size_t i = 0; i < icache_size / icache_line_size; i++) {
            icache[i].data = calloc(1, icache_line_size);
        }
    }

    if (cache_info.has_dcache) {
        dcache = calloc(dcache_size / dcache_line_size, sizeof(struct cache_line));
        for (size_t i = 0; i < dcache_size / dcache_line_size; i++) {
            dcache[i].data = calloc(1, dcache_line_size);
        }
    }
}

static void flush_dcache_line(unsigned long addr) {
    if (!cache_info.has_dcache)
        return;

    unsigned long line_size = cache_info.dcache_line_size;
    unsigned long line_mask = ~(line_size - 1);
    unsigned long tag = addr & line_mask;
    unsigned long index = (addr / line_size) % (cache_info.dcache_size / line_size);

    if (dcache[index].valid && dcache[index].tag == tag) {
        dcache[index].valid = 0;
        dcache[index].dirty = 0;
        stats.dcache_hits++;
    } else {
        stats.dcache_misses++;
    }
}

static void flush_icache_line(unsigned long addr) {
    if (!cache_info.has_icache)
        return;

    unsigned long line_size = cache_info.icache_line_size;
    unsigned long line_mask = ~(line_size - 1);
    unsigned long tag = addr & line_mask;
    unsigned long index = (addr / line_size) % (cache_info.icache_size / line_size);

    if (icache[index].valid && icache[index].tag == tag) {
        icache[index].valid = 0;
        stats.icache_hits++;
    } else {
        stats.icache_misses++;
    }
}

void flush_cache_all(void) {
    unsigned long i;

    /* Flush D-cache */
    if (cache_info.has_dcache) {
        for (i = 0; i < cache_info.dcache_size / cache_info.dcache_line_size; i++) {
            dcache[i].valid = 0;
            dcache[i].dirty = 0;
        }
    }

    /* Flush I-cache */
    if (cache_info.has_icache) {
        for (i = 0; i < cache_info.icache_size / cache_info.icache_line_size; i++) {
            icache[i].valid = 0;
        }
    }
}

void flush_icache_range(unsigned long start, unsigned long end) {
    unsigned long addr;
    
    start &= ~(cache_info.icache_line_size - 1);
    end = (end + cache_info.icache_line_size - 1) & ~(cache_info.icache_line_size - 1);

    for (addr = start; addr < end; addr += cache_info.icache_line_size) {
        flush_icache_line(addr);
    }
}

void flush_dcache_range(unsigned long start, unsigned long end) {
    unsigned long addr;
    
    start &= ~(cache_info.dcache_line_size - 1);
    end = (end + cache_info.dcache_line_size - 1) & ~(cache_info.dcache_line_size - 1);

    for (addr = start; addr < end; addr += cache_info.dcache_line_size) {
        flush_dcache_line(addr);
    }
}

void invalidate_dcache_range(unsigned long start, unsigned long end) {
    unsigned long addr;
    
    start &= ~(cache_info.dcache_line_size - 1);
    end = (end + cache_info.dcache_line_size - 1) & ~(cache_info.dcache_line_size - 1);

    for (addr = start; addr < end; addr += cache_info.dcache_line_size) {
        unsigned long index = (addr / cache_info.dcache_line_size) % 
                            (cache_info.dcache_size / cache_info.dcache_line_size);
        dcache[index].valid = 0;
    }
}

void *cached_memcpy(void *dest, const void *src, size_t n) {
    void *ret = memcpy(dest, src, n);
    flush_dcache_range((unsigned long)dest, (unsigned long)dest + n);
    return ret;
}

void cached_memset(void *s, int c, size_t n) {
    memset(s, c, n);
    flush_dcache_range((unsigned long)s, (unsigned long)s + n);
}

unsigned long get_icache_misses(void) { return stats.icache_misses; }
unsigned long get_dcache_misses(void) { return stats.dcache_misses; }
unsigned long get_icache_hits(void) { return stats.icache_hits; }
unsigned long get_dcache_hits(void) { return stats.dcache_hits; }

void print_cache_stats(void) {
    printf("\nCache Statistics:\n");
    printf("I-Cache hits  : %lu\n", stats.icache_hits);
    printf("I-Cache misses: %lu\n", stats.icache_misses);
    printf("D-Cache hits  : %lu\n", stats.dcache_hits);
    printf("D-Cache misses: %lu\n", stats.dcache_misses);
}

/* Example usage */
int main() {
    /* Initialize cache with example sizes */
    init_cache_info(
        16 * 1024,    /* 16KB I-cache */
        16 * 1024,    /* 16KB D-cache */
        32,           /* 32B I-cache line */
        32            /* 32B D-cache line */
    );

    /* Example memory buffer */
    char buffer[1024];
    
    /* Test cache operations */
    cached_memset(buffer, 0, sizeof(buffer));
    
    /* Flush various ranges */
    flush_dcache_range((unsigned long)buffer, (unsigned long)(buffer + 512));
    flush_icache_range((unsigned long)buffer, (unsigned long)(buffer + 512));
    invalidate_dcache_range((unsigned long)buffer, (unsigned long)(buffer + 1024));
    
    /* Copy some data */
    char src[256] = "Test data";
    cached_memcpy(buffer, src, strlen(src) + 1);
    
    /* Print cache statistics */
    print_cache_stats();
    
    return 0;
}
