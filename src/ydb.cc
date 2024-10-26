/*
 * Package:    NodeM
 * File:       ydb.cc
 * Summary:    Functions that wrap calls to the SimpleAPI interface
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2018-2024 Fourth Watch Software LC
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License (AGPL) as published
 * by the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#if NODEM_SIMPLE_API == 1
#   include "ydb.hh"

using std::boolalpha;
using std::cerr;
using std::string;

namespace ydb {

/*
 * @function {private} ydb::extended_ref
 * @summary Set new global directory file (in $zgbldir), to support extended global references with the SimpleAPI
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {string} name - ISV name ($zbgldir)
 * @member {string} value - Value to set
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @param {string} save_result - The buffer containing the original value of the ISV ($zgbldir)
 * @param {bool} change_isv - Whether to change the ISV ($zgbldir) back after API call
 * @returns {ydb_status_t} - Return code; 0 is success, any other number is an error code
 */
static ydb_status_t extended_ref(nodem::NodemBaton* nodem_baton, string save_result, bool& change_isv)
{
    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    ydb::extended_ref enter");
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    value: ", nodem_baton->value.c_str());
    }

    char* var_name = (char*) nodem_baton->name.c_str();
    char* value_name = (char*) nodem_baton->value.c_str();

    if (strstr(var_name, "^[\"") == var_name && strrchr(var_name, ']') != NULL) {
        string save_var_name = var_name;
        string save_value_name = value_name;

        nodem_baton->name = string {"$zgbldir"};

        ydb_status_t get_stat = get(nodem_baton);

        if (get_stat != YDB_OK) return get_stat;

        save_result = string {nodem_baton->result};

        char* name;
        char* reference;

        nodem_baton->name = save_var_name;
        var_name = (char*) nodem_baton->name.c_str();

        var_name = strtok_r(var_name, "[", &name);
        reference = strtok_r(NULL, "]", &name);

        string new_var_name(var_name);
        new_var_name += string {name};

        reference = strtok_r(reference, "\"", &name);

        nodem_baton->value = string {reference};
        nodem_baton->name = string {"$zgbldir"};

        ydb_status_t set_stat = set(nodem_baton);

        if (set_stat != YDB_OK) return set_stat;

        nodem_baton->name = new_var_name;
        nodem_baton->value = save_value_name;

        change_isv = true;
    } else if (strstr(var_name, "^|\"") == var_name && strrchr(var_name, '|') != NULL && strrchr(var_name, '|') != (var_name + 1)) {
        string save_var_name = var_name;
        string save_value_name = value_name;

        nodem_baton->name = string {"$zgbldir"};

        ydb_status_t get_stat = get(nodem_baton);

        if (get_stat != YDB_OK) return get_stat;

        save_result = string {nodem_baton->result};

        char* name;
        char* reference;

        nodem_baton->name = save_var_name;
        var_name = (char*) nodem_baton->name.c_str();

        var_name = strtok_r(var_name, "|", &name);
        reference = strtok_r(NULL, "|", &name);

        string new_var_name(var_name);
        new_var_name += string {name};

        reference = strtok_r(reference, "\"", &name);

        nodem_baton->value = string {reference};
        nodem_baton->name = string {"$zgbldir"};

        ydb_status_t set_stat = set(nodem_baton);

        if (set_stat != YDB_OK) return set_stat;

        nodem_baton->name = new_var_name;
        nodem_baton->value = save_value_name;

        change_isv = true;
    } else if (change_isv) {
        nodem_baton->name = string {"$zgbldir"};
        nodem_baton->value = save_result;

        ydb_status_t set_stat = set(nodem_baton);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    ydb::extended_ref exit");
        nodem::debug_log(">>>    save_result: ", save_result);
        nodem::debug_log(">>>    change_isv: ", boolalpha, change_isv);
    }

    return YDB_OK;
} // @end ydb::extended_ref

// ***Begin Public APIs***

