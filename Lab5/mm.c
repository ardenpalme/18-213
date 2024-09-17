/**
 * @file mm.c
 * @brief A 64-bit struct-based implicit free list memory allocator
 *
 * 15-213: Introduction to Computer Systems
 *
 * TODO: insert your documentation here. :)
 *
 *************************************************************************
 *
 * ADVICE FOR STUDENTS.
 * - Step 0: Please read the writeup!
 * - Step 1: Write your heap checker.
 * - Step 2: Write contracts / debugging assert statements.
 * - Good luck, and have fun!
 *
 *************************************************************************
 *
 * @author Arden Diakhate-Palme <aqd@andrew.cmu.edu>
 */

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memlib.h"
#include "mm.h"

/* Do not change the following! */

#ifdef DRIVER
/* create aliases for driver tests */
#define malloc mm_malloc
#define free mm_free
#define realloc mm_realloc
#define calloc mm_calloc
#define memset mem_memset
#define memcpy mem_memcpy
#endif /* def DRIVER */

/* You can change anything from here onward */

/*
 *****************************************************************************
 * If DEBUG is defined (such as when running mdriver-dbg), these macros      *
 * are enabled. You can use them to print debugging output and to check      *
 * contracts only in debug mode.                                             *
 *                                                                           *
 * Only debugging macros with names beginning "dbg_" are allowed.            *
 * You may not define any other macros having arguments.                     *
 *****************************************************************************
 */
#ifdef DEBUG
/* When DEBUG is defined, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(expr) assert(expr)
#define dbg_assert(expr) assert(expr)
#define dbg_ensures(expr) assert(expr)
#define dbg_printheap(...) print_heap(__VA_ARGS__)
#else
/* When DEBUG is not defined, no code gets generated for these */
/* The sizeof() hack is used to avoid "unused variable" warnings */
#define dbg_printf(...) (sizeof(__VA_ARGS__), -1)
#define dbg_requires(expr) (sizeof(expr), 1)
#define dbg_assert(expr) (sizeof(expr), 1)
#define dbg_ensures(expr) (sizeof(expr), 1)
#define dbg_printheap(...) ((void)sizeof(__VA_ARGS__))
#endif

/* Basic constants */

typedef uint64_t word_t;

/** @brief Word and header size (bytes) */
static const size_t wsize = sizeof(word_t);

/** @brief Double word size (bytes) */
static const size_t dsize = 2 * wsize;

/** @brief Minimum block size of an alloc'd block (bytes) */
static const size_t min_block_size = 1 * dsize;

/**
 * Ammount to extend the heap by minimally
 * (Must be divisible by dsize)
 */
static const size_t chunksize = (1 << 12);

/**
 * get alloc bit from header / footer
 */
static const word_t alloc_mask = 0x1;
static const word_t prev_alloc_mask = 0x1<<1;
static const word_t prev_min_mask= 0x1<<2;
static const word_t size_mask = ~(word_t)0xF;

union min_list_header {
    void *next;
    char payload[0];
};

typedef struct min_block {
    word_t header;
    union min_list_header val;
} min_block_t;

/** @brief Represents the header and payload of one block in the heap */
union list_header {
    struct {
        void *prev;
        void *next;
    } list;
    char payload[0];
};

typedef struct block {
    word_t header;
    union list_header val;
} block_t;

/* Global variables */

#define NUM_CLASSES 8
/** @brief Pointer to first block in the heap */
static block_t *heap_start = NULL;
/** @brief Pointer to last block in the heap */
static block_t *heap_end= NULL;
block_t *segList[NUM_CLASSES];
min_block_t *minList[6];

/*
 *****************************************************************************
 * The functions below are short wrapper functions to perform                *
 * bit manipulation, pointer arithmetic, and other helper operations.        *
 *                                                                           *
 * We've given you the function header comments for the functions below      *
 * to help you understand how this baseline code works.                      *
 *                                                                           *
 * Note that these function header comments are short since the functions    *
 * they are describing are short as well; you will need to provide           *
 * adequate details for the functions that you write yourself!               *
 *****************************************************************************
 */

/*
 * ---------------------------------------------------------------------------
 *                        BEGIN SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Returns the maximum of two integers.
 * @param[in] x
 * @param[in] y
 * @return `x` if `x > y`, and `y` otherwise.
 */
