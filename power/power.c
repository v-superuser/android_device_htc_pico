/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_TAG "HTC PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define SCALING_GOVERNOR_PATH "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"
#define BOOSTPULSE_ONDEMAND "/sys/devices/system/cpu/cpufreq/ondemand/boostpulse"
#define BOOSTPULSE_INTERACTIVE "/sys/devices/system/cpu/cpufreq/interactive/boostpulse"
#define BOOSTPULSE_SMARTASS2 "/sys/devices/system/cpu/cpufreq/smartass/boost_pulse"
#define BOOSTPULSE_SMARTASSH3 "/sys/devices/system/cpu/cpufreq/smartassH3/boost_pulse"
#define SAMPLING_RATE_SCREEN_ON "50000"
#define SAMPLING_RATE_SCREEN_OFF "500000"
#define TIMER_RATE_SCREEN_ON "30000"
#define TIMER_RATE_SCREEN_OFF "500000"

struct lge_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    int boostpulse_fd;
    int boostpulse_warned;
};

static char governor[20];

static int sysfs_read(char *path, char *s, int num_bytes)
{
    char buf[80];
    int count;
    int ret = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);

        return -1;
    }

    if ((count = read(fd, s, num_bytes - 1)) < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);

        ret = -1;
    } else {
        s[count] = '\0';
    }

    close(fd);

    return ret;
}

static void sysfs_write(char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }

    close(fd);
}

static int get_scaling_governor() {
    if (sysfs_read(SCALING_GOVERNOR_PATH, governor,
                sizeof(governor)) == -1) {
        return -1;
    } else {
        // Strip newline at the end.
        int len = strlen(governor);

        len--;

        while (len >= 0 && (governor[len] == '\n' || governor[len] == '\r'))
            governor[len--] = '\0';
    }

    return 0;
}

static void lge_power_set_interactive(struct power_module *module, int on)
{
    if (strncmp(governor, "ondemand", 8) == 0)
        sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/sampling_rate",
                on ? SAMPLING_RATE_SCREEN_ON : SAMPLING_RATE_SCREEN_OFF);
    else if (strncmp(governor, "interactive", 11) == 0)
        sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_rate",
                on ? TIMER_RATE_SCREEN_ON : TIMER_RATE_SCREEN_OFF);
}


static void configure_governor()
{
    lge_power_set_interactive(NULL, 1);

    if (strncmp(governor, "ondemand", 8) == 0) {
        sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/up_threshold", "90");
        sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/io_is_busy", "1");
        sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/sampling_down_factor", "4");
        sysfs_write("/sys/devices/system/cpu/cpufreq/ondemand/down_differential", "10");

    } else if (strncmp(governor, "interactive", 11) == 0) {
        sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/min_sample_time", "90000");
        sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/io_is_busy", "1");
        sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq", "1134000");
        sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay", "30000");
    }
}

static int boostpulse_open(struct lge_power_module *lge)
{
    char buf[80];

    pthread_mutex_lock(&lge->lock);

    if (lge->boostpulse_fd < 0) {
        if (get_scaling_governor() < 0) {
            ALOGE("Can't read scaling governor.");
            lge->boostpulse_warned = 1;
        } else {
            if (strncmp(governor, "ondemand", 8) == 0)
                lge->boostpulse_fd = open(BOOSTPULSE_ONDEMAND, O_WRONLY);
            else if (strncmp(governor, "interactive", 11) == 0)
                lge->boostpulse_fd = open(BOOSTPULSE_INTERACTIVE, O_WRONLY);
            else if (strncmp(governor, "smartassV2", 10) == 0)
                lge->boostpulse_fd = open(BOOSTPULSE_SMARTASS2, O_WRONLY);
            else if (strncmp(governor, "SmartassH3", 10) == 0)
                lge->boostpulse_fd = open(BOOSTPULSE_SMARTASSH3, O_WRONLY);

            if (lge->boostpulse_fd < 0 && !lge->boostpulse_warned) {
                strerror_r(errno, buf, sizeof(buf));
                ALOGV("Error opening boostpulse: %s\n", buf);
                lge->boostpulse_warned = 1;
            } else if (lge->boostpulse_fd > 0) {
                configure_governor();
                ALOGD("Opened %s boostpulse interface", governor);
            }
        }
    }

    pthread_mutex_unlock(&lge->lock);
    return lge->boostpulse_fd;
}

static void lge_power_hint(struct power_module *module, power_hint_t hint,
                            void *data)
{
    struct lge_power_module *lge = (struct lge_power_module *) module;
    char buf[80];
    int len;
    int duration = 1;

    switch (hint) {
    case POWER_HINT_INTERACTION:
    case POWER_HINT_CPU_BOOST:
        if (boostpulse_open(lge) >= 0) {
            if (data != NULL)
                duration = (int) data;

            snprintf(buf, sizeof(buf), "%d", duration);
            len = write(lge->boostpulse_fd, buf, strlen(buf));

            if (len < 0) {
                strerror_r(errno, buf, sizeof(buf));
	            ALOGE("Error writing to boostpulse: %s\n", buf);

                pthread_mutex_lock(&lge->lock);
                close(lge->boostpulse_fd);
                lge->boostpulse_fd = -1;
                lge->boostpulse_warned = 0;
                pthread_mutex_unlock(&lge->lock);
            }
        }
        break;

    case POWER_HINT_VSYNC:
        break;

    default:
        break;
    }
}

static void lge_power_init(struct power_module *module)
{
    get_scaling_governor();
    configure_governor();
}

static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct lge_power_module HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            version_major: 1,
            version_minor: 0,
            id: POWER_HARDWARE_MODULE_ID,
            name: "HTC-MSM7x27a Power HAL",
            author: "The CyanogenMod Project",
            methods: &power_module_methods,
        },

       init: lge_power_init,
       setInteractive: lge_power_set_interactive,
       powerHint: lge_power_hint,
    },

    lock: PTHREAD_MUTEX_INITIALIZER,
    boostpulse_fd: -1,
    boostpulse_warned: 0,
};
