// CXPS5 - CarX Street mod loader for a jailbroken PS5 (ps5-payload-sdk).
//
// How it works: CarX Street's PC mod system swaps Addressables ".bundle" files by name. A PS5 build
// has no such system, so CXPS5 does the same thing on disk: it indexes the .bundle files of your
// dumped game, and for every enabled mod bundle with a matching file name it backs the original up
// once and swaps the mod's file in. "Restore" puts every original back. No code is injected into the
// game process, so this does not depend on the game build.
//
// The web UI (ui/index.html) is embedded and served on the port below.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#ifdef __PROSPERO__
#include <sys/sysctl.h>
#include <sys/user.h>
#endif
#ifdef HAVE_CURL
#include <curl/curl.h>
#endif

#include "ui_html.h"

#define VERSION "0.1.0"
#ifdef __PROSPERO__
#define DEFAULT_DATA "/data/cxps5"
#else
#define DEFAULT_DATA "./cxps5-data"
#endif

// ------------------------------------------------------------------ config / globals
typedef struct {
  char data_dir[256], mods_dir[300], backup_dir[300], state_path[320], log_path[320], cacert_path[320];
  char game_root[512];   // optional override; empty = auto-detect
  char catalog_url[512]; // optional JSON catalog the Browse tab reads
  char pin[32];          // optional; when set every /api call needs it
  int  port;
} cfg_t;
static cfg_t g_cfg;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // mod store, index, apply

static void cpy(char *d, size_t n, const char *s) {
  if (n) snprintf(d, n, "%s", s ? s : "");
}

// ------------------------------------------------------------------ log + notify
static FILE *g_logf;
static pthread_mutex_t g_loglock = PTHREAD_MUTEX_INITIALIZER;

static void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void LOG(const char *fmt, ...) {
  char msg[1500];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  time_t t = time(NULL);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char ts[32];
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
  pthread_mutex_lock(&g_loglock);
  printf("[cxps5] %s\n", msg);
  fflush(stdout);
  if (g_logf) {
    fprintf(g_logf, "%s %s\n", ts, msg);
    fflush(g_logf);
  }
  pthread_mutex_unlock(&g_loglock);
}

#ifdef __PROSPERO__
typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);
static void notify(const char *msg) {
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}
#else
static void notify(const char *msg) { (void)msg; }
#endif

// ------------------------------------------------------------------ small utils
typedef struct {
  char  *p;
  size_t len, cap;
} sb_t;

static void sb_add(sb_t *s, const char *b, size_t n) {
  if (s->len + n + 1 > s->cap) {
    s->cap = (s->len + n + 1) * 2 + 256;
    s->p = realloc(s->p, s->cap);
  }
  memcpy(s->p + s->len, b, n);
  s->len += n;
  s->p[s->len] = 0;
}
static void sb_puts(sb_t *s, const char *t) { sb_add(s, t, strlen(t)); }
static void sb_printf(sb_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sb_t *s, const char *fmt, ...) {
  char tmp[1024];
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  if (n >= (int)sizeof tmp) {
    char *big = malloc((size_t)n + 1);
    vsnprintf(big, (size_t)n + 1, fmt, ap2);
    sb_add(s, big, (size_t)n);
    free(big);
  } else if (n > 0) {
    sb_add(s, tmp, (size_t)n);
  }
  va_end(ap2);
}
static void sb_json(sb_t *s, const char *t) {
  sb_puts(s, "\"");
  for (; t && *t; t++) {
    unsigned char c = (unsigned char)*t;
    if (c == '"' || c == '\\') {
      char e[3] = {'\\', (char)c, 0};
      sb_puts(s, e);
    } else if (c < 0x20) {
      sb_printf(s, "\\u%04x", c);
    } else {
      sb_add(s, (const char *)&c, 1);
    }
  }
  sb_puts(s, "\"");
}

static int mkdirs(const char *path) {
  char tmp[600];
  cpy(tmp, sizeof tmp, path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0777);
      *p = '/';
    }
  }
  return mkdir(tmp, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static int has_ext(const char *n, const char *ext) {
  size_t a = strlen(n), b = strlen(ext);
  return a >= b && strcasecmp(n + a - b, ext) == 0;
}

static int valid_id(const char *s) {
  size_t n = strlen(s);
  if (n < 1 || n > 64) return 0;
  for (; *s; s++)
    if (!isalnum((unsigned char)*s) && *s != '_' && *s != '-') return 0;
  return 1;
}
static int valid_bundle_name(const char *s) {
  size_t n = strlen(s);
  if (n < 8 || n > 150 || s[0] == '.' || !has_ext(s, ".bundle") || strstr(s, "..")) return 0;
  for (; *s; s++)
    if (!isalnum((unsigned char)*s) && !strchr("._-@+=,()[] ", *s)) return 0;
  return 1;
}

static void pct_decode(char *s, int plus) {
  char *o = s;
  for (; *s; s++) {
    if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
      char h[3] = {s[1], s[2], 0};
      *o++ = (char)strtol(h, NULL, 16);
      s += 2;
    } else if (plus && *s == '+') {
      *o++ = ' ';
    } else {
      *o++ = *s;
    }
  }
  *o = 0;
}

static int qget(const char *q, const char *key, char *out, size_t n) {
  size_t kl = strlen(key);
  for (const char *p = q; p && *p;) {
    if (!strncmp(p, key, kl) && p[kl] == '=') {
      p += kl + 1;
      size_t l = strcspn(p, "&");
      if (l >= n) l = n - 1;
      memcpy(out, p, l);
      out[l] = 0;
      pct_decode(out, 1);
      return 1;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return 0;
}

// Tiny JSON field reader for the flat objects the UI posts: strings, numbers, true/false.
static int json_get(const char *j, const char *key, char *out, size_t n) {
  char pat[80];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(j, pat);
  if (!p) return 0;
  p += strlen(pat);
  while (*p && isspace((unsigned char)*p)) p++;
  if (*p != ':') return 0;
  p++;
  while (*p && isspace((unsigned char)*p)) p++;
  size_t o = 0;
  if (*p == '"') {
    p++;
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) {
        p++;
        char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
        if (o + 1 < n) out[o++] = c;
        p++;
        continue;
      }
      if (o + 1 < n) out[o++] = *p;
      p++;
    }
  } else {
    while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) {
      if (o + 1 < n) out[o++] = *p;
      p++;
    }
  }
  out[o] = 0;
  return 1;
}

static int write_all(int fd, const void *b, size_t n) {
  const char *p = b;
  while (n) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

// Copies src to dst through a temp file, then renames over dst. 0 on success.
static int copy_file(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0) return -1;
  char tmp[1100];
  snprintf(tmp, sizeof tmp, "%s.cxtmp", dst);
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (out < 0) {
    close(in);
    return -1;
  }
  size_t bs = 1 << 20;
  char  *buf = malloc(bs);
  int    rc = 0;
  for (;;) {
    ssize_t n = read(in, buf, bs);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) rc = -1;
    if (n <= 0) break;
    if (write_all(out, buf, (size_t)n)) {
      rc = -1;
      break;
    }
  }
  free(buf);
  close(in);
  if (rc == 0 && fsync(out) != 0) rc = -1;
  if (close(out) != 0) rc = -1;
  if (rc == 0 && rename(tmp, dst) != 0) rc = -1;
  if (rc) unlink(tmp);
  return rc;
}

