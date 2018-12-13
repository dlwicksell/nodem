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

#include "mumps.h"

namespace ydb {

ydb_status_t data(nodem::Baton*);
ydb_status_t function(nodem::Baton*);
ydb_status_t get(nodem::Baton*);
ydb_status_t global_directory(nodem::Baton*);
ydb_status_t increment(nodem::Baton*);
ydb_status_t kill(nodem::Baton*);
ydb_status_t local_directory(nodem::Baton*);
ydb_status_t lock(nodem::Baton*);
ydb_status_t merge(nodem::Baton*);
ydb_status_t next_node(nodem::Baton*);
ydb_status_t order(nodem::Baton*);
ydb_status_t previous(nodem::Baton*);
ydb_status_t previous_node(nodem::Baton*);
ydb_status_t procedure(nodem::Baton*);
ydb_status_t set(nodem::Baton*);
ydb_status_t unlock(nodem::Baton*);

} // @end ydb namespace

#endif // @end YDB_H

#endif // @end YDB_SIMPLE_API
