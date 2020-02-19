/*
 * zwrite.js - A zwrite clone
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2016,2018,2020 Fourth Watch Software LC
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
 *
 * Dumps the contents of a full global in a similar format to the zwrite command
 * in YottaDB/GT.M.
 *
 * Pass the name of the global you want to dump, or don't pass any argument and
 * it will use a default of ^v4wTest. You can optionally supply the following
 * arguments, in any order, before or after the global name.
 *
 * -f [fast mode] - <on> - Creates a temporary GT.M routine and calls into the
 *    routine (which will also dump the entire contents of the global), and then
 *    cleans up after itself. Calling it this way will execute much faster than
 *    the default JavaScript implementation.
 *
 * -m [data mode] - <canonical>|strict - The operating mode, which controls the way
 *    data is formatted, and a few other aspects of the API. Strict mode follows
 *    the cache.node API as closely as possible, including treating all data as
 *    a string.
 *
 * -c [character set encoding] - <utf-8>|m - The character encoding of the data
 *
 * -d [debug mode] - [<false>|true]|[<off>|low|medium|high]|[<0>|1|2|3] - Turns on debug mode, which provides debug tracing data
 */

process.on('uncaughtException', function(err) {
    gtm.close();
    console.trace('Uncaught Exception:\n', err);
    process.exit(1);
});

var gtm = require('../lib/nodem').Gtm();

var charset = 'utf-8',
    command,
    debug = false,
    fast = false,
    global = 'v4wTest',
    mode = 'canonical',
    node = {};

process.argv.forEach(function(argument) {
    if (process.argv[0] === argument || process.argv[1] === argument) return;

    if (command === '-c') {
        charset = argument;
        command = '';
        return;
    } else if (command === '-d') {
        debug = argument;
        command = '';
        return;
    } else if (command === '-m') {
        mode = argument;
        command = '';
        return;
    }

    if (argument === '-f') {
        fast = true;
        return;
    } else if (argument === '-c') {
        command = '-c';
        return;
    } else if (argument === '-d') {
        command = '-d';
        return;
    } else if (argument === '-m') {
        command = '-m';
        return;
    } else {
        global = argument;
    }
});

gtm.open({mode: mode, charset: charset, debug: debug});

if (process.env.ydb_routines !== undefined) {
    process.env.ydb_routines = '. ' + process.env.ydb_routines;
} else {
    process.env.ydb_routines = '.';
}

if (process.env.gtmroutines !== undefined) {
    process.env.gtmroutines = '. ' + process.env.gtmroutines;
} else {
    process.env.gtmroutines = '.';
}

var version = gtm.version();

if (typeof version === 'object') {
    gtm.close();
    console.log(version.ErrorMessage || version.errorMessage);
    process.exit(1);
}

if (fast) {
    var fs = require('fs');
    var code = 'zwrite(glvn) set:$extract(glvn)\'="^" glvn="^"_glvn zwrite @glvn quit ""\n';
    var fd = fs.openSync('v4wTest.m', 'w');

    fs.writeSync(fd, code);
    var result = gtm.function({function: 'zwrite^v4wTest', arguments: [global]});

    fs.closeSync(fd);
    fs.unlinkSync('v4wTest.m');
    fs.unlinkSync('v4wTest.o');

    if (!result.ok) console.log(result);
} else {
    if (charset === 'm') process.stdout.setDefaultEncoding('binary');

    var pre = '';
    if (global[0] != '^') pre = '^';

    if (gtm.data({global: global}).defined % 2) {
        node = gtm.get({global: global});
        console.log(pre + global + '=' + JSON.stringify(node.data));
    }

    while (true) {
        node = gtm.next_node({global: global, subscripts: node.subscripts});

        if (!node.defined) break;

        console.log(pre + global + '(' + JSON.stringify(node.subscripts).slice(1, -1) + ')=' + JSON.stringify(node.data));
    }
}

gtm.close();
process.exit(0);
