/*  main.c
 *
 *
 *  Copyright (C) 2016 toxcrawler All Rights Reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>

#include <tox/tox.h>
#include "../../../toxcore/toxcore/DHT.h"
#include "../../../toxcore/toxcore/Messenger.h"

#include "util.h"

/* Seconds to wait between new crawler instances */
#define NEW_CRAWLER_INTERVAL 180

/* Maximum number of concurrent crawler instances */
#define MAX_CRAWLERS 6

/* Number of seconds to wait for new nodes before a crawler times out and exits */
#define CRAWLER_TIMEOUT 20

/* Default maximum number of nodes the nodes list can store */
#define DEFAULT_NODES_LIST_SIZE 4096

/* Seconds to wait between getnodes requests */
#define GETNODES_REQUEST_INTERVAL 1

/* Max number of nodes to send getnodes requests to per GETNODES_REQUEST_INTERVAL */
#define MAX_GETNODES_REQUESTS 4

/* Number of random node requests to make for each node we send a request to */
#define NUM_RAND_GETNODE_REQUESTS 32


typedef struct Crawler {
    Tox         *tox;
    DHT         *dht;
    Node_format *nodes_list;
    uint32_t     num_nodes;
    uint32_t     nodes_list_size;
    uint32_t     send_ptr;    /* index of the oldest node that we haven't sent a getnodes request to */
    time_t       last_new_node;   /* Last time we found an unknown node */
    time_t       last_getnodes_request;

    pthread_t      tid;
    pthread_attr_t attr;
} Crawler;


/* Use these to lock and unlock the global threads struct */
#define LOCK   pthread_mutex_lock(&threads.lock)
#define UNLOCK pthread_mutex_unlock(&threads.lock)

struct Threads {
    uint16_t  num_active;
    time_t    last_created;
    pthread_mutex_t lock;
} threads;


static struct toxNodes {
    const char *ip;
    uint16_t    port;
    const char *key;
} bs_nodes[] = {
    { "51.254.84.212",   33445, "AEC204B9A4501412D5F0BB67D9C81B5DB3EE6ADA64122D32A3E9B093D544327D" },
    { "185.14.30.213",   443,   "2555763C8C460495B14157D234DD56B86300A2395554BCAE4621AC345B8C1B1B" },
    { "144.217.86.39",   33445, "7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C" },
    { "37.48.122.22",    33445, "1B5A8AB25FFFB66620A531C4646B47F0F32B74C547B30AF8BD8266CA50A3AB59" },
    { "46.229.52.198",   33445, "813C8F4187833EF0655B10F7752141A352248462A567529A38B6BBF73E979307" },
    { "85.172.30.117",   33445, "8E7D0B859922EF569298B4D261A8CCB5FEA14FB91ED412A7603A585A25698832" },
    { "163.172.136.118", 33445, "2C289F9F37C20D09DA83565588BF496FAB3764853FA38141817A72E3F18ACA0B" },
    { "78.46.73.141",    33445, "02807CF4F8BB8FB390CC3794BDF1E8449E9A8392C5D3F2200019DA9F1E812E46" },
    { "95.31.18.227",    33445, "257744DBF57BE3E117FE05D145B5F806089428D4DCE4E3D0D50616AA16D9417E" },
    { "198.199.98.108",  33445, "BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F" },
    { "52.53.185.100",   33445, "A04F5FE1D006871588C8EC163676458C1EC75B20B4A147433D271E1E85DAF839" },
    { "116.196.77.132",  33445, "040326E850DDCB49B1B2D9E3E2789D425774E4C5D783A55C09A024D05D2A8A66" },
    { "87.118.126.207",  33445, "0D303B1778CA102035DA01334E7B1855A45C3EFBC9A83B9D916FFDEBC6DD3B2E" },
    { "81.169.136.229",  33445, "D031DAC44F00464D3C9636F9850BF0064BC37FEB55789A13B6F59052CAE8A958" },
    { NULL, 0, NULL },
};

/* Attempts to bootstrap to every listed bootstrap node */
static void bootstrap_tox(Crawler *cwl)
{
    for (size_t i = 0; bs_nodes[i].ip != NULL; ++i) {
        char bin_key[TOX_PUBLIC_KEY_SIZE];
        if (hex_string_to_bin(bs_nodes[i].key, strlen(bs_nodes[i].key), bin_key, sizeof(bin_key)) == -1) {
            continue;
        }

        TOX_ERR_BOOTSTRAP err;
        tox_bootstrap(cwl->tox, bs_nodes[i].ip, bs_nodes[i].port, (uint8_t *) bin_key, &err);

        if (err != TOX_ERR_BOOTSTRAP_OK) {
            fprintf(stderr, "Failed to bootstrap DHT via: %s %d (error %d)\n", bs_nodes[i].ip, bs_nodes[i].port, err);
        }
    }
}

