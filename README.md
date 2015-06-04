# NodeM #

## A Node.js binding and driver for the GT.M language and database ##

Version 0.6.0 - 2015 Jun 4

## Copyright and License ##

Addon Module written and maintained by David Wicksell <dlw@linux.com>  
Copyright © 2012-2015 Fourth Watch Software LC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License (AGPL)
as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.

***

## Disclaimer ##

Nodem is still in development and its interface may change in future
versions. Use in production at your own risk.

## Summary and Info ##

Nodem is an open source addon module for Node.js that integrates Node.js
with the [GT.M][] database, providing in-process access to GT.M's database
from Javascript, via GT.M's C call-in interface. From Node.js you can
perform the basic primitive global database handling operations and also
invoke GT.M/Mumps functions. Although designed exclusively for use with
GT.M, Nodem aims to be API-compatible with the in-process Node.js interface
for [Globals][] and [Caché][], at least for the APIs that have been
implemented in Nodem. As such, please refer to the Caché Node.js
[API documentation][Docs] for further details on how to use those APIs.

Currently only the synchronous and extrinsic versions of the Nodem APIs
are working. The open() call works slightly differently than the
Caché/Globals version. It does not require any arguments, and it will
default to using the database specified in the environment variable,
$gtmgbldir. If you have more than one database and would like to connect
to a different one than $gtmgbldir points to, you can define an object,
with a property called namespace, defined as the path to your global
directory file for that database. E.g.

    > db.open({namespace: '/home/dlw/g/db_utf.gld'});

Nodem supports a feature called auto-relink, which will automatically relink
a routine image containing any function called by the function API. By
default auto-relink is off. You can enable it in one of three ways. First,
you can pass it as a property in the object parameter, passed to the
function API directly, with a value of true, or any non-zero number. This
will turn on auto-relink just for that call. You can also disable it, by
setting autoRelink to false, or 0, if it was already enabled by one of the
global settings. E.g.

    > db.function({function: 'version^v4wNode', autoRelink: true});

Second, you can enable it globally, for every call to the function API, by
setting the same configuration property in an object passed to the open API.
E.g.

    > db.open({autoRelink: true});

Third, you can enable it globally, by setting the environment variable
NODEM_AUTO_RELINK to 1, or any other non-zero number. E.g.

    $ export NODEM_AUTO_RELINK=1
    $ node test/testSet.js 
    or
    $ NODEM_AUTO_RELINK=1 node test/testSet.js

## Installation ##

There are a few things to be aware of in order to use the Nodem addon.
You will need to have GT.M installed and configured correctly, including
setting up your environment with the required GT.M environment variables.
You will also need to have Node.js installed and working.

**ATTENTION:** These instructions assume that the nodem repository has been
installed in your home directory. The paths will likely be different if you
have installed this with npm.

**NOTE:** If you have installed Nodem using npm, it will attempt to build
mumps.node during installation. If there is a file in the nodem/ directory
called builderror.log, and if that file is empty, mumps.node built without
issue; you can then skip the following step of copying a pre-built shared
library to ~/nodem/lib/mumps.node.

You might need to copy the correct version of mumps.node for your system
architecture and version of Node.js to ~/nodem/lib/mumps.node. The
mumps.node pre-built modules, which you will find in ~/nodem/lib/, are named
for the version of Node.js that they support, mumps12 for Node.js version
0.12.x, mumps10 for Node.js version 0.10.x, and mumps8 for Node.js version
0.8.x. Each module will also end in _x8664 for 64-bit, or _i686 for 32-bit
systems. By default there is a mumps.node already there, which is a copy of
the 64-bit version for Node.js version 0.12.x. It is important to realize
that the addon will not function unless it is called mumps.node, and a
symbolic link won't work. E.g.

    $ cd ~/nodem/lib/
    $ cp mumps12.node_x8664 mumps.node
    $ cd -

**NOTE:** The build file specifies several runtime linker paths, which are
also embedded in the pre-built modules. One of them might match the path
that GT.M is installed at on your system. These include the paths that are
used with the Ubuntu fis-gtm package, /usr/lib/fis-gtm/V6.0-003_x86_64/, and
the dEWDrop virtual machine image, ${HOME}/lib/gtm/. If one of the paths
match on your system, you can skip the next step of copying libgtmshr.so or
setting $LD_LIBRARY_PATH.

You might also have to move a copy of libgtmshr.so (GT.M shared runtime
library) into a directory that will be searched by the dynamic linker/loader
when mumps.node is loaded at runtime, if it isn't already located in one.
You will find libgtmshr.so bundled with GT.M wherever you have installed it.
There are a couple of things you can do at this point. You can move
libgtmshr.so to a standard directory that is searched by the loader, such as
lib/, or /usr/lib/, or on some systems, /usr/local/lib/. Then you will have
to run ldconfig as root to rebuild the linker's cache. You could also create
a symbolic link to it if you choose. E.g.

    $ sudo -i
    # cd /usr/local/lib/
    # ln -s /opt/lsb-gtm/6.0-003_x8664/libgtmshr.so
    # ldconfig
    # exit

