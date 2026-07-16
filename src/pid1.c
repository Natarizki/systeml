#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <errno.h>

static char g_pid1_conf[256];
#define SYSTEML_BIN     "/sbin/systeml"
#define MAX_TREES       16

static void pid1_init_paths(void) {
    const char *prefix = getenv("PREFIX");
    if (!prefix) prefix = "/usr";
    snprintf(g_pid1_conf, sizeof(g_pid1_conf), "%s/etc/systeml/pid1.conf", prefix);
}

typedef struct {
    char name[64];
    pid_t pid;
    int restart_count;
} tree_child_t;

#define PID1_RESTART_LIMIT 5

static tree_child_t g_children[MAX_TREES];
static int g_nchildren = 0;

static void mount_essentials(void) {
    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
}

static void spawn_trees(void) {
    FILE *f = fopen(g_pid1_conf, "r");
    if (!f) {
        fprintf(stderr, "systeml(pid1): no %s found, falling back to tree 'default'\n", g_pid1_conf);
        strcpy(g_children[0].name, "default");
        g_nchildren = 1;
    } else {
        char line[64];
        while (fgets(line, sizeof(line), f) && g_nchildren < MAX_TREES) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (line[0] == '\0' || line[0] == '#') continue;
            strncpy(g_children[g_nchildren].name, line, sizeof(g_children[g_nchildren].name) - 1);
            g_nchildren++;
        }
        fclose(f);
    }

    for (int i = 0; i < g_nchildren; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            execl(SYSTEML_BIN, SYSTEML_BIN, g_children[i].name, (char *)NULL);
            fprintf(stderr, "systeml(pid1): exec failed for tree '%s': %s\n",
                    g_children[i].name, strerror(errno));
            _exit(127);
        } else if (pid > 0) {
            g_children[i].pid = pid;
            fprintf(stderr, "systeml(pid1): tree '%s' spawned (pid %d)\n", g_children[i].name, pid);
        } else {
            fprintf(stderr, "systeml(pid1): fork failed for tree '%s': %s\n",
                    g_children[i].name, strerror(errno));
        }
    }
}

static void reap_all(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        int known = 0;
        for (int i = 0; i < g_nchildren; i++) {
            if (g_children[i].pid == pid) {
                g_children[i].restart_count++;
                if (g_children[i].restart_count > PID1_RESTART_LIMIT) {
                    fprintf(stderr, "systeml(pid1): tree '%s' failed too many times, giving up\n",
                            g_children[i].name);
                    g_children[i].pid = -1;
                    known = 1;
                    break;
                }

                fprintf(stderr, "systeml(pid1): tree '%s' (pid %d) exited, restarting (attempt %d)\n",
                        g_children[i].name, pid, g_children[i].restart_count);
                pid_t newpid = fork();
                if (newpid == 0) {
                    execl(SYSTEML_BIN, SYSTEML_BIN, g_children[i].name, (char *)NULL);
                    _exit(127);
                } else {
                    g_children[i].pid = newpid;
                }
                known = 1;
                break;
            }
        }
        if (!known) {
            /* plain orphan, just reap */
        }
    }
}

static void shutdown_all(void) {
    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].pid > 0) {
            kill(g_children[i].pid, SIGTERM);
        }
    }
    for (int i = 0; i < g_nchildren; i++) {
        if (g_children[i].pid > 0) {
            waitpid(g_children[i].pid, NULL, 0);
        }
    }
}

int pid1_main(void) {
    pid1_init_paths();
    mount_essentials();

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

    spawn_trees();

    fprintf(stderr, "systeml(pid1): all trees spawned, entering main loop\n");

    int running = 1;
    while (running) {
        struct epoll_event events[8];
        int n = epoll_wait(epfd, events, 8, -1);
        if (n < 0) continue;

        for (int i = 0; i < n; i++) {
            if (events[i].data.fd == sfd) {
                struct signalfd_siginfo si;
                while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
                    if (si.ssi_signo == SIGCHLD) {
                        reap_all();
                    } else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        fprintf(stderr, "systeml(pid1): shutdown received\n");
                        shutdown_all();
                        running = 0;
                        break;
                    }
                }
            }
        }
    }

    sync();
    reboot(RB_POWER_OFF);
    return 0;
}
