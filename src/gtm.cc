/*
 * Package:    NodeM
 * File:       gtm.cc
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

#include <iostream>
#include <string>
#include "gtm.h"

namespace gtm {

using namespace v8;
using std::cout;
using std::string;

/*
 * @function {public} data
 * @summary Check if global or local node has data and/or children or not
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} glvn - Global or local variable name
 * @member {string} subs - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t data(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::data enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_data[] = "data";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_data;
    access.rtn_name.length = 4;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_data, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::data exit" << "\n";

    return stat_buf;
} // @end data function

/*
 * @function {public} get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} glvn - Global, local, or intrinsic special variable name
 * @member {string} subs - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t get(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::get enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_get[] = "get";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_get;
    access.rtn_name.length = 3;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_get, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::get exit" << "\n";

    return stat_buf;
} // @end get function

/*
 * @function {public} kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {Baton*} baton - struct containing the following members
 * @member {string} glvn - Global or local variable name
 * @member {string} subs - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t kill(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::kill enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_kill[] = "kill";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_kill;
    access.rtn_name.length = 4;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_kill, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::kill exit" << "\n";

    return stat_buf;
} // @end kill function

/*
 * @function {public} next_node
 * @summary Return the next global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} glvn - Global or local variable name
 * @member {string} subs - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t next_node(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::next_node enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_next_node[] = "next_node";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_next_node;
    access.rtn_name.length = 9;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_next_node, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::next_node exit" << "\n";

    return stat_buf;
} // @end next_node function

/*
 * @function {public} order
 * @summary Return the next global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} glvn - Global or local variable name
 * @member {string} subs - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t order(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::order enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_order[] = "order";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_order;
    access.rtn_name.length = 5;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_order, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::order exit" << "\n";

    return stat_buf;
} // @end order function

/*
 * @function {public} previous
 * @summary Return the previous global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} glvn - Global or local variable name
 * @member {string} subs - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::previous enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_previous[] = "previous";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_previous;
    access.rtn_name.length = 8;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_previous, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::previous exit" << "\n";

    return stat_buf;
} // @end previous function

/*
 * @function {public} previous_node
 * @summary Return the previous global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from YottaDB/GT.M, via the call-in interface
 * @member {string} glvn - Global or local variable name
 * @member {string} subs - Subscripts
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t previous_node(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::previous_node enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_previous_node[] = "previous_node";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_previous_node;
    access.rtn_name.length = 13;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_previous_node, baton->ret_buf, baton->glvn.c_str(), baton->subs.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::previous_node exit" << "\n";

    return stat_buf;
} // @end previous_node function

/*
 * @function {public} set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {string} glvn - Global, local, or intrinsic special variable name
 * @member {string} subs - Subscripts
 * @member {string} value - Value to set
 * @member {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t set(nodem::Baton* baton)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::set enter" << "\n";

    if (nodem::debug_g > nodem::MEDIUM) {
        cout << "DEBUG>>> glvn: " << baton->glvn << "\n";
        cout << "DEBUG>>> subs: " << baton->subs << "\n";
        cout << "DEBUG>>> value: " << baton->value << "\n";
        cout << "DEBUG>>> mode: " << baton->mode << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_set[] = "set";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_set;
    access.rtn_name.length = 3;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_cip(&access, baton->glvn.c_str(), baton->subs.c_str(), baton->value.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#else
    uv_mutex_lock(&nodem::mutex_g);
    stat_buf = gtm_ci(gtm_set, baton->glvn.c_str(), baton->subs.c_str(), baton->value.c_str(), baton->mode);
    uv_mutex_unlock(&nodem::mutex_g);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::set exit" << "\n";

    return stat_buf;
} // @end set function

} // @end gtm namespace
