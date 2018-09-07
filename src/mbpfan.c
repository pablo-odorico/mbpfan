/**
 *  mbpfan.c - automatically control fan for MacBook Pro
 *  Copyright (C) 2010  Allan McRae <allan@archlinux.org>
 *  Modifications by Rafael Vega <rvega@elsoftwarehamuerto.org>
 *  Modifications (2012) by Ismail Khatib <ikhatib@gmail.com>
 *  Modifications (2012-present) by Daniel Graziotin <daniel@ineed.coffee> [CURRENT MAINTAINER]
 *  Modifications (2017-present) by Robert Musial <rmusial@fastmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *
 *  Notes:
 *    Assumes any number of processors and fans (max. 10)
 *    It uses only the temperatures from the processors as input.
 *    Requires coretemp and applesmc kernel modules to be loaded.
 *    Requires root use
 *
 *  Tested models: see README.md
 */


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <syslog.h>
#include <stdbool.h>
#include <sys/utsname.h>
#include <sys/errno.h>
#include "mbpfan.h"
#include "global.h"
#include "settings.h"

/* lazy min/max... */
#define min(a,b) ((a) < (b) ? (a) : (b))
#define max(a,b) ((a) > (b) ? (a) : (b))

int min_fan_speed = -1;
int max_fan_speed = -1;

/* temperature thresholds
 * low_temp - temperature below which fan speed will be at minimum
 * high_temp - fan will increase speed when higher than this temperature
 * max_temp - fan will run at full speed above this temperature */
int low_temp = 63;   // try ranges 55-63
int high_temp = 66;  // try ranges 58-66
int max_temp = 86;   // do not set it > 90

int polling_interval = 7;

// Per-fan settings
char* fan_list = NULL;
float fan_ratios[MAX_FANS];
int fan_min_speeds[MAX_FANS];
int fan_max_speeds[MAX_FANS];

float* pid_values = NULL;

t_sensors* sensors = NULL;
t_fans* fans = NULL;

char *smprintf(const char *fmt, ...)
{
    char *buf;
    int cnt;
    va_list ap;

    // find buffer length
    va_start(ap, fmt);
    cnt = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (cnt < 0) {
        return NULL;
    }

    // create and write to buffer
    buf = malloc(cnt + 1);
    va_start(ap, fmt);
    vsnprintf(buf, cnt + 1, fmt, ap);
    va_end(ap);
    return buf;
}

bool is_modern_sensors_path()
{
    struct utsname kernel;
    uname(&kernel);

    char *str_kernel_version;
    str_kernel_version = strtok(kernel.release, ".");

    if (atoi(str_kernel_version) < 3){
        FAIL("mbpfan detected a pre-3.x.x linux kernel. Detected version: %s. Exiting.", kernel.release);
    }

    int counter;

    for (counter = 0; counter < 10; counter++) {
        int temp;
        for (temp = 1; temp < 10; ++temp) {
            char *path = smprintf("/sys/devices/platform/coretemp.0/hwmon/hwmon%d/temp%d_input", counter, temp);
            int res = access(path, R_OK);
            free(path);
            if (res == 0) {
                return 1;
            }
        }
    }

    return 0;
}


