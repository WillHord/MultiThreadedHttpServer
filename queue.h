#ifndef queue_h
#define queue_h

#include "methods.h"

struct Queue {
    struct requestNode *front, *end;
    int size;
};

struct Queue *createQueue();

int isEmpty(struct Queue *queue);

void enqueueNode(struct Queue *queue, struct requestNode *node);

struct requestNode *dequeue(struct Queue *queue);

int connfdExists(struct Queue *queue, int connfd);

struct requestNode *getNode(struct Queue *queue, int connfd);

int getSize(struct Queue *queue);

void deleteQueue(struct Queue *queue);

#endif
