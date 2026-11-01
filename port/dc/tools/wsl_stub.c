/* Stand-in for wsl.exe, for testing build-cdi.bat under WINE (which has no WSL). It does not run
 * anything: it logs its arguments to the file named by WSL_STUB_LOG, answers "-e true", converts
 * "wslpath -a -u PATH" for WINE's Z: drive (which is /), and returns WSL_STUB_RC (default 0) for
 * "bash SCRIPT ROM". This checks the launcher's own logic -- argument and path handling, error
 * messages, exit status -- not WSL.
 *
 *   x86_64-w64-mingw32-gcc -O2 -o wsl.exe port/dc/tools/wsl_stub.c
 *   WINEPATH='Z:\path\to\dir\of\wsl.exe' WSL_STUB_LOG=/tmp/wsl.log BUILD_CDI_NOPAUSE=1 \
 *       wine cmd /c 'Z:\...\port\dc\build-cdi.bat' 'Z:\path\to\rom.gba'
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *logp = getenv("WSL_STUB_LOG");
    FILE *log = logp ? fopen(logp, "a") : NULL;
    int i;
    if (log) {
        fprintf(log, "wsl.exe");
        for (i = 1; i < argc; i++) fprintf(log, " [%s]", argv[i]);
        fprintf(log, "\n");
        fclose(log);
    }
    if (argc == 3 && !strcmp(argv[1], "-e") && !strcmp(argv[2], "true")) return 0;
    if (argc == 5 && !strcmp(argv[1], "wslpath")) {
        const char *p = argv[4];
        if ((p[0] == 'Z' || p[0] == 'z') && p[1] == ':') p += 2;
        for (; *p; p++) putchar(*p == '\\' ? '/' : *p);
        putchar('\n');
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "bash")) return getenv("WSL_STUB_RC") ? atoi(getenv("WSL_STUB_RC")) : 0;
    return 1;
}
