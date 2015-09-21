/*
 * zwrite.js - A ZWRITE clone
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2015 Fourth Watch Software LC
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
 * Pass the name of your global as the first argument and it will dump
 * the entire contents of the global. If you add a - to the beginning of
 * the global name, it will create a temporary GT.M routine and call into
 * the routine (which will also dump the entire contents of the global),
 * and then clean up after itself. Calling it this way will execute much
 * faster than the default JavaScript implementation.
 */


if (process.argv[2] === undefined) {
  console.error('You must pass the name of the global you want to dump.');

  process.exit(1);
}

var mumps = require('../lib/nodem');
var db = new mumps.Gtm();

db.open();

process.on('uncaughtException', function(err) {
  db.close();

  console.trace('Uncaught Exception:\n', err);

  process.exit(1);
});

var global = process.argv[2];

if (global[0] === '-') {
  var fs = require('fs'),
      ret,
      fd,
      code = 'zwr(glvn) s:$e(glvn)\'="^" glvn="^"_glvn zwr @glvn q ""';

  fd = fs.openSync('v4wTest.m', 'w');
  fs.writeSync(fd, code);

  process.env.gtmroutines = process.env.gtmroutines + ' .';

  db.function({function: 'zwr^v4wTest', arguments: [global.slice(1)]});

  fs.closeSync(fd);
  fs.unlinkSync('v4wTest.m');
  fs.unlinkSync('v4wTest.o');
} else {
  var node = {};

  while (true) {
    node = db.next_node({global: global, subscripts: node.subscripts});

    if (node.subscripts === undefined) break;

    console.log('^' + global + '(' +
                JSON.stringify(node.subscripts)
                .replace('[', '')
                .replace(']', '') +
                ')=' + JSON.stringify(node.data));
  }
}

db.close();
