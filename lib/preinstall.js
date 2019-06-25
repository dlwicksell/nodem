/*
 * preinstall.js - Check for the existence of $ydb_dist or $gtm_dist
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2019 Fourth Watch Software LC
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

var gtmDist = process.env.ydb_dist ? process.env.ydb_dist : process.env.gtm_dist;

if (gtmDist === undefined) {
    console.error(__filename + ': Error: Need to set $ydb_dist (YottaDB) or $gtm_dist (GT.M) before installing Nodem\n');
    process.exit(1);
}

process.exit(0);
