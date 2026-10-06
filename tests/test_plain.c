#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "vita2d.h"
#include "ui.h"
int main(void)
{
    char b[128];
    assert(!strcmp(ui_plain("Dağ Işık Şişe Çiçek Üzüm Öğretmen İstanbul ğĞıİşŞçÇüÜöÖ", b, sizeof b), "Dag Isik Sise Cicek Uzum Ogretmen Istanbul gGiIsScCuUoO"));
    const char *s = "TRT 1 HD";
    assert(ui_plain(s, b, sizeof b) == s);                  /* untouched text is not copied */
    assert(!strcmp(ui_plain("Café é", b, sizeof b), "Café é"));   /* other accents stay */
    char tiny[4]; ui_plain("ğğğğğğ", tiny, sizeof tiny); assert(!strcmp(tiny, "ggg"));
    puts("ui_plain ok");
    return 0;
}
