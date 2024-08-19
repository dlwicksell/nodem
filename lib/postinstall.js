/*
 * Package:    NodeM
 * File:       postinstall.js
 * Summary:    Compile v4wNode and log compile errors and warnings
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

'use strict';

var dist = process.env.ydb_dist ? process.env.ydb_dist : process.env.gtm_dist;

if (dist === undefined) {
    console.error(__filename + ': Error:\n\nNeed to set $ydb_dist (YottaDB) or $gtm_dist (GT.M) before compiling v4wNode.m\n');
    process.exit(1);
}

var dir = require('path').dirname(__dirname);
var routines = process.env.ydb_routines ? process.env.ydb_routines : process.env.gtmroutines;
var cwd = process.cwd();
var gtm = "echo 'zlink \"v4wNode.m\"' | " + dist + "/mumps -direct_mode";

if (routines === undefined) {
    process.env.ydb_routines = dir + '/src';
    process.env.gtmroutines = process.env.ydb_routines;
    process.chdir(dir + '/src');

    gtm = dist + '/mumps -nowarning ' + dir + '/src/v4wNode.m';
}

var exec = require('child_process').exec;

exec(gtm, {shell: '/bin/bash'}, function (error, stdout, stderr) {
    if (error) {
        console.error(__filename + ': ' + error.stack);
    } else if (stderr) {
        if (stderr.match(/%YDB-E-ZLINKFILE/g) || stderr.match(/%GTM-E-ZLINKFILE/g)) {
            console.error(__filename + ': Warning:\n\n' + stderr);

            var dir = require('path').dirname(__dirname);
            var home = process.env.HOME;

            console.info("Add 'source " + dir + "/resources/environ' to " + home + '/.profile or ' + home + '/.bash_profile\nor');
            console.info("Copy " + dir + '/src/v4wNode.m to a path located in $ydb_routines (YottaDB) or $gtmroutines (GT.M)\n');
        } else {
            console.error(__filename + ': Warning:\n\n' + stderr);
        }
    }

    if (routines === undefined) process.chdir(cwd);
    process.exit(0);
});
