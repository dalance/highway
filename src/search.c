#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gperftools/tcmalloc.h>
#include "search.h"
#include "regex.h"
#include "file.h"
#include "fjs.h"
#include "color.h"
#include "util.h"
#include "line_list.h"
#include "oniguruma.h"

#define DOT_LENGTH 4
#define APPEND_DOT(c,t) if(c)strcat((t), OMIT_COLOR);\
                             strcat((t), "....");\
                        if(c)strcat((t), RESET_COLOR)

bool is_word_match(const char *buf, int len, match *m)
{
    return (m->start == 0       || is_word_sp(buf[m->start - 1])) &&
           (m->end   == len - 1 || is_word_sp(buf[m->end      ]));
}

/**
 * Search backward from the end of the `n` bytes pointed to by `buf`. The buffer and target char is
 * compared as unsigned char.
 */
char *reverse_char(const char *buf, char c, size_t n)
{
    if (n == 0) {
        return NULL;
    }

    unsigned char *p = (unsigned char *)buf;
    unsigned char uc = c;
    while (--n != 0) {
        if (buf[n] == uc) return (char *)buf + n;
    }
    return p[n] == uc ? (char *)p : NULL;
}

/**
 * Scan the new line in the buffer from `from to `to`. Scaned new line count will be stored to the
 * `line_count` pointer.
 */
char *scan_newline(char *from, char *to, size_t *line_count, char eol)
{
    const char *start = from;
    while (start <= to && (start = memchr(start, eol, to - start + 1)) != NULL) {
        // Found the new line. The `start` variable points to a new line position, so it is
        // increments in order to search next line.
        start++;

        // Also line count is incriments.
        (*line_count)++;
    }

    return to + 1;
}

char *grow_buf_if_shortage(size_t *cur_buf_size,
                           size_t need_size,
                           int buf_offset,
                           char *copy_buf,
                           char *current_buf)
{
    char *new_buf;
    if (*cur_buf_size < need_size + buf_offset + N) {
        *cur_buf_size += need_size + (N - need_size % N);
        new_buf = (char *)tc_calloc(*cur_buf_size, SIZE_OF_CHAR);
        memcpy(new_buf, copy_buf, need_size);
        tc_free(current_buf);
    } else {
        new_buf = copy_buf;
    }

    return new_buf;
}

int search_by(const char *buf,
              size_t search_len,
              const char *pattern,
              int pattern_len,
              enum file_type t,
              match *m,
              int thread_no)
{
    if (op.use_regex) {
        return regex(buf, search_len, pattern, t, m, thread_no);
    } else {
        return fjs(buf, search_len, pattern, pattern_len, t, m);
    }
}

/**
 * Search PATTERN from the line and format them.
 */
