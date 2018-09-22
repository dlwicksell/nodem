/*
 * Package:    NodeM
 * File:       gtm.h
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

#ifndef GTM_H
#define GTM_H

extern "C" {
    #include <gtmxc_types.h>
}

#include <string>

namespace gtm {

gtm_status_t data(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t function(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t get(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t global_directory(gtm_char_t [], std::string, std::string, std::string, mode_t);
gtm_status_t increment(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t kill(std::string, std::string, mode_t);
gtm_status_t local_directory(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t lock(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t merge(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t next_node(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t order(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t previous(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t previous_node(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t procedure(gtm_char_t [], std::string, std::string, mode_t);
gtm_status_t set(std::string, std::string, std::string, mode_t);
gtm_status_t unlock(std::string, std::string, mode_t);

} // @end gtm namespace

#endif // @end GTM_H
