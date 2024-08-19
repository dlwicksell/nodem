/*
 * Package:    NodeM
 * File:       nodem.js
 * Summary:    Nodem start file
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2024 Fourth Watch Software LC
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

var dir = require('path').dirname(__dirname);
var callinTable = process.env.ydb_ci ? process.env.ydb_ci : process.env.GTMCI;

if (callinTable === undefined) {
    process.env.ydb_ci = dir + '/resources/nodem.ci';
    process.env.GTMCI = process.env.ydb_ci;
}

var routines = process.env.ydb_routines ? process.env.ydb_routines : process.env.gtmroutines;

if (routines === undefined) {
    process.env.ydb_routines = dir + '/src';
    process.env.gtmroutines = process.env.ydb_routines;
}

try {
    module.exports = require('../build/Release/nodem.node');
} catch (error) {
    try {
        module.exports = require('../build/Debug/nodem.node');
    } catch {
        console.error(error.stack + '\n');
        console.info("Try rebuilding Nodem with 'npm run install' in the", dir, 'directory\n');
    }
}