static long long file_size(const char *p) {
  struct stat st;
  return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

static uint64_t fnv64(const char *s) {
  uint64_t h = 1469598103934665603ULL;
  for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
  return h;
}

// ------------------------------------------------------------------ Unity bundle header
typedef struct {
  int      ok;
  unsigned ver;
  char     engine[48];
} ufs_t;

// UnityFS header: "UnityFS\0", u32 BE format version, NUL-terminated player and engine versions.
static void ufs_read(const char *path, ufs_t *u) {
  memset(u, 0, sizeof *u);
  unsigned char b[256];
  FILE         *f = fopen(path, "rb");
  if (!f) return;
  size_t n = fread(b, 1, sizeof b - 1, f);
  fclose(f);
  b[n] = 0;
  if (n < 20 || memcmp(b, "UnityFS", 8) != 0) return;
  u->ver = ((unsigned)b[8] << 24) | (b[9] << 16) | (b[10] << 8) | b[11];
  size_t o = 12 + strnlen((char *)b + 12, n - 12) + 1;  // skip player version
  if (o >= n) return;
  snprintf(u->engine, sizeof u->engine, "%.*s", (int)strnlen((char *)b + o, n - o), b + o);
  u->ok = 1;
}

// ------------------------------------------------------------------ game detection + bundle index
typedef struct {
  char      name[160];
  char      path[768];
  long long size;
} gfile_t;
static gfile_t *g_idx;
static int      g_nidx, g_capidx;
static char     g_root[512], g_title[32], g_gname[128], g_gver[32], g_runnote[400];
static time_t   g_last_apply;

static void idx_add(const char *name, const char *path, long long size) {
  if (strlen(name) >= sizeof g_idx[0].name || strlen(path) >= sizeof g_idx[0].path) return;
  if (g_nidx == g_capidx) {
    g_capidx = g_capidx ? g_capidx * 2 : 1024;
    g_idx = realloc(g_idx, (size_t)g_capidx * sizeof *g_idx);
  }
  cpy(g_idx[g_nidx].name, sizeof g_idx[0].name, name);
  cpy(g_idx[g_nidx].path, sizeof g_idx[0].path, path);
  g_idx[g_nidx].size = size;
  g_nidx++;
}

static void scan_dir(const char *dir, int depth) {
  if (depth > 14 || g_nidx >= 60000) return;
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (lstat(p, &st)) continue;
    if (S_ISDIR(st.st_mode))
      scan_dir(p, depth + 1);
    else if (S_ISREG(st.st_mode) && has_ext(e->d_name, ".bundle"))
      idx_add(e->d_name, p, (long long)st.st_size);
  }
  closedir(d);
}

// A CarX Street install has sce_sys/param.json naming the game.
static int probe_game(const char *dir) {
  char p[800];
  snprintf(p, sizeof p, "%s/sce_sys/param.json", dir);
  FILE *f = fopen(p, "rb");
  if (!f) return 0;
  char  *buf = calloc(1, 65537);
  size_t n = fread(buf, 1, 65536, f);
  fclose(f);
  buf[n] = 0;
  int hit = strcasestr(buf, "carx") != NULL;
  if (hit) {
    json_get(buf, "titleId", g_title, sizeof g_title);
    json_get(buf, "titleName", g_gname, sizeof g_gname);
    if (!json_get(buf, "contentVersion", g_gver, sizeof g_gver)) json_get(buf, "masterVersion", g_gver, sizeof g_gver);
    cpy(g_root, sizeof g_root, dir);
  }
  free(buf);
  return hit;
}

static int probe_children(const char *dir, int levels) {
  if (probe_game(dir)) return 1;
  if (levels <= 0) return 0;
  DIR *d = opendir(dir);
  if (!d) return 0;
  struct dirent *e;
  int            seen = 0, hit = 0;
  while (!hit && (e = readdir(d)) && seen++ < 3000) {
    if (e->d_name[0] == '.') continue;
    char p[800];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) hit = probe_children(p, levels - 1);
  }
  closedir(d);
  return hit;
}

// (Re)detects the game and rebuilds the bundle index. Caller holds g_lock.
static void game_scan(void) {
  g_nidx = 0;
  g_root[0] = g_title[0] = g_gname[0] = g_gver[0] = 0;
  if (g_cfg.game_root[0]) {
    if (!probe_game(g_cfg.game_root)) cpy(g_root, sizeof g_root, g_cfg.game_root);  // trust the override
  } else {
    static const char *const roots[] = {"/user/app",        "/data/homebrew",   "/data/games", "/data/etaHEN/games",
                                        "/mnt/ext0",        "/mnt/ext1",        "/mnt/usb0",   "/mnt/usb1",
                                        "/mnt/usb2",        "/mnt/usb3",        "/data",       NULL};
    for (int i = 0; roots[i] && !g_root[0]; i++) probe_children(roots[i], i == 10 ? 1 : 2);
  }
  if (g_root[0]) scan_dir(g_root, 0);
  LOG("game scan: root=%s title=%s bundles=%d", g_root[0] ? g_root : "(none)", g_title, g_nidx);
}

static int find_target(const char *name) {
  for (int i = 0; i < g_nidx; i++)
    if (!strcmp(g_idx[i].name, name)) return i;
  for (int i = 0; i < g_nidx; i++)
    if (!strcasecmp(g_idx[i].name, name)) return i;
  return -1;
}

#ifdef __PROSPERO__
static int game_running(void) {
  int    mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0)) return 0;
  size += 32 * sizeof(struct kinfo_proc);
  char *buf = malloc(size);
  if (!buf) return 0;
  int found = 0;
  if (sysctl(mib, 4, buf, &size, NULL, 0) == 0) {
    for (char *p = buf; p < buf + size;) {
      struct kinfo_proc *ki = (struct kinfo_proc *)p;
      if (ki->ki_structsize <= 0) break;
      p += ki->ki_structsize;
      if (!strcmp(ki->ki_comm, "eboot.bin")) found = 1;
    }
  }
  free(buf);
  return found;
}
#else
static int game_running(void) { return getenv("CXPS5_FAKE_RUNNING") != NULL; }
#endif

// ------------------------------------------------------------------ mod store
// <mods_dir>/<id>/mod.ini (name, enabled, priority, added, source) + <id>/*.bundle
typedef struct {
  char id[65], name[100], source[200];
  int  enabled, prio;
  long added;
} meta_t;

static void meta_path(const char *id, char *out, size_t n) { snprintf(out, n, "%s/%s/mod.ini", g_cfg.mods_dir, id); }

static void meta_read(const char *id, meta_t *m) {
  memset(m, 0, sizeof *m);
  cpy(m->id, sizeof m->id, id);
  cpy(m->name, sizeof m->name, id);
  m->enabled = 1;
  char  p[700], line[400];
  meta_path(id, p, sizeof p);
  FILE *f = fopen(p, "r");
  if (!f) return;
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = 0;
    if (!strcmp(line, "name")) cpy(m->name, sizeof m->name, eq);
    else if (!strcmp(line, "source")) cpy(m->source, sizeof m->source, eq);
    else if (!strcmp(line, "enabled")) m->enabled = atoi(eq);
    else if (!strcmp(line, "priority")) m->prio = atoi(eq);
    else if (!strcmp(line, "added")) m->added = atol(eq);
  }
  fclose(f);
}

