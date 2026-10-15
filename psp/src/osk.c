#include "osk.h"

#include <pspkernel.h>
#include <psputility.h>
#include <psputility_osk.h>
#include <string.h>

#define OSK_MAX 32

static SceUtilityOskData g_data;
static SceUtilityOskParams g_params;
static unsigned short g_desc[OSK_MAX], g_in[OSK_MAX], g_out[OSK_MAX + 1];
static int g_open;  // started and not yet back to PSP_UTILITY_DIALOG_NONE
static int g_ready; // a result is waiting
static int g_ok;

static void to_wide(unsigned short *dst, const char *src, int n) {
    int i = 0;
    for (; src && src[i] && i < n - 1; i++) dst[i] = (unsigned char)src[i];
    dst[i] = 0;
}

int osk_start(const char *title, const char *initial, int max_len) {
    if (g_open) return -1;
    if (max_len > OSK_MAX) max_len = OSK_MAX;
    to_wide(g_desc, title, OSK_MAX);
    to_wide(g_in, initial, OSK_MAX);
    memset(g_out, 0, sizeof(g_out));
    memset(&g_data, 0, sizeof(g_data));
    g_data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    g_data.lines = 1;
    g_data.unk_24 = 1;
    g_data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    g_data.desc = g_desc;
    g_data.intext = g_in;
    g_data.outtextlength = max_len + 1;
    g_data.outtextlimit = max_len;
    g_data.outtext = g_out;
    memset(&g_params, 0, sizeof(g_params));
    g_params.base.size = sizeof(g_params);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &g_params.base.language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_UNKNOWN, &g_params.base.buttonSwap);
    g_params.base.graphicsThread = 17;
    g_params.base.accessThread = 19;
    g_params.base.fontThread = 18;
    g_params.base.soundThread = 16;
    g_params.datacount = 1;
    g_params.data = &g_data;
    if (sceUtilityOskInitStart(&g_params) < 0) return -1;
    g_open = 1;
    g_ready = 0;
    return 0;
}

int osk_frame(void) {
    if (!g_open) return 0;
    switch (sceUtilityOskGetStatus()) {
    case PSP_UTILITY_DIALOG_VISIBLE:
        sceUtilityOskUpdate(1);
        break;
    case PSP_UTILITY_DIALOG_QUIT:
        sceUtilityOskShutdownStart();
        break;
    case PSP_UTILITY_DIALOG_NONE:
        g_open = 0;
        g_ready = 1;
        g_ok = g_data.result == PSP_UTILITY_OSK_RESULT_CHANGED || g_data.result == PSP_UTILITY_OSK_RESULT_UNCHANGED;
        g_ok = g_ok && g_params.base.result == 0; // 1: cancelled with O
        return 0;
    default:
        break;
    }
    return 1;
}

int osk_active(void) {
    return g_open;
}

int osk_result(char *out, int out_len) {
    if (!g_ready) return 0;
    g_ready = 0;
    if (!g_ok) return 0;
    int n = 0;
    for (int i = 0; g_out[i] && n < out_len - 1; i++) out[n++] = g_out[i] >= 32 && g_out[i] < 127 ? (char)g_out[i] : '?';
    out[n] = 0;
    return 1;
}