static size_t max(size_t x, size_t y) {
    return (x > y) ? x : y;
}

/**
 * @brief Rounds `size` up to next multiple of n
 * @param[in] size
 * @param[in] n
 * @return The size after rounding up
 */
static size_t round_up(size_t size, size_t n) {
    return n * ((size + (n - 1)) / n);
}

/**
 * @brief Packs the `size` and `alloc` of a block into a word suitable for
 *        use as a packed value.
 *
 * Packed values are used for both headers and footers.
 *
 * The allocation status is packed into the lowest bit of the word.
 *
 * @param[in] size The size of the block being represented
 * @param[in] alloc True if the block is allocated
 * @return The packed value
 */
static word_t pack(size_t size, bool prev_min, bool prev_alloc, bool alloc) {
    word_t word = size;
    if (prev_min) {
        word |= prev_min_mask;
    }
    if(prev_alloc){
        word |= prev_alloc_mask;
    }
    if (alloc) {
        word |= alloc_mask;
    }
    return word;
}



/**
 * @brief Extracts the size represented in a packed word.
 *
 * This function simply clears the lowest 4 bits of the word, as the heap
 * is 16-byte aligned.
 *
 * @param[in] word
 * @return The size of the block represented by the word
 */
static size_t extract_size(word_t word) {
    return (word & size_mask);
}

/**
 * @brief Extracts the size of a block from its header.
 * @param[in] block
 * @return The size of the block
 */
static size_t get_size(block_t *block) {
    return extract_size(block->header);
}

/**
 * @brief Given a payload pointer, returns a pointer to the corresponding
 *        block.
 * @param[in] bp A pointer to a block's payload
 * @return The corresponding block
 */