static int meta_write(const meta_t *m) {
  char p[700];
  meta_path(m->id, p, sizeof p);
  FILE *f = fopen(p, "w");
  if (!f) return -1;
  char nm[100], src[200];
  cpy(nm, sizeof nm, m->name);
  cpy(src, sizeof src, m->source);
  for (char *c = nm; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
  for (char *c = src; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
  fprintf(f, "name=%s\nsource=%s\nenabled=%d\npriority=%d\nadded=%ld\n", nm, src, m->enabled, m->prio, m->added);
  return fclose(f);
}

static void ensure_mod(const char *id, const char *name, const char *source) {
  char dir[600];
  snprintf(dir, sizeof dir, "%s/%s", g_cfg.mods_dir, id);
  mkdirs(dir);
  char p[700];
  meta_path(id, p, sizeof p);
  if (access(p, F_OK) == 0) return;
  meta_t m;
  memset(&m, 0, sizeof m);
  cpy(m.id, sizeof m.id, id);
  cpy(m.name, sizeof m.name, name && *name ? name : id);
  cpy(m.source, sizeof m.source, source);
  m.enabled = 1;
  m.added = (long)time(NULL);
  meta_write(&m);
}

typedef struct {
  char name[160], modid[65], src[900];
  long long size;
  int  prio;
  long added;
} want_t;

static int cmp_mod_order(const void *a, const void *b) {
  const want_t *x = a, *y = b;
  if (x->prio != y->prio) return x->prio - y->prio;
  if (x->added != y->added) return x->added < y->added ? -1 : 1;
  return strcmp(x->modid, y->modid);
}

// The bundle files of every enabled mod, as (name -> source file); a later (higher priority) mod
// replaces an earlier one with the same bundle name. Caller frees *out.
static int build_desired(want_t **out) {
  want_t *all = NULL;
  int     n = 0, cap = 0;
  DIR    *d = opendir(g_cfg.mods_dir);
  struct dirent *e;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.' || !valid_id(e->d_name)) continue;
    meta_t m;
    meta_read(e->d_name, &m);
    if (!m.enabled) continue;
    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", g_cfg.mods_dir, e->d_name);
    DIR *md = opendir(dir);
    struct dirent *fe;
    while (md && (fe = readdir(md))) {
      if (!valid_bundle_name(fe->d_name)) continue;
      if (n == cap) {
        cap = cap ? cap * 2 : 64;
        all = realloc(all, (size_t)cap * sizeof *all);
      }
      want_t *w = &all[n++];
      cpy(w->name, sizeof w->name, fe->d_name);
      cpy(w->modid, sizeof w->modid, m.id);
      snprintf(w->src, sizeof w->src, "%s/%s", dir, fe->d_name);
      w->size = file_size(w->src);
      w->prio = m.prio;
      w->added = m.added;
    }
    if (md) closedir(md);
  }
  if (d) closedir(d);
  qsort(all, (size_t)n, sizeof *all, cmp_mod_order);
  // collapse duplicates: keep the last (highest priority) entry per bundle name
  int k = 0;
  for (int i = 0; i < n; i++) {
    int dup = 0;
    for (int j = i + 1; j < n; j++)
      if (!strcasecmp(all[i].name, all[j].name)) { dup = 1; break; }
    if (!dup) all[k++] = all[i];
  }
  *out = all;
  return k;
}

// ------------------------------------------------------------------ applied state
typedef struct {
  char name[160], modid[65], target[768], backup[900];
  long long size;
} rec_t;

static int state_load(rec_t **out) {
  rec_t *r = NULL;
  int    n = 0, cap = 0;
  FILE  *f = fopen(g_cfg.state_path, "r");
  char   line[2200];
  while (f && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *fld[5];
    int   k = 0;
    for (char *p = line; k < 5;) {
      fld[k++] = p;
      p = strchr(p, '\t');
      if (!p) break;
      *p++ = 0;
    }
    if (k < 5) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 32;
      r = realloc(r, (size_t)cap * sizeof *r);
    }
    cpy(r[n].name, sizeof r[n].name, fld[0]);
    cpy(r[n].modid, sizeof r[n].modid, fld[1]);
    r[n].size = atoll(fld[2]);
    cpy(r[n].target, sizeof r[n].target, fld[3]);
    cpy(r[n].backup, sizeof r[n].backup, fld[4]);
    n++;
  }
  if (f) fclose(f);
  *out = r;
  return n;
}

static int state_save(const rec_t *r, int n) {
  char tmp[400];
  snprintf(tmp, sizeof tmp, "%s.tmp", g_cfg.state_path);
  FILE *f = fopen(tmp, "w");
  if (!f) return -1;
  for (int i = 0; i < n; i++) fprintf(f, "%s\t%s\t%lld\t%s\t%s\n", r[i].name, r[i].modid, r[i].size, r[i].target, r[i].backup);
  if (fclose(f)) return -1;
  return rename(tmp, g_cfg.state_path);
}

static void backup_path_for(const char *target, const char *name, char *out, size_t n) {
  char shortn[120];
  cpy(shortn, sizeof shortn, name);
  snprintf(out, n, "%s/%016llx_%s", g_cfg.backup_dir, (unsigned long long)fnv64(target), shortn);
}

// ------------------------------------------------------------------ HTTP plumbing
typedef struct {
  int       fd;
  char      method[8], path[600], query[600], pin[64];
  long long clen;
  char      buf[16384];
  int       buflen, bodyoff;
  int       loopback;
} req_t;

static void send_all(int fd, const char *b, size_t n) {
  while (n) {
    ssize_t w = send(fd, b, n, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return;
    b += w;
    n -= (size_t)w;
  }
}

static void respond_h(int fd, int code, const char *ctype, const char *extra, const char *body, size_t len) {
  const char *msg = code == 200 ? "OK" : code == 204 ? "No Content" : code == 400 ? "Bad Request"
                  : code == 401 ? "Unauthorized" : code == 404 ? "Not Found" : code == 409 ? "Conflict"
                  : code == 501 ? "Not Implemented" : "Error";
  char h[420];
  int  n = snprintf(h, sizeof h,
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                   "Connection: close\r\n%s\r\n", code, msg, ctype, len, extra);
  send_all(fd, h, (size_t)n);
  if (len) send_all(fd, body, len);
}
static void respond(int fd, int code, const char *ctype, const char *body, size_t len) { respond_h(fd, code, ctype, "", body, len); }
static void respond_json(int fd, int code, sb_t *s) {
  respond(fd, code, "application/json", s->p ? s->p : "{}", s->len);
  free(s->p);
  s->p = NULL;
}
static void respond_err(int fd, int code, const char *err) {
  sb_t s = {0};
  sb_puts(&s, "{\"error\":");
  sb_json(&s, err);
  sb_puts(&s, "}");
  respond_json(fd, code, &s);
}

static char *body_alloc(req_t *r, long long max) {
  if (r->clen < 0 || r->clen > max) return NULL;
  char *b = malloc((size_t)r->clen + 1);
  long long have = r->buflen - r->bodyoff;
  if (have > r->clen) have = r->clen;
  if (have > 0) memcpy(b, r->buf + r->bodyoff, (size_t)have);
  while (have < r->clen) {
    ssize_t n = recv(r->fd, b + have, (size_t)(r->clen - have), 0);
    if (n <= 0) { free(b); return NULL; }
    have += n;
  }
  b[r->clen] = 0;
  return b;
}

static long long body_to_fd(req_t *r, int ofd) {
  long long left = r->clen;
  if (left < 0) return -1;
  long long have = r->buflen - r->bodyoff;
  if (have > left) have = left;
  if (have > 0 && write_all(ofd, r->buf + r->bodyoff, (size_t)have)) return -1;
  left -= have;
  char *tmp = malloc(1 << 16);
  while (left > 0) {
    ssize_t n = recv(r->fd, tmp, left > (1 << 16) ? (1 << 16) : (size_t)left, 0);
    if (n <= 0 || write_all(ofd, tmp, (size_t)n)) { free(tmp); return -1; }
    left -= n;
  }
  free(tmp);
  return r->clen;
}

// ------------------------------------------------------------------ zip import (stored + deflate)
static unsigned rd16(const unsigned char *b) { return b[0] | (b[1] << 8); }
static unsigned rd32(const unsigned char *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24); }

