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

// Захват серии кадров. Первые кадры после старта потока часто пустые или
// сильно сжатые — по одному судить о качестве нельзя, поэтому берём серию
// и показываем разброс, а на диск кладём самый большой.
static int capture(const char *dev, const char *out, unsigned int want_fmt,
                   int want_w, int want_h, int frames_wanted, int quiet) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("не открыть %s: %s\n", dev, strerror(errno)); return -1; }

    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (want_fmt) {
        f.fmt.pix.width = want_w;
        f.fmt.pix.height = want_h;
        f.fmt.pix.pixelformat = want_fmt;
        f.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(fd, VIDIOC_S_FMT, &f) != 0) {
            if (!quiet) printf("  S_FMT не удался: %s\n", strerror(errno));
            close(fd); return -1;
        }
    }
    if (ioctl(fd, VIDIOC_G_FMT, &f) != 0) { close(fd); return -1; }

    char b[5];
    // Драйвер вправе выдать не то, что просили — показываем, что реально встало.
    printf("  режим: %ux%u %s\n", f.fmt.pix.width, f.fmt.pix.height,
           fourcc(f.fmt.pix.pixelformat, b));

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = 6; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) != 0) { printf("  REQBUFS: %s\n", strerror(errno)); close(fd); return -1; }

    void *bufs[8]; unsigned int lens[8];
    for (unsigned i = 0; i < rb.count && i < 8; i++) {
        struct v4l2_buffer bf;
        memset(&bf, 0, sizeof bf);
        bf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; bf.memory = V4L2_MEMORY_MMAP; bf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &bf) != 0) { close(fd); return -1; }
        bufs[i] = mmap(NULL, bf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bf.m.offset);
        lens[i] = bf.length;
        if (bufs[i] == MAP_FAILED) { close(fd); return -1; }
        if (ioctl(fd, VIDIOC_QBUF, &bf) != 0) { close(fd); return -1; }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        printf("  STREAMON: %s\n", strerror(errno));
        for (unsigned i = 0; i < rb.count && i < 8; i++) munmap(bufs[i], lens[i]);
        close(fd); return -1;
    }

    unsigned int best = 0, total = 0, count = 0, minsz = 0xffffffff, maxsz = 0;
    for (int frame = 0; frame < frames_wanted; frame++) {
        struct v4l2_buffer bf;
        memset(&bf, 0, sizeof bf);
        bf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; bf.memory = V4L2_MEMORY_MMAP;
        int ok = -1;
        for (int a = 0; a < 60; a++) {
            if (ioctl(fd, VIDIOC_DQBUF, &bf) == 0) { ok = 0; break; }
            if (errno != EAGAIN) break;
            usleep(20000);
        }
        if (ok != 0) break;
        if (bf.bytesused > 2000) {
            count++; total += bf.bytesused;
            if (bf.bytesused < minsz) minsz = bf.bytesused;
            if (bf.bytesused > maxsz) maxsz = bf.bytesused;
            if (bf.bytesused > best && out) {
                best = bf.bytesused;
                FILE *o = fopen(out, "wb");
                if (o) { fwrite(bufs[bf.index], 1, bf.bytesused, o); fclose(o); }
            }
        }
        ioctl(fd, VIDIOC_QBUF, &bf);
    }

    if (count)
        printf("  кадров: %u   мин %u   средн %u   МАКС %u байт\n",
               count, minsz, total / count, maxsz);
    else
        printf("  содержательных кадров нет\n");

    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < rb.count && i < 8; i++) munmap(bufs[i], lens[i]);
    close(fd);
    return count ? 0 : -1;
}

static void cmd_grab(const char *dev, const char *out, unsigned int fmt, int w, int h) {
    capture(dev, out, fmt, w, h, 40, 0);
}

/** Прогон всех заявленных режимов: что из них реально отдаёт кадры и какого веса. */
static void cmd_modes(const char *dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("не открыть %s: %s\n", dev, strerror(errno)); return; }
    struct { unsigned int fmt; int w, h; } list[32];
    int n = 0;
    for (int i = 0; i < 8 && n < 32; i++) {
        struct v4l2_fmtdesc fd_;
        memset(&fd_, 0, sizeof fd_);
        fd_.index = i; fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fd_) != 0) break;
        for (int j = 0; j < 12 && n < 32; j++) {
            struct v4l2_frmsizeenum fs;
            memset(&fs, 0, sizeof fs);
            fs.index = j; fs.pixel_format = fd_.pixelformat;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) != 0) break;
            if (fs.type != V4L2_FRMSIZE_TYPE_DISCRETE) break;
            list[n].fmt = fd_.pixelformat;
            list[n].w = fs.discrete.width;
            list[n].h = fs.discrete.height;
            n++;
        }
    }
    close(fd);

    char b[5];
    for (int i = 0; i < n; i++) {
        printf("\n=== %s %dx%d ===\n", fourcc(list[i].fmt, b), list[i].w, list[i].h);
        char path[128];
        snprintf(path, sizeof path, "/data/local/tmp/mode_%s_%dx%d.bin",
                 fourcc(list[i].fmt, b), list[i].w, list[i].h);
        capture(dev, path, list[i].fmt, list[i].w, list[i].h, 25, 1);
    }
}


