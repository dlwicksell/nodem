/*
 * set.js - Test the set API
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
 */


var gtm = require('../lib/nodem');
var db = new gtm.Gtm();
db.open();

var node;
var ret;
var i;

console.log('Testing the set command, starting at: ' + Date());

for (i = 0; i < 1000000; i++) {
  node = {global: 'dlw', subscripts: ['testing', i], data: 'record ' + i};

  ret = db.set(node);

  if (ret.ok === 0) {
    break;
  }
}

db.close();

if (ret.ok === 1) {
  console.log('Set a million nodes in ^dlw("testing"), ending at: ' + Date());
} else {
  console.log('There was an error: ' + ret.errorCode + ' ' + ret.errorMessage);
}
