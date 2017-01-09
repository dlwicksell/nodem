# NodeM #

## A Node.js binding and driver for the GT.M language and database ##

Version 0.9.0 - 2017 Jan 8

## Copyright and License ##

Addon Module written and maintained by David Wicksell <dlw@linux.com>  
Copyright © 2012-2017 Fourth Watch Software LC

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU Affero General Public License (AGPL) as published by the
Free Software Foundation, either version 3 of the License, or (at your option)
any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.

Contact me if you are interested in using Nodem with a different license.
***

## Disclaimer ##

Nodem is still in development and its interface may change in future versions.
Use in production at your own risk.

## Summary and Info ##

Nodem is an open source addon module for Node.js that integrates Node.js with
the [GT.M][] database, providing in-process access to GT.M's database from
Javascript, via GT.M's C call-in interface. From Node.js you can perform the
basic primitive global database handling operations and also invoke GT.M/MUMPS
functions. Although designed exclusively for use with GT.M, Nodem aims to be
API-compatible with the in-process Node.js interface for [Globals][] and
[Caché][], at least for the APIs that have been implemented in Nodem. As such,
please refer to the Caché Node.js [API documentation][Docs] for further details
on how to use those APIs.

Currently only the synchronous versions of the Nodem APIs are fully
implemented, and their arguments have to be passed in Javascript objects.
However, the asynchronous versions of the APIs, and the ability to also pass
arguments by position, is coming soon.

The open() call works a bit differently than the Caché/Globals version; it does
not require any arguments, and it will default to using the database specified
in the environment variable, $gtmgbldir. If you have more than one database and
would like to connect to a different one than $gtmgbldir points to, you can
define an object, with a property called namespace, defined as the path to your
global directory file for that database, e.g.

    > gtm.open({namespace: '/home/dlw/g/db_utf.gld'});

Also, if you have more than one version of GT.M installed and would like to
connect to a different one than $gtm_dist points to, you can define an object,
with a property called path, passed to the open() call, defined as the path to
your GT.M installation root that you would like to connect to, e.g.

    > gtm.open({path: '/usr/lib/x86_64-linux-gnu/fis-gtm/V6.3-000_x86_64/'});

Nodem supports setting a [GT.CM][] client to allow for remote connections
across a network interface, using GT.M's built-in GT.CM functionality. In the
open() method, you can set an ip_address and/or a tcp_port property, and Nodem
will set up the environment to connect with a GT.M database on a remote server
that already has a GT.CM server running on that ip_address and tcp_port. If
only ip_address or tcp_port is defined, the other one will be set with a
default value; 127.0.0.1 for ip_address, or 6879 for tcp_port. Nodem will then
set the $GTCM_NODEM environment variable, for that Nodem process only, with the
ip_address and tcp_port you set in the open() call, e.g.

    > gtm.open({ip_address: '127.0.0.1', tcp_port: 6879});

You will also need to create a global directory file that maps one or more
database segments to a data file on the remote server you want to connect with,
noting that the prefix to the -file argument in the example below must be
NODEM, in order to match the $GTCM_NODEM environment variable name that Nodem
sets up for you, e.g.

    $ mumps -r GDE
    GDE> change -segment DEFAULT -file=NODEM:/home/dlw/g/gtcm-server.dat

Then on the server you are connecting to, make sure you have the data file set
up at the same path that you set the '-file=' option to in the global directory
of your GT.CM client configuration, and have started the GT.CM server on the
same ip_address and tcp_port that you configured in the open() call in Nodem,
e.g.

    $ $gtm_dist/gtcm_gnp_server -log=gtcm.log -service=127.0.0.1:6879

Nodem allows you to set the data mode you want to use. Currently mode can be
set to either 'canonical' or 'strict'. The default is canonical, and
interprets data using MUMPS' canonical representation. I.e. Numbers will be
represented numerically, etc. Strict mode interprets all data as strings,
strictly following the convention set with InterSystems' Node.js driver, e.g.

    > gtm.open({mode: 'strict'});

Nodem supports a feature called auto-relink, which will automatically relink a
routine object containing any function (or procedure) called by the function
(or procedure) API. By default auto-relink is off. You can enable it in one of
three ways. First, you can pass it as a property of the Javascript object
argument which is passed to the function (or procedure) API directly, with a
value of true, or any non-zero number. This will turn on auto-relink just for
that call. You can also disable it, by setting autoRelink to false, or 0, if it
was already enabled by one of the global settings, e.g.

    > gtm.function({function: 'version^v4wNode', autoRelink: true});

