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

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>

#include <tox/tox.h>
#include <tox/tox_private.h>

#include "util.h"

/* Seconds to wait between new crawler instances */
#define NEW_CRAWLER_INTERVAL 180

/* Maximum number of concurrent crawler instances */
#define MAX_CRAWLERS 4

/* The number of passes we make through the nodes list before giving up */
#define MAX_NUM_PASSES 2

/* Seconds to wait for new nodes before a crawler times out and exits once pass limit is reached */
#define CRAWLER_TIMEOUT 10

/* Default maximum number of nodes the nodes list can store */
#define DEFAULT_NODES_LIST_SIZE 4096

/* Seconds to wait between getnodes requests */
#define GETNODES_REQUEST_INTERVAL 0

/* Max number of nodes to send getnodes requests to per GETNODES_REQUEST_INTERVAL */
#define MAX_GETNODES_REQUESTS 12

/* Number of random node requests to make for each node we send a request to */
#define NUM_RAND_GETNODE_REQUESTS 15

typedef struct DHT_Node {
    uint8_t  public_key[TOX_DHT_NODE_PUBLIC_KEY_SIZE];
    char     ip[TOX_DHT_NODE_IP_STRING_SIZE];
    uint16_t port;
} DHT_Node;

typedef struct Crawler {
    Tox          *tox;
    DHT_Node     **nodes_list;
    uint32_t     num_nodes;
    uint32_t     nodes_list_size;
    uint32_t     send_ptr;    /* index of the oldest node that we haven't sent a getnodes request to */
    time_t       last_new_node;   /* Last time we found an unknown node */
    time_t       last_getnodes_request;
    size_t       passes;  /* How many times we've iterated the full nodes list */
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

static const struct toxNodes {
    const char *ip;
    uint16_t    port;
    const char *key;
} bs_nodes[] = {
    { "144.217.86.39",   33445, "7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C" },
    { "46.229.52.198",   33445, "813C8F4187833EF0655B10F7752141A352248462A567529A38B6BBF73E979307" },
    { "85.172.30.117",   33445, "8E7D0B859922EF569298B4D261A8CCB5FEA14FB91ED412A7603A585A25698832" },
    { "198.199.98.108",  33445, "BEF0CFB37AF874BD17B9A8F9FE64C75521DB95A37D33C5BDB00E9CF58659C04F" },
    { "81.169.136.229",  33445, "E0DB78116AC6500398DDBA2AEEF3220BB116384CAB714C5D1FCD61EA2B69D75E" },
    { "46.101.197.175",  33445, "CD133B521159541FB1D326DE9850F5E56A6C724B5B8E5EB5CD8D950408E95707" },
    { "209.59.144.175",  33445, "214B7FEA63227CAEC5BCBA87F7ABEEDB1A2FF6D18377DD86BF551B8E094D5F1E" },
    { "188.225.9.167",   33445, "1911341A83E02503AB1FD6561BD64AF3A9D6C3F12B5FBB656976B2E678644A67" },
    { "122.116.39.151",  33445, "5716530A10D362867C8E87EE1CD5362A233BAFBBA4CF47FA73B7CAD368BD5E6E" },
    { "195.123.208.139", 33445, "534A589BA7427C631773D13083570F529238211893640C99D1507300F055FE73" },
    { "139.162.110.188", 33445, "F76A11284547163889DDC89A7738CF271797BF5E5E220643E97AD3C7E7903D55" },
    { "198.98.49.206",   33445, "28DB44A3CEEE69146469855DFFE5F54DA567F5D65E03EFB1D38BBAEFF2553255" },
    { "172.105.109.31",  33445, "D46E97CF995DC1820B92B7D899E152A217D36ABE22730FEA4B6BF1BFC06C617C" },
    { "91.146.66.26",    33445, "B5E7DAC610DBDE55F359C7F8690B294C8E4FCEC4385DE9525DBFA5523EAD9D53" },
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

#define MIN(x, y)((x) < (y) ? (x) : (y))

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
        if (memcmp(cwl->nodes_list[i]->public_key, public_key, TOX_DHT_NODE_PUBLIC_KEY_SIZE) == 0) {
            return true;
        }
    }

    return false;
}

