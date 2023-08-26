/*
 * Package:    NodeM
 * File:       ydb.h
 * Summary:    Functions that wrap calls to the SimpleAPI interface
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2018-2023 Fourth Watch Software LC
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

#if NODEM_SIMPLE_API == 1
#   ifndef YDB_H
#       define YDB_H

#include "nodem.h"

namespace ydb {

ydb_status_t data(nodem::NodemBaton*);
ydb_status_t get(nodem::NodemBaton*);
ydb_status_t set(nodem::NodemBaton*);
ydb_status_t kill(nodem::NodemBaton*);
ydb_status_t order(nodem::NodemBaton*);
ydb_status_t previous(nodem::NodemBaton*);
ydb_status_t next_node(nodem::NodemBaton*);
ydb_status_t previous_node(nodem::NodemBaton*);
ydb_status_t increment(nodem::NodemBaton*);
ydb_status_t lock(nodem::NodemBaton*);
ydb_status_t unlock(nodem::NodemBaton*);

} // @end ydb namespace

#   endif // @end YDB_H
#endif // @end NODEM_SIMPLE_API
