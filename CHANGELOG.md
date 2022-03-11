# NodeM Changelog #

## v0.20.2 - 2022 Mar 10 ##

- Reformat various parts of the source code
- Rename several classes, instances, types, and variables
- Rename addon module shared library
- Rename multiple source files
- Fix bugs in environ script

## v0.20.1 - 2021 Sep 28 ##

- Fix potential segfault in version API

## v0.20.0 - 2021 Mar 10 ##

- Add transaction processing API
- Add transaction.js example program
- Make various improvements

## v0.19.0 - 2020 Aug 8 ##

- Add full asynchronous support to the merge API

## v0.18.1 - 2020 Jul 29 ##

- Update version documentation

## v0.18.0 - 2020 Jul 28 ##

- Add full asynchronous support to the version API
- Add polyfill implementation of previousNode in v4wNode.m for GT.M and YottaDB r1.00

## v0.17.3 - 2020 Jul 1 ##

- Improve debug tracing serialization

## v0.17.2 - 2020 Jun 24 ##

- Improve the performance of set.js

## v0.17.1 - 2020 May 29 ##

- Add support for auto-relink syntax in environ

## v0.17.0 - 2020 May 27 ##

- Add full asynchronous support to the lock and unlock APIs
- Add SimpleAPI support to the lock and unlock APIs
- Add support for calling arguments by-position to the lock and unlock APIs
- Add new `string` mode to the open and configure APIs

## v0.16.2 - 2020 Feb 21 ##

- Fix error/warning handling in postinstall.js

## v0.16.1 - 2020 Feb 20 ##

- Fix potential application crash [SIGSEGV] when calling the configure API with no arguments on older versions of Node.js
- Fix compiler error on systems with older GLIBC version
- Rework error handling in postinstall.js

## v0.16.0 - 2020 Feb 18 ##

- Add full synchronous and asynchronous support for the worker_threads API
- Add configure API, which can be called as often as wanted, to change database connection options
  - Add charset, mode, autoRelink, and debug properties to configure
- Make charset, mode, autoRelink, and debug properties in open and configure work per-thread
- Reimplement debug tracing
  - Add debug code in utility.h
  - Add worker thread support
  - Add asynchronous support
  - Change debug output to write to stderr
    - Make it work when running M code
  - Change structure for debug messages
    - Start with thread ID or MUMPS in square brackets
- Add threadpoolSize option to the open API
- Add tid property to the output for the open API
- Improve performance
- Move utility.h to nodem.h
- Restructure most of code base
- Add support for Node.js 12.x and 13.x
- Add version check in debug mode for v4wNode.m
- Add support for extended global references for the SimpleAPI

## v0.15.0 - 2019 Jul 9 ##

- Add full asynchronous support to the increment API
- Add SimpleAPI support to the increment API
- Add support for calling arguments by-position to the increment API

## v0.14.4 - 2019 Jul 2 ##

- Fix increment bug that prevented any increment from working
- Fix set bug with negative numbers using the SimpleAPI
- Improve debug tracing for SimpleAPI subscripts vector

## v0.14.3 - 2019 Jun 24 ##

- Add support for Node.js 12.x

## v0.14.2 - 2019 Feb 17 ##

- Fix support for YottaDB r1.24

## v0.14.1 - 2019 Feb 1 ##

- Add support for YottaDB r1.24

## v0.14.0 - 2019 Jan 17 ##

- Add full asynchronous support to the function and procedure APIs
- Add support for calling arguments by-position to the function and procedure APIs
- Add nodeOnly option to kill API to support zkill
- Improve usage of gtm_cip
- Fix possible race conditions with error handling
- Improve debug tracing code
- Improve error handling during subscript parsing
- Update built-in API help method

## v0.13.4 - 2019 Jan 6 ##

- Fix minor documentation mistakes
- Fix indirection limit bug in v4wNode.m routine
- Fix bug in environ script

## v0.13.3 - 2018 Dec 13 ##

- Fix V8 type bug

## v0.13.2 - 2018 Dec 13 ##

