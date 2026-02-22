#define _GNU_SOURCE

#include <ctype.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/lock.h>
#include <sys/sysctl.h> 
#include <sys/mman.h> /* POSIX IPC */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h> 
#include <syslog.h>
#include <limits.h>
#include <pthread.h>
#include <semaphore.h> /* POSIX IPC */
#include <poll.h>


#define DAEMON_NAME         "c_daemon"
#define PIDFILE_NAME        "/tmp/c_daemon.pid"
#define FIFO_NAME           "/tmp/c_fifo"
#define SHM_STATS_FILE       "server_stats.file"
#define PIDFILE_MODE        (S_IRWXU | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#define FIFO_MODE           (S_IRWXU | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#define SEM_MODE            (S_IRWXU | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#define MAX_USERS           3
#define USER1               501
#define USER2               502
#define ROOT                0

#define MAX_SIZE_SEMAPHORE  60
#define MAX_UIDS            1000
#define SHM_SIZE            (1024 * 1024)
#define LEN_LAST_PIDS       10
#define POLL_TIMEOUT        100 /* миллисекунд */


/* api */
void to_daemon(const char *);
int lockfile(int fd);
int already_running(void);
void cleanup(void);
uid_t get_uid_by_pid(pid_t);
void default_stats(void);
void *sig_thr(void *);
void *handle_client(void *);
void reread_env(void);
int *parse_config(const char *, int *);


sigset_t mask;
sem_t *semServerStats;
char sem_name[MAX_SIZE_SEMAPHORE];

static volatile int server_running = 1;
static int active_pthreads = 0;
pthread_cond_t condActiveThr = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mtxActiveThr = PTHREAD_MUTEX_INITIALIZER;


struct RequestClient {
    pid_t pid;
    char message[BUFSIZ];
};

struct ServerStats {
    int total_requests;      
    int uid_counts[MAX_UIDS];
    pid_t last_pids[LEN_LAST_PIDS];
    int last_index;            
};
struct ServerStats *stats;

struct {
    int *uids;
    int count;
    int poll_timeout;
    pthread_mutex_t mutex_env;
} config = {
    .uids = NULL,
    .count = 0,
    .poll_timeout = 100,
    .mutex_env = PTHREAD_MUTEX_INITIALIZER,
};

