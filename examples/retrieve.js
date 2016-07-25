/*
 * retrieve.js - Test the Nodem APIs
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

var node = {global: 'zewd', subscripts: ['config']};

var retrieve = function (node) {
  "use strict";

  var global = node.global,
      subscripts = JSON.stringify(node.subscripts),
      obj,
      retrieveData;

  retrieveData = function (node) {
    console.log('in retrieveData: node = ' + JSON.stringify(node));

    var subs = '',
        data,
        value,
        newNode,
        subObj,
        obj = {};

    node.subscripts.push(subs);

    do {
      console.log('calling order: node = ' + JSON.stringify(node));

      subs = db.order(node).result;

      console.log('subs = ' + subs);

      if (subs !== '') {
        node.subscripts.pop();
        node.subscripts.push(subs);

        data = db.data(node).defined;

        if (data === 10) {
          newNode = {global: node.global, subscripts: node.subscripts};
          subObj = retrieveData(newNode);

          obj[subs] = subObj;

          node.subscripts.pop();
        } else if (data === 1) {
          value = db.get(node).data;

          obj[subs] = value;
        }
      }
    } while (subs !== '');

    return obj;
  };

  obj = retrieveData(node);

  return {
    node: {
      global: global,
      subscripts: JSON.parse(subscripts)
    },

    object: obj
  };
};

var obj = retrieve(node);

console.log('results = ' + JSON.stringify(obj));

db.close();
