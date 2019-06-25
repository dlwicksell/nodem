/*
 * Package:    NodeM
 * File:       gtm.h
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

#ifndef GTM_H
#define GTM_H

#include "mumps.h"

namespace gtm {

#if GTM_CIP_API == 1
extern ci_name_descriptor debug_access_g, function_access_g, procedure_access_g;
#endif

extern gtm_char_t gtm_debug_g[];
extern gtm_char_t gtm_function_g[];
extern gtm_char_t gtm_procedure_g[];

#if YDB_SIMPLE_API == 0
#if GTM_CIP_API == 1
extern ci_name_descriptor data_access_g, get_access_g, kill_access_g, next_node_access_g,
                          order_access_g, previous_access_g, previous_node_access_g, set_access_g;
#endif

extern gtm_char_t gtm_data_g[];
extern gtm_char_t gtm_get_g[];
extern gtm_char_t gtm_kill_g[];
extern gtm_char_t gtm_next_node_g[];
extern gtm_char_t gtm_order_g[];
extern gtm_char_t gtm_previous_g[];
extern gtm_char_t gtm_previous_node_g[];
extern gtm_char_t gtm_set_g[];
#endif

gtm_status_t function(nodem::Baton*);
gtm_status_t procedure(nodem::Baton*);

gtm_status_t global_directory(nodem::Baton*);
gtm_status_t increment(nodem::Baton*);
gtm_status_t local_directory(nodem::Baton*);
gtm_status_t lock(nodem::Baton*);
gtm_status_t merge(nodem::Baton*);
gtm_status_t unlock(nodem::Baton*);

#if YDB_SIMPLE_API == 0
gtm_status_t data(nodem::Baton*);
gtm_status_t get(nodem::Baton*);
gtm_status_t kill(nodem::Baton*);
gtm_status_t next_node(nodem::Baton*);
gtm_status_t order(nodem::Baton*);
gtm_status_t previous(nodem::Baton*);
gtm_status_t previous_node(nodem::Baton*);
gtm_status_t set(nodem::Baton*);
#endif

} // @end gtm namespace

#endif // @end GTM_H