- Add missing release note to v0.13.0
- Remove incorrect release note from v0.13.1

## v0.13.1 - 2018 Dec 13 ##

- Fix several typos in README.md

## v0.13.0 - 2018 Dec 12 ##

- Add full asynchronous support to the data, previous, nextNode, and previousNode APIs
- Add SimpleAPI support to the data, previous, nextNode, and previousNode APIs
- Add support for calling arguments by-position to the data, previous, nextNode, and previousNode APIs
- Restructured the README.md documentation
- Remove simple-api script now that SimpleAPI support is more fully tested
- Update zwrite.js
  - Add support for full debug tracing to CLI
  - Improve error handling
  - Improve argument handling
- Update gtm.hh and gtm.cc
  - Add support for data, previous, nextNode, and previousNode to enable asynchronous versions
  - Improve efficiency of debug tracing code
- Update ydb.hh and ydb.cc
  - Add support for data, previous, nextNode, and previousNode to enable asynchronous versions
  - Ignore local variables that begin with the `v4w` namespace in order and previous
  - Improve efficiency of debug tracing code
- Update v4wNode.m
  - Ignore local variables that begin with the `v4w` namespace in order and previous
  - Improve previousNode handling for versions of YottaDB and GT.M that do not implement it
  - Add ok property to the JSON returned by previousNode, retrieve, and update
  - Simplify version API when connected to YottaDB
- Update mumps.cc
  - Add signalHandler property to open API
  - Improve and update help API data
  - Rename the catch_interrupt function as clean_shutdown
  - Rename the gtm_status function as error_status
  - Improve signal handling in clean_shutdown signal handler function
  - Improve efficiency of debug tracing code
  - Add is_number function to support `canonical` mode for YottaDB
  - Change internal API interfaces to pass the Baton struct around for better maintainability
  - Replace the asynchronous conditional jump blocks with function pointers in the Baton struct
  - Fix utf-8 bug in the encode_arguments function
  - Add build_subscripts function to more easily support the YottaDB API
  - Fix potential memory leaks

## v0.12.1 - 2018 Oct 10 ##

- Turn SimpleAPI support on by default
- Speed up performance of SimpleAPI support

## v0.12.1-pre - 2018 Sep 24 ##

- Pre-release version with SimpleAPI support on by default, for testing

## v0.12.0 - 2018 Sep 22 ##

- Add asynchronous support to the get, kill, order, and set APIs
- Add call-by-position support for the get, kill, order, and set APIs
- Add support for intrinsic special variables (ISVs) to the get and set APIs
- Add new experimental support for YottaDB's SimpleAPI (for get, kill, order, and set)
- Remove preinstall.js and move functionality to binding.gyp
- Add new implementation files for better architectural support of new SimpleAPI

## v0.11.2 - 2018 Apr 24 ##

- Fix CPP bug

## v0.11.1 - 2018 Apr 15 ##

- Merge Chris Edwards' enhancements
- Make minor changes, including more support for ydb_* environment variables
- Add v4wNode.o compiled object file to .gitignore

## v0.11.0 - 2018 Apr 10 ##

- Improve support for legacy Mumps APIs
- Add support for passing arguments by reference, and by local variable, to function and procedures
- Add documentation and examples about new functionality in README.md
- Fix typo and restructure documentation in README.md

## v0.10.1 - 2018 Apr 8 ##

- Make a few updates to v0.10.0 that were missed
- Increase combined length limit of arguments in the function and procedure methods from 8 KiB to nearly 1 MiB (added in v0.10.0)
- Allow passing more data types in the subscripts and arguments array, and convert them to strings, in mumps.cc
- Add information about the new help method from v0.10.0 to README.md
- Add an example of using Nodem to README.md
- Fix scoping bug in v4wNode.m

## v0.10.0 - 2018 Apr 5 ##

