#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <regex.h>
#include <libgen.h>

#include <pthread.h>
#include <sys/file.h>

#include "methods.h"

pthread_mutex_t tempFileLock = PTHREAD_MUTEX_INITIALIZER;

char *get_tempFile() {
    pthread_mutex_lock(&tempFileLock);
    char *tempFile = malloc(sizeof(char) * 100 + 1);
    int i = 0;
    struct stat filestat;

    while (1) {
        snprintf(tempFile, 100, "tempFile%d", i);
        if (stat(tempFile, &filestat) == -1) {
            break;
        }
        i++;
    }
    int fd = open(tempFile, O_CREAT | O_EXCL, 0644);
    close(fd);
    pthread_mutex_unlock(&tempFileLock);
    return tempFile;
}

void remove_tempFile(char *tempFile) {
    // Check if tempFile exists
    struct stat filestat;
    if (tempFile == NULL) {
        return;
    }
    int err = stat(tempFile, &filestat);
    if (err == 0) {
        remove(tempFile);
    }
    free(tempFile);
}

struct status OK = { 200, "OK" }, CREATED = { 201, "Created" },
              BAD_REQUEST = { 400, "Bad Request" }, FORBIDDEN = { 403, "Forbidden" },
              NOT_FOUND = { 404, "Not Found" },
              INTERNAL_SERVER_ERROR = { 500, "Internal Server Error" },
              NOT_IMPLEMENTED = { 501, "Not Implemented" };

void send_status(int connfd, struct status stat, int buffer_size) {
    if (buffer_size == 0) {
        char buf[BUF_SIZE];
        int n = snprintf(buf, BUF_SIZE, "HTTP/1.1 %d %s\r\nContent-Length: %zd\r\n\r\n%s\n",
            stat.code, stat.body, strlen(stat.body) + 1, stat.body);
        write(connfd, buf, n);
    }
}

void handleGet(struct requestNode *node) {
    char *filename = node->req.path + 1;
    int fd, r, w, tempW;
    int ferror = 0;
    int totalRead = 0;
    int totalWritten = 0;
    struct stat filestat;
    if (stat(filename, &filestat) == -1) {
        node->status = NOT_FOUND.code;
        return;
    }

    int toRead;
    int bytesLeft = filestat.st_size - BUF_SIZE;
    if (bytesLeft < 0) {
        toRead = filestat.st_size;
    } else {
        toRead = BUF_SIZE;
    }

    char file[BUF_SIZE];
    memset(file, 0, BUF_SIZE);

    fd = open(filename, O_RDONLY);
    int lockCheck = flock(fd, LOCK_EX);
    if (lockCheck < 0) {
        if (errno == EWOULDBLOCK) {
            node->reentrant = 1;
            return;
        }
    }

    if (fd == -1) {
        ferror = errno;
    }

    if (ferror == 0) {
        if (!node->reentrant) {
            char buf[BUF_SIZE];
            int n = snprintf(
                buf, BUF_SIZE, "HTTP/1.1 200 OK\r\nContent-Length: %zd\r\n\r\n", filestat.st_size);
            if (write(node->connfd, buf, n) == -1) {
                while (errno == EWOULDBLOCK) {
                    errno = 0;
                    write(node->connfd, buf, n);
                }
                if (errno != 0) {
                    ferror = errno;
                }
            }
        }

        while ((r = read(fd, file, BUF_SIZE)) > 0) {
            if ((w = write(node->connfd, file, r)) == -1) {
                while (errno == EWOULDBLOCK) {
                    errno = 0;
                    w = write(node->connfd, file, r);
                }
                if (errno != 0) {
                    ferror = errno;
                }
            }
            if (w != r) {
                tempW = w;
                while (tempW != r) {
                    if ((w = write(node->connfd, file + tempW, r - tempW)) == -1) {
                        if (errno != EWOULDBLOCK) {
                            break;
                        }
                        while (errno == EWOULDBLOCK) {
                            errno = 0;
                            w = write(node->connfd, file + tempW, r - tempW);
                        }
                        if (errno != 0) {
                            ferror = errno;
                            break;
                        }
                    }
                    if (w != -1)
                        tempW += w;
                }
            }
            totalWritten += w;
            totalRead += r;
            memset(file, 0, BUF_SIZE);
        }
        if (r == -1) {
            ferror = errno;
        }
    }
    close(fd);
    flock(fd, LOCK_UN);

    if (ferror == ENOENT || ferror == 9) {
        send_status(node->connfd, NOT_FOUND, 0);
        node->status = NOT_FOUND.code;
        return;
    } else if (ferror == EACCES || ferror == EISDIR) {
        send_status(node->connfd, FORBIDDEN, 0);
        node->status = FORBIDDEN.code;
        return;
    } else if (ferror != 0) {
        send_status(node->connfd, INTERNAL_SERVER_ERROR, 0);
        node->status = INTERNAL_SERVER_ERROR.code;
        return;
    }
    node->status = OK.code;
    return;
}

