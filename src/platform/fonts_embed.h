/*
SCOPO: i font dell'interfaccia (Fira Sans, JetBrains Mono, licenza OFL in assets/fonts) inclusi
       nell'eseguibile, così basta scaricare un solo file: Windows li carica dalla memoria,
       Linux li copia una volta nella cache dell'utente per fontconfig.
*/
#ifndef SV_FONTS_EMBED_H
#define SV_FONTS_EMBED_H

#include <stddef.h>

typedef struct { const char* name; const unsigned char* data; size_t size; } EmbeddedFont;

// numero di font e il font i-esimo
int                 sv_embedded_font_count(void);
EmbeddedFont        sv_embedded_font(int i);

#endif