// Extracts every *.bundle in the zip (flattened to its base name) into destdir. Returns the number
// extracted, or -1 with err set.
static int zip_extract(const char *zip, const char *destdir, char *err, size_t en) {
  FILE *f = fopen(zip, "rb");
  if (!f) { cpy(err, en, "cannot open zip"); return -1; }
  fseeko(f, 0, SEEK_END);
  off_t size = ftello(f);
  size_t tail = size < 70000 ? (size_t)size : 70000;
  unsigned char *t = malloc(tail);
  fseeko(f, size - (off_t)tail, SEEK_SET);
  if (fread(t, 1, tail, f) != tail) { free(t); fclose(f); cpy(err, en, "zip read error"); return -1; }
  long eocd = -1;
  for (long i = (long)tail - 22; i >= 0; i--)
    if (rd32(t + i) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) { free(t); fclose(f); cpy(err, en, "not a zip file"); return -1; }
  unsigned total = rd16(t + eocd + 10), cdsize = rd32(t + eocd + 12), cdoff = rd32(t + eocd + 16);
  free(t);
  if (cdoff == 0xFFFFFFFFu || cdsize == 0xFFFFFFFFu) { fclose(f); cpy(err, en, "zip64 archives are not supported, re-zip with normal settings"); return -1; }
  unsigned char *cd = malloc(cdsize + 1);
  fseeko(f, cdoff, SEEK_SET);
  if (fread(cd, 1, cdsize, f) != cdsize) { free(cd); fclose(f); cpy(err, en, "zip directory unreadable"); return -1; }
  int got = 0;
  unsigned char *p = cd;
  unsigned char *ibuf = malloc(1 << 16), *obuf = malloc(1 << 16);
  for (unsigned i = 0; i < total && p + 46 <= cd + cdsize && rd32(p) == 0x02014b50; i++) {
    unsigned flags = rd16(p + 8), method = rd16(p + 10), fnl = rd16(p + 28), exl = rd16(p + 30), cml = rd16(p + 32);
    unsigned csize = rd32(p + 20), usize = rd32(p + 24), loff = rd32(p + 42);
    char name[300];
    size_t nl = fnl < sizeof name - 1 ? fnl : sizeof name - 1;
    memcpy(name, p + 46, nl);
    name[nl] = 0;
    p += 46 + fnl + exl + cml;
    char *base = name;
    for (char *c = name; *c; c++) if (*c == '/' || *c == '\\') base = c + 1;
    if (!*base || !valid_bundle_name(base) || (flags & 1) || (method != 0 && method != 8)) continue;
    unsigned char lh[30];
    fseeko(f, loff, SEEK_SET);
    if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) continue;
    fseeko(f, loff + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
    char out[800], tmp[820];
    snprintf(out, sizeof out, "%s/%s", destdir, base);
    snprintf(tmp, sizeof tmp, "%s.part", out);
    int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (ofd < 0) { cpy(err, en, strerror(errno)); got = -1; break; }
    int ok = 1;
    if (method == 0) {
      unsigned left = csize;
      while (left && ok) {
        size_t want = left > (1 << 16) ? (1 << 16) : left;
        if (fread(ibuf, 1, want, f) != want || write_all(ofd, ibuf, want)) ok = 0;
        left -= (unsigned)want;
      }
    } else {
      z_stream zs;
      memset(&zs, 0, sizeof zs);
      if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) ok = 0;
      unsigned left = csize;
      int zr = Z_OK;
      while (ok && zr != Z_STREAM_END) {
        if (zs.avail_in == 0) {
          size_t want = left > (1 << 16) ? (1 << 16) : left;
          if (!want) { ok = 0; break; }
          if (fread(ibuf, 1, want, f) != want) { ok = 0; break; }
          left -= (unsigned)want;
          zs.next_in = ibuf;
          zs.avail_in = (unsigned)want;
        }
        zs.next_out = obuf;
        zs.avail_out = 1 << 16;
        zr = inflate(&zs, Z_NO_FLUSH);
        if (zr != Z_OK && zr != Z_STREAM_END) { ok = 0; break; }
        if (write_all(ofd, obuf, (1 << 16) - zs.avail_out)) ok = 0;
      }
      inflateEnd(&zs);
    }
    (void)usize;
    close(ofd);
    if (ok && rename(tmp, out) == 0) got++;
    else { unlink(tmp); cpy(err, en, "failed to extract a bundle"); got = -1; break; }
  }
  free(ibuf); free(obuf); free(cd);
  fclose(f);
  if (got == 0) cpy(err, en, "no .bundle files found in the zip");
  return got;
}

// ------------------------------------------------------------------ downloads (libcurl)
typedef struct {
  int  used, id, pct, cancel;
  char state[16], msg[200], url[512], modid[65], modname[100], file[160];
} job_t;
static job_t g_jobs[8];
#ifdef HAVE_CURL
static int   g_jobseq;
#endif

#ifdef HAVE_CURL
extern const char cacert_start[], cacert_end[];
__asm__(".section .rodata\n.global cacert_start\ncacert_start:\n.incbin \"" CACERT_PATH "\"\n"
        ".global cacert_end\ncacert_end:\n.byte 0\n.text\n");

static void ensure_cacert(void) {
  long long want = (long long)(cacert_end - cacert_start);
  if (file_size(g_cfg.cacert_path) == want) return;
  int fd = open(g_cfg.cacert_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) { write_all(fd, cacert_start, (size_t)want); close(fd); }
}

static size_t cb_file(void *p, size_t s, size_t n, void *u) { return fwrite(p, s, n, (FILE *)u); }
static size_t cb_mem(void *p, size_t s, size_t n, void *u) {
  sb_t *b = u;
  if (b->len > (1 << 20)) return 0;
  sb_add(b, p, s * n);
  return s * n;
}
static int cb_prog(void *u, curl_off_t dt, curl_off_t dn, curl_off_t ut, curl_off_t un) {
  (void)ut; (void)un;
  job_t *j = u;
  if (dt > 0) j->pct = (int)(dn * 100 / dt);
  return j->cancel;
}

static CURL *curl_new(const char *url) {
  CURL *c = curl_easy_init();
  curl_easy_setopt(c, CURLOPT_URL, url);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "CXPS5/" VERSION);
  curl_easy_setopt(c, CURLOPT_CAINFO, g_cfg.cacert_path);
  curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
  return c;
}

static void *job_thread(void *arg) {
  job_t *j = arg;
  char dir[600], tmp[700];
  snprintf(dir, sizeof dir, "%s/%s", g_cfg.mods_dir, j->modid);
  snprintf(tmp, sizeof tmp, "%s/.download", dir);
  FILE *f = fopen(tmp, "wb");
  if (!f) { snprintf(j->msg, sizeof j->msg, "cannot write: %s", strerror(errno)); cpy(j->state, sizeof j->state, "error"); return NULL; }
  CURL *c = curl_new(j->url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_file);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
  curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cb_prog);
  curl_easy_setopt(c, CURLOPT_XFERINFODATA, j);
  curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  fclose(f);
  if (rc != CURLE_OK) {
    snprintf(j->msg, sizeof j->msg, "download failed: %s", curl_easy_strerror(rc));
    cpy(j->state, sizeof j->state, "error");
    unlink(tmp);
    return NULL;
  }
  unsigned char magic[8] = {0};
  FILE *m = fopen(tmp, "rb");
  if (m) { if (fread(magic, 1, 8, m) == 0) magic[0] = 0; fclose(m); }
  pthread_mutex_lock(&g_lock);
  if (!memcmp(magic, "PK\3\4", 4)) {
    char err[120] = "";
    int n = zip_extract(tmp, dir, err, sizeof err);
    if (n > 0) { snprintf(j->msg, sizeof j->msg, "%d bundle(s) added", n); cpy(j->state, sizeof j->state, "done"); }
    else { cpy(j->msg, sizeof j->msg, err); cpy(j->state, sizeof j->state, "error"); }
    unlink(tmp);
  } else if (!memcmp(magic, "UnityFS", 7)) {
    char name[160];
    if (j->file[0]) cpy(name, sizeof name, j->file);
    else {
      char u[512];
      cpy(u, sizeof u, j->url);
      u[strcspn(u, "?#")] = 0;
      char *b = strrchr(u, '/');
      cpy(name, sizeof name, b ? b + 1 : u);
      pct_decode(name, 0);
    }
    char out[800];
    snprintf(out, sizeof out, "%s/%s", dir, name);
    if (valid_bundle_name(name) && rename(tmp, out) == 0) { snprintf(j->msg, sizeof j->msg, "%s added", name); cpy(j->state, sizeof j->state, "done"); }
    else { cpy(j->msg, sizeof j->msg, "bundle has no usable file name, enter one"); cpy(j->state, sizeof j->state, "error"); unlink(tmp); }
  } else {
    cpy(j->msg, sizeof j->msg, "link is not a .bundle or .zip file (Steam Workshop pages are not direct links)");
    cpy(j->state, sizeof j->state, "error");
    unlink(tmp);
  }
  pthread_mutex_unlock(&g_lock);
  return NULL;
}
#else
static void ensure_cacert(void) {}
#endif

// ------------------------------------------------------------------ API handlers
static void api_status(int fd) {
  sb_t s = {0};
  rec_t *st = NULL;
  pthread_mutex_lock(&g_lock);
  int applied = state_load(&st);
  free(st);
  int writable = g_root[0] && access(g_root, W_OK) == 0;
  sb_printf(&s, "{\"version\":\"%s\",\"curl\":%s,\"pin\":%s,\"catalog\":%s,\"game\":{\"found\":%s,\"name\":", VERSION,
#ifdef HAVE_CURL
            "true",
#else
            "false",
#endif
            g_cfg.pin[0] ? "true" : "false", g_cfg.catalog_url[0] ? "true" : "false", g_root[0] ? "true" : "false");
  sb_json(&s, g_gname);
  sb_puts(&s, ",\"titleId\":");
  sb_json(&s, g_title);
  sb_puts(&s, ",\"version\":");
  sb_json(&s, g_gver);
  sb_puts(&s, ",\"root\":");
  sb_json(&s, g_root);
  sb_printf(&s, ",\"bundles\":%d,\"writable\":%s,\"running\":%s},\"applied\":%d}", g_nidx, writable ? "true" : "false",
            game_running() ? "true" : "false", applied);
  pthread_mutex_unlock(&g_lock);
  respond_json(fd, 200, &s);
}

