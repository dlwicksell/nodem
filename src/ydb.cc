/*
 * Package:    NodeM
 * File:       ydb.cc
 * Summary:    Functions that wrap calls to the SimpleAPI interface
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2018-2020 Fourth Watch Software LC
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License (AGPL)
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#if NODEM_SIMPLE_API == 1

#include "ydb.h"

using std::cerr;
using std::string;

namespace ydb {

/*
 * @function {private} ydb::extended_ref
 * @summary Set new global directory file (in $zgbldir), to support extended global references with the SimpleAPI
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {class} {GtmState*} gtm_state - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - ISV name ($zbgldir)
 * @member {string} value - Value to set
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @param {string} save_result - The buffer containing the original value of the ISV ($zgbldir)
 * @param {bool} change_isv - Whether to change the ISV ($zgbldir) back after API call
 * @returns {ydb_status_t} - Return code; 0 is success, any other number is an error code
 */
static ydb_status_t extended_ref(nodem::GtmBaton* gtm_baton, string save_result, bool& change_isv)
{
    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    ydb::extended_ref enter");
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    value: ", gtm_baton->value.c_str());
    }

    char* var_name = (char*) gtm_baton->name.c_str();
    char* value_name = (char*) gtm_baton->value.c_str();

    if (strstr(var_name, "^[\"") == var_name && strrchr(var_name, ']') != NULL) {
        string save_var_name = var_name;
        string save_value_name = value_name;

        gtm_baton->name = string {"$zgbldir"};

        ydb_status_t get_stat = get(gtm_baton);

        if (get_stat != YDB_OK)
            return get_stat;

        save_result = string {gtm_baton->result};

        char* name;
        char* reference;

        gtm_baton->name = save_var_name;
        var_name = (char*) gtm_baton->name.c_str();

        var_name = strtok_r(var_name, "[", &name);
        reference = strtok_r(NULL, "]", &name);

        string new_var_name(var_name);
        new_var_name += string {name};

        reference = strtok_r(reference, "\"", &name);

        gtm_baton->value = string {reference};
        gtm_baton->name = string {"$zgbldir"};

        ydb_status_t set_stat = set(gtm_baton);

        if (set_stat != YDB_OK)
            return set_stat;

        gtm_baton->name = new_var_name;
        gtm_baton->value = save_value_name;

        change_isv = true;
    } else if (strstr(var_name, "^|\"") == var_name && strrchr(var_name, '|') != NULL && strrchr(var_name, '|') != (var_name + 1)) {
        string save_var_name = var_name;
        string save_value_name = value_name;

        gtm_baton->name = string {"$zgbldir"};

        ydb_status_t get_stat = get(gtm_baton);

        if (get_stat != YDB_OK)
            return get_stat;

        save_result = string {gtm_baton->result};

        char* name;
        char* reference;

        gtm_baton->name = save_var_name;
        var_name = (char*) gtm_baton->name.c_str();

        var_name = strtok_r(var_name, "|", &name);
        reference = strtok_r(NULL, "|", &name);

        string new_var_name(var_name);
        new_var_name += string {name};

        reference = strtok_r(reference, "\"", &name);

        gtm_baton->value = string {reference};
        gtm_baton->name = string {"$zgbldir"};

        ydb_status_t set_stat = set(gtm_baton);

        if (set_stat != YDB_OK)
            return set_stat;

        gtm_baton->name = new_var_name;
        gtm_baton->value = save_value_name;

        change_isv = true;
    } else if (change_isv) {
        gtm_baton->name = string {"$zgbldir"};
        gtm_baton->value = save_result;

        ydb_status_t set_stat = set(gtm_baton);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    ydb::extended_ref exit");
        nodem::debug_log(">>>    save_result: ", save_result);
        nodem::debug_log(">>>    change_isv: ", std::boolalpha, change_isv);
    }

    return YDB_OK;
} // @end ydb::extended_ref

// ***Begin Public APIs***