t_sensors *retrieve_sensors()
{

    t_sensors *sensors_head = NULL;
    t_sensors *s = NULL;

    char *path = NULL;
    char *path_begin = NULL;

    if (!is_modern_sensors_path()) {
        if(verbose) {
            LOG("Using legacy sensor path for kernel < 3.15.0");
        }

        path_begin = strdup("/sys/devices/platform/coretemp.0/temp");

    } else {

        if(verbose) {
            LOG("Using new sensor path for kernel >= 3.15.0 or some CentOS versions with kernel 3.10.0");
        }

        path_begin = strdup("/sys/devices/platform/coretemp.0/hwmon/hwmon");

        int counter;
        for (counter = 0; counter < 10; counter++) {

            char hwmon_path[strlen(path_begin)+2];

            sprintf(hwmon_path, "%s%d", path_begin, counter);

            int res = access(hwmon_path, R_OK);
            if (res == 0) {

                free(path_begin);
                path_begin = smprintf("%s/temp", hwmon_path);

                if(verbose) {
                    LOG("Found hwmon path at %s", path_begin);
                }

                break;
            }
        }
    }

    const char *path_end = "_input";

    int sensors_found = 0;

    int counter = 0;
    for(counter = 0; counter<10; counter++) {
        path = smprintf("%s%d%s", path_begin, counter, path_end);

        FILE *file = fopen(path, "r");

        if(file != NULL) {
            s = (t_sensors *) malloc( sizeof( t_sensors ) );
            s->path = strdup(path);
            fscanf(file, "%d", &s->temperature);

            if (sensors_head == NULL) {
                sensors_head = s;
                sensors_head->next = NULL;

            } else {
                t_sensors *tmp = sensors_head;

                while (tmp->next != NULL) {
                    tmp = tmp->next;
                }

                tmp->next = s;
                tmp->next->next = NULL;
            }

            s->file = file;
            sensors_found++;
        }

        free(path);
        path = NULL;
    }

    if(verbose) {
        LOG("Found %d sensors", sensors_found);
    }

    if (sensors_found == 0) {
        FAIL("mbpfan could not detect any temp sensor. Please contact the developer.");
    }

    free(path_begin);
    path_begin = NULL;

    return sensors_head;
}

void populate_fan_list()
{
    free(fan_list);
    fan_list = malloc(10 * MAX_FANS);
    *fan_list = '\0';

    for (int counter = 0; counter < MAX_SEARCH_FANS; counter++) {
        char* path_label = smprintf("/sys/devices/platform/applesmc.768/fan%d_label", counter);

        FILE* file = fopen(path_label, "r");
        if (file != NULL) {
            char label[1024];
            memset(label, 0, sizeof(label));
            fread(label, sizeof(label), 1, file);

            // Remove trailing spaces
            char* last = label + strlen(label) - 1;
            while (isspace(*last) && last != label) last--;
            *(last + 1) = '\0';

            strncat(fan_list, label, sizeof(label));
            strncat(fan_list, ",", sizeof(","));

            fclose(file);
        }

        free(path_label);
    }
    // Remove trailing ,
    if (*fan_list) {
        fan_list[strlen(fan_list) - 1] = '\0';
    }
}

t_fans* retrieve_fans()
{
    if (!fan_list) {
        populate_fan_list();
    }

    LOG("fan_list: %s", fan_list);

    int fan_count = 0;
    char* fan_names[MAX_FANS];

    // Split comma-delimited fan_list info fan_names and fan_count
    char* fan_list_copy = strdup(fan_list);
    char* fan_list_temp = fan_list_copy;
    char* fan_name = NULL;

    while ((fan_name = strsep(&fan_list_temp, ",")) && (fan_count < MAX_FANS)) {
        fan_names[fan_count] = strdup(fan_name);
        fan_count++;
    }

    free(fan_list_copy);

    if (fan_count == 0) {
        FAIL("mbpfan could not detect any fan. Please contact the developer.");
    }

    // Read up to MAX_SEARCH_FANS labels from fan#_label
    char labels[MAX_SEARCH_FANS][100];
    memset(labels, 0, sizeof(labels));

    for (int counter = 0; counter < MAX_SEARCH_FANS; counter++) {
        char* path_label = smprintf("/sys/devices/platform/applesmc.768/fan%d_label", counter);

        FILE *file = fopen(path_label, "r");
        if (file != NULL) {
            fread(labels[counter], sizeof(labels[counter]), 1, file);
            labels[counter][strlen(labels[counter]) - 2] = '\0';

            fclose(file);
        }

        free(path_label);
    }

    // Create fan list
    t_fans* fans_head = NULL;
    t_fans* fan = NULL;

    for (int fan_counter = 0; fan_counter < fan_count; fan_counter++) {

        t_fans* new_fan = (t_fans*)malloc(sizeof(t_fans));
        memset(new_fan, 0, sizeof(t_fans));
        new_fan->name = fan_names[fan_counter];
        new_fan->speed_ratio = fan_ratios[fan_counter];
        new_fan->max_speed = fan_max_speeds[fan_counter];
        new_fan->min_speed = fan_min_speeds[fan_counter];

        if (fan != NULL) fan->next = new_fan;
        fan = new_fan;

        if (fans_head == NULL) fans_head = fan;

        // Find fan ID matching the name
        fan->fan_id = -1;
        for (int counter = 0; counter < MAX_SEARCH_FANS; counter++) {
            if (strncmp(labels[counter], fan_names[fan_counter], sizeof(labels[counter])) == 0) {
                fan->fan_id = counter;
            }
        }

        if (fan->fan_id == -1) {
            FAIL("Unable to find ID of fan '%s'", fan_names[fan_counter]);
        }

        fan->fan_output_path = smprintf("/sys/devices/platform/applesmc.768/fan%d_output", fan->fan_id);
        fan->fan_manual_path = smprintf("/sys/devices/platform/applesmc.768/fan%d_manual", fan->fan_id);

        fan->file = fopen(fan->fan_output_path, "w");
        if(fan->file == NULL) {
            FAIL("Unable to open '%s'", fan->fan_output_path);
        }
    }

    if(verbose) {
        for (fan = fans_head; fan != NULL; fan = fan->next) {
            LOG("%9s: fan%d, ratio %.01f, max %4d RPM, min %4d RPM",
                fan->name, fan->fan_id, fan->speed_ratio, fan->min_speed, fan->max_speed);
        }
    }

    return fans_head;
}

