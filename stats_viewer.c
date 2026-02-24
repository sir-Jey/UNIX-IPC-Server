#define _GNU_SOURCE

#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHM_STATS_FILE      "server_stats.file"
#define SHM_SIZE            (1024 * 1024)
#define MAX_UIDS            1000
#define LEN_LAST_PIDS       10
#define ROOT                0
#define USER1               501  /* the first USER  */
#define USER2               502  /* the second USER */

struct ServerStats {
    int total_requests;
    int uid_counts[MAX_UIDS];    
    pid_t last_pids[LEN_LAST_PIDS];
    int last_index;               
};

int main(void) {
    int shm_fd = shm_open(SHM_STATS_FILE, O_RDWR);
    if (shm_fd == -1) {
        if (errno == 2) {
            fprintf(stderr, "отображаемая память файла %s не создана", SHM_STATS_FILE);
            exit(1);
        }
        perror("shm_open");
        exit(1);
    }

    struct ServerStats *stats;
    stats = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, 
        shm_fd, 0);
    if (stats == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }

    printf("Общее кол-во запросов на сервер: %d\n", stats->total_requests);
    printf("кол-во запросов uID = 0: %d\n", stats->uid_counts[ROOT]);
    printf("Последний PID клиента отправивший запрос: %d\n", stats->last_pids[stats->last_index]);

    printf("\nНиже перечислены 10 последних PID'ов прогамм-клиентов которые отправляли запрос на сервер:\n");
    for (int i=0; i<LEN_LAST_PIDS; i++) {
        printf("%d%s", stats->last_pids[i], i == LEN_LAST_PIDS-1 ? "\n" : ":");
    }

    return (0);
}
