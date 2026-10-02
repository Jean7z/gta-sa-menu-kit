/* SA Music Player mod - portada y titulo de una pista (ver cover.h).
   MediaMetadataRetriever es la clase de Java que trae el sistema: ya sabe leer
   la caratula de un FLAC, un MP3 con ID3v2 o un M4A con covr, y no hay que
   vendorizar ni un parser de contenedor aqui.
   POR QUE JNI Y NO C: el NDK no expone AMediaMetadataRetriever. En r29 los
   headers de <media>/ son NdkImage, NdkImageReader, NdkMediaCodec,
   NdkMediaCodecInfo, NdkMediaCodecStore, NdkMediaCrypto, NdkMediaDataSource,
   NdkMediaDrm, NdkMediaError, NdkMediaExtractor, NdkMediaFormat y NdkMediaMuxer.
   Ni MediaMetadataRetriever ni MediaPlayer. La unica via es la clase Java, y el
   mod ya trae JNI de serie via <jni.h>. Por eso este fichero es el unico del
   nucleo que habla con la VM. */
#include "cover.h"

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Una caratula de mas de 8 MB no es una caratula: es un fichero que se ha
   colado en la carpeta. Por encima de eso se pasa en silencio en vez de
   reservarlo en RAM. */
#define COVER_MAX_BYTES (8u * 1024u * 1024u)

/* FNV-1a sobre los bytes de la imagen. El motor cachea las texturas por ruta
   del icono, asi que dos canciones con la MISMA portada deben caer en el mismo
   fichero (una sola textura, una sola escritura) y dos portadas distintas
   deben caer en ficheros distintos (si no, la segunda mostraria la primera). */
