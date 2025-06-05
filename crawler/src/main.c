/*  main.c
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

#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <tox/tox.h>
#include <tox/tox_private.h>

/* Seconds to wait between new crawler instances */
#define NEW_CRAWLER_INTERVAL (60u * 2)

/* Maximum number of concurrent crawler instances */
#define MAX_CRAWLERS 5u

/* Seconds since last seen new node before we time a crawler out */
#define CRAWLER_TIMEOUT 20u

/* The maximum time a crawler can run before timing out */
#define CRAWLER_MAX_RUNTIME (60 * 20)

/* Default maximum number of nodes the nodes list can store */
#define DEFAULT_NODES_LIST_SIZE 4096u

/* Max number of nodes to send getnodes requests to per iteration */
#define MAX_GETNODES_REQUESTS 6u

/* Number of random node requests to make for each node we send a request to */
#define NUM_RAND_GETNODE_REQUESTS 8u

typedef struct DHT_Node {
    uint8_t  public_key[TOX_DHT_NODE_PUBLIC_KEY_SIZE];
    char     ip[TOX_DHT_NODE_IP_STRING_SIZE];
    uint16_t port;
    bool     seen;  // true if we have sent a getnodes request to this node
} DHT_Node;

typedef struct Crawler {
    Tox          *tox;
    DHT_Node     **nodes_list;
    uint32_t     num_nodes;
    uint32_t     nodes_list_size;
    uint32_t     send_ptr;    /* index of the oldest node that we haven't sent a getnodes request to */
    time_t       last_seen_new_node;
    time_t       start_time;
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
    { "144.217.86.39",      33445, "7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C" },
    { "tox.abilinski.com",  33445, "10C00EB250C3233E343E2AEBA07115A5C28920E9C8D29492F6D00B29049EDC7E" },
    { "81.169.136.229",     33445, "836D1DA2BE12FE0E669334E437BE3FB02806F1528C2B2782113E0910C7711409" },
    { "172.104.215.182",    33445, "DA2BD927E01CD05EBCC2574EBE5BEBB10FF59AE0B2105A7D1E2B40E49BB20239" },
    { "188.225.9.167",      33445, "1911341A83E02503AB1FD6561BD64AF3A9D6C3F12B5FBB656976B2E678644A67" },
    { "tox.initramfs.io",   33445, "3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25" },
    { "139.162.110.188",    33445, "F76A11284547163889DDC89A7738CF271797BF5E5E220643E97AD3C7E7903D55" },
    { "172.105.109.31",     33445, "D46E97CF995DC1820B92B7D899E152A217D36ABE22730FEA4B6BF1BFC06C617C" },
    { "91.146.66.26",       33445, "B5E7DAC610DBDE55F359C7F8690B294C8E4FCEC4385DE9525DBFA5523EAD9D53" },
    { "5.19.249.240",       33445, "DA98A4C0CD7473A133E115FEA2EBDAEEA2EF4F79FD69325FC070DA4DE4BA3238" },
    { "188.225.9.167",      33445, "1911341A83E02503AB1FD6561BD64AF3A9D6C3F12B5FBB656976B2E678644A67" },
    { "104.225.141.59",     43334, "933BA20B2E258B4C0D475B6DECE90C7E827FE83EFA9655414E7841251B19A72C" },
    { NULL, 0, NULL },
};

/* Attempts to bootstrap to every listed bootstrap node */
static void bootstrap_tox(Crawler *cwl)
{
    for (size_t i = 0; bs_nodes[i].ip != NULL; ++i) {
        char bin_key[TOX_PUBLIC_KEY_SIZE];
        if (hex_string_to_bin(bs_nodes[i].key, strlen(bs_nodes[i].key), bin_key, sizeof(bin_key)) == -1) {
            fprintf(stderr, "failed to parse bootstrap node: %s\n", bs_nodes[i].key);
            continue;
        }

        Tox_Err_Bootstrap err;
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
        if (memcmp(cwl->nodes_list[i]->public_key, public_key, TOX_DHT_NODE_PUBLIC_KEY_SIZE) == 0) {
            return true;
        }
    }

    return false;
}

void cb_getnodes_response(Tox *tox, const uint8_t *public_key, const char *ip, uint32_t ip_length,
    uint16_t port, void *user_data)
{
    Crawler *cwl = (Crawler *)user_data;

    if (cwl == NULL) {
        return;
    }

    if (public_key == NULL || ip == NULL || ip_length == 0) {
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

    DHT_Node *new_node = (DHT_Node *) calloc(1, sizeof(DHT_Node));

    if (new_node == NULL) {
        return;
    }

    memcpy(new_node->public_key, public_key, TOX_DHT_NODE_PUBLIC_KEY_SIZE);
    snprintf(new_node->ip, sizeof(new_node->ip), "%s", ip);
    new_node->port = port;

    cwl->last_seen_new_node = get_time();

    cwl->nodes_list[cwl->num_nodes] = new_node;
    ++cwl->num_nodes;
}

/*
 * Sends getnodes requests to nodes in the nodes list.
 *
 * Returns the number of requests sent.
 */
static uint32_t send_node_requests(Crawler *cwl)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = cwl->send_ptr; count < MAX_GETNODES_REQUESTS && i < cwl->num_nodes; ++i) {
        DHT_Node *node = cwl->nodes_list[i];

        for (int32_t j = 0; j < NUM_RAND_GETNODE_REQUESTS; ++j) {
            const uint32_t r = ((uint32_t) rand()) % cwl->num_nodes;
            const DHT_Node *rand_node = cwl->nodes_list[r];

            tox_dht_send_nodes_request(cwl->tox, node->public_key, node->ip, node->port, rand_node->public_key, NULL);
            tox_dht_send_nodes_request(cwl->tox, rand_node->public_key, rand_node->ip, rand_node->port,
                                       node->public_key, NULL);
        }

        node->seen = true;
        ++count;
    }

    cwl->send_ptr = i == cwl->num_nodes ? 0 : i;

    return count;
}