static block_t *payload_to_header(void *bp) {
    return (block_t *)((char *)bp - offsetof(block_t, val.payload));
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        payload.
 * @param[in] block
 * @return A pointer to the block's payload
 * @pre The block must be a valid block, not a boundary tag.
 */
static void *header_to_payload(block_t *block) {
    dbg_requires(get_size(block) != 0);
    return (void *)(block->val.payload);
}

/**
 * @brief Given a block pointer, returns a pointer to the corresponding
 *        footer.
 * @param[in] block
 * @return A pointer to the block's footer
 * @pre The block must be a valid block, not a boundary tag.
 */
static word_t *header_to_footer(block_t *block) {
    if(get_size(block) == 0) printf("called header_to_footer on epilogue\n");
    //dbg_requires(get_size(block) != 0 && "Called header_to_footer on the epilogue block");
    return (word_t *)(block->val.payload + get_size(block) - dsize);
}

/**
 * @brief Given a block footer, returns a pointer to the corresponding
 *        header.
 * @param[in] footer A pointer to the block's footer
 * @return A pointer to the start of the block
 * @pre The footer must be the footer of a valid block, not a boundary tag.
 */
static block_t *footer_to_header(word_t *footer) {
    size_t size = extract_size(*footer);
    dbg_assert(size != 0 && "Called footer_to_header on the prologue block");
    return (block_t *)((char *)footer + wsize - size);
}

/**
 * @brief Returns the payload size of a given block.
 *
 * The payload size is equal to the entire block size minus the sizes of the
 * block's header and footer.
 *
 * @param[in] block
 * @return The size of the block's payload
 */
static size_t get_payload_size(block_t *block) {
    size_t asize = get_size(block);
    return asize - wsize;
}

/**
 * @brief Returns the allocation status of a given header value.
 *
 * This is based on the lowest bit of the header value.
 *
 * @param[in] word
 * @return The allocation status correpsonding to the word
 */
static bool extract_alloc(word_t word) {
    return (bool)(word & alloc_mask);
}

static bool extract_prev_alloc(word_t word) {
    return (bool)(word & prev_alloc_mask);
}

static bool extract_prev_min(word_t word) {
    return (bool)(word & prev_min_mask);
}

/**
 * @brief Returns the allocation status of a block, based on its header.
 * @param[in] block
 * @return The allocation status of the block
 */
static bool get_alloc(block_t *block) {
    return extract_alloc(block->header);
}

static bool get_prev_alloc(block_t *block) {
    return extract_prev_alloc(block->header);
}

static bool get_prev_min(block_t *block) {
    return extract_prev_min(block->header);
}

/**
 * @brief Writes an epilogue header at the given address.
 *
 * The epilogue header has size 0, and is marked as allocated.
 *
 * @param[out] block The location to write the epilogue header
 */
static void write_epilogue(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires((char *)block == mem_heap_hi() - 7);
    block->header = pack(0, false, false, true);
}

/**
 * @brief Writes a block starting at the given address.
 *
 * This function writes both a header and footer, where the location of the
 * footer is computed in relation to the header.
 *
 * Payloads of size 0 are disallowed, inputed blocks must not be NULL
 *
 * @param[out] block The location to begin writing the block header
 * @param[in] size The size of the new block
 * @param[in] alloc The allocation status of the new block
 */
static void write_block(block_t *block, size_t size, bool prev_min_status, bool prev_alloc_status, bool alloc) {
    dbg_requires(block != NULL);
    dbg_requires(size > 0);

    //bool prev_alloc_status= get_prev_alloc(block);
    //bool prev_min_status= get_prev_min(block);
    block->header= pack(size, prev_min_status, prev_alloc_status, alloc);

    if(!alloc &&  size != min_block_size){
        word_t *footerp = header_to_footer(block);
        *footerp = pack(size, prev_min_status, prev_alloc_status, alloc);
    }
}

/**
 * @brief Finds the next consecutive block on the heap.
 *
 * This function accesses the next block in the "implicit list" of the heap
 * by adding the size of the block.
 *
 * @param[in] block A block in the heap
 * @return The next consecutive block on the heap
 * @pre The block is not the epilogue
 */
static block_t *find_next(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(get_size(block) != 0 &&
                 "Called find_next on the last block in the heap");
    return (block_t *)((char *)block + get_size(block));
}

/**
 * @brief Finds the footer of the previous block on the heap.
 * @param[in] block A block in the heap
 * @return The location of the previous block's footer
 */
static word_t *find_prev_footer(block_t *block) {
    // Compute previous footer position as one word before the header
    return &(block->header) - 1;
}

/**
 * @brief Finds the previous consecutive block on the heap.
 *
 * This is the previous block in the "implicit list" of the heap.
 *
 * If the function is called on the first block in the heap, NULL will be
 * returned, since the first block in the heap has no previous block!
 *
 * The position of the previous block is found by reading the previous
 * block's footer to determine its size, then calculating the start of the
 * previous block based on its size.
 *
 * @param[in] block A block in the heap
 * @return The previous consecutive block in the heap.
 */
static block_t *find_prev(block_t *block) {
    dbg_requires(block != NULL);
    word_t *footerp = find_prev_footer(block);

    // Return NULL if called on first block in the heap
    if (extract_size(*footerp) == 0) {
        return NULL;
    }

    return footer_to_header(footerp);
}

/*
 * ---------------------------------------------------------------------------
 *                        END SHORT HELPER FUNCTIONS
 * ---------------------------------------------------------------------------
 */


void pop_min_block(min_block_t *block){
    dbg_requires((block->header & size_mask) == min_block_size);

    word_t block_addr= (word_t)block;
    size_t index= block_addr % (word_t)6;
    min_block_t *root= minList[index];
    dbg_assert(root != NULL);

    //one ele
    if((word_t)block == (word_t)root && block->val.next == NULL){
        minList[index]= NULL;
        return;
    }

    //ele at root
    if((word_t)block == (word_t)root && block->val.next != NULL){
        minList[index]= block->val.next;
        block->val.next= NULL;
        return;
    }

    //general case
    //linearly search hash table
    min_block_t *tmp= root;
    min_block_t *prev= root;
    while((word_t)tmp != (word_t)block){
        prev= tmp;
        tmp= (min_block_t*)tmp->val.next;
    }
    dbg_assert((word_t)tmp == (word_t)block);

    //ele at end
    if((word_t)block != (word_t)root && block->val.next == NULL){
        prev->val.next= NULL;
    }

    prev->val.next= tmp->val.next;
    tmp->val.next= NULL;
}

void push_min_block(min_block_t *block){
    dbg_requires((block->header & size_mask) == min_block_size);

    word_t block_addr= (word_t)block;
    size_t index= block_addr % (word_t)6;
    min_block_t *root= minList[index];

    //no ele
    if(root == NULL){
        block->val.next= NULL;
        minList[index]= block;
        return;
    }

    //general case
    block->val.next= (void*)root;
    minList[index]= block;
}

//gets the index into the segList based on size for appropriate size class
size_t get_index(size_t asize) {
    size_t index = 0;
    if (asize < (1 << 6))
        index = 0;
    if (asize >= (1 << 6) && asize < (1 << 7))
        index = 1;
    if (asize >= (1 << 7) && asize < (1 << 8))
        index = 2;
    if (asize >= (1 << 8) && asize < (1 << 9))
        index = 3;
    if (asize >= (1 << 9) && asize < (1 << 10))
        index = 4;
    if (asize >= (1 << 10) && asize < (1 << 11))
        index = 5;
    if (asize >= (1 << 11) && asize < (1 << 12))
        index = 6;
    if (asize >= (1 << 12))
        index = 7;
    return index;
}

// removes the inputted block from the appropriate "root" (i.e index into the
// segList) Cases over the previous states of the free list prior to LIFO removal
void pop(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    size_t size=get_size(block);
    if(size == min_block_size){
        pop_min_block((min_block_t*)block);
        return;
    }

    size_t i = get_index(size);
    block_t *root = segList[i];
    dbg_requires(root != NULL);

    block_t *prev_block = (block_t *)block->val.list.prev;
    block_t *next_block = (block_t *)block->val.list.next;

    // one ele
    if ((word_t)block == (word_t)root && prev_block == NULL &&
        next_block == NULL) {
        segList[i] = NULL;
        return;
    }

    // two ele  1
    if ((word_t)block == (word_t)root && next_block->val.list.next == NULL &&
        prev_block == NULL) {
        next_block->val.list.prev = NULL;
        block->val.list.next = NULL;
        segList[i] = next_block;
        return;
    }

    // two ele 2
    if ((word_t)block != (word_t)root && prev_block->val.list.prev == NULL &&
        next_block == NULL) {
        prev_block->val.list.next = NULL;
        block->val.list.prev = NULL;
        // no need to reset root
        return;
    }

    // 3+ ele 1
    if ((word_t)block == (word_t)root && next_block->val.list.next != NULL &&
        prev_block == NULL) {
        next_block->val.list.prev = NULL;
        block->val.list.next = NULL;
        segList[i] = next_block;
        return;
    }

    // 3+ ele 2 general case
    if ((word_t)block != (word_t)root && next_block != NULL &&
        prev_block != NULL) {
        prev_block->val.list.next = (void *)next_block;
        next_block->val.list.prev = (void *)prev_block;
        block->val.list.next = NULL;
        block->val.list.prev = NULL;
        // no need to reset root
        return;
    }

    // 3+ ele 3 end
    if ((word_t)block != (word_t)root && prev_block->val.list.prev != NULL &&
        next_block == NULL) {
        prev_block->val.list.next = NULL;
        block->val.list.prev = NULL;
        // no need to reset root
        return;
    }

    dbg_assert(0);
    return;
}

// adds a block to the appropriate segList (based on the calculated size of the
// block) block is inserted at the head (LIFO structure maintained)
void push(block_t *block) {
    dbg_requires(block != NULL);
    dbg_requires(!get_alloc(block));

    size_t size=get_size(block);
    if(size == min_block_size){
        push_min_block((min_block_t*)block);
        return;
    }

    size_t i = get_index(size);
    block_t *root = segList[i];

    if (root == NULL) {
        block->val.list.prev = NULL;
        block->val.list.next = NULL;
        segList[i] = block;
        return;
    }
    dbg_assert(root->val.list.prev == NULL);

    root->val.list.prev = (void *)block;
    block->val.list.next = (void *)root;
    block->val.list.prev = NULL;

    segList[i] = block;
}

// adds and coalesces an inputted block with other blocks by popping and pushing */
// to the LIFO structure of the appropriate segList
static block_t *coalesce_block(block_t *block) {
    dbg_requires(!get_alloc(block));
    dbg_requires(block != NULL);
    dbg_requires(block->header != (word_t)0x1);

    block_t *next_block = find_next(block);
    size_t next_size = get_size(next_block);
    size_t size = get_size(block);
    bool c= get_prev_min(block);

    word_t *tmp= (word_t*)block - 1;
    if (*tmp == (word_t)0x1 && (word_t)tmp == (word_t)heap_start && (word_t)(next_block->header) != (word_t)0x1) {
        if (!get_alloc(next_block)) {
            pop(next_block);
            write_block(block, size + next_size, get_prev_min(block), get_prev_alloc(block),false);
            push(block);
            return block;
        }
        push(block);
        return block;
    }

    dbg_assert(next_block->header != (word_t)0x1);

    bool nnext_valid= true;
    block_t *next_next= find_next(next_block);
    if((word_t)next_next->header == (word_t)0x1)
        nnext_valid= false;

    // case 1: Allocated | just_freed | Allocated
    if (get_prev_alloc(block) && get_alloc(next_block)) {
        push(block);
        dbg_assert(mm_checkheap(__LINE__));
        return block;
    }

    // case 2: Allocated | just_freed | Free
    if (get_prev_alloc(block) && !get_alloc(next_block)) {

        pop(next_block);
        write_block(block, size + next_size, get_prev_min(block), get_prev_alloc(block), false);
        push(block);

        if(nnext_valid && get_prev_min(next_next))
            next_next->header &= ~prev_min_mask;

        if((word_t)next_block == (word_t)heap_end)
            heap_end= block;

        dbg_assert(mm_checkheap(__LINE__));
        return block;
    }

    // case 3: Free | just_freed | Allocated
    if (!get_prev_alloc(block) && get_alloc(next_block)) {
        block_t *prev_block;
        if(c){
            word_t *tmp= (word_t*)block - 2;
            prev_block= (block_t*)tmp;
        }else{
            prev_block= find_prev(block);
        }
        size_t prev_size= get_size(prev_block);

        if(get_prev_min(next_block) ){
            next_block->header &= ~prev_min_mask;
        }

        pop(prev_block);
        write_block(prev_block, prev_size + size, get_prev_min(prev_block), get_prev_alloc(prev_block), false);
        push(prev_block);


        dbg_assert(mm_checkheap(__LINE__));
        return prev_block;
    }

    // case 4: Free | just_freed | Free
    if (!get_prev_alloc(block) && !get_alloc(next_block)) {
        block_t *prev_block;
        if(c){
            word_t *tmp= (word_t*)block - 2;
            prev_block= (block_t*)tmp;
        }else{
            prev_block= find_prev(block);
        }
        size_t prev_size= get_size(prev_block);

        pop(prev_block);
        pop(next_block);
        write_block(prev_block, prev_size + size + next_size, get_prev_min(prev_block), get_prev_alloc(prev_block), false);
        push(prev_block);

        if(nnext_valid && get_prev_min(next_next))
            next_next->header &= ~prev_min_mask;

        if((word_t)next_block == (word_t)heap_end || (word_t)block == (word_t)heap_end)
            heap_end= prev_block;

        dbg_assert(mm_checkheap(__LINE__));
        return prev_block;
    }

    dbg_assert(0);
    return block;
}

// extends the heap a certain given number of bytes, coalescing by pusing and
// popping any previous block which may have been free
static block_t *extend_heap(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));
    void *bp;

    size = round_up(size, dsize);
    if ((bp = mem_sbrk(size)) == (void *)-1) {
        return NULL;
    }

    block_t *block = (block_t *)payload_to_header(bp);

    //no ele
    word_t *tmp= (word_t*)(block)-1;
    word_t *tmp1= (word_t*)(block);
    if(*tmp == (word_t)0x1 && *tmp1 == (word_t)0x1){
        //block->header |= prev_alloc_mask;
        //block->header &= ~prev_min_mask;
        write_block(block, size, false, true, false);
        dbg_assert(get_prev_alloc(block) && !get_alloc(block));

        push(block);
        heap_end= block;

        block_t *block_next = find_next(block);
        write_epilogue(block_next);
        dbg_ensures(mm_checkheap(__LINE__));
        return block;
    }

    //any ele
    if(get_alloc(heap_end)){
        //block->header |= prev_alloc_mask;
        if(get_size(heap_end) == min_block_size){
            //block->header |= prev_min_mask;
            write_block(block, size, true, true, false);
        }else{
            //block->header &= ~prev_min_mask;
            write_block(block, size, false, true, false);
        }
        dbg_assert(get_prev_alloc(block) && !get_alloc(block));

        push(block);
        heap_end= block;

        block_t *block_next = find_next(block);
        write_epilogue(block_next);
        dbg_ensures(mm_checkheap(__LINE__));
        return block;
    }else{
        pop(heap_end);
        write_block(heap_end, size + get_size(heap_end), get_prev_min(heap_end), get_prev_alloc(heap_end), false);
        push(heap_end);

        block_t *block_next = find_next(heap_end);
        write_epilogue(block_next);
        dbg_ensures(mm_checkheap(__LINE__));
        return heap_end;
    }
    dbg_assert(0);
}

