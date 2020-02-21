/*
 * Package:    NodeM
 * File:       gtm.cc
 * Summary:    Functions that wrap calls to the Call-in interface
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

#include "gtm.h"

using std::cerr;

namespace gtm {

#if NODEM_SIMPLE_API == 0

// ***Begin Public APIs***

/*
 * @function gtm::data
 * @summary Check if global or local node has data and/or children or not
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t data(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::data enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_data[] = "data";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor data_access;

    data_access.rtn_name.address = gtm_data;
    data_access.rtn_name.length = strlen(gtm_data);
    data_access.handle = NULL;

    stat_buf = gtm_cip(&data_access, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_data, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::data exit");

    return stat_buf;
} // @end gtm::data function

/*
 * @function gtm::get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t get(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::get enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_get[] = "get";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor get_access;

    get_access.rtn_name.address = gtm_get;
    get_access.rtn_name.length = strlen(gtm_get);
    get_access.handle = NULL;

    stat_buf = gtm_cip(&get_access, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_get, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::get exit");

    return stat_buf;
} // @end gtm::get function

/*
 * @function gtm::set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {string} args - Subscripts
 * @member {string} value - Value to set
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t set(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::set enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    value: ", gtm_baton->value);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_set[] = "set";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor set_access;

    set_access.rtn_name.address = gtm_set;
    set_access.rtn_name.length = strlen(gtm_set);
    set_access.handle = NULL;

    stat_buf = gtm_cip(&set_access, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->value.c_str(), gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_set, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->value.c_str(), gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::set exit");

    return stat_buf;
} // @end gtm::set function

/*
 * @function gtm::kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {int32_t} node_only (-1|<0>|1) - Whether to kill only the node, or also kill child subscripts; 0 is children, 1 node-only
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t kill(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::kill enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    node_only: ", gtm_baton->node_only);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_kill[] = "kill";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor kill_access;

    kill_access.rtn_name.address = gtm_kill;
    kill_access.rtn_name.length = strlen(gtm_kill);
    kill_access.handle = NULL;

    stat_buf = gtm_cip(&kill_access, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->node_only, gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_kill, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->node_only, gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::kill exit");

    return stat_buf;
} // @end gtm::kill function

/*
 * @function gtm::order
 * @summary Return the next global or local node at the same level
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t order(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::order enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_order[] = "order";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor order_access;

    order_access.rtn_name.address = gtm_order;
    order_access.rtn_name.length = strlen(gtm_order);
    order_access.handle = NULL;

    stat_buf = gtm_cip(&order_access, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_order, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::order exit");

    return stat_buf;
} // @end gtm::order function

/*
 * @function gtm::previous
 * @summary Return the previous global or local node at the same level
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::previous enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_previous[] = "previous";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor previous_access;

    previous_access.rtn_name.address = gtm_previous;
    previous_access.rtn_name.length = strlen(gtm_previous);
    previous_access.handle = NULL;

    stat_buf = gtm_cip(&previous_access, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_previous, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::previous exit");

    return stat_buf;
} // @end gtm::previous function

/*
 * @function gtm::next_node
 * @summary Return the next global or local node, depth first
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t next_node(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::next_node enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_next_node[] = "next_node";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor next_node_access;

    next_node_access.rtn_name.address = gtm_next_node;
    next_node_access.rtn_name.length = strlen(gtm_next_node);
    next_node_access.handle = NULL;

    stat_buf = gtm_cip(&next_node_access, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_next_node, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::next_node exit");

    return stat_buf;
} // @end gtm::next_node function

/*
 * @function gtm::previous_node
 * @summary Return the previous global or local node, depth first
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous_node(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::previous_node enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_previous_node[] = "previous_node";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor previous_node_access;

    previous_node_access.rtn_name.address = gtm_previous_node;
    previous_node_access.rtn_name.length = strlen(gtm_previous_node);
    previous_node_access.handle = NULL;

    stat_buf = gtm_cip(&previous_node_access, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_previous_node, gtm_baton->result, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::previous_node exit");

    return stat_buf;
} // @end gtm::previous_node function

/*
 * @function gtm::increment
 * @summary Increment or decrement the number in a global or local node
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {gtm_double_t} increment - Number to increment or decrement by
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t increment(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::increment enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    subscripts: ", gtm_baton->args);
        nodem::debug_log(">>>    increment: ", gtm_baton->incr);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_increment[] = "increment";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor increment_access;

    increment_access.rtn_name.address = gtm_increment;
    increment_access.rtn_name.length = strlen(gtm_increment);
    increment_access.handle = NULL;

    stat_buf = gtm_cip(&increment_access, gtm_baton->result, gtm_baton->name.c_str(),
      gtm_baton->args.c_str(), gtm_baton->incr, gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_increment, gtm_baton->result, gtm_baton->name.c_str(),
      gtm_baton->args.c_str(), gtm_baton->incr, gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::increment exit");

    return stat_buf;
} // @end gtm::increment function

#endif // @end NODEM_SIMPLE_API

/*
 * @function gtm::function
 * @summary Call a Mumps extrinsic function
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {string} name - Function name, with line label
 * @member {string} args - Arguments
 * @member {uint32_t} relink (<0>|1) - Whether to relink the function before calling it
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t function(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::function enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    arguments: ", gtm_baton->args);
        nodem::debug_log(">>>    relink: ", gtm_baton->relink);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_function[] = "function";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor function_access;

    function_access.rtn_name.address = gtm_function;
    function_access.rtn_name.length = strlen(gtm_function);
    function_access.handle = NULL;

    stat_buf = gtm_cip(&function_access, gtm_baton->result, gtm_baton->name.c_str(),
      gtm_baton->args.c_str(), gtm_baton->relink, gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_function, gtm_baton->result, gtm_baton->name.c_str(),
      gtm_baton->args.c_str(), gtm_baton->relink, gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::function exit");

    return stat_buf;
} // @end gtm::function function

/*
 * @function gtm::procedure
 * @summary Call a Mumps procedure/subroutine
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {string} name - Procedure name, with line label
 * @member {string} args - Arguments
 * @member {uint32_t} relink (<0>|1) - Whether to relink the procedure before calling it
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t procedure(nodem::GtmBaton* gtm_baton)
{
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::procedure enter");

    if (gtm_baton->gtm_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", gtm_baton->name);
        nodem::debug_log(">>>    arguments: ", gtm_baton->args);
        nodem::debug_log(">>>    relink: ", gtm_baton->relink);
        nodem::debug_log(">>>    mode: ", gtm_baton->mode);
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_procedure[] = "procedure";

#if NODEM_CIP_API == 1
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor procedure_access;

    procedure_access.rtn_name.address = gtm_procedure;
    procedure_access.rtn_name.length = strlen(gtm_procedure);
    procedure_access.handle = NULL;

    stat_buf = gtm_cip(&procedure_access, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->relink, gtm_baton->mode);
#else
    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   call using gtm_ci");

    stat_buf = gtm_ci(gtm_procedure, gtm_baton->name.c_str(), gtm_baton->args.c_str(), gtm_baton->relink, gtm_baton->mode);
#endif

    if (stat_buf != EXIT_SUCCESS)
        gtm_zstatus(gtm_baton->error, ERR_LEN);

    if (gtm_baton->gtm_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);

    if (gtm_baton->gtm_state->debug > nodem::LOW)
        nodem::debug_log(">>   gtm::procedure exit");

    return stat_buf;
} // @end gtm::procedure function

// ***End Public APIs***

} // @end gtm namespace
