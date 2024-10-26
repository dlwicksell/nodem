# NodeM Changelog #

## v0.20.9 - 2024 Oct 26 ##

- Add support for Node.js 23.x
- Rename multiple source files
  - nodem.h => nodem.hh
  - compat.h => compat.hh
  - utility.h => utility.hh
  - gtm.h => gtm.hh
  - ydb.h => ydb.hh

## v0.20.8 - 2024 Aug 19 ##

- Improve the `function` and `procedure` implementations
- Add speed optimizations to the `function` method implementation

## v0.20.7 - 2024 Aug 7 ##

- Fix incompatible data type bugs in data component of the `set` API
- Make improvements to the nodem.js script
  - Add default use of nodem.ci in the repository to simplify configuration
  - No longer necessary to set `ydb_ci` or `GTMCI`, or `callinTable` in `open`
- Make improvements to the postinstall.js script
  - Add default routine search path to simplify configuration when used without
    the need to call other M code with the `function` or `procedure` APIs
  - No longer necessary to set `ydb_routines` or `gtmroutines`, or
    `routinesPath` in `open`

## v0.20.6 - 2024 Apr 29 ##

- Fix broken link to documentation on the YottaDB, LLC. site

## v0.20.5 - 2024 Apr 28 ##

- Add support for Node.js 22.x
- Add better support for the `Infinity` primitive to `lock` timeouts
- Improve signal handling when calling functions and procedures in v4wNode.m
- Make minor type improvements

## v0.20.4 - 2023 Sep 1 ##

- Fix infinite loop bug in `ydb::order` and `ydb::previous`
- Improve error handling in `ydb::next_node` and `ydb::previous_node`
- Remove unnecessary handling of locals from `ydb::order` and `ydb::previous`

## v0.20.3 - 2023 Aug 26 ##

- Improve consistency of API usage and return values
- Change prototype of `merge` in v4wNode.m
- Update built-in API `help` method
- Improve `transaction` method return value flexibility for signaling semantics
- Remove unnecessary `strict` mode
- Deprecate `strict` mode option in `open` and `configure` methods
- Make `strict` an alias for `string` in the `open` and `configure` methods
- Deprecate redundant options in the `open` method
  - `ip_address`
  - `tcp_port`
- Deprecate redundant arguments passed by-position in `increment` and `lock`
- Deprecate redundant method names
  - `next_node`
  - `previous_node`
  - `global_directory`
  - `local_directory`
- Add DEPRECATED message to first use of each deprecated option and method
- Remove unnecessary fast mode in zwrite.js
- Fix bug that prevented `batch` mode without a variable list in `transaction`
- Improve asynchronous error handling in nodem.cc
- Fix line display bug in transaction.js
- Fix bug in polyfill of `previousNode` [`reverseQuery`] in v4wNode.m
- Fix regression that prevented call by-reference arguments in v4wNode.m
- Improve the contents of CHANGELOG.md
- Add .npmignore file to repository

## v0.20.2 - 2022 Mar 10 ##

- Reformat various parts of the source code
- Rename several classes, instances, types, and variables
- Rename addon module shared library from mumps.node => nodem.node
- Rename multiple source files
  - nodem.h => compat.h
  - mumps.h => nodem.h
  - mumps.cc => nodem.cc
  - release-notes.md => CHANGELOG.md
- Fix bugs in the environ script

## v0.20.1 - 2021 Sep 28 ##

- Fix potential segfault in `version` API

## v0.20.0 - 2021 Mar 10 ##

- Add transaction processing API [`transaction`]
- Add transaction.js example program
- Change structure for debug messages to start with [C _TID_] or [M _$JOB_]
- Make various improvements

## v0.19.0 - 2020 Aug 8 ##

- Add full asynchronous support to the `merge` API

## v0.18.1 - 2020 Jul 29 ##

- Update version documentation in `help` method

## v0.18.0 - 2020 Jul 28 ##

- Add full asynchronous support to the `version` API
- Add polyfill implementation of `previousNode` in v4wNode.m (GT.M & YDB r1.00)

## v0.17.3 - 2020 Jul 1 ##

- Improve debug tracing serialization

## v0.17.2 - 2020 Jun 24 ##

- Improve the performance of set.js

## v0.17.1 - 2020 May 29 ##

- Add support for YottaDB and GT.M auto-relink syntax in the environ script

## v0.17.0 - 2020 May 27 ##

- Add full asynchronous support to the `lock` and `unlock` APIs
- Add SimpleAPI support to the `lock` and `unlock` APIs
- Add support for calling arguments by-position to the `lock` and `unlock` APIs
- Add new `string` mode to the `open` and `configure` APIs

## v0.16.2 - 2020 Feb 21 ##

- Fix error and warning handling in postinstall.js

## v0.16.1 - 2020 Feb 20 ##

- Fix potential application crash [SIGSEGV] when calling the `configure` API
  with no arguments on older versions of Node.js
- Fix compiler error on systems with older GLIBC version
- Rework error handling in postinstall.js

