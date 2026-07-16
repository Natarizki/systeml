#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <dirent.h>

static char g_run_dir[256];
static char g_trees_root[256];
static char g_pid1_conf[256];
static char g_current_link[256];

static void init_paths(void) {
    const char *home = getenv("HOME");
    const char *prefix = getenv("PREFIX");
    if (!home) home = "/root";
    if (!prefix) prefix = "/usr";

    snprintf(g_run_dir, sizeof(g_run_dir), "%s/var/run/systeml", prefix);
    snprintf(g_trees_root, sizeof(g_trees_root), "%s/systeml/trees", home);
    snprintf(g_pid1_conf, sizeof(g_pid1_conf), "%s/etc/systeml/pid1.conf", prefix);
    snprintf(g_current_link, sizeof(g_current_link), "%s/etc/systeml/current", prefix);
}

static void usage(const char *prog) {
    fprintf(stderr, "usage:\n");
    fprintf(stderr, "  %s <tree> status\n", prog);
    fprintf(stderr, "  %s <tree> start <svc>\n", prog);
    fprintf(stderr, "  %s <tree> stop <svc>\n", prog);
    fprintf(stderr, "  %s <tree> restart <svc>\n", prog);
    fprintf(stderr, "  %s tree list\n", prog);
    fprintf(stderr, "  %s tree enable <name>\n", prog);
    fprintf(stderr, "  %s tree disable <name>\n", prog);
    fprintf(stderr, "  %s tree switch <name>\n", prog);
}

/* ---- tree management (file-based, no socket needed) ---- */

static int tree_list(void) {
    DIR *d = opendir(g_trees_root);
    if (!d) {
        fprintf(stderr, "systemlctl: cannot open %s: %s\n", g_trees_root, strerror(errno));
        return 1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        printf("%s\n", ent->d_name);
    }
    closedir(d);
    return 0;
}

static int pid1_conf_contains(const char *name) {
    FILE *f = fopen(g_pid1_conf, "r");
    if (!f) return 0;
    char line[64];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strcmp(line, name) == 0) { found = 1; break; }
    }
    fclose(f);
    return found;
}

static void ensure_conf_dir(void) {
    char dir[256];
    strncpy(dir, g_pid1_conf, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir(dir, 0755); /* abaikan kalau udah ada / gagal */
    }
}

static int tree_enable(const char *name) {
    ensure_conf_dir();

    if (pid1_conf_contains(name)) {
        printf("tree '%s' already enabled\n", name);
        return 0;
    }
    FILE *f = fopen(g_pid1_conf, "a");
    if (!f) {
        fprintf(stderr, "systemlctl: cannot open %s: %s\n", g_pid1_conf, strerror(errno));
        return 1;
    }
    fprintf(f, "%s\n", name);
    fclose(f);
    printf("tree '%s' enabled\n", name);
    return 0;
}

static int tree_disable(const char *name) {
    FILE *f = fopen(g_pid1_conf, "r");
    if (!f) {
        fprintf(stderr, "systemlctl: cannot open %s: %s\n", g_pid1_conf, strerror(errno));
        return 1;
    }

    char lines[64][64];
    int n = 0;
    char line[64];
    int found = 0;
    while (fgets(line, sizeof(line), f) && n < 64) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strcmp(line, name) == 0) { found = 1; continue; }
        strncpy(lines[n], line, sizeof(lines[n]) - 1);
        n++;
    }
    fclose(f);

    if (!found) {
        printf("tree '%s' was not enabled\n", name);
        return 0;
    }

    f = fopen(g_pid1_conf, "w");
    if (!f) {
        fprintf(stderr, "systemlctl: cannot write %s: %s\n", g_pid1_conf, strerror(errno));
        return 1;
    }
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", lines[i]);
    fclose(f);

    printf("tree '%s' disabled\n", name);
    return 0;
}

static int tree_switch(const char *name) {
    char target[256];
    snprintf(target, sizeof(target), "%s/%s", g_trees_root, name);

    struct stat st;
    if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "systemlctl: tree '%s' does not exist at %s\n", name, target);
        return 1;
    }

    ensure_conf_dir();
    unlink(g_current_link);
    if (symlink(target, g_current_link) != 0) {
        fprintf(stderr, "systemlctl: failed to switch: %s\n", strerror(errno));
        return 1;
    }

    printf("current tree switched to '%s'\n", name);
    return 0;
}

/* ---- per-tree instance control (via socket) ---- */

static int socket_command(const char *tree_name, const char *command, const char *arg) {
    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", g_run_dir, tree_name);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("systemlctl: socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "systemlctl: cannot connect to tree '%s' (%s): %s\n",
                tree_name, sock_path, strerror(errno));
        close(fd);
        return 1;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "%s %s\n", command, arg ? arg : "");
    write(fd, msg, strlen(msg));

    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
    }

    close(fd);
    return 0;
}

int main(int argc, char *argv[]) {
    init_paths();

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "tree") == 0) {
        const char *sub = argv[2];
        if (strcmp(sub, "list") == 0) {
            return tree_list();
        } else if (strcmp(sub, "enable") == 0 && argc > 3) {
            return tree_enable(argv[3]);
        } else if (strcmp(sub, "disable") == 0 && argc > 3) {
            return tree_disable(argv[3]);
        } else if (strcmp(sub, "switch") == 0 && argc > 3) {
            return tree_switch(argv[3]);
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    const char *tree_name = argv[1];
    const char *command = argv[2];
    const char *arg = (argc > 3) ? argv[3] : "";

    return socket_command(tree_name, command, arg);
}
