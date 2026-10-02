/* SA Music Player mod - decoder de fichero local con la plataforma Android
   (AMediaExtractor + AMediaCodec, libmediandk, API 21+).

   Por que esto y no un decodificador propio: el telefono ya trae decodificadores
   para todos los formatos que soporta su hardware, asi que no hay que vendorizar
   ni una linea de stb_vorbis/dr_flac/minimp4. El .so no crece.

   Dos trampas reales, medidas en dispositivo (no en la doc):
   1. AMediaExtractor_setDataSource(ruta) FALLA con -10002 porque construye un
      data source que tambien habla HTTP y eso exige un hilo de Java:
        E NdkMediaDataSource: http service must be created from Java thread
      Aqui se usa setDataSourceFd, que solo envuelve un fd y no tiene esa
      dependencia. Para un mod nativo en el proceso del juego es la unica via.
   2. El decodificador REMUESTREA. Un .m4a de 22050 Hz entrega 44100 Hz, asi que
      el formato de salida se toma de AMediaCodec_getOutputFormat() cuando
      cambia, jamas del track de origen: si no, ese fichero suena al doble de
      rapido. */
#ifndef RADIO_MEDDEC_H
#define RADIO_MEDDEC_H

#include <stddef.h>
#include <stdint.h>

#include "decode.h"   /* RadioFormat */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RadioMeddec RadioMeddec;

/* Abre `path` y deja el decodificador listo. La pista de audio es la PRIMERA
   con mime de audio: un .m4a de videoclip trae antes una pista video/avc y
   decodificarla daria ruido. NO filtra por extension; que el extractor diga si
   hay audio es mas robusto que mantener una lista de extensiones.
   Devuelve NULL si no hay decodificador para el mime o no se pudo configurar;
   en ese caso escribe el motivo en `why` (si no es NULL), porque un "no se pudo
   abrir" sin decir por que no sirve de nada en un log del juego. */
RadioMeddec* radio_meddec_open(const char* path, char* why, size_t why_len);
void radio_meddec_close(RadioMeddec* d);

/* Lee PCM decodificado. Devuelve muestras int16 escritas, 0 al final de la
   pista, -1 en error. `fmt` se rellena con el formato REAL de salida del
   decodificador (no el del fichero).
   No bloquea: decodifica solo lo que cabe en `out`, asi el ritmo lo sigue
   imponiendo el que escribe en el ring PCM. */
int radio_meddec_read(RadioMeddec* d, int16_t* out, size_t out_capacity,
                      RadioFormat* fmt);

/* Ultimo error (texto estatico, no se libera). "" si no hubo. */
const char* radio_meddec_err(const RadioMeddec* d);
/* mime del track de audio ("audio/flac", "audio/mp4a-latm", ...). */
const char* radio_meddec_mime(const RadioMeddec* d);
/* 1 si la pista se decodifico entera, 0 si se corto por error. */
int radio_meddec_done(const RadioMeddec* d);

#ifdef __cplusplus
}
#endif
#endif /* RADIO_MEDDEC_H */
