#include <android/log.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <unistd.h>

#define LOG_TAG "XTouchTune"
#define SELECT_TOUCH_ID 3
#define COMMON_DATA_CMD 0
#define SET_CUR_VALUE 0
#define GET_MODE_VALUE 5

#define MODE_UP_THRESHOLD 2
#define MODE_TOLERANCE 3
#define MODE_AIM_SENS 4
#define MODE_TAP_STABILITY 5
#define MODE_AOD_EN 11
#define MODE_DOUBLETAP 14
#define MODE_NONUI 17

#define BACKLIGHT_PATH "/sys/class/backlight/panel0-backlight/brightness"
#define LOCK_PATH "/data/adb/.cos17_xtouch_tune.lock"

typedef struct {
    int8_t touch_id;
    uint8_t cmd;
    uint16_t mode;
    uint16_t data_len;
    int32_t data_buf[256];
} common_data_t;

static int set_mode(int fd, uint16_t mode, int32_t val) {
    common_data_t d = {0};
    d.touch_id = 0;
    d.cmd = SET_CUR_VALUE;
    d.mode = mode;
    d.data_len = 1;
    d.data_buf[0] = val;
    return ioctl(fd, _IOWR('T', COMMON_DATA_CMD, common_data_t), &d);
}

static int get_mode(int fd, uint16_t mode, int32_t out[4]) {
    common_data_t d = {0};
    d.touch_id = 0;
    d.cmd = GET_MODE_VALUE;
    d.mode = mode;
    d.data_len = 4;
    if (ioctl(fd, _IOWR('T', COMMON_DATA_CMD, common_data_t), &d) < 0) return -1;
    memcpy(out, d.data_buf, 16);
    return 0;
}

static void apply_tuning(int fd) {
    set_mode(fd, MODE_UP_THRESHOLD, 1);   // stock=3 (0..4): lighter tap-up trigger
    set_mode(fd, MODE_TOLERANCE, 1);      // stock=2 (0..4): sharper tap acceptance
    set_mode(fd, MODE_AIM_SENS, 4);       // stock=2 (0..4): responsive detection
    set_mode(fd, MODE_TAP_STABILITY, 1);  // stock=2 (0..4): lower double-tap debounce lag
}

static int read_backlight(void) {
    char buf[32];
    int fd = open(BACKLIGHT_PATH, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    return atoi(buf);
}

static int dt2w_prop_enabled(void) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("persist.sys.oplus.dt2w_enable", buf) > 0) {
        if (strcmp(buf, "0") == 0) return 0;
    }
    return 1;
}

static int run_daemon(int fd) {
    int lfd = open(LOCK_PATH, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lfd >= 0 && flock(lfd, LOCK_EX | LOCK_NB) < 0) {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "daemon already running, exiting");
        close(lfd);
        return 0;
    }

    apply_tuning(fd);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "DT2W daemon started");

    int last_bl = -1;
    for (;;) {
        int bl = read_backlight();
        int en = dt2w_prop_enabled();
        if (en && bl == 0) {
            int32_t v[4] = {0};
            if (get_mode(fd, MODE_DOUBLETAP, v) == 0 && v[0] == 0) {
                apply_tuning(fd);
                set_mode(fd, MODE_DOUBLETAP, 1);
                __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                    "restored DOUBLETAP=1 & tuning on sleep (nonui unchanged)");
            } else if (last_bl > 0) {
                apply_tuning(fd);
            }
        }
        last_bl = bl;
        usleep(250000);
    }
}

int main(int argc, char** argv) {
    int fd = open("/dev/xiaomi-touch", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/xiaomi-touch");
        return 1;
    }
    if (ioctl(fd, _IO('T', SELECT_TOUCH_ID), 0) < 0) {
        perror("SELECT_TOUCH_ID");
        close(fd);
        return 1;
    }

    int apply = 0;
    int dt2w = 0;
    int daemon_mode = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--apply") == 0) apply = 1;
        else if (strcmp(argv[i], "--dt2w") == 0) dt2w = 1;
        else if (strcmp(argv[i], "--daemon") == 0) daemon_mode = 1;
    }

    if (daemon_mode) {
        int rc = run_daemon(fd);
        close(fd);
        return rc;
    }

    if (apply) apply_tuning(fd);
    if (dt2w) set_mode(fd, MODE_DOUBLETAP, 1);

    const struct { uint16_t m; const char* name; } table[] = {
        {MODE_UP_THRESHOLD, "UP_THRESHOLD"},
        {MODE_TOLERANCE, "TOLERANCE"},
        {MODE_AIM_SENS, "AIM_SENS"},
        {MODE_TAP_STABILITY, "TAP_STABILITY"},
        {MODE_AOD_EN, "AOD_EN"},
        {MODE_DOUBLETAP, "DOUBLETAP"},
        {MODE_NONUI, "NONUI"},
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        int32_t v[4] = {0};
        if (get_mode(fd, table[i].m, v) == 0) {
            printf("mode %2u (%-13s): cur=%d def=%d min=%d max=%d\n",
                   table[i].m, table[i].name, v[0], v[1], v[2], v[3]);
        }
    }
    close(fd);
    return 0;
}