// scans the appropriate segList's free list with a first fit algorithm, to find
// a block of the requested size or greater
static block_t *find_fit(size_t asize) {
    if(asize == min_block_size){
        for(int i=0; i<6; i++){
            min_block_t *tmp1= minList[i];
            if(tmp1 != NULL) return (block_t*)tmp1;
        }
    }

    size_t start_index = get_index(asize);
    for (size_t i = start_index; i < NUM_CLASSES; i++) {
        block_t *root = segList[i];
        if (root == NULL)
            continue;
        block_t *tmp = root;
        do {
            if (asize <= get_size(tmp)) {
                return tmp;
            }
            tmp = (block_t *)tmp->val.list.next;

        } while ((word_t)tmp != 0x0);
    }
    return NULL;
}

// checks the heap for correctness, for debugging purposes, once using the
// implicit list to travel the heap, once scanning all segLists and checking if
// the free-lists are correctly implemented
bool mm_checkheap(int line) {
    // check prologue
    word_t *tmp1 = (word_t *)heap_start;
    if ((word_t)(*(tmp1 - 1)) != (word_t)0x1) {
        printf("prologue missing\n");
        return false;
    }

    block_t *block;

    word_t heap_hi = (word_t)mem_heap_hi();
    word_t heap_lo = (word_t)mem_heap_lo();

    int free_blocks = 0;
    int free_list_blocks = 0;


    block_t *prev = heap_start;
    printf("-----%d-----\n",line);
    for (block = heap_start; get_size(block) > 0; block = find_next(block)) {
        printf("%p => ", block);

        if ((word_t)block % (word_t)wsize != 0) {
            printf("unaligned addreses %zu (mm.c:%d)\n",
                   (word_t)block % (word_t)dsize, line);
            return false;
        }

        if ((word_t)block <= heap_lo || (word_t)block >= heap_hi) {
            printf("block addreses below lowest addr in heap (mm.c:%d)\n",
                   line);
            return false;
        }

        //only free (non min_) blocks have footers
        if(!get_alloc(block) && get_size(block) != min_block_size){
            word_t *footer = header_to_footer(block);

            if (extract_size(*footer) != get_size(block)) {
                printf("footer header size mismatch\n");
                return false;
            }
            if (extract_alloc(*footer) != get_alloc(block)) {
                printf("footer header alloc mismatch\n");
                return false;
            }
            if (extract_prev_alloc(*footer) != get_prev_alloc(block)) {
                printf("footer header prev_alloc mismatch\n");
                return false;
            }
            free_blocks++;
        }

        if(!get_alloc(block) && get_size(block) == min_block_size){
            free_blocks++;
        }

        if ((word_t)heap_start != (word_t)block) {
            if (!get_alloc(block) && !get_alloc(prev)) {
                printf("two coalescing free blocks\n");
                return false;
            }
            if(get_prev_alloc(block) != get_alloc(prev)){
                printf("b field of headers don't match\n");
                return false;
            }
            if(get_prev_min(block) && (get_size(prev) != dsize)){
                printf("c field of headers don't match\n");
                return false;
            }
        }
        if((word_t)block == (word_t)heap_start && ((word_t)block->header & (word_t)0x2) != (word_t)0x2
                && (word_t)((find_next(block))->header) != (word_t)0x1){
            printf("heap_start has incorrect prev allocation status\n");
            return false;
        }
        prev = block;
    }
    printf("\n");

    block_t *epilogue= (block_t*)((char*)heap_end + get_size(heap_end));
    if((word_t)epilogue->header != (word_t)0x1){
        printf("heap_end is not maintained\n");
        return false;
    }

    // check epilogue
    if (get_size(block) != 0 || !get_alloc(block)) {
        printf("epilogue check failed %p(mm.c:%d)\n", block, line);
        return false;
    }

    //check minList
    for(int i=0; i<6; i++){
        min_block_t *root= minList[i];
        if(root == NULL) continue;

        min_block_t *tmp3= root;
        min_block_t *prev= root;
        //printf("m[%d] ",i);
        while(tmp3 != NULL){
            //printf("%p .. ",tmp3);
            min_block_t *next_block= tmp3->val.next;

            free_list_blocks++;
            tmp3= next_block;
            prev= tmp3;
        }
        //printf("\n");
    }


    //check segList
    for (int i = 0; i < NUM_CLASSES; i++) {
        block_t *root = segList[i];
        if (root == NULL) {
            //printf("root%d null @ %d\n", i, line);
            continue;
        }

        if (free_blocks == 0 && root != NULL) {
            printf("zero free blocks root @%p\n",root);
            return false;
        }
        //printf("[%d] ", i);
        block_t *tmp = root;
        do {
            //printf("%p --> ", tmp);
            block_t *prev_block = (block_t *)tmp->val.list.prev;
            block_t *next_block = (block_t *)tmp->val.list.next;

            if ((word_t)tmp >= heap_hi || (word_t)tmp <= heap_lo) {
                printf("block next/prev ptr are out of bouds\n");
                return false;
            }

            if (get_alloc(tmp)) {
                printf("an elem of the free list is allocated(mm.c:%d)\n",
                       line);
                return false;
            }

            if (tmp != root && next_block != NULL) {
                if (next_block->val.list.prev != prev_block->val.list.next) {
                    printf("error here\n");
                    return false;
                }
            }

            free_list_blocks++;
            tmp = (block_t *)tmp->val.list.next;

        } while ((word_t)tmp != 0x0);
        //printf("\n");
    }

    if (free_list_blocks != free_blocks) {
        printf("number of free blocks mismatch %d =/= %d (mm.c:%d)\n",
               free_list_blocks, free_blocks, line);
        return false;
    }

    return true;
}

