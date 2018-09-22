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

#include "gtm.h"
#include "mumps.h"

#include <node.h>
#include <uv.h>

#include <iostream>

namespace gtm {

using namespace v8;
using std::string;
using std::cout;

/*
 * @function {public} get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {gtm_char_t} buffer - Data returned from YottaDB/GT.M database, via Call-in interface
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t get(gtm_char_t buffer[], string name, string subs, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::get enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> buffer: " << buffer << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_get[] = "get";

#if (GTM_CIP_API == 1)
    ci_name_descriptor access;

    access.rtn_name.address = gtm_get;
    access.rtn_name.length = 3;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_cip(&access, buffer, name.c_str(), subs.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#else
    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_ci(gtm_get, buffer, name.c_str(), subs.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::get exit" << "\n";

    return stat_buf;
} // @end get function

/*
 * @function {public} kill
 * @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t kill(string name, string subs, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::kill enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_kill[] = "kill";

#if (GTM_CIP_API == 1)
    ci_name_descriptor access;

    access.rtn_name.address = gtm_kill;
    access.rtn_name.length = 4;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_cip(&access, name.c_str(), subs.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#else
    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_ci(gtm_kill, name.c_str(), subs.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::kill exit" << "\n";

    return stat_buf;
} // @end kill function

/*
 * @function {public} order
 * @summary Return the next global or local node at the same level
 * @param {gtm_char_t} buffer - Data returned from YottaDB/GT.M database, via Call-in interface
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t order(gtm_char_t buffer[], string name, string subs, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::order enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> buffer: " << buffer << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_order[] = "order";

#if (GTM_CIP_API == 1)
    ci_name_descriptor access;

    access.rtn_name.address = gtm_order;
    access.rtn_name.length = 5;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_cip(&access, buffer, name.c_str(), subs.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#else
    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_ci(gtm_order, buffer, name.c_str(), subs.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::order exit" << "\n";

    return stat_buf;
} // @end order function

/*
 * @function {public} set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {string} name - Global, local, or intrinsic special variable name
 * @param {string} subs - Subscripts
 * @param {mode_t} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 * @returns {gtm_status_t} stat_buf - Return code; 0 is success, any other number is an error code
 */
gtm_status_t set(string name, string subs, string data, mode_t mode)
{
    if (nodem::debug_g > nodem::LOW) cout << "\nDEBUG>> gtm::set enter" << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> name: " << name << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> subs: " << subs << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> data: " << data << "\n";
    if (nodem::debug_g > nodem::MEDIUM) cout << "DEBUG>>> mode: " << mode << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_set[] = "set";

#if (GTM_CIP_API == 1)
    ci_name_descriptor access;

    access.rtn_name.address = gtm_set;
    access.rtn_name.length = 3;
    access.handle = NULL;

    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_cip(&access, name.c_str(), subs.c_str(), data.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#else
    uv_mutex_lock(&nodem::mutex);
    stat_buf = gtm_ci(gtm_set, name.c_str(), subs.c_str(), data.c_str(), mode);
    uv_mutex_unlock(&nodem::mutex);
#endif

    if (nodem::debug_g > nodem::LOW) cout << "DEBUG>> gtm::set exit" << "\n";

    return stat_buf;
} // @end set function

} // @end gtm namespace