static unsigned fnv1a(const unsigned char* p, size_t n)
{
    unsigned h = 2166136261u;
    size_t i;
    for(i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

/* stb_image, que es lo que usa el motor para el icono, huele la cabecera y no la
   extension. Aun asi el nombre se parece al contenido: si un dia hay que mirar
   un fichero a mano, saber si es jpg o png ayuda. */
static const char* ext_for(const unsigned char* p, size_t n)
{
    if(n >= 3 && p[0] == 0xFFu && p[1] == 0xD8u && p[2] == 0xFFu) return "jpg";
    if(n >= 8 && p[0] == 0x89u && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return "png";
    return "img";
}

/* El motor solo dibuja 0x20..0x7E en su fuente 5x7, y el resto sale como '?'.
   Un byte no ASCII se descarta en vez de convertirse en interrogante: "A Mi" se
   lee mejor que "A M?". */
static void sanitize_ascii(const char* in, char* out, size_t out_len)
{
    size_t w = 0;
    if(!out_len) return;
    if(in)
    {
        for(; *in && w + 1 < out_len; in++)
        {
            unsigned char c = (unsigned char)*in;
            if(c >= 0x20u && c <= 0x7Eu) out[w++] = (char)c;
        }
    }
    while(w > 0 && out[w - 1] == ' ') w--;   /* sin espacios colgando */
    out[w] = 0;
}

/* Toda llamada a Java puede dejar una excepcion pendiente, y en ART una
   excepcion pendiente envenena la siguiente llamada JNI. Este parche no la
   necesita, se limpia y se sigue. Devuelve 1 si habia algo pendiente, que el
   llamante usa para distinguir "el fichero no es media" de un fallo real. */
static int clear_exc(JNIEnv* env)
{
    if((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return 1; }
    return 0;
}

int radio_cover_extract(void* jni_env, const char* track_path, const char* cache_dir,
                        char* out_path, size_t out_len,
                        char* out_title, size_t title_len)
{
    JNIEnv*  env = (JNIEnv*)jni_env;
    JavaVM*  vm = NULL;
    jclass   cls = NULL;
    jobject  ret = NULL;
    jmethodID mid;
    jfieldID fid;
    jstring  jpath = NULL, jtitle = NULL;
    jbyteArray pic = NULL;
    const char* raw_title = NULL;
    unsigned char* bytes = NULL;
    jsize    n = 0;
    const char* ext;
    unsigned h;
    char     full[640];
    struct stat st;
    FILE*    f;
    int      rc = 0;
    int      attached = 0;

    if(out_path && out_len) out_path[0] = 0;
    if(out_title && title_len) out_title[0] = 0;
    if(!env || !track_path || !out_path || !out_len) return 0;

    /* El hilo de reproduccion no lo creo Java, asi que FindClass sobre el sin
       contexto pondria la busqueda en el ClassLoader del sistema y fallaria con
       un NoClassDefFoundError diferido. AML expone la VM, asi que se adjunta
       aqui y se suelta al final. */
    if((*env)->GetJavaVM(env, &vm) != JNI_OK || !vm) { clear_exc(env); return -1; }
    if((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        if((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) { clear_exc(env); return -1; }
        attached = 1;
    }

    cls = (*env)->FindClass(env, "android/media/MediaMetadataRetriever");
    if(!cls) { clear_exc(env); goto done; }

    mid = (*env)->GetMethodID(env, cls, "<init>", "()V");
    if(!mid) { clear_exc(env); goto done; }
    ret = (*env)->NewObject(env, cls, mid);
    if(!ret) { clear_exc(env); goto done; }

    jpath = (*env)->NewStringUTF(env, track_path);
    mid = (*env)->GetMethodID(env, cls, "setDataSource", "(Ljava/lang/String;)V");
    if(!mid || !jpath) { clear_exc(env); goto done; }
    (*env)->CallVoidMethod(env, ret, mid, jpath);
    /* Si setDataSource revienta, el fichero no es media que Java sepa leer. Eso
       es el caso normal de una pista con una cabecera rara, no un fallo: se
       devuelve 0 y la UI se queda con el nombre de fichero. */
    if(clear_exc(env)) goto done;

    /* Titulo primero: es barato y el label se actualiza aunque no haya portada,
       que es el caso mas comun (un MP3 sin APIC, un WAV). */
    if(out_title && title_len)
    {
        fid = (*env)->GetStaticFieldID(env, cls, "METADATA_KEY_TITLE", "I");
        if(clear_exc(env)) fid = NULL;   /* el campo no existe: sin titulo */
        if(fid)
        {
            mid = (*env)->GetMethodID(env, cls, "extractMetadata",
                                      "(I)Ljava/lang/String;");
            if(mid && !clear_exc(env))
            {
                jtitle = (jstring)(*env)->CallObjectMethod(env, ret, mid,
                    (jint)(*env)->GetStaticIntField(env, cls, fid));
                clear_exc(env);
                if(jtitle)
                {
                    raw_title = (*env)->GetStringUTFChars(env, jtitle, NULL);
                    if(raw_title) sanitize_ascii(raw_title, out_title, title_len);
                }
            }
        }
    }

    mid = (*env)->GetMethodID(env, cls, "getEmbeddedPicture", "()[B");
    if(!mid) { clear_exc(env); goto done; }
    pic = (jbyteArray)(*env)->CallObjectMethod(env, ret, mid);
    clear_exc(env);
    if(!pic) goto done;                       /* sin portada: 0, no es error */
    n = (*env)->GetArrayLength(env, pic);
    if(n <= 0) goto done;
    if((size_t)n > COVER_MAX_BYTES) goto done;

    bytes = (unsigned char*)malloc((size_t)n);
    if(!bytes) goto done;
    (*env)->GetByteArrayRegion(env, pic, 0, n, (jbyte*)bytes);
    clear_exc(env);

    h = fnv1a(bytes, (size_t)n);
    ext = ext_for(bytes, (size_t)n);
    if(snprintf(full, sizeof full, "%s/cover_%08x.%s", cache_dir, h, ext)
       >= (int)sizeof full)
        goto done;

    /* El motor cachea por ruta, asi que si el fichero ya existe NO se reescribe:
       misma portada -> misma textura. Reescribirlo gastaria I/O y no aportaria
       nada nuevo. */
    if(stat(full, &st) != 0 || (size_t)st.st_size != (size_t)n)
    {
        mkdir(cache_dir, 0777);   /* si ya existe da EEXIST y da igual */
        f = fopen(full, "wb");
        if(!f) goto done;
        if(fwrite(bytes, 1, (size_t)n, f) != (size_t)n) { fclose(f); goto done; }
        fclose(f);
    }

    snprintf(out_path, out_len, "%s", full);
    rc = 1;

done:
    if(raw_title) (*env)->ReleaseStringUTFChars(env, jtitle, raw_title);
    free(bytes);
    if(cls && ret)
        (*env)->CallVoidMethod(env, ret, (*env)->GetMethodID(env, cls, "release", "()V"));
    if(jtitle) (*env)->DeleteLocalRef(env, jtitle);
    if(jpath)  (*env)->DeleteLocalRef(env, jpath);
    if(pic)    (*env)->DeleteLocalRef(env, pic);
    if(ret)    (*env)->DeleteLocalRef(env, ret);
    if(cls)    (*env)->DeleteLocalRef(env, cls);
    clear_exc(env);
    if(attached) (*vm)->DetachCurrentThread(vm);
    return rc;
}