/*
 * @function ydb::data
 * @summary Check if global or local node has data and/or children or not
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t data(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::data enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    unsigned int  temp_value = 0;
    unsigned int* ret_value = &temp_value;

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t status = ydb_data_s(&glvn, subs_size, subs_array, ret_value);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    if (int len = snprintf(nodem_baton->result, sizeof(int), "%u", *ret_value) < 0) {
        char error[BUFSIZ];

        cerr << strerror_r(errno, error, BUFSIZ);

        status = len;
    }

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::data exit");

    return status;
} // @end ydb::data function

/*
 * @function ydb::get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t get(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::get enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    char get_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &get_data;

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t status = ydb_get_s(&glvn, subs_size, subs_array, &value);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    strncpy(nodem_baton->result, value.buf_addr, value.len_used);
    nodem_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::get exit");

    return status;
} // @end ydb::get function

/*
 * @function ydb::set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {string} value - Value to set
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t set(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::set enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    value: ", nodem_baton->value);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    char* value = (char*) nodem_baton->value.c_str();

    ydb_buffer_t data_node;
    data_node.len_alloc = data_node.len_used = strlen(value);
    data_node.buf_addr = value;

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t status = ydb_set_s(&glvn, subs_size, subs_array, &data_node);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::set exit");

    return status;
} // @end ydb::set

/*
 * @function ydb::kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {bool} node_only (<false>|true) - Whether to kill only the node, or the node and child subscripts
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t kill(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::kill enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    node_only: ", boolalpha, nodem_baton->node_only);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    ydb_status_t status;

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");

    if (nodem_baton->name == "") {
        ydb_buffer_t subs_array[1] = {8, 8, (char*) "v4wDebug"};

        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

        status = ydb_delete_excl_s(1, subs_array);
    } else {
        char* var_name = (char*) nodem_baton->name.c_str();

        ydb_buffer_t glvn;
        glvn.len_alloc = glvn.len_used = strlen(var_name);
        glvn.buf_addr = var_name;

        ydb_buffer_t subs_array[YDB_MAX_SUBS];
        unsigned int subs_size = nodem_baton->subs_array.size();

        for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
        }

        int delete_type = (nodem_baton->node_only) ? YDB_DEL_NODE : YDB_DEL_TREE;

        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

        status = ydb_delete_s(&glvn, subs_size, subs_array, delete_type);
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::kill exit");

    return status;
} // @end ydb::kill function

/*
 * @function ydb::order
 * @summary Return the next global or local node at the same level
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t order(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::order enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    char order_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &order_data;

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");

    ydb_status_t status;

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    status = ydb_subscript_next_s(&glvn, subs_size, subs_array, &value);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    while (strncmp(value.buf_addr, "v4w", 3) == 0 && subs_size == 0) {
        glvn.len_alloc = glvn.len_used = strlen(value.buf_addr);
        glvn.buf_addr = value.buf_addr;
        value.len_used = 0;

        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

        status = ydb_subscript_next_s(&glvn, subs_size, subs_array, &value);

        if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
        if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
        if (value.len_used == 0 || status != YDB_OK) break;
    }

    strncpy(nodem_baton->result, value.buf_addr, value.len_used);
    nodem_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::order exit");

    return status;
} // @end ydb::order function

/*
 * @function ydb::previous
 * @summary Return the previous global or local node at the same level
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t previous(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::previous enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    char previous_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &previous_data;

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");

    ydb_status_t status;

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    status = ydb_subscript_previous_s(&glvn, subs_size, subs_array, &value);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    while (strncmp(value.buf_addr, "v4w", 3) == 0 && subs_size == 0) {
        glvn.len_alloc = glvn.len_used = strlen(value.buf_addr);
        glvn.buf_addr = value.buf_addr;
        value.len_used = 0;

        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

        status = ydb_subscript_previous_s(&glvn, subs_size, subs_array, &value);

        if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
        if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
        if (value.len_used == 0 || status != YDB_OK) break;
    }

    strncpy(nodem_baton->result, value.buf_addr, value.len_used);
    nodem_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::previous exit");

    return status;
} // @end ydb::previous function

/*
 * @function ydb::next_node
 * @summary Return the next global or local node, depth first
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t next_node(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::next_node enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    int  subs_test = YDB_MAX_SUBS;
    int* subs_used = &subs_test;

    static char next_node_data[YDB_MAX_SUBS][YDB_MAX_STR];
    static ydb_buffer_t ret_array[YDB_MAX_SUBS];

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    for (int i = 0; i < YDB_MAX_SUBS; i++) {
        ret_array[i].len_alloc = YDB_MAX_STR;
        ret_array[i].len_used = 0;
        ret_array[i].buf_addr = (char*) &next_node_data[i][0];
    }

    ydb_status_t status = ydb_node_next_s(&glvn, subs_size, subs_array, subs_used, ret_array);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);

    nodem_baton->subs_array.clear();

    if (status != YDB_OK) {
        ydb_zstatus(nodem_baton->error, ERR_LEN);

        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
        if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::next_node exit");

        if (change_isv) {
            ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

            if (set_stat != YDB_OK) return set_stat;
        }

        return status;
    }

    if (*subs_used != YDB_NODE_END) {
        for (int i = 0; i < *subs_used; i++) {
            ret_array[i].buf_addr[ret_array[i].len_used] = '\0';
            nodem_baton->subs_array.push_back(ret_array[i].buf_addr);
        }
    } else {
        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
        if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::next_node exit");

        if (change_isv) {
            ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

            if (set_stat != YDB_OK) return set_stat;
        }

        nodem_baton->result[0] = '\0';
        return YDB_NODE_END;
    }

    char ret_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &ret_data;

    status = ydb_get_s(&glvn, *subs_used, ret_array, &value);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    strncpy(nodem_baton->result, value.buf_addr, value.len_used);
    nodem_baton->result[value.len_used] = '\0';

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::next_node exit");

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    return status;
} // @end ydb::next_node function

/*
 * @function ydb::previous_node
 * @summary Return the previous global or local node, depth first
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t previous_node(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::previous_node enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    int  subs_test = YDB_MAX_SUBS;
    int* subs_used = &subs_test;

    static char previous_node_data[YDB_MAX_SUBS][YDB_MAX_STR];
    static ydb_buffer_t ret_array[YDB_MAX_SUBS];

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    for (int i = 0; i < YDB_MAX_SUBS; i++) {
        ret_array[i].len_alloc = YDB_MAX_STR;
        ret_array[i].len_used = 0;
        ret_array[i].buf_addr = (char*) &previous_node_data[i][0];
    }

    ydb_status_t status = ydb_node_previous_s(&glvn, subs_size, subs_array, subs_used, ret_array);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);

    nodem_baton->subs_array.clear();

    if (status != YDB_OK) {
        ydb_zstatus(nodem_baton->error, ERR_LEN);

        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
        if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::previous_node exit");

        if (change_isv) {
            ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

            if (set_stat != YDB_OK) return set_stat;
        }

        return status;
    }

    if (*subs_used != YDB_NODE_END) {
        for (int i = 0; i < *subs_used; i++) {
            ret_array[i].buf_addr[ret_array[i].len_used] = '\0';
            nodem_baton->subs_array.push_back(ret_array[i].buf_addr);
        }
    } else {
        *subs_used = 0;
    }

    char ret_data[YDB_MAX_STR];

    ydb_buffer_t value;
    value.len_alloc = YDB_MAX_STR;
    value.len_used = 0;
    value.buf_addr = (char*) &ret_data;

    status = ydb_get_s(&glvn, *subs_used, ret_array, &value);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    if (subs_size == 0 || status == YDB_ERR_GVUNDEF || status == YDB_ERR_LVUNDEF) {
        if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::next_node exit");

        if (change_isv) {
            ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

            if (set_stat != YDB_OK) return set_stat;
        }

        nodem_baton->result[0] = '\0';
        return YDB_NODE_END;
    } else {
        strncpy(nodem_baton->result, value.buf_addr, value.len_used);
        nodem_baton->result[value.len_used] = '\0';
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::previous_node exit");

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    return status;
} // @end ydb::previous_node function

/*
 * @function ydb::increment
 * @summary Increment or decrement the number in a global or local node
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_double_t} option - Number to increment or decrement by
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t increment(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::increment enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    increment: ", nodem_baton->option);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    char incr_val[YDB_MAX_STR];

    if (snprintf(incr_val, YDB_MAX_STR, "%.16g", nodem_baton->option) < 0) {
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

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t status = ydb_incr_s(&glvn, subs_size, subs_array, &incr, &value);

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    strncpy(nodem_baton->result, value.buf_addr, value.len_used);
    nodem_baton->result[value.len_used] = '\0';

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::increment exit");

    return status;
} // @end ydb::increment function

/*
 * @function ydb::lock
 * @summary Lock a global or local node, incrementally
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_double_t} option - The time to wait for the lock, or -1 to wait forever
 * @member {ydb_char_t*} result - Data returned from YottaDB, via the SimpleAPI interface
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t lock(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::lock enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    timeout: ", nodem_baton->option);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    char* var_name = (char*) nodem_baton->name.c_str();

    ydb_buffer_t glvn;
    glvn.len_alloc = glvn.len_used = strlen(var_name);
    glvn.buf_addr = var_name;

    ydb_buffer_t subs_array[YDB_MAX_SUBS];
    unsigned int subs_size = nodem_baton->subs_array.size();

    for (unsigned int i = 0; i < subs_size; i++) {
        subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
        subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
    }

    unsigned long long timeout;

    if (nodem_baton->option == -1) {
        timeout = YDB_MAX_TIME_NSEC;
    } else {
        timeout = nodem_baton->option * 1000000000;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    ydb_status_t status = ydb_lock_incr_s(timeout, &glvn, subs_size, subs_array);

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);

    if (status == YDB_OK) {
        strncpy(nodem_baton->result, "1\0", 2);
    } else if (status == YDB_LOCK_TIMEOUT) {
        strncpy(nodem_baton->result, "0\0", 2);

        status = YDB_OK;
    } else {
        ydb_zstatus(nodem_baton->error, ERR_LEN);
    }

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::lock exit");

    return status;
} // @end ydb::lock function

/*
 * @function ydb::unlock
 * @summary Lock a global or local node, incrementally
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {vector<string>} subs_array - Subscripts
 * @member {ydb_char_t*} error - Error message returned from YottaDB, via the SimpleAPI interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {ydb_status_t} status - Return code; 0 is success, any other number is an error code
 */