static void set_fans_mode(t_fans *fans, int mode)
{
    t_fans *tmp = fans;
    FILE *file;

    while(tmp != NULL) {
        file = fopen(tmp->fan_manual_path, "rw+");

        if(file != NULL) {
            fprintf(file, "%d", mode);
            fclose(file);
        }

        tmp = tmp->next;
    }
}

void set_fans_man(t_fans *fans)
{
    set_fans_mode(fans, 1);
}

void set_fans_auto(t_fans *fans)
{
    set_fans_mode(fans, 0);
}

t_sensors *refresh_sensors(t_sensors *sensors)
{
    t_sensors *tmp = sensors;

    while(tmp != NULL) {
        if(tmp->file != NULL) {
            char buf[16];
            int len = pread(fileno(tmp->file), buf, sizeof(buf), /*offset=*/ 0);
            buf[len] = '\0';
            sscanf(buf, "%d", &tmp->temperature);
        }

        tmp = tmp->next;
    }

    return sensors;
}


/* Controls the speed of the fan */
void set_fan_speed(t_fans* fans, int speed)
{
    t_fans* fan = fans;
    while(fan != NULL) {
        const int fan_speed = max(min(speed * fan->speed_ratio, fan->max_speed), fan->min_speed);
        /*
        if (verbose) {
            LOG("%9s: %d * %.01f = %.0f -> %4d RPM (min %4d max %4d)",
                fan->name, speed, fan->speed_ratio, speed * fan->speed_ratio,
                fan_speed, fan->min_speed, fan->max_speed);
        }
        */

        if(fan->file != NULL && fan->old_speed != speed) {
            char buf[16];
            int len = snprintf(buf, sizeof(buf), "%d", fan_speed);
            int res = pwrite(fileno(fan->file), buf, len, /*offset=*/ 0);
            if (res == -1) {
                perror("Could not set fan speed");
            }
            fan->old_speed = fan_speed;
        }

        fan = fan->next;
    }
}


float get_temp(t_sensors* sensors)
{
    sensors = refresh_sensors(sensors);
    int sum_temp = 0;

    t_sensors* sensor = sensors;
    int number_sensors = 0;

    while (sensor != NULL) {
        sum_temp += sensor->temperature;
        sensor = sensor->next;
        number_sensors++;
    }

    // just to be safe
    if (number_sensors == 0) {
        number_sensors++;
    }

    return (float)sum_temp / (number_sensors * 1000);
}