## v0.16.0 - 2020 Feb 18 ##

- Add full synchronous and asynchronous support for the worker_threads API
- Add `configure` API, which can change per-thread database connection options
  - Add `charset`, `mode`, `autoRelink`, and `debug` properties to `configure`
- Make `charset`, `mode`, `autoRelink`, and `debug` in `open` and `configure`
  work per-thread
- Reimplement debug tracing
  - Add debug code in utility.h
  - Add worker thread support
  - Add asynchronous support
  - Change debug output to write to stderr
    - Make it work when running M code
  - Change structure for debug messages
    - Start with thread ID or MUMPS in square brackets
- Add `threadpoolSize` option to the `open` API
- Add `tid` property to the output for the `open` API
- Improve performance
- Rename utility.h => nodem.h
- Restructure most of code base
- Add support for Node.js 12.x and 13.x
- Add version check in debug mode for v4wNode.m
- Add support for extended global references for the SimpleAPI

## v0.15.0 - 2019 Jul 9 ##

- Add full asynchronous support to the `increment` API
- Add SimpleAPI support to the `increment` API
- Add support for calling arguments by-position to the `increment` API

## v0.14.4 - 2019 Jul 2 ##

- Fix `increment` bug that prevented any increment from working
- Fix `set` bug with negative numbers using the SimpleAPI
- Improve debug tracing for SimpleAPI subscripts vector

## v0.14.3 - 2019 Jun 24 ##

- Add support for Node.js 12.x

## v0.14.2 - 2019 Feb 17 ##

- Fix support for YottaDB r1.24

## v0.14.1 - 2019 Feb 1 ##

- Add support for YottaDB r1.24

## v0.14.0 - 2019 Jan 17 ##

- Add full asynchronous support to the `function` and `procedure` APIs
- Add support for calling arguments by-position to `function` and `procedure`
- Add `nodeOnly` option to `kill` API [zkill]
- Improve usage of `gtm_cip`
- Fix possible race conditions with error handling
- Improve debug tracing code
- Improve error handling during subscript parsing
- Update built-in API `help` method

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

- Add asynchronous support to `data`, `previous`, `nextNode`, and `previousNode`
- Add SimpleAPI support to `data`, `previous`, `nextNode`, and `previousNode`
- Add support for call by-position to `data`, `previous`, `nextNode`, and
  `previousNode`
- Restructured the README.md documentation
- Remove simple-api script now that SimpleAPI support is more fully tested
- Update zwrite.js
  - Add support for full debug tracing to CLI
  - Improve error handling
  - Improve argument handling
- Update gtm.hh and gtm.cc
  - Add asynchronous support
  - Improve efficiency of debug tracing code
- Update ydb.hh and ydb.cc
  - Add asynchronous support
  - Ignore variables that begin with `v4w` in `order` and `previous`
  - Improve efficiency of debug tracing code
- Update v4wNode.m
  - Ignore variables that begin with `v4w` in `order` and `previous`
  - Improve `previousNode` for versions of YottaDB and GT.M without it
  - Add `ok` property to the return object of `previousNode`, `retrieve`, and
    `update`
  - Simplify version API when connected to YottaDB
- Update mumps.cc
  - Add `signalHandler` property to the `open` API
  - Improve and update `help` API data
  - Rename the `catch_interrupt` function => `clean_shutdown`
  - Rename the `gtm_status` function => `error_status`
  - Improve signal handling in `clean_shutdown` signal handler function
  - Improve efficiency of debug tracing code
  - Add `is_number` function to support `canonical` mode for YottaDB SimpleAPI
  - Change internal API interfaces to pass the Baton struct around for better
    maintainability
  - Replace the asynchronous conditional jump blocks with function pointers in
    the Baton struct
  - Fix UTF-8 bug in the `encode_arguments` function
  - Add `build_subscripts` function to more easily support the SimpleAPI
  - Fix potential memory leaks

## v0.12.1 - 2018 Oct 10 ##

- Turn SimpleAPI support on by default
- Speed up performance of SimpleAPI support

## v0.12.1-pre - 2018 Sep 24 ##

- Pre-release version with SimpleAPI support on by default, for testing

## v0.12.0 - 2018 Sep 22 ##

- Add asynchronous support to the `get`, `kill`, `order`, and `set` APIs
- Add call by-position support for the `get`, `kill`, `order`, and `set` APIs
- Add support for intrinsic special variables (ISVs) to the `get` and `set` APIs
- Add new experimental support for YottaDB's SimpleAPI (for `get`, `kill`,
  `order`, and `set`)
- Remove preinstall.js and move functionality to binding.gyp
- Add new implementation files for better architectural support of new SimpleAPI
- Rename source file mumps.hh => mumps.h
- Rename the `routinePath` option => `routinesPath`

## v0.11.2 - 2018 Apr 24 ##

- Fix CPP bug

## v0.11.1 - 2018 Apr 15 ##