static const char *compat_for(const char *srcpath, const char *name, int tgt, ufs_t *mine, ufs_t *stock, char *stockpath, size_t sn) {
  ufs_read(srcpath, mine);
  memset(stock, 0, sizeof *stock);
  if (tgt < 0) return "nomatch";
  rec_t *st = NULL;
  int    n = state_load(&st);
  stockpath[0] = 0;
  for (int i = 0; i < n; i++)
    if (!strcmp(st[i].target, g_idx[tgt].path)) cpy(stockpath, sn, st[i].backup);  // already swapped: stock is the backup
  free(st);
  if (!stockpath[0]) cpy(stockpath, sn, g_idx[tgt].path);
  ufs_read(stockpath, stock);
  (void)name;
  if (!mine->ok) return "invalid";
  if (!stock->ok) return "unknown";
  return strcmp(mine->engine, stock->engine) == 0 ? "same" : "differs";
}

static void api_mods(int fd) {
  sb_t s = {0};
  pthread_mutex_lock(&g_lock);
  want_t *want = NULL;
  int     nw = build_desired(&want);
  rec_t  *st = NULL;
  int     ns = state_load(&st);
  int     pending = 0;
  for (int i = 0; i < nw; i++) {
    int found = 0;
    for (int j = 0; j < ns; j++)
      if (!strcmp(st[j].name, want[i].name) && !strcmp(st[j].modid, want[i].modid) && st[j].size == want[i].size) found = 1;
    if (!found && find_target(want[i].name) >= 0) pending = 1;
  }
  for (int j = 0; j < ns && !pending; j++) {
    int keep = 0;
    for (int i = 0; i < nw; i++)
      if (!strcmp(st[j].name, want[i].name) && !strcmp(st[j].modid, want[i].modid)) keep = 1;
    if (!keep) pending = 1;
  }
  sb_printf(&s, "{\"pending\":%s,\"mods\":[", pending ? "true" : "false");
  DIR *d = opendir(g_cfg.mods_dir);
  struct dirent *e;
  int first = 1;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.' || !valid_id(e->d_name)) continue;
    meta_t m;
    meta_read(e->d_name, &m);
    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", g_cfg.mods_dir, e->d_name);
    if (!first) sb_puts(&s, ",");
    first = 0;
    sb_puts(&s, "{\"id\":");
    sb_json(&s, m.id);
    sb_puts(&s, ",\"name\":");
    sb_json(&s, m.name);
    sb_puts(&s, ",\"source\":");
    sb_json(&s, m.source);
    sb_printf(&s, ",\"enabled\":%s,\"priority\":%d,\"added\":%ld,\"files\":[", m.enabled ? "true" : "false", m.prio, m.added);
    DIR *md = opendir(dir);
    struct dirent *fe;
    int ff = 1, napplied = 0;
    long long bytes = 0;
    while (md && (fe = readdir(md))) {
      if (!valid_bundle_name(fe->d_name)) continue;
      char src[900], sp[900];
      snprintf(src, sizeof src, "%s/%s", dir, fe->d_name);
      long long sz = file_size(src);
      bytes += sz > 0 ? sz : 0;
      int tgt = find_target(fe->d_name);
      ufs_t mine, stock;
      const char *c = compat_for(src, fe->d_name, tgt, &mine, &stock, sp, sizeof sp);
      int isap = 0;
      for (int j = 0; j < ns; j++)
        if (!strcmp(st[j].name, fe->d_name) && !strcmp(st[j].modid, m.id)) isap = 1;
      napplied += isap;
      if (!ff) sb_puts(&s, ",");
      ff = 0;
      sb_puts(&s, "{\"name\":");
      sb_json(&s, fe->d_name);
      sb_printf(&s, ",\"size\":%lld,\"compat\":\"%s\",\"applied\":%s,\"engine\":", sz, c, isap ? "true" : "false");
      sb_json(&s, mine.engine);
      sb_puts(&s, ",\"stockEngine\":");
      sb_json(&s, stock.engine);
      sb_puts(&s, "}");
    }
    if (md) closedir(md);
    sb_printf(&s, "],\"bytes\":%lld,\"applied\":%d}", bytes, napplied);
  }
  if (d) closedir(d);
  sb_puts(&s, "]}");
  free(want);
  free(st);
  pthread_mutex_unlock(&g_lock);
  respond_json(fd, 200, &s);
}

static void api_put_file(req_t *r, const char *id, const char *name) {
  if (!valid_id(id) || !valid_bundle_name(name)) return respond_err(r->fd, 400, "bad mod id or file name (only .bundle files are accepted)");
  char modname[100] = "", src[200] = "upload";
  qget(r->query, "modname", modname, sizeof modname);
  pthread_mutex_lock(&g_lock);
  ensure_mod(id, modname, src);
  pthread_mutex_unlock(&g_lock);
  char tmp[800], out[800];
  snprintf(out, sizeof out, "%s/%s/%s", g_cfg.mods_dir, id, name);
  snprintf(tmp, sizeof tmp, "%s.part", out);
  int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (ofd < 0) return respond_err(r->fd, 400, strerror(errno));
  long long n = body_to_fd(r, ofd);
  int cl = close(ofd);
  if (n < 0 || cl != 0) {
    unlink(tmp);
    return respond_err(r->fd, 400, "upload interrupted or disk full");
  }
  ufs_t u;
  ufs_read(tmp, &u);
  if (!u.ok) {
    unlink(tmp);
    return respond_err(r->fd, 400, "not a Unity bundle (missing UnityFS header)");
  }
  rename(tmp, out);
  LOG("mod %s: stored %s (%lld bytes, engine %s)", id, name, n, u.engine);
  sb_t s = {0};
  sb_puts(&s, "{\"ok\":true}");
  respond_json(r->fd, 200, &s);
}

static void api_put_zip(req_t *r, const char *id) {
  if (!valid_id(id)) return respond_err(r->fd, 400, "bad mod id");
  char modname[100] = "";
  qget(r->query, "modname", modname, sizeof modname);
  pthread_mutex_lock(&g_lock);
  ensure_mod(id, modname, "zip upload");
  pthread_mutex_unlock(&g_lock);
  char dir[600], tmp[700];
  snprintf(dir, sizeof dir, "%s/%s", g_cfg.mods_dir, id);
  snprintf(tmp, sizeof tmp, "%s/.upload.zip", dir);
  int ofd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (ofd < 0) return respond_err(r->fd, 400, strerror(errno));
  long long n = body_to_fd(r, ofd);
  close(ofd);
  if (n < 0) { unlink(tmp); return respond_err(r->fd, 400, "upload interrupted or disk full"); }
  char err[160] = "";
  pthread_mutex_lock(&g_lock);
  int got = zip_extract(tmp, dir, err, sizeof err);
  pthread_mutex_unlock(&g_lock);
  unlink(tmp);
  if (got <= 0) return respond_err(r->fd, 400, err[0] ? err : "nothing extracted");
  sb_t s = {0};
  sb_printf(&s, "{\"ok\":true,\"files\":%d}", got);
  respond_json(r->fd, 200, &s);
}

static void api_mod_update(req_t *r, const char *id) {
  char *b = body_alloc(r, 8192);
  if (!b || !valid_id(id)) { free(b); return respond_err(r->fd, 400, "bad request"); }
  pthread_mutex_lock(&g_lock);
  meta_t m;
  meta_read(id, &m);
  char v[120];
  if (json_get(b, "name", v, sizeof v) && v[0]) cpy(m.name, sizeof m.name, v);
  if (json_get(b, "enabled", v, sizeof v)) m.enabled = !strcmp(v, "true") || !strcmp(v, "1");
  if (json_get(b, "priority", v, sizeof v)) m.prio = atoi(v);
  int rc = meta_write(&m);
  pthread_mutex_unlock(&g_lock);
  free(b);
  if (rc) return respond_err(r->fd, 400, "could not save");
  sb_t s = {0};
  sb_puts(&s, "{\"ok\":true}");
  respond_json(r->fd, 200, &s);
}

static void api_mod_delete(int fd, const char *id) {
  if (!valid_id(id)) return respond_err(fd, 400, "bad mod id");
  pthread_mutex_lock(&g_lock);
  char dir[600];
  snprintf(dir, sizeof dir, "%s/%s", g_cfg.mods_dir, id);
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.' && (!e->d_name[1] || e->d_name[1] == '.')) continue;
    char p[900];
    snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
    unlink(p);
  }
  if (d) closedir(d);
  rmdir(dir);
  pthread_mutex_unlock(&g_lock);
  LOG("mod %s deleted", id);
  sb_t s = {0};
  sb_puts(&s, "{\"ok\":true}");
  respond_json(fd, 200, &s);
}

