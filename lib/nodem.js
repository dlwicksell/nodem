/*
 * nodem.js - Start file for Nodem
 *
 * Written by David Wicksell <dlw@linux.com>
 *
 * Copyright © 2012-2014 Fourth Watch Software, LC
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

try {
    module.exports = require('../build/Release/mumps');
} catch (e) {
    try {
        module.exports = require('./mumps');
    } catch (e) {
        console.error(e.stack + '\n');
        console.error("mumps.node seems to be missing. Try 'npm run install'.");
    }
}