// initializes a heap
bool mm_init(void) {
    // Create the initial empty heap
    word_t *start = (word_t *)(mem_sbrk(2 * wsize));

    for(int i=0; i<NUM_CLASSES; i++){
        segList[i]= NULL;
    }

    for(int i=0; i<6; i++){
        minList[i]= NULL;
    }

    if (start == (void *)-1) {
        return false;
    }

    start[0] = pack(0, false, false, true); // Heap prologue (block footer)
    start[1] = pack(0, false, false, true); // Heap epilogue (block header)

    // Heap starts with first "block header", currently the epilogue
    heap_start = (block_t *)&(start[1]);
    heap_end= heap_start;

    // Extend the empty heap with a free block of chunksize bytes
    // epilogue written here
    if (extend_heap(chunksize) == NULL) {
        return false;
    }

    return true;
}

// given a free block, splits the block if the block has
// initially more space than is necessary for the allocated space
static void split_block(block_t *block, size_t asize) {
    dbg_requires(mm_checkheap(__LINE__));
    dbg_requires(!get_alloc(block));

    size_t block_size = get_size(block);
    dbg_requires(asize <= block_size);

    bool is_last_block= false;
    block_t *B= find_next(block);
    if(B->header == (word_t)0x1){
        is_last_block= true;
    }

    if ((block_size - asize) >= min_block_size) {
        pop(block);
        write_block(block, asize, get_prev_min(block), get_prev_alloc(block), true);
        block_t *free_block = find_next(block);
//        free_block->header |= prev_alloc_mask;
        //write_block(free_block, block_size - asize, get_prev_min(block), true, false);

        if(asize == dsize){
            //free_block->header |= prev_min_mask;
            write_block(free_block, block_size - asize, true, true, false);
        }else{
            //free_block->header &= ~prev_min_mask;
            write_block(free_block, block_size - asize, false, true, false);
        }

        push(free_block);

        block_t *tmp= find_next(free_block);
        if(tmp->header != (word_t)0x1 && (block_size - asize) == dsize){
            tmp->header |= prev_min_mask;
        }else{
            tmp->header &= ~prev_min_mask;
        }

        if(is_last_block) heap_end= free_block;

        dbg_ensures(mm_checkheap(__LINE__));
        return;
    }
    pop(block);
    write_block(block, block_size, get_prev_min(block), get_prev_alloc(block), true);

    block_t *block_next= find_next(block);
    block_next->header |= prev_alloc_mask;
    if(block_size == min_block_size)
        block_next->header |= prev_min_mask;

    if(is_last_block) heap_end= block;

    dbg_ensures(get_alloc(block));
    dbg_ensures(mm_checkheap(__LINE__));
}

