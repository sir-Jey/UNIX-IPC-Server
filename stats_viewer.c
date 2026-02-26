#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <errno.h>

#define SHM_STATS_FILE      "/server_stats"
#define SHM_SIZE            (1024 * 1024)
#define MAX_UIDS            1000
#define LEN_LAST_PIDS       10
#define ROOT                0

struct ServerStats {
    int total_requests;
    int uid_counts[MAX_UIDS];    
    pid_t last_pids[LEN_LAST_PIDS];
    int last_index;               
};

int main(void) {
    int shm_fd = shm_open(SHM_STATS_FILE, O_RDWR, 0644);
    if (shm_fd == -1) {
        perror("shm_open");
        if(errno == 2) 
            fprintf(stderr, "сервер еще не создал файл '%s' в разделяемой памяти\n", 
                    SHM_STATS_FILE);
        else  
            fprintf(stderr, "ошибка вызова функции shm_open для файла '%s'\n", SHM_STATS_FILE);
        exit(1);
    }
    off_t size_sv_stats;
    if((size_sv_stats = lseek(shm_fd, 0, SEEK_END)) < sizeof(struct ServerStats)) {
        fprintf(stderr, "разделяемой памяти недостаточно\n");
        exit(1);
    }
    struct ServerStats *stats;
    stats = mmap(NULL, size_sv_stats, PROT_READ | PROT_WRITE, MAP_SHARED, 
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

    munmap(stats, size_sv_stats);
    close(shm_fd);

    return (0);
}
