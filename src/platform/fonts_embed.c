#include "fonts_embed.h"

// .incbin copia il file dentro l'oggetto al momento della compilazione (percorso relativo alla
// cartella del progetto, da cui si lancia make)
#ifdef _WIN32
  #define SV_RODATA ".section .rdata,\"dr\"\n"
#else
  #define SV_RODATA ".section .rodata\n"
#endif
#define SV_INCBIN(sym, file) \
    __asm__(SV_RODATA ".global " #sym "\n.balign 16\n" #sym ":\n.incbin \"" file "\"\n" \
            ".global " #sym "_end\n" #sym "_end:\n.byte 0\n.text\n");

SV_INCBIN(sv_font_ui, "assets/fonts/FiraSans-Regular.ttf")
SV_INCBIN(sv_font_ui_bold, "assets/fonts/FiraSans-SemiBold.ttf")
SV_INCBIN(sv_font_mono, "assets/fonts/JetBrainsMono-Regular.ttf")
SV_INCBIN(sv_font_mono_bold, "assets/fonts/JetBrainsMono-Bold.ttf")

extern const unsigned char sv_font_ui[], sv_font_ui_end[], sv_font_ui_bold[], sv_font_ui_bold_end[];
extern const unsigned char sv_font_mono[], sv_font_mono_end[], sv_font_mono_bold[], sv_font_mono_bold_end[];

int sv_embedded_font_count(void) { return 4; }

EmbeddedFont sv_embedded_font(int i) {
    switch (i) {
        case 0:  return (EmbeddedFont){ "FiraSans-Regular.ttf", sv_font_ui, (size_t)(sv_font_ui_end - sv_font_ui) };
        case 1:  return (EmbeddedFont){ "FiraSans-SemiBold.ttf", sv_font_ui_bold, (size_t)(sv_font_ui_bold_end - sv_font_ui_bold) };
        case 2:  return (EmbeddedFont){ "JetBrainsMono-Regular.ttf", sv_font_mono, (size_t)(sv_font_mono_end - sv_font_mono) };
        default: return (EmbeddedFont){ "JetBrainsMono-Bold.ttf", sv_font_mono_bold, (size_t)(sv_font_mono_bold_end - sv_font_mono_bold) };
    }
}