static int restore_rec(const rec_t *r, sb_t *errs) {
  if (access(r->backup, F_OK) != 0) {
    sb_puts(errs, errs->len ? "," : "");
    sb_printf(errs, "\"backup missing for %s, verify the game files\"", r->name);
    return -1;
  }
  if (copy_file(r->backup, r->target) != 0) {
    sb_puts(errs, errs->len ? "," : "");
    sb_printf(errs, "\"could not restore %s: %s\"", r->name, strerror(errno));
    return -1;
  }
  unlink(r->backup);
  return 0;
}

static void add_name(sb_t *list, const char *n) {
  if (list->len) sb_puts(list, ",");
  sb_json(list, n);
}

static void api_apply(req_t *r, int restore_all) {
  char *b = body_alloc(r, 4096);
  char force[8] = "";
  if (b) json_get(b, "force", force, sizeof force);
  free(b);
  if (game_running() && strcmp(force, "true")) return respond_err(r->fd, 409, "The game is running. Close CarX Street first, swapping files under a running game can corrupt it.");
  pthread_mutex_lock(&g_lock);
  game_scan();
  if (!g_root[0]) { pthread_mutex_unlock(&g_lock); return respond_err(r->fd, 400, "CarX Street was not found. Set game_root in config.ini."); }
  mkdirs(g_cfg.backup_dir);
  rec_t *st = NULL;
  int    ns = state_load(&st);
  want_t *want = NULL;
  int    nw = restore_all ? 0 : build_desired(&want);
  rec_t  *keep = calloc((size_t)(ns + nw + 1), sizeof *keep);
  int     nk = 0, nrest = 0, napp = 0;
  sb_t    errs = {0}, unmatched = {0}, applied = {0};

  // 1. put back anything no longer wanted (or everything on a full restore)
  for (int j = 0; j < ns; j++) {
    int wanted = 0;
    for (int i = 0; i < nw; i++)
      if (!strcmp(st[j].name, want[i].name)) wanted = 1;
    if (wanted) { keep[nk++] = st[j]; continue; }
    if (restore_rec(&st[j], &errs) == 0) nrest++;
    else keep[nk++] = st[j];  // keep the record so nothing is forgotten
  }
  // 2. swap in every wanted bundle
  for (int i = 0; i < nw; i++) {
    int tgt = find_target(want[i].name);
    if (tgt < 0) { add_name(&unmatched, want[i].name); continue; }
    int cur = -1;
    for (int k = 0; k < nk; k++)
      if (!strcmp(keep[k].name, want[i].name)) cur = k;
    if (cur >= 0 && !strcmp(keep[cur].modid, want[i].modid) && keep[cur].size == want[i].size && file_size(g_idx[tgt].path) == want[i].size) {
      napp++;
      continue;  // already in place
    }
    rec_t rec;
    memset(&rec, 0, sizeof rec);
    cpy(rec.name, sizeof rec.name, g_idx[tgt].name);
    cpy(rec.modid, sizeof rec.modid, want[i].modid);
    cpy(rec.target, sizeof rec.target, g_idx[tgt].path);
    rec.size = want[i].size;
    if (cur >= 0) cpy(rec.backup, sizeof rec.backup, keep[cur].backup);
    else backup_path_for(g_idx[tgt].path, g_idx[tgt].name, rec.backup, sizeof rec.backup);
    if (access(rec.backup, F_OK) != 0) {
      if (copy_file(g_idx[tgt].path, rec.backup) != 0 || file_size(rec.backup) != g_idx[tgt].size) {
        unlink(rec.backup);
        sb_puts(&errs, errs.len ? "," : "");
        sb_printf(&errs, "\"backup of %s failed: %s\"", rec.name, strerror(errno));
        continue;
      }
    }
    if (copy_file(want[i].src, rec.target) != 0) {
      sb_puts(&errs, errs.len ? "," : "");
      sb_printf(&errs, "\"could not write %s: %s\"", rec.name, strerror(errno));
      continue;
    }
    if (cur >= 0) keep[cur] = rec;
    else keep[nk++] = rec;
    add_name(&applied, rec.name);
    napp++;
    LOG("applied %s from %s", rec.name, rec.modid);
  }
  state_save(keep, nk);
  g_last_apply = time(NULL);
  pthread_mutex_unlock(&g_lock);

  sb_t s = {0};
  sb_printf(&s, "{\"ok\":%s,\"applied\":%d,\"restored\":%d,\"unmatched\":[%s],\"changed\":[%s],\"errors\":[%s]}", errs.len ? "false" : "true", napp, nrest,
            unmatched.p ? unmatched.p : "", applied.p ? applied.p : "", errs.p ? errs.p : "");
  free(unmatched.p); free(applied.p); free(errs.p); free(st); free(want); free(keep);
  respond_json(r->fd, 200, &s);
}


// ------------------------------------------------------------------ diagnostics
// Plain-text findings, one per line ("ERROR ...", "WARN ...", "INFO ..."). Caller holds g_lock.
static void diag_findings(sb_t *o) {
  if (!g_root[0]) sb_puts(o, "ERROR CarX Street was not found. Set game_root in config.ini.\n");
  else {
    if (access(g_root, W_OK) != 0) sb_puts(o, "ERROR The game folder is not writable, Apply will fail.\n");
    if (g_nidx == 0) sb_puts(o, "ERROR No .bundle files found under the game folder, the bundle layout may differ.\n");
  }
  if (game_running()) sb_puts(o, "INFO The game is running now. Close it before applying.\n");
  want_t *want = NULL;
  int     nw = build_desired(&want);
  for (int i = 0; i < nw; i++) {
    int tgt = find_target(want[i].name);
    if (tgt < 0) {
      sb_printf(o, "INFO %s (mod %s) has no matching game file and will be skipped.\n", want[i].name, want[i].modid);
      continue;
    }
    ufs_t mine, stock;
    char  sp[900];
    const char *c = compat_for(want[i].src, want[i].name, tgt, &mine, &stock, sp, sizeof sp);
    if (!strcmp(c, "invalid")) sb_printf(o, "ERROR %s (mod %s) is not a valid Unity bundle.\n", want[i].name, want[i].modid);
    else if (!strcmp(c, "differs"))
      sb_printf(o, "WARN %s (mod %s) was built with Unity %s but the game uses %s. It will probably fail to load or look broken.\n", want[i].name,
                want[i].modid, mine.engine, stock.engine);
  }
  free(want);
  rec_t *st = NULL;
  int    ns = state_load(&st);
  for (int i = 0; i < ns; i++)
    if (access(st[i].backup, F_OK) != 0) sb_printf(o, "ERROR The backup of %s is missing, restoring it is not possible. Verify or reinstall the game files.\n", st[i].name);
  free(st);
  if (g_runnote[0]) sb_printf(o, "%s\n", g_runnote);
}

static void api_diag(int fd) {
  sb_t f = {0}, s = {0};
  pthread_mutex_lock(&g_lock);
  diag_findings(&f);
  pthread_mutex_unlock(&g_lock);
  sb_puts(&s, "{\"findings\":[");
  int first = 1;
  for (char *l = f.p, *nl; l && *l; l = nl ? nl + 1 : NULL) {
    nl = strchr(l, '\n');
    if (nl) *nl = 0;
    if (*l) { if (!first) sb_puts(&s, ","); first = 0; sb_json(&s, l); }
    if (!nl) break;
  }
  sb_puts(&s, "]}");
  free(f.p);
  respond_json(fd, 200, &s);
}

