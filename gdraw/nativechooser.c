/* Native file chooser built on tinyfiledialogs (osascript on macOS, and
 * zenity/kdialog/etc. on Linux, Win32 dialogs on Windows).
 *
 * - Remembers the last directory used in ~/.config/fontforge/last_file_dialog_dir
 * - Converts FontForge glob filters ("*.{sfd,ttf}{.gz,}") to tinyfd patterns
 * - tinyfd is blocking, so the dialog runs on a helper thread while the main
 *   thread keeps pumping GDK events so the window manager doesn't decide
 *   FontForge has hung.
 *
 * Returns NULL on cancel. If no dialog backend is usable, *unavailable is set
 * to 1 and the caller should fall back to the built-in dialog.
 *
 * Limitations of tinyfd: it supports a single filter (one description + a
 * list of patterns), so when several filters are supplied their patterns are
 * merged. Multiple selections are '|' separated by tinyfd, so file names
 * containing '|' can't be told apart. Names containing quote characters
 * can't be passed as default paths.
 */
#include <fontforge-config.h>

#include "gdraw.h"
#include "gwidget.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include "tinyfiledialogs.h"
#include <unistd.h>

static int fc_busy = 0;

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

/* ---------- run tinyfd ---------- */

struct fc_job {
    int save, multiple;
    const char *title, *start, *desc;
    int npat;
    const char *const *pats;
    char *result;		/* malloc'd copy of tinyfd's answer */
    volatile int done;
};

static void fc_run(struct fc_job *j) {
    char *r;
    if (j->save)
        r = tinyfd_saveFileDialog(j->title, j->start, j->npat, j->pats, j->desc);
    else
        r = tinyfd_openFileDialog(j->title, j->start, j->npat, j->pats, j->desc,
                j->multiple);
    j->result = r != NULL ? strdup(r) : NULL;
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
}

#ifndef _WIN32
static void *fc_thread(void *arg) {
    fc_run(arg);
    return NULL;
}
#endif

/* tinyfd blocks, so run it on a thread and keep the GUI event loop alive. */
static char *run_tinyfd(struct fc_job *j) {
#ifdef _WIN32
    fc_run(j);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, fc_thread, j) != 0) {
        fc_run(j);
    } else {
        while (!__atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) {
            GDrawProcessPendingEvents(NULL);
            usleep(30000);
        }
        pthread_join(th, NULL);
    }
#endif
    return j->result;
}

static int has_quote(const char *s) {
    return s != NULL && strpbrk(s, "\"'") != NULL;
}

/* 1 if a graphical backend exists (tinyfd's "query" mode). */
static int backend_available(void) {
    return (int) (intptr_t) tinyfd_openFileDialog("tinyfd_query", NULL, 0, NULL,
            NULL, 0) != 0;
}

/* ---------- filters ---------- */

/* FontForge globs list compressed variants ("*.sfd.gz"); they only bloat the
 * pattern list (tinyfd builds a fixed 1 KiB command line), so skip them. */
static int is_compressed_pat(const char *p, size_t n) {
    static const char *const sfx[] = { ".gz", ".Z", ".bz2", ".lzma", ".xz", NULL };
    for (int i = 0; sfx[i] != NULL; ++i) {
        size_t l = strlen(sfx[i]);
        if (n > l + 2 && strncasecmp(p + n - l, sfx[i], l) == 0)
            return 1;
    }
    return 0;
}

/* Add "*.ext" style patterns from a whitespace separated list, skipping "*"
 * and duplicates (tinyfd wants patterns of the form "*.ext"). */
static void add_patterns(struct svec *pats, const char *list) {
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        const char *e = p;
        while (*e && *e != ' ' && *e != '\t') ++e;
        if (e - p > 2 && p[0] == '*' && p[1] == '.' && !is_compressed_pat(p, e - p)) {
            char *pat = xstrndup(p, e - p);
            int dup = 0;
            for (size_t i = 0; i < pats->n; ++i)
                if (strcasecmp(pats->v[i], pat) == 0) { dup = 1; break; }
            if (dup) free(pat); else sv_push(pats, pat);
        }
        p = e;
    }
}

/* ---------- public entry points ---------- */

/* filters: NULL-terminated array of "Description | *.ext *.ext2" strings.
 * tinyfd has a single filter, so the patterns of all entries are merged
 * (the "All files | *" entry contributes nothing). May be NULL. */