/*
 * Returns a pointer to a new crawler instance.
 * Returns NULL on failure.
 */
Crawler *crawler_new(void)
{
    Crawler *cwl = (Crawler *) calloc(1, sizeof(Crawler));

    if (cwl == NULL) {
        return cwl;
    }

    DHT_Node **nodes_list = (DHT_Node **) calloc(DEFAULT_NODES_LIST_SIZE, sizeof(DHT_Node *));

    if (nodes_list == NULL) {
        free(cwl);
        return NULL;
    }

    Tox_Err_Options_New options_new_err;
    struct Tox_Options *options = tox_options_new(&options_new_err);

    if (options == NULL) {
        fprintf(stderr, "tox_options_new() failed with error: %d\n", options_new_err);
        free(cwl);
        free(nodes_list);
        return NULL;
    }

    tox_options_default(options);

    Tox_Err_New err;
    Tox *tox = tox_new(options, &err);

    tox_options_free(options);

    if (err != TOX_ERR_NEW_OK || tox == NULL) {
        fprintf(stderr, "tox_new() failed: %d\n", err);
        free(cwl);
        free(nodes_list);
        return NULL;
    }

    cwl->tox = tox;
    cwl->nodes_list = nodes_list;
    cwl->nodes_list_size = DEFAULT_NODES_LIST_SIZE;

    tox_callback_dht_nodes_response(tox, cb_getnodes_response);

    cwl->start_time = get_time();
    cwl->last_seen_new_node = get_time();

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

    const uint32_t temp_len = strlen(log_path) + strlen(TEMP_FILE_EXT) + 1;
    char *log_path_temp = malloc(temp_len);

    if (log_path_temp == NULL) {
        return -2;
    }

    snprintf(log_path_temp, temp_len, "%s%s", log_path, TEMP_FILE_EXT);

    FILE *fp = fopen(log_path_temp, "w");

    if (fp == NULL) {
        free(log_path_temp);
        return -3;
    }

    for (uint32_t i = 0; i < cwl->num_nodes; ++i) {
        fprintf(fp, "%s ", cwl->nodes_list[i]->ip);
    }

    fclose(fp);

    if (rename(log_path_temp, log_path) != 0) {
        free(log_path_temp);
        return -4;
    }

    free(log_path_temp);

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

/*
 * Returns true if all nodes in the crawler's nodes list have been seen.
 */
static bool crawler_all_nodes_seen(const Crawler *cwl)
{
    uint32_t count = 0;

    for (uint32_t i = 0; i < cwl->num_nodes; ++i) {
        const DHT_Node *node = cwl->nodes_list[i];

        if (node->seen) {
            ++count;
        }
    }

    return count == cwl->num_nodes;
}

/* Returns true if the crawler is unable to find new nodes in the DHT or the exit flag has been triggered */
static bool crawler_finished(Crawler *cwl)
{
    LOCK;
    if (FLAG_EXIT
        || (timed_out(cwl->last_seen_new_node, CRAWLER_TIMEOUT) && crawler_all_nodes_seen(cwl))
        || timed_out(cwl->start_time, CRAWLER_MAX_RUNTIME)) {
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
        sleep_thread(tox_iteration_interval(cwl->tox) * 1000);
    }

    // uncomment to show ratio of nodes that are compatible with NGC
    /* const uint16_t num_close = tox_dht_get_num_closelist(cwl->tox); */
    /* const uint16_t num_new = tox_dht_get_num_closelist_announce_capable(cwl->tox); */
    /* const float r = num_new > 0 ? (float)num_new / (float)num_close : 0; */

    char time_format[128];
    get_time_format(time_format, sizeof(time_format));

    /* printf("[%s] Nodes: %llu (Support NGC ratio: %.2f)\n", time_format, (unsigned long long) cwl->num_nodes, r); */
    printf("[%s] Nodes: %llu\n", time_format, (unsigned long long) cwl->num_nodes);

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
    const uint16_t num_active_threads = threads.num_active;
    const time_t last_created_thread = threads.last_created;
    UNLOCK;

    if (num_active_threads >= MAX_CRAWLERS || !timed_out(last_created_thread, NEW_CRAWLER_INTERVAL)) {
        return 0;
    }

    Crawler *cwl = crawler_new();

    if (cwl == NULL) {
        return -1;
    }

    const int ret = init_crawler_thread(cwl);

    if (ret != 0) {
        fprintf(stderr, "init_crawler_thread() failed with error: %d\n", ret);
        return -2;
    }

    LOCK;
    ++threads.num_active;
    threads.last_created = get_time();
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
        const bool flag_exit = FLAG_EXIT;
        UNLOCK;

        if (flag_exit) {
            break;
        }

        const int ret = do_thread_control();

        if (ret < 0) {
            fprintf(stderr, "do_thread_control() failed with error %d\n", ret);
            sleep(5);
        } else {
            sleep_thread(10000);
        }
    }

    /* Wait for threads to exit cleanly */
    while (true) {
        LOCK;
        const uint16_t num_active_threads = threads.num_active;
        UNLOCK;

        if (num_active_threads == 0) {
            break;
        }

        sleep_thread(10000);
    }

    return 0;
}
