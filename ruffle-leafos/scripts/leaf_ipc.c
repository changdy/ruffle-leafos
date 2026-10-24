/* Bounded local development client for Jawaka's length-prefixed JSON IPC. */
#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int transfer(int fd, void *data, size_t size, int writing) {
    unsigned char *p = data;
    while (size) {
        ssize_t count = writing ? send(fd, p, size, MSG_NOSIGNAL) : read(fd, p, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        p += count;
        size -= (size_t)count;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3 || strlen(argv[1]) >= sizeof(((struct sockaddr_un *)0)->sun_path) || strlen(argv[2]) > 262144) {
        fprintf(stderr, "usage: leaf-ipc SOCKET JSON (max 256 KiB)\n");
        return 2;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    struct timeval timeout = {.tv_sec = 8};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    strcpy(address.sun_path, argv[1]);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address))) { perror("connect"); close(fd); return 1; }
    uint32_t length = htonl((uint32_t)strlen(argv[2]));
    if (transfer(fd, &length, sizeof(length), 1) || transfer(fd, argv[2], strlen(argv[2]), 1) ||
        transfer(fd, &length, sizeof(length), 0)) { perror("IPC transfer"); close(fd); return 1; }
    length = ntohl(length);
    if (length > 262144) { fprintf(stderr, "response too large\n"); close(fd); return 1; }
    char *response = calloc((size_t)length + 1, 1);
    if (!response || transfer(fd, response, length, 0)) { perror("IPC response"); free(response); close(fd); return 1; }
    puts(response);
    free(response);
    close(fd);
    return 0;
}