void retrieve_settings(const char* settings_path)
{
    Settings *settings = NULL;
    int result = 0;
    FILE *f = NULL;

    if (settings_path == NULL) {
        f = fopen("/etc/mbpfan.conf", "r");

    } else {
        f = fopen(settings_path, "r");
    }

    if (f == NULL) {
        /* Could not open configfile */
        if(verbose) {
            LOG("Couldn't open configfile, using defaults");
        }

    } else {
        settings = settings_open(f);
        fclose(f);

        if (settings == NULL) {
            /* Could not read configfile */
            if(verbose) {
                LOG("Couldn't read configfile");
            }

        } else {
            /* Read configfile values */
            result = settings_get_int(settings, "general", "min_fan_speed");

            if (result != 0) {
                min_fan_speed = result;
            }

            result = settings_get_int(settings, "general", "max_fan_speed");

            if (result != 0) {
                max_fan_speed = result;
            }

            result = settings_get_int(settings, "general", "low_temp");

            if (result != 0) {
                low_temp = result;
            }

            result = settings_get_int(settings, "general", "high_temp");

            if (result != 0) {
                high_temp = result;
            }

            result = settings_get_int(settings, "general", "max_temp");

            if (result != 0) {
                max_temp = result;
            }

            result = settings_get_int(settings, "general", "polling_interval");

            if (result != 0) {
                polling_interval = result;
            }

            char fan_list_temp[10 * MAX_FANS];
            result = settings_get(settings, "general", "fan_list", fan_list_temp, sizeof(fan_list_temp));

            if (result != 0) {
                fan_list = strdup(fan_list_temp);
            }

            double fan_ratios_temp[MAX_FANS];
            result = settings_get_double_tuple(settings, "general", "fan_ratios", fan_ratios_temp, MAX_FANS, NULL);

            for (int i = 0; i < MAX_FANS; i++) {
                fan_ratios[i] = (result != 0) ? max(0.1, fan_ratios_temp[i]) : 1.0;
            }

            result = settings_get_int_tuple(settings, "general", "fan_min_speeds", fan_min_speeds, MAX_FANS, NULL);

            if (result == 0) {
                for (int i = 0; i < MAX_FANS; i++) fan_min_speeds[i] = min_fan_speed;
            }

            result = settings_get_int_tuple(settings, "general", "fan_max_speeds", fan_max_speeds, MAX_FANS, NULL);

            if (result == 0) {
                for (int i = 0; i < MAX_FANS; i++) fan_max_speeds[i] = max_fan_speed;
            }

            int pid_values_temp[10];
            unsigned int readCount = 0;
            result = settings_get_int_tuple(settings, "general", "pid_values", pid_values_temp, 10, &readCount);

            if (result != 0) {
                if (readCount != 3) FAIL("Wrong number of PID constants, 3 expected.");
                pid_values = malloc(sizeof(float) * 3);
                pid_values[0] = pid_values_temp[0];
                pid_values[1] = pid_values_temp[1];
                pid_values[2] = pid_values_temp[2];
            }

            /* Destroy the settings object */
            settings_delete(settings);
        }
    }

    // Sanity checks
    if (min_fan_speed > max_fan_speed) {
        FAIL("Invalid fan speeds: min_fan_speed %d, max_fan_speed %d", min_fan_speed, max_fan_speed);
    }
    if (low_temp > high_temp || high_temp > max_temp) {
        FAIL("Invalid temperatures: low_temp %d, high_temp %d, max_temp %d", low_temp, high_temp, max_temp);
    }
}

//
// "Classic" fan control
//

typedef struct
{
    int step_up;
    int step_down;
    int fan_speed;
    int old_temp;
} t_state_classic;

void fan_speed_classic_init(t_state_classic* state, float start_temperature)
{
    memset(state, 0, sizeof(*state));

    state->step_up = (float)( max_fan_speed - min_fan_speed ) /
                     (float)( ( max_temp - high_temp ) * ( max_temp - high_temp + 1 ) / 2 );

    state->step_down = (float)( max_fan_speed - min_fan_speed ) /
                       (float)( ( max_temp - low_temp ) * ( max_temp - low_temp + 1 ) / 2 );

    state->fan_speed = min_fan_speed;
    state->old_temp = start_temperature;

    LOG("Classic control initialized.");
}