You may have to add the /usr/local/lib/ directory to /etc/ld.so.conf or
create an /etc/ld.so.conf.d/libc.conf file and add it there, and then run
ldconfig as root. If you go this route, you should consider giving the
library a real linker name and soname link, based on its version. Instead,
you can avoid having to copy or link to the library by setting the
environment variable, $LD_LIBRARY_PATH, to point to it, as the loader will
search there first. It is usually advisable not to export $LD_LIBRARY_PATH
into your environment, so you might want to define it right before calling
node. E.g.

    $ LD_LIBRARY_PATH=${gtm_dist} node
    or
    $ LD_LIBRARY_PATH=${gtm_dist} node test/testSet.js

In addition you will need to set a few environment variables in order for
GT.M to find the call-in table and the MUMPS routine that it maps to. The
Nodem package supplies a sample environment file, called environ. It has a
commented out command to set $LD_LIBRARY_PATH to $gtm_dist, which you will
need to uncomment if you need it. It is located in ~/nodem/resources/ and
can simply be sourced into your working environment, either directly, or
from your own environment scripts or profile/login script. E.g.

    $ cd ~/nodem/resources/
    $ source environ
    or
    $ echo "source ~/nodem/resources/environ" >> ~/.profile

If you did not install Nodem in your home directory, you will need to fix
the paths in the environ file. If you don't source the environ file, then
you will need to put a copy of v4wNode.m into a directory that is specified
in your $gtmroutines routine path, so that GT.M can find it. It is located
in the ~/nodem/src/ directory. Again, if you don't source the environ file,
then you will need to define the $GTMCI environment variable, and point it
at the file nodem.ci, located in the ~/nodem/resources/ directory. E.g.

    $ export GTMCI=~/nodem/resources/nodem.ci

You can clone the repository with this command..

    $ git clone https://github.com/dlwicksell/nodem.git

You can also install it via npm with this command..

    $ npm install nodem

You can update to the latest version with this command..

    $ npm update nodem

## Building from source ##

Normally you do not need to build Nodem from source. You can either use one
of the pre-built shared library objects, or if you installed Nodem via npm,
it will build the library for you automatically. But if you happen to be
running a version of GT.M that is older than V5.5-000, none of those options
will work. This is because Nodem is using a GT.M API that didn't exist in
older versions. In that case, you will have to build from source. First,
open up the file binding.gyp (the build specification file) and edit line 36
, 'GTM_VERSION=60', to the version of GT.M that you are running. You can find
out what version of GT.M you are running by invoking GT.M in direct mode.
E.g.

    $ mumps -direct
    GTM>write $zversion
    GT.M V6.0-003 Linux x86_64

Then take the first two numbers in the version string, in this example, 6.0,
and drop the decimal point and that is what you would set GTM_VERSION to in
binding.gyp. Next, if you have installed Nodem via npm, while in the root of
the repository, run this command:

    $ npm run install

Or, if you have not installed Nodem via npm, while in the root of the
repository, run this command instead:

    $ node-gyp rebuild 2> builderror.log

If there were any errors, they will show up in builderror.log.

That's it, now Nodem should work with your older version of GT.M.

I hope you enjoy the Nodem package. If you have any questions, feature
requests, or bugs to report, please contact David Wicksell <dlw@linux.com>

### See Also ###

* The [GT.M][] implementation of MUMPS.

[GT.M]: http://sourceforge.net/projects/fis-gtm/
[Globals]: http://globalsdb.org/
[Caché]: http://www.intersystems.com/cache/
[Docs]: http://docs.intersystems.com/documentation/cache/20151/pdfs/BXJS.pdf

### APIs ###

* *about* - Display version information
* *version* - Display version information
* *close* - Close the database
* *data* - Call the $DATA intrinsic function
* *function* - Call an extrinisic function
* *get* - Call the $GET intrinsic function
* *global_directory* - List the names of the globals in the database
* *increment* - Call the $INCREMENT intrinsic function
* *kill* - Delete a global node, and all of its children
* *lock* - Lock a global or global node incrementally
* *merge* - Merge a global or a global node, to a global or a global node
* *next* - Call the $ORDER intrinsic function
* *order* - Call the $ORDER intrinsic function
* *next_node* - Call the $QUERY intrinsic function
* *open* - Open the database
* *previous* - Call the $ORDER intrinsic function in reverse
* *previous_node* - Not yet implemented
* *retrieve* - Not yet implemented
* *set* - Set a global node to a new value
* *unlock* - Unlock a global or global node incrementally, or release all locks
* *update* - Not yet implemented
