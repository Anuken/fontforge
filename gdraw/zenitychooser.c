/* Hacky native file chooser for Linux: shells out to zenity.
 *
 * - Remembers the last directory used in ~/.config/fontforge/last_file_dialog_dir
 * - Converts FontForge glob filters ("*.{sfd,ttf}{.gz,}") to zenity file filters
 * - Keeps the GLib main loop pumped while zenity is open so the window manager
 *   doesn't decide FontForge has hung.
 *
 * Returns NULL on cancel. If zenity can't be run at all, *unavailable is set
 * to 1 and the caller should fall back to the built-in dialog.
 */
#include <fontforge-config.h>

#include "gdraw.h"
#include "gwidget.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int zen_busy = 0;

/* ---------- tiny string vector ---------- */

struct svec {
    char **v;
    size_t n, cap;
};

static void sv_push(struct svec *s, char *str) {
    if (s->n + 2 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->v = realloc(s->v, s->cap * sizeof(char *));
    }
    s->v[s->n++] = str;
    s->v[s->n] = NULL;
}

static void sv_free(struct svec *s) {
    for (size_t i = 0; i < s->n; ++i)
        free(s->v[i]);
    free(s->v);
    memset(s, 0, sizeof(*s));
}

static char *xstrndup(const char *s, size_t n) {
    char *r = malloc(n + 1);
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

/* ---------- brace expansion: "*.{a,b}{.gz,}" -> "*.a.gz" "*.a" "*.b.gz" "*.b" ---------- */

static void expand_braces(const char *pat, struct svec *out) {
    const char *open = strchr(pat, '{'), *close = NULL, *p, *a;
    int depth = 0;

    if (open == NULL) {
        sv_push(out, strdup(pat));
        return;
    }
    for (p = open; *p; ++p) {
        if (*p == '{') ++depth;
        else if (*p == '}' && --depth == 0) { close = p; break; }
    }
    if (close == NULL) {
        sv_push(out, strdup(pat));
        return;
    }

    char *prefix = xstrndup(pat, open - pat);
    const char *suffix = close + 1;

    depth = 0;
    a = open + 1;
    for (p = open + 1; ; ++p) {
        if (p == close || (*p == ',' && depth == 0)) {
            char *alt = xstrndup(a, p - a);
            char *full = malloc(strlen(prefix) + strlen(alt) + strlen(suffix) + 1);
            strcpy(full, prefix);
            strcat(full, alt);
            strcat(full, suffix);
            expand_braces(full, out);
            free(full);
            free(alt);
            a = p + 1;
            if (p == close) break;
        } else if (*p == '{') {
            ++depth;
        } else if (*p == '}') {
            --depth;
        }
    }
    free(prefix);
}

/* ---------- last-directory persistence ---------- */

static int is_dir(const char *p) {
    struct stat st;
    return p != NULL && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *config_file(int create_dir) {
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char base[4096], dir[4200], *file;

    if (xdg != NULL && xdg[0] == '/')
        snprintf(base, sizeof(base), "%s", xdg);
    else
        snprintf(base, sizeof(base), "%s/.config", home != NULL ? home : "/tmp");
    snprintf(dir, sizeof(dir), "%s/fontforge", base);
    if (create_dir) {
        mkdir(base, 0755);
        mkdir(dir, 0755);
    }
    file = malloc(strlen(dir) + 32);
    sprintf(file, "%s/last_file_dialog_dir", dir);
    return file;
}

static char *lastdir_load(void) {
    char *cf = config_file(0), buf[4096];
    FILE *f = fopen(cf, "r");
    free(cf);
    if (f == NULL) return NULL;
    if (fgets(buf, sizeof(buf), f) == NULL) { fclose(f); return NULL; }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = '\0';
    return is_dir(buf) ? strdup(buf) : NULL;
}

static void lastdir_save(const char *chosen) {
    char *dir, *cf, *slash;
    FILE *f;

    if (chosen == NULL || chosen[0] != '/') return;
    dir = xstrndup(chosen, strcspn(chosen, "\n"));	/* first line only */
    if (!is_dir(dir)) {
        slash = strrchr(dir, '/');
        if (slash == NULL) { free(dir); return; }
        if (slash == dir) dir[1] = '\0'; else *slash = '\0';
    }
    cf = config_file(1);
    f = fopen(cf, "w");
    if (f != NULL) {
        fprintf(f, "%s\n", dir);
        fclose(f);
    }
    free(cf);
    free(dir);
}

/* ---------- run zenity ---------- */

/* Returns malloc'd stdout, or NULL if zenity couldn't be run / was cancelled.
 * *status: 0 ok, 1 cancelled, -1 couldn't run. */
static char *run_zenity(char **argv, int *status) {
    int fds[2];
    pid_t pid;
    char *out = NULL;
    size_t len = 0, cap = 0;
    int wst = 0;

    *status = -1;
    if (pipe(fds) != 0) return NULL;

    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return NULL; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(fds[1], STDOUT_FILENO);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp("zenity", argv);
        _exit(127);
    }
    close(fds[1]);

    for (;;) {
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        int r = poll(&pfd, 1, 30);
        if (r > 0) {
            char buf[4096];
            ssize_t n = read(fds[0], buf, sizeof(buf));
            if (n > 0) {
                if (len + n + 1 > cap) {
                    cap = (len + n + 1) * 2;
                    out = realloc(out, cap);
                }
                memcpy(out + len, buf, n);
                len += n;
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                break;
            }
        } else if (r < 0 && errno != EINTR) {
            break;
        }
        /* keep GDK responding to WM pings / repaints while the dialog is up */
        GDrawProcessPendingEvents(NULL);
    }
    close(fds[0]);
    while (waitpid(pid, &wst, 0) < 0 && errno == EINTR);

    if (out != NULL) out[len] = '\0';

    if (WIFEXITED(wst) && WEXITSTATUS(wst) == 0) {
        *status = 0;
    } else if (WIFEXITED(wst) && WEXITSTATUS(wst) == 1) {
        *status = 1;
    } else {
        *status = -1;	/* 127 = not installed, 255 = zenity error, etc. */
    }
    if (*status != 0) { free(out); return NULL; }
    if (out == NULL) out = strdup("");
    return out;
}

/* ---------- public entry points ---------- */

/* filters: NULL-terminated array of zenity --file-filter values, e.g.
 * "TrueType | *.ttf *.TTF". May be NULL. */
char *FF_ZenityFileChooserFilters(int save, int multiple, const char *title,
        const char *defaultfile, const char *const *filters, int *unavailable) {
    struct svec argv = {0};
    char *last, *start = NULL, *result = NULL, *arg;
    int status;

    *unavailable = 0;
    if (zen_busy) return NULL;	/* re-entered while a dialog is open: ignore */

    /* Starting location */
    last = lastdir_load();
    if (defaultfile != NULL && defaultfile[0] == '/') {
        if (is_dir(defaultfile) && defaultfile[strlen(defaultfile) - 1] != '/') {
            start = malloc(strlen(defaultfile) + 2);
            sprintf(start, "%s/", defaultfile);
        } else
            start = strdup(defaultfile);
    } else {
        const char *base = NULL;
        if (save && defaultfile != NULL && defaultfile[0] != '\0') {
            base = strrchr(defaultfile, '/');
            base = base != NULL ? base + 1 : defaultfile;
        }
        if (last != NULL) {
            start = malloc(strlen(last) + (base ? strlen(base) : 0) + 2);
            sprintf(start, "%s/%s", last, base ? base : "");
        } else if (base != NULL) {
            start = strdup(base);
        }
    }
    free(last);

    sv_push(&argv, strdup("zenity"));
    sv_push(&argv, strdup("--file-selection"));
    if (save) {
        sv_push(&argv, strdup("--save"));
        sv_push(&argv, strdup("--confirm-overwrite"));
    } else if (multiple) {
        sv_push(&argv, strdup("--multiple"));
    }
    sv_push(&argv, strdup("--separator=\n"));
    if (title != NULL && title[0] != '\0') {
        arg = malloc(strlen(title) + 16);
        sprintf(arg, "--title=%s", title);
        sv_push(&argv, arg);
    }
    if (start != NULL) {
        arg = malloc(strlen(start) + 16);
        sprintf(arg, "--filename=%s", start);
        sv_push(&argv, arg);
    }
    for (size_t i = 0; filters != NULL && filters[i] != NULL; ++i) {
        arg = malloc(strlen(filters[i]) + 16);
        sprintf(arg, "--file-filter=%s", filters[i]);
        sv_push(&argv, arg);
    }

    zen_busy = 1;
    result = run_zenity(argv.v, &status);
    zen_busy = 0;

    if (status < 0) {
        *unavailable = 1;
    } else if (result != NULL) {
        /* strip trailing newlines */
        size_t n = strlen(result);
        while (n > 0 && (result[n - 1] == '\n' || result[n - 1] == '\r'))
            result[--n] = '\0';
        if (n == 0) {
            free(result);
            result = NULL;
        } else {
            if (!multiple) {
                char *nl = strchr(result, '\n');
                if (nl != NULL) *nl = '\0';
            }
            lastdir_save(result);	/* first line is all that's looked at */
        }
    }

    free(start);
    sv_free(&argv);
    return result;
}

/* Single glob filter (FontForge syntax, braces allowed) + "All files". */
char *FF_ZenityFileChooser(int save, int multiple, const char *title,
        const char *defaultfile, const char *filter_glob, int *unavailable) {
    struct svec pats = {0}, filt = {0};
    char *arg, *ret;

    if (filter_glob != NULL && filter_glob[0] != '\0' &&
            strcmp(filter_glob, "*") != 0) {
        size_t total = 64;
        expand_braces(filter_glob, &pats);
        for (size_t i = 0; i < pats.n; ++i)
            total += 2 * (strlen(pats.v[i]) + 1);
        arg = malloc(total);
        strcpy(arg, "Matching files |");
        for (size_t i = 0; i < pats.n; ++i) {
            char *up = strdup(pats.v[i]);
            strcat(arg, " ");
            strcat(arg, pats.v[i]);
            for (char *c = up; *c; ++c)
                *c = toupper((unsigned char) *c);
            if (strcmp(up, pats.v[i]) != 0) {
                strcat(arg, " ");
                strcat(arg, up);
            }
            free(up);
        }
        sv_push(&filt, arg);
        sv_push(&filt, strdup("All files | *"));
    }
    ret = FF_ZenityFileChooserFilters(save, multiple, title, defaultfile,
            (const char *const *) filt.v, unavailable);
    sv_free(&pats);
    sv_free(&filt);
    return ret;
}

/* Simple modal error box. Silently does nothing if zenity is missing. */
void FF_ZenityError(const char *title, const char *text) {
    char *argv[7], *t, *x;
    int status;
    char *out;

    t = malloc(strlen(title) + 16);
    x = malloc(strlen(text) + 16);
    sprintf(t, "--title=%s", title);
    sprintf(x, "--text=%s", text);
    argv[0] = "zenity";
    argv[1] = "--error";
    argv[2] = "--no-markup";
    argv[3] = t;
    argv[4] = x;
    argv[5] = NULL;
    out = run_zenity(argv, &status);
    free(out);
    free(t);
    free(x);
}
