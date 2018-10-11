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
using std::vector;

/*
 * @function {public} get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {ydb_char_t} buffer - Data returned from YottaDB/GT.M database, via SimpleAPI interface
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t get(ydb_char_t buffer[], string name, vector<string> subs)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::get enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> buffer: " << buffer << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";

    char *var_name = (char *) name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];

    for (unsigned int i = 0; i < subs.size(); i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = subs[i].length();
            subs_array[i].buf_addr = (char *) subs[i].c_str();
    }

    char data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char *) &data;

    uv_mutex_lock(&nodem::mutex);
    int code = ydb_get_s(&glvn, subs.size(), subs_array, &value);
    uv_mutex_unlock(&nodem::mutex);

    strncpy(buffer, value.buf_addr, value.len_used);
    buffer[value.len_used] = '\0';

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::get exit" << "\n";

    return code;
} // @end get function

/*
 * @function {public} kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {string} name - Global or local variable name
 * @param {string} subs - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t kill(string name, vector<string> subs)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::kill enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";

    char *var_name = (char *) name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];

    for (unsigned int i = 0; i < subs.size(); i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = subs[i].length();
            subs_array[i].buf_addr = (char *) subs[i].c_str();
    }

    int delete_type = YDB_DEL_TREE;

    uv_mutex_lock(&nodem::mutex);
    int code = ydb_delete_s(&glvn, subs.size(), subs_array, delete_type);
    uv_mutex_unlock(&nodem::mutex);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::kill exit" << "\n";

    return code;
} // @end kill function

/*
 * @function {public} order
 * @summary Return the next global or local node at the same level
 * @param {ydb_char_t} buffer - Data returned from YottaDB/GT.M database, via SimpleAPI interface
 * @param {string} name - Global or local variable name
 * @param {string} subs - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t order(ydb_char_t buffer[], string name, vector<string> subs)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::order enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> buffer: " << buffer << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";

    char *var_name = (char *) name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];

    for (unsigned int i = 0; i < subs.size(); i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = subs[i].length();
            subs_array[i].buf_addr = (char *) subs[i].c_str();
    }

    char data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char *) &data;

    uv_mutex_lock(&nodem::mutex);
    int code = ydb_subscript_next_s(&glvn, subs.size(), subs_array, &value);
    uv_mutex_unlock(&nodem::mutex);

    strncpy(buffer, value.buf_addr, value.len_used);
    buffer[value.len_used] = '\0';

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::order exit" << "\n";

    return code;
} // @end order function

/*
 * @function {public} set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @returns {ydb_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
ydb_status_t set(string name, vector<string> subs, string data)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> ydb::set enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name.c_str() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs.size() << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> data: " << data.c_str() << "\n";

    char *var_name = (char *) name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];

    for (unsigned int i = 0; i < subs.size(); i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = subs[i].length();
            subs_array[i].buf_addr = (char *) subs[i].c_str();
    }

    char *value = (char *) data.c_str();

    ydb_buffer_t data_node;
    data_node.len_alloc = data_node.len_used = strlen(value);
    data_node.buf_addr = value;

    uv_mutex_lock(&nodem::mutex);
    int code = ydb_set_s(&glvn, subs.size(), subs_array, &data_node);
    uv_mutex_unlock(&nodem::mutex);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> ydb::set exit" << "\n";

    return code;
} // @end set

} // @end ydb namespace

#endif // @end YDB_SIMPLE_API