Second, you can enable it globally, for every call to the function (or
procedure) API, by setting the same property in a Javascript object passed to
the open API, e.g.

    > gtm.open({autoRelink: true});

Third, you can also enable it globally, by setting the environment variable
NODEM_AUTO_RELINK to 1, or any other non-zero number, e.g.

    $ export NODEM_AUTO_RELINK=1
    $ node examples/set.js
    or
    $ NODEM_AUTO_RELINK=1 node examples/set.js

GT.M changes some settings of its controlling terminal device, and Nodem has to
reset them when it closes the database connection. By default, Nodem will
restore the terminal device to the state it was in when the open() call was
invoked. Normally this is the desired option, however, if you wish to reset the
terminal to typically sane settings, the close() call allows this by setting
the resetTerminal property to true, or any non-zero number, e.g.

    > gtm.close({resetTerminal: true});

Nodem has a new procedure API, which is similar to the function API, except
that it is used to call M procedures or subroutines, which do not return any
values. The procedure API accepts one argument, a Javascript object. The object
must contain the required procedure property, set to the name of the procedure
or subroutine. It may also contain an optional property, called arguments,
which is an array of arguments to pass to the procedure, e.g.

    > gtm.procedure({procedure: 'set^node', arguments: ['dlw', 5]});

Nodem has added support for timeouts in the lock API. You can pass a timeout,in
seconds, as the second argument, after the Javascript object. If you do not
pass a timeout argument, the lock API will wait to acquire a lock indefinitely.
If you wish to come back from the call right away, if the lock is not
available, simply pass a timeout argument of 0, e.g.

    > gtm.lock({global: 'dlw'}, 0);

## Installation ##

There are a few things to be aware of in order to use the Nodem addon. You will
need to have GT.M installed and configured correctly, including setting up your
environment with the required GT.M environment variables. You will also need to
have Node.js installed and working.

**ATTENTION:** These instructions assume that the nodem repository has been
installed in your home directory. The paths will likely be different if you
have installed this with npm.

**NOTE:** If you have installed Nodem using npm, it will attempt to build
mumps.node during installation. If there is a file in the nodem/ directory
called builderror.log, and if that file contains no build errors, mumps.node
built without issue. If you downloaded Nodem any other way, including cloning
it from its github repository, then you'll have to build it from source. While
in the root of the Nodem repository, you simply run the 'npm run install'
command, e.g.

    $ cd ~/nodem
    $ npm run install
    or
    $ npm install

Nodem should run on every version of Node.js starting with version 0.8.0, as
well as every version of IO.js. However, in the future, both Node.js and the V8
Javascript engine at its core, could change their APIs in a
non-backwards-compatible way, which might break Nodem for that version.

**NOTE:** The build file specifies several runtime linker paths, which are also
embedded in the pre-built modules. One of them might match the path that GT.M
is installed at on your system. These include the paths that are used with the
latest fis-gtm package for Ubuntu 16.04 (64-bit), Ubuntu 14.04 (32-bit), and
the dEWDrop virtual machine image infrastructure link, as well as the path that
is defined in the $gtm_dist environment variable, if it is defined when Nodem
is built. If one of the paths match on your system, you can skip the next step
of copying libgtmshr.so or setting $LD_LIBRARY_PATH.

You might also have to move a copy of libgtmshr.so (GT.M shared runtime
library) into a directory that will be searched by the dynamic linker/loader
when mumps.node is loaded at runtime, if it isn't already located in one. You
will find libgtmshr.so bundled with GT.M wherever you have installed it. There
are a couple of things you can do at this point. You can move libgtmshr.so to a
standard directory that is searched by the loader, such as lib/, or /usr/lib/,
or on some systems, /usr/local/lib/. Then you will have to run ldconfig as root
to rebuild the linker's cache. You could also create a symbolic link to it if
you choose, e.g.

    $ sudo -i
    # cd /usr/local/lib/
    # ln -s /opt/lsb-fis/gtm/V6.3-000_x86_64/libgtmshr.so
    # ldconfig
    # exit

You may have to add the /usr/local/lib/ directory to /etc/ld.so.conf or create
an /etc/ld.so.conf.d/libc.conf file and add it there, and then run ldconfig as
root. If you go this route, you should consider giving the library a real
linker name and soname link, based on its version. Instead, you can avoid
having to copy or link to the library by setting the environment variable,
$LD_LIBRARY_PATH, to point to it, as the loader will search there first. It is
usually advisable not to export $LD_LIBRARY_PATH into your environment, so you
might want to define it right before calling node, e.g.

    $ LD_LIBRARY_PATH=${gtm_dist} node
    or
    $ LD_LIBRARY_PATH=${gtm_dist} node examples/set.js