int format_line(const char *line,
                int line_len,
                const char *pattern,
                int pattern_len,
                enum file_type t,
                int line_no,
                match *first_match,
                match_line_list *match_line,
                int thread_no)
{
    // Create new match object default size. Maybe, in the most case, default size is very enough
    // because the PATTERN is appeared in one line only less than 10 count.
    int n = 10;
    match *matches = (match *)tc_malloc(sizeof(match) * n);

    int match_count = 1;
    int offset = first_match->end;
    matches[0] = *first_match;

    // Search the all of PATTERN in the line.
    while (search_by(line + offset, line_len - offset, pattern, pattern_len, t, &matches[match_count], thread_no)) {
        // Two times memory will be reallocated if match size is not enough.
        if (n <= match_count) {
            n *= 2;
            matches = (match *)realloc(matches, sizeof(match) * n);
        }

        matches[match_count].start += offset;
        matches[match_count].end   += offset;
        offset = matches[match_count].end;

        match_count++;
    }

    int buffer_len = line_len + (MATCH_WORD_ESCAPE_LEN + OMIT_ESCAPE_LEN) * match_count;
    match_line_node *node = (match_line_node *)tc_malloc(sizeof(match_line_node));
    node->line_no = line_no;
    node->context = CONTEXT_NONE;
    node->line = (char *)tc_calloc(buffer_len, SIZE_OF_CHAR);

    const char *s = line;
    int old_end = 0;
    for (int i = 0; i < match_count; i++) {
        int prefix_len = matches[i].start - old_end;
        int plen = matches[i].end - matches[i].start;

        if (!op.no_omit && matches[i].start - old_end > op.omit_threshold) {
            if (i == 0) {
                int rest_len = op.omit_threshold - DOT_LENGTH;
                APPEND_DOT(op.color, node->line);
                strncat(node->line, s + prefix_len - rest_len, rest_len);
            } else {
                int rest_len = (op.omit_threshold - DOT_LENGTH) / 2;
                strncat(node->line, s, rest_len);
                APPEND_DOT(op.color, node->line);
                strncat(node->line, s + prefix_len - rest_len, rest_len);
            }
        } else {
            strncat(node->line, s, prefix_len);
        }

        if (op.color) {
            strcat(node->line, MATCH_WORD_COLOR);
        }

        strncat(node->line, s + prefix_len, plen);

        if (op.color) {
            strcat(node->line, RESET_COLOR);
        }

        s += prefix_len + plen;
        old_end = matches[i].end;
    }

    int last_end = matches[match_count - 1].end;
    int suffix_len = line_len - last_end;
    if (!op.stdout_redirect && !op.no_omit && suffix_len > op.omit_threshold) {
        strncat(node->line, s, op.omit_threshold - DOT_LENGTH);
        APPEND_DOT(op.color, node->line);
    } else {
        strncat(node->line, s, line_len - last_end);
    }
    enqueue_match_line(match_line, node);

    return match_count;
}

void before_context(const char *buf,
                    const char *line_head,
                    const char *last_match_line_end_pos,
                    int line_no,
                    match_line_list *match_lines,
                    char eol)
{
    int lim = MAX(op.context, op.before_context);
    const char *lines[lim + 1];
    lines[0] = line_head;

    int before_count = 0;
    for (int i = 0; i < lim; i++) {
        if (lines[i] == buf || last_match_line_end_pos == lines[i]) {
            break;
        }

        const char *p = reverse_char(buf, eol, lines[i] - buf - 1);
        p = p == NULL ? buf : p + 1;

        lines[i + 1] = p;
        before_count++;
    }

    for (int i = before_count; i > 0; i--) {
        int line_len = lines[i - 1] - lines[i] - 1;

        match_line_node *node = (match_line_node *)tc_malloc(sizeof(match_line_node));
        node->line_no = line_no - i;
        node->context = CONTEXT_BEFORE;
        node->line = (char *)tc_calloc(line_len + 1, SIZE_OF_CHAR);

        strncat(node->line, lines[i], line_len);
        enqueue_match_line(match_lines, node);
    }
}

char *after_context(const char *line_head,
                    char *last_line_end,
                    int rest_len,
                    int line_no,
                    match_line_list *match_lines,
                    char eol,
                    int *count)
{
    int lim = MAX(op.context, op.after_context);
    *count = 0;

    for (int i = 0; i < lim; i++) {
        if (line_head == last_line_end) {
            break;
        }
        char *after_line = memchr(last_line_end, eol, rest_len);
        if (after_line == NULL) {
            break;
        }

        int line_len = after_line - last_line_end;

        match_line_node *node = (match_line_node *)tc_malloc(sizeof(match_line_node));
        node->line_no = line_no + i + 1;
        node->context = CONTEXT_AFTER;
        node->line = (char *)tc_calloc(line_len + 1, SIZE_OF_CHAR);

        strncat(node->line, last_line_end, line_len);
        enqueue_match_line(match_lines, node);
        (*count)++;

        last_line_end = ++after_line;
    }

    return last_line_end;
}

/**
 * Search the pattern from the file descriptor and add formatted matched lines to the queue if the
 * pattern was matched on the read buffer.
 */