/**
 * Отправка сырого HID-репорта в очки и чтение ответа.
 *
 * Байты берутся как есть: CRC в кадре XREAL зависит только от содержимого
 * (одна и та же команда в разных перехватах шла с одинаковой контрольной
 * суммой), поэтому записанную команду можно воспроизводить дословно,
 * не зная алгоритма подсчёта.
 */
static void cmd_sendhid(const char *dev, const char *hex) {
    unsigned char buf[1024];
    int n = 0;
    for (const char *p = hex; *p && n < (int)sizeof buf; ) {
        if (*p == ' ' || *p == ':' || *p == ',') { p++; continue; }
        unsigned int v;
        if (sscanf(p, "%2x", &v) != 1) { printf("плохой hex у позиции %d\n", (int)(p - hex)); return; }
        buf[n++] = (unsigned char)v;
        p += 2;
    }
    if (n == 0) { printf("пустая команда\n"); return; }

    printf("отправляю %d байт в %s:\n  ", n, dev);
    for (int i = 0; i < n; i++) printf("%02x ", buf[i]);
    printf("\n");

    // O_NONBLOCK обязателен: очки отвечают не на всякую команду, а блокирующий
    // read() в этом случае вис бы навсегда.
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) { printf("не открыть %s: %s\n", dev, strerror(errno)); return; }

    ssize_t w = write(fd, buf, n);
    if (w < 0) { printf("ошибка записи: %s\n", strerror(errno)); close(fd); return; }
    printf("записано: %zd байт\n", w);

    // Ответ приходит на interrupt IN. Ждём недолго: очки отвечают быстро,
    // а после команды активации устройство вообще переподключается.
    unsigned char in[1024];
    int got = 0;
    for (int attempt = 0; attempt < 30; attempt++) {
        ssize_t r = read(fd, in, sizeof in);
        if (r > 0) {
            printf("ответ %zd байт:\n  ", r);
            for (int i = 0; i < r && i < 32; i++) printf("%02x ", in[i]);
            printf("\n");
            got = 1;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            printf("чтение прервано: %s\n", strerror(errno));
            break;
        }
        usleep(50000);
    }
    if (!got) printf("ответа нет (команда могла быть принята без подтверждения)\n");
    close(fd);
}


/** Регуляторы камеры: яркость, экспозиция, качество сжатия — что вообще доступно. */
static void cmd_ctrls(const char *dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("не открыть %s: %s\n", dev, strerror(errno)); return; }
    printf("=== регуляторы %s ===\n", dev);
    int found = 0;
    struct v4l2_queryctrl q;
    memset(&q, 0, sizeof q);
    // V4L2_CTRL_FLAG_NEXT_CTRL обходит все, включая расширенные классы.
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
        if (!(q.flags & V4L2_CTRL_FLAG_DISABLED)) {
            struct v4l2_control c;
            memset(&c, 0, sizeof c);
            c.id = q.id;
            int have = (ioctl(fd, VIDIOC_G_CTRL, &c) == 0);
            printf("  0x%08x %-34s мин=%d макс=%d шаг=%d умолч=%d", q.id, q.name,
                   q.minimum, q.maximum, q.step, q.default_value);
            if (have) printf("  ТЕКУЩЕЕ=%d", c.value);
            printf("\n");
            found++;
        }
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    if (!found) printf("  регуляторов нет — камера ничего не отдаёт на настройку\n");
    close(fd);
}


static int set_ctrl(int fd, unsigned int id, int value) {
    struct v4l2_control c;
    memset(&c, 0, sizeof c);
    c.id = id; c.value = value;
    return ioctl(fd, VIDIOC_S_CTRL, &c);
}

static void cmd_setctrl(const char *dev, unsigned int id, int value) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("не открыть: %s\n", strerror(errno)); return; }
    if (set_ctrl(fd, id, value) != 0) printf("не удалось: %s\n", strerror(errno));
    else {
        struct v4l2_control c;
        memset(&c, 0, sizeof c);
        c.id = id;
        ioctl(fd, VIDIOC_G_CTRL, &c);
        printf("0x%08x = %d (запрошено %d)\n", id, c.value, value);
    }
    close(fd);
}

/**
 * Подбор фокуса по объёму кадра — в одной сессии потока.
 *
 * Открывать устройство на каждое положение нельзя: закрытие сбрасывает
 * взведение потока, и следующий S_FMT падает с EIO. Поэтому поток
 * запускается один раз, а фокус переставляется на ходу.
 *
 * Резкий кадр содержит больше высокочастотных деталей и потому весит
 * больше — прямого показателя резкости камера не отдаёт.
 */
