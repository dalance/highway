#ifndef _HW_IGNORE_H_
#define _HW_IGNORE_H_

#include "common.h"

typedef struct _ignore_list_node ignore_list_node;

struct _ignore_list_node {
    char ignore[1024];
    bool is_root;
    bool is_dir;
    bool is_no_dir;
    ignore_list_node *next;
};

typedef struct _ignore_list {
    ignore_list_node *first;
    ignore_list_node *last;
} ignore_list;

ignore_list *create_ignore_list_from_gitignore(const char *path);
ignore_list_node *add_ignore_list(ignore_list *list, char *ignore);
bool is_ignore(ignore_list *list, const char *base, const char *filename, const struct dirent *entry);

#endif // _HW_IGNORE_H_
