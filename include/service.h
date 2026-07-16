#ifndef SYSTEML_SERVICE_H
#define SYSTEML_SERVICE_H

#include <sys/types.h>
#include <stddef.h>
#include <time.h>

#define SVC_NAME_MAX     64
#define SVC_CMD_MAX      256
#define SVC_MAX_DEPS     16
#define SVC_MAX_ARGS     32

typedef enum {
    SVC_STOPPED = 0,
    SVC_STARTING,
    SVC_STARTED,
    SVC_STOPPING,
    SVC_FAILED
} svc_state_t;

typedef enum {
    SVC_TYPE_PROCESS = 0,   /* long-running daemon, kita track pid */
    SVC_TYPE_BGPROCESS,     /* forks sendiri ke background, butuh pidfile */
    SVC_TYPE_SCRIPTED       /* oneshot start/stop script, run to completion */
} svc_type_t;

typedef struct service {
    char name[SVC_NAME_MAX];
    svc_type_t type;
    svc_state_t state;

    char command[SVC_CMD_MAX];
    char stop_command[SVC_CMD_MAX];
    char *argv[SVC_MAX_ARGS];       /* diisi parse_args() sebelum spawn */

    char pidfile[SVC_CMD_MAX];      /* dipakai kalau type == SVC_TYPE_BGPROCESS */

    struct service *deps[SVC_MAX_DEPS];
    int ndeps;

    pid_t pid;
    int status_pipe[2];             /* buat exec-success/fail signaling */

    int auto_restart;

    int restart_count;
    time_t last_restart_ts;

    struct service *next;           /* linked list dalam satu tree */
} service_t;

typedef struct tree {
    char name[SVC_NAME_MAX];
    service_t *services;
    struct tree *deps[8];
    int ndeps;
    struct tree *next;
} tree_t;

service_t *service_new(const char *name);
service_t *service_find(tree_t *t, const char *name);
tree_t *tree_load(const char *trees_root, const char *tree_name);

int service_start(service_t *svc);
int service_stop(service_t *svc);
void service_start_recursive(service_t *svc);

#endif
