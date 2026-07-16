#ifndef SYSTEML_CONTROL_H
#define SYSTEML_CONTROL_H

#include "service.h"

int control_socket_init(const char *run_dir, const char *tree_name);
void control_socket_handle(int listen_fd, tree_t *t);

#endif
