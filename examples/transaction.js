/*
 * Package:    NodeM
 * File:       transaction.js
 * Summary:    Test the transaction API
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2021-2024 Fourth Watch Software LC
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
 * Test transaction processing in YottaDB, using a worker thread
 *
 * -c [commit mode] - Test transaction commits
 * -r [restart/rollback mode] - Test transaction restarts/rollbacks
 */

'use strict';

process.on('uncaughtException', (error) => {
    console.trace('Uncaught Exception:\n', error);
    nodem.close();
    process.exit(1);
});

try {
    var { Worker, isMainThread, parentPort } = require('worker_threads');
} catch (error) {
    if (error) console.error(__filename + ' must be run on Node.js version 11.7.0 or newer');
    process.exit(1);
}

const nodem = require('../lib/nodem.js').Gtm();

if (nodem.version().split(' ')[3].slice(0, -1) !== 'YottaDB') {
    console.error('Transaction processing is currently only supported on YottaDB');
    process.exit(1);
}

let line = 0;

if (isMainThread) {
    console.log('Nodem main thread begin');

    var value;

    if (process.argv[2] === '-c') {
        value = 0;
    } else if (process.argv[2] === '-r') {
        value = 1;
    } else {
        console.error('Usage:\ttransaction.js\t-c  # Test transaction commits\n\t\t\t-r  # Test transaction restarts/rollbacks');
        process.exit(1);
    }

    nodem.open();

    try {
        if (nodem.data('^v4wTest') !== 0) {
            console.error('^v4wTest already contains data, aborting...');
            nodem.close();
            process.exit(1);
        }
    } catch (error) {
        console.log(error);
        nodem.close();
        process.exit(1);
    }

    console.log('nodem.set:        ', nodem.set({global: 'v4wTest', data: value}), '\n');

    let worker = new Worker(__filename);

    worker.on('message', (message) => {
        console.log('\nNodem main thread after transaction has completed in worker thread');
        console.log('nodem.transaction:', message);
        console.log('nodem.get:        ', nodem.get({local: '$tlevel'}));
        console.log('nodem.get:        ', nodem.get({local: '$trestart'}));
        console.log('nodem.get:        ', nodem.get({global: 'v4wTest'}));
        console.log('Nodem main thread end');

        nodem.kill('^v4wTest');
        nodem.close();
        process.exit(0);
    });

    let count = 0;

    setInterval(() => {
        count++;
        console.log('Nodem main thread doing work', count, 'seconds');
    }, 1000);
} else {
    const fs = require('fs');

    console.log('Nodem worker thread begin');
    console.log('Nodem worker thread before 5 second blocking operation\n');

    setTimeout(() => {
        console.log('\nNodem worker thread after 5 second blocking operation\n');

        const outerTpResult = nodem.transaction(() => {
            console.log('Entering outer transaction..\n');
            console.log('nodem.get:        ', nodem.get({local: '$tlevel'}));
            console.log('nodem.get:        ', nodem.get({local: '$trestart'}));
            console.log('nodem.increment:  ', nodem.increment({global: 'v4wTest'}));

            const outerResult = nodem.get({global: 'v4wTest'});
            console.log('nodem.get:        ', outerResult);

            const innerTpResult = nodem.transaction(() => {
                console.log('\nEntering inner transaction..\n');

                fs.readFile('./CHANGELOG.md', {encoding: 'utf8'}, (error, content) => {
                    if (error) return console.error(error);
                    console.log('Asynchronous: CHANGELOG.md line ' + (line + 1) + ':\n' + content.split('\n')[line] + '\n');
                    line = line + 2;
                });

                console.log('nodem.get:        ', nodem.get({local: '$tlevel'}));
                console.log('nodem.get:        ', nodem.get({local: '$trestart'}));
                console.log('nodem.increment:  ', nodem.increment({global: 'v4wTest'}));

                const innerResult = nodem.get({global: 'v4wTest'});
                console.log('nodem.get:        ', innerResult);

                console.log('\nExiting inner transaction..\n');
                if (innerResult.data > 2) return 'Restart';
                return 'Commit';
            }, {variables: ['*']});

            console.log('nodem.transaction:', innerTpResult);
            console.log('\nExiting outer transaction..\n');

            if (innerTpResult.errorCode === nodem.tpRestart) return 'Restart';
            if (!innerTpResult.ok) return 'Rollback';
            if (outerResult.data > 1) return 'Restart';
        }, {variables: ['*']});

        setTimeout(() => {
            console.log('\nNodem worker thread end');
            parentPort.postMessage(outerTpResult);
        }, 1000);
    }, 5000);
}