int search(int fd,
           const char *pattern,
           int pattern_len,
           enum file_type t,
           match_line_list *match_line,
           int thread_no)
{
    char eol = '\n';
    size_t line_count = 0;
    size_t read_sum = 0;
    size_t n = N;
    ssize_t read_len;
    int buf_offset = 0;
    int match_count = 0;
    int after_count;
    bool do_search = false;
    char *buf = (char *)tc_calloc(n, SIZE_OF_CHAR);
    char *last_new_line_scan_pos = buf;
    match m;

    if (!op.use_regex) {
        prepare_fjs(pattern, pattern_len, t);
    }

do_search:
    while ((read_len = read(fd, buf + buf_offset, N)) > 0) {
        read_sum += read_len;

        // Search end of position of the last line in the buffer.
        char *last_line_end = reverse_char(buf + buf_offset, eol, read_len);
        if (last_line_end == NULL) {
            buf = last_new_line_scan_pos = grow_buf_if_shortage(&n, read_sum, buf_offset, buf, buf);
            buf_offset += read_len;
            continue;
        }

        do_search = true;

        size_t search_len = last_line_end - buf;
        size_t org_search_len = search_len;
        char *p = buf;

        // Search the first pattern in the buffer.
        while (search_by(p, search_len, pattern, pattern_len, t, &m, thread_no)) {
            // Search head/end of the line, then calculate line length by using them.
            int plen = m.end - m.start;
            size_t rest_len = search_len - m.start - plen + 1;
            char *line_head = reverse_char(p, eol, m.start);
            char *line_end  = memchr(p + m.start + plen, eol, rest_len);
            line_head = line_head == NULL ? p : line_head + 1;

            // Show after context.
            char *last_line_end_by_after = p;
            if (match_count > 0 && (op.after_context > 0 || op.context > 0)) {
                last_line_end_by_after = after_context(line_head, p, p - buf, line_count, match_line, eol, &after_count);
            }

            // Count lines.
            last_new_line_scan_pos = scan_newline(last_new_line_scan_pos, line_end, &line_count, eol);

            // Show before context.
            if (op.before_context > 0 || op.context > 0) {
                before_context(buf, line_head, last_line_end_by_after, line_count, match_line, eol);
            }

            // Search next pattern in the current line and format them in order to print.
            m.start -= line_head - p;
            m.end    = m.start + plen;
            match_count += format_line(line_head, line_end - line_head, pattern, plen, t, line_count, &m, match_line, thread_no);

            search_len -= line_end - p + 1;
            p = line_end + 1;
        }

        // Show last after context. And calculate max line number in this file in order to do
        // padding line number on printing result.
        if (match_count > 0 && (op.after_context > 0 || op.context > 0)) {
            after_context(NULL, p, p - buf, line_count, match_line, eol, &after_count);
            match_line->max_line_no = line_count + after_count;
        } else {
            match_line->max_line_no = line_count;
        }

        if (read_len < N) {
            break;
        }

        last_new_line_scan_pos = scan_newline(last_new_line_scan_pos, last_line_end, &line_count, eol);
        last_line_end++;

        size_t rest = read_sum - org_search_len - 1;
        char *new_buf = grow_buf_if_shortage(&n, rest, 0, last_line_end, buf);
        if (new_buf == last_line_end) {
            new_buf = buf;
            memmove(new_buf, last_line_end, rest);
        }
        buf = last_new_line_scan_pos = new_buf;

        buf_offset = rest;
        read_sum = rest;
    }

    // If there is no new line in the file, we try to search again by '\r' from the head of the
    // file. And also if there is no '\r' in the file, we will skip this file.
    if (read_len > 0 && !do_search && eol == '\n') {
        eol = '\r';
        read_sum = buf_offset = 0;
        lseek(fd, 0, SEEK_SET);
        goto do_search;
    }

    tc_free(buf);
    return match_count;
}
