#define _GNU_SOURCE

#include <err.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include "methods.h"
#include "queue.h"

#include <pthread.h>
#include <sys/epoll.h>
#include <search.h>

#include <libgen.h>
#include <errno.h>
#include <regex.h>
#include <sys/stat.h>

#define OPTIONS              "t:l:"
#define DEFAULT_THREAD_COUNT 4

static FILE *logfile;
#define LOG(...) fprintf(logfile, __VA_ARGS__);

struct epoll_event events[MAX_EVENTS];

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ping = PTHREAD_COND_INITIALIZER;
pthread_mutex_t cond_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t logLock = PTHREAD_MUTEX_INITIALIZER;

static volatile int running = 1;

// Converts a string to an 16 bits unsigned integer.
// Returns 0 if the string is malformed or out of the range.
static size_t strtouint16(char number[]) {
    char *last;
    long num = strtol(number, &last, 10);
    if (num <= 0 || num > UINT16_MAX || *last != '\0') {
        return 0;
    }
    return num;
}

// Creates a socket for listening for connections.
// Closes the program and prints an error message on error.
static int create_listen_socket(uint16_t port) {
    struct sockaddr_in addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        err(EXIT_FAILURE, "socket error");
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htons(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *) &addr, sizeof addr) < 0) {
        err(EXIT_FAILURE, "bind error");
    }
    if (listen(listenfd, 128) < 0) {
        err(EXIT_FAILURE, "listen error");
    }
    return listenfd;
}

struct request checkRequest(char *request) {
    struct request req = { { 0 }, { 0 }, 0 };
    char *method, *path, *version, *sp;

    method = strtok_r(request, " ", &sp);
    path = strtok_r(NULL, " ", &sp);
    version = strtok_r(NULL, " ", &sp);

    if (method == NULL || path == NULL || version == NULL) {
        req.error = -1;
    } else if (path[0] == '/' && strcmp(version, "HTTP/1.1") == 0) {
        memcpy(req.request_type, method, strlen(method));
        memcpy(req.path, path, strlen(path));
    } else {
        req.error = -1;
    }

    return req;
}

struct headerData checkHeaders(char *headers) {
    struct headerData hd;
    char *sp;

    hd.error = 0;
    hd.content_length = 0;
    hd.requestid = 0;

    char *header = strtok_r(headers, "\r\n", &sp);
    while (header != NULL) {
        char *key, *value, *theRest;
        key = strtok_r(header, " ", &theRest);
        if (key == NULL)
            hd.error = -1;
        value = strtok_r(NULL, " ", &theRest);
        if (value == NULL)
            hd.error = -1;

        if (key[strlen(key) - 1] != ':' || strlen(theRest) > 0 || key[strlen(key) - 2] == ':') {
            hd.error = -1;
        }
        if (strcmp(key, "Content-Length:") == 0) {
            hd.content_length = atoi(value);
        } else if (strcmp(key, "Request-Id:") == 0) {
            hd.requestid = atoi(value);
        }
        header = strtok_r(NULL, "\r\n", &sp);
    }
    return hd;
}

struct requestNode *handle_connection(struct requestNode *node) {
    int status;
    int error = 0;

    while (error == 0) {
        if (strcmp(node->req.request_type, "GET") == 0
            || strcmp(node->req.request_type, "get") == 0) {
            // status = handleGet(node->connfd, node->req.path);
            handleGet(node);
            status = node->status;
        } else if (strcmp(node->req.request_type, "PUT") == 0
                   || strcmp(node->req.request_type, "put") == 0) {
            handlePut(node);
            status = node->status;
        } else if (strcmp(node->req.request_type, "APPEND") == 0
                   || strcmp(node->req.request_type, "append") == 0) {
            handleAppend(node);
            status = node->status;
        } else {
            send_status(node->connfd, INTERNAL_SERVER_ERROR, 0);
            status = 500;
        }
        if (!node->reentrant) {
            pthread_mutex_lock(&logLock);
            LOG("%s,%s,%d,%d\n", node->req.request_type, node->req.path, status,
                node->headerData.requestid);
            fflush(logfile);
            pthread_mutex_unlock(&logLock);

            node->state = DONE;
            return node;
        } else {
            node->state = PROCESSING;
            return node;
        }
        break;
    }
    return node;
}