// memory allocation of a certain requested size, tries to find a fit, if none
// found, requests more heap space
void *malloc(size_t size) {
    dbg_requires(mm_checkheap(__LINE__));

    size_t asize;      // Adjusted block size
    size_t extendsize; // Amount to extend heap if no fit is found
    block_t *block;
    void *bp = NULL;

    // Initialize heap if it isn't initialized
    if (heap_start == NULL) {
        mm_init();
    }

    // Ignore spurious request
    if (size == 0) {
        dbg_ensures(mm_checkheap(__LINE__));
        return bp;
    }

    if(size <= wsize){
        asize= min_block_size;
    }else{
        asize = round_up(size + wsize, dsize);
    }

    block = find_fit(asize);

    // If no fit is found, request more memory, and then and place the block
    if (block == NULL) {
        extendsize = max(asize, chunksize);
        block = extend_heap(extendsize);
        if (block == NULL) {
            return bp;
        }
    }

    // The block should be marked as free
    dbg_assert(!get_alloc(block));

    // Try to split the block if too large and allocate space too
    if(get_size(block) == min_block_size){
    }
    split_block(block, asize);

    bp = header_to_payload(block);

    dbg_ensures(mm_checkheap(__LINE__));
    return bp;
}

// frees a block designated by a pointer to the payload of the block
void free(void *bp) {
    dbg_requires(mm_checkheap(__LINE__));

    if (bp == NULL) {
        return;
    }

    block_t *block = payload_to_header(bp);
    size_t size = get_size(block);

    // The block should be marked as allocated
    dbg_assert(get_alloc(block));

    write_block(block, size, get_prev_min(block), get_prev_alloc(block), false);
    block_t *block_next= find_next(block);
    block_next->header &= ~prev_alloc_mask;

    block = coalesce_block(block);

    dbg_ensures(mm_checkheap(__LINE__));
}

