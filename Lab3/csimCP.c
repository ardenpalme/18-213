/* Arden Diakhate-Palme
 * id: aqd
 * Cache simulator for Cache Lab
 */
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include "cachelab.h"
#include "queue.h"

struct addr{
    int setIndex;
    int blockOffset;
    unsigned long tag;
    unsigned long orig;
};
typedef struct addr* addr_t;

struct line{
    long tag;
    bool d;
    bool v;
};
typedef struct line* line_t;

struct set{
    line_t *lines;
    long MRU;
    queue_t LRU;
};
typedef struct set* set_t;

struct cache{
    long S;
    long B;
    long E;
    set_t *data;
};
typedef struct cache* cache_t;

/*Queue Implementation*/
queue_t queue_new(){
    queue_t new= malloc(sizeof(struct queue_header));
    new->head= NULL;
    new->tail= NULL;
    return new;
}

bool queue_empty(queue_t q){
    if (q != NULL){
        if(q->head == NULL && q->tail == NULL) return true;
        return false;
    }
    return false;
}

void enq(queue_t q, long lineTag){
    elem e= malloc(sizeof(struct queue_elem));
    e->tag= lineTag;
    e->next= NULL;

    if (q->tail == NULL && q->head == NULL) {
        q->head = e;
        q->tail = e;
        return;
    }
    if(q->tail == q->head && q->head != NULL){
        elem tmp= q->head;
        tmp->next=e;
        q->tail= e;
        return;
    }
    q->tail->next = e;
    q->tail = e;
    return;

}
elem deq(queue_t q){
    elem tmp= q->head;
    if (q->head == q->tail && q->head != NULL) {
        q->tail = tmp->next;
    }
    q->head= tmp->next;
    return tmp;
}


//adapted from the cprogramming Lab 0 assignment
void queue_free(queue_t q) {
    if (q == NULL)
        return;
    // no elems
    if (q->head == NULL && q->tail == NULL) {
        free(q);
        return;
    }

    // one elem
    if (q->head == q->tail && q->head != NULL) {
        free(q->head);
        free(q);
        return;
    }

    if (q->head != q->tail && q->head != NULL) {
        elem tmp = q->head;
        elem tmp1 = q->head;
        while (tmp != q->tail) {
            tmp1 = tmp;
            tmp = tmp->next;
            free(tmp1);
        }
        // tmp is @ the tail
        free(tmp);
        free(q);
    }
}
void queue_print(queue_t q){
    if(queue_empty(q)) printf("The queue is empty..\n");
    elem tmp= q->head;
    while(tmp != NULL){
        printf("%x | ", tmp->tag);
        tmp= tmp->next;
    }printf("\n");
}

cache_t initCache(int s, int E, int b){
    int S= (0x1 << s);
    int B= (0x1 << b);
    cache_t C= malloc(sizeof(struct cache));
    C->S= S;
    C->B= B;
    C->E= E;
    set_t *cacheData= calloc(S, sizeof(set_t));

    for(int i=0; i<S; i++){
        set_t cache_set= malloc(sizeof(struct set));
        line_t *setLines= calloc(E, sizeof(line_t));
        cache_set->LRU= queue_new();
        cache_set->MRU=0;


        for(int j=0; j<E; j++){
            line_t cache_line= malloc(sizeof(struct line));
            cache_line->tag= 0;
            cache_line->v= false;
            cache_line->d= false;

            setLines[j]= cache_line;
        }

        cache_set->lines= setLines;
        cacheData[i]= cache_set;
    }
    C->data= cacheData;
    return C;
}


addr_t parseAddr(long addr, int s, int b){
    addr_t newAddr= malloc(sizeof(struct addr));
    unsigned long mask_B= ~(-1 << b);
    newAddr->blockOffset= addr & mask_B;

    unsigned long mask_s= (~(-1 << (b+s))) ^ (~(-1 << b));
    newAddr->setIndex= (addr & mask_s) >> b; //get the set index value

    unsigned long mask_tag= (-1 << (s+b));
    newAddr->tag= addr & mask_tag;
    newAddr->orig= addr;

    return newAddr;
}

