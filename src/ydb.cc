/*
 * Package:    NodeM
 * File:       ydb.cc
 * Summary:    Functions that wrap calls to the SimpleAPI interface
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2018-2019 Fourth Watch Software LC
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

#if YDB_SIMPLE_API == 1

#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "ydb.h"

namespace ydb {

using namespace v8;
using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;

/*
 * @function {public} data
 * @summary Check if global or local node has data and/or children or not
 * @param {Baton*} baton - struct containing the following members
 * @member {ydb_char_t} ret_buf - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t data(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::data enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    unsigned int  temp_value = 0;
    unsigned int* ret_value = &temp_value;

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    uv_mutex_lock(&nodem::mutex_g);
    ydb_status_t stat_buf = ydb_data_s(&glvn, subs_size, subs_array, ret_value);
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (int len = snprintf(baton->ret_buf, sizeof(int), "%u", *ret_value) < 0) {
        char error[BUFSIZ];
        cerr << strerror_r(errno, error, BUFSIZ) << endl;

        stat_buf = len;
    }

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::data exit" << "\n";

    return stat_buf;
} // @end data function

/*
 * @function {public} get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {ydb_char_t} ret_buf - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {vector<string>} subs_array - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t get(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::get enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    char get_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &get_data;

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    uv_mutex_lock(&nodem::mutex_g);
    ydb_status_t stat_buf = ydb_get_s(&glvn, subs_size, subs_array, &value);
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    strncpy(baton->ret_buf, value.buf_addr, value.len_used);
    baton->ret_buf[value.len_used] = '\0';

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::get exit" << "\n";

    return stat_buf;
} // @end get function

/*
 * @function {public} increment
 * @summary Increment or decrement the number in a global or local node
 * @param {Baton*} baton - struct containing the following members
 * @member {ydb_char_t} ret_buf - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_double_t} increment - Number to increment or decrement by
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t increment(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::increment enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> increment: " << baton->incr << "\n";

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    char incr_val[YDB_MAX_STR];
    snprintf(incr_val, YDB_MAX_STR, "%.16g", baton->incr);

    ydb_buffer_t incr;
    incr.len_alloc = incr.len_used = strlen(incr_val);
    incr.buf_addr = (char*) &incr_val;

    char increment_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &increment_data;

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    uv_mutex_lock(&nodem::mutex_g);
    ydb_status_t stat_buf = ydb_incr_s(&glvn, subs_size, subs_array, &incr, &value);
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    strncpy(baton->ret_buf, value.buf_addr, value.len_used);
    baton->ret_buf[value.len_used] = '\0';

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::increment exit" << "\n";

    return stat_buf;
} // @end increment function

/*
 * @function {public} kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {Baton*} baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {int32_t} node_only (-1|<0>|1) - Whether to kill only the node, or also kill child subscripts; 0 is children, 1 node-only
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t kill(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::kill enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";
        cout << "DEBUG>>> node_only: " << baton->node_only << "\n";
    }

    ydb_status_t stat_buf;

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    if (baton->name == "") {
        ydb_buffer_t subs_array[1] = {8, 8, (char*) "v4wDebug"};

        uv_mutex_lock(&nodem::mutex_g);
        stat_buf = ydb_delete_excl_s(1, subs_array);
        if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
        uv_mutex_unlock(&nodem::mutex_g);
    } else {
        char* var_name = (char*) baton->name.c_str();

        ydb_buffer_t glvn;
        glvn.len_alloc = glvn.len_used = strlen(var_name);
        glvn.buf_addr = var_name;

        ydb_buffer_t subs_array[YDB_MAX_SUBS];
        unsigned int subs_size = baton->subs_array.size();

        for (unsigned int i = 0; i < subs_size; i++) {
                subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
                subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
        }

        int delete_type = baton->node_only == 1 ? YDB_DEL_NODE : YDB_DEL_TREE;

        uv_mutex_lock(&nodem::mutex_g);
        stat_buf = ydb_delete_s(&glvn, subs_size, subs_array, delete_type);
        if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
        uv_mutex_unlock(&nodem::mutex_g);
    }

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::kill exit" << "\n";

    return stat_buf;
} // @end kill function

/*
 * @function {public} next_node
 * @summary Return the next global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {ydb_char_t} ret_buf - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t next_node(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::next_node enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    int  subs_test = YDB_MAX_SUBS;
    int* subs_used = &subs_test;

    static char next_node_data[YDB_MAX_SUBS][YDB_MAX_STR];
    static ydb_buffer_t ret_array[YDB_MAX_SUBS];

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    uv_mutex_lock(&nodem::mutex_g);

    for (int i = 0; i < YDB_MAX_SUBS; i++) {
        ret_array[i].len_alloc = YDB_MAX_STR;
        ret_array[i].len_used = 0;
        ret_array[i].buf_addr = (char*) &next_node_data[i][0];
    }

    ydb_status_t stat_buf = ydb_node_next_s(&glvn, subs_size, subs_array, subs_used, ret_array);

    baton->subs_array.clear();

    if (stat_buf != YDB_OK) {
        ydb_zstatus(baton->msg_buf, MSG_LEN);
        uv_mutex_unlock(&nodem::mutex_g);

        if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::next_node exit" << "\n";

        return stat_buf;
    }

    if (*subs_used != YDB_NODE_END) {
        for (int i = 0; i < *subs_used; i++) {
            ret_array[i].buf_addr[ret_array[i].len_used] = '\0';
            baton->subs_array.push_back(ret_array[i].buf_addr);
        }
    } else {
        uv_mutex_unlock(&nodem::mutex_g);

        baton->ret_buf[0] = '\0';
        return YDB_NODE_END;
    }

    char ret_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &ret_data;

    stat_buf = ydb_get_s(&glvn, *subs_used, ret_array, &value);
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    strncpy(baton->ret_buf, value.buf_addr, value.len_used);
    baton->ret_buf[value.len_used] = '\0';

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::next_node exit" << "\n";

    return stat_buf;
} // @end next_node function

/*
 * @function {public} order
 * @summary Return the next global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {ydb_char_t} ret_buf - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t order(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::order enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    char order_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &order_data;

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    if (strncmp(glvn.buf_addr, "^", 1) != 0 && subs_size > 0) {
        unsigned int  temp_value = 0;
        unsigned int* ret_value = &temp_value;

        uv_mutex_lock(&nodem::mutex_g);
        ydb_status_t stat_buf = ydb_data_s(&glvn, 0, NULL, ret_value);
        uv_mutex_unlock(&nodem::mutex_g);

        if (stat_buf == YDB_OK && *ret_value == 0) {
            baton->ret_buf[0] = '\0';

            if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::order exit" << "\n";

            return stat_buf;
        }
    }

    ydb_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = ydb_subscript_next_s(&glvn, subs_size, subs_array, &value);
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    while (strncmp(value.buf_addr, "v4w", 3) == 0 && subs_size == 0) {
        glvn.len_alloc = glvn.len_used = strlen(value.buf_addr);
        glvn.buf_addr = value.buf_addr;

        uv_mutex_lock(&nodem::mutex_g);
        stat_buf = ydb_subscript_next_s(&glvn, subs_size, subs_array, &value);
        if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
        uv_mutex_unlock(&nodem::mutex_g);

        if (value.len_used == 0) break;
    }

    strncpy(baton->ret_buf, value.buf_addr, value.len_used);
    baton->ret_buf[value.len_used] = '\0';

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::order exit" << "\n";

    return stat_buf;
} // @end order function

/*
 * @function {public} previous
 * @summary Return the previous global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {ydb_char_t} ret_buf - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t previous(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::previous enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    char previous_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &previous_data;

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    if (strncmp(glvn.buf_addr, "^", 1) != 0 && subs_size > 0) {
        unsigned int  temp_value = 0;
        unsigned int* ret_value = &temp_value;

        uv_mutex_lock(&nodem::mutex_g);
        ydb_status_t stat_buf = ydb_data_s(&glvn, 0, NULL, ret_value);
        uv_mutex_unlock(&nodem::mutex_g);

        if (stat_buf == YDB_OK && *ret_value == 0) {
            baton->ret_buf[0] = '\0';

            if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::previous exit" << "\n";

            return stat_buf;
        }
    }

    ydb_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = ydb_subscript_previous_s(&glvn, subs_size, subs_array, &value);
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    while (strncmp(value.buf_addr, "v4w", 3) == 0 && subs_size == 0) {
        glvn.len_alloc = glvn.len_used = strlen(value.buf_addr);
        glvn.buf_addr = value.buf_addr;

        uv_mutex_lock(&nodem::mutex_g);
        stat_buf = ydb_subscript_previous_s(&glvn, subs_size, subs_array, &value);
        if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
        uv_mutex_unlock(&nodem::mutex_g);

        if (value.len_used == 0) break;
    }

    strncpy(baton->ret_buf, value.buf_addr, value.len_used);
    baton->ret_buf[value.len_used] = '\0';

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::previous exit" << "\n";

    return stat_buf;
} // @end previous function

/*
 * @function {public} previous_node
 * @summary Return the previous global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {ydb_char_t} ret_buf - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t previous_node(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::previous_node enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    int  subs_test = YDB_MAX_SUBS;
    int* subs_used = &subs_test;

    static char previous_node_data[YDB_MAX_SUBS][YDB_MAX_STR];
    static ydb_buffer_t ret_array[YDB_MAX_SUBS];

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    uv_mutex_lock(&nodem::mutex_g);

    for (int i = 0; i < YDB_MAX_SUBS; i++) {
        ret_array[i].len_alloc = YDB_MAX_STR;
        ret_array[i].len_used = 0;
        ret_array[i].buf_addr = (char*) &previous_node_data[i][0];
    }

    ydb_status_t stat_buf = ydb_node_previous_s(&glvn, subs_size, subs_array, subs_used, ret_array);

    baton->subs_array.clear();

    if (stat_buf != YDB_OK) {
        ydb_zstatus(baton->msg_buf, MSG_LEN);
        uv_mutex_unlock(&nodem::mutex_g);

        if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::previous_node exit" << "\n";

        return stat_buf;
    }

    if (*subs_used != YDB_NODE_END) {
        for (int i = 0; i < *subs_used; i++) {
            ret_array[i].buf_addr[ret_array[i].len_used] = '\0';
            baton->subs_array.push_back(ret_array[i].buf_addr);
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
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);

    uv_mutex_unlock(&nodem::mutex_g);

    if (subs_size == 0 || stat_buf == YDB_ERR_GVUNDEF || stat_buf == YDB_ERR_LVUNDEF) {
        baton->ret_buf[0] = '\0';
        return YDB_NODE_END;
    } else {
        strncpy(baton->ret_buf, value.buf_addr, value.len_used);
        baton->ret_buf[value.len_used] = '\0';
    }

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::previous_node exit" << "\n";

    return stat_buf;
} // @end previous_node function

/*
 * @function {public} set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {string} value - Value to set
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t set(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::set enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name.c_str() << "\n";
        cout << "DEBUG>>> value: " << baton->value.c_str() << "\n";
    }

    char* var_name = (char*) baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) baton->subs_array[i].c_str();
    }

    char* value = (char*) baton->value.c_str();

    ydb_buffer_t data_node;
    data_node.len_alloc = data_node.len_used = strlen(value);
    data_node.buf_addr = value;

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using simpleAPI" << "\n";

    uv_mutex_lock(&nodem::mutex_g);
    ydb_status_t stat_buf = ydb_set_s(&glvn, subs_size, subs_array, &data_node);
    if (stat_buf != YDB_OK) ydb_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::set exit" << "\n";

    return stat_buf;
} // @end set

} // @end ydb namespace

#endif // @end YDB_SIMPLE_API
