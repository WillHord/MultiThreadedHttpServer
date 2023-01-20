#ifndef methods
#define methods

#include <stdio.h>
#include <pthread.h>

#define BUF_SIZE   4096
#define MAX_EVENTS 10000

struct Queue *workQueue;

pthread_mutex_t queueLock;

struct status OK, CREATED, BAD_REQUEST, FORBIDDEN, NOT_FOUND, INTERNAL_SERVER_ERROR,
    NOT_IMPLEMENTED;

struct status {
    int code;
    const char *body;
};

struct request {
    char request_type[128];
    char path[128];
    int error;
};

struct headerData {
    size_t content_length;
    int requestid;
    int error;
};

enum requestState { CONNFD, REQUEST_LINE, HEADERS, PROCESSING, DONE };

struct requestNode {
    int connfd;
    struct request req;
    struct headerData headerData;
    char buf[BUF_SIZE];
    size_t buf_size;
    struct requestNode *next;
    size_t total_read;
    int state;
    int reentrant;
    int status;
    char *tempFile;
    int Donereentrant;
    size_t written;
};

void send_status(int connfd, struct status stat, int buffer_size);

void handleGet(struct requestNode *node);

void handlePut(struct requestNode *node);

void handleAppend(struct requestNode *node);

#endif
