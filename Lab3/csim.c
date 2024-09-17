/* Arden Diakhate-Palme
 * id: aqd
 * Cache simulator for Cache Lab
 */
#include "cachelab.h"
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// data structures: constituents of the cache
struct line {
    long tag;
    bool d;
    bool v;
    int index;
};
typedef struct line *line_t;

struct set {
    line_t *lines;
    int LRU;
};
typedef struct set *set_t;

struct cache {
    long S;
    long B;
    long E;
    set_t *data;
};
typedef struct cache *cache_t;

// initializes a group of data structures to represent a cache
// returns a data structure which represents a cache
cache_t initCache(int s, int E, int b) {
    int S = (0x1 << s);
    int B = (0x1 << b);
    cache_t C = (struct cache *)malloc(sizeof(struct cache));
    C->S = S;
    C->B = B;
    C->E = E;
    C->data = calloc(S, sizeof(set_t));
    set_t *cacheSets = C->data;

    for (int i = 0; i < S; i++) {
        cacheSets[i] = (struct set *)malloc(sizeof(struct set));
        set_t cache_set = cacheSets[i];

        cache_set->lines = (line_t *)malloc(sizeof(line_t) * E);
        line_t *setLines = cache_set->lines;
        cache_set->LRU = -1;
        // if direct mapped
        if (C->E == 1)
            cache_set->LRU = 0;

        for (int j = 0; j < E; j++) {
            setLines[j] = (struct line *)malloc(sizeof(struct line));
            line_t cache_line = setLines[j];
            if (setLines == NULL)
                printf("it's NULL!\n");
            if (cache_line == NULL)
                printf("it's cache is NULL!\n");
            cache_line->tag = 0;
            cache_line->v = false;
            cache_line->d = false;
            cache_line->index = j;
        }
    }
    return C;
}

// loads an address set and index into a structure representing a Cache
// updates stats with appropriate hits, misses, evictions, etc..
void load(int addr_index, long addr_tag, cache_t C, csim_stats_t *stats,
          char mode) {
    set_t currSet = C->data[addr_index];
    if (currSet == NULL)
        printf("it's bloody NULL\n");
    for (int i = 0; i < C->E; i++) {
        line_t L = currSet->lines[i];

        // compulsory miss
        if (!L->v) {
            L->tag = addr_tag;
            L->v = true;
            if (mode == 'S')
                L->d = true;
            if (C->E > 1) {
                if (L->index == 1)
                    currSet->LRU = 0;
                if (L->index == 0)
                    currSet->LRU = 1;
            }
            stats->misses++;
            return;
        }
        // cache hit
        if (L->v && L->tag == addr_tag) {
            if (mode == 'S')
                L->d = true;
            if (C->E > 1) {
                if (L->index == 1)
                    currSet->LRU = 0;
                if (L->index == 0)
                    currSet->LRU = 1;
            }
            stats->hits++;
            return;
        }
    }
    // eviction, miss
    line_t replace = currSet->lines[currSet->LRU];
    replace->tag = addr_tag;
    if (replace->d) {
        replace->d = false;
        stats->dirty_evictions += C->B;
    }
    if (mode == 'S')
        replace->d = true;
    if (C->E > 1) {
        if (replace->index == 1)
            currSet->LRU = 0;
        if (replace->index == 0)
            currSet->LRU = 1;
    }
    stats->evictions++;
    stats->misses++;
}

int main(int argc, char **argv) {

    int opt_success = getopt(argc, argv, "s:E:b:t:");
    extern char *optarg;
    extern int optind;

    int s = 0;
    int E = 0;
    int b = 0;
    const char *fileName = "file";
    const char *mode = "r";

    // parses arguments using getopt
    while (opt_success != -1) {
        switch (opt_success) {
        case 's':
            s = atoi(optarg);
            break;
        case 'E':
            E = atoi(optarg);
            break;
        case 'b':
            b = atoi(optarg);
            break;
        case 't':
            fileName = (char *)optarg;
            break;
        default:
            break;
        }
        opt_success = getopt(argc, argv, "s:E:b:t:");
    }

    cache_t C = initCache(s, E, b);
    csim_stats_t stats;

    FILE *instrFile;
    instrFile = fopen(fileName, mode);
    size_t instr_len = 30;
    char *instr = calloc(instr_len + 1, sizeof(char));
    char *success = "a";

    // reads each instruction string from file, loading the corresponding
    // addresses into the cache data structure
    while (success != NULL) {
        success = fgets(instr, instr_len, instrFile);
        char mode = instr[0];
        if (success != NULL) {
            char *addr_instr = instr + 2;
            char *lim = calloc(5, sizeof(char));
            unsigned long addr = strtoul(addr_instr, &lim, 16);

            if (mode == 'S' || mode == 'L') {

                long mask_s = (~(-1 << (b + s))) ^ (~(-1 << b));
                long addr_setIndex = (addr & mask_s) >> b;

                long mask_tag = (-1 << (s + b));
                long addr_tag = addr & mask_tag;

                load(addr_setIndex, addr_tag, C, &stats, mode);
            }
        }
    }

    // free all allocated memory
    for (int i = 0; i < C->S; i++) {
        set_t S = C->data[i];
        for (int j = 0; j < C->E; j++) {
            line_t L = S->lines[j];
            if (L->d) {
                stats.dirty_bytes += C->B;
            }
        }
        free(S->lines);
        free(S);
    }
    free(C->data);
    free(C);

    stats.dirty_evictions -= 1;
    printSummary(&stats);
    fclose(instrFile);
    free(instr);
    return 0;
}
