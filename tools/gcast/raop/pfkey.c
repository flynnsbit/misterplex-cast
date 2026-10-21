/* AirPlay audio test sender helper: aeskey = playfair_decrypt(keymsg[164], ekey[72]) as UxPlay's
 * fairplay_decrypt computes it. usage: pfkey <keymsg-hex> <ekey-hex>  -> 32 hex chars */
#include <stdio.h>
#include <string.h>
#include "playfair.h"
static int unhex(const char* s, unsigned char* o, int n) {
    if ((int)strlen(s) != 2 * n) return -1;
    for (int i = 0; i < n; ++i) { unsigned v; if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1; o[i] = (unsigned char)v; }
    return 0;
}
int main(int c, char** v) {
    unsigned char km[164], ek[72], out[16];
    if (c != 3 || unhex(v[1], km, 164) || unhex(v[2], ek, 72)) { fprintf(stderr, "usage: pfkey keymsg164hex ekey72hex\n"); return 2; }
    playfair_decrypt(km, ek, out);
    for (int i = 0; i < 16; ++i) printf("%02x", out[i]);
    printf("\n");
    return 0;
}
