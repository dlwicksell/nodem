/*
 * Package:    NodeM
 * File:       gtm.cc
 * Summary:    Functions that wrap calls to the Call-in interface
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

#include "gtm.hh"

using std::boolalpha;
using std::cerr;

namespace gtm {

#if NODEM_SIMPLE_API == 0
// ***Begin Public APIs***

/*
 * @function gtm::data
 * @summary Check if global or local node has data and/or children or not
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t data(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::data enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_data[] = "data";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor data_access;

    data_access.rtn_name.address = gtm_data;
    data_access.rtn_name.length = strlen(gtm_data);
    data_access.handle = NULL;

    status = gtm_cip(&data_access, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_data, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::data exit");

    return status;
} // @end gtm::data function

/*
 * @function gtm::get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t get(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::get enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_get[] = "get";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor get_access;

    get_access.rtn_name.address = gtm_get;
    get_access.rtn_name.length = strlen(gtm_get);
    get_access.handle = NULL;

    status = gtm_cip(&get_access, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_get, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::get exit");

    return status;
} // @end gtm::get function

/*
 * @function gtm::set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {string} args - Subscripts
 * @member {string} value - Value to set
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t set(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::set enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    value: ", nodem_baton->value);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_set[] = "set";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor set_access;

    set_access.rtn_name.address = gtm_set;
    set_access.rtn_name.length = strlen(gtm_set);
    set_access.handle = NULL;

    status = gtm_cip(&set_access, nodem_baton->name.c_str(), nodem_baton->args.c_str(),
             nodem_baton->value.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_set, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->value.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::set exit");

    return status;
} // @end gtm::set function

/*
 * @function gtm::kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {bool} node_only (<false>|true) - Whether to kill only the node, or the node and child subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t kill(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::kill enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    node_only: ", boolalpha, nodem_baton->node_only);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_kill[] = "kill";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor kill_access;

    kill_access.rtn_name.address = gtm_kill;
    kill_access.rtn_name.length = strlen(gtm_kill);
    kill_access.handle = NULL;

    status = gtm_cip(&kill_access, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->node_only, nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_kill, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->node_only, nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::kill exit");

    return status;
} // @end gtm::kill function

/*
 * @function gtm::order
 * @summary Return the next global or local node at the same level
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t order(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::order enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_order[] = "order";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor order_access;

    order_access.rtn_name.address = gtm_order;
    order_access.rtn_name.length = strlen(gtm_order);
    order_access.handle = NULL;

    status = gtm_cip(&order_access, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_order, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::order exit");

    return status;
} // @end gtm::order function

/*
 * @function gtm::previous
 * @summary Return the previous global or local node at the same level
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::previous enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_previous[] = "previous";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor previous_access;

    previous_access.rtn_name.address = gtm_previous;
    previous_access.rtn_name.length = strlen(gtm_previous);
    previous_access.handle = NULL;

    status = gtm_cip(&previous_access, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_previous, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::previous exit");

    return status;
} // @end gtm::previous function

/*
 * @function gtm::next_node
 * @summary Return the next global or local node, depth first
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t next_node(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::next_node enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_next_node[] = "next_node";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor next_node_access;

    next_node_access.rtn_name.address = gtm_next_node;
    next_node_access.rtn_name.length = strlen(gtm_next_node);
    next_node_access.handle = NULL;

    status = gtm_cip(&next_node_access, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_next_node, nodem_baton->result, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::next_node exit");

    return status;
} // @end gtm::next_node function

/*
 * @function gtm::previous_node
 * @summary Return the previous global or local node, depth first
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous_node(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::previous_node enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_previous_node[] = "previous_node";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor previous_node_access;

    previous_node_access.rtn_name.address = gtm_previous_node;
    previous_node_access.rtn_name.length = strlen(gtm_previous_node);
    previous_node_access.handle = NULL;

    status = gtm_cip(&previous_node_access, nodem_baton->result, nodem_baton->name.c_str(),
               nodem_baton->args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");

    status = gtm_ci(gtm_previous_node, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::previous_node exit");

    return status;
} // @end gtm::previous_node function

/*
 * @function gtm::increment
 * @summary Increment or decrement the number in a global or local node
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {gtm_double_t} option - Number to increment or decrement by
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t increment(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::increment enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    increment: ", nodem_baton->option);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_increment[] = "increment";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor increment_access;

    increment_access.rtn_name.address = gtm_increment;
    increment_access.rtn_name.length = strlen(gtm_increment);
    increment_access.handle = NULL;

    status = gtm_cip(&increment_access, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->option, nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");

    status = gtm_ci(gtm_increment, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->option, nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::increment exit");

    return status;
} // @end gtm::increment function

/*
 * @function gtm::lock
 * @summary Lock a global or local node, incrementally
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {gtm_double_t} option - The time to wait for the lock, or -1 to wait forever
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t lock(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::lock enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    timeout: ", nodem_baton->option);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_lock[] = "lock";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor lock_access;

    lock_access.rtn_name.address = gtm_lock;
    lock_access.rtn_name.length = strlen(gtm_lock);
    lock_access.handle = NULL;

    status = gtm_cip(&lock_access, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->option, nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");

    status = gtm_ci(gtm_lock, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->option, nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::lock exit");

    return status;
} // @end gtm::lock function

/*
 * @function gtm::unlock
 * @summary Unlock a global or local node, incrementally, or release all locks
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t unlock(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::unlock enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_unlock[] = "unlock";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor unlock_access;

    unlock_access.rtn_name.address = gtm_unlock;
    unlock_access.rtn_name.length = strlen(gtm_unlock);
    unlock_access.handle = NULL;

    status = gtm_cip(&unlock_access, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_unlock, nodem_baton->name.c_str(), nodem_baton->args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::unlock exit");

    return status;
} // @end gtm::unlock function
#endif // @end NODEM_SIMPLE_API

/*
 * @function gtm::version
 * @summary Return the about/version string
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - The Nodem version string
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t version(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::version enter");
    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) nodem::debug_log(">>>    version: ", nodem_baton->name);

    gtm_status_t status;

    if (nodem::nodem_state_g < nodem::OPEN) return 0;
    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_version[] = "version";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor version_access;

    version_access.rtn_name.address = gtm_version;
    version_access.rtn_name.length = strlen(gtm_version);
    version_access.handle = NULL;

    status = gtm_cip(&version_access, nodem_baton->result, nodem_baton->name.c_str());
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");
    status = gtm_ci(gtm_version, nodem_baton->result, nodem_baton->name.c_str());
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::version exit");

    return status;
} // @end gtm::version function

/*
 * @function gtm::merge
 * @summary Merge a global or local array tree to another global or local array tree
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Global or local variable name to merge from
 * @member {string} args - Subscripts to merge from
 * @member {string} to_name - Global or local variable name to merge to
 * @member {string} to_args - Subscripts to merge to
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t merge(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::merge enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    from_name: ", nodem_baton->name);
        nodem::debug_log(">>>    from_subscripts: ", nodem_baton->args);
        nodem::debug_log(">>>    to_name: ", nodem_baton->to_name);
        nodem::debug_log(">>>    to_subscripts: ", nodem_baton->to_args);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
    }

    gtm_status_t status;

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_merge[] = "merge";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor merge_access;

    merge_access.rtn_name.address = gtm_merge;
    merge_access.rtn_name.length = strlen(gtm_merge);
    merge_access.handle = NULL;

    status = gtm_cip(&merge_access, nodem_baton->name.c_str(), nodem_baton->args.c_str(),
             nodem_baton->to_name.c_str(), nodem_baton->to_args.c_str(), nodem_baton->mode);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");

    status = gtm_ci(gtm_merge, nodem_baton->name.c_str(), nodem_baton->args.c_str(),
             nodem_baton->to_name.c_str(), nodem_baton->to_args.c_str(), nodem_baton->mode);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::merge exit");

    return status;
} // @end gtm::merge function

/*
 * @function gtm::function
 * @summary Call an M extrinsic function
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Function name, with line label
 * @member {string} args - Arguments
 * @member {uint32_t} relink (<0>|1) - Whether to relink the function before calling it
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_uint_t*} info - Indirection limit on input - (0|1) Return data type on output; 0 is string, 1 is canonical number
 * @member {gtm_char_t*} result - Data returned from YottaDB/GT.M, via the Call-in interface
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t function(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::function enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    arguments: ", nodem_baton->args);
        nodem::debug_log(">>>    relink: ", nodem_baton->relink);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
        nodem::debug_log(">>>    info: ", nodem_baton->info);
    }

    gtm_status_t status;

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_function[] = "function";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor function_access;

    function_access.rtn_name.address = gtm_function;
    function_access.rtn_name.length = strlen(gtm_function);
    function_access.handle = NULL;

    status = gtm_cip(&function_access, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->relink, nodem_baton->mode, &nodem_baton->info);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");

    status = gtm_ci(gtm_function, nodem_baton->result, nodem_baton->name.c_str(),
             nodem_baton->args.c_str(), nodem_baton->relink, nodem_baton->mode, &nodem_baton->info);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::function exit");

    return status;
} // @end gtm::function function

/*
 * @function gtm::procedure
 * @summary Call an M procedure/routine
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {string} name - Procedure name, with line label
 * @member {string} args - Arguments
 * @member {uint32_t} relink (<0>|1) - Whether to relink the procedure before calling it
 * @member {mode_t} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 * @member {gtm_uint_t} info - Indirection limit
 * @member {gtm_char_t*} error - Error message returned from YottaDB/GT.M, via the Call-in interface
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {gtm_status_t} status - Return code; 0 is success, any other number is an error code
 */
