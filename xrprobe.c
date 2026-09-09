// xrprobe — диагностика XREAL One Pro + камеры Eye.
// Только чтение: без явного флага в устройство ничего не пишется.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/hidraw.h>

#define XREAL_VID 0x3318
#define XREAL_PID 0x0436

static const char *NRBSP_SYMS[] = {
    "NRBSPGetCameraStatus", "NRBSPGetCameraStatusWithHandle",
    "NRBSPGetUsbConfig",    "NRBSPGetUsbConfigAll",
    "NRBSPSetUsbConfig",    "NRBSPSetUsbConfigAll",
    "NRBSPGetProperty",     "NROTAGetHandle", "NROTA_Init",
    "NRBSPWaitPilotReady",  "NRBSPCheckServiceReady", NULL
};

static void hexdump(const unsigned char *p, int n) {
    for (int i = 0; i < n; i++) {
        if (i && i % 16 == 0) printf("\n            ");
        printf("%02x ", p[i]);
    }
    printf("\n");
}

static void cmd_info(void) {
    printf("=== HID-устройства ===\n");
    for (int i = 0; i < 8; i++) {
        char path[64];
        snprintf(path, sizeof path, "/dev/hidraw%d", i);
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0) { if (errno != ENOENT) printf("%s: %s\n", path, strerror(errno)); continue; }

        struct hidraw_devinfo di;
        memset(&di, 0, sizeof di);
        if (ioctl(fd, HIDIOCGRAWINFO, &di) == 0) {
            int mine = ((di.vendor & 0xffff) == XREAL_VID) && ((di.product & 0xffff) == XREAL_PID);
            printf("%s: bus=%d vid=%04x pid=%04x%s\n", path, di.bustype,
                   di.vendor & 0xffff, di.product & 0xffff, mine ? "   <-- XREAL" : "");
        }
        char name[256] = {0};
        if (ioctl(fd, HIDIOCGRAWNAME(sizeof name), name) >= 0) printf("    имя: %s\n", name);

        int dsize = 0;
        if (ioctl(fd, HIDIOCGRDESCSIZE, &dsize) == 0) {
            struct hidraw_report_descriptor rd;
            memset(&rd, 0, sizeof rd);
            rd.size = dsize;
            if (ioctl(fd, HIDIOCGRDESC, &rd) == 0) {
                printf("    дескриптор (%d байт):\n            ", dsize);
                hexdump(rd.value, dsize);
            }
        }
        close(fd);
    }

    printf("\n=== USB: интерфейсы XREAL ===\n");
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d) { printf("не открыть sysfs: %s\n", strerror(errno)); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        char p[512], buf[64];
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/idVendor", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) { fclose(f); continue; }
        fclose(f);
        if (strncmp(buf, "3318", 4) != 0) continue;

        int n = 0;
        snprintf(p, sizeof p, "/sys/bus/usb/devices/%s/bNumInterfaces", e->d_name);
        if ((f = fopen(p, "r"))) { if (fscanf(f, "%d", &n) != 1) n = 0; fclose(f); }
        printf("устройство %s: интерфейсов=%d\n", e->d_name, n);
        printf("  (17 означает, что камера Eye активирована; 9 — нет)\n");
    }
    closedir(d);
}

static void cmd_syms(const char *so) {
    void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen(%s) не удался: %s\n", so, dlerror()); return; }
    printf("=== символы в %s ===\n", so);
    for (int i = 0; NRBSP_SYMS[i]; i++) {
        void *s = dlsym(h, NRBSP_SYMS[i]);
        printf("  %-34s %s\n", NRBSP_SYMS[i], s ? "есть" : "нет");
    }
    dlclose(h);
}

// Каждый вариант вызова — в дочернем процессе: если ABI не совпал и он упадёт,
// сам пробник выживет и попробует следующий.
static void try_call(const char *so, const char *desc, int variant) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { printf("fork: %s\n", strerror(errno)); return; }
    if (pid == 0) {
        void *h = dlopen(so, RTLD_NOW | RTLD_LOCAL);
        if (!h) _exit(90);
        void *f = dlsym(h, "NRBSPGetCameraStatus");
        if (!f) _exit(91);
        int r = -1;
        char tag[64] = "xrprobe";
        int out = 0;
        if (variant == 0)      r = ((int(*)(const char*))f)(tag);
        else if (variant == 1) r = ((int(*)(int, int*))f)(0, &out);
        else if (variant == 2) r = ((int(*)(int, const char*))f)(0, tag);
        else if (variant == 3) r = ((int(*)(const char*, int*))f)(tag, &out);
        printf("    [%s] вернула %d (out=%d)\n", desc, r, out);
        fflush(stdout);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) printf("    [%s] упала по сигналу %d — ABI не тот\n", desc, WTERMSIG(st));
    else if (WEXITSTATUS(st) == 90) printf("    [%s] dlopen не удался\n", desc);
    else if (WEXITSTATUS(st) == 91) printf("    [%s] символ не найден\n", desc);
}

static void cmd_call(const char *so) {
    printf("=== пробуем NRBSPGetCameraStatus (каждый вариант в отдельном процессе) ===\n");
    try_call(so, "int f(const char*)",        0);
    try_call(so, "int f(int, int*)",          1);
    try_call(so, "int f(int, const char*)",   2);
    try_call(so, "int f(const char*, int*)",  3);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("использование:\n"
               "  xrprobe --info\n"
               "  xrprobe --syms <путь к .so>\n"
               "  xrprobe --call <путь к .so>\n");
        return 1;
    }
    if (!strcmp(argv[1], "--info")) cmd_info();
    else if (!strcmp(argv[1], "--syms") && argc > 2) cmd_syms(argv[2]);
    else if (!strcmp(argv[1], "--call") && argc > 2) cmd_call(argv[2]);
    else { printf("неизвестная команда\n"); return 1; }
    return 0;
}
