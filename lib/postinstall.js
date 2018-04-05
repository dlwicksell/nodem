/*
 * postinstall.js - Compile v4wNode and log compile errors and warnings
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

var exec = require('child_process').exec;
var gtmDist = process.env.gtm_dist;

if (gtmDist === undefined) {
    console.error(__filename + ': Error: Set the $gtm_dist environment variable to the YottaDB/GT.M root directory\n');
    process.exit(1);
}

var gtmRoutines = process.env.gtmroutines;

if (gtmRoutines === undefined) {
    console.error(__filename + ': Error: Set the $gtmroutines environment variable to include v4wNode.m\n');

    process.exit(1);
}

var gtm = "echo 'zlink \"v4wNode.m\"' | " + gtmDist + "/mumps -direct";

exec(gtm, {shell: '/bin/bash'}, function (error, stdout, stderr) {
    'use strict';

    if (error) {
        console.error(__filename + ':', error.stack);
        process.exit(0);
    } else if (stderr) {
        if (stderr.match(/%GTM-E-ZLINKFILE/g)) {
            console.error(__filename + ': Error:\n\n' + stderr);
            process.exit(0);
        } else {
            console.error(__filename + ': Warning:\n\n' + stderr);
            process.exit(0);
        }
    }

    process.exit(0);
});