void cb_getnodes_response(Tox *tox, const uint8_t *public_key, const char *ip, uint16_t port, void *user_data)
{
    Crawler *cwl = (Crawler *)user_data;

    if (cwl == NULL) {
        return;
    }

    if (public_key == NULL || ip == NULL) {
        return;
    }

    if (node_crawled(cwl, public_key)) {
        return;
    }

    if (cwl->num_nodes + 1 >= cwl->nodes_list_size) {
        DHT_Node **tmp = realloc(cwl->nodes_list, cwl->nodes_list_size * 2 * sizeof(DHT_Node *));

        if (tmp == NULL) {
            return;
        }

        cwl->nodes_list = tmp;
        cwl->nodes_list_size *= 2;
    }

    DHT_Node *new_node = calloc(1, sizeof(DHT_Node));

    if (new_node == NULL) {
        return;
    }

    memcpy(new_node->public_key, public_key, TOX_DHT_NODE_PUBLIC_KEY_SIZE);
    snprintf(new_node->ip, sizeof(new_node->ip), "%s", ip);
    new_node->port = port;

    cwl->nodes_list[cwl->num_nodes++] = new_node;
    cwl->last_new_node = get_time();

    // fprintf(stderr, "Node %u: %s:%u\n", cwl->num_nodes, ip, port);
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
        const DHT_Node *node = cwl->nodes_list[i];

        Tox_Err_Dht_Get_Nodes err;
        tox_dht_get_nodes(cwl->tox, node->public_key, node->ip, node->port, node->public_key, &err);

        const size_t num_rand_requests = MIN(NUM_RAND_GETNODE_REQUESTS / 2, cwl->num_nodes);

        for (size_t j = 0; j < num_rand_requests; ++j) {
            const uint32_t r = rand() % cwl->num_nodes;
            const DHT_Node *rand_node = cwl->nodes_list[r];

            tox_dht_get_nodes(cwl->tox, node->public_key, node->ip, node->port, rand_node->public_key, NULL);
            tox_dht_get_nodes(cwl->tox, rand_node->public_key, rand_node->ip, rand_node->port, node->public_key, NULL);
        }

        ++count;
    }

    cwl->send_ptr = i;
    cwl->last_getnodes_request = get_time();

    if (cwl->send_ptr == cwl->num_nodes) {
        ++cwl->passes;
        cwl->send_ptr = 0;
    }

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

    DHT_Node **nodes_list = calloc(DEFAULT_NODES_LIST_SIZE, sizeof(DHT_Node *));

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

    cwl->tox = tox;
    cwl->nodes_list = nodes_list;
    cwl->nodes_list_size = DEFAULT_NODES_LIST_SIZE;

    tox_callback_dht_get_nodes_response(tox, cb_getnodes_response);

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

    for (uint32_t i = 0; i < cwl->num_nodes; ++i) {
        fprintf(fp, "%s ", cwl->nodes_list[i]->ip);
    }

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

    for (size_t i = 0; i < cwl->num_nodes; ++i) {
        free(cwl->nodes_list[i]);
    }

    free(cwl->nodes_list);
    free(cwl);
}

/* Returns true if the crawler is unable to find new nodes in the DHT or the exit flag has been triggered */
static bool crawler_finished(Crawler *cwl)
{
    LOCK;
    if (FLAG_EXIT || (cwl->passes >= MAX_NUM_PASSES && timed_out(cwl->last_new_node, CRAWLER_TIMEOUT))) {
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
        tox_iterate(cwl->tox, cwl);
        send_node_requests(cwl);
        usleep(tox_iteration_interval(cwl->tox) * 1000);
    }

    char time_format[128];
    get_time_format(time_format, sizeof(time_format));
    fprintf(stderr, "[%s] Nodes: %llu\n", time_format, (unsigned long long) cwl->num_nodes);

    LOCK;
    const bool interrupted = FLAG_EXIT;
    UNLOCK;

    if (!interrupted) {
        const int ret = crawler_dump_log(cwl);

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

    const int ret = init_crawler_thread(cwl);

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

        const int ret = do_thread_control();

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