ydb_status_t unlock(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::unlock enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);

        if (nodem_baton->subs_array.size()) {
            for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
                nodem::debug_log(">>>    subscripts[", i, "]: ", nodem_baton->subs_array[i]);
            }
        }
    }

    string save_result;
    bool change_isv = false;

    if (nodem_baton->name.compare(0, 2, "^[") == 0 || nodem_baton->name.compare(0, 2, "^|") == 0) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    ydb_status_t status;

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using SimpleAPI");

    if (nodem_baton->name == "") {
        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

        status = ydb_lock_s(0, 0);
    } else {
        char* var_name = (char*) nodem_baton->name.c_str();

        ydb_buffer_t glvn;
        glvn.len_alloc = glvn.len_used = strlen(var_name);
        glvn.buf_addr = var_name;

        ydb_buffer_t subs_array[YDB_MAX_SUBS];
        unsigned int subs_size = nodem_baton->subs_array.size();

        for (unsigned int i = 0; i < subs_size; i++) {
            subs_array[i].len_alloc = subs_array[i].len_used = nodem_baton->subs_array[i].length();
            subs_array[i].buf_addr = (char*) nodem_baton->subs_array[i].c_str();
        }

        if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

        status = ydb_lock_decr_s(&glvn, subs_size, subs_array);
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != YDB_OK) ydb_zstatus(nodem_baton->error, ERR_LEN);
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);

    if (change_isv) {
        ydb_status_t set_stat = extended_ref(nodem_baton, save_result, change_isv);

        if (set_stat != YDB_OK) return set_stat;
    }

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   ydb::unlock exit");

    return status;
} // @end ydb::unlock function

// ***End Public APIs***

} // @end ydb namespace

#endif // @end NODEM_SIMPLE_API
