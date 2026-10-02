/* SA Music Player mod - lista de reproduccion por directorio (ver playlist.h). */
#ifndef RADIO_PLAYLIST_H
#define RADIO_PLAYLIST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RadioPlaylist RadioPlaylist;

/* Escanea `dir` y guarda los ficheros regulares que hay dentro, ordenados por
   nombre para que la reproduccion sea estable entre arranques.
   NO filtra por extension: el extractor de la plataforma decide si un fichero
   trae pista de audio, y asi una lista no se queda obsoleta el dia que aparezca
   un formato raro. Un filtro por extension seria codigo que hay que mantener y
   que ademas rechazaria .opus o .mka con la misma facilidad que un .mp3.
   Ocultos (punto inicial) y subdirectorios se saltan. */
RadioPlaylist* radio_playlist_new(const char* dir);
void radio_playlist_free(RadioPlaylist* p);

int         radio_playlist_count(const RadioPlaylist* p);
/* Ruta completa del elemento `i`, o NULL si se pasa de rango. */
const char* radio_playlist_path(const RadioPlaylist* p, int i);
/* Solo el nombre de fichero, para logs. */
const char* radio_playlist_name(const RadioPlaylist* p, int i);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_PLAYLIST_H */