static volatile bool FLAG_EXIT = false;
static void catch_SIGINT(int sig)
{
    LOCK;
    FLAG_EXIT = true;
    UNLOCK;
}

/*
 * Return true if public_key is in the crawler's nodes list.
 * TODO: A hashtable would be nice but the str8C holds up for now.
 */
static bool node_crawled(Crawler *cwl, const uint8_t *public_key)
{
    for (uint32_t i = 0; i < cwl->num_nodes; ++i) {
        if (memcmp(public_key, cwl->nodes_list[i].public_key, TOX_PUBLIC_KEY_SIZE) == 0) {
            return true;
        }
    }

    return false;
}

void cb_getnodes_response(IP_Port *ip_port, const uint8_t *public_key, void *object)
{
    Crawler *cwl = object;

    if (node_crawled(cwl, public_key)) {
        return;
    }

    if (cwl->num_nodes + 1 >= cwl->nodes_list_size) {
        Node_format *tmp = realloc(cwl->nodes_list, cwl->nodes_list_size * 2 * sizeof(Node_format));

        if (tmp == NULL) {
            return;
        }

        cwl->nodes_list = tmp;
        cwl->nodes_list_size *= 2;
    }

    Node_format node;
    memcpy(&node.ip_port, ip_port, sizeof(IP_Port));
    memcpy(node.public_key, public_key, TOX_PUBLIC_KEY_SIZE);
    memcpy(&cwl->nodes_list[cwl->num_nodes++], &node, sizeof(Node_format));
    cwl->last_new_node = get_time();
}

/*
 * Sends a getnodes request to up to MAX_GETNODES_REQUESTS nodes in the nodes list that have not been queried.
 * Returns the number of requests sent.
 */
static size_t send_node_requests(Crawler *cwl)
{
    if (!timed_out(cwl->last_getnodes_request, GETNODES_REQUEST_INTERVAL)) {
        return 0;
    }

    size_t count = 0;
    uint32_t i;

    for (i = cwl->send_ptr; count < MAX_GETNODES_REQUESTS && i < cwl->num_nodes; ++i) {
        dht_getnodes(cwl->dht, &cwl->nodes_list[i].ip_port,
                     cwl->nodes_list[i].public_key,
                     cwl->nodes_list[i].public_key);

        for (size_t j = 0; j < NUM_RAND_GETNODE_REQUESTS; ++j) {
            int r = rand() % cwl->num_nodes;

            dht_getnodes(cwl->dht, &cwl->nodes_list[i].ip_port,
                         cwl->nodes_list[i].public_key,
                         cwl->nodes_list[r].public_key);
        }

        ++count;
    }

    cwl->send_ptr = i;
    cwl->last_getnodes_request = get_time();

    return count;
}

/*
 * Returns a pointer to an inactive crawler in the threads array.
 * Returns NULL if there are no crawlers available.
 */
Crawler *crawler_new(void)
{
    Crawler *cwl = calloc(1, sizeof(Crawler));

    if (cwl == NULL) {
        return cwl;
    }

    Node_format *nodes_list = malloc(DEFAULT_NODES_LIST_SIZE * sizeof(Node_format));

    if (nodes_list == NULL) {
        free(cwl);
        return NULL;
    }

    struct Tox_Options options;
    tox_options_default(&options);

    TOX_ERR_NEW err;
    Tox *tox = tox_new(&options, &err);

    if (err != TOX_ERR_NEW_OK || tox == NULL) {
        fprintf(stderr, "tox_new() failed: %d\n", err);
        free(cwl);
        free(nodes_list);
        return NULL;
    }

    Messenger *m = *(Messenger **) tox;   // Casting fuckery so we can access the DHT object directly
    cwl->dht = m->dht;
    cwl->tox = tox;
    cwl->nodes_list = nodes_list;
    cwl->nodes_list_size = DEFAULT_NODES_LIST_SIZE;

    DHT_callback_getnodes_response(cwl->dht, cb_getnodes_response, cwl);

    cwl->last_getnodes_request = get_time();
    cwl->last_new_node = get_time();

    bootstrap_tox(cwl);

    return cwl;
}

#define TEMP_FILE_EXT ".tmp"