- Merge Chris Edwards' enhancements
- Make minor changes, including more support for ydb* environment variables
- Add v4wNode.o compiled object file to .gitignore

## v0.11.0 - 2018 Apr 10 ##

- Improve support for legacy M APIs
- Add support for passing arguments by-reference, and by-variable, to functions
  and procedures
- Add documentation and examples about new functionality in README.md
- Fix typo and restructure documentation in README.md

## v0.10.1 - 2018 Apr 8 ##

- Make a few updates to v0.10.0 that were missed
- Allow passing more data types in the subscripts and arguments array, and
  convert them to strings, in mumps.cc
- Add information about the new `help` method from v0.10.0 to README.md
- Add an example of using Nodem to README.md
- Fix scoping bug in v4wNode.m

## v0.10.0 - 2018 Apr 5 ##

- Refactor Nodem for future maintainability
- Add support to APIs for local variables with the new `local` property and the
  new `localDirectory` method
- Change character set encoding to default to UTF-8 and decouple it from the
  encoding set for the underlying YottaDB or GT.M database
- Add support for the `previousNode` API in YottaDB versions r1.10 and newer
- Remove support for Node.js versions earlier than 0.12.0
- Add new `help` method, with a list of APIs, and more detailed call information
  for each API
- Add support to the `open` API for new configuration settings
  - Add `routinePath` to change the routine look-up path when using the
    `function` and `procedure` methods
  - Add `callinPath` to make it easier to support environments running more than
    just Nodem with the Call-in interface
  - Add `debug` to turn on debug tracing
  - Add `charset` to enable changing the character set encoding directly
  - Remove `path` , as it was unhelpful, and misleading
- Refactor mumps.hh for maintainability
  - Remove C macro support, no longer necessary after removing support for
    Node.js versions prior to 0.12.0
  - Add support for distinguishing between YottaDB and GT.M distributions
  - Add Nodem namespace
- Refactor mumps.cc to use a more modular structure for maintainability
  - Add various improvements for stability
  - Add function and method documentation, similar to JSDoc
  - Add several new utility helper functions
  - Add more input guarding code
  - Add GtmValue class, to support character set encoding conversions using RAII
  - Add support for MaybeLocal types where necessary to fix V8 deprecated
    functionality, which will become obsolete in the future
  - Add debug tracing support for four levels of debugging verbosity
  - Add exception handling around parsing of return JSON from v4wNode.m
  - Improve signal handling
  - Add alias camel-case versions of methods that use underscores
    - Add `globalDirectory` for `global_directory`
    - Add `nextNode` for `next_node`
    - Add `previousNode` for `previous_node`
  - Add `localDirectory` and `local_directory` alias for listing the variables
    in the local symbol table
  - Add `routine` alias method for `procedure`
  - Add `routine` alias property for `procedure` and `routine` methods
  - Add `timeout` property to the `lock` API as an alias for the second argument
    for passing timeouts in seconds
  - Improve error messages in thrown exceptions
  - Add support for calling the `kill` method without arguments, clearing the
    local symbol table
  - Improve return object format in `merge` API while in `canonical` mode
  - Add Nodem namespace
- Refactor v4wNode.m for maintainability
  - Add various improvements for stability
  - Add new utility functions for better maintainability and code reuse
  - Add function and routine label documentation, similar to JSDoc
  - Add debug tracing support for four levels of debugging verbosity
  - Update parsing functionality for better maintainability
  - Improve handling of data edge cases in `canonical` mode
  - Namespace local variables to enable local symbol table management support
  - Improve handling of signals across different YottaDB and GT.M configurations
  - Improve scoping of symbol table when using `function` and `procedure`
  - Use full argument, intrinsic function, and intrinsic special variable names
  - Increase combined length limit of arguments in `function` and `procedure`
    - Increase from 8 KiB to nearly 1 MiB
- Update README.md with new features and improved instructions
- Strip out superfluous RUNPATH linker flags in binding.gyp
- Improve the quality of the set.js performance testing example script
  - Add new command line option to test the `set` method on a local or global
    - By passing the keyword `local` or `global` as either argument
  - Add new command line option to test the `set` method on any size array
    - By passing the number of nodes as either argument
- Improve the quality of the zwrite.js testing example script
- Add new command line flags to use zwrite.js in multiple ways
  - [-f] - turns on fast mode, which bypasses the JavaScript loop
  - [-m] <canonical>|strict - changes the data mode for the data stored or
    retrieved from the database
  - [-c] <utf-8>|m - changes the character set encoding
  - [-d] - turns on debug mode, displaying low verbosity debugging statements
- Improve help message when Nodem fails to load on a require in nodem.js
- Improve preinstall.js so that it doesn't throw a stack trace unnecessarily
- Change preinstall.js so that it only writes to binding.gyp when necessary
- Add new postinstall.js script to pre-compile v4wNode.m
- Update package.json with new script options
  - Add debug script
  - Improve preinstall, install, and postinstall script error handling
- Update nodem.ci Call-in table with new function and new function prototypes