struct requestNode *handleHeaders(struct requestNode *node) {
    char buf[BUF_SIZE];
    ssize_t bytes_read;
    struct headerData hd;
    char tempReq[8192] = "";
    char *restOfRequest = NULL;
    int total_read = 0;
    total_read = strlen(node->buf);

    memcpy(tempReq, node->buf, node->buf_size);

    restOfRequest = strstr(tempReq, "\r\n\r\n");
    if (restOfRequest == NULL) {
        do {
            bytes_read = read(node->connfd, buf, BUF_SIZE);
            if (bytes_read < 0) {
                if (errno == EAGAIN) {
                    return node;
                } else {
                    node->state = DONE;
                    return node;
                }
            }

            if (strcmp(tempReq, "") == 0) {
                memcpy(tempReq, buf, bytes_read);
            } else {
                strcpy(tempReq + total_read, buf);
            }
            total_read += bytes_read;

            restOfRequest = strstr(tempReq, "\r\n\r\n");
            if (restOfRequest != NULL)
                break;

            memset(buf, 0, BUF_SIZE);
        } while (restOfRequest == NULL && bytes_read > 0);
    }

    tempReq[total_read] = '\0';
    if (restOfRequest == NULL || strlen(restOfRequest) == 0) {
        restOfRequest = "";
    } else {
        *restOfRequest = '\0';
        restOfRequest += 4;
    }

    hd = checkHeaders(tempReq);

    node->headerData = hd;
    node->state = HEADERS;
    memset(node->buf, 0, BUF_SIZE);
    memcpy(node->buf, restOfRequest, strlen(restOfRequest));
    node->buf_size = strlen(restOfRequest);
    node->total_read = node->total_read - (total_read + 5);

    return node;
}

struct requestNode *processRequest(struct requestNode *node) {
    char buf[BUF_SIZE];
    ssize_t bytes_read;
    struct request req;
    char tempReq[8192] = "";
    char *restOfRequest = NULL;
    int total_read = 0;

    do {
        bytes_read = read(node->connfd, buf, BUF_SIZE);
        if (bytes_read <= 0) {
            if (errno == EAGAIN) {
                return node;
            } else {
                node->state = DONE;
                return node;
            }
        }

        if (strcmp(tempReq, "") == 0) {
            memcpy(tempReq, buf, bytes_read);
        } else {
            strcpy(tempReq + total_read, buf);
        }
        total_read += bytes_read;

        restOfRequest = strstr(tempReq, "\r\n");
        if (restOfRequest != NULL)
            break;

        memset(buf, 0, BUF_SIZE);

    } while (restOfRequest == NULL && bytes_read > 0);

    tempReq[total_read] = '\0';
    if (strlen(restOfRequest) == 0) {
        restOfRequest = "";
    } else {
        *restOfRequest = '\0';
        restOfRequest += 2;
    }

    req = checkRequest(tempReq);

    node->req = req;
    memset(node->buf, 0, BUF_SIZE);
    memcpy(node->buf, restOfRequest, strlen(restOfRequest));

    node->reentrant = 0;
    node->status = -1;

    node->buf_size = strlen(restOfRequest);
    node->state = REQUEST_LINE;
    node->total_read = total_read - strlen(tempReq);

    return node;
}

static void sigterm_handler(int sig) {
    if (sig == SIGTERM) {
        warnx("received SIGTERM");
        fclose(logfile);
        exit(EXIT_SUCCESS);
    }
}

static void sigint_handler(int sig) {
    if (sig == SIGINT) {
        running = 0;
    }
}

static void usage(char *exec) {
    fprintf(stderr, "usage: %s [-t threads] [-l logfile] <port>\n", exec);
}

void cleanup_handler(void *plock) {
    pthread_mutex_unlock(plock);
}

