#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "queue.h"
#include "methods.h"

struct Queue *createQueue() {
    struct Queue *queue = (struct Queue *) malloc(sizeof(struct Queue));
    queue->front = NULL;
    queue->end = NULL;
    queue->size = 0;
    return queue;
}

int isEmpty(struct Queue *queue) {
    struct requestNode *temp = queue->front;
    if (temp == NULL) {
        return 1;
    }
    return 0;
}

void enqueueNode(struct Queue *queue, struct requestNode *node) {
    node->next = NULL;
    if (queue->front == NULL) {
        queue->front = node;
        queue->end = node;
    } else {
        queue->end->next = node;
        queue->end = node;
    }
    queue->size++;
}

struct requestNode *dequeue(struct Queue *queue) {
    if (isEmpty(queue)) {
        return NULL;
    }
    struct requestNode *node = queue->front;
    if (queue->front == NULL) {
        queue->end = NULL;
        queue->size = 0;
        return NULL;
    }
    queue->front = queue->front->next;
    queue->size--;
    return node;
}

void deleteQueue(struct Queue *queue) {
    struct requestNode *current = queue->front;
    struct requestNode *previous = NULL;
    while (current != NULL) {
        previous = current;
        current = current->next;
        free(previous);
    }
    free(queue);
}

int connfdExists(struct Queue *queue, int connfd) {
    struct requestNode *current = queue->front;
    while (current != NULL) {
        if (current->connfd == connfd) {
            return 1;
        }
        current = current->next;
    }
    return 0;
}

int getSize(struct Queue *queue) {
    struct requestNode *current = queue->front;
    int count = 0;
    while (current != NULL) {
        count++;
        current = current->next;
    }
    return count;
}

struct requestNode *getNode(struct Queue *queue, int connfd) {
    struct requestNode *current = queue->front;
    struct requestNode *previous = NULL;

    while (current != NULL) {
        if (current->connfd == connfd) {
            if (previous == NULL) {
                queue->front = current->next;
            } else {
                previous->next = current->next;
            }
            queue->size--;
            return current;
        }
        previous = current;
        current = current->next;
    }
    return NULL;
}
