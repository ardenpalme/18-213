/* Arden Diakhate-Palme id:aqd
 * Implementation for queues to store Tags of lines
 */
#include "queue.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

#define REQURIES(COND) assert(cond)

queue_t queue_new(){
    queue_t new= malloc(sizeof(queue_t));
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
    q->tail->next= e;
    q->tail= e;
}

elem deq(queue_t q){
    //empty
    if(q->head == NULL && q->tail == NULL){
        return NULL;
    }

    elem tmp= q->head;
    //one elem
    if(q->head == q->tail && q->head != NULL){
        q->head= NULL;
        q->tail= NULL;
        printf("is tmp really NULL!?");
        return tmp;
    }

    q->head= q->head->next;
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