// reallocates a block to adjust the size, using malloc and free functions
// previously defined
void *realloc(void *ptr, size_t size) {
    block_t *block = payload_to_header(ptr);
    size_t copysize;
    void *newptr;

    // If size == 0, then free block and return NULL
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    // If ptr is NULL, then equivalent to malloc
    if (ptr == NULL) {
        return malloc(size);
    }

    // Otherwise, proceed with reallocation
    newptr = malloc(size);

    // If malloc fails, the original block is left untouched
    if (newptr == NULL) {
        return NULL;
    }

    // Copy the old data
    copysize = get_payload_size(block); // gets size of old payload
    if (size < copysize) {
        copysize = size;
    }
    memcpy(newptr, ptr, copysize);

    // Free the old block
    free(ptr);

    return newptr;
}

// allocates and initializes a block of request size to zero using malloc
// function, previously defined
void *calloc(size_t elements, size_t size) {
    void *bp;
    size_t asize = elements * size;

    if (elements == 0) {
        return NULL;
    }
    if (asize / elements != size) {
        // Multiplication overflowed
        return NULL;
    }

    bp = malloc(asize);
    if (bp == NULL) {
        return NULL;
    }

    // Initialize all bits to 0
    memset(bp, 0, asize);

    return bp;
}

/*
 *****************************************************************************
 * Do not delete the following super-secret(tm) lines!                       *
 *                                                                           *
 * 53 6f 20 79 6f 75 27 72 65 20 74 72 79 69 6e 67 20 74 6f 20               *
 *                                                                           *
 * 66 69 67 75 72 65 20 6f 75 74 20 77 68 61 74 20 74 68 65 20               *
 * 68 65 78 61 64 65 63 69 6d 61 6c 20 64 69 67 69 74 73 20 64               *
 * 6f 2e 2e 2e 20 68 61 68 61 68 61 21 20 41 53 43 49 49 20 69               *
 *                                                                           *
 * 73 6e 27 74 20 74 68 65 20 72 69 67 68 74 20 65 6e 63 6f 64               *
 * 69 6e 67 21 20 4e 69 63 65 20 74 72 79 2c 20 74 68 6f 75 67               *
 * 68 21 20 2d 44 72 2e 20 45 76 69 6c 0a c5 7c fc 80 6e 57 0a               *
 *                                                                           *
 *****************************************************************************
 */

