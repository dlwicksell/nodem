/*
 * preinstall.js - Configure binding.gyp build script, if necessary
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2016,2018 Fourth Watch Software LC
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

var exec = require('child_process').exec;
var gtmDist = process.env.ydb_dist ? process.env.ydb_dist : process.env.gtm_dist;

if (gtmDist === undefined) {
    console.error(__filename + ': Error: Set the $gtm_dist environment variable to the YottaDB/GT.M root directory\n');
    process.exit(1);
}

var gtm = "echo 'write $zversion' | " + gtmDist + "/mumps -direct | grep 'GT.M' | cut -dV -f2 | cut -d- -f1 | tr -d '.\n'";

exec(gtm, function (error, stdout, stderr) {
    'use strict';

    if (error) {
        console.error(__filename + ':', error.stack);
        process.exit(0);
    } else if (stderr) {
        console.error(__filename + ': Warning:\n\n' + stderr);
        process.exit(0);
    } else {
        if (stdout < 55) {
            var sed = 'sed -i "s/\'GTM_VERSION=.*\'/\'GTM_VERSION=' + stdout + '\'/g" ./binding.gyp';

            exec(sed, function (error, stdout, stderr) {
                if (error) {
                    console.error(__filename + ':', error.stack);
                    process.exit(0);
                } else if (stderr) {
                    console.error(__filename + ': Warning:\n\n' + stderr);
                    process.exit(0);
                }
            });
        }
    }

    process.exit(0);
});
