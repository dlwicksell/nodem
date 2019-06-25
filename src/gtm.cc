/*
 * Package:    NodeM
 * File:       gtm.cc
 * Summary:    Functions that wrap calls to the call-in interface
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

#include <iostream>
#include <string>
#include "gtm.h"

namespace gtm {

#if GTM_CIP_API == 1
ci_name_descriptor debug_access_g, function_access_g, procedure_access_g;
#endif

gtm_char_t gtm_debug_g[] = "debug";
gtm_char_t gtm_function_g[] = "function";
gtm_char_t gtm_procedure_g[] = "procedure";

#if YDB_SIMPLE_API == 0
#if GTM_CIP_API == 1
ci_name_descriptor data_access_g, get_access_g, kill_access_g, next_node_access_g,
                   order_access_g, previous_access_g, previous_node_access_g, set_access_g;
#endif

gtm_char_t gtm_data_g[] = "data";
gtm_char_t gtm_get_g[] = "get";
gtm_char_t gtm_kill_g[] = "kill";
gtm_char_t gtm_next_node_g[] = "next_node";
gtm_char_t gtm_order_g[] = "order";
gtm_char_t gtm_previous_g[] = "previous";
gtm_char_t gtm_previous_node_g[] = "previous_node";
gtm_char_t gtm_set_g[] = "set";
#endif

using namespace v8;
using std::cout;
using std::string;

/*
 * @function {public} function
 * @summary Call a MUMPS extrinsic function
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} name - Function name, with line label
 * @member {string} args - Arguments
 * @member {uint32_t} relink (<0>|1) - Whether to relink the function before calling it
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t function(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::function enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> arguments: " << baton->args << "\n";
        cout << "DEBUG>>> relink: " << baton->relink << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&function_access_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->relink, baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_function_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->relink, baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::function exit" << "\n";

    return stat_buf;
} // @end function function

/*
 * @function {public} procedure
 * @summary Call a MUMPS procedure/subroutine
 * @param {Baton*} baton - struct containing the following members
 * @member {string} name - Procedure name, with line label
 * @member {string} args - Arguments
 * @member {uint32_t} relink (<0>|1) - Whether to relink the procedure before calling it
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t procedure(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::procedure enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> arguments: " << baton->args << "\n";
        cout << "DEBUG>>> relink: " << baton->relink << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&procedure_access_g, baton->name.c_str(), baton->args.c_str(), baton->relink, baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_procedure_g, baton->name.c_str(), baton->args.c_str(), baton->relink, baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::procedure exit" << "\n";

    return stat_buf;
} // @end procedure function

#if YDB_SIMPLE_API == 0

/*
 * @function {public} data
 * @summary Check if global or local node has data and/or children or not
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t data(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::data enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&data_access_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_data_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::data exit" << "\n";

    return stat_buf;
} // @end data function

/*
 * @function {public} get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t get(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::get enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&get_access_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_get_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::get exit" << "\n";

    return stat_buf;
} // @end get function

/*
 * @function {public} kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {Baton*} baton - struct containing the following members
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {int32_t} node_only (-1|<0>|1) - Whether to kill only the node, or also kill child subscripts; 0 is children, 1 node-only
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t kill(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::kill enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> node_only: " << baton->node_only << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&kill_access_g, baton->name.c_str(), baton->args.c_str(), baton->node_only, baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_kill_g, baton->name.c_str(), baton->args.c_str(), baton->node_only, baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::kill exit" << "\n";

    return stat_buf;
} // @end kill function

/*
 * @function {public} next_node
 * @summary Return the next global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t next_node(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::next_node enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&next_node_access_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_next_node_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::next_node exit" << "\n";

    return stat_buf;
} // @end next_node function

/*
 * @function {public} order
 * @summary Return the next global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t order(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::order enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&order_access_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_order_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::order exit" << "\n";

    return stat_buf;
} // @end order function

/*
 * @function {public} previous
 * @summary Return the previous global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::previous enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&previous_access_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_previous_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::previous exit" << "\n";

    return stat_buf;
} // @end previous function

/*
 * @function {public} previous_node
 * @summary Return the previous global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} name - Global or local variable name
 * @member {string} args - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous_node(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::previous_node enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&previous_node_access_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_previous_node_g, baton->ret_buf, baton->name.c_str(), baton->args.c_str(), baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::previous_node exit" << "\n";

    return stat_buf;
} // @end previous_node function

/*
 * @function {public} set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {string} name - Global, local, or intrinsic special variable name
 * @member {string} args - Subscripts
 * @member {string} value - Value to set
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t set(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::set enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> name: " << baton->name << "\n";
        cout << "DEBUG>>> subscripts: " << baton->args << "\n";
        cout << "DEBUG>>> value: " << baton->value << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&nodem::mutex_g);
#if GTM_CIP_API == 1
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_cip" << "\n";

    stat_buf = gtm_cip(&set_access_g, baton->name.c_str(), baton->args.c_str(), baton->value.c_str(), baton->mode);
#else
    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> call using gtm_ci" << "\n";

    stat_buf = gtm_ci(gtm_set_g, baton->name.c_str(), baton->args.c_str(), baton->value.c_str(), baton->mode);
#endif
    if (stat_buf != EXIT_SUCCESS) gtm_zstatus(baton->msg_buf, MSG_LEN);
    uv_mutex_unlock(&nodem::mutex_g);

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::set exit" << "\n";

    return stat_buf;
} // @end set function

#endif

} // @end gtm namespace
