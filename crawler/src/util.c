/*  util.c
 *
 *
 *  Copyright (C) 2016-2024 toxcrawler All Rights Reserved.
 *
 *  This file is part of toxcrawler.
 *
 *  toxcrawler is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  toxcrawler is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with toxcrawler.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "util.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

void sleep_thread(long int usec)
{
    struct timespec req;
    struct timespec rem;

    req.tv_sec = 0;
    req.tv_nsec = usec * 1000L;

    if (nanosleep(&req, &rem) == -1) {
        if (nanosleep(&rem, NULL) == -1) {
            fprintf(stderr, "nanosleep() returned -1\n");
        }
    }
}

int32_t min(int32_t x, int32_t y)
{
    return x < y ? x : y;
}

time_t get_time(void)
{
    return time(NULL);
}

bool timed_out(time_t timestamp, time_t timeout)
{
    return timestamp + timeout <= get_time();
}

void get_time_format(char *buf, int bufsize)
{
    struct tm *timeinfo;
    const time_t t = get_time();
    timeinfo = localtime((const time_t*) &t);
    strftime(buf, bufsize, "%H:%M:%S", timeinfo);
}

int hex_string_to_bin(const char *hex_string, size_t hex_len, char *output, size_t output_size)
{
    if (output_size == 0 || hex_len != output_size * 2) {
        return -1;
    }

    for (size_t i = 0; i < output_size; ++i) {
        sscanf(hex_string, "%2hhx", (unsigned char *) &output[i]);
        hex_string += 2;
    }

    return 0;
}

#define BASE_LOG_PATH "../crawler_logs"
int get_log_path(char *buf, size_t buf_len)
{
    const time_t tm = get_time();

    char tmstr[32];
    strftime(tmstr, sizeof(tmstr), "%Y-%m-%d", localtime(&tm));

    char path[strlen(BASE_LOG_PATH) + strlen(tmstr) + 3];
    snprintf(path, sizeof(path), "%s/%s/", BASE_LOG_PATH, tmstr);

    struct stat st;

    if (stat(BASE_LOG_PATH, &st) == -1) {
        if (mkdir(BASE_LOG_PATH, 0700) == -1) {
            return -1;
        }
    }

    if (stat(path, &st) == -1) {
        if (mkdir(path, 0700) == -1) {
            return -1;
        }
    }

    snprintf(buf, buf_len, "%s/%llu.cwl", path, (long long unsigned) tm);

    return 0;
}

