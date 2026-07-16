#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include "../include/service.h"
#include "../include/control.h"

#define RESTART_LIMIT     5
#define RESTART_WINDOW    60  /* detik */

extern int pid1_main(void);

static tree_t *g_tree = NULL;
static char g_trees_root_default[256];
static char g_run_dir_default[256];

static void init_paths(void) {
    const char *home = getenv("HOME");
    const char *prefix = getenv("PREFIX");
    if (!home) home = "/root";
    if (!prefix) prefix = "/usr";

    snprintf(g_trees_root_default, sizeof(g_trees_root_default), "%s/systeml/trees", home);
    snprintf(g_run_dir_default, sizeof(g_run_dir_default), "%s/var/run/systeml", prefix);
}

void service_start_recursive(service_t *svc) {
    if (svc->state == SVC_STARTED || svc->state == SVC_STARTING) return;

    for (int i = 0; i < svc->ndeps; i++) {
        service_start_recursive(svc->deps[i]);
    }
    service_start(svc);
}

static void start_all(tree_t *t) {
    for (service_t *s = t->services; s != NULL; s = s->next) {
        service_start_recursive(s);
    }
}

static void stop_all(tree_t *t) {
    for (service_t *s = t->services; s != NULL; s = s->next) {
        service_stop(s);
    }
}

static service_t *find_by_pid(tree_t *t, pid_t pid) {
    for (service_t *s = t->services; s != NULL; s = s->next) {
        if (s->pid == pid) return s;
    }
    return NULL;
}

static void reap_children(tree_t *t) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        service_t *s = find_by_pid(t, pid);
        if (!s) continue;

        fprintf(stderr, "systeml: '%s' (pid %d) exited\n", s->name, pid);
        s->pid = -1;

        if (s->state == SVC_STOPPING) {
            s->state = SVC_STOPPED;
            s->restart_count = 0;
        } else if (s->auto_restart) {
            time_t now = time(NULL);

            if (s->last_restart_ts != 0 && (now - s->last_restart_ts) > RESTART_WINDOW) {
                s->restart_count = 0;
            }

            s->restart_count++;
            s->last_restart_ts = now;

            if (s->restart_count > RESTART_LIMIT) {
                fprintf(stderr, "systeml: '%s' restarted too many times (%d in %ds), giving up\n",
                        s->name, s->restart_count, RESTART_WINDOW);
                s->state = SVC_FAILED;
            } else {
                fprintf(stderr, "systeml: restarting '%s' (attempt %d)\n", s->name, s->restart_count);
                s->state = SVC_STOPPED;
                service_start(s);
            }
        } else {
            s->state = SVC_STOPPED;
        }
    }
}

int main(int argc, char *argv[]) {
    if (getpid() == 1) {
        return pid1_main();
    }

    init_paths();
    const char *trees_root = g_trees_root_default;
    const char *tree_name = "default";

    if (argc > 1) tree_name = argv[1];
    if (argc > 2) trees_root = argv[2];

    fprintf(stderr, "systeml: loading tree '%s' from %s\n", tree_name, trees_root);

    g_tree = tree_load(trees_root, tree_name);
    if (!g_tree) {
        fprintf(stderr, "systeml: failed to load tree '%s'\n", tree_name);
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        perror("sigprocmask");
        return 1;
    }

    int sfd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (sfd == -1) {
        perror("signalfd");
        return 1;
    }

    int epfd = epoll_create1(0);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = sfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, sfd, &ev);

    int ctrl_fd = control_socket_init(g_run_dir_default, tree_name);
    if (ctrl_fd >= 0) {
        struct epoll_event cev = { .events = EPOLLIN, .data.fd = ctrl_fd };
        epoll_ctl(epfd, EPOLL_CTL_ADD, ctrl_fd, &cev);
    }

    start_all(g_tree);

    fprintf(stderr, "systeml: tree '%s' up, entering main loop\n", tree_name);

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
                        reap_children(g_tree);
                    } else if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
                        fprintf(stderr, "systeml: shutdown signal received\n");
                        stop_all(g_tree);
                        running = 0;
                        break;
                    }
                }
            } else if (events[i].data.fd == ctrl_fd) {
                control_socket_handle(ctrl_fd, g_tree);
            }
        }
    }

    close(sfd);
    close(epfd);
    fprintf(stderr, "systeml: exit\n");
    return 0;
}
