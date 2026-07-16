#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include "../include/control.h"

extern void service_start_recursive(service_t *svc);

static char g_sock_path[256];

int control_socket_init(const char *run_dir, const char *tree_name) {
    mkdir(run_dir, 0755); /* abaikan error kalau udah ada */

    snprintf(g_sock_path, sizeof(g_sock_path), "%s/%s.sock", run_dir, tree_name);
    unlink(g_sock_path); /* bersihin socket lama kalau ada sisa crash */

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("systeml: socket");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("systeml: bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 8) < 0) {
        perror("systeml: listen");
        close(fd);
        return -1;
    }

    fprintf(stderr, "systeml: control socket at %s\n", g_sock_path);
    return fd;
}

static const char *state_str(svc_state_t s) {
    switch (s) {
        case SVC_STOPPED:  return "stopped";
        case SVC_STARTING: return "starting";
        case SVC_STARTED:  return "started";
        case SVC_STOPPING: return "stopping";
        case SVC_FAILED:   return "failed";
        default:           return "unknown";
    }
}

static void handle_command(int cfd, tree_t *t, char *cmd) {
    char reply[512];
    reply[0] = '\0';

    char *nl = strchr(cmd, '\n');
    if (nl) *nl = '\0';

    char verb[32] = {0};
    char arg[64] = {0};
    sscanf(cmd, "%31s %63s", verb, arg);

    if (strcmp(verb, "status") == 0) {
        for (service_t *s = t->services; s != NULL; s = s->next) {
            char line[128];
            snprintf(line, sizeof(line), "%s: %s (pid %d)\n", s->name, state_str(s->state), s->pid);
            strncat(reply, line, sizeof(reply) - strlen(reply) - 1);
        }
        if (reply[0] == '\0') strcpy(reply, "no services\n");

    } else if (strcmp(verb, "start") == 0) {
        service_t *s = service_find(t, arg);
        if (!s) {
            snprintf(reply, sizeof(reply), "error: '%s' not found\n", arg);
        } else {
            service_start_recursive(s);
            snprintf(reply, sizeof(reply), "ok: '%s' start requested\n", arg);
        }

    } else if (strcmp(verb, "stop") == 0) {
        service_t *s = service_find(t, arg);
        if (!s) {
            snprintf(reply, sizeof(reply), "error: '%s' not found\n", arg);
        } else {
            service_stop(s);
            snprintf(reply, sizeof(reply), "ok: '%s' stopped\n", arg);
        }

    } else if (strcmp(verb, "restart") == 0) {
        service_t *s = service_find(t, arg);
        if (!s) {
            snprintf(reply, sizeof(reply), "error: '%s' not found\n", arg);
        } else {
            service_stop(s);
            service_start_recursive(s);
            snprintf(reply, sizeof(reply), "ok: '%s' restarted\n", arg);
        }

    } else {
        snprintf(reply, sizeof(reply), "error: unknown command '%s'\n", verb);
    }

    write(cfd, reply, strlen(reply));
}

void control_socket_handle(int listen_fd, tree_t *t) {
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("systeml: accept");
        }
        return;
    }

    char buf[256];
    ssize_t n = read(cfd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        handle_command(cfd, t, buf);
    }

    close(cfd);
}
