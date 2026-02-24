#define _GNU_SOURCE

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIFO_NAME    "/tmp/c_fifo"

struct RequestClient {
    pid_t pid;
    char message[BUFSIZ];
};

int main(void)
{
    struct RequestClient req;
    char message[BUFSIZ] = "hello from Client =)";
    int fd;
    
    fd = open(FIFO_NAME, O_WRONLY);
    if (fd == -1) {
        perror("open");
        exit(1);
    }

    req.pid = getpid();
    strcpy(req.message, message);

    int nbyte;
    if ((nbyte = write(fd, &req, sizeof(req))) != sizeof(req)) {
        perror("write");
        exit(1);
    }
    printf("Запрос отправлен (nbyte = %d)\n", nbyte);
    printf("PID (клиента): %d\n", getpid());

    close(fd);
    return(0);
}