In addition you will need to set a few environment variables in order for GT.M
to find the call-in table and the MUMPS routine that it maps to. The Nodem
package supplies a sample environment file, called environ. It has a commented
out command to set $LD_LIBRARY_PATH to $gtm_dist, which you will need to
uncomment if you need it. It is located in ~/nodem/resources/ and can simply be
sourced into your working environment, either directly, or from your own
environment scripts or profile/login script, e.g.

    $ cd ~/nodem/resources/
    $ source environ
    or
    $ echo "source ~/nodem/resources/environ" >> ~/.profile

The environ file defaults to using the paths that exist if you installed Nodem
via npm in your home directory. If you did not install Nodem with npm in your
home directory, you will probably need to fix the paths in the environ file. If
you don't source the environ file, then you will need to put a copy of
v4wNode.m into a directory that is specified in your $gtmroutines routine path,
so that GT.M can find it. It is located in the ~/nodem/src/ directory. Again,
if you don't source the environ file, then you will need to define the $GTMCI
environment variable, and point it at the file nodem.ci, located in the
~/nodem/resources/ directory, e.g.

    $ export GTMCI=~/nodem/resources/nodem.ci

You can clone the repository with this command..

    $ git clone https://github.com/dlwicksell/nodem.git

You can also install it via npm with this command..

    $ npm install nodem

You can update to the latest version with this command..

    $ npm update nodem

## Building from source ##

If you have installed Nodem via npm, it will usually build the shared library
for you automatically, and you can skip the step of building it manually from
source.

If you happen to be running a version of GT.M that is older than V5.5-000, you
might have to change a configuration option in the build file, before you build
from source. This is because Nodem is using a GT.M API that didn't exist in
older versions. This shouldn't be necessary, however, as the preinstall script
should take care of this automatically for you. If it is necessary, open up the
file binding.gyp (the build specification file), and edit the line that
contains the string 'GTM_VERSION', to the version of GT.M that you are running.
You can find out what version of GT.M you are running by invoking GT.M in
direct mode, e.g.

    $ mumps -direct
    GTM>write $zversion
    GT.M V6.3-000A Linux x86_64

Then take the first two numbers in the version string, in this example, 6.3,
and drop the decimal point and that is what you would set GTM_VERSION to in
binding.gyp. That would be 'GTM_VERSION=63' in this case. Next, if you have
installed Nodem via npm, while in the root of the repository, run this command:

    $ npm run install

Or, if you have not installed Nodem via npm, while in the root of the
repository, run this command instead:

    $ node-gyp rebuild 2> builderror.log

If there were any errors, they will show up in builderror.log. That's it, now
Nodem should work with your older version of GT.M.

If you have any questions, feature requests, or bugs to report, please contact
David Wicksell <dlw@linux.com>

### See Also ###

* The [GT.M][] implementation of MUMPS.

[GT.M]: http://sourceforge.net/projects/fis-gtm/
[Globals]: http://globalsdb.org/
[Caché]: http://www.intersystems.com/cache/
[Docs]: http://docs.intersystems.com/documentation/cache/20162/pdfs/BXJS.pdf
[GT.CM]: http://tinco.pair.com/bhaskar/gtm/doc/books/ao/UNIX_manual/webhelp/content/ch13.html

### APIs ###

* *about* or *version* - Display version information
* *close*              - Close the database connection
* *data*               - Call the $DATA intrinsic function
* *function*           - Call an extrinsic function
* *get*                - Call the $GET intrinsic function
* *global_directory*   - List the names of the globals in the database
* *increment*          - Call the $INCREMENT intrinsic function
* *kill*               - Delete a global node, and all of its children
* *lock*               - Lock a global or global node incrementally
* *merge*              - Merge a global or a global node, to a global or a global node
* *next* or *order*    - Call the $ORDER intrinsic function
* *next_node*          - Call the $QUERY intrinsic function
* *open*               - Open the database connection
* *previous*           - Call the $ORDER intrinsic function in reverse
* *previous_node*      - Not yet implemented
* *procedure*          - Call a procedure/subroutine
* *retrieve*           - Not yet implemented
* *set*                - Set a global node to a new value
* *unlock*             - Unlock a global or global node incrementally, or release all locks
* *update*             - Not yet implemented
