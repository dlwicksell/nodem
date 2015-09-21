/*
 * function.js - Test the function API
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

var func1 = db.function({function: 'version^%zewdAPI'});
console.log(JSON.stringify(func1) + '\n');

var func2 = db.function({function: 'version^v4wNode'});
console.log(JSON.stringify(func2));

db.close();
