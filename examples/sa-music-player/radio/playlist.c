/* SA Music Player mod - lista de reproduccion por directorio (ver playlist.h). */
#include "playlist.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

struct RadioPlaylist {
    char** items;
    int    count;
    int    cap;
};

/* qsort no admite comparar con contexto, y comparar rutas completas haria que
   "sub/b.mp3" ordenase antes que "a.mp3" solo por el separador. Ordenamos por
   el nombre de fichero, que es lo que el usuario ve. */
static int cmp_name(const void* a, const void* b)
{
    const char* const* x = (const char* const*)a;
    const char* const* y = (const char* const*)b;
    const char* nx = strrchr(*x, '/');
    const char* ny = strrchr(*y, '/');
    return strcmp(nx ? nx + 1 : *x, ny ? ny + 1 : *y);
}

static int push(RadioPlaylist* p, const char* path)
{
    if(p->count == p->cap)
    {
        int ncap = p->cap ? p->cap * 2 : 16;
        char** ni = (char**)realloc(p->items, (size_t)ncap * sizeof(char*));
        if(!ni) return -1;
        p->items = ni;
        p->cap   = ncap;
    }
    p->items[p->count] = strdup(path);
    if(!p->items[p->count]) return -1;
    p->count++;
    return 0;
}

RadioPlaylist* radio_playlist_new(const char* dir)
{
    RadioPlaylist* p;
    DIR* d;
    struct dirent* e;
    size_t dlen;

    if(!dir || !*dir) return NULL;
    p = (RadioPlaylist*)calloc(1, sizeof(*p));
    if(!p) return NULL;

    d = opendir(dir);
    if(!d) { free(p); return NULL; }

    dlen = strlen(dir);
    while((e = readdir(d)) != NULL)
    {
        char path[1024];
        struct stat st;

        /* . y .. y cualquier oculto (.nomedia, .thumbnails) */
        if(e->d_name[0] == '.') continue;
        if(dlen + 1 + strlen(e->d_name) + 1 > sizeof path) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if(stat(path, &st) != 0) continue;
        if(!S_ISREG(st.st_mode)) continue;   /* los subdirectorios se saltan */
        if(push(p, path) != 0) { radio_playlist_free(p); closedir(d); return NULL; }
    }
    closedir(d);

    if(p->count > 1) qsort(p->items, (size_t)p->count, sizeof(char*), cmp_name);
    return p;
}

void radio_playlist_free(RadioPlaylist* p)
{
    int i;
    if(!p) return;
    for(i = 0; i < p->count; i++) free(p->items[i]);
    free(p->items);
    free(p);
}

int radio_playlist_count(const RadioPlaylist* p) { return p ? p->count : 0; }

const char* radio_playlist_path(const RadioPlaylist* p, int i)
{
    if(!p || i < 0 || i >= p->count) return NULL;
    return p->items[i];
}

const char* radio_playlist_name(const RadioPlaylist* p, int i)
{
    const char* s;
    if(!p || i < 0 || i >= p->count) return NULL;
    s = strrchr(p->items[i], '/');
    return s ? s + 1 : p->items[i];
}
