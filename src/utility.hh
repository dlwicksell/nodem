/*
 * Package:    NodeM
 * File:       utility.hh
 * Summary:    Utility functions
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2019-2024 Fourth Watch Software LC
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
 */

#ifndef UTILITY_HH
#   define UTILITY_HH

#include <unistd.h>
#include <iostream>
#include <sstream>

namespace nodem {

#if __GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 30)
#   include <sys/syscall.h>
#   define gettid() syscall(SYS_gettid)
#endif

/*
 * @template {private} nodem::logger
 * @summary Add debug tracing data to custom error stream object
 * @param {ostream} stream - Error stream for debug tracing messages
 * @param {T} value - Data to put in to the error stream
 * @returns {void}
 */
template<class T>
inline static void logger(std::ostream& stream, T value)
{
    stream << value;
    return;
} // @end nodem::logger template function

/*
 * @template {private} nodem::logger
 * @summary Recurse through a variadic list of arguments, putting each one in to a custom error stream object
 * @param {ostream} stream - Error stream for debug tracing messages
 * @param {T} value - Next data argument to put in to the error stream
 * @param {variadic} {A} args - Variable arguments to the debugging logger
 * @returns {void}
 */
template<class T, class... A>
inline static void logger(std::ostream& stream, T value, A... args)
{
    logger(stream, value);
    logger(stream, args...);
    return;
} // @end nodem::logger variadic template function

/*
 * @template {private} nodem::debug_log
 * @summary Output debug information to the stderr stream, with support for threads, preventing intermixing of output
 * @param {variadic} {A} args - Variable arguments to the debugging logger
 * @returns {void}
 */
template<class... A>
static void debug_log(A... args)
{
    std::ostringstream stream;

    stream << "[C " << gettid() << "] DEBUG";
    logger(stream, args...);
    stream << std::endl;
    std::clog << stream.str();
    return;
} // @end nodem::debug_log variadic template function

} // @end namespace nodem

#endif // @end UTILITY_HH