static void api_report(int fd) {
  sb_t o = {0};
  char ts[32];
  time_t now = time(NULL);
  struct tm tmv;
  gmtime_r(&now, &tmv);
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S UTC", &tmv);
  pthread_mutex_lock(&g_lock);
  sb_printf(&o, "CXPS5 diagnostic report\nCXPS5 %s, %s\n\n== Game ==\nName: %s\nTitle id: %s\nVersion: %s\nFolder: %s\nBundles indexed: %d\nWritable: %s\nRunning: %s\n",
            VERSION, ts, g_gname, g_title, g_gver[0] ? g_gver : "(unknown)", g_root, g_nidx, g_root[0] && access(g_root, W_OK) == 0 ? "yes" : "no",
            game_running() ? "yes" : "no");
  sb_puts(&o, "\n== Findings ==\n");
  size_t before = o.len;
  diag_findings(&o);
  if (o.len == before) sb_puts(&o, "No problems detected.\n");
  rec_t *st = NULL;
  int    ns = state_load(&st);
  sb_printf(&o, "\n== Files swapped into the game (%d) ==\n", ns);
  for (int i = 0; i < ns; i++) sb_printf(&o, "%s  <-  mod %s  (%lld bytes)\n", st[i].name, st[i].modid, st[i].size);
  free(st);
  sb_puts(&o, "\n== Mods ==\n");
  DIR *d = opendir(g_cfg.mods_dir);
  struct dirent *e;
  while (d && (e = readdir(d))) {
    if (e->d_name[0] == '.' || !valid_id(e->d_name)) continue;
    meta_t m;
    meta_read(e->d_name, &m);
    sb_printf(&o, "%s [%s] enabled=%d priority=%d source=%s\n", m.name, m.id, m.enabled, m.prio, m.source);
    char dir[600];
    snprintf(dir, sizeof dir, "%s/%s", g_cfg.mods_dir, e->d_name);
    DIR *md = opendir(dir);
    struct dirent *fe;
    while (md && (fe = readdir(md))) {
      if (!valid_bundle_name(fe->d_name)) continue;
      char src[900], sp[900];
      snprintf(src, sizeof src, "%s/%s", dir, fe->d_name);
      ufs_t mine, stock;
      const char *c = compat_for(src, fe->d_name, find_target(fe->d_name), &mine, &stock, sp, sizeof sp);
      sb_printf(&o, "  %s  %lld bytes  %s  mod-unity=%s game-unity=%s\n", fe->d_name, file_size(src), c, mine.engine, stock.engine);
    }
    if (md) closedir(md);
  }
  if (d) closedir(d);
  pthread_mutex_unlock(&g_lock);
  sb_puts(&o, "\n== Log (most recent) ==\n");
  FILE *f = fopen(g_cfg.log_path, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f), from = n > 200000 ? n - 200000 : 0;
    fseek(f, from, SEEK_SET);
    char buf[4096];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0) sb_add(&o, buf, k);
    fclose(f);
  }
  respond_h(fd, 200, "text/plain; charset=utf-8", "Content-Disposition: attachment; filename=\"cxps5-report.txt\"\r\n", o.p ? o.p : "", o.len);
  free(o.p);
}

// Watches for the game starting and stopping. A short run right after files were swapped in is the
// usual sign of a crash or an incompatible bundle, so it is written to the log and the findings.
static void *game_watch(void *arg) {
  (void)arg;
  int    was = 0;
  time_t start = 0;
  int    nmods = 0;
  for (;;) {
    sleep(2);
    int now = game_running();
    if (now && !was) {
      start = time(NULL);
      pthread_mutex_lock(&g_lock);
      rec_t *st = NULL;
      nmods = state_load(&st);
      LOG("game started with %d modded file(s)", nmods);
      for (int i = 0; i < nmods && i < 40; i++) LOG("  modded: %s (mod %s)", st[i].name, st[i].modid);
      free(st);
      pthread_mutex_unlock(&g_lock);
    } else if (!now && was) {
      long d = (long)(time(NULL) - start);
      pthread_mutex_lock(&g_lock);
      if (nmods > 0 && d < 90) {
        snprintf(g_runnote, sizeof g_runnote, "WARN The game closed after %lds with %d modded file(s). That looks like a crash or an incompatible bundle. Disable the newest mod, or use Restore originals.", d, nmods);
        LOG("%s", g_runnote);
      } else {
        g_runnote[0] = 0;
        LOG("game exited after %lds (%d modded file(s))", d, nmods);
      }
      pthread_mutex_unlock(&g_lock);
    }
    was = now;
  }
  return NULL;
}

static void api_scan(int fd) {
  pthread_mutex_lock(&g_lock);
  game_scan();
  pthread_mutex_unlock(&g_lock);
  api_status(fd);
}

static void api_log(int fd) {
  sb_t s = {0};
  FILE *f = fopen(g_cfg.log_path, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    long from = n > 6000 ? n - 6000 : 0;
    fseek(f, from, SEEK_SET);
    char buf[6001];
    size_t k = fread(buf, 1, 6000, f);
    buf[k] = 0;
    fclose(f);
    sb_puts(&s, "{\"log\":");
    sb_json(&s, buf);
    sb_puts(&s, "}");
  } else {
    sb_puts(&s, "{\"log\":\"\"}");
  }
  respond_json(fd, 200, &s);
}

static void api_jobs(int fd) {
  sb_t s = {0};
  sb_puts(&s, "{\"jobs\":[");
  int first = 1;
  for (int i = 0; i < 8; i++) {
    if (!g_jobs[i].used) continue;
    if (!first) sb_puts(&s, ",");
    first = 0;
    sb_printf(&s, "{\"id\":%d,\"pct\":%d,\"state\":\"%s\",\"name\":", g_jobs[i].id, g_jobs[i].pct, g_jobs[i].state);
    sb_json(&s, g_jobs[i].modname);
    sb_puts(&s, ",\"msg\":");
    sb_json(&s, g_jobs[i].msg);
    sb_puts(&s, "}");
  }
  sb_puts(&s, "]}");
  respond_json(fd, 200, &s);
}

static void api_fetch(req_t *r) {
#ifdef HAVE_CURL
  char *b = body_alloc(r, 8192);
  if (!b) return respond_err(r->fd, 400, "bad request");
  job_t j;
  memset(&j, 0, sizeof j);
  json_get(b, "url", j.url, sizeof j.url);
  json_get(b, "id", j.modid, sizeof j.modid);
  json_get(b, "name", j.modname, sizeof j.modname);
  json_get(b, "file", j.file, sizeof j.file);
  free(b);
  if (strncmp(j.url, "http://", 7) && strncmp(j.url, "https://", 8)) return respond_err(r->fd, 400, "link must start with http:// or https://");
  if (!valid_id(j.modid)) return respond_err(r->fd, 400, "bad mod id");
  if (j.file[0] && !valid_bundle_name(j.file)) return respond_err(r->fd, 400, "file name must end in .bundle");
  pthread_mutex_lock(&g_lock);
  int slot = -1;
  for (int i = 0; i < 8; i++)
    if (!g_jobs[i].used || strcmp(g_jobs[i].state, "running")) { slot = i; break; }
  if (slot >= 0) {
    ensure_mod(j.modid, j.modname, j.url);
    j.used = 1;
    j.id = ++g_jobseq;
    cpy(j.state, sizeof j.state, "running");
    g_jobs[slot] = j;
  }
  pthread_mutex_unlock(&g_lock);
  if (slot < 0) return respond_err(r->fd, 409, "too many downloads running");
  pthread_t th;
  pthread_create(&th, NULL, job_thread, &g_jobs[slot]);
  pthread_detach(th);
  sb_t s = {0};
  sb_printf(&s, "{\"ok\":true,\"job\":%d}", g_jobs[slot].id);
  respond_json(r->fd, 200, &s);
#else
  respond_err(r->fd, 501, "this build has no libcurl, upload files from your device instead");
#endif
}

static void api_catalog(int fd) {
#ifdef HAVE_CURL
  if (!g_cfg.catalog_url[0]) { sb_t s = {0}; sb_puts(&s, "{\"items\":[]}"); return respond_json(fd, 200, &s); }
  sb_t out = {0};
  CURL *c = curl_new(g_cfg.catalog_url);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, cb_mem);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
  CURLcode rc = curl_easy_perform(c);
  curl_easy_cleanup(c);
  if (rc != CURLE_OK || !out.p) { free(out.p); return respond_err(fd, 400, "could not load the catalog"); }
  respond(fd, 200, "application/json", out.p, out.len);
  free(out.p);
#else
  sb_t s = {0};
  sb_puts(&s, "{\"items\":[]}");
  respond_json(fd, 200, &s);
#endif
}

