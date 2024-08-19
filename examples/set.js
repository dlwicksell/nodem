/*
 * Package:    NodeM
 * File:       set.js
 * Summary:    Test the set API
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
 *
 *
 * Store nodes of data in the database (^v4wTest) or symbol table (test), while
 * providing performance timing information.
 *
 * Pass, in any order, the string 'global' or 'local' to test storing in to the
 * database, or local symbol table.
 *
 * Pass, in any order, the number of nodes you want to store data in to.
 */

'use strict';

process.on('uncaughtException', function(error) {
    console.trace('Uncaught Exception:\n', error);
    nodem.close();
    process.exit(1);
});

var type = 'global';
var name = '^v4wTest';

if (process.argv[2] === 'global' || process.argv[2] === 'local') {
    type = process.argv[2];
} else if (process.argv[3] === 'global' || process.argv[3] === 'local') {
    type = process.argv[3];
}

if (type === 'local') name = 'test';

var nodes = 100000;

if (!isNaN(parseInt(process.argv[2]))) {
    nodes = process.argv[2];
} else if (!isNaN(parseInt(process.argv[3]))) {
    nodes = process.argv[3];
}

var nodem = require('../lib/nodem.js').Gtm();
nodem.open();

if (type === 'global') {
    if (name.slice(0, 1) !== '^') name = '^' + name;

    try {
        if (nodem.data(name, 'testing') !== 0) {
            console.error(name + '("testing") already contains data, aborting...');
            nodem.close();
            process.exit(1);
        }
    } catch (error) {
        console.log(error);
        nodem.close();
        process.exit(1);
    }
}

console.log(nodem.version() + '\n');
console.log('Testing the set command, starting at: ' + Date());
var start = process.hrtime(), i;

for (i = 0; i < nodes; i++) {
    try {
        nodem.set(name, 'testing', i, 'record ' + i);
    } catch (error) {
        console.log(error);
        nodem.close();
        process.exit(1);
    }
}

var end = process.hrtime(start);

console.log('Set ' + nodes + ' nodes in ' + name + '("testing"), ending at: ' + Date());
console.log('You set approximately', Math.round(nodes / (end[0] + end[1] / 1e+9)), 'nodes per second');

if (type === 'global') {
    console.log('Killing ' + name + '("testing") now...');
    nodem.kill(name, 'testing');
}

nodem.close();
process.exit(0);