gtm_status_t procedure(nodem::NodemBaton* nodem_baton)
{
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::procedure enter");

    if (nodem_baton->nodem_state->debug > nodem::MEDIUM) {
        nodem::debug_log(">>>    name: ", nodem_baton->name);
        nodem::debug_log(">>>    arguments: ", nodem_baton->args);
        nodem::debug_log(">>>    relink: ", nodem_baton->relink);
        nodem::debug_log(">>>    mode: ", nodem_baton->mode);
        nodem::debug_log(">>>    info: ", nodem_baton->info);
    }

    gtm_status_t status;

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_lock(&nodem::mutex_g);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

    gtm_char_t gtm_procedure[] = "procedure";

#if NODEM_CIP_API == 1
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_cip");

    ci_name_descriptor procedure_access;

    procedure_access.rtn_name.address = gtm_procedure;
    procedure_access.rtn_name.length = strlen(gtm_procedure);
    procedure_access.handle = NULL;

    status = gtm_cip(&procedure_access, nodem_baton->name.c_str(), nodem_baton->args.c_str(),
             nodem_baton->relink, nodem_baton->mode, nodem_baton->info);
#else
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   call using gtm_ci");

    status = gtm_ci(gtm_procedure, nodem_baton->name.c_str(), nodem_baton->args.c_str(),
             nodem_baton->relink, nodem_baton->mode, nodem_baton->info);
#endif

    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   status: ", status);
    if (status != EXIT_SUCCESS) gtm_zstatus(nodem_baton->error, ERR_LEN);

    if (nodem_baton->nodem_state->debug > nodem::LOW) {
        funlockfile(stderr);

        if (dup2(nodem::save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    if (nodem_baton->nodem_state->tp_level == 0) uv_mutex_unlock(&nodem::mutex_g);
    if (nodem_baton->nodem_state->debug > nodem::LOW) nodem::debug_log(">>   gtm::procedure exit");

    return status;
} // @end gtm::procedure function

// ***End Public APIs***

} // @end gtm namespace