// ------------------------------------------------------------------ connection + routing
static void handle(req_t *r) {
  if (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html")) return respond(r->fd, 200, "text/html; charset=utf-8", (const char *)UI_HTML, UI_HTML_LEN);
  if (!strcmp(r->path, "/favicon.ico")) return respond(r->fd, 204, "text/plain", "", 0);
  if (!strcmp(r->method, "POST") && !strcmp(r->path, "/api/shutdown") && r->loopback) {
    respond(r->fd, 200, "application/json", "{\"ok\":true}", 11);
    LOG("shutdown requested by a newer instance");
    _exit(0);
  }
  if (strncmp(r->path, "/api/", 5)) return respond_err(r->fd, 404, "not found");
  if (g_cfg.pin[0] && strcmp(r->pin, g_cfg.pin)) {
    sb_t s = {0};
    sb_puts(&s, "{\"error\":\"PIN required\",\"pin\":true}");
    return respond_json(r->fd, 401, &s);
  }
  const char *p = r->path + 5;
  int get = !strcmp(r->method, "GET"), post = !strcmp(r->method, "POST"), put = !strcmp(r->method, "PUT"), del = !strcmp(r->method, "DELETE");
  if (get && !strcmp(p, "status")) return api_status(r->fd);
  if (get && !strcmp(p, "mods")) return api_mods(r->fd);
  if (get && !strcmp(p, "log")) return api_log(r->fd);
  if (get && !strcmp(p, "diag")) return api_diag(r->fd);
  if (get && !strcmp(p, "report")) return api_report(r->fd);
  if (get && !strcmp(p, "jobs")) return api_jobs(r->fd);
  if (get && !strcmp(p, "catalog")) return api_catalog(r->fd);
  if (post && !strcmp(p, "scan")) return api_scan(r->fd);
  if (post && !strcmp(p, "apply")) return api_apply(r, 0);
  if (post && !strcmp(p, "restore")) return api_apply(r, 1);
  if (post && !strcmp(p, "fetch")) return api_fetch(r);
  if (!strncmp(p, "mods/", 5)) {
    char id[80];
    const char *q = p + 5;
    size_t l = strcspn(q, "/");
    if (l >= sizeof id) return respond_err(r->fd, 400, "bad id");
    memcpy(id, q, l);
    id[l] = 0;
    const char *rest = q + l;
    if (!*rest && post) return api_mod_update(r, id);
    if (!*rest && del) return api_mod_delete(r->fd, id);
    if (put && !strncmp(rest, "/files/", 7)) return api_put_file(r, id, rest + 7);
    if (put && !strcmp(rest, "/zip")) return api_put_zip(r, id);
  }
  respond_err(r->fd, 404, "unknown endpoint");
}

static void *conn_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  struct timeval tv = {60, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  req_t *r = calloc(1, sizeof *r);
  r->fd = fd;
  struct sockaddr_in pa;
  socklen_t pl = sizeof pa;
  if (getpeername(fd, (struct sockaddr *)&pa, &pl) == 0) r->loopback = (ntohl(pa.sin_addr.s_addr) >> 24) == 127;
  char *hend = NULL;
  while (r->buflen < (int)sizeof r->buf - 1) {
    ssize_t n = recv(fd, r->buf + r->buflen, sizeof r->buf - 1 - (size_t)r->buflen, 0);
    if (n <= 0) goto done;
    r->buflen += (int)n;
    r->buf[r->buflen] = 0;
    if ((hend = strstr(r->buf, "\r\n\r\n"))) break;
  }
  if (!hend) goto done;
  r->bodyoff = (int)(hend - r->buf) + 4;
  *hend = 0;
  char target[1300] = "";
  if (sscanf(r->buf, "%7s %1299s", r->method, target) != 2) goto done;
  char *qm = strchr(target, '?');
  if (qm) { *qm++ = 0; cpy(r->query, sizeof r->query, qm); }
  cpy(r->path, sizeof r->path, target);
  pct_decode(r->path, 0);
  for (char *line = strstr(r->buf, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
    line += 2;
    if (!strncasecmp(line, "Content-Length:", 15)) r->clen = atoll(line + 15);
    else if (!strncasecmp(line, "X-CXPS5-PIN:", 12)) {
      const char *v = line + 12;
      while (*v == ' ') v++;
      size_t l = strcspn(v, "\r\n");
      if (l >= sizeof r->pin) l = sizeof r->pin - 1;
      memcpy(r->pin, v, l);
      r->pin[l] = 0;
    }
  }
  handle(r);
done:
  free(r);
  close(fd);
  return NULL;
}

static void config_load(const char *path) {
  FILE *f = fopen(path, "r");
  char  line[700];
  while (f && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    char *eq = strchr(line, '=');
    if (line[0] == '#' || !eq) continue;
    *eq++ = 0;
    char *k = line, *v = eq;
    while (*v == ' ') v++;
    for (char *e = k + strlen(k); e > k && e[-1] == ' '; e--) e[-1] = 0;
    if (!strcmp(k, "port")) g_cfg.port = atoi(v);
    else if (!strcmp(k, "game_root")) cpy(g_cfg.game_root, sizeof g_cfg.game_root, v);
    else if (!strcmp(k, "catalog_url")) cpy(g_cfg.catalog_url, sizeof g_cfg.catalog_url, v);
    else if (!strcmp(k, "pin")) cpy(g_cfg.pin, sizeof g_cfg.pin, v);
  }
  if (f) fclose(f);
  size_t l = strlen(g_cfg.game_root);
  while (l > 1 && g_cfg.game_root[l - 1] == '/') g_cfg.game_root[--l] = 0;
}

static void stop_previous(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  struct timeval tv = {2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
    static const char REQ[] = "POST /api/shutdown HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    char buf[128];
    if (send(fd, REQ, sizeof REQ - 1, 0) > 0) (void)recv(fd, buf, sizeof buf, 0);
  }
  close(fd);
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);
  const char *data = getenv("CXPS5_DATA");
  cpy(g_cfg.data_dir, sizeof g_cfg.data_dir, data && *data ? data : DEFAULT_DATA);
  g_cfg.port = 8090;
  char cfgpath[300];
  snprintf(cfgpath, sizeof cfgpath, "%s/config.ini", g_cfg.data_dir);
  if (argc > 1) cpy(cfgpath, sizeof cfgpath, argv[1]);
  config_load(cfgpath);
  snprintf(g_cfg.mods_dir, sizeof g_cfg.mods_dir, "%s/mods", g_cfg.data_dir);
  snprintf(g_cfg.backup_dir, sizeof g_cfg.backup_dir, "%s/backup", g_cfg.data_dir);
  snprintf(g_cfg.state_path, sizeof g_cfg.state_path, "%s/applied.tsv", g_cfg.data_dir);
  snprintf(g_cfg.log_path, sizeof g_cfg.log_path, "%s/cxps5.log", g_cfg.data_dir);
  snprintf(g_cfg.cacert_path, sizeof g_cfg.cacert_path, "%s/cacert.pem", g_cfg.data_dir);
  mkdirs(g_cfg.mods_dir);
  mkdirs(g_cfg.backup_dir);
  if (file_size(g_cfg.log_path) > 2 * 1024 * 1024) { char old[400]; snprintf(old, sizeof old, "%s.1", g_cfg.log_path); rename(g_cfg.log_path, old); }
  g_logf = fopen(g_cfg.log_path, "a");
  LOG("CXPS5 %s starting, data %s, port %d", VERSION, g_cfg.data_dir, g_cfg.port);
#ifdef HAVE_CURL
  curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
  ensure_cacert();
  pthread_mutex_lock(&g_lock);
  game_scan();
  pthread_mutex_unlock(&g_lock);

  int ls = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in sa = {0};
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)g_cfg.port);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  for (int attempt = 0;; attempt++) {
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) == 0) break;
    if (errno != EADDRINUSE || attempt >= 50) { LOG("cannot bind port %d: %s", g_cfg.port, strerror(errno)); notify("CXPS5: port in use"); return 1; }
    if (attempt == 0) stop_previous(g_cfg.port);
    usleep(100 * 1000);
  }
  listen(ls, 16);
  { pthread_t wt; if (pthread_create(&wt, NULL, game_watch, NULL) == 0) pthread_detach(wt); }
  char msg[160];
  snprintf(msg, sizeof msg, "CXPS5 %s ready: open http://<PS5 IP>:%d in a browser", VERSION, g_cfg.port);
  LOG("%s", msg);
  notify(msg);
  for (;;) {
    int c = accept(ls, NULL, NULL);
    if (c < 0) { if (errno == EINTR) continue; usleep(50000); continue; }
    pthread_t th;
    if (pthread_create(&th, NULL, conn_thread, (void *)(intptr_t)c) == 0) pthread_detach(th);
    else close(c);
  }
}