static void cmd_focus(const char *dev) {
    int fd = open(dev, O_RDWR);
    if (fd < 0) { printf("не открыть %s: %s\n", dev, strerror(errno)); return; }

    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = 1920; f.fmt.pix.height = 1080;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &f) != 0) {
        printf("S_FMT: %s\n  (поток не взведён — пошлите команду активации 0xd3)\n", strerror(errno));
        close(fd); return;
    }

    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = 6; rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; rb.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &rb) != 0) { printf("REQBUFS: %s\n", strerror(errno)); close(fd); return; }

    void *bufs[8]; unsigned int lens[8];
    for (unsigned i = 0; i < rb.count && i < 8; i++) {
        struct v4l2_buffer bf;
        memset(&bf, 0, sizeof bf);
        bf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; bf.memory = V4L2_MEMORY_MMAP; bf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &bf) != 0) { close(fd); return; }
        bufs[i] = mmap(NULL, bf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, bf.m.offset);
        lens[i] = bf.length;
        if (bufs[i] == MAP_FAILED) { close(fd); return; }
        ioctl(fd, VIDIOC_QBUF, &bf);
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) { printf("STREAMON: %s\n", strerror(errno)); close(fd); return; }

    printf("=== подбор фокуса (больше байт = резче) ===\n");
    int best_f = 0; unsigned int best_sz = 0;
    for (int fv = 200; fv <= 800; fv += 50) {
        set_ctrl(fd, 0x009a090a, fv);

        // Первые кадры после перестановки ещё сняты старым фокусом —
        // объектив едет, поэтому их отбрасываем.
        unsigned int mx = 0;
        for (int k = 0; k < 22; k++) {
            struct v4l2_buffer bf;
            memset(&bf, 0, sizeof bf);
            bf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; bf.memory = V4L2_MEMORY_MMAP;
            int ok = -1;
            for (int a = 0; a < 40; a++) {
                if (ioctl(fd, VIDIOC_DQBUF, &bf) == 0) { ok = 0; break; }
                if (errno != EAGAIN) break;
                usleep(20000);
            }
            if (ok != 0) break;
            if (k >= 12 && bf.bytesused > mx) {
                mx = bf.bytesused;
                char path[128];
                snprintf(path, sizeof path, "/data/local/tmp/focus_%d.jpg", fv);
                FILE *o = fopen(path, "wb");
                if (o) { fwrite(bufs[bf.index], 1, bf.bytesused, o); fclose(o); }
            }
            ioctl(fd, VIDIOC_QBUF, &bf);
        }
        printf("  фокус %3d: %6u байт%s\n", fv, mx, mx > best_sz ? "   <-- лучший" : "");
        if (mx > best_sz) { best_sz = mx; best_f = fv; }
    }
    printf("\nЛУЧШИЙ ФОКУС: %d (%u байт), снимок в /data/local/tmp/focus_%d.jpg\n", best_f, best_sz, best_f);
    set_ctrl(fd, 0x009a090a, best_f);

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
               "  xrprobe --grab <устройство> <файл> [MJPG|HEVC] [ШxВ]\n"
               "  xrprobe --modes <устройство>\n"
               "  xrprobe --sendhid <hidraw> <hex-байты>\n"
               "  xrprobe --ctrls <устройство>\n"
               "  xrprobe --setctrl <устройство> <id 0x..> <значение>\n"
               "  xrprobe --focus <устройство>\n");
        return 1;
    }
    if (!strcmp(argv[1], "--info")) cmd_info();
    else if (!strcmp(argv[1], "--syms") && argc > 2) cmd_syms(argv[2]);
    else if (!strcmp(argv[1], "--call") && argc > 2) cmd_call(argv[2]);
    else if (!strcmp(argv[1], "--v4l2") && argc > 2) cmd_v4l2(argv[2]);
    else if (!strcmp(argv[1], "--grab") && argc > 3) {
        unsigned int fmt = 0; int w = 0, h = 0;
        if (argc > 4) {
            if (!strcasecmp(argv[4], "mjpg") || !strcasecmp(argv[4], "mjpeg")) fmt = V4L2_PIX_FMT_MJPEG;
            else if (!strcasecmp(argv[4], "hevc")) fmt = v4l2_fourcc('H','E','V','C');
            w = 1920; h = 1080;
        }
        if (argc > 5) sscanf(argv[5], "%dx%d", &w, &h);
        cmd_grab(argv[2], argv[3], fmt, w, h);
    }
    else if (!strcmp(argv[1], "--modes") && argc > 2) cmd_modes(argv[2]);
    else if (!strcmp(argv[1], "--sendhid") && argc > 3) cmd_sendhid(argv[2], argv[3]);
    else if (!strcmp(argv[1], "--ctrls") && argc > 2) cmd_ctrls(argv[2]);
    else if (!strcmp(argv[1], "--setctrl") && argc > 4)
        cmd_setctrl(argv[2], (unsigned int)strtoul(argv[3], NULL, 0), atoi(argv[4]));
    else if (!strcmp(argv[1], "--focus") && argc > 2) cmd_focus(argv[2]);
    else { printf("неизвестная команда\n"); return 1; }
    return 0;
}
