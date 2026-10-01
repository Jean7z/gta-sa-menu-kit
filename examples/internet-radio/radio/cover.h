/* Internet Radio mod - portada de la cancion y titulo por JNI (ver cover.h). */
#ifndef RADIO_COVER_H
#define RADIO_COVER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*  1 = hay portada, y out_path tiene la ruta del fichero cacheado que se le
        pasa a AddButton como icono.
     0 = este fichero no trae portada, o no se pudo leer. NO es un error.
    -1 = la API de Java no esta disponible (no se pudo adjuntar el hilo, o
        falta MediaMetadataRetriever). Esto SI es un error y conviene loguearlo.

   out_title es opcional (puede ser NULL) y recibe el titulo del metadata ya
   saneado a ASCII imprimible, porque el motor solo dibuja 0x20..0x7E.
   Los bytes se leen del array de Java con GetByteArrayRegion y se liberan en el
   acto, asi que no queda ningun byte[] de Java sostenido por este codigo. */
int radio_cover_extract(void* jni_env, const char* track_path, const char* cache_dir,
                        char* out_path, size_t out_len,
                        char* out_title, size_t title_len);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_COVER_H */
