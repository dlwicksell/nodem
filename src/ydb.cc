/*
 * Package:    NodeM
 * File:       ydb.cc
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2018 Fourth Watch Software LC
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

#include "ydb.h"
#include "mumps.h"

#include <node.h>
#include <uv.h>

#include <cstring>

#include <iostream>

namespace ydb {

using namespace v8;
using std::cout;
using std::string;
using std::stringstream;
using std::vector;

/*
 * @function {public} get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {ydb_char_t} buffer - Data returned from YottaDB/GT.M database, via SimpleAPI interface
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t get(ydb_char_t buffer[], string name, vector<string> subs, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::get enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> buffer: " << buffer << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    int ret_code = 0;
    char *varname = (char *) name.c_str();
    unsigned int var_size = strlen(varname);

    ydb_buffer_t var_name;
    var_name.len_alloc = var_size;
    var_name.len_used = var_size;
    var_name.buf_addr = varname;

    uv_mutex_lock(&nodem::mutex);
    ydb_buffer_t *subs_array = NULL;

    if (subs.size() > 0) {
        subs_array = (ydb_buffer_t *) ydb_malloc(subs.size() * sizeof(ydb_buffer_t));

        if (subs_array == NULL) return -1;

        for (unsigned int i = 0; i < subs.size(); i++) {
            unsigned int subs_size = subs[i].length();

            subs_array[i].len_alloc = subs_size;
            subs_array[i].len_used = subs_size;
            subs_array[i].buf_addr = (gtm_char_t*) ydb_malloc(subs_size);

            if (subs_array[i].buf_addr == NULL) return -1;

            strncpy(subs_array[i].buf_addr, subs[i].c_str(), subs_size);
        }
    }

    ydb_buffer_t ret_value;
    ret_value.len_alloc = YDB_MAX_STR;
    ret_value.len_used = 0;
    ret_value.buf_addr = (char *) ydb_malloc(YDB_MAX_STR);

    if (ret_value.buf_addr == NULL) return -1;

    ret_code = ydb_get_s(&var_name, subs.size(), subs_array, &ret_value);

    strncpy(buffer, ret_value.buf_addr, ret_value.len_used);
    buffer[ret_value.len_used] = '\0';

    for (unsigned int i = 0; i < subs.size(); i++) {
        ydb_free(subs_array[i].buf_addr);
    }

    if (subs.size() > 0) ydb_free(subs_array);

    ydb_free(ret_value.buf_addr);
    uv_mutex_unlock(&nodem::mutex);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::get exit" << "\n";

    return ret_code;
} // @end get function

/*
 * @function {public} kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t kill(string name, vector<string> subs, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::kill enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    int ret_code = 0;
    char *varname = (char *) name.c_str();
    unsigned int var_size = strlen(varname);

    ydb_buffer_t var_name;
    var_name.len_alloc = var_size;
    var_name.len_used = var_size;
    var_name.buf_addr = varname;

    uv_mutex_lock(&nodem::mutex);
    ydb_buffer_t *subs_array = NULL;

    if (subs.size() > 0) {
        subs_array = (ydb_buffer_t *) ydb_malloc(subs.size() * sizeof(ydb_buffer_t));

        if (subs_array == NULL) return -1;

        for (unsigned int i = 0; i < subs.size(); i++) {
            unsigned int subs_size = subs[i].length();

            subs_array[i].len_alloc = subs_size;
            subs_array[i].len_used = subs_size;
            subs_array[i].buf_addr = (gtm_char_t*) ydb_malloc(subs_size);

            if (subs_array[i].buf_addr == NULL) return -1;

            strncpy(subs_array[i].buf_addr, subs[i].c_str(), subs_size);
        }
    }

    int delete_type = YDB_DEL_TREE;

    ret_code = ydb_delete_s(&var_name, subs.size(), subs_array, delete_type);

    for (unsigned int i = 0; i < subs.size(); i++) {
        ydb_free(subs_array[i].buf_addr);
    }

    if (subs.size() > 0) ydb_free(subs_array);
    uv_mutex_unlock(&nodem::mutex);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::kill exit" << "\n";

    return ret_code;
} // @end kill function

/*
 * @function {public} order
 * @summary Return the next global or local node at the same level
 * @param {ydb_char_t} buffer - Data returned from YottaDB/GT.M database, via SimpleAPI interface
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t order(ydb_char_t buffer[], string name, vector<string> subs, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::order enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> buffer: " << buffer << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    int ret_code = 0;
    char *varname = (char *) name.c_str();
    unsigned int var_size = strlen(varname);

    ydb_buffer_t var_name;
    var_name.len_alloc = var_size;
    var_name.len_used = var_size;
    var_name.buf_addr = varname;

    uv_mutex_lock(&nodem::mutex);
    ydb_buffer_t *subs_array = NULL;

    if (subs.size() > 0) {
        subs_array = (ydb_buffer_t *) ydb_malloc(subs.size() * sizeof(ydb_buffer_t));

        if (subs_array == NULL) return -1;

        for (unsigned int i = 0; i < subs.size(); i++) {
            unsigned int subs_size = subs[i].length();

            subs_array[i].len_alloc = subs_size;
            subs_array[i].len_used = subs_size;
            subs_array[i].buf_addr = (gtm_char_t*) ydb_malloc(subs_size);

            if (subs_array[i].buf_addr == NULL) return -1;

            strncpy(subs_array[i].buf_addr, subs[i].c_str(), subs_size);
        }
    }

    ydb_buffer_t ret_value;
    ret_value.len_alloc = YDB_MAX_STR;
    ret_value.buf_addr = (char *) ydb_malloc(YDB_MAX_STR);

    if (ret_value.buf_addr == NULL) return -1;

    ret_code = ydb_subscript_next_s(&var_name, subs.size(), subs_array, &ret_value);

    strncpy(buffer, ret_value.buf_addr, ret_value.len_used);
    buffer[ret_value.len_used] = '\0';

    for (unsigned int i = 0; i < subs.size(); i++) {
        ydb_free(subs_array[i].buf_addr);
    }

    if (subs.size() > 0) ydb_free(subs_array);

    ydb_free(ret_value.buf_addr);
    uv_mutex_unlock(&nodem::mutex);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::order exit" << "\n";

    return ret_code;
} // @end order function

/*
 * @function {public} set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t set(string name, vector<string> subs, string data, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::set enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> data: " << data.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    int ret_code = 0;
    char *varname = (char *) name.c_str();
    unsigned int var_size = strlen(varname);

    ydb_buffer_t var_name;
    var_name.len_alloc = var_size;
    var_name.len_used = var_size;
    var_name.buf_addr = varname;

    uv_mutex_lock(&nodem::mutex);
    ydb_buffer_t *subs_array = NULL;

    if (subs.size() > 0) {
        subs_array = (ydb_buffer_t *) ydb_malloc(subs.size() * sizeof(ydb_buffer_t));

        if (subs_array == NULL) return -1;

        for (unsigned int i = 0; i < subs.size(); i++) {
            unsigned int subs_size = subs[i].length();

            subs_array[i].len_alloc = subs_size;
            subs_array[i].len_used = subs_size;
            subs_array[i].buf_addr = (gtm_char_t*) ydb_malloc(subs_size);

            if (subs_array[i].buf_addr == NULL) return -1;

            strncpy(subs_array[i].buf_addr, subs[i].c_str(), subs_size);
        }
    }

    char *data_name = (char *) data.c_str();
    unsigned int data_size = strlen(data_name);

    ydb_buffer_t data_buffer;
    data_buffer.len_alloc = data_size;
    data_buffer.len_used = data_size;
    data_buffer.buf_addr = data_name;

    ret_code = ydb_set_s(&var_name, subs.size(), subs_array, &data_buffer);

    for (unsigned int i = 0; i < subs.size(); i++) {
        ydb_free(subs_array[i].buf_addr);
    }

    if (subs.size() > 0) ydb_free(subs_array);
    uv_mutex_unlock(&nodem::mutex);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::set exit" << "\n";

    return ret_code;
} // @end set

} // @end ydb namespace

#endif // @end YDB_SIMPLE_API
