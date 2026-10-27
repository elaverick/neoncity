#include "assets.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

/* The executable's directory with its trailing separator, or "". */
static void exe_dir(char out[1024]) {
    char exe[1024] = "";
#ifdef _WIN32
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (len == 0 || len >= sizeof(exe)) exe[0] = 0;
#else
    ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[len > 0 ? len : 0] = 0;
#endif
    char *slash = strrchr(exe, '/'), *bs = strrchr(exe, '\\');
    if (bs > slash) slash = bs;
    if (slash) slash[1] = 0;
    else exe[0] = 0;
    snprintf(out, 1024, "%s", exe);
}

int asset_roots(char roots[2][1024]) {
    char exe[1024];
    exe_dir(exe);
    int n = 0;
    if (exe[0]) snprintf(roots[n++], 1024, "%sassets", exe);
    snprintf(roots[n++], 1024, "assets");
    return n;
}

int data_roots(char roots[3][1024]) {
    char exe[1024];
    exe_dir(exe);
    int n = 0;
    if (exe[0]) snprintf(roots[n++], 1024, "%s", exe);
    roots[n++][0] = 0;
    snprintf(roots[n++], 1024, "../");
    return n;
}
