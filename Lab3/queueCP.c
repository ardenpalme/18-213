/* Arden Diakhate-Palme id:aqd
 * Implementation for queues to store Tags of lines
 */
#include "queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#define REQURIES(COND) assert(cond)

queue_t queue_new(){
    queue_t new= malloc(sizeof(struct queue_header));
    new->head= NULL;
    new->tail= NULL;
    return new;
}

bool queue_empty(queue_t q){
    if(q->head == NULL && q->tail == NULL) return true;
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
    queue_print(q);
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


void queue_print(queue_t q){
    elem tmp= q->head;
    while(tmp != NULL){
        printf("%x | ", tmp->tag);
        tmp= tmp->next;
    }printf("\n");
}

elem queue_peek(const queue_t *q){
    elem tmp= (*q)->head;
    return tmp;
}
