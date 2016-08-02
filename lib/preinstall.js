/*
 * preinstall.js - Configure binding.gyp build script
 *
 * Written by David Wicksell <dlw@linux.com>
 *
 * Copyright © 2016 Fourth Watch Software LC
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

var gtmDist = process.env.gtm_dist + '/';
var cmd = "echo 'write $zversion' | " + gtmDist + "mumps -dir | grep 'GT.M' | cut -dV -f2 | cut -d- -f1 | tr -d '.\n'";

exec(cmd, function (error, stdout, stderr) {
    "use strict";

    var sed = 'sed -i "s/\'GTM_VERSION=.*\'/\'GTM_VERSION=' + stdout + '\'/g" ./binding.gyp';

    if (error) {
        console.error('./lib/preinstall.js:', error.stack);
    } else if (stderr) {
        console.error('./lib/preintstall.js: Error: ' + stderr);
    } else {
        exec(sed, function (error, stdout, stderr) {
            if (error) {
                console.error('./lib/preinstall.js:', error.stack);
            } else if (stderr) {
                console.error('./lib/preintstall.js: Error: ' + stderr);
            }
        });
    }
});