int fan_speed_classic(float temperature, t_state_classic* state)
{
    const int new_temp = temperature; // Keep int logic for classic
    const int temp_change = new_temp - state->old_temp;
    state->old_temp = new_temp;

    if(new_temp >= max_temp && state->fan_speed != max_fan_speed) {
        return max_fan_speed;
    }

    if(new_temp <= low_temp && state->fan_speed != min_fan_speed) {
        return min_fan_speed;
    }

    if(temp_change > 0 && new_temp > high_temp && new_temp < max_temp) {
        const int steps = ( new_temp - high_temp ) * ( new_temp - high_temp + 1 ) / 2;
        return max( state->fan_speed, ceil(min_fan_speed + steps * state->step_up) );
    }

    if(temp_change < 0 && new_temp > low_temp && new_temp < max_temp) {
        const int steps = ( max_temp - new_temp ) * ( max_temp - new_temp + 1 ) / 2;
        return min( state->fan_speed, floor(max_fan_speed - steps * state->step_down) );
    }

    return min_fan_speed;
}

//
// PID fan control
//

typedef struct
{
    float error_prior;
    float integral;
    int last_speed;
} t_state_pid;

void fan_speed_pid_init(t_state_pid* state)
{
    memset(state, 0, sizeof(*state));

    state->error_prior = 0;
    state->integral = 0;
    state->last_speed = 0;
    LOG("PID control initialized. Kp=%.1f Ki=%.1f Kd=%.1f", pid_values[0], pid_values[1], pid_values[2]);
}

int fan_speed_pid(float temperature, t_state_pid* state)
{
    if (temperature > low_temp)
    {
        const float error = temperature - high_temp; // high_temp is the target temperature
        state->integral = state->integral + (error * polling_interval);

        const int p = pid_values[0] * error;
        const int i = pid_values[1] * state->integral;
        const int d = pid_values[2] * (error - state->error_prior) / polling_interval;

        const int new_speed = max(min_fan_speed + p + i + d, min_fan_speed); // min_fan_speed is the bias
        if (verbose) {
            const int delta = new_speed - state->last_speed;
            LOG("PID: Error %.1fC. P=%d I=%d D=%d -> %d RPM (%+d RPM)",
                error, p, i, d, new_speed, delta);
        }

        state->last_speed = new_speed;
        state->error_prior = error;
    }
    else
    {
        // Discard PID state once we go below low_temp and set min_fan_speed
        state->last_speed = min_fan_speed;
        state->integral = 0;
        state->error_prior = 0;
    }

    return state->last_speed;
}

void mbpfan()
{
    retrieve_settings(NULL);

    sensors = retrieve_sensors();
    fans = retrieve_fans();

    set_fans_man(fans);

    float temp = get_temp(sensors);

    set_fan_speed(fans, min_fan_speed);

    if(verbose) {
        LOG("Sleeping for 2 seconds to get first temp delta.");
    }
    sleep(2);

    t_state_pid state_pid;
    t_state_classic state_classic;
    if (pid_values) {
        fan_speed_pid_init(&state_pid);
    } else {
        fan_speed_classic_init(&state_classic, temp);
    }

    while(1) {

        temp = get_temp(sensors);

        const int fan_speed = pid_values
            ? fan_speed_pid(temp, &state_pid)
            : fan_speed_classic(temp, &state_classic);

        if(verbose) {
            LOG("Temperature: %.1f C. Base Speed: %d RPM", temp, fan_speed);
        }

        set_fan_speed(fans, fan_speed);

        if(verbose) {
            fflush(stdout);
        }

        // call nanosleep instead of sleep to avoid rt_sigprocmask and rt_sigaction
        struct timespec ts;
        ts.tv_sec = polling_interval;
        ts.tv_nsec = 0;
        nanosleep(&ts, NULL);
    }
}