void printAddr(addr_t A){
    printf("\torig: %x\n",A->orig);
    printf("\ttag: %x\n",A->tag);
    printf("\tsetIndex: %d\n",A->setIndex);
    printf("\tblockOffset: %d\n", A->blockOffset);
}


void load(addr_t A, cache_t C, csim_stats_t *stats, char mode){
    set_t currSet= C->data[A->setIndex];
    for(int i=0; i<C->E; i++){
        line_t L= currSet->lines[i];

        if(L->v){
            if(L->tag == A->tag){
                printf("hit\n");
                if(mode == 'S') L->d= true;
                enq(currSet->LRU, A->tag);
                currSet->MRU= A->tag;
                stats->hits++;
                return;
            }
        }else{
            printf("miss\n");
            if(mode == 'S') L->d= true;
            L->v= true;
            L->tag= A->tag;
            enq(currSet->LRU, A->tag);
            currSet->MRU= A->tag;
            stats->misses++;
            return;
        }

    }

    printf("miss eviction");
    elem tmp1= deq(currSet->LRU);
    while(tmp1 != NULL){
        long tagToEvict= tmp1->tag;
        for(int i=0; i<C->E; i++){
            line_t L= currSet->lines[i];
            if(tagToEvict == L->tag && tagToEvict != currSet->MRU){
                printf("evicting tag: %x\n",tagToEvict);
                if(L->d){
                    L->d= false;
                    stats->dirty_evictions+= C->B;
                }
                if(mode == 'S') L->d= true;
                L->tag= A->tag;

                enq(currSet->LRU, A->tag);
                currSet->MRU= A->tag;
                stats->evictions++;
                stats->misses++;
                return;
            }
        }
        tmp1= deq(currSet->LRU);
    }
}



int main(int argc, char **argv){
    int opt_success= getopt(argc, argv, "s:E:b:t:");
    extern char *optarg;
    extern int optind;

    int s=0;
    int E=0;
    int b=0;
    const char *fileName;
    const char *mode= "r";

    while(opt_success != -1){
        switch(opt_success){
            case 's':
                s= atoi(optarg);
                break;
            case 'E':
                E= atoi(optarg);
                break;
            case 'b':
                b= atoi(optarg);
                break;
            case 't':
                fileName= (char *)optarg;
                break;
            default:
                break;
        }
        opt_success= getopt(argc, argv, "s:E:b:t:");
    }

    cache_t C= (cache_t) initCache(s,E,b);
    csim_stats_t *stats= malloc(sizeof(csim_stats_t));

    FILE *instrFile;
    instrFile= fopen(fileName, mode);

    size_t instr_len= 15;
    char *instr= calloc(instr_len+1, sizeof(char)); //account for '\0'
    char *success= "a";

    while(success != NULL){
        success= fgets(instr, instr_len, instrFile);

        char mode= instr[0];
        instr+= (3 * sizeof(char));
        char *lim;
        long addr= strtoul(instr, &lim, 16);

        if(mode == 'S' || mode == 'L'){
            addr_t loadingAddr= parseAddr(addr,s, b);
            //if(loadingAddr->setIndex == 1){
            int i= 0;
            char c= instr[i];
            printf("%c ", mode);
            while(instr[i] != '\n'){
                printf("%c",instr[i]);
                i++;
            }printf("  ");
            //}
            load(loadingAddr, C, stats, mode);
        }
    }

    for(int i=0; i<C->S; i++){
        set_t S= C->data[i];
        for(int j=0; j<C->E; j++){
            line_t L= S->lines[j];
            if(L->d){ stats->dirty_bytes+= C->B; /*printf("L->tag: %x\n",L->tag);*/ }
            free(L);
        }
    }

    printf("hits:%ld misses:%ld evictions:%ld dirty_bytes_in_cache:%ld dirty_bytes_evicted:%ld\n", stats->hits, stats->misses, stats->evictions, stats->dirty_bytes, stats->dirty_evictions);

    return 0;
}
