/*
 * Package:    NodeM
 * File:       gtm.hh
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

#ifndef GTM_HH
#   define GTM_HH

#include "nodem.hh"

namespace gtm {

#if NODEM_SIMPLE_API == 0
gtm_status_t data(nodem::NodemBaton*);
gtm_status_t get(nodem::NodemBaton*);
gtm_status_t set(nodem::NodemBaton*);
gtm_status_t kill(nodem::NodemBaton*);
gtm_status_t order(nodem::NodemBaton*);
gtm_status_t previous(nodem::NodemBaton*);
gtm_status_t next_node(nodem::NodemBaton*);
gtm_status_t previous_node(nodem::NodemBaton*);
gtm_status_t increment(nodem::NodemBaton*);
gtm_status_t lock(nodem::NodemBaton*);
gtm_status_t unlock(nodem::NodemBaton*);
#endif // @end NODEM_SIMPLE_API

gtm_status_t version(nodem::NodemBaton*);
gtm_status_t merge(nodem::NodemBaton*);
gtm_status_t function(nodem::NodemBaton*);
gtm_status_t procedure(nodem::NodemBaton*);

} // @end gtm namespace

#endif // @end GTM_HH