- Refactor Nodem for future maintainability
- Add support to APIs for local variables with the new `local` property and the new localDirectory method
- Change character set encoding to default to UTF-8 and decouple it from the encoding set for the underlying YottaDB/GT.M database
- Add support for the previousNode API in YottaDB versions r1.10 and newer
- Remove support for Node.js versions earlier than 0.12.0
- Add new help method, with a list of APIs, and more detailed call information for each API
- Add support to the open API for new configuration settings
  - Add `routinePath` configuration to change the routine look-up path when using the function and procedure methods
  - Add `callinPath` configuration to make it easier to support environments running more than just Nodem with the Call-in interface
  - Add `debug` configuration to turn on debug tracing
  - Add `charset` configuration to enable changing the character set encoding directly in the API
  - Remove `path` configuration, as it was unhelpful, and misleading
- Refactor mumps.hh for maintainability
  - Remove C Macro support, no longer necessary after removing support for Node.js versions prior to 0.12.0
  - Add support for distinguishing between YottaDB and GT.M distributions
  - Add name space Nodem
- Refactor mumps.cc to use a more modular structure for maintainability
  - Add various improvements for stability
  - Add function and method documentation, similar to JSDoc
  - Add several new utility helper functions
  - Add more input guarding code
  - Add new GtmValue class, to support character set encoding conversions, using RAII
  - Add support for MaybeLocal types where necessary to fix V8 deprecated functionality, which will become obsolete in the future
  - Add debug tracing support for four levels of debugging verbosity
  - Add exception handling around parsing of return JSON from v4wNode.m
  - Improve signal handling
  - Improve cache.node compatibility in `strict` mode
  - Add alias camel-case versions of methods that use underscores
    - Add globalDirectory for global_directory
    - Add nextNode for next_node
    - Add previousNode for previous_node
  - Add localDirectory and local_directory alias for listing the variables in the local symbol table
  - Add routine alias method for procedure to match cache.node
  - Add routine alias property for procedure and routine to match cache.node
  - Add timeout property to lock API as an alias for the second argument for passing timeouts in seconds
  - Improve error messages in thrown exceptions
  - Add support for calling the kill method without arguments, clearing the local symbol table
  - Improve return object format in merge API while in `canonical` mode
  - Add name space Nodem
- Refactor v4wNode.m for maintainability
  - Add various improvements for stability
  - Add new utility functions for better maintainability and code reuse
  - Add function and subroutine documentation, similar to JSDoc
  - Add debug tracing support for four levels of debugging verbosity
  - Update parsing functionality for better maintainability
  - Improve cache.node compatibility in `strict` mode
  - Improve handling of data edge cases in `canonical` mode
  - Name space local variables to enable local symbol table management support
  - Improve handling of signals, even with use of environment variables that can change YottaDB and GT.M's behavior around them
  - Improve scoping of local symbol table when calling the function and procedure APIs
  - Use full argument, intrinsic function, and intrinsic special variable names for ease of reading by new folks
- Update README.md with new features and improved instructions
- Strip out superfluous RUNPATH linker flags in binding.gyp
- Improve the quality of the set.js performance testing example script
- Add new command line option to test the set method on a local or global array
  - By passing the keyword `local` or `global` as either the first or second argument
- Add new command line option to test the set method on any size array
  - By passing the number of nodes as either the first or second argument
- Improve the quality of the zwrite.js testing example script
- Add new command line flags to use zwrite.js in multiple ways
  - [-f] - turns on fast mode, which bypasses the JavaScript loop
  - [-m] <canonical>|strict - changes the data mode for the data stored or retrieved from the database
  - [-c] <utf-8>|m - changes the character set encoding
  - [-d] - turns on debug mode, which displays low verbosity debugging trace statements
- Improve help message when Nodem fails to load on a require in nodem.js
- Improve the quality of preinstall.js so that it doesn't cause NPM to throw a stack trace unnecessarily
- Change preinstall.js so that it only writes to binding.gyp when necessary
- Add new postinstall.js script to pre-compile v4wNode.m
- Update package.json with new script options
  - Add debug script
  - Improve preinstall, install, and postinstall script error handling
- Update nodem.ci Call-in table with new function and new function prototypes
