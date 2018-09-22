/*
 * Package:    NodeM
 * File:       ydb.h
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

#ifndef YDB_H
#define YDB_H

extern "C" {
    #include <libydberrors.h>
    #include <libyottadb.h>
}

#include <string>
#include <vector>

namespace ydb {

ydb_status_t data(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t function(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t get(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t global_directory(ydb_char_t [], std::string, std::vector<std::string>, std::string, mode_t);
ydb_status_t increment(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t kill(std::string, std::vector<std::string>, mode_t);
ydb_status_t local_directory(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t lock(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t merge(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t next_node(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t order(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t previous(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t previous_node(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t procedure(ydb_char_t [], std::string, std::vector<std::string>, mode_t);
ydb_status_t set(std::string, std::vector<std::string>, std::string, mode_t);
ydb_status_t unlock(std::string, std::vector<std::string>, mode_t);

} // @end ydb namespace

#endif // @end YDB_H

#endif // @end YDB_SIMPLE_API
