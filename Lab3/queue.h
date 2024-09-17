/* Queue header file for a generic queue implementation
 *
 */

#include <stdbool.h>
#include <stdlib.h>

struct queue_elem{
    long tag;
    void *next;
};
typedef struct queue_elem* elem;


struct queue_header {
    elem head;
    elem tail;
};
typedef struct queue_header* queue_t;

queue_t queue_new();

void enq(queue_t q, long lineTag);

elem deq(queue_t q);

void queue_free(queue_t q);

void queue_print(queue_t q);

bool queue_empty(queue_t q);