int createDir(char *path) {
    char *dirr = strdup(path);
    char *dir = dirname(dirr);
    char currDir[4096] = ".";
    char tempDir[4096];
    char *token, *sp;
    int r = 0;

    token = strtok_r(dir, "/", &sp);
    while (token != NULL) {
        snprintf(tempDir, 4096, "%s/%s", currDir, token);
        memcpy(currDir, tempDir, 4096);
        r = mkdir(currDir, 0777);
        if (r == -1) {
            if (errno != EEXIST) {
                warn("%s", strerror(errno));
                return errno;
            }
        }
        token = strtok_r(NULL, "/", &sp);
    }
    free(dirr);
    memset(currDir, 0, 4096);
    return 0;
}

void handlePut(struct requestNode *node) {
    char *newpath = node->req.path + 1;
    int exists = 1;
    int error = 0;
    int messageSize = 0;
    messageSize = strlen(node->buf);
    struct stat filestat;
    ssize_t bytez = 0;
    int toRead;
    int fd;

    if (node->tempFile == NULL) {
        node->tempFile = get_tempFile();
    }

    if (!node->Donereentrant) {
        if (!node->reentrant) {
            if (stat(newpath, &filestat) == -1) {
                if (errno == ENOENT || errno == ENOTDIR) {
                    int r = createDir(newpath);
                    if (r != 0) {
                        send_status(node->connfd, INTERNAL_SERVER_ERROR, 0);
                        error = INTERNAL_SERVER_ERROR.code;
                        node->status = error;
                        remove_tempFile(node->tempFile);
                        return;
                    }
                    exists = 0;
                } else {
                    send_status(node->connfd, NOT_FOUND, 0);
                    node->status = NOT_FOUND.code;
                    remove_tempFile(node->tempFile);
                    return;
                }
            }
            fd = open(node->tempFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd == -1) {
                error = errno;
            } else {
                if (write(fd, node->buf, messageSize) < 0) {
                    error = errno;
                }
                node->written = messageSize;
                if ((node->headerData.content_length - node->written) > BUF_SIZE) {
                    toRead = BUF_SIZE;
                } else {
                    toRead = node->headerData.content_length - node->written;
                }
                close(fd);
            }
        }

        fd = open(node->tempFile, O_WRONLY | O_APPEND);
        if (fd < 0) {
            if (errno == EWOULDBLOCK) {
                node->reentrant = 1;
                return;
            }
            error = errno;
            if (error == ENOENT) {
                return;
            }
        }
        if (error == 0) {
            do {
                bytez = read(node->connfd, node->buf, BUF_SIZE);
                if (bytez < 0 && errno == EAGAIN) {
                    node->reentrant = 1;
                    break;
                }
                if (bytez < 0) {
                    error = errno;
                    break;
                }
                if (bytez == 0) {
                    node->reentrant = 0;
                    break;
                }
                if (write(fd, node->buf, bytez) < 0) {
                    error = errno;
                    break;
                }
                node->written += bytez;
                memset(node->buf, 0, BUF_SIZE);
            } while (error == 0 && bytez > 0 && node->written < node->headerData.content_length);
            close(fd);
        }
        if (node->written < node->headerData.content_length) {
            node->reentrant = 1;
            return;
        }

        if (node->reentrant) {
            return;
        }
    }

    if (error == 0) {
        pthread_mutex_lock(&tempFileLock);
        fd = open(newpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int eflock = flock(fd, LOCK_EX);
        if (eflock == -1 && errno == EWOULDBLOCK) {
            node->Donereentrant = 1;
            return;
        }

        char fpath[4096];
        snprintf(fpath, 4096, "./%s", newpath);

        if (rename(node->tempFile, fpath) == -1) {
            error = errno;
        }
        flock(fd, LOCK_UN);
        close(fd);
        pthread_mutex_unlock(&tempFileLock);
    }

    if (node->tempFile != NULL) {
        free(node->tempFile);
        node->tempFile = NULL;
    }

    if (error == 0) {
        if (exists) {
            send_status(node->connfd, OK, 0);
            node->status = OK.code;
            return;
        } else {
            send_status(node->connfd, CREATED, 0);
            node->status = CREATED.code;
            return;
        }
    } else {
        send_status(node->connfd, NOT_FOUND, 0);
        node->status = NOT_FOUND.code;
        remove_tempFile(node->tempFile);
        return;
    }
}

void handleAppend(struct requestNode *node) {
    char *newpath = node->req.path + 1;
    struct stat filestat;
    int error = 0, flockError = 0;
    ssize_t bytez = 0;
    int ferr = stat(newpath, &filestat);
    int fd;

    if (!node->Donereentrant) {
        if (node->tempFile == NULL) {
            node->tempFile = get_tempFile();

            fd = open(newpath, O_RDONLY);
            flockError = flock(fd, LOCK_SH);
            if (flockError < 0) {
                if (errno == EWOULDBLOCK) {
                    node->reentrant = 1;
                    return;
                }
                error = errno;
            }

            if (error == 0) {
            }

            char buf[BUF_SIZE];
            if (error == 0) {
                int fd2 = open(node->tempFile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd2 < 0) {
                    error = errno;
                }
                if (error == 0) {
                    do {
                        bytez = read(fd, buf, BUF_SIZE);
                        if (bytez < 0 && errno == EAGAIN) {
                            continue;
                        }
                        if (bytez < 0) {
                            error = errno;
                            break;
                        }
                        if (bytez == 0) {
                            node->reentrant = 0;
                            break;
                        }
                        if (write(fd2, buf, bytez) < 0) {
                            error = errno;
                            break;
                        }
                        memset(buf, 0, BUF_SIZE);
                    } while (error == 0 && bytez > 0);
                    close(fd2);
                }
                close(fd);
                flock(fd, LOCK_UN);
            }
        }

        if (ferr == -1) {
            if (errno == ENOENT || errno == ENOTDIR) {
                send_status(node->connfd, NOT_FOUND, 0);
                error = NOT_FOUND.code;
            } else if (errno == EACCES || errno == EISDIR) {
                send_status(node->connfd, FORBIDDEN, 0);
                error = FORBIDDEN.code;
            } else {
                send_status(node->connfd, INTERNAL_SERVER_ERROR, 0);
                error = INTERNAL_SERVER_ERROR.code;
            }
        }
        if (error != 0) {
            node->status = error;
            remove_tempFile(node->tempFile);
            return;
        }

        int fd = open(node->tempFile, O_WRONLY | O_APPEND);
        flock(fd, LOCK_EX);
        if (fd == -1) {
            error = errno;
        }
        if (write(fd, node->buf, strlen(node->buf)) < 0) {
            error = errno;
        }
        if (error == 0) {
            do {
                bytez = read(node->connfd, node->buf, BUF_SIZE);
                if (bytez < 0 && errno == EAGAIN) {
                    node->reentrant = 1;
                    break;
                }
                if (bytez < 0) {
                    error = errno;
                    break;
                }
                if (bytez == 0) {
                    node->reentrant = 0;
                    break;
                }
                if (write(fd, node->buf, bytez) < 0) {
                    error = errno;
                    break;
                }
                memset(node->buf, 0, BUF_SIZE);
            } while (error == 0 && bytez > 0);
            close(fd);
            flock(fd, LOCK_UN);

            if (node->reentrant)
                return;
        }
    }

    if (error == 0) {
        fd = open(newpath, O_WRONLY | O_APPEND);
        flockError = flock(fd, LOCK_EX);
        if (flockError == -1 && errno == EWOULDBLOCK) {
            node->Donereentrant = 1;
            node->reentrant = 1;
            return;
        }
        char fpath[4096];
        snprintf(fpath, 4096, "./%s", newpath);

        if (rename(node->tempFile, fpath) == -1) {
            error = errno;
        }
        flock(fd, LOCK_UN);
    }

    if (error == 0) {
        send_status(node->connfd, OK, 0);
        node->status = OK.code;
        return;
    } else {
        send_status(node->connfd, NOT_FOUND, 0);
        node->status = NOT_FOUND.code;
        remove_tempFile(node->tempFile);
        return;
    }
}
