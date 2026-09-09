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
#include <linux/videodev2.h>
#include <sys/mman.h>

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


static const char *fourcc(unsigned int f, char *b) {
    b[0]=f&0xff; b[1]=(f>>8)&0xff; b[2]=(f>>16)&0xff; b[3]=(f>>24)&0xff; b[4]=0;
    return b;
}

static void cmd_v4l2(const char *dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("не открыть %s: %s\n", dev, strerror(errno)); return; }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof cap);
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
        printf("=== %s ===\n", dev);
        printf("  драйвер: %s\n  карта:   %s\n  шина:    %s\n", cap.driver, cap.card, cap.bus_info);
        printf("  возможности: 0x%08x%s\n", cap.capabilities,
               (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ? " (захват видео)" : "");
    } else { printf("QUERYCAP не удался: %s\n", strerror(errno)); close(fd); return; }

    char b[5];
    for (int i = 0; i < 16; i++) {
        struct v4l2_fmtdesc fmt;
        memset(&fmt, 0, sizeof fmt);
        fmt.index = i;
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0) break;
        printf("  формат %d: %s  (%s)%s\n", i, fourcc(fmt.pixelformat, b), fmt.description,
               (fmt.flags & V4L2_FMT_FLAG_COMPRESSED) ? " сжатый" : "");
        for (int j = 0; j < 24; j++) {
            struct v4l2_frmsizeenum fs;
            memset(&fs, 0, sizeof fs);
            fs.index = j;
            fs.pixel_format = fmt.pixelformat;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) break;
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
            printf("       %ux%u", fs.discrete.width, fs.discrete.height);
            for (int k = 0; k < 12; k++) {
                struct v4l2_frmivalenum fi;
                memset(&fi, 0, sizeof fi);
                fi.index = k;
                fi.pixel_format = fmt.pixelformat;
                fi.width = fs.discrete.width;
                fi.height = fs.discrete.height;
                if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fi) != 0) break;
                if (fi.type != V4L2_FRMIVAL_TYPE_DISCRETE) break;
                if (fi.discrete.numerator)
                    printf("  %.0f fps", (double)fi.discrete.denominator / fi.discrete.numerator);
            }
            printf("\n");
        }
    }
    close(fd);
}

// Захват одного кадра через mmap. Пишет сырые байты в файл.
static void cmd_grab(const char *dev, const char *out, int want_mjpeg) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("не открыть %s: %s\n", dev, strerror(errno)); return; }

    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (want_mjpeg) {
        f.fmt.pix.width = 1920;
        f.fmt.pix.height = 1080;
        f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &f) != 0) printf("S_FMT MJPEG не удался: %s\n", strerror(errno));
        else printf("формат переключён на MJPEG\n");
    }
    if (ioctl(fd, VIDIOC_G_FMT, &f) != 0) { printf("G_FMT: %s\n", strerror(errno)); close(fd); return; }
    char b[5];
    printf("текущий формат: %ux%u %s, кадр %u байт\n", f.fmt.pix.width, f.fmt.pix.height,
           fourcc(f.fmt.pix.pixelformat, b), f.fmt.pix.sizeimage);

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = 4; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) != 0) { printf("REQBUFS: %s\n", strerror(errno)); close(fd); return; }
    printf("буферов выделено: %u\n", rb.count);

    void *bufs[8]; unsigned int lens[8];
    for (unsigned i = 0; i < rb.count && i < 8; i++) {
        struct v4l2_buffer bf;
        memset(&bf, 0, sizeof bf);
        bf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; bf.memory = V4L2_MEMORY_MMAP; bf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &bf) != 0) { printf("QUERYBUF: %s\n", strerror(errno)); close(fd); return; }
        bufs[i] = mmap(NULL, bf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bf.m.offset);
        lens[i] = bf.length;
        if (bufs[i] == MAP_FAILED) { printf("mmap: %s\n", strerror(errno)); close(fd); return; }
        if (ioctl(fd, VIDIOC_QBUF, &bf) != 0) { printf("QBUF: %s\n", strerror(errno)); close(fd); return; }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) { printf("STREAMON: %s\n", strerror(errno)); close(fd); return; }
    printf("поток запущен, ждём кадр...\n");

    struct v4l2_buffer bf;
    int saved = 0;
    for (int frame = 0; frame < 40 && !saved; frame++) {
        memset(&bf, 0, sizeof bf);
        bf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; bf.memory = V4L2_MEMORY_MMAP;
        int ok = -1;
        for (int a = 0; a < 40; a++) {
            if (ioctl(fd, VIDIOC_DQBUF, &bf) == 0) { ok = 0; break; }
            if (errno != EAGAIN) { printf("DQBUF: %s\n", strerror(errno)); break; }
            usleep(25000);
        }
        if (ok != 0) break;
        const unsigned char *p = bufs[bf.index];
        printf("  кадр %2d: %7u байт", frame, bf.bytesused);
        if (bf.bytesused >= 4) printf("  начало: %02x %02x %02x %02x", p[0], p[1], p[2], p[3]);
        printf("\n");
        if (bf.bytesused > 2000) {
            FILE *o = fopen(out, "wb");
            if (o) {
                fwrite(p, 1, bf.bytesused, o);
                fclose(o);
                printf("СОДЕРЖАТЕЛЬНЫЙ КАДР записан в %s (%u байт)\n", out, bf.bytesused);
                if (p[0] == 0xff && p[1] == 0xd8) printf("это корректный JPEG (маркер SOI ff d8)\n");
                saved = 1;
            }
        }
        ioctl(fd, VIDIOC_QBUF, &bf);
    }
    if (!saved) printf("содержательных кадров не получено\n");

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < rb.count && i < 8; i++) munmap(bufs[i], lens[i]);
    close(fd);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("использование:\n"
               "  xrprobe --info\n"
               "  xrprobe --syms <путь к .so>\n"
               "  xrprobe --call <путь к .so>\n"
               "  xrprobe --v4l2 <устройство>\n"
               "  xrprobe --grab <устройство> <файл> [mjpeg]\n");
        return 1;
    }
    if (!strcmp(argv[1], "--info")) cmd_info();
    else if (!strcmp(argv[1], "--syms") && argc > 2) cmd_syms(argv[2]);
    else if (!strcmp(argv[1], "--call") && argc > 2) cmd_call(argv[2]);
    else if (!strcmp(argv[1], "--v4l2") && argc > 2) cmd_v4l2(argv[2]);
    else if (!strcmp(argv[1], "--grab") && argc > 3) cmd_grab(argv[2], argv[3], argc > 4);
    else { printf("неизвестная команда\n"); return 1; }
    return 0;
}
