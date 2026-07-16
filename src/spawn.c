#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include "../include/service.h"

static void parse_args(service_t *svc, const char *cmdline) {
    static char buf[SVC_CMD_MAX];
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int argc = 0;
    char *tok = strtok(buf, " \t");
    while (tok != NULL && argc < SVC_MAX_ARGS - 1) {
        svc->argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    svc->argv[argc] = NULL;
}

static pid_t spawn_with_status_pipe(char *const argv[], int *out_errno, int block_until_exit, int *exit_status) {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        *out_errno = errno;
        return -1;
    }
    if (fcntl(pipefd[1], F_SETFD, FD_CLOEXEC) == -1) {
        *out_errno = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        *out_errno = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pipefd[0]);
        setpgid(0, 0);

        sigset_t empty;
        sigemptyset(&empty);
        sigprocmask(SIG_SETMASK, &empty, NULL);

        execvp(argv[0], argv);
        int e = errno;
        write(pipefd[1], &e, sizeof(e));
        close(pipefd[1]);
        _exit(127);
    }

    close(pipefd[1]);

    int child_errno = 0;
    ssize_t r = read(pipefd[0], &child_errno, sizeof(child_errno));
    close(pipefd[0]);

    if (r > 0) {
        *out_errno = child_errno;
        waitpid(pid, NULL, 0);
        return -1;
    }

    if (block_until_exit) {
        int status;
        waitpid(pid, &status, 0);
        if (exit_status) *exit_status = status;
    }

    return pid;
}

/* baca pidfile, retry sampai timeout_ms, return -1 kalau gagal/timeout */
static pid_t read_pidfile_wait(const char *path, int timeout_ms) {
    int waited = 0;
    while (waited < timeout_ms) {
        FILE *f = fopen(path, "r");
        if (f) {
            long pid = 0;
            if (fscanf(f, "%ld", &pid) == 1 && pid > 0) {
                fclose(f);
                return (pid_t)pid;
            }
            fclose(f);
        }
        usleep(50000); /* 50ms */
        waited += 50;
    }
    return -1;
}

static int run_command_str(const char *cmdline, int block_until_exit, int *exit_status) {
    static char buf[SVC_CMD_MAX];
    char *argv[SVC_MAX_ARGS];
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int argc = 0;
    char *tok = strtok(buf, " \t");
    while (tok != NULL && argc < SVC_MAX_ARGS - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;

    int err = 0;
    pid_t pid = spawn_with_status_pipe(argv, &err, block_until_exit, exit_status);
    if (pid < 0) return -1;
    return 0;
}

int service_start(service_t *svc) {
    if (svc->state == SVC_STARTING || svc->state == SVC_STARTED) {
        return 0;
    }

    parse_args(svc, svc->command);
    svc->state = SVC_STARTING;

    if (svc->type == SVC_TYPE_SCRIPTED) {
        int err = 0;
        int status = 0;
        pid_t pid = spawn_with_status_pipe(svc->argv, &err, 1, &status);

        if (pid < 0) {
            fprintf(stderr, "systeml: failed to run '%s': %s\n", svc->name, strerror(err));
            svc->state = SVC_FAILED;
            return -1;
        }

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            svc->state = SVC_STARTED;
            svc->pid = -1;
            fprintf(stderr, "systeml: '%s' ran to completion (ok)\n", svc->name);
            return 0;
        } else {
            svc->state = SVC_FAILED;
            svc->pid = -1;
            fprintf(stderr, "systeml: '%s' script failed\n", svc->name);
            return -1;
        }
    }

    if (svc->type == SVC_TYPE_BGPROCESS) {
        int err = 0;
        pid_t launcher_pid = spawn_with_status_pipe(svc->argv, &err, 0, NULL);

        if (launcher_pid < 0) {
            fprintf(stderr, "systeml: failed to start '%s': %s\n", svc->name, strerror(err));
            svc->state = SVC_FAILED;
            return -1;
        }

        if (svc->pidfile[0] == '\0') {
            fprintf(stderr, "systeml: '%s' is bgprocess but has no pidfile=, cannot track it\n", svc->name);
            svc->state = SVC_FAILED;
            return -1;
        }

        pid_t real_pid = read_pidfile_wait(svc->pidfile, 3000); /* tunggu max 3 detik */
        if (real_pid < 0) {
            fprintf(stderr, "systeml: '%s' pidfile '%s' never appeared\n", svc->name, svc->pidfile);
            svc->state = SVC_FAILED;
            return -1;
        }

        svc->pid = real_pid;
        svc->state = SVC_STARTED;
        fprintf(stderr, "systeml: '%s' started (pid %d, from pidfile)\n", svc->name, real_pid);
        return 0;
    }

    /* default: SVC_TYPE_PROCESS, long-running, kita fork langsung */
    int err = 0;
    pid_t pid = spawn_with_status_pipe(svc->argv, &err, 0, NULL);

    if (pid < 0) {
        fprintf(stderr, "systeml: failed to start '%s': %s\n", svc->name, strerror(err));
        svc->state = SVC_FAILED;
        return -1;
    }

    svc->pid = pid;
    svc->state = SVC_STARTED;
    fprintf(stderr, "systeml: '%s' started (pid %d)\n", svc->name, pid);
    return 0;
}

int service_stop(service_t *svc) {
    if (svc->state == SVC_STOPPED) {
        return 0;
    }

    svc->state = SVC_STOPPING;

    if (svc->type == SVC_TYPE_SCRIPTED) {
        if (svc->stop_command[0] != '\0') {
            run_command_str(svc->stop_command, 1, NULL);
        }
        svc->pid = -1;
        svc->state = SVC_STOPPED;
        fprintf(stderr, "systeml: '%s' stopped\n", svc->name);
        return 0;
    }

    if (svc->pid <= 0) {
        svc->state = SVC_STOPPED;
        return 0;
    }

    if (svc->stop_command[0] != '\0') {
        run_command_str(svc->stop_command, 1, NULL);
    } else {
        kill(svc->pid, SIGTERM);
    }

    if (svc->type == SVC_TYPE_BGPROCESS) {
        /* bukan child kita, gak bisa waitpid -- anggap stopped setelah kirim sinyal */
        svc->pid = -1;
        svc->state = SVC_STOPPED;
        fprintf(stderr, "systeml: '%s' stopped (bgprocess, signal sent)\n", svc->name);
        return 0;
    }

    int status;
    waitpid(svc->pid, &status, 0);

    svc->pid = -1;
    svc->state = SVC_STOPPED;
    fprintf(stderr, "systeml: '%s' stopped\n", svc->name);
    return 0;
}
