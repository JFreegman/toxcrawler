/*  util.h
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

#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Sleeps the calling thread for `usec` nanoseconds. */
void sleep_thread(long int usec);

/* Returns the smaller value between x and y. */
int32_t min(int32_t x, int32_t y);

/* Returns the current unix time. */
time_t get_time(void);

/* Returns true if timestamp has timed out according to timeout value. */
bool timed_out(time_t timestamp, time_t timeout);

/* Puts the current time in buf in the format of [HH:mm:ss] */
void get_time_format(char *buf, int bufsize);

/*
 * Converts a hexidecimal string of length hex_len to binary format and puts the result in output.
 * output_size must be exactly half of hex_len.
 *
 * Returns 0 on success.
 * Returns -1 on failure.
 */
int hex_string_to_bin(const char *hex_string, size_t hex_len, char *output, size_t output_size);

/* Puts log file path into buf in the form: BASE_LOG_PATH/YY-mm-dd/unixtime
 * The date is the current day, and the unixtime is the current unixtime.
 *
 * If the current day's directory does not exist it is automatically created.
 *
 * Returns 0 on success.
 * Returns -1 on failure.
 */
int get_log_path(char *buf, size_t buf_len);

#endif  /* UTIL_H */