/*
 * @function ydb::data
 * @summary Check if global or local node has data and/or children or not
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t data(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::data enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    unsigned int  temp_value = 0;
    unsigned int* ret_value = &temp_value;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t stat_buf = ydb_data_s(&glvn, subs_size, subs_array, ret_value);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    if (int len = snprintf(gtm_baton->result, sizeof(int), "%u", *ret_value) < 0) {
        char error[BUFSIZ];
        cerr << strerror_r(errno, error, BUFSIZ);

        stat_buf = len;
    }

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::data exit");

    return stat_buf;
} // @end ydb::data function

/*
 * @function ydb::get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t get(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::get enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    char get_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &get_data;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t stat_buf = ydb_get_s(&glvn, subs_size, subs_array, &value);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    strncpy(gtm_baton->result, value.buf_addr, value.len_used);
    gtm_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::get exit");

    return stat_buf;
} // @end ydb::get function

/*
 * @function ydb::set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {string} value - Value to set
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t set(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::set enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    value: ", gtm_baton->value);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    char* value = (char*) gtm_baton->value.c_str();

    ydb_buffer_t data_node;
    data_node.len_alloc = data_node.len_used = strlen(value);
    data_node.buf_addr = value;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t stat_buf = ydb_set_s(&glvn, subs_size, subs_array, &data_node);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::set exit");

    return stat_buf;
} // @end ydb::set

/*
 * @function ydb::kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {int32_t} node_only (-1|<0>|1) - Whether to kill only the node, or also kill child subscripts; 0 is children, 1 node-only
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t kill(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::kill enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    node_only: ", gtm_baton->node_only);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    ydb_status_t stat_buf;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    if (gtm_baton->name == "") {
        ydb_buffer_t subs_array[1] = {8, 8, (char*) "v4wDebug"};

        uv_mutex_lock(&nodem::mutex_g);

        stat_buf = ydb_delete_excl_s(1, subs_array);
    } else {
        char* var_name = (char*) gtm_baton->name.c_str();

        ydb_buffer_t glvn;
        glvn.len_alloc = glvn.len_used = strlen(var_name);
        glvn.buf_addr = var_name;

        ydb_buffer_t subs_array[YDB_MAX_SUBS];
        unsigned int subs_size = gtm_baton->subs_array.size();

        for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
        }

        int delete_type = gtm_baton->node_only == 1 ? YDB_DEL_NODE : YDB_DEL_TREE;

        uv_mutex_lock(&nodem::mutex_g);

        stat_buf = ydb_delete_s(&glvn, subs_size, subs_array, delete_type);
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::kill exit");

    return stat_buf;
} // @end ydb::kill function

/*
 * @function ydb::order
 * @summary Return the next global or local node at the same level
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t order(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::order enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    char order_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &order_data;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    if (strncmp(glvn.buf_addr, "^", 1) != 0 && subs_size > 0) {
        unsigned int  temp_value = 0;
        unsigned int* ret_value = &temp_value;

        uv_mutex_lock(&nodem::mutex_g);

        ydb_status_t stat_buf = ydb_data_s(&glvn, 0, NULL, ret_value);

        if (gtm_baton->gtm_state->debug > nodem::LOW)
            nodem::debug_log(">>   stat_buf: ", stat_buf);

        uv_mutex_unlock(&nodem::mutex_g);

        if (stat_buf == YDB_OK && *ret_value == 0) {
            gtm_baton->result[0] = '\0';

            if (gtm_baton->gtm_state->debug > nodem::LOW)
                nodem::debug_log(">>   ydb::order exit");

            return stat_buf;
        }
    }

    ydb_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    stat_buf = ydb_subscript_next_s(&glvn, subs_size, subs_array, &value);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    while (strncmp(value.buf_addr, "v4w", 3) == 0 && subs_size == 0) {
        glvn.len_alloc = glvn.len_used = strlen(value.buf_addr);
        glvn.buf_addr = value.buf_addr;

        uv_mutex_lock(&nodem::mutex_g);

        stat_buf = ydb_subscript_next_s(&glvn, subs_size, subs_array, &value);

        if (gtm_baton->gtm_state->debug > nodem::LOW)
            nodem::debug_log(">>   stat_buf: ", stat_buf);

        if (stat_buf != YDB_OK)
            ydb_zstatus(gtm_baton->error, ERR_LEN);

        uv_mutex_unlock(&nodem::mutex_g);

        if (value.len_used == 0)
            break;
    }

    strncpy(gtm_baton->result, value.buf_addr, value.len_used);
    gtm_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::order exit");

    return stat_buf;
} // @end ydb::order function

/*
 * @function ydb::previous
 * @summary Return the previous global or local node at the same level
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t previous(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::previous enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    char previous_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &previous_data;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    if (strncmp(glvn.buf_addr, "^", 1) != 0 && subs_size > 0) {
        unsigned int  temp_value = 0;
        unsigned int* ret_value = &temp_value;

        uv_mutex_lock(&nodem::mutex_g);

        ydb_status_t stat_buf = ydb_data_s(&glvn, 0, NULL, ret_value);

        if (gtm_baton->gtm_state->debug > nodem::LOW)
            nodem::debug_log(">>   stat_buf: ", stat_buf);

        uv_mutex_unlock(&nodem::mutex_g);

        if (stat_buf == YDB_OK && *ret_value == 0) {
            gtm_baton->result[0] = '\0';

            if (gtm_baton->gtm_state->debug > nodem::LOW)
                nodem::debug_log(">>   ydb::previous exit");

            return stat_buf;
        }
    }

    ydb_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    stat_buf = ydb_subscript_previous_s(&glvn, subs_size, subs_array, &value);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    while (strncmp(value.buf_addr, "v4w", 3) == 0 && subs_size == 0) {
        glvn.len_alloc = glvn.len_used = strlen(value.buf_addr);
        glvn.buf_addr = value.buf_addr;

        uv_mutex_lock(&nodem::mutex_g);

        stat_buf = ydb_subscript_previous_s(&glvn, subs_size, subs_array, &value);

        if (gtm_baton->gtm_state->debug > nodem::LOW)
            nodem::debug_log(">>   stat_buf: ", stat_buf);

        if (stat_buf != YDB_OK)
            ydb_zstatus(gtm_baton->error, ERR_LEN);

        uv_mutex_unlock(&nodem::mutex_g);

        if (value.len_used == 0)
            break;
    }

    strncpy(gtm_baton->result, value.buf_addr, value.len_used);
    gtm_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::previous exit");

    return stat_buf;
} // @end ydb::previous function

/*
 * @function ydb::next_node
 * @summary Return the next global or local node, depth first
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t next_node(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::next_node enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    int  subs_test = YDB_MAX_SUBS;
    int* subs_used = &subs_test;

    static char next_node_data[YDB_MAX_SUBS][YDB_MAX_STR];
    static ydb_buffer_t ret_array[YDB_MAX_SUBS];

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    uv_mutex_lock(&nodem::mutex_g);

    for (int i = 0; i < YDB_MAX_SUBS; i++) {
        ret_array[i].len_alloc = YDB_MAX_STR;
        ret_array[i].len_used = 0;
        ret_array[i].buf_addr = (char*) &next_node_data[i][0];
    }

    ydb_status_t stat_buf = ydb_node_next_s(&glvn, subs_size, subs_array, subs_used, ret_array);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    gtm_baton->subs_array.clear();

    if (stat_buf != YDB_OK) {
        ydb_zstatus(gtm_baton->error, ERR_LEN);

        uv_mutex_unlock(&nodem::mutex_g);

        if (gtm_baton->gtm_state->debug > nodem::LOW)
            nodem::debug_log(">>   ydb::next_node exit");

        return stat_buf;
    }

    if (*subs_used != YDB_NODE_END) {
        for (int i = 0; i < *subs_used; i++) {
            ret_array[i].buf_addr[ret_array[i].len_used] = '\0';
            gtm_baton->subs_array.push_back(ret_array[i].buf_addr);
        }
    } else {
        uv_mutex_unlock(&nodem::mutex_g);

        gtm_baton->result[0] = '\0';
        return YDB_NODE_END;
    }

    char ret_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &ret_data;

    stat_buf = ydb_get_s(&glvn, *subs_used, ret_array, &value);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    strncpy(gtm_baton->result, value.buf_addr, value.len_used);
    gtm_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::next_node exit");

    return stat_buf;
} // @end ydb::next_node function

/*
 * @function ydb::previous_node
 * @summary Return the previous global or local node, depth first
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t previous_node(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::previous_node enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    int  subs_test = YDB_MAX_SUBS;
    int* subs_used = &subs_test;

    static char previous_node_data[YDB_MAX_SUBS][YDB_MAX_STR];
    static ydb_buffer_t ret_array[YDB_MAX_SUBS];

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    uv_mutex_lock(&nodem::mutex_g);

    for (int i = 0; i < YDB_MAX_SUBS; i++) {
        ret_array[i].len_alloc = YDB_MAX_STR;
        ret_array[i].len_used = 0;
        ret_array[i].buf_addr = (char*) &previous_node_data[i][0];
    }

    ydb_status_t stat_buf = ydb_node_previous_s(&glvn, subs_size, subs_array, subs_used, ret_array);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    gtm_baton->subs_array.clear();

    if (stat_buf != YDB_OK) {
        ydb_zstatus(gtm_baton->error, ERR_LEN);

        uv_mutex_unlock(&nodem::mutex_g);

        if (gtm_baton->gtm_state->debug > nodem::LOW)
            nodem::debug_log(">>   ydb::previous_node exit");

        return stat_buf;
    }

    if (*subs_used != YDB_NODE_END) {
        for (int i = 0; i < *subs_used; i++) {
            ret_array[i].buf_addr[ret_array[i].len_used] = '\0';
            gtm_baton->subs_array.push_back(ret_array[i].buf_addr);
        }
    } else {
        *subs_used = 0;
    }

    char ret_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &ret_data;

    stat_buf = ydb_get_s(&glvn, *subs_used, ret_array, &value);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    if (subs_size == 0 || stat_buf == YDB_ERR_GVUNDEF || stat_buf == YDB_ERR_LVUNDEF) {
        gtm_baton->result[0] = '\0';
        return YDB_NODE_END;
    } else {
        strncpy(gtm_baton->result, value.buf_addr, value.len_used);
        gtm_baton->result[value.len_used] = '\0';
    }

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::previous_node exit");

    return stat_buf;
} // @end ydb::previous_node function

/*
 * @function ydb::increment
 * @summary Increment or decrement the number in a global or local node
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_double_t} option - Number to increment or decrement by
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t increment(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::increment enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    increment: ", gtm_baton->option);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    char incr_val[YDB_MAX_STR];

    if (snprintf(incr_val, YDB_MAX_STR, "%.16g", gtm_baton->option) < 0) {
        char error[BUFSIZ];
        cerr << strerror_r(errno, error, BUFSIZ);
    }

    ydb_buffer_t incr;
    incr.len_alloc = incr.len_used = strlen(incr_val);
    incr.buf_addr = (char*) &incr_val;

    char increment_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &increment_data;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t stat_buf = ydb_incr_s(&glvn, subs_size, subs_array, &incr, &value);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    strncpy(gtm_baton->result, value.buf_addr, value.len_used);
    gtm_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::increment exit");

    return stat_buf;
} // @end ydb::increment function

/*
 * @function ydb::lock
 * @summary Lock a global or local node, incrementally
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_double_t} option - The time to wait for the lock, or -1 to wait forever
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t lock(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::lock enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    timeout: ", gtm_baton->option);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    char* var_name = (char*) gtm_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = gtm_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
    }

    unsigned long long timeout;

    if (gtm_baton->option == -1) {
        timeout = YDB_MAX_TIME_NSEC;
    } else {
        timeout = gtm_baton->option * 1000000000;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t stat_buf = ydb_lock_incr_s(timeout, &glvn, subs_size, subs_array);

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf == YDB_OK) {
        strncpy(gtm_baton->result, "1\0", 2);
    } else if (stat_buf == YDB_LOCK_TIMEOUT) {
        strncpy(gtm_baton->result, "0\0", 2);

        stat_buf = YDB_OK;
    } else {
        ydb_zstatus(gtm_baton->error, ERR_LEN);
    }

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::lock exit");

    return stat_buf;
} // @end ydb::lock function

/*
 * @function ydb::unlock
 * @summary Lock a global or local node, incrementally
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t unlock(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::unlock enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);

        if (gtm_baton->subs_array.size()) {
            for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
                nodem::debug_log(">>   subscripts[", i, "]: ", gtm_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (gtm_baton->name.compare(0, 2, "^[") == 0 || gtm_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    ydb_status_t stat_buf;

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using SimpleAPI");

    if (gtm_baton->name == "") {
        uv_mutex_lock(&nodem::mutex_g);

        stat_buf = ydb_lock_s(0, 0);
    } else {
        char* var_name = (char*) gtm_baton->name.c_str();

        ydb_buffer_t glvn;
        glvn.len_alloc = glvn.len_used = strlen(var_name);
        glvn.buf_addr = var_name;

        ydb_buffer_t subs_array[YDB_MAX_SUBS];
        unsigned int subs_size = gtm_baton->subs_array.size();

        for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = gtm_baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) gtm_baton->subs_array[i].c_str();
        }

        uv_mutex_lock(&nodem::mutex_g);

        stat_buf = ydb_lock_decr_s(&glvn, subs_size, subs_array);
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != YDB_OK)
        ydb_zstatus(gtm_baton->error, ERR_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(gtm_baton, save_result, change_isv);

        if (set_stat != YDB_OK)
            return set_stat;
    }

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   ydb::unlock exit");

    return stat_buf;
} // @end ydb::unlock function

// ***End Public APIs***

} // @end ydb namespace

#endif // @end NODEM_SIMPLE_API