/* Dumps crawler nodes list to log file. */
static int crawler_dump_log(Crawler *cwl)
{
    char log_path[PATH_MAX];
    if (get_log_path(log_path, sizeof(log_path)) == -1) {
        return -1;
    }

    char log_path_temp[strlen(log_path) + strlen(TEMP_FILE_EXT) + 1];
    snprintf(log_path_temp, sizeof(log_path_temp), "%s%s", log_path, TEMP_FILE_EXT);

    FILE *fp = fopen(log_path_temp, "w");

    if (fp == NULL) {
        return -2;
    }

    LOCK;   // ip_ntoa() isn't thread safe
    for (uint32_t i = 0; i < cwl->num_nodes; ++i) {
        char ip_str[IP_NTOA_LEN];
        ip_ntoa(&cwl->nodes_list[i].ip_port.ip, ip_str, sizeof(ip_str));
        fprintf(fp, "%s ", ip_str);
    }
    UNLOCK;

    fclose(fp);

    if (rename(log_path_temp, log_path) != 0) {
        return -3;
    }

    return 0;
}

static void crawler_kill(Crawler *cwl)
{
    pthread_attr_destroy(&cwl->attr);
    tox_kill(cwl->tox);
    free(cwl->nodes_list);
    free(cwl);
}

/* Returns true if the crawler is unable to find new nodes in the DHT or the exit flag has been triggered */
static bool crawler_finished(Crawler *cwl)
{
    LOCK;
    if (FLAG_EXIT || (cwl->send_ptr == cwl->num_nodes && timed_out(cwl->last_new_node, CRAWLER_TIMEOUT))) {
        UNLOCK;
        return true;
    }
    UNLOCK;

    return false;
}

void *do_crawler_thread(void *data)
{
    Crawler *cwl = (Crawler *) data;

    while (!crawler_finished(cwl)) {
        tox_iterate(cwl->tox, NULL);
        send_node_requests(cwl);
        usleep(tox_iteration_interval(cwl->tox) * 1000);
    }

    char time_format[128];
    get_time_format(time_format, sizeof(time_format));
    fprintf(stderr, "[%s] Nodes: %llu\n", time_format, (unsigned long long) cwl->num_nodes);

    LOCK;
    bool interrupted = FLAG_EXIT;
    UNLOCK;

    if (!interrupted) {
        int ret = crawler_dump_log(cwl);

        if (ret < 0) {
            fprintf(stderr, "crawler_dump_log() failed with error %d\n", ret);
        }
    }

    crawler_kill(cwl);

    LOCK;
    --threads.num_active;
    UNLOCK;

    pthread_exit(0);
}

/* Initializes a crawler thread.
 *
 * Returns 0 on success.
 * Returns -1 if thread attributes cannot be set.
 * Returns -2 if thread state cannot be set.
 * Returns -3 if thread cannot be created.
 */
static int init_crawler_thread(Crawler *cwl)
{
    if (pthread_attr_init(&cwl->attr) != 0) {
        return -1;
    }

    if (pthread_attr_setdetachstate(&cwl->attr, PTHREAD_CREATE_DETACHED) != 0) {
        pthread_attr_destroy(&cwl->attr);
        return -2;
    }

    if (pthread_create(&cwl->tid, NULL, do_crawler_thread, (void *) cwl) != 0) {
        pthread_attr_destroy(&cwl->attr);
        return -3;
    }

    return 0;
}

/*
 * Creates new crawler instances.
 *
 * Returns 0 on success or if new instance is not needed.
 * Returns -1 if crawler instance fails to initialize.
 * Returns -2 if thread fails to initialize.
 */
static int do_thread_control(void)
{
    LOCK;
    if (threads.num_active >= MAX_CRAWLERS || !timed_out(threads.last_created, NEW_CRAWLER_INTERVAL)) {
        UNLOCK;
        return 0;
    }
    UNLOCK;

    Crawler *cwl = crawler_new();

    if (cwl == NULL) {
        return -1;
    }

    int ret = init_crawler_thread(cwl);

    if (ret != 0) {
        fprintf(stderr, "init_crawler_thread() failed with error: %d\n", ret);
        return -2;
    }

    threads.last_created = get_time();

    LOCK;
    ++threads.num_active;
    UNLOCK;

    return 0;
}

int main(int argc, char **argv)
{
    if (pthread_mutex_init(&threads.lock, NULL) != 0) {
        fprintf(stderr, "pthread mutex failed to init in main()\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, catch_SIGINT);

    while (true) {
        LOCK;
        if (FLAG_EXIT) {
            UNLOCK;
            break;
        }
        UNLOCK;

        int ret = do_thread_control();

        if (ret < 0) {
            fprintf(stderr, "do_thread_control() failed with error %d\n", ret);
            sleep(5);
        } else {
            usleep(10000);
        }
    }

    /* Wait for threads to exit cleanly */
    while (true) {
        LOCK;
        if (threads.num_active == 0) {
            UNLOCK;
            break;
        }
        UNLOCK;

        usleep(10000);
    }

    return 0;
}
