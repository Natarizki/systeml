#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "../include/service.h"

service_t *service_new(const char *name) {
    service_t *svc = calloc(1, sizeof(service_t));
    if (!svc) return NULL;
    strncpy(svc->name, name, SVC_NAME_MAX - 1);
    svc->state = SVC_STOPPED;
    svc->pid = -1;
    svc->type = SVC_TYPE_PROCESS;
    return svc;
}

service_t *service_find(tree_t *t, const char *name) {
    for (service_t *s = t->services; s != NULL; s = s->next) {
        if (strcmp(s->name, name) == 0) return s;
    }
    return NULL;
}

static void trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t') s++;
    end = s + strlen(s) - 1;
    while (end > s && (*end == '\n' || *end == ' ' || *end == '\t' || *end == '\r')) {
        *end = '\0';
        end--;
    }
}

static service_t *parse_service_file(const char *path, const char *name) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    service_t *svc = service_new(name);
    if (!svc) {
        fclose(f);
        return NULL;
    }

    char line[512];
    char deps_raw[256] = {0};

    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        if (line[0] == '\0' || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcmp(key, "type") == 0) {
            if (strcmp(val, "bgprocess") == 0) svc->type = SVC_TYPE_BGPROCESS;
            else if (strcmp(val, "scripted") == 0) svc->type = SVC_TYPE_SCRIPTED;
            else svc->type = SVC_TYPE_PROCESS;
        } else if (strcmp(key, "command") == 0) {
            strncpy(svc->command, val, SVC_CMD_MAX - 1);
        } else if (strcmp(key, "stop-command") == 0) {
            strncpy(svc->stop_command, val, SVC_CMD_MAX - 1);
        } else if (strcmp(key, "pidfile") == 0) {
            strncpy(svc->pidfile, val, SVC_CMD_MAX - 1);
        } else if (strcmp(key, "depends-on") == 0) {
            strncpy(deps_raw, val, sizeof(deps_raw) - 1);
        } else if (strcmp(key, "auto-restart") == 0) {
            svc->auto_restart = (strcmp(val, "yes") == 0 || strcmp(val, "true") == 0);
        }
    }
    fclose(f);

    if (deps_raw[0] != '\0') {
        svc->argv[SVC_MAX_ARGS - 1] = strdup(deps_raw);
    }

    return svc;
}

static void resolve_deps(tree_t *t) {
    for (service_t *s = t->services; s != NULL; s = s->next) {
        char *deps_raw = s->argv[SVC_MAX_ARGS - 1];
        if (!deps_raw) continue;

        char buf[256];
        strncpy(buf, deps_raw, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        char *tok = strtok(buf, " \t");
        while (tok != NULL && s->ndeps < SVC_MAX_DEPS) {
            service_t *dep = service_find(t, tok);
            if (dep) {
                s->deps[s->ndeps++] = dep;
            } else {
                fprintf(stderr, "systeml: warning: '%s' depends-on '%s' not found in tree '%s'\n",
                        s->name, tok, t->name);
            }
            tok = strtok(NULL, " \t");
        }

        free(deps_raw);
        s->argv[SVC_MAX_ARGS - 1] = NULL;
    }
}

tree_t *tree_load(const char *trees_root, const char *tree_name) {
    char dirpath[512];
    snprintf(dirpath, sizeof(dirpath), "%s/%s", trees_root, tree_name);

    DIR *d = opendir(dirpath);
    if (!d) {
        fprintf(stderr, "systeml: cannot open tree '%s': %s\n", tree_name, dirpath);
        return NULL;
    }

    tree_t *t = calloc(1, sizeof(tree_t));
    strncpy(t->name, tree_name, SVC_NAME_MAX - 1);

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char filepath[600];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, ent->d_name);

        struct stat st;
        if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        service_t *svc = parse_service_file(filepath, ent->d_name);
        if (svc) {
            svc->next = t->services;
            t->services = svc;
        }
    }
    closedir(d);

    resolve_deps(t);
    return t;
}