int main(int argc, char *argv[], char *envp[])
{
    char daemon_name[_POSIX_NAME_MAX];
    int fd;
    int shm_fd;
    struct sigaction sa;
    pthread_attr_t attr;
    pthread_t tid;

    snprintf(daemon_name, sizeof(daemon_name), "%s.%d", DAEMON_NAME, getpid());
    to_daemon(daemon_name);

    if (already_running() < 0) {
        exit(1);
    }
    
    //syslog(LOG_INFO, "СЕРВЕР ЗАПУЩЕН");
    
    shm_unlink(SHM_STATS_FILE);

    //syslog(LOG_DEBUG, "shm_open...");
    shm_fd = shm_open(SHM_STATS_FILE, O_CREAT | O_RDWR, 0644);
    if (shm_fd == -1) {
        syslog(LOG_ERR, "shm_open %s: %s", SHM_STATS_FILE, strerror(errno));
        cleanup();
        exit(1);
    }
    
    //syslog(LOG_DEBUG, "ftruncate...");
    if (ftruncate(shm_fd, SHM_SIZE) < 0) {
        syslog(LOG_ERR, "ftruncate: %s", strerror(errno));
        close(shm_fd);
        cleanup();
        exit(1);
    }

    //syslog(LOG_DEBUG, "mmap...");
    stats = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (stats == MAP_FAILED) {
        syslog(LOG_ERR, "невозможно отобразить память: %s", strerror(errno));
        cleanup();
        exit(1);
    }
    memset((struct ServerStats *) stats, 0, SHM_SIZE);
    
    snprintf(sem_name, sizeof(sem_name), "/sem.%d", getpid());

    sem_unlink(sem_name);
    semServerStats = sem_open(sem_name, O_CREAT | O_RDWR, SEM_MODE, 1);
    if (semServerStats == SEM_FAILED) {
        syslog(LOG_ERR, "невозможно создать семафор: %s", strerror(errno));
        cleanup();
        exit(1);
    }

    sleep(1);
    syslog(LOG_INFO, "СЕРВЕР ЗАПУЩЕН");
    default_stats();

    //syslog(LOG_INFO, "значения по умолчанию установлены");

    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_DFL;

    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        syslog(LOG_ERR, "sigaction: невозможно перехватить сигнал SIGHUP:\
%s", strerror(errno));
            cleanup();
            exit(1);
    }
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        syslog(LOG_ERR, "sigaction: невозможно перехватить сигнал SIGCHLD:\
%s", strerror(errno));
            cleanup();
            exit(1);
    }

    sigfillset(&mask);
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) < 0 ) {
        syslog(LOG_ERR, "pthread_sigmask: %s", strerror(errno));
        cleanup();
        exit(1);
    }
    
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    if (pthread_create(&tid, &attr, sig_thr, NULL) < 0) {
        syslog(LOG_ERR, "pthread_create: %s", strerror(errno));
        pthread_attr_destroy(&attr);
        cleanup();
        exit(1);
    }
    pthread_attr_destroy(&attr);

    if (mkfifo(FIFO_NAME, FIFO_MODE) < 0) {
        if (errno != EEXIST) {
            syslog(LOG_ERR, "mkfifo: %s", strerror(errno));
            cleanup();
            exit(1);
        }
    }

    fd = open(FIFO_NAME, O_RDWR);
    if (fd == -1) {
        syslog(LOG_ERR, "open %s: %s", FIFO_NAME, strerror(errno));
        closelog();
        exit(1);
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    struct RequestClient *req;
    while(server_running) {

        int ret = poll(&pfd, 1, POLL_TIMEOUT);
        if (ret == -1) {
            syslog(LOG_ERR, "poll: %s", strerror(errno));
            cleanup();
            exit(1);
        }

        if (pfd.revents & POLL_ERR) {
            syslog(LOG_ERR, "соединение сломалось: %s", strerror(errno));
            cleanup();
            exit(1);
        }

        if (pfd.revents & POLL_HUP) {
            syslog(LOG_ERR, "соединение разорвано: %s", strerror(errno));
            cleanup();
            exit(1);
        }

        if (pfd.revents & POLLNVAL) {
            syslog(LOG_ERR, "дескриптор %d неверный (закрыт или никогда не открывался)",fd);
            cleanup();
            exit(1);
        }

        if (pfd.revents & POLLIN) {
            int nbyte;
            req = (struct RequestClient *)malloc(sizeof(*req));
            if (!req) {
                syslog(LOG_ERR, "malloc: %s", strerror(errno));
                cleanup();
                exit(1);
            }
            if ((nbyte = read(fd, req, sizeof(*req))) < 0) {
                syslog(LOG_ERR, "read file %s: %s\n", FIFO_NAME, strerror(errno));
                closelog();
                break;
            }
            if (nbyte == 0) {
                syslog(LOG_ERR, "канал %s закрыт со стороны клиента", FIFO_NAME);
                break;
            } else {
                pthread_t tid;
                pthread_attr_t attr;
                pthread_attr_init(&attr);
                pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

                if (pthread_create(&tid, NULL, handle_client, (void *)req) < 0) {
                    syslog(LOG_ERR, "pthread_create (handle_client): %s", strerror(errno));
                    pthread_attr_destroy(&attr);
                    cleanup();
                    exit(1);
                }
                pthread_attr_destroy(&attr);
            }
        } 
        
        pfd.revents = 0;
    }

    pthread_mutex_lock(&mtxActiveThr);
    while(active_pthreads > 0) {
        pthread_cond_wait(&condActiveThr, &mtxActiveThr);
    }
    pthread_mutex_unlock(&mtxActiveThr);
    
    cleanup();
    exit(0);
}

void default_stats(void) 
{
    stats->total_requests = 0;
    stats->last_index = 0;

    for (int i=0; i<MAX_UIDS; i++)
        stats->uid_counts[i] = 0;
    
    for (int i=0; i<LEN_LAST_PIDS; i++)
        stats->last_pids[i] = 0;
}

void *handle_client(void *arg) {
    if (!server_running)
        return ((void *)0);

    pthread_mutex_lock(&mtxActiveThr);
    active_pthreads ++;
    pthread_mutex_unlock(&mtxActiveThr);

    struct RequestClient *req = (struct RequestClient *)arg;
    if (req->pid <= 0) {
        syslog(LOG_ERR, "Получен запрос от клиента который неверно передал свой PID: %d",
            req->pid);
        return ((void *) 0);
    }

    int white_list[4] = {ROOT, USER1, USER2};
    int uid_is_white = 0;
    uid_t uid_client = get_uid_by_pid(req->pid);

    for (int i=0; i<MAX_USERS; i++ ) {
        if(white_list[i] == uid_client) {
            uid_is_white = 1;
            break;
        }
    }

    syslog(LOG_INFO, "Запрос клиента | PID: %d | UID: %d | %s | %s",
        req->pid,
        uid_client,
        uid_is_white ? "ДОВЕРИЕ" : "НЕИЗВЕСТНЫЙ",
        req->message);

    if (sem_wait(semServerStats) < 0) {
        syslog(LOG_ERR, "sem_wait: %s", strerror(errno));
        cleanup();
        exit(1);
    }

    stats->uid_counts[uid_client]++;
    stats->last_index = (stats->last_index + 1) % LEN_LAST_PIDS;
    stats->last_pids[stats->last_index] = req->pid;
    
    // syslog(LOG_DEBUG, "последний индекс = %d", stats->last_index);
    // syslog(LOG_DEBUG, "последний pid = %d", stats->last_pids[stats->last_index]);
    stats->total_requests ++;
    if (sem_post(semServerStats) < 0) {
        syslog(LOG_ERR, "sem_post: %s",strerror(errno));
        cleanup();
        exit(1);
    }

    pthread_mutex_lock(&mtxActiveThr);
    active_pthreads--;
    if (active_pthreads == 0 && server_running == 0) {
        pthread_cond_signal(&condActiveThr);
    }
    pthread_mutex_unlock(&mtxActiveThr);

    return ((void *)0);
}