char *FF_NativeFileChooserFilters(int save, int multiple, const char *title,
        const char *defaultfile, const char *const *filters, int *unavailable) {
    struct svec pats = {0};
    struct fc_job job;
    char *last, *start = NULL, *result = NULL, *desc = NULL, *t = NULL;
    int nfilters_with_pats = 0;

    *unavailable = 0;
    if (fc_busy) return NULL;	/* re-entered while a dialog is open: ignore */
    if (!backend_available()) {
        *unavailable = 1;
        return NULL;
    }

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
    if (has_quote(start)) {	/* tinyfd refuses quotes in paths */
        free(start);
        start = NULL;
    }

    /* tinyfd also refuses quotes in the title */
    if (title != NULL && title[0] != '\0') {
        t = strdup(title);
        for (char *c = t; *c; ++c)
            if (*c == '"' || *c == '\'') *c = '`';
    }

    for (size_t i = 0; filters != NULL && filters[i] != NULL; ++i) {
        const char *bar = strchr(filters[i], '|');
        size_t before = pats.n;
        add_patterns(&pats, bar != NULL ? bar + 1 : filters[i]);
        if (pats.n > before) {
            ++nfilters_with_pats;
            if (nfilters_with_pats == 1 && bar != NULL) {
                size_t n = bar - filters[i];
                while (n > 0 && filters[i][n - 1] == ' ') --n;
                desc = xstrndup(filters[i], n);
                for (char *c = desc; *c; ++c)
                    if (*c == '"' || *c == '\'') *c = '`';
            }
        }
    }
    if (nfilters_with_pats > 1) {
        free(desc);
        desc = strdup("Supported files");
    }
#ifndef __APPLE__
    /* Non-macOS backends match case-sensitively */
    for (size_t i = 0, n = pats.n; i < n; ++i) {
        char *up = strdup(pats.v[i]);
        for (char *c = up; *c; ++c)
            *c = toupper((unsigned char) *c);
        if (strcmp(up, pats.v[i]) != 0) sv_push(&pats, up); else free(up);
    }
#endif

    /* tinyfd assembles its command line in a fixed 1024 byte buffer with
     * unchecked strcat()s, so keep the variable parts well inside that. */
    {
        size_t total = 0;
        for (size_t i = 0; i < pats.n; ++i)
            total += strlen(pats.v[i]) + 4;
        if (total > 300) {		/* too many patterns: don't filter */
            sv_free(&pats);
            free(desc);
            desc = NULL;
        }
        if (desc != NULL && strlen(desc) > 60)
            desc[60] = '\0';
        if (start != NULL && strlen(start) > 300) {
            free(start);
            start = NULL;
        }
        if (t != NULL && strlen(t) > 100)
            t[100] = '\0';
    }

    memset(&job, 0, sizeof(job));
    job.save = save;
    job.multiple = multiple;
    job.title = t;
    job.start = start;
    job.desc = desc;
    job.npat = (int) pats.n;
    job.pats = (const char *const *) pats.v;

    fc_busy = 1;
    result = run_tinyfd(&job);
    fc_busy = 0;

    if (result != NULL) {
        /* tinyfd separates multiple selections with '|'; callers expect '\n' */
        size_t n;
        for (char *c = result; *c; ++c)
            if (*c == '|') *c = '\n';
        n = strlen(result);
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

    free(t);
    free(desc);
    free(start);
    sv_free(&pats);
    return result;
}

/* Single glob filter (FontForge syntax, braces allowed). */
char *FF_NativeFileChooser(int save, int multiple, const char *title,
        const char *defaultfile, const char *filter_glob, int *unavailable) {
    struct svec pats = {0}, filt = {0};
    char *arg, *ret;

    if (filter_glob != NULL && filter_glob[0] != '\0' &&
            strcmp(filter_glob, "*") != 0) {
        size_t total = 64;
        expand_braces(filter_glob, &pats);
        for (size_t i = 0; i < pats.n; ++i)
            total += strlen(pats.v[i]) + 1;
        arg = malloc(total);
        strcpy(arg, "Matching files |");
        for (size_t i = 0; i < pats.n; ++i) {
            strcat(arg, " ");
            strcat(arg, pats.v[i]);
        }
        sv_push(&filt, arg);
    }
    ret = FF_NativeFileChooserFilters(save, multiple, title, defaultfile,
            (const char *const *) filt.v, unavailable);
    sv_free(&pats);
    sv_free(&filt);
    return ret;
}

/* Simple modal error box. */
void FF_NativeError(const char *title, const char *text) {
    char *t = strdup(title != NULL ? title : ""), *x = strdup(text != NULL ? text : "");
    for (char *c = t; *c; ++c)
        if (*c == '"' || *c == '\'') *c = '`';
    for (char *c = x; *c; ++c)
        if (*c == '"' || *c == '\'') *c = '`';
    tinyfd_messageBox(t, x, "ok", "error", 1);
    free(t);
    free(x);
}