void *handleThread(void *args) {
    pthread_cleanup_push(cleanup_handler, &cond_mutex);
    (void) args;
    struct requestNode *request;
    pthread_setcanceltype(PTHREAD_CANCEL_ENABLE, NULL);

    while (running) {
        pthread_mutex_lock(&cond_mutex);
        if (running) {
            pthread_cond_wait(&ping, &cond_mutex);
        } else
            break;

        if (!running) {
            return NULL;
        }

        pthread_mutex_lock(&queueLock);
        pthread_mutex_unlock(&cond_mutex);
        if (!isEmpty(workQueue)) {
            request = dequeue(workQueue);
            if (request == NULL) {
                pthread_mutex_unlock(&queueLock);
                continue;
            }
            pthread_mutex_unlock(&queueLock);
            if (request->state == CONNFD) {
                request = processRequest(request);
            }
            if (request->state == REQUEST_LINE) {
                request = handleHeaders(request);
            }
            if (request->state == HEADERS || request->state == PROCESSING) {
                request = handle_connection(request);
            }
            if (request->state == DONE) {
                close(request->connfd);
                free(request);
                continue;
            }
            pthread_mutex_lock(&queueLock);
            enqueueNode(workQueue, request);
            pthread_mutex_unlock(&queueLock);
        } else {
            pthread_mutex_unlock(&queueLock);
        }
    }
    pthread_cleanup_pop(0);
    return NULL;
}

int main(int argc, char *argv[]) {
    int opt = 0;
    int threads = DEFAULT_THREAD_COUNT;
    logfile = stderr;
    struct epoll_event ev = { 0 };
    int connfd, nfds, epollfd;

    workQueue = createQueue();

    while ((opt = getopt(argc, argv, OPTIONS)) != -1) {
        switch (opt) {
        case 't':
            threads = strtol(optarg, NULL, 10);
            if (threads <= 0) {
                errx(EXIT_FAILURE, "bad number of threads");
            }
            break;
        case 'l':
            logfile = fopen(optarg, "w");
            if (!logfile) {
                errx(EXIT_FAILURE, "bad logfile");
            }
            break;
        default: usage(argv[0]); return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        warnx("wrong number of arguments");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    uint16_t port = strtouint16(argv[optind]);
    if (port == 0) {
        errx(EXIT_FAILURE, "bad port number: %s", argv[1]);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigint_handler);

    int listenfd = create_listen_socket(port);
    // LOG("port=%" PRIu16 ", threads=%d\n", port, threads);

    epollfd = epoll_create1(0);
    if (epollfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    ev.events = EPOLLIN;
    ev.data.fd = listenfd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) {
        perror("epoll_ctl: listenfd");
        exit(EXIT_FAILURE);
    }

    // Creating n threads
    pthread_t *threadArray = (pthread_t *) malloc(sizeof(pthread_t) * threads);
    for (int i = 0; i < threads; i++) {
        pthread_create(&threadArray[i], NULL, handleThread, NULL);
    }

    while (running) {
        nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }

        for (int n = 0; n < nfds; ++n) {
            if (events[n].data.fd == listenfd) {
                connfd = accept(listenfd, NULL, NULL);
                if (connfd < 0) {
                    warn("accept error");
                    continue;
                }

                struct requestNode *req = (struct requestNode *) malloc(sizeof(struct requestNode));

                req->connfd = connfd;
                req->state = CONNFD;
                req->tempFile = NULL;
                req->Donereentrant = 0;

                fcntl(connfd, F_SETFL, O_NONBLOCK);

                ev.events = EPOLLIN;
                ev.data.fd = connfd;
                if (epoll_ctl(epollfd, EPOLL_CTL_ADD, connfd, &ev) == -1) {
                    perror("epoll_ctl: conn_sock");
                    exit(EXIT_FAILURE);
                }
                pthread_mutex_lock(&queueLock);
                enqueueNode(workQueue, req);
                pthread_mutex_unlock(&queueLock);

            } else {
                pthread_cond_signal(&ping);
            }
        }
    }

    for (int i = 0; i < threads; i++) {
        pthread_cancel(threadArray[i]);
        pthread_join(threadArray[i], NULL);
    }

    free(threadArray);
    deleteQueue(workQueue);

    fclose(logfile);
    return EXIT_SUCCESS;
}