void *sig_thr(void *arg)
{
    int signo, ret;
    for (;;) {
        int ret = sigwait(&mask, &signo);        
        if (ret != 0) {
            syslog(LOG_ERR, "невозможно дождаться сигнала: sigwait: %s", strerror(errno));
            cleanup();
            exit(1);
        }

        if (signo == SIGHUP) {
            syslog(LOG_INFO, "Получен сигнал SIGHUP - перечитываю среду окружения");
        } else if (signo == SIGTERM) {
            syslog(LOG_INFO, "Получен сигнал SIGTERM - завершаюсь");
            server_running = 0;
        } else {
            syslog(LOG_INFO, "Получен сигнал %s", strsignal(signo));
        }
    }

    return((void *)0);
}

uid_t get_uid_by_pid(pid_t pid)
{
    struct kinfo_proc kp;
    size_t len = sizeof(kp);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    sysctl(mib, 4, &kp, &len, NULL, 0);

    return(kp.kp_eproc.e_pcred.p_ruid);
}

void cleanup(void)
{
    if (unlink(PIDFILE_NAME) < 0)
        syslog(LOG_ERR, "невозможно удалить файл: %s: %s", 
            PIDFILE_NAME, strerror(errno));
    else
        syslog(LOG_INFO, "файл %s удален", PIDFILE_NAME);
    
    if (shm_unlink(SHM_STATS_FILE) < 0) 
        syslog(LOG_ERR, "невозможно удалить файл: %s: %s", 
            SHM_STATS_FILE, strerror(errno));
    else
        syslog(LOG_INFO, "файл %s удален", SHM_STATS_FILE);

    if (sem_unlink(sem_name) < 0) 
        syslog(LOG_ERR, "невозможно удалить файл: %s: %s", 
            sem_name, strerror(errno));
    else 
        syslog(LOG_INFO, "файл-семафор %s удален", sem_name);

    syslog(LOG_INFO, "СЕРВЕР ЗАВЕРШИЛСЯ");

    pthread_cond_destroy(&condActiveThr);
    pthread_mutex_destroy(&mtxActiveThr);

    closelog();
}

int lockfile(int fd)
{
    struct flock lock;
    lock.l_len = 0;
    lock.l_start = 0;
    lock.l_whence = SEEK_SET;
    lock.l_type = F_WRLCK;

    if (fcntl(fd, F_SETLK, &lock) < 0) {
        return(-1);
    }
    return(0);
}

int already_running(void)
{
    int fd = open(PIDFILE_NAME, O_RDWR|O_CREAT, PIDFILE_MODE);
    if (fd == -1) {
        syslog(LOG_ERR, "open: %s",strerror(errno));
        exit(1);
    }

    if (lockfile(fd) < 0) {
        struct flock lock;
        if (fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type != F_ULOCK) {
            syslog(LOG_ERR, "другой демон пытается запуститься (pid=%d)", lock.l_pid);
        }
        close(fd);
        closelog();
        exit(0);
    }

    ftruncate(fd, 0);
    char pidline[_POSIX_NAME_MAX];
    snprintf(pidline, sizeof(pidline), "%d", getpid());

    if(write(fd, pidline, strlen(pidline)) != strlen(pidline)) {
        syslog(LOG_ERR, "write: %s",strerror(errno));
        close(fd);
        exit(1);
    }

    return(0);
}

void to_daemon(const char *cmd)
{
    umask(0);

    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGHUP, &sa, NULL) < 0) {
        perror("невозможно перехватить сигнал -HUP\n");
        exit(1);
    }

    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("невозможно перехватить сигнал -CHLD");
        exit(1);
    }

    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) < 0) {
        perror("getrlimit");
        exit(1);
    }

    pid_t pid;
    if ((pid = fork()) < 0) {
        perror("fork");
        exit(1);
    } else if (pid > 0) {
        exit(0);
    }

    if (setsid() < 0) {
        perror("setsid");
        exit(1);
    }

    if ((pid = fork()) < 0) {
        perror("fork");
        exit(1);
    } else if (pid > 0) {
        exit(0);
    }

    if (chdir("/") < 0) {
        perror("chdir in '/'");
        exit(1);
    }

    if (rl.rlim_max == RLIM_INFINITY) rl.rlim_max = 1024;

    for (int i=0; i<rl.rlim_max; ++ i)
        close(i);

    openlog(cmd, LOG_CONS, LOG_DAEMON);
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1) {
        syslog(LOG_ERR, "not open file '/dev/null'");
        exit(1);
    }

    if (fd > 2) {
      syslog(LOG_ERR, "неверный дескриптор: fd: %d", fd);
      exit(1);
    }

    dup2(fd, STDIN_FILENO);             
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
}
