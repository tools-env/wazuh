/**
 * @file fim_sync.c
 * @author Vikman Fernandez-Castro (victor@wazuh.com)
 * @brief Definition of FIM data synchronization library
 * @version 0.1
 * @date 2019-08-28
 *
 * @copyright Copyright (c) 2019 Wazuh, Inc.
 */

/*
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include <shared.h>
#include <openssl/evp.h>
#include "syscheck.h"
#include "integrity_op.h"

static long fim_sync_cur_id;
static long fim_sync_last_msg_time;
static w_queue_t * fim_sync_queue;

// Starting data synchronization thread
void * fim_run_integrity(void * args) {
    fim_sync_queue = queue_init(syscheck.sync_queue_size);

    while (1) {
        mdebug2("Performing synchronization check.");
        fim_sync_checksum();

        // Wait for sync_response_timeout seconds since the last message received, or sync_interval

        struct timespec timeout = { .tv_sec = time(NULL) + syscheck.sync_interval };
        long margin = fim_sync_last_msg_time + syscheck.sync_response_timeout;

        timeout.tv_sec = timeout.tv_sec > margin ? timeout.tv_sec : margin;

        // Get messages until timeout
        char * msg;

        while ((msg = queue_pop_ex_timedwait(fim_sync_queue, &timeout))) {
            fim_sync_dispatch(msg);
            free(msg);
        }
    }

    return args;
}

void fim_sync_checksum() {
    char ** keys;
    int i;
    EVP_MD_CTX * ctx = EVP_MD_CTX_create();
    EVP_DigestInit(ctx, EVP_sha1());

    w_mutex_lock(&syscheck.fim_entry_mutex);

    {
        keys = rbtree_keys(syscheck.fim_entry);

        for (i = 0; keys[i]; i++) {
            fim_entry_data * data = rbtree_get(syscheck.fim_entry, keys[i]);
            assert(data);
            EVP_DigestUpdate(ctx, data->checksum, strlen(data->checksum));
        }
    }

    w_mutex_unlock(&syscheck.fim_entry_mutex);
    fim_sync_cur_id = time(NULL);

    if (i > 0) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int digest_size;
        os_sha1 hexdigest;

        EVP_DigestFinal_ex(ctx, digest, &digest_size);
        OS_SHA1_Hexdigest(digest, hexdigest);

        char * plain = dbsync_check_msg("syscheck", INTEGRITY_CHECK_GLOBAL, fim_sync_cur_id, keys[0], keys[i - 1], NULL, hexdigest);
        fim_send_sync_msg(plain);
        free(plain);
    } else {
        char * plain = dbsync_check_msg("syscheck", INTEGRITY_CLEAR, 0, NULL, NULL, NULL, NULL);
        fim_send_sync_msg(plain);
        free(plain);
    }

    EVP_MD_CTX_destroy(ctx);
    free_strarray(keys);
}

void fim_sync_checksum_split(const char * start, const char * top, long id) {
    cJSON * entry_data = NULL;
    char ** keys;
    int n;
    int m = 0;
    EVP_MD_CTX * ctx_left = EVP_MD_CTX_create();
    EVP_MD_CTX * ctx_right = EVP_MD_CTX_create();

    EVP_DigestInit(ctx_left, EVP_sha1());
    EVP_DigestInit(ctx_right, EVP_sha1());

    w_mutex_lock(&syscheck.fim_entry_mutex);

    {
        keys = rbtree_range(syscheck.fim_entry, start, top);
        for (n = 0; keys[n]; n++);

        switch (n) {
        case 0:
            break;

        case 1:
            // Unary list: send the file state
            entry_data = fim_entry_json(keys[0], (fim_entry_data *) rbtree_get(syscheck.fim_entry, keys[0]));
            break;

        default:
            // Other case: split the list
            m = n / 2;

            for (int i = 0; i < m; i++) {
                fim_entry_data * data = rbtree_get(syscheck.fim_entry, keys[i]);
                assert(data);
                EVP_DigestUpdate(ctx_left, data->checksum, strlen(data->checksum));
            }

            for (int i = m; i < n; i++) {
                fim_entry_data * data = rbtree_get(syscheck.fim_entry, keys[i]);
                assert(data);
                EVP_DigestUpdate(ctx_right, data->checksum, strlen(data->checksum));
            }
        }
    }

    w_mutex_unlock(&syscheck.fim_entry_mutex);

    if (n > 0) {
        if (entry_data == NULL) {
            unsigned char digest[EVP_MAX_MD_SIZE];
            unsigned int digest_size;
            os_sha1 hexdigest;

            EVP_DigestFinal_ex(ctx_left, digest, &digest_size);
            OS_SHA1_Hexdigest(digest, hexdigest);
            char * plain = dbsync_check_msg("syscheck", INTEGRITY_CHECK_LEFT, id, keys[0], keys[m - 1], keys[m], hexdigest);
            fim_send_sync_msg(plain);
            free(plain);

            EVP_DigestFinal_ex(ctx_right, digest, &digest_size);
            OS_SHA1_Hexdigest(digest, hexdigest);
            plain = dbsync_check_msg("syscheck", INTEGRITY_CHECK_RIGHT, id, keys[m], keys[n - 1], "", hexdigest);
            fim_send_sync_msg(plain);
            free(plain);
        } else {
            char * plain = dbsync_state_msg("syscheck", entry_data);
            fim_send_sync_msg(plain);
            free(plain);
        }
    }

    free_strarray(keys);
    EVP_MD_CTX_destroy(ctx_left);
    EVP_MD_CTX_destroy(ctx_right);
}

void fim_sync_send_list(const char * start, const char * top) {
    w_mutex_lock(&syscheck.fim_entry_mutex);
    char ** keys = rbtree_range(syscheck.fim_entry, start, top);
    w_mutex_unlock(&syscheck.fim_entry_mutex);

    for (int i = 0; keys[i]; i++) {
        w_mutex_lock(&syscheck.fim_entry_mutex);
        fim_entry_data * data = rbtree_get(syscheck.fim_entry, keys[i]);

        if (data == NULL) {
            w_mutex_unlock(&syscheck.fim_entry_mutex);
            continue;
        }

        cJSON * entry_data = fim_entry_json(keys[i], data);
        w_mutex_unlock(&syscheck.fim_entry_mutex);

        char * plain = dbsync_state_msg("syscheck", entry_data);
        fim_send_sync_msg(plain);
        free(plain);
    }

    free_strarray(keys);
}

void fim_sync_dispatch(char * payload) {
    assert(payload != NULL);

    char * command = payload;
    char * json_arg = strchr(payload, ' ');

    if (json_arg == NULL) {
        mdebug1(FIM_DBSYNC_NO_ARGUMENT, payload);
        return;
    }

    *json_arg++ = '\0';
    cJSON * root = cJSON_Parse(json_arg);

    if (root == NULL) {
        mdebug1(FIM_DBSYNC_INVALID_ARGUMENT, json_arg);
        return;
    }

    cJSON * id = cJSON_GetObjectItem(root, "id");

    if (!cJSON_IsNumber(id)) {
        mdebug1(FIM_DBSYNC_INVALID_ARGUMENT, json_arg);
        goto end;
    }

    fim_sync_last_msg_time = time(NULL);

    // Discard command if (data.id > global_id)
    // Decrease global ID if (data.id < global_id)

    if (id->valuedouble < fim_sync_cur_id) {
        fim_sync_cur_id = id->valuedouble;
        mdebug1(FIM_DBSYNC_DEC_ID, fim_sync_cur_id);
    } else if (id->valuedouble < fim_sync_cur_id) {
        mdebug1(FIM_DBSYNC_DROP_MESSAGE, (long)id->valuedouble, fim_sync_cur_id);
        return;
    }

    char * begin = cJSON_GetStringValue(cJSON_GetObjectItem(root, "begin"));
    char * end = cJSON_GetStringValue(cJSON_GetObjectItem(root, "end"));

    if (begin == NULL || end == NULL) {
        mdebug1(FIM_DBSYNC_INVALID_ARGUMENT, json_arg);
        goto end;
    }

    if (strcmp(command, "checksum_fail") == 0) {
        fim_sync_checksum_split(begin, end, id->valuedouble);
    } else if (strcmp(command, "no_data") == 0) {
        fim_sync_send_list(begin, end);
    } else {
        mdebug1(FIM_DBSYNC_UNKNOWN_CMD, command);
    }

end:
    cJSON_Delete(root);
}

void fim_sync_push_msg(const char * msg) {

    if (fim_sync_queue == NULL) {
        mwarn("A data synchronization response was received before sending the first message.");
        return;
    }

    char * copy;
    os_strdup(msg, copy);

    if (queue_push_ex_block(fim_sync_queue, copy) == -1) {
        merror("Cannot push a data synchronization message.");
        free(copy);
    }
}