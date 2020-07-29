/*
 * Package:    NodeM
 * File:       mumps.cc
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2020 Fourth Watch Software LC
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

#include "mumps.h"
#include "nodem.h"
#include "gtm.h"
#include "ydb.h"

#include <cerrno>
#include <csignal>
#include <cstdlib>

#include <algorithm>

#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
using node::AddEnvironmentCleanupHook;
using node::GetCurrentEventLoop;
#endif

using v8::Array;
using v8::Boolean;
using v8::EscapableHandleScope;
using v8::Exception;
using v8::External;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
#if NODE_MAJOR_VERSION >= 3
using v8::MaybeLocal;
#endif
#if NODE_MAJOR_VERSION >= 7 || NODE_MAJOR_VERSION == 6 && NODE_MINOR_VERSION >= 8
using v8::NewStringType;
#endif
using v8::Number;
using v8::Object;
using v8::String;
using v8::TryCatch;
using v8::Value;

using std::cerr;
using std::cout;
using std::clog;
using std::endl;
using std::string;
using std::stringstream;
using std::vector;

/*
 * @function {private} unistd_close
 * @summary Call the C Standard Library close(2) Syscall
 * @param {int} fd - The file descriptor to close
 * @returns int - The return value, 0 on success, -1 on failure
 */
inline static int unistd_close(int fd)
{
    return close(fd);
} // @end unistd_close

namespace nodem {

uv_mutex_t mutex_g;
int save_stdout_g = -1;
bool utf8_g = true;
bool auto_relink_g = false;
enum mode_t mode_g = CANONICAL;
enum debug_t debug_g = OFF;

static bool reset_term_g = false;
static bool signal_sigint_g = true;
static bool signal_sigquit_g = true;
static bool signal_sigterm_g = true;
static struct termios term_attr_g;

static enum {
    CLOSED,
    NOT_OPEN,
    OPEN
} gtm_state_g = NOT_OPEN;

/*
 * @function nodem::clean_shutdown
 * @summary Handle a SIGINT/SIGQUIT/SIGTERM signal, by cleaning up everything, and exiting Node.js
 * @param {int} signal_num - The number of the caught signal
 * @returns {void}
 */
void clean_shutdown(const int signal_num)
{
    if (gtm_state_g == OPEN) {
        if (uv_mutex_trylock(&mutex_g) == 0) {
#if NODEM_SIMPLE_API == 1
            ydb_exit();
#else
            gtm_exit();
#endif

            uv_mutex_unlock(&mutex_g);
        }

        term_attr_g.c_iflag |= ICRNL;
        term_attr_g.c_lflag |= (ICANON | ECHO);

        if (isatty(STDIN_FILENO)) {
            tcsetattr(STDIN_FILENO, TCSANOW, &term_attr_g);
        } else if (isatty(STDOUT_FILENO)) {
            tcsetattr(STDOUT_FILENO, TCSANOW, &term_attr_g);
        } else if (isatty(STDERR_FILENO)) {
            tcsetattr(STDERR_FILENO, TCSANOW, &term_attr_g);
        }
    }

    if (signal_num == SIGQUIT) {
        struct sigaction signal_attr;
        signal_attr.sa_handler = SIG_DFL;

        sigaction(SIGABRT, &signal_attr, NULL);

        abort();
    }

    _exit(EXIT_FAILURE);
} // @end nodem::clean_shutdown function

#if YDB_RELEASE >= 126
/*
 * @function {private} nodem::reset_handler
 * @summary Reset the SIGINT signal when running YottaDB r1.26 or newer (this is a hack)
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {bool} reset_handler - Flag that controls whether to reset the signal handlers
 * @returns {void}
 */
inline static void reset_handler(GtmState* gtm_state)
{

    if (gtm_state->reset_handler == false && signal_sigint_g == true) {
        struct sigaction signal_attr;

        signal_attr.sa_handler = clean_shutdown;
        signal_attr.sa_flags = 0;

        sigfillset(&signal_attr.sa_mask);
        sigaction(SIGINT, &signal_attr, NULL);

        gtm_state->reset_handler = true;
    }

    return;
} // @end nodem::reset_handler function
#endif

#if NODEM_SIMPLE_API == 1
/*
 * @function {private} nodem::is_number
 * @summary Check if a value returned from YottaDB's SimpleAPI is a canonical number
 * @param {string} data - The data value to be tested
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {boolean} - Whether the data value is a canonical number or not
 */
inline static bool is_number(const string data, GtmState* gtm_state)
{
    /*
     * YottaDB/GT.M approximate (using number of digits, rather than number value) number limits:
     *   - 47 digits before overflow (resulting in an overflow error)
     *   - 18 digits of precision
     * Node.js/JavaScript approximate (using number of digits, rather than number value) number limits:
     *   - 309 digits before overflow (represented as the Infinity primitive)
     *   - 21 digits before conversion to exponent notation
     *   - 16 digits of precision
     * This is why anything over 16 characters needs to be treated as a string
     */

    // In string and strict modes, all data is treated as a string
    if (gtm_state->mode == STRICT || gtm_state->mode == STRING)
        return false;

    bool flag = false;
    size_t neg_cnt = count(data.begin(), data.end(), '-');
    size_t decp_cnt = count(data.begin(), data.end(), '.');

    if ((decp_cnt == 0 || decp_cnt == 1) && (neg_cnt == 0 || (neg_cnt == 1 && data[0] == '-')))
        flag = true;

    if ((decp_cnt == 1 || neg_cnt == 1) && data.length() <= 1)
        flag = false;

    if (data.length() > 16 || data[data.length() - 1] == '.')
        flag = false;

    if (flag && !data.empty() && all_of(data.begin(), data.end(), [](char c) {return (std::isdigit(c) || c == '-' || c == '.');})) {
        if ((data[0] == '0' && data.length() > 1) || (decp_cnt == 1 && data[data.length() - 1] == '0')) {
            return false;
        } else {
            return true;
        }
    } else {
        return false;
    }
} // @end nodem::is_number function
#endif

/*
 * @function {private} nodem::json_method
 * @summary Call a method on the built-in Node.js JSON object
 * @param {Local<Value>} data - A JSON string containing the data to parse or a JavaScript object to stringify
 * @param {string} type - The name of the method to call on JSON
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} - An object containing the output data
 */
static Local<Value> json_method(Local<Value> data, const string type, GtmState* gtm_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    json_method enter");

        if (!data->IsObject())
            debug_log(">>>    data: ", *(UTF8_VALUE_TEMP_N(isolate, data)));

        debug_log(">>>    type: ", type);
    }

    Local<Object> global = isolate->GetCurrentContext()->Global();
    Local<Object> json = to_object_n(isolate, get_n(isolate, global, new_string_n(isolate, "JSON")));
    Local<Function> method = Local<Function>::Cast(get_n(isolate, json, new_string_n(isolate, type.c_str())));

    if (gtm_state->debug > MEDIUM)
        debug_log(">>>    json_method exit");

    return scope.Escape(call_n(isolate, method, json, 1, &data));
} // @end nodem::json_method function

/*
 * @function {private} nodem::error_status
 * @summary Handle an error from the YottaDB/GT.M runtime
 * @param {gtm_char_t*} error - A character string representing the YottaDB/GT.M run-time error
 * @param {bool} position - Whether the API was called by positional arguments or not
 * @param {bool} async - Whether the API was called asynchronously or not
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} result - An object containing the formatted error content
 */
static Local<Value> error_status(gtm_char_t* error, const bool position, const bool async, GtmState* gtm_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    error_status enter");
        debug_log(">>>    error: ", error);
        debug_log(">>>    position: ", std::boolalpha, position);
        debug_log(">>>    async: ", std::boolalpha, async);
    }

    char* error_msg;
    char* code = strtok_r(error, ",", &error_msg);

    int error_code = atoi(code);

    if (strstr(error_msg, "%YDB-E-CTRAP") != NULL || strstr(error_msg, "%GTM-E-CTRAP") != NULL)
        clean_shutdown(SIGINT); // Handle SIGINT caught by YottaDB or GT.M

    Local<Object> result = Object::New(isolate);

    if (position && !async) {
        if (gtm_state->debug > MEDIUM) {
            debug_log(">>>    error_status exit");
            debug_log(">>>    error_msg: ", error_msg);
        }

        return scope.Escape(new_string_n(isolate, error_msg));
    } else if (gtm_state->mode == STRICT) {
        set_n(isolate, result, new_string_n(isolate, "ok"), Number::New(isolate, 0));
        set_n(isolate, result, new_string_n(isolate, "ErrorCode"), Number::New(isolate, error_code));
        set_n(isolate, result, new_string_n(isolate, "ErrorMessage"), new_string_n(isolate, error_msg));
    } else {
        set_n(isolate, result, new_string_n(isolate, "ok"), Boolean::New(isolate, false));
        set_n(isolate, result, new_string_n(isolate, "errorCode"), Number::New(isolate, error_code));
        set_n(isolate, result, new_string_n(isolate, "errorMessage"), new_string_n(isolate, error_msg));
    }

    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    error_status exit");

        Local<Value> result_string = json_method(result, "stringify", gtm_state);
        debug_log(">>>    result: ", *(UTF8_VALUE_TEMP_N(isolate, result_string)));
    }

    return scope.Escape(result);
} // @end nodem::error_status function

/*
 * @function {private} nodem::invalid_name
 * @summary If a variable name contains subscripts, it is not valid, and cannot be used
 * @param {char*} name - The name to test against
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {bool} - Whether the name is invalid
 */
static bool invalid_name(const char* name, GtmState* gtm_state)
{
    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    invalid_name enter");
        debug_log(">>>    name: ", name);
    }

    if (strchr(name, '(') != NULL || strchr(name, ')') != NULL) {
        if (gtm_state->debug > MEDIUM)
            debug_log(">>>    invalid_name exit: ", std::boolalpha, true);

        return true;
    }

    if (gtm_state->debug > MEDIUM)
        debug_log(">>>    invalid_name exit: ", std::boolalpha, false);

    return false;
} // @end nodem::invalid_name function

/*
 * @function {private} nodem::invalid_local
 * @summary If a local variable name starts with v4w, it is not valid, and cannot be manipulated
 * @param {char*} name - The name to test against
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {bool} - Whether the local name is invalid
 */
static bool invalid_local(const char* name, GtmState* gtm_state)
{
    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    invalid_local enter");
        debug_log(">>>    name: ", name);
    }

    if (strncmp(name, "v4w", 3) == 0) {
        if (gtm_state->debug > MEDIUM)
            debug_log(">>>    invalid_local exit: ", std::boolalpha, true);

        return true;
    }

    if (gtm_state->debug > MEDIUM)
        debug_log(">>>    invalid_local exit: ", std::boolalpha, false);

    return false;
} // @end nodem::invalid_local function

/*
 * @function {private} nodem::globalize_name
 * @summary If a variable name (or function/procedure) doesn't start with (or contain) the optional '^' character, add it for output
 * @param {Local<Value>} name - The name to be normalized for output
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} [new_name|name] - A string containing the normalized name
 */
static Local<Value> globalize_name(const Local<Value> name, GtmState* gtm_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    globalize_name enter");
        debug_log(">>>    name: ", *(UTF8_VALUE_TEMP_N(isolate, name)));
    }

    UTF8_VALUE_N(isolate, data_string, name);

    const gtm_char_t* data_name = *data_string;
    const gtm_char_t* char_ptr = strchr(data_name, '^');

    if (char_ptr == NULL) {
        Local<Value> new_name = concat_n(isolate, new_string_n(isolate, "^"), to_string_n(isolate, name));

        if (gtm_state->debug > MEDIUM)
            debug_log(">>>    globalize_name exit: ", *(UTF8_VALUE_TEMP_N(isolate, new_name)));

        return scope.Escape(new_name);
    }

    if (gtm_state->debug > MEDIUM)
        debug_log(">>>    globalize_name exit: ", *(UTF8_VALUE_TEMP_N(isolate, name)));

    return scope.Escape(name);
} // @end nodem::globalize_name function

/*
 * @function {private} nodem::localize_name
 * @summary If a variable name starts with the optional '^' character, strip it off for output
 * @param {Local<Value>} name - The name to be normalized for output
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} [data_name|name] - A string containing the normalized name
 */
static Local<Value> localize_name(const Local<Value> name, GtmState* gtm_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    localize_name enter");
        debug_log(">>>     name: ", *(UTF8_VALUE_TEMP_N(isolate, name)));
    }

    UTF8_VALUE_N(isolate, data_string, name);

    const gtm_char_t* data_name = *data_string;
    const gtm_char_t* char_ptr = strchr(data_name, '^');

    if (char_ptr != NULL && char_ptr - data_name == 0) {
        if (gtm_state->debug > MEDIUM)
            debug_log(">>>    localize_name exit: ", &data_name[1]);

        return scope.Escape(new_string_n(isolate, &data_name[1]));
    }

    if (gtm_state->debug > MEDIUM)
        debug_log(">>>    localize_name exit: ", *(UTF8_VALUE_TEMP_N(isolate, name)));

    return scope.Escape(name);
} // @end nodem::localize_name function

/*
 * @function {private} nodem::encode_arguments
 * @summary Encode an array of arguments for parsing in v4wNode.m
 * @param {Local<Value>} arguments - The array of subscripts or arguments to be encoded
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @param {boolean} function <false> - Whether the arguments to encode are from the function or procedure call or not
 * @returns {Local<Value>} [Undefined|encoded_array] - The encoded array of subscripts or arguments, or Undefined if it has bad data
 */
static Local<Value> encode_arguments(const Local<Value> arguments, GtmState* gtm_state, const bool function = false)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    encode_arguments enter");

        Local<Value> argument_string = json_method(arguments, "stringify", gtm_state);
        debug_log(">>>    arguments: ", *(UTF8_VALUE_TEMP_N(isolate, argument_string)));
    }

    Local<Array> argument_array = Local<Array>::Cast(arguments);
    Local<Array> encoded_array = Array::New(isolate);

    for (unsigned int i = 0; i < argument_array->Length(); i++) {
        Local<Value> data_test = get_n(isolate, argument_array, i);
        Local<String> data_value = to_string_n(isolate, data_test);
        Local<String> colon = new_string_n(isolate, ":");
        Local<String> length;
        Local<Value> new_data = Undefined(isolate);

        if (data_test->IsUndefined()) {
            new_data = new_string_n(isolate, "0:");
        } else if (data_test->IsSymbol() || data_test->IsSymbolObject()) {
            return Undefined(isolate);
        } else if (data_test->IsNumber()) {
            length = to_string_n(isolate, Number::New(isolate, data_value->Length()));
            new_data = concat_n(isolate, length, concat_n(isolate, colon, data_value));
        } else if (data_test->IsObject()) {
            if (!function)
                return Undefined(isolate);

            Local<Object> object = to_object_n(isolate, data_test);
            Local<Value> type = get_n(isolate, object, new_string_n(isolate, "type"));
            Local<Value> value_test = get_n(isolate, object, new_string_n(isolate, "value"));
            Local<String> value = to_string_n(isolate, value_test);

            if (value_test->IsSymbol() || value_test->IsSymbolObject()) {
                return Undefined(isolate);
            } else if (type->StrictEquals(new_string_n(isolate, "reference"))) {
                if (!value_test->IsString())
                    return Undefined(isolate);
                if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, value)), gtm_state))
                    return Undefined(isolate);
                if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, value)), gtm_state))
                    return Undefined(isolate);

                Local<String> new_value = to_string_n(isolate, localize_name(value, gtm_state));
                Local<String> dot = new_string_n(isolate, ".");

                if (gtm_state->utf8 == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, new_value) + 1));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, new_value->Length() + 1));
                }

                new_data = concat_n(isolate, length, concat_n(isolate, colon, concat_n(isolate, dot, new_value)));
            } else if (type->StrictEquals(new_string_n(isolate, "variable"))) {
                if (!value_test->IsString())
                    return Undefined(isolate);
                if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, value)), gtm_state))
                    return Undefined(isolate);
                if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, value)), gtm_state))
                    return Undefined(isolate);

                Local<String> new_value = to_string_n(isolate, localize_name(value, gtm_state));

                if (gtm_state->utf8 == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, new_value)));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, new_value->Length()));
                }

                new_data = concat_n(isolate, length, concat_n(isolate, colon, new_value));
            } else if (type->StrictEquals(new_string_n(isolate, "value"))) {
                if (value_test->IsUndefined()) {
                    new_data = new_string_n(isolate, "0:");
                } else if (value_test->IsSymbol() || value_test->IsSymbolObject()) {
                    return Undefined(isolate);
                } else if (value_test->IsNumber()) {
                    length = to_string_n(isolate, Number::New(isolate, value->Length()));
                    new_data = concat_n(isolate, length, concat_n(isolate, colon, value));
                } else {
                    if (gtm_state->utf8 == true) {
                        length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, value) + 2));
                    } else {
                        length = to_string_n(isolate, Number::New(isolate, value->Length() + 2));
                    }

                    Local<String> quote = new_string_n(isolate, "\"");
                    new_data = concat_n(isolate, concat_n(isolate, length,
                      concat_n(isolate, colon, quote)), concat_n(isolate, value, quote));
                }
            } else {
                if (gtm_state->utf8 == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, data_value) + 2));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, data_value->Length() + 2));
                }

                Local<String> quote = new_string_n(isolate, "\"");
                new_data = concat_n(isolate, concat_n(isolate, length,
                  concat_n(isolate, colon, quote)), concat_n(isolate, data_value, quote));
            }
        } else {
            if (gtm_state->utf8 == true) {
                length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, data_value) + 2));
            } else {
                length = to_string_n(isolate, Number::New(isolate, data_value->Length() + 2));
            }

            Local<String> quote = new_string_n(isolate, "\"");
            new_data = concat_n(isolate, concat_n(isolate, length,
              concat_n(isolate, colon, quote)), concat_n(isolate, data_value, quote));
        }

        set_n(isolate, encoded_array, i, new_data);
    }

    if (gtm_state->debug > MEDIUM)
        debug_log(">>>    encode_arguments exit: ", *(UTF8_VALUE_TEMP_N(isolate, encoded_array)));

    return scope.Escape(encoded_array);
} // @end nodem::encode_arguments function

#if NODEM_SIMPLE_API == 1
/*
 * @function {private} nodem::build_subscripts
 * @summary Build an array of subscritps for passing to the SimpleAPI
 * @param {Local<Value>} subscripts - The array of subscripts to be built
 * @param {bool&} error - If this is set to true, it signals an error with subscript data
 * @param {GtmState*} gtm_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @returns {vector<string>} [build_array] - The built array of subscripts
 */
static vector<string> build_subscripts(const Local<Value> subscripts, bool& error, GtmState* gtm_state)
{
    Isolate* isolate = Isolate::GetCurrent();

    if (gtm_state->debug > MEDIUM) {
        debug_log(">>>    build_subscripts enter");

        Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_state);
        debug_log(">>>    subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
    }

    Local<Array> subscripts_array = Local<Array>::Cast(subscripts);
    unsigned int length = subscripts_array->Length();

    string subs_data;
    vector<string> subs_array;

    for (unsigned int i = 0; i < length; i++) {
        Local<Value> data = get_n(isolate, subscripts_array, i);

        if (data->IsSymbol() || data->IsSymbolObject() || data->IsObject() || data->IsArray()) {
            error = true;

            return subs_array;
        }

        if (gtm_state->utf8 == true) {
            subs_data = *(UTF8_VALUE_TEMP_N(isolate, data));
        } else {
            GtmValue gtm_state {data};
            subs_data = gtm_state.to_byte();
        }

        if (gtm_state->mode == CANONICAL && data->IsNumber()) {
            if (subs_data.substr(0, 2) == "0.")
                subs_data = subs_data.substr(1, string::npos);

            if (subs_data.substr(0, 3) == "-0.")
                subs_data = "-" + subs_data.substr(2, string::npos);
        }

        if (gtm_state->debug > MEDIUM)
            debug_log(">>>    subs_data[", i, "]: ", subs_data);

        subs_array.push_back(subs_data);
    }

    if (gtm_state->debug > MEDIUM)
        debug_log(">>>    build_subscripts exit");

    return subs_array;
} // @end nodem::build_subscripts function
#endif

/*
 * @class nodem::GtmValue
 * @method {instance} to_byte
 * @summary Convert a UTF-8 encoded buffer to a byte encoded buffer
 * @returns {gtm_char_t*} A byte encoded buffer
 */
gtm_char_t* GtmValue::to_byte(void)
{
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 12
    Isolate* isolate = Isolate::GetCurrent();

    value->WriteOneByte(isolate, buffer, 0, size);
#else
    value->WriteOneByte(buffer, 0, size);
#endif
    return reinterpret_cast<gtm_char_t*>(buffer);
} // @end GtmValue::to_byte method

/*
 * @class nodem::GtmValue
 * @method {class} from_byte
 * @summary Convert a byte encoded buffer to a UTF-8 encoded buffer
 * @param {gtm_char_t[]} buffer - A byte encoded buffer
 * @returns {Local<String>} A UTF-8 encoded buffer
 */
Local<String> GtmValue::from_byte(gtm_char_t buffer[])
{
    Isolate* isolate = Isolate::GetCurrent();

#if NODE_MAJOR_VERSION >= 7 || NODE_MAJOR_VERSION == 6 && NODE_MINOR_VERSION >= 8
    MaybeLocal<String> string = String::NewFromOneByte(isolate, reinterpret_cast<const uint8_t*>(buffer), NewStringType::kNormal);

    if (string.IsEmpty()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Unable to convert from a byte buffer to UTF-8")));
        return String::Empty(isolate);
    } else {
        return string.ToLocalChecked();
    }
#else
    return String::NewFromOneByte(isolate, reinterpret_cast<const uint8_t*>(buffer));
#endif
} // @end GtmValue::from_byte method

/*
 * @function {private} nodem::version
 * @summary Return the about/version string
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from data call
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> version(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  version enter");

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
    }

    Local<String> return_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        return_string = new_string_n(isolate, gtm_baton->result);
    } else {
        return_string = GtmValue::from_byte(gtm_baton->result);
    }

    Local<String> nodem_version = new_string_n(isolate,
      "Node.js Adaptor for " NODEM_DB ": Version: " NODEM_VERSION " (ABI=" NODEM_STRING(NODE_MODULE_VERSION) ") [FWS]");

    Local<String> ret_string = new_string_n(isolate, gtm_baton->result);
    Local<String> version_string = concat_n(isolate, nodem_version, concat_n(isolate, new_string_n(isolate, "; "), ret_string));

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  version exit");

    if (gtm_state_g < OPEN) {
        return scope.Escape(nodem_version);
    } else {
        return scope.Escape(version_string);
    }
} // @end nodem::version function

/*
 * @function {private} nodem::data
 * @summary Check if global or local node has data and/or children or not
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from data call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> data(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  data enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, atof(gtm_baton->result)));
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  data JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  data exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"),
              get_n(isolate, temp_object, new_string_n(isolate, "defined")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "defined")));
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "defined"),
          get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  data exit");

    return scope.Escape(return_object);
} // @end nodem::data function

/*
 * @function {private} nodem::get
 * @summary Return data from a global or local node, or an intrinsic special variable
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @member {gtm_char_t*} result - Data returned from get call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> get(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  get enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   status: ", gtm_baton->status);
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (gtm_baton->status == YDB_ERR_GVUNDEF || gtm_baton->status == YDB_ERR_LVUNDEF) {
        if (gtm_baton->mode == STRICT) {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, 0));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, false));
        }
    } else {
        if (gtm_baton->mode == STRICT) {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, 1));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, true));
        }
    }

    string data(gtm_baton->result);

    if (is_number(data, gtm_baton->gtm_state)) {
        set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(gtm_baton->result)));
    } else {
        if (gtm_baton->gtm_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, gtm_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), GtmValue::from_byte(gtm_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  get JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  get exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"),
              get_n(isolate, temp_object, new_string_n(isolate, "data")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "data")));
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "data"), get_n(isolate, temp_object, new_string_n(isolate, "data")));
        set_n(isolate, return_object, new_string_n(isolate, "defined"),
          get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  get exit");

    return scope.Escape(return_object);
} // @end nodem::get function

/*
 * @function {private} nodem::set
 * @summary Return data about the store of a global or local node, or an intrinsic special variable
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Local<Value>} data - V8 object containing the data to store in the node that was called
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> set(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  set enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);
    Local<Value> data_value = Local<Value>::New(isolate, gtm_baton->data_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }

        debug_log(">>   data: ", *(UTF8_VALUE_TEMP_N(isolate, data_value)));
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  set exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), new_string_n(isolate, "0"));

            return scope.Escape(return_object);
        } else if (gtm_baton->gtm_state->mode == STRICT) {
            return scope.Escape(Number::New(isolate, 0));
        } else {
            Local<Value> ret_data = Boolean::New(isolate, true);
            return scope.Escape(ret_data);
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "data"), data_value);

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), new_string_n(isolate, "0"));
        } else if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), Number::New(isolate, 0));
        }
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  set exit");

    return scope.Escape(return_object);
} // @end nodem::set function

/*
 * @function {private} nodem::kill
 * @summary Return data about removing a global or global node, or a local or local node, or the entire local symbol table
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {int32_t} node_only - Whether the API was called on a single node only, or on a node and all its children
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> kill(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  kill enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (name->StrictEquals(new_string_n(isolate, "")) || gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  kill exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), new_string_n(isolate, "0"));

            return scope.Escape(return_object);
        } else if (gtm_baton->gtm_state->mode == STRICT) {
            return scope.Escape(Number::New(isolate, 0));
        } else {
            Local<Value> ret_data = Boolean::New(isolate, true);
            return scope.Escape(ret_data);
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), new_string_n(isolate, "0"));
        } else if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), Number::New(isolate, 0));
        }

        if (gtm_baton->node_only > -1)
            set_n(isolate, return_object, new_string_n(isolate, "nodeOnly"), Boolean::New(isolate, gtm_baton->node_only));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  kill exit");

    return scope.Escape(return_object);
} // @end nodem::kill function

/*
 * @function {private} nodem::order
 * @summary Return data about the next global or local node at the same level
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from order call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> order(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  order enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(gtm_baton->result);

    if (is_number(data, gtm_baton->gtm_state)) {
        set_n(isolate, temp_object, new_string_n(isolate, "result"), Number::New(isolate, atof(gtm_baton->result)));
    } else {
        if (gtm_baton->gtm_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), new_string_n(isolate, gtm_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), GtmValue::from_byte(gtm_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  order JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  order exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"),
              get_n(isolate, temp_object, new_string_n(isolate, "result")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "result")));
        }
    } else {
        Local<Value> result = get_n(isolate, temp_object, new_string_n(isolate, "result"));

        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            set_n(isolate, new_subscripts, Number::New(isolate, new_subscripts->Length() - 1), result);
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), new_subscripts);
        }

        set_n(isolate, return_object, new_string_n(isolate, "result"), localize_name(result, gtm_baton->gtm_state));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  order exit");

    return scope.Escape(return_object);
} // @end nodem::order function

/*
 * @function {private} nodem::previous
 * @summary Return data about the previous global or local node at the same level
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from previous call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> previous(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  previous enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(gtm_baton->result);

    if (is_number(data, gtm_baton->gtm_state)) {
        set_n(isolate, temp_object, new_string_n(isolate, "result"), Number::New(isolate, atof(gtm_baton->result)));
    } else {
        if (gtm_baton->gtm_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), new_string_n(isolate, gtm_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), GtmValue::from_byte(gtm_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  previous JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  previous exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"),
              get_n(isolate, temp_object, new_string_n(isolate, "result")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "result")));
        }
    } else {
        Local<Value> result = get_n(isolate, temp_object, new_string_n(isolate, "result"));

        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            set_n(isolate, new_subscripts, Number::New(isolate, new_subscripts->Length() - 1), result);
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), new_subscripts);
        }

        set_n(isolate, return_object, new_string_n(isolate, "result"), localize_name(result, gtm_baton->gtm_state));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  previous exit");

    return scope.Escape(return_object);
} // @end nodem::previous function

/*
 * @function {private} nodem::next_node
 * @summary Return the next global or local node, depth first
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @member {gtm_char_t*} result - Data returned from next_node call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {vector<string>} {ydb only} subs_array - The subscripts of the next node
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> next_node(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  next_node enter");

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   status: ", gtm_baton->status);
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (gtm_baton->status == YDB_NODE_END) {
        if (gtm_baton->mode == STRICT) {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, 0));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, false));
        }
    } else {
        if (gtm_baton->mode == STRICT) {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, 1));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, true));
        }
    }

    if (gtm_baton->status != YDB_NODE_END) {
        string data(gtm_baton->result);

        if (is_number(data, gtm_baton->gtm_state)) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(gtm_baton->result)));
        } else {
            if (gtm_baton->gtm_state->utf8 == true) {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, gtm_baton->result));
            } else {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), GtmValue::from_byte(gtm_baton->result));
            }
        }
    }

    Local<Array> subs_array = Array::New(isolate);

    if (gtm_baton->subs_array.size()) {
        for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
            if (gtm_baton->gtm_state->debug > LOW)
                debug_log(">>   subs_array[", i, "]: ", gtm_baton->subs_array[i]);

            if (is_number(gtm_baton->subs_array[i], gtm_baton->gtm_state)) {
                set_n(isolate, subs_array, i, Number::New(isolate, atof(gtm_baton->subs_array[i].c_str())));
            } else {
                if (gtm_baton->gtm_state->utf8 == true) {
                    set_n(isolate, subs_array, i, new_string_n(isolate, gtm_baton->subs_array[i].c_str()));
                } else {
                    set_n(isolate, subs_array, i, GtmValue::from_byte((gtm_char_t*) gtm_baton->subs_array[i].c_str()));
                }
            }
        }

        set_n(isolate, temp_object, new_string_n(isolate, "subscripts"), subs_array);
    }
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  next_node JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  next_node exit");

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            if (temp_subs->IsUndefined()) {
                set_n(isolate, return_object, new_string_n(isolate, "result"), Array::New(isolate));
            } else {
                set_n(isolate, return_object, new_string_n(isolate, "result"), temp_subs);
            }

            return scope.Escape(return_object);
        } else {
            if (temp_subs->IsUndefined()) {
                return scope.Escape(Array::New(isolate));
            } else {
                return scope.Escape(temp_subs);
            }
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));
        if (!temp_subs->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = get_n(isolate, temp_object, new_string_n(isolate, "data"));
        if (!temp_data->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "data"), temp_data);

        set_n(isolate, return_object, new_string_n(isolate, "defined"),
          get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  next_node exit");

    return scope.Escape(return_object);
} // @end nodem::next_node function

/*
 * @function {private} nodem::previous_node
 * @summary Return the previous global or local node, depth first
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @member {gtm_char_t*} result - Data returned from previous_node call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {vector<string>} {ydb only} subs_array - The subscripts of the previous node
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> previous_node(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  previous_node enter");

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   status: ", gtm_baton->status);
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (gtm_baton->status == YDB_NODE_END) {
        if (gtm_baton->mode == STRICT) {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, 0));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, false));
        }
    } else {
        if (gtm_baton->mode == STRICT) {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, 1));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, true));
        }
    }

    if (gtm_baton->status != YDB_NODE_END) {
        string data(gtm_baton->result);

        if (is_number(data, gtm_baton->gtm_state)) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(gtm_baton->result)));
        } else {
            if (gtm_baton->gtm_state->utf8 == true) {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, gtm_baton->result));
            } else {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), GtmValue::from_byte(gtm_baton->result));
            }
        }
    }

    Local<Array> subs_array = Array::New(isolate);

    if (gtm_baton->subs_array.size()) {
        for (unsigned int i = 0; i < gtm_baton->subs_array.size(); i++) {
            if (gtm_baton->gtm_state->debug > LOW)
                debug_log(">>   subs_array[", i, "]: ", gtm_baton->subs_array[i]);

            if (is_number(gtm_baton->subs_array[i], gtm_baton->gtm_state)) {
                set_n(isolate, subs_array, i, Number::New(isolate, atof(gtm_baton->subs_array[i].c_str())));
            } else {
                if (gtm_baton->gtm_state->utf8 == true) {
                    set_n(isolate, subs_array, i, new_string_n(isolate, gtm_baton->subs_array[i].c_str()));
                } else {
                    set_n(isolate, subs_array, i, GtmValue::from_byte((gtm_char_t*) gtm_baton->subs_array[i].c_str()));
                }
            }
        }

        set_n(isolate, temp_object, new_string_n(isolate, "subscripts"), subs_array);
    }
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  previous_node JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    if (!get_n(isolate, temp_object, new_string_n(isolate, "status"))->IsUndefined())
        return scope.Escape(temp_object);

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  previous_node exit");

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            if (temp_subs->IsUndefined()) {
                set_n(isolate, return_object, new_string_n(isolate, "result"), Array::New(isolate));
            } else {
                set_n(isolate, return_object, new_string_n(isolate, "result"), temp_subs);
            }

            return scope.Escape(return_object);
        } else {
            if (temp_subs->IsUndefined()) {
                return scope.Escape(Array::New(isolate));
            } else {
                return scope.Escape(temp_subs);
            }
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));
        if (!temp_subs->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = get_n(isolate, temp_object, new_string_n(isolate, "data"));
        if (!temp_data->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "data"), temp_data);

        set_n(isolate, return_object, new_string_n(isolate, "defined"),
          get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  previous_node exit");

    return scope.Escape(return_object);
} // @end nodem::previous_node function

/*
 * @function {private} nodem::increment
 * @summary Return the value of an incremented or decremented number in a global or local node
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from increment call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> increment(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  increment enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(gtm_baton->result);

    if (is_number(data, gtm_baton->gtm_state)) {
        set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(gtm_baton->result)));
    } else {
        if (gtm_baton->gtm_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, gtm_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), GtmValue::from_byte(gtm_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  increment JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  increment exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"),
              get_n(isolate, temp_object, new_string_n(isolate, "data")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "data")));
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "data"), get_n(isolate, temp_object, new_string_n(isolate, "data")));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  increment exit");

    return scope.Escape(return_object);
} // @end nodem::increment function

/*
 * @function {private} nodem::lock
 * @summary Return data about an incremental lock of a global or local node
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from lock call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> lock(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  lock enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(gtm_baton->result);

    if (is_number(data, gtm_baton->gtm_state)) {
        set_n(isolate, temp_object, new_string_n(isolate, "result"), Number::New(isolate, atof(gtm_baton->result)));
    } else {
        if (gtm_baton->gtm_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), new_string_n(isolate, gtm_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), GtmValue::from_byte(gtm_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  lock JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  lock exit");

        Local<Value> result = get_n(isolate, temp_object, new_string_n(isolate, "result"));

        if (gtm_baton->gtm_state->mode == STRICT)
            result = to_string_n(isolate, result);

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), result);

            return scope.Escape(return_object);
        } else {
            return scope.Escape(result);
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (gtm_baton->gtm_state->mode == STRICT) {
            if (!subscripts->IsUndefined()) {
                Local<Value> temp_subscripts = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));

                if (!temp_subscripts->IsUndefined()) {
                    set_n(isolate, return_object, new_string_n(isolate, "subscripts"), temp_subscripts);
                } else {
                    set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);
                }
            }
        } else {
            if (!subscripts->IsUndefined())
                set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);
        }

        set_n(isolate, return_object, new_string_n(isolate, "result"),
          to_number_n(isolate, get_n(isolate, temp_object, new_string_n(isolate, "result"))));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  lock exit");

    return scope.Escape(return_object);
} // @end nodem::lock function

/*
 * @function {private} nodem::unlock
 * @summary Return data about unlocking a global or local node, or releasing all locks
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> unlock(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  unlock enter");

    Local<Value> subscripts = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", gtm_baton->gtm_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, gtm_baton->name.c_str());

    if (name->StrictEquals(new_string_n(isolate, "")) || gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  unlock exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), new_string_n(isolate, "0"));

            return scope.Escape(return_object);
        } else if (gtm_baton->gtm_state->mode == STRICT) {
            return scope.Escape(new_string_n(isolate, "0"));
        } else {
            Local<Value> ret_data = Number::New(isolate, 0);
            return scope.Escape(ret_data);
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, gtm_baton->gtm_state));
        }

        if (!subscripts->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "result"), Number::New(isolate, 0));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  unlock exit");

    return scope.Escape(return_object);
} // @end nodem::unlock function

/*
 * @function {private} nodem::function
 * @summary Return value from an arbitrary extrinsic function
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from function call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {string} relink - Whether to relink the function before calling it
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> function(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  function enter");

    Local<Value> arguments = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   result: ", gtm_baton->result);
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!arguments->IsUndefined()) {
            Local<Value> argument_string = json_method(arguments, "stringify", gtm_baton->gtm_state);
            debug_log(">>   arguments: ", *(UTF8_VALUE_TEMP_N(isolate, argument_string)));
        }

        debug_log(">>   relink: ", gtm_baton->relink);
    }

    Local<String> json_string;

    if (gtm_baton->gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, gtm_baton->result);
    } else {
        json_string = GtmValue::from_byte(gtm_baton->result);
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  function JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_baton->gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> function = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  function exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"),
              get_n(isolate, temp_object, new_string_n(isolate, "result")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "result")));
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        set_n(isolate, return_object, new_string_n(isolate, "function"), localize_name(function, gtm_baton->gtm_state));

        if (!arguments->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "arguments"), arguments);

        set_n(isolate, return_object, new_string_n(isolate, "result"),
          get_n(isolate, temp_object, new_string_n(isolate, "result")));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  function exit");

    return scope.Escape(return_object);
} // @end nodem::function function

/*
 * @function {private} nodem::procedure
 * @summary Return value from an arbitrary procedure/subroutine
 * @param {GtmBaton*} gtm_baton - struct containing the following members
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {string} relink - Whether to relink the procedure before calling it
 * @member {GtmState*} gtm_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRICT, STRING, or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> procedure(GtmBaton* gtm_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  procedure enter");

    Local<Value> arguments = Local<Value>::New(isolate, gtm_baton->arguments_p);

    if (gtm_baton->gtm_state->debug > LOW) {
        debug_log(">>   position: ", std::boolalpha, gtm_baton->position);
        debug_log(">>   local: ", std::boolalpha, gtm_baton->local);
        debug_log(">>   async: ", std::boolalpha, gtm_baton->async);
        debug_log(">>   name: ", gtm_baton->name);

        if (!arguments->IsUndefined()) {
            Local<Value> argument_string = json_method(arguments, "stringify", gtm_baton->gtm_state);
            debug_log(">>   arguments: ", *(UTF8_VALUE_TEMP_N(isolate, argument_string)));
        }

        debug_log(">>   relink: ", gtm_baton->relink);
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> procedure = new_string_n(isolate, gtm_baton->name.c_str());

    if (gtm_baton->position) {
        if (gtm_baton->gtm_state->debug > OFF)
            debug_log(">  procedure exit");

        if (gtm_baton->async && gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "result"), String::Empty(isolate));

            return scope.Escape(return_object);
        } else if (gtm_baton->gtm_state->mode == STRICT) {
            return scope.Escape(String::Empty(isolate));
        } else {
            Local<Value> ret_data = Undefined(isolate);
            return scope.Escape(ret_data);
        }
    } else {
        if (gtm_baton->gtm_state->mode == STRICT) {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (gtm_baton->routine) {
            set_n(isolate, return_object, new_string_n(isolate, "routine"), localize_name(procedure, gtm_baton->gtm_state));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "procedure"), localize_name(procedure, gtm_baton->gtm_state));
        }

        if (!arguments->IsUndefined())
            set_n(isolate, return_object, new_string_n(isolate, "arguments"), arguments);

        if (gtm_baton->gtm_state->mode == STRICT)
            set_n(isolate, return_object, new_string_n(isolate, "result"), String::Empty(isolate));
    }

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  procedure exit");

    return scope.Escape(return_object);
} // @end nodem::procedure function

#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
/*
 * @function {private} nodem::cleanup_gtm
 * @summary Delete heap resources after worker threads exit
 * @param {void*} class_name - The class instance to delete
 * @returns {void}
 */
inline static void cleanup_gtm(void* class_name)
{
  delete static_cast<Gtm*>(class_name);
  return;
} // @end nodem::cleanup_gtm

/*
 * @function {private} nodem::cleanup_gtm_state
 * @summary Delete heap resources after worker threads exit
 * @param {void*} class_name - The class instance to delete
 * @returns {void}
 */
inline static void cleanup_gtm_state(void* class_name)
{
  delete static_cast<GtmState*>(class_name);
  return;
} // @end nodem::cleanup_gtm_state
#endif

/*
 * @function nodem::async_work
 * @summary Call in to YottaDB/GT.M asynchronously, via a Node.js worker thread
 * @param {uv_work_t*} request - A pointer to the GtmBaton structure for transferring data between the main thread and worker thread
 * @returns {void}
 */
void async_work(uv_work_t* request)
{
    GtmBaton* gtm_baton = static_cast<GtmBaton*>(request->data);

    if (gtm_baton->gtm_state->debug > LOW)
        debug_log(">>   async_work enter");

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    gtm_baton->status = (*gtm_baton->gtm_function)(gtm_baton);

    if (gtm_baton->gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    if (gtm_baton->gtm_state->debug > LOW)
        debug_log(">>   async_work exit\n");

    return;
} // @end nodem::async_work function

/*
 * @function nodem::async_after
 * @summary Call in to the return functions, passing the data from YottaDB/GT.M, after receiving the data from the worker thread
 * @param {uv_work_t*} request - A pointer to the GtmBaton structure for transferring data between the main thread and worker thread
 * @returns {void}
 */
void async_after(uv_work_t* request, int status)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmBaton* gtm_baton = static_cast<GtmBaton*>(request->data);

    if (gtm_baton->gtm_state->debug > LOW)
        debug_log(">>   async_after enter: ", status);

    Local<Value> error_code = Null(isolate);
    Local<Value> return_object;
    Local<Value> error_object;

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->callback_p.Reset();
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        delete[] gtm_baton->error;
        delete[] gtm_baton->result;

        delete gtm_baton;

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK && gtm_baton->status != YDB_ERR_GVUNDEF && gtm_baton->status != YDB_ERR_LVUNDEF) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (gtm_baton->gtm_state->debug > LOW)
            debug_log(">>   " NODEM_DB " error code: ", gtm_baton->status);

        if (gtm_baton->gtm_state->mode == STRICT) {
            error_code = Number::New(isolate, 1);

            return_object = error_status(gtm_baton->error, gtm_baton->position, gtm_baton->async, gtm_baton->gtm_state);
        } else {
            error_object = error_status(gtm_baton->error, gtm_baton->position, gtm_baton->async, gtm_baton->gtm_state);

            error_code = Exception::Error(new_string_n(isolate,
              *(UTF8_VALUE_TEMP_N(isolate, get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "errorMessage"))))));

            set_n(isolate, ((Object*) *error_code), new_string_n(isolate, "ok"),
              get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "ok")));

            set_n(isolate, ((Object*) *error_code), new_string_n(isolate, "errorCode"),
              get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "errorCode")));

            set_n(isolate, ((Object*) *error_code), new_string_n(isolate, "errorMessage"),
              get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "errorMessage")));

            return_object = Undefined(isolate);
        }
    } else {
        return_object = (*gtm_baton->ret_function)(gtm_baton);

        if (gtm_baton->gtm_state->mode == STRICT)
            error_code = Number::New(isolate, 0);
    }

    Local<Value> argv[2] = {error_code, return_object};
    call_n(isolate, Local<Function>::New(isolate, gtm_baton->callback_p), Null(isolate), 2, argv);

    gtm_baton->callback_p.Reset();
    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    delete[] gtm_baton->error;
    delete[] gtm_baton->result;

    if (gtm_baton->gtm_state->debug > LOW)
        debug_log(">>   async_after exit\n");

    delete gtm_baton;

    return;
} // @end nodem::async_after function

// ***Begin Public APIs***

/*
 * @method nodem::Gtm::open
 * @summary Open a connection with YottaDB/GT.M
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::open(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->pid != gtm_state->tid) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection must be managed by main thread")));
        return;
    } else if (gtm_state_g == CLOSED) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection cannot be reopened")));
        return;
    } else if (gtm_state_g == OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection already open")));
        return;
    }

    char* relink = getenv("NODEM_AUTO_RELINK");

    if (relink != NULL)
        auto_relink_g = gtm_state->auto_relink = static_cast<bool>(atoi(relink));

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);

        if (has_n(isolate, arg_object, new_string_n(isolate, "debug"))) {
            UTF8_VALUE_N(isolate, debug, get_n(isolate, arg_object, new_string_n(isolate, "debug")));

            if (strcasecmp(*debug, "off") == 0) {
                debug_g = gtm_state->debug = OFF;
            } else if (strcasecmp(*debug, "low") == 0) {
                debug_g = gtm_state->debug = LOW;
            } else if (strcasecmp(*debug, "medium") == 0) {
                debug_g = gtm_state->debug = MEDIUM;
            } else if (strcasecmp(*debug, "high") == 0) {
                debug_g = gtm_state->debug = HIGH;
            } else {
                debug_g = gtm_state->debug = static_cast<debug_t>
                  (uint32_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "debug"))));

                if (gtm_state->debug < 0) {
                    debug_g = gtm_state->debug = OFF;
                } else if (gtm_state->debug > 3) {
                    debug_g = gtm_state->debug = HIGH;
                }
            }
        }

        if (gtm_state->debug > OFF) {
            debug_log(">  Gtm::open enter");

            char* debug_display = (char*) "off";

            if (gtm_state->debug == LOW) {
                debug_display = (char*) "low";
            } else if (gtm_state->debug == MEDIUM) {
                debug_display = (char*) "medium";
            } else if (gtm_state->debug >= HIGH) {
                debug_display = (char*) "high";
            }

            debug_log(">>   debug: ", debug_display);
        }

        Local<Value> global_directory = get_n(isolate, arg_object, new_string_n(isolate, "globalDirectory"));

        if (global_directory->IsUndefined())
            global_directory = get_n(isolate, arg_object, new_string_n(isolate, "namespace"));

        if (!global_directory->IsUndefined() && global_directory->IsString()) {
            if (gtm_state->debug > LOW)
                debug_log(">>   globalDirectory: ", *(UTF8_VALUE_TEMP_N(isolate, global_directory)));

#if NODEM_SIMPLE_API == 1
            if (setenv("ydb_gbldir", *(UTF8_VALUE_TEMP_N(isolate, global_directory)), 1) == -1) {
#else
            if (setenv("gtmgbldir", *(UTF8_VALUE_TEMP_N(isolate, global_directory)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        Local<Value> routines_path = get_n(isolate, arg_object, new_string_n(isolate, "routinesPath"));

        if (!routines_path->IsUndefined() && routines_path->IsString()) {
            if (gtm_state->debug > LOW)
                debug_log(">>   routinesPath: ", *(UTF8_VALUE_TEMP_N(isolate, routines_path)));

#if NODEM_SIMPLE_API == 1
            if (setenv("ydb_routines", *(UTF8_VALUE_TEMP_N(isolate, routines_path)), 1) == -1) {
#else
            if (setenv("gtmroutines", *(UTF8_VALUE_TEMP_N(isolate, routines_path)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        Local<Value> callin_table = get_n(isolate, arg_object, new_string_n(isolate, "callinTable"));

        if (!callin_table->IsUndefined() && callin_table->IsString()) {
            if (gtm_state->debug > LOW)
                debug_log(">>   callinTable: ", *(UTF8_VALUE_TEMP_N(isolate, callin_table)));

#if NODEM_SIMPLE_API == 1
            if (setenv("ydb_ci", *(UTF8_VALUE_TEMP_N(isolate, callin_table)), 1) == -1) {
#else
            if (setenv("GTMCI", *(UTF8_VALUE_TEMP_N(isolate, callin_table)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        Local<Value> addr = get_n(isolate, arg_object, new_string_n(isolate, "ipAddress"));
        const char* addrMsg;

        if (addr->IsUndefined()) {
            addr = get_n(isolate, arg_object, new_string_n(isolate, "ip_address"));
            addrMsg = "ip_address must be a string";
        } else {
            addrMsg = "ipAddress must be a string";
        }

        Local<Value> port = get_n(isolate, arg_object, new_string_n(isolate, "tcpPort"));
        const char* portMsg;

        if (port->IsUndefined()) {
            port = get_n(isolate, arg_object, new_string_n(isolate, "tcp_port"));
            portMsg = "tcp_port must be a number or string";
        } else {
            portMsg = "tcpPort must be a number or string";
        }

        if (!addr->IsUndefined() || !port->IsUndefined()) {
            Local<Value> gtcm_nodem;

            if (addr->IsUndefined())
                addr = Local<Value>::New(isolate, new_string_n(isolate, "127.0.0.1"));

            if (!addr->IsString()) {
                isolate->ThrowException(Exception::TypeError(new_string_n(isolate, addrMsg)));
                return;
            }

            if (port->IsUndefined())
                port = Local<Value>::New(isolate, new_string_n(isolate, "6789"));

            if (port->IsNumber() || port->IsString()) {
                Local<String> gtcm_port = concat_n(isolate, new_string_n(isolate, ":"), to_string_n(isolate, port));
                gtcm_nodem = concat_n(isolate, to_string_n(isolate, addr), gtcm_port);
            } else {
                isolate->ThrowException(Exception::TypeError(new_string_n(isolate, portMsg)));
                return;
            }

#if NODEM_SIMPLE_API == 1
            if (gtm_state->debug > LOW)
                debug_log(">>   ydb_cm_NODEM: ", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)));

            if (setenv("ydb_cm_NODEM", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)), 1) == -1) {
#else
            if (gtm_state->debug > LOW)
                debug_log(">>   GTCM_NODEM: ", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)));

            if (setenv("GTCM_NODEM", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink")))
            auto_relink_g = gtm_state->auto_relink =
              boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));

        if (gtm_state->debug > LOW)
            debug_log(">>   autoRelink: ", std::boolalpha, gtm_state->auto_relink);

        UTF8_VALUE_N(isolate, nodem_mode, get_n(isolate, arg_object, new_string_n(isolate, "mode")));

        if (strcasecmp(*nodem_mode, "strict") == 0) {
            mode_g = gtm_state->mode = STRICT;

            if (gtm_state->debug > LOW)
                debug_log(">>   mode: strict");
        } else if (strcasecmp(*nodem_mode, "string") == 0) {
            mode_g = gtm_state->mode = STRING;

            if (gtm_state->debug > LOW)
                debug_log(">>   mode: string");
        } else if (strcasecmp(*nodem_mode, "canonical") == 0) {
            mode_g = gtm_state->mode = CANONICAL;

            if (gtm_state->debug > LOW)
                debug_log(">>   mode: canonical");
        } else if (gtm_state->debug > LOW) {
            if (gtm_state->mode == STRICT) {
                debug_log(">>   mode: strict");
            } else if (gtm_state->mode == STRING) {
                debug_log(">>   mode: string");
            } else {
                debug_log(">>   mode: canonical");
            }
        }

        Local<Value> charset = get_n(isolate, arg_object, new_string_n(isolate, "charset"));

        if (charset->IsUndefined())
            charset = get_n(isolate, arg_object, new_string_n(isolate, "encoding"));

        UTF8_VALUE_N(isolate, data_charset, charset);

        if (strcasecmp(*data_charset, "m") == 0 || strcasecmp(*data_charset, "binary") == 0 ||
          strcasecmp(*data_charset, "ascii") == 0) {
            utf8_g = gtm_state->utf8 = false;
        } else if (strcasecmp(*data_charset, "utf-8") == 0 || strcasecmp(*data_charset, "utf8") == 0) {
            utf8_g = gtm_state->utf8 = true;
        }

        if (gtm_state->debug > LOW) {
            char* encoding = (char*) "utf-8";

            if (gtm_state->utf8 == false)
                encoding = (char*) "m";

            debug_log(">>   charset: ", encoding);
        }

        if (has_n(isolate, arg_object, new_string_n(isolate, "signalHandler"))) {
            if (get_n(isolate, arg_object, new_string_n(isolate, "signalHandler"))->IsObject()) {
                Local<Object> signal_handlers =
                  to_object_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "signalHandler")));

                if (has_n(isolate, signal_handlers, new_string_n(isolate, "sigint"))) {
                    signal_sigint_g = boolean_value_n(isolate, get_n(isolate, signal_handlers, new_string_n(isolate, "sigint")));
                } else if (has_n(isolate, signal_handlers, new_string_n(isolate, "SIGINT"))) {
                    signal_sigint_g = boolean_value_n(isolate, get_n(isolate, signal_handlers, new_string_n(isolate, "SIGINT")));
                }

                if (has_n(isolate, signal_handlers, new_string_n(isolate, "sigquit"))) {
                    signal_sigquit_g = boolean_value_n(isolate, get_n(isolate, signal_handlers, new_string_n(isolate, "sigquit")));
                } else if (has_n(isolate, signal_handlers, new_string_n(isolate, "SIGQUIT"))) {
                    signal_sigquit_g = boolean_value_n(isolate, get_n(isolate, signal_handlers, new_string_n(isolate, "SIGQUIT")));
                }

                if (has_n(isolate, signal_handlers, new_string_n(isolate, "sigterm"))) {
                    signal_sigterm_g = boolean_value_n(isolate, get_n(isolate, signal_handlers, new_string_n(isolate, "sigterm")));
                } else if (has_n(isolate, signal_handlers, new_string_n(isolate, "SIGTERM"))) {
                    signal_sigterm_g = boolean_value_n(isolate, get_n(isolate, signal_handlers, new_string_n(isolate, "SIGTERM")));
                }
            } else {
                Local<Value> signal_handlers = get_n(isolate, arg_object, new_string_n(isolate, "signalHandler"));

                signal_sigint_g = signal_sigquit_g = signal_sigterm_g = boolean_value_n(isolate, signal_handlers);
            }

            if (gtm_state->debug > LOW) {
                debug_log(">>   sigint: ", std::boolalpha, signal_sigint_g);
                debug_log(">>   sigquit: ", std::boolalpha, signal_sigquit_g);
                debug_log(">>   sigterm: ", std::boolalpha, signal_sigterm_g);
            }
        }

        Local<Value> threadpool_size = get_n(isolate, arg_object, new_string_n(isolate, "threadpoolSize"));

        if (!threadpool_size->IsUndefined() && threadpool_size->IsNumber()) {
            if (gtm_state->debug > LOW)
                debug_log(">>   threadpoolSize: ", *(UTF8_VALUE_TEMP_N(isolate, threadpool_size)));

            if (setenv("UV_THREADPOOL_SIZE", *(UTF8_VALUE_TEMP_N(isolate, threadpool_size)), 1) == -1) {
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }
    }

    if (signal_sigint_g == true) {
        if (getenv("ydb_nocenable") != NULL) {
            if (setenv("ydb_nocenable", "0", 1) == -1) {
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        if (getenv("gtm_nocenable") != NULL) {
            if (setenv("gtm_nocenable", "0", 1) == -1) {
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }
    }

    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDOUT_FILENO)) {
        if (tcgetattr(STDOUT_FILENO, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDERR_FILENO)) {
        if (tcgetattr(STDERR_FILENO, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    }

    if (signal_sigint_g == true) {
        if (sigaction(SIGINT, NULL, &gtm_state->signal_attr) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot retrieve SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, NULL, &gtm_state->signal_attr) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot retrieve SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, NULL, &gtm_state->signal_attr) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot retrieve SIGTERM handler")));
            return;
        }
    }

    if ((save_stdout_g = dup(STDOUT_FILENO)) == -1) {
        char error[BUFSIZ];
        cerr << strerror_r(errno, error, BUFSIZ);
    }

    uv_mutex_lock(&mutex_g);

    if (gtm_state->debug > LOW) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }

        flockfile(stderr);
    }

#if NODEM_SIMPLE_API == 1
    if (ydb_init() != YDB_OK) {
#else
    if (gtm_init() != EXIT_SUCCESS) {
#endif
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        if (gtm_state->debug > LOW) {
            funlockfile(stderr);

            if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }
        }

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    gtm_state_g = OPEN;

    if (gtm_state->debug > LOW) {
        funlockfile(stderr);

        if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    uv_mutex_unlock(&mutex_g);

    struct sigaction signal_attr;

    if (signal_sigint_g == true || signal_sigquit_g == true || signal_sigterm_g == true) {
        signal_attr.sa_handler = clean_shutdown;
        signal_attr.sa_flags = 0;

        if (sigfillset(&signal_attr.sa_mask) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot set mask for signal handlers")));
            return;
        }
    }

    if (signal_sigint_g == true) {
        if (sigaction(SIGINT, &signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, &signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, &signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGTERM handler")));
            return;
        }
    }

    gtm_status_t stat_buf;

    uv_mutex_lock(&mutex_g);

    gtm_char_t debug[] = "debug";

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = debug;
    access.rtn_name.length = strlen(debug);
    access.handle = NULL;

    stat_buf = gtm_cip(&access, gtm_state->debug);
#else
    stat_buf = gtm_ci(debug, gtm_state->debug);
#endif

    if (gtm_state->debug > LOW)
        debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    uv_mutex_unlock(&mutex_g);

    Local<Object> result = Object::New(isolate);

    if (gtm_state->mode == STRICT) {
        set_n(isolate, result, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        set_n(isolate, result, new_string_n(isolate, "result"), Number::New(isolate, 1));
        set_n(isolate, result, new_string_n(isolate, "gtm_pid"), to_string_n(isolate, Number::New(isolate, gtm_state->pid)));
        set_n(isolate, result, new_string_n(isolate, "gtm_tid"), to_string_n(isolate, Number::New(isolate, gtm_state->tid)));
    } else {
        set_n(isolate, result, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        set_n(isolate, result, new_string_n(isolate, "pid"), Number::New(isolate, gtm_state->pid));
        set_n(isolate, result, new_string_n(isolate, "tid"), Number::New(isolate, gtm_state->tid));
    }

    info.GetReturnValue().Set(result);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::open exit\n");

    return;
} // @end nodem::Gtm::open method

/*
 * @method nodem::Gtm::configure
 * @summary Configure per-thread parameters of the YottaDB/GT.M connection
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::configure(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    if (info.Length() >= 1 && !info[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = Object::New(isolate);

    if (info[0]->IsObject())
        arg_object = to_object_n(isolate, info[0]);

    if (has_n(isolate, arg_object, new_string_n(isolate, "debug"))) {
        UTF8_VALUE_N(isolate, debug, get_n(isolate, arg_object, new_string_n(isolate, "debug")));

        if (strcasecmp(*debug, "off") == 0) {
            gtm_state->debug = OFF;
        } else if (strcasecmp(*debug, "low") == 0) {
            gtm_state->debug = LOW;
        } else if (strcasecmp(*debug, "medium") == 0) {
            gtm_state->debug = MEDIUM;
        } else if (strcasecmp(*debug, "high") == 0) {
            gtm_state->debug = HIGH;
        } else {
            gtm_state->debug = static_cast<debug_t>
              (uint32_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "debug"))));

            if (gtm_state->debug < 0) {
                gtm_state->debug = OFF;
            } else if (gtm_state->debug > 3) {
                gtm_state->debug = HIGH;
            }
        }
    }

    if (gtm_state->debug > OFF) {
        debug_log(">  Gtm::configure enter");

        char* debug_display = (char*) "off";

        if (gtm_state->debug == LOW) {
            debug_display = (char*) "low";
        } else if (gtm_state->debug == MEDIUM) {
            debug_display = (char*) "medium";
        } else if (gtm_state->debug >= HIGH) {
            debug_display = (char*) "high";
        }

        debug_log(">>   debug: ", debug_display);
    }

    if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink")))
        gtm_state->auto_relink = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));

    if (gtm_state->debug > LOW)
        debug_log(">>   autoRelink: ", std::boolalpha, gtm_state->auto_relink);

    if (has_n(isolate, arg_object, new_string_n(isolate, "mode"))) {
        UTF8_VALUE_N(isolate, nodem_mode, get_n(isolate, arg_object, new_string_n(isolate, "mode")));

        if (strcasecmp(*nodem_mode, "strict") == 0) {
            gtm_state->mode = STRICT;
        } else if (strcasecmp(*nodem_mode, "string") == 0) {
            gtm_state->mode = STRING;
        } else if (strcasecmp(*nodem_mode, "canonical") == 0) {
            gtm_state->mode = CANONICAL;
        }
    }

    if (gtm_state->debug > LOW) {
        if (gtm_state->mode == STRICT) {
            debug_log(">>   mode: strict");
        } else if (gtm_state->mode == STRING) {
            debug_log(">>   mode: string");
        } else {
            debug_log(">>   mode: canonical");
        }
    }

    if (has_n(isolate, arg_object, new_string_n(isolate, "charset"))) {
        Local<Value> charset = get_n(isolate, arg_object, new_string_n(isolate, "charset"));
        UTF8_VALUE_N(isolate, data_charset, charset);

        if (strcasecmp(*data_charset, "m") == 0 || strcasecmp(*data_charset, "binary") == 0 ||
          strcasecmp(*data_charset, "ascii") == 0) {
            gtm_state->utf8 = false;
        } else if (strcasecmp(*data_charset, "utf-8") == 0 || strcasecmp(*data_charset, "utf8") == 0) {
            gtm_state->utf8 = true;
        }
    } else if (has_n(isolate, arg_object, new_string_n(isolate, "encoding"))) {
        Local<Value> encoding = get_n(isolate, arg_object, new_string_n(isolate, "encoding"));
        UTF8_VALUE_N(isolate, data_encoding, encoding);

        if (strcasecmp(*data_encoding, "m") == 0 || strcasecmp(*data_encoding, "binary") == 0 ||
          strcasecmp(*data_encoding, "ascii") == 0) {
            gtm_state->utf8 = false;
        } else if (strcasecmp(*data_encoding, "utf-8") == 0 || strcasecmp(*data_encoding, "utf8") == 0) {
            gtm_state->utf8 = true;
        }
    }

    if (gtm_state->debug > LOW) {
        char* charset = (char*) "utf-8";

        if (gtm_state->utf8 == false)
            charset = (char*) "m";

        debug_log(">>   charset: ", charset);
    }

    if (has_n(isolate, arg_object, new_string_n(isolate, "debug"))) {
        gtm_status_t stat_buf;

        uv_mutex_lock(&mutex_g);

        gtm_char_t debug[] = "debug";

#if NODEM_CIP_API == 1
        ci_name_descriptor access;

        access.rtn_name.address = debug;
        access.rtn_name.length = strlen(debug);
        access.handle = NULL;

        stat_buf = gtm_cip(&access, gtm_state->debug);
#else
        stat_buf = gtm_ci(debug, gtm_state->debug);
#endif

        if (gtm_state->debug > LOW)
            debug_log(">>   stat_buf: ", stat_buf);

        if (stat_buf != EXIT_SUCCESS) {
            gtm_char_t msg_buf[ERR_LEN];
            gtm_zstatus(msg_buf, ERR_LEN);

            uv_mutex_unlock(&mutex_g);

            info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
            return;
        }

        uv_mutex_unlock(&mutex_g);
    }

    Local<Object> result = Object::New(isolate);

    if (gtm_state->mode == STRICT) {
        set_n(isolate, result, new_string_n(isolate, "ok"), Number::New(isolate, 1));
        set_n(isolate, result, new_string_n(isolate, "result"), Number::New(isolate, 1));
        set_n(isolate, result, new_string_n(isolate, "gtm_pid"), to_string_n(isolate, Number::New(isolate, gtm_state->pid)));
        set_n(isolate, result, new_string_n(isolate, "gtm_tid"), to_string_n(isolate, Number::New(isolate, gtm_state->tid)));
    } else {
        set_n(isolate, result, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
        set_n(isolate, result, new_string_n(isolate, "pid"), Number::New(isolate, gtm_state->pid));
        set_n(isolate, result, new_string_n(isolate, "tid"), Number::New(isolate, gtm_state->tid));
    }

    info.GetReturnValue().Set(result);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::configure exit\n");

    return;
} // @end nodem::Gtm::configure method

/*
 * @method nodem::Gtm::close
 * @summary Close a connection with YottaDB/GT.M
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::close(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::close enter");

    if (gtm_state->pid != gtm_state->tid) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection must be managed by main thread")));
        return;
    } else if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    uv_mutex_lock(&mutex_g);

    if (info[0]->IsObject() && has_n(isolate, to_object_n(isolate, info[0]), new_string_n(isolate, "resetTerminal"))) {
        reset_term_g = boolean_value_n(isolate,
          get_n(isolate, to_object_n(isolate, info[0]), new_string_n(isolate, "resetTerminal")));
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   resetTerminal: ", std::boolalpha, reset_term_g);

#if NODEM_SIMPLE_API == 1
    if (ydb_exit() != YDB_OK) {
#else
    if (gtm_exit() != EXIT_SUCCESS) {
#endif
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    gtm_state_g = CLOSED;

    if (unistd_close(save_stdout_g) == -1) {
        char error[BUFSIZ];
        cerr << strerror_r(errno, error, BUFSIZ);
    }

    uv_mutex_unlock(&mutex_g);

    if (signal_sigint_g == true) {
        if (sigaction(SIGINT, &gtm_state->signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, &gtm_state->signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, &gtm_state->signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGTERM handler")));
            return;
        }
    }

    if (reset_term_g == true) {
        term_attr_g.c_iflag |= ICRNL;
        term_attr_g.c_lflag |= (ISIG | ECHO);
    }

    if (isatty(STDIN_FILENO)) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDOUT_FILENO)) {
        if (tcsetattr(STDOUT_FILENO, TCSANOW, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDERR_FILENO)) {
        if (tcsetattr(STDERR_FILENO, TCSANOW, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    }

    if (gtm_state->mode == STRICT) {
        info.GetReturnValue().Set(new_string_n(isolate, "1"));
    } else {
        info.GetReturnValue().Set(Undefined(isolate));
    }

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::close exit\n");

    return;
} // @end nodem::Gtm::close method

/*
 * @method nodem::Gtm::help
 * @summary Built-in help menu for Gtm methods
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::help(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "open"))) {
        cout << "open method:\n"
            "\tOpen connection to " NODEM_DB " - all methods, except for help and version, require an open connection\n"
            "\n\tRequired arguments: {None}\n"
            "\n\tOptional arguments:\n"
            "\t{\n"
            "\t\tglobalDirectory|namespace:\t{string} <none>,\n"
            "\t\troutinesPath:\t\t\t{string} <none>,\n"
            "\t\tcallinTable:\t\t\t{string} <none>,\n"
            "\t\tipAddress|ip_address:\t\t{string} <none>,\n"
            "\t\ttcpPort|tcp_port:\t\t{number|string} <none>,\n"
            "\t\tcharset|encoding:\t\t{string} [<utf8|utf-8>|m|binary|ascii]/i,\n"
            "\t\tmode:\t\t\t\t{string} [<canonical>|string|strict]/i,\n"
            "\t\tautoRelink:\t\t\t{boolean} <false>,\n"
            "\t\tdebug:\t\t\t\t{boolean} <false>|{string} [<off>|low|medium|high]/i|{number} [<0>|1|2|3],\n"
            "\t\tthreadpoolSize:\t\t\t{number} [1-1024] <4>,\n"
            "\t\tsignalHandler:\t\t\t{boolean} <true>|{object {boolean} sigint,sigterm,sigquit/i} [<true>|false] [<1>|0]\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tresult:\t\t{optional} {number} 1,\n"
            "\t\tpid|gtm_pid:\t{number}|{string},\n"
            "\t\ttid|gtm_tid:\t{number}|{string}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\t- Failures from bad environment set-ups result in internal errors from " NODEM_DB "\n"
            "\n\tFor more information about the open method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "configure"))) {
        cout << "configure method:\n"
            "\tConfigure per-thread parameters of the connection to " NODEM_DB "\n"
            "\n\tRequired arguments: {None}\n"
            "\n\tOptional arguments:\n"
            "\t{\n"
            "\t\tcharset|encoding:\t{string} [<utf8|utf-8>|m|binary|ascii]/i,\n"
            "\t\tmode:\t\t\t{string} [<canonical>|string|strict]/i,\n"
            "\t\tautoRelink:\t\t{boolean} <false>,\n"
            "\t\tdebug:\t\t\t{boolean} <false>|{string} [<off>|low|medium|high]/i|{number} [<0>|1|2|3]\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tresult:\t\t{optional} {number} 1,\n"
            "\t\tpid|gtm_pid:\t{number}|{string},\n"
            "\t\ttid|gtm_tid:\t{number}|{string}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\t- Failures from bad environment set-ups result in internal errors from " NODEM_DB "\n"
            "\n\tFor more information about the configure method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "close"))) {
        cout << "close method:\n"
            "\tClose connection to " NODEM_DB " - once closed, cannot be reopened during the current process\n"
            "\n\tRequired arguments: {None}\n"
            "\n\tOptional arguments:\n"
            "\t{\n"
            "\t\tresetTerminal: {boolean} <false>\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{undefined}|{string} 1\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\n\tFor more information about the close method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "version"))) {
        cout << "version or about method:\n"
            "\tDisplay version data - includes " NODEM_DB " version if connection has been established\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments: {None}\n"
            "\n\tReturns on success: {string}\n"
            "\n\tReturns on failure: Should never fail\n"
            "\n\tFor more information about the version/about method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "data"))) {
        cout << "data method:\n"
            "\tDisplay information about the existence of data and/or children in global or local variables\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tdefined:\t{number} [0|1|10|11]\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{number} [0|1|10|11]\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the data method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "get"))) {
        cout << "get method:\n"
            "\tRetrieve the data stored at a global or local node, or intrinsic special variable (ISV)\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tdata:\t\t{string|number},\n"
            "\t\tdefined:\t{boolean} [false|true]|{number} [0|1]\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|$ISV|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{string|number}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the get method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "set"))) {
        cout << "set method:\n"
            "\tStore data in a global or local node, or intrinsic special variable (ISV)\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tdata:\t\t{required} {string|number}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tdata:\t\t{string|number},\n"
            "\t\tresult:\t\t{optional} {number} 0\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|$ISV|local, [subscripts+], data\n"
            "\n\tReturns on success:\n"
            "\t{boolean} true|{number} 0\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the set method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "kill"))) {
        cout << "kill method:\n"
            "\tRemove data stored in a global or global node, or in a local or local node, or remove all local variables\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tRequired arguments: {None} - Without an argument, will clear the entire local symbol table for that process\n"
            "\tReturns on success: {undefined}|{number} 0\n"
            "\n\tOptional arguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tnodeOnly:\t{optional} {boolean} <false>|{number} [<0>|1]\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tresult:\t\t{optional} {number} 0\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{boolean} true|{number} 0\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the kill method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "merge"))) {
        cout << "merge method:\n"
            "\tCopy an entire data tree, or sub-tree, from a global or local array, to another global or local array\n"
            "\n\tRequired arguments:\n"
            "\t{\n"
            "\t\tfrom: {\n"
            "\t\t\tglobal|local:\t{required} {string},\n"
            "\t\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t\t},\n"
            "\t\tto: {\n"
            "\t\t\tglobal|local:\t{required} {string},\n"
            "\t\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t\t}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t{boolean} true|{number} 1,\n"
            "\t\tfrom: {\n"
            "\t\t\tglobal|local:\t{string},\n"
            "\t\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t\t},\n"
            "\t\tto: {\n"
            "\t\t\tglobal|local:\t{string},\n"
            "\t\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t\t},\n"
            "\t\tresult:\t{number} 1\n"
            "\t}\n"
            "\tOR:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tresult:\t\t{string} 1\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the merge method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "order"))) {
        cout << "order or next method:\n"
            "\tRetrieve the next node, at the current subscript depth\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tresult:\t\t{string|number}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{string|number}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the order/next method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "previous"))) {
        cout << "previous method:\n"
            "\tRetrieve the previous node, at the current subscript depth\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tresult:\t\t{string|number}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{string|number}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the previous method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "nextNode"))) {
        cout << "nextNode or next_node method:\n"
            "\tRetrieve the next node, regardless of subscript depth\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tdata:\t\t{string|number},\n"
            "\t\tdefined:\t{boolean} [false|true]|{number} [0|1]\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{array {string|number}}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the nextNode/next_node method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "previousNode"))) {
        cout << "previousNode or previous_node method:\n"
            "\tRetrieve the previous node, regardless of subscript depth\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tdata:\t\t{string|number},\n"
            "\t\tdefined:\t{boolean} [false|true]|{number} [0|1]\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{array {string|number}}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the previousNode/previous_node method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "increment"))) {
        cout << "increment method:\n"
            "\tAtomically increment or decrement a global or local data node\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tincrement:\t{optional} {number} <1>\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tdata:\t\t{string|number}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{string|number}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the increment method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "lock"))) {
        cout << "lock method:\n"
            "\tLock a local or global tree, or individual node, incrementally - locks are advisory, not mandatory\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\ttimeout:\t{optional} {number} <-1>\n"
            "\t}\n"
            "\n\tOptional arguments: Timeout {number} <-1> as second argument\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tresult:\t\t{number} [0|1]\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{string|number} [0|1]\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the lock method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "unlock"))) {
        cout << "unlock method:\n"
            "\tUnlock a local or global tree, or individual node, incrementally; or release all locks held by process\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tRequired arguments: {None} - Without an argument, will clear the entire lock table for that process\n"
            "\tReturns on success: {undefined}|{number} 0\n"
            "\n\tOptional arguments - via object:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tresult:\t\t{optional} {number} 0\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\t^global|local, [subscripts+]\n"
            "\n\tReturns on success:\n"
            "\t{boolean} true|{string} 0\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the unlock method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "function"))) {
        cout << "function method:\n"
            "\tCall an extrinsic function in " NODEM_DB " code\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tfunction:\t{required} {string},\n"
            "\t\targuments:\t{optional} {array {string|number|empty}},\n"
            "\t\tautoRelink:\t{optional} {boolean} <false>\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tfunction:\t{string},\n"
            "\t\targuments:\t{optional} {array {string|number|empty}},\n"
            "\t\tresult:\t\t{string|number}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\tfunction, [arguments+]\n"
            "\n\tReturns on success:\n"
            "\t{string|number}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the function method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "procedure"))) {
        cout << "procedure or routine method:\n"
            "\tCall a procedure/routine/subroutine label in " NODEM_DB " code\n"
            "\tPassing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            "\n\tArguments - via object:\n"
            "\t{\n"
            "\t\tprocedure|routine:\t{required} {string},\n"
            "\t\targuments:\t\t{optional} {array {string|number|empty}},\n"
            "\t\tautoRelink:\t\t{optional} {boolean} <false>\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t\t{boolean} true|{number} 1,\n"
            "\t\tprocedure|routine:\t{string},\n"
            "\t\targuments:\t\t{optional} {array {string|number|empty}},\n"
            "\t\tresult:\t\t\t{optional} {string} ''\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\n\tArguments - via argument position:\n"
            "\tprocedure, [arguments+]\n"
            "\n\tReturns on success:\n"
            "\t{undefined}|{string} ''\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the procedure/routine method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "globalDirectory"))) {
        cout << "globalDirectory or global_directory method:\n"
            "\tList globals stored in the database\n"
            "\n\tRequired arguments: {None} - Without an argument, will list all the globals stored in the database\n"
            "\n\tOptional arguments:\n"
            "\t{\n"
            "\t\tmax:\t{optional} {number},\n"
            "\t\tlo:\t{optional} {string},\n"
            "\t\thi:\t{optional} {string}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t[\n"
            "\t\t<global name>* {string}\n"
            "\t]\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the globalDirectory/global_directory method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "localDirectory"))) {
        cout << "localDirectory or local_directory method:\n"
            "\tList local variables stored in the symbol table\n"
            "\n\tRequired arguments: {None} - Without an argument, will list all the variables in the local symbol table\n"
            "\n\tOptional arguments:\n"
            "\t{\n"
            "\t\tmax:\t{optional} {number},\n"
            "\t\tlo:\t{optional} {string},\n"
            "\t\thi:\t{optional} {string}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t[\n"
            "\t\t<local variable name>* {string}\n"
            "\t]\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the localDirectory/local_directory method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "retrieve"))) {
        cout << "retrieve method:\n"
            "\tRetrieve a local or global tree or sub-tree structure as an object - NOT YET IMPLEMENTED\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "update"))) {
        cout << "update method:\n"
            "\tStore an object in a local or global tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            << endl;
    } else {
#if NODEM_YDB == 1
        cout << "NodeM: Ydb/Gtm Object API Help Menu - Methods:\n"
#else
        cout << "NodeM: Gtm Object API Help Menu - Methods:\n"
#endif
            "\nopen\t\tOpen connection to " NODEM_DB " - all methods, except for help and version, require an open connection\n"
            "configure\tConfigure per-thread parameters of the connection to " NODEM_DB "\n"
            "close\t\tClose connection to " NODEM_DB " - once closed, cannot be reopened during the current process\n"
            "version\t\tDisplay version data - includes " NODEM_DB " version if connection has been established (AKA about)\n"
            "data\t\tDisplay information about the existence of data and/or children in global or local variables\n"
            "get\t\tRetrieve the data stored at a global or local node, or intrinsic special variable (ISV)\n"
            "set\t\tStore data in a global or local node, or intrinsic special variable (ISV)\n"
            "kill\t\tRemove data stored in a global or global node, or in a local or local node; or remove all local variables\n"
            "merge\t\tCopy an entire data tree, or sub-tree, from a global or local array, to another global or local array\n"
            "order\t\tRetrieve the next node, at the current subscript depth (AKA next)\n"
            "previous\tRetrieve the previous node, at the current subscript depth\n"
            "nextNode\tRetrieve the next node, regardless of subscript depth\n"
            "previousNode\tRetrieve the previous node, regardless of subscript depth\n"
            "increment\tAtomically increment a global or local data node\n"
            "lock\t\tLock a global or local tree, or individual node, incrementally - locks are advisory, not mandatory\n"
            "unlock\t\tUnlock a global or local tree, or individual node, incrementally; or release all locks held by process\n"
            "function\tCall an extrinsic function in " NODEM_DB " code\n"
            "procedure\tCall a procedure/routine/subroutine label in " NODEM_DB " code (AKA routine)\n"
            "globalDirectory\tList globals stored in the database\n"
            "localDirectory\tList local variables stored in the symbol table\n"
            "retrieve\tRetrieve a global or local tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            "update\t\tStore an object in a global or local tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            "\nFor more information about each method, call help with the method name as an argument\n"
            << endl;
    }

    info.GetReturnValue().Set(new_string_n(isolate, "NodeM - Copyright (C) 2012-2020 Fourth Watch Software LC"));
    return;
} // @end nodem::Gtm::help method

/*
 * @method nodem::Gtm::version
 * @summary Return the about/version string
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::version(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::version enter");

    bool async = false;

    if (info[0]->IsFunction())
        async = true;

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[0]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, Undefined(isolate));
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = NODEM_VERSION;
    gtm_baton->async = async;
    gtm_baton->status = 0;
    gtm_baton->gtm_function = &gtm::version;
    gtm_baton->ret_function = &nodem::version;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into ", NODEM_DB);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::version exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    gtm_baton->status = gtm::version(gtm_baton);

    if (gtm_state->debug > OFF)
        debug_log(">  return from ", NODEM_DB);

    if (gtm_baton->status != EXIT_SUCCESS) {
        isolate->ThrowException(Exception::Error(
          to_string_n(isolate, error_status(gtm_baton->error, true, async, gtm_state))));

        info.GetReturnValue().Set(Undefined(isolate));

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into version");

    Local<Value> return_value = nodem::version(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_value);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::version exit\n");

    return;
} // @end nodem::Gtm::version

/*
 * @method nodem::Gtm::data
 * @summary Check if global or local node has data and/or children or not
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::data(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::data enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::data;
#else
    gtm_baton->gtm_function = &gtm::data;
#endif
    gtm_baton->ret_function = &nodem::data;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::data exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::data(gtm_baton);
#else
    gtm_baton->status = gtm::data(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into data");

    Local<Value> return_object = nodem::data(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::data exit\n");

    return;
} // @end nodem::Gtm::data method

/*
 * @method nodem::Gtm::get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::get(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::get enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::get;
#else
    gtm_baton->gtm_function = &gtm::get;
#endif
    gtm_baton->ret_function = &nodem::get;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::get exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::get(gtm_baton);
#else
    gtm_baton->status = gtm::get(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK && gtm_baton->status != YDB_ERR_GVUNDEF && gtm_baton->status != YDB_ERR_LVUNDEF) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into get");

    Local<Value> return_object = nodem::get(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::get exit\n");

    return;
} // @end nodem::Gtm::get method

/*
 * @method nodem::Gtm::set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::set(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::set enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    Local<Value> data_value;
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
        data_value = get_n(isolate, arg_object, new_string_n(isolate, "data"));
    } else {
        if (args_cnt < 2) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
            return;
        }

        glvn = info[0];
        data_value = info[args_cnt - 1];

        if (args_cnt > 2) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 2);

            for (unsigned int i = 1; i < args_cnt - 1; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    if (data_value->IsUndefined()) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'data' property")));
        return;
    }

    Local<Value> data_node;

#if NODEM_SIMPLE_API == 1
    data_node = data_value;
#else
    Local<Array> data_array = Array::New(isolate, 1);
    set_n(isolate, data_array, 0, data_value);

    data_node = encode_arguments(data_array, gtm_state);
#endif

    if (data_node->IsUndefined()) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Property 'data' contains invalid data")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub, value;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
        value = *(UTF8_VALUE_TEMP_N(isolate, data_node));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};
        GtmValue gtm_data {data_node};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
        value = gtm_data.to_byte();
    }

#if NODEM_SIMPLE_API == 1
    if (gtm_state->mode == CANONICAL && data_value->IsNumber()) {
        if (value.substr(0, 2) == "0.")
            value = value.substr(1, string::npos);

        if (value.substr(0, 3) == "-0.")
            value = "-" + value.substr(2, string::npos);
    }
#endif

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
        debug_log(">>   data: ", value);
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, data_value);
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->value = value;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::set;
#else
    gtm_baton->gtm_function = &gtm::set;
#endif
    gtm_baton->ret_function = &nodem::set;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::set exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::set(gtm_baton);
#else
    gtm_baton->status = gtm::set(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into set");

    Local<Value> return_object = nodem::set(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::set exit\n");

    return;
} // @end nodem::Gtm::set method

/*
 * @method nodem::Gtm::kill
 * @summary Kill a global or local, or global or local node, or remove the entire symbol table
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::kill(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::kill enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;
    int32_t node_only = -1;

    if (info[0]->IsObject() && !info[0]->IsFunction()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));

        if (has_n(isolate, arg_object, new_string_n(isolate, "nodeOnly")))
            node_only = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "nodeOnly")));
    } else if (args_cnt > 0) {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    Local<Value> subs = String::Empty(isolate);
    vector<string> subs_array;

    if (!glvn->IsUndefined() && !glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    } else if (glvn->IsUndefined()) {
        glvn = String::Empty(isolate);
        local = true;
    } else {
        if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
            bool error = false;
            subs_array = build_subscripts(subscripts, error, gtm_state);

            if (error) {
                isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
                return;
            }
#else
            subs = encode_arguments(subscripts, gtm_state);

            if (subs->IsUndefined()) {
                isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
                return;
            }
#endif
        } else if (!subscripts->IsUndefined()) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
            return;
        }
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->node_only = node_only;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::kill;
#else
    gtm_baton->gtm_function = &gtm::kill;
#endif
    gtm_baton->ret_function = &nodem::kill;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::kill exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::kill(gtm_baton);
#else
    gtm_baton->status = gtm::kill(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into kill");

    Local<Value> return_object = nodem::kill(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::kill exit\n");

    return;
} // @end nodem::Gtm::kill method

/*
 * @method nodem::Gtm::merge
 * @summary Merge an global or local array node to another global or local array node
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::merge(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::merge enter");

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    if (info.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an argument")));
        return;
    } else if (!info[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = to_object_n(isolate, info[0]);
    Local<Value> from_test = get_n(isolate, arg_object, new_string_n(isolate, "from"));
    Local<Value> to_test = get_n(isolate, arg_object, new_string_n(isolate, "to"));
    bool from_local = false;
    bool to_local = false;

    if (!has_n(isolate, arg_object, new_string_n(isolate, "from"))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'from' property")));
        return;
    } else if (!from_test->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "'from' property must be an object")));
        return;
    }

    Local<Object> from = to_object_n(isolate, from_test);

    if (!has_n(isolate, arg_object, new_string_n(isolate, "to"))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'to' property")));
        return;
    } else if (!to_test->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "'to' property must be an object")));
        return;
    }

    Local<Object> to = to_object_n(isolate, to_test);
    Local<Value> from_glvn = get_n(isolate, from, new_string_n(isolate, "global"));

    if (from_glvn->IsUndefined()) {
        from_glvn = get_n(isolate, from, new_string_n(isolate, "local"));

        if (from_glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate,
                    "Need a 'global' or 'local' property in your 'from' object")));
            return;
        } else {
            from_local = true;
        }
    }

    if (!from_glvn->IsString()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global in 'from' must be a string")));
        return;
    } else if (from_glvn->StrictEquals(new_string_n(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global in 'from' must not be an empty string")));
        return;
    }

    Local<Value> to_glvn = get_n(isolate, to, new_string_n(isolate, "global"));

    if (to_glvn->IsUndefined()) {
        to_glvn = get_n(isolate, to, new_string_n(isolate, "local"));

        if (to_glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate,
                    "Need a 'global' or 'local' property in your 'to' object")));
            return;
        } else {
            to_local = true;
        }
    }

    if (!to_glvn->IsString()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global in 'to' must be a string")));
        return;
    } else if (to_glvn->StrictEquals(new_string_n(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global in 'to' must not be an empty string")));
        return;
    }

    Local<Value> from_subscripts = get_n(isolate, from, new_string_n(isolate, "subscripts"));
    Local<Value> from_subs = Undefined(isolate);

    if (from_subscripts->IsUndefined()) {
        from_subs = String::Empty(isolate);
    } else if (from_subscripts->IsArray()) {
        from_subs = encode_arguments(from_subscripts, gtm_state);

        if (from_subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate,
              "Property 'subscripts' in 'from' object contains invalid data")));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate,
          "Property 'subscripts' in 'from' must contain an array")));
        return;
    }

    Local<Value> to_subscripts = get_n(isolate, to, new_string_n(isolate, "subscripts"));
    Local<Value> to_subs = Undefined(isolate);

    if (to_subscripts->IsUndefined()) {
        to_subs = String::Empty(isolate);
    } else if (to_subscripts->IsArray()) {
        to_subs = encode_arguments(to_subscripts, gtm_state);

        if (to_subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate,
              "Property 'subscripts' in 'to' object contains invalid data")));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' in 'to' must contain an array")));
        return;
    }

    const char* from_name_msg;
    Local<Value> from_name;

    if (from_local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, from_glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' is an invalid name")));
            return;
        }

        from_name_msg = ">>   from_local: ";
        from_name = localize_name(from_glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, from_name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' in 'from' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, from_glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'global' is an invalid name")));
            return;
        }

        from_name_msg = ">>   from_global: ";
        from_name = globalize_name(from_glvn, gtm_state);
    }

    const char* to_name_msg;
    Local<Value> to_name;

    if (to_local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, to_glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' is an invalid name")));
            return;
        }

        to_name_msg = ">>   to_local: ";
        to_name = localize_name(to_glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, to_name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' in 'to' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, to_glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'global' is an invalid name")));
            return;
        }

        to_name_msg = ">>   to_global: ";
        to_name = globalize_name(to_glvn, gtm_state);
    }

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    gtm_status_t stat_buf;
    gtm_char_t merge[] = "merge";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = merge;
    access.rtn_name.length = strlen(merge);
    access.handle = NULL;

    if (gtm_state->utf8 == true) {
        if (gtm_state->debug > LOW) {
            debug_log(from_name_msg, *(UTF8_VALUE_TEMP_N(isolate, from_name)));
            debug_log(">> from_subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, from_subs)));
            debug_log(to_name_msg, *(UTF8_VALUE_TEMP_N(isolate, to_name)));
            debug_log(">> to_subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, to_subs)));
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_cip(&access, ret_buf, *(UTF8_VALUE_TEMP_N(isolate, from_name)), *(UTF8_VALUE_TEMP_N(isolate, from_subs)),
          *(UTF8_VALUE_TEMP_N(isolate, to_name)), *(UTF8_VALUE_TEMP_N(isolate, to_subs)), gtm_state->mode);
    } else {
        GtmValue gtm_from_name {from_name};
        GtmValue gtm_from_subs {from_subs};
        GtmValue gtm_to_name {to_name};
        GtmValue gtm_to_subs {to_subs};

        if (gtm_state->debug > LOW) {
            debug_log(from_name_msg, gtm_from_name.to_byte());
            debug_log(">> from_subscripts: ", gtm_from_subs.to_byte());
            debug_log(to_name_msg, gtm_to_name.to_byte());
            debug_log(">> to_subscripts: ", gtm_to_subs.to_byte());
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_cip(&access, ret_buf, gtm_from_name.to_byte(), gtm_from_subs.to_byte(),
          gtm_to_name.to_byte(), gtm_to_subs.to_byte(), gtm_state->mode);
    }
#else
    if (gtm_state->utf8 == true) {
        if (gtm_state->debug > LOW) {
            debug_log(from_name_msg, *(UTF8_VALUE_TEMP_N(isolate, from_name)));
            debug_log(">> from_subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, from_subs)));
            debug_log(to_name_msg, *(UTF8_VALUE_TEMP_N(isolate, to_name)));
            debug_log(">> to_subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, to_subs)));
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_ci(merge, ret_buf, *(UTF8_VALUE_TEMP_N(isolate, from_name)), *(UTF8_VALUE_TEMP_N(isolate, from_subs)),
          *(UTF8_VALUE_TEMP_N(isolate, to_name)), *(UTF8_VALUE_TEMP_N(isolate, to_subs)), gtm_state->mode);
    } else {
        GtmValue gtm_from_name {from_name};
        GtmValue gtm_from_subs {from_subs};
        GtmValue gtm_to_name {to_name};
        GtmValue gtm_to_subs {to_subs};

        if (gtm_state->debug > LOW) {
            debug_log(from_name_msg, gtm_from_name.to_byte());
            debug_log(">> from_subscripts: ", gtm_from_subs.to_byte());
            debug_log(to_name_msg, gtm_to_name.to_byte());
            debug_log(">> to_subscripts: ", gtm_to_subs.to_byte());
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_ci(merge, ret_buf, gtm_from_name.to_byte(), gtm_from_subs.to_byte(),
          gtm_to_name.to_byte(), gtm_to_subs.to_byte(), gtm_state->mode);
    }
#endif

    if (gtm_state->debug > LOW) {
        funlockfile(stderr);

        if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    uv_mutex_unlock(&mutex_g);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::merge JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
        return;
    } else {
        temp_object = to_object_n(isolate, json);
    }

    Local<Object> return_object = Object::New(isolate);

    if (gtm_state->mode == STRICT) {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Number::New(isolate, 1));

        if (from_local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), from_name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(from_glvn, gtm_state));
        }

        if (!from_subscripts->IsUndefined() || !to_subscripts->IsUndefined()) {
            Local<Value> temp_subscripts = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));

            if (!temp_subscripts->IsUndefined()) {
                set_n(isolate, return_object, new_string_n(isolate, "subscripts"), temp_subscripts);
            } else {
                if (!from_subscripts->IsUndefined()) {
                    set_n(isolate, return_object, new_string_n(isolate, "subscripts"), from_subscripts);
                } else {
                    set_n(isolate, return_object, new_string_n(isolate, "subscripts"), to_subscripts);
                }
            }
        }

        set_n(isolate, return_object, new_string_n(isolate, "result"), new_string_n(isolate, "1"));
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (from_local) {
            set_n(isolate, from, new_string_n(isolate, "local"), from_name);
        } else {
            set_n(isolate, from, new_string_n(isolate, "global"), localize_name(from_glvn, gtm_state));
        }

        set_n(isolate, return_object, new_string_n(isolate, "from"), from);

        if (to_local) {
            set_n(isolate, to, new_string_n(isolate, "local"), to_name);
        } else {
            set_n(isolate, to, new_string_n(isolate, "global"), localize_name(to_glvn, gtm_state));
        }

        set_n(isolate, return_object, new_string_n(isolate, "to"), to);
        set_n(isolate, return_object, new_string_n(isolate, "result"), Number::New(isolate, 1));
    }

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::merge exit\n");

    return;
} // @end nodem::Gtm::merge method

/*
 * @method nodem::Gtm::order
 * @summary Return the next global or local node at the same level
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::order(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::order enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::order;
#else
    gtm_baton->gtm_function = &gtm::order;
#endif
    gtm_baton->ret_function = &nodem::order;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::order exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::order(gtm_baton);
#else
    gtm_baton->status = gtm::order(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK && gtm_baton->status != YDB_NODE_END) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into order");

    Local<Value> return_object = nodem::order(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::order exit\n");

    return;
} // @end nodem::Gtm::order method

/*
 * @method nodem::Gtm::previous
 * @summary Return the previous global or local node at the same level
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::previous(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::previous enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::previous;
#else
    gtm_baton->gtm_function = &gtm::previous;
#endif
    gtm_baton->ret_function = &nodem::previous;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::previous exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::previous(gtm_baton);
#else
    gtm_baton->status = gtm::previous(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK && gtm_baton->status != YDB_NODE_END) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into previous");

    Local<Value> return_object = nodem::previous(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::previous exit\n");

    return;
} // @end nodem::Gtm::previous method

/*
 * @method nodem::Gtm::next_node
 * @summary Return the next global or local node, depth first
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::next_node(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::next_node enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, Undefined(isolate));
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::next_node;
#else
    gtm_baton->gtm_function = &gtm::next_node;
#endif
    gtm_baton->ret_function = &nodem::next_node;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::next_node exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::next_node(gtm_baton);
#else
    gtm_baton->status = gtm::next_node(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK && gtm_baton->status != YDB_NODE_END) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into next_node");

    Local<Value> return_object = nodem::next_node(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::next_node exit\n");

    return;
} // @end nodem::Gtm::next_node method

/*
 * @method nodem::Gtm::previous_node
 * @summary Same as Gtm::next_node, only in reverse
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::previous_node(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::previous_node enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, Undefined(isolate));
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::previous_node;
#else
    gtm_baton->gtm_function = &gtm::previous_node;
#endif
    gtm_baton->ret_function = &nodem::previous_node;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::previous_node exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::previous_node(gtm_baton);
#else
    gtm_baton->status = gtm::previous_node(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK && gtm_baton->status != YDB_NODE_END) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into previous_node");

    Local<Value> return_object = nodem::previous_node(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::previous_node exit\n");

    return;
} // @end nodem::Gtm::previous_node method

/*
 * @method nodem::Gtm::increment
 * @summary Increment or decrement the number in a global or local node
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::increment(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::increment enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    Local<Value> increment = Number::New(isolate, 1);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));

        if (has_n(isolate, arg_object, new_string_n(isolate, "increment"))) {
            increment = get_n(isolate, arg_object, new_string_n(isolate, "increment"));
        } else if (gtm_state->mode == STRICT && args_cnt > 1) {
            increment = info[1];
        }
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!increment->IsNumber())
        increment = Number::New(isolate, 0);

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif

        debug_log(">>   increment: ", number_value_n(isolate, increment));
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->option = number_value_n(isolate, increment);
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::increment;
#else
    gtm_baton->gtm_function = &gtm::increment;
#endif
    gtm_baton->ret_function = &nodem::increment;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::increment exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::increment(gtm_baton);
#else
    gtm_baton->status = gtm::increment(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into increment");

    Local<Value> return_object = nodem::increment(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::increment exit\n");

    return;
} // @end nodem::Gtm::increment method

/*
 * @method nodem::Gtm::lock
 * @summary Lock a global or local node, incrementally
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::lock(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::lock enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    Local<Value> timeout = Number::New(isolate, -1);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));

        if (has_n(isolate, arg_object, new_string_n(isolate, "timeout"))) {
            timeout = get_n(isolate, arg_object, new_string_n(isolate, "timeout"));

            if (number_value_n(isolate, timeout) < 0)
                timeout = Number::New(isolate, 0);
        } else if (gtm_state->mode == STRICT && args_cnt > 1) {
            timeout = info[1];

            if (number_value_n(isolate, timeout) < 0)
                timeout = Number::New(isolate, 0);
        }
    } else {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    if (!timeout->IsNumber())
        timeout = Number::New(isolate, 0);

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error, gtm_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, gtm_state);

        if (subs->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif

        debug_log(">>   timeout: ", number_value_n(isolate, timeout));
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->option = number_value_n(isolate, timeout);
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::lock;
#else
    gtm_baton->gtm_function = &gtm::lock;
#endif
    gtm_baton->ret_function = &nodem::lock;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::lock exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::lock(gtm_baton);
#else
    gtm_baton->status = gtm::lock(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into lock");

    Local<Value> return_object = nodem::lock(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::lock exit\n");

    return;
} // @end nodem::Gtm::lock method

/*
 * @method nodem::Gtm::unlock
 * @summary Unlock a global or local node, incrementally, or release all locks
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::unlock(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::unlock enter");

#if YDB_RELEASE >= 126
    reset_handler(gtm_state);
#endif

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (info[0]->IsObject() && !info[0]->IsFunction()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        glvn = get_n(isolate, arg_object, new_string_n(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = get_n(isolate, arg_object, new_string_n(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = get_n(isolate, arg_object, new_string_n(isolate, "subscripts"));
    } else if (args_cnt > 0) {
        glvn = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_subscripts, i - 1, info[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^')
            local = true;
    }

    Local<Value> subs = String::Empty(isolate);
    vector<string> subs_array;

    if (!glvn->IsUndefined() && !glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(new_string_n(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Global must not be an empty string")));
        }

        return;
    } else if (glvn->IsUndefined()) {
        glvn = String::Empty(isolate);
        local = true;
    } else {
        if (subscripts->IsArray()) {
#if NODEM_SIMPLE_API == 1
            bool error = false;
            subs_array = build_subscripts(subscripts, error, gtm_state);

            if (error) {
                isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
                return;
            }
#else
            subs = encode_arguments(subscripts, gtm_state);

            if (subs->IsUndefined()) {
                isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
                return;
            }
#endif
        } else if (!subscripts->IsUndefined()) {
            isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'subscripts' must contain an array")));
            return;
        }
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, gtm_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)), gtm_state)) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, gtm_state);
    }

    string gvn, sub;

    if (gtm_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(name_msg, gvn);

#if NODEM_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                debug_log(">>   subscripts[", i, "]: ", subs_array[i]);
            }
        }
#else
        debug_log(">>   subscripts: ", sub);
#endif
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, subscripts);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = gvn;
    gtm_baton->args = sub;
    gtm_baton->subs_array = subs_array;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    gtm_baton->gtm_function = &ydb::unlock;
#else
    gtm_baton->gtm_function = &gtm::unlock;
#endif
    gtm_baton->ret_function = &nodem::unlock;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW)
        debug_log(">>   mode: ", gtm_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::unlock exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if NODEM_SIMPLE_API == 1
    gtm_baton->status = ydb::unlock(gtm_baton);
#else
    gtm_baton->status = gtm::unlock(gtm_baton);
#endif

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (gtm_baton->status == -1) {
        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (gtm_baton->status != YDB_OK) {
#else
    if (gtm_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into unlock");

    Local<Value> return_object = nodem::unlock(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::unlock exit\n");

    return;
} // @end nodem::Gtm::unlock method

/*
 * @method nodem::Gtm::function
 * @summary Call an arbitrary extrinsic function
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::function(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::function enter");

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> function;
    Local<Value> arguments = Undefined(isolate);
    uint32_t relink = gtm_state->auto_relink;
    bool local = false;
    bool position = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        function = get_n(isolate, arg_object, new_string_n(isolate, "function"));

        if (function->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'function' property")));
            return;
        }

        arguments = get_n(isolate, arg_object, new_string_n(isolate, "arguments"));

        if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink")))
            relink = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));
    } else {
        function = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_arguments = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_arguments, i - 1, info[i]);
            }

            arguments = temp_arguments;
        }

        position = true;
    }

    if (!function->IsString()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Function must be a string")));
        return;
    } else if (function->StrictEquals(new_string_n(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Function must not be an empty string")));
        return;
    }

    Local<Value> args = Undefined(isolate);

    if (arguments->IsUndefined()) {
        args = String::Empty(isolate);
    } else if (arguments->IsArray()) {
        args = encode_arguments(arguments, gtm_state, true);

        if (args->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Arguments contain invalid data")));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'arguments' must contain an array")));
        return;
    }

    Local<Value> name = globalize_name(function, gtm_state);

    string func_s, args_s;

    if (gtm_state->utf8 == true) {
        func_s = *(UTF8_VALUE_TEMP_N(isolate, name));
        args_s = *(UTF8_VALUE_TEMP_N(isolate, args));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {args};

        func_s = gtm_name.to_byte();
        args_s = gtm_args.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(">>   function: ", func_s);
        debug_log(">>   arguments: ", args_s);
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, arguments);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = func_s;
    gtm_baton->args = args_s;
    gtm_baton->relink = relink;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->status = 0;
    gtm_baton->gtm_function = &gtm::function;
    gtm_baton->ret_function = &nodem::function;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW) {
        debug_log(">>   relink: ", relink);
        debug_log(">>   mode: ", gtm_state->mode);
    }

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::function exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    gtm_baton->status = gtm::function(gtm_baton);

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    if (gtm_baton->status != EXIT_SUCCESS) {
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into function");

    Local<Value> return_object = nodem::function(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::function exit\n");

    return;
} // @end nodem::Gtm::function method

/*
 * @method nodem::Gtm::procedure
 * @summary Call an arbitrary procedure/subroutine
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::procedure(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::procedure enter");

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> procedure;
    Local<Value> arguments = Undefined(isolate);
    uint32_t relink = gtm_state->auto_relink;
    bool local = false;
    bool position = false;
    bool routine = false;

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);
        procedure = get_n(isolate, arg_object, new_string_n(isolate, "procedure"));

        if (procedure->IsUndefined()) {
            procedure = get_n(isolate, arg_object, new_string_n(isolate, "routine"));

            if (procedure->IsUndefined()) {
                isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate,
                  "Need to supply a 'procedure' or 'routine' property")));
                return;
            } else {
                routine = true;
            }
        }

        arguments = get_n(isolate, arg_object, new_string_n(isolate, "arguments"));

        if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink")))
            relink = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));
    } else {
        procedure = info[0];

        if (args_cnt > 1) {
            Local<Array> temp_arguments = Array::New(isolate, args_cnt - 1);

            for (unsigned int i = 1; i < args_cnt; i++) {
                set_n(isolate, temp_arguments, i - 1, info[i]);
            }

            arguments = temp_arguments;
        }

        position = true;
    }

    if (!procedure->IsString()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Procedure must be a string")));
        return;
    } else if (procedure->StrictEquals(new_string_n(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Procedure must not be an empty string")));
        return;
    }

    Local<Value> args = Undefined(isolate);

    if (arguments->IsUndefined()) {
        args = String::Empty(isolate);
    } else if (arguments->IsArray()) {
        args = encode_arguments(arguments, gtm_state, true);

        if (args->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Arguments contain invalid data")));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'arguments' must contain an array")));
        return;
    }

    Local<Value> name = globalize_name(procedure, gtm_state);

    string proc_s, args_s;

    if (gtm_state->utf8 == true) {
        proc_s = *(UTF8_VALUE_TEMP_N(isolate, name));
        args_s = *(UTF8_VALUE_TEMP_N(isolate, args));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {args};

        proc_s = gtm_name.to_byte();
        args_s = gtm_args.to_byte();
    }

    if (gtm_state->debug > LOW) {
        debug_log(">>   procedure: ", proc_s);
        debug_log(">>   arguments: ", args_s);
    }

    GtmBaton* gtm_baton;
    GtmBaton new_baton;

    if (async) {
        gtm_baton = new GtmBaton();

        gtm_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        gtm_baton->error = new gtm_char_t[ERR_LEN];
        gtm_baton->result = new gtm_char_t[RES_LEN];
    } else {
        gtm_baton = &new_baton;

        gtm_baton->callback_p.Reset();

        gtm_baton->error = gtm_state->error;
        gtm_baton->result = gtm_state->result;
    }

    gtm_baton->request.data = gtm_baton;
    gtm_baton->arguments_p.Reset(isolate, arguments);
    gtm_baton->data_p.Reset(isolate, Undefined(isolate));
    gtm_baton->name = proc_s;
    gtm_baton->args = args_s;
    gtm_baton->relink = relink;
    gtm_baton->mode = gtm_state->mode;
    gtm_baton->async = async;
    gtm_baton->local = local;
    gtm_baton->position = position;
    gtm_baton->routine = routine;
    gtm_baton->status = 0;
    gtm_baton->gtm_function = &gtm::procedure;
    gtm_baton->ret_function = &nodem::procedure;
    gtm_baton->gtm_state = gtm_state;

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW) {
        debug_log(">>   relink: ", relink);
        debug_log(">>   mode: ", gtm_state->mode);
    }

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        uv_queue_work(GetCurrentEventLoop(isolate), &gtm_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &gtm_baton->request, async_work, async_after);
#endif

        if (gtm_state->debug > OFF)
            debug_log(">  Gtm::procedure exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    gtm_baton->status = gtm::procedure(gtm_baton);

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    if (gtm_baton->status != EXIT_SUCCESS) {
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(gtm_baton->error, position, async, gtm_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(gtm_baton->error, position, async, gtm_state));
        }

        gtm_baton->arguments_p.Reset();
        gtm_baton->data_p.Reset();

        return;
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   call into procedure");

    Local<Value> return_object = nodem::procedure(gtm_baton);

    gtm_baton->arguments_p.Reset();
    gtm_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::procedure exit\n");

    return;
} // @end nodem::Gtm::procedure method

/*
 * @method nodem::Gtm::global_directory
 * @summary List the globals in a database, with optional filters
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::global_directory(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::global_directory enter");

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    Local<Value> max, lo, hi = Undefined(isolate);

    if (info.Length() > 0 && !info[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Argument must be an object")));
        return;
    } else if (info.Length() > 0) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);

        max = get_n(isolate, arg_object, new_string_n(isolate, "max"));

        if (max->IsUndefined() || !max->IsNumber() || number_value_n(isolate, max) < 0)
            max = Number::New(isolate, 0);

        lo = get_n(isolate, arg_object, new_string_n(isolate, "lo"));

        if (lo->IsUndefined() || !lo->IsString())
            lo = String::Empty(isolate);

        hi = get_n(isolate, arg_object, new_string_n(isolate, "hi"));

        if (hi->IsUndefined() || !hi->IsString())
            hi = String::Empty(isolate);
    } else {
        max = Number::New(isolate, 0);
        lo = String::Empty(isolate);
        hi = String::Empty(isolate);
    }

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW) {
        debug_log(">>   mode: ", gtm_state->mode);
        debug_log(">>   max: ", uint32_value_n(isolate, max));
    }

    gtm_status_t stat_buf;
    gtm_char_t global_directory[] = "global_directory";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = global_directory;
    access.rtn_name.length = strlen(global_directory);
    access.handle = NULL;

    if (gtm_state->utf8 == true) {
        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolate, lo)),
          *(UTF8_VALUE_TEMP_N(isolate, hi)), gtm_state->mode);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", gtm_lo.to_byte());
            debug_log(">>   hi: ", gtm_hi.to_byte());
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), gtm_lo.to_byte(), gtm_hi.to_byte(), gtm_state->mode);
    }
#else
    if (gtm_state->utf8 == true) {
        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_ci(global_directory, ret_buf, uint32_value_n(isolate, max),
          *(UTF8_VALUE_TEMP_N(isolae, lo)), *(UTF8_VALUE_TEMP_N(isolate, hi)), gtm_state->mode);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", gtm_lo.to_byte());
            debug_log(">>   hi: ", gtm_hi.to_byte());
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_ci(global_directory, ret_buf, uint32_value_n(isolate, max),
          gtm_lo.to_byte(), gtm_hi.to_byte(), gtm_state->mode);
    }
#endif

    if (gtm_state->debug > LOW) {
        funlockfile(stderr);

        if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    uv_mutex_unlock(&mutex_g);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::global_directory JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Value> json = json_method(json_string, "parse", gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
    } else {
        info.GetReturnValue().Set(Local<Array>::Cast(json));
    }

    if (gtm_state->debug > OFF)
        debug_log(">   Gtm::global_directory exit\n");

    return;
} // @end nodem::Gtm::global_directory method

/*
 * @method nodem::Gtm::local_directory
 * @summary List the local variables in the symbol table, with optional filters
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::local_directory(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::local_directory enter");

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    Local<Value> max, lo, hi = Undefined(isolate);

    if (info.Length() > 0 && !info[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Argument must be an object")));
        return;
    } else if (info.Length() > 0) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);

        max = get_n(isolate, arg_object, new_string_n(isolate, "max"));

        if (max->IsUndefined() || !max->IsNumber() || number_value_n(isolate, max) < 0)
            max = Number::New(isolate, 0);

        lo = get_n(isolate, arg_object, new_string_n(isolate, "lo"));

        if (lo->IsUndefined() || !lo->IsString())
            lo = String::Empty(isolate);

        hi = get_n(isolate, arg_object, new_string_n(isolate, "hi"));

        if (hi->IsUndefined() || !hi->IsString())
            hi = String::Empty(isolate);
    } else {
        max = Number::New(isolate, 0);
        lo = String::Empty(isolate);
        hi = String::Empty(isolate);
    }

    if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, lo)), gtm_state)) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'lo' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, lo)), gtm_state)) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'lo' is an invalid name")));
        return;
    }

    if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, hi)), gtm_state)) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'hi' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, hi)), gtm_state)) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'hi' is an invalid name")));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    if (gtm_state->debug > LOW) {
        debug_log(">>   mode: ", gtm_state->mode);
        debug_log(">>   max: ", uint32_value_n(isolate, max));
    }

    gtm_status_t stat_buf;
    gtm_char_t local_directory[] = "local_directory";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = local_directory;
    access.rtn_name.length = strlen(local_directory);
    access.handle = NULL;

    if (gtm_state->utf8 == true) {
        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolate, lo)),
          *(UTF8_VALUE_TEMP_N(isolate, hi)), gtm_state->mode);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", gtm_lo.to_byte());
            debug_log(">>   hi: ", gtm_hi.to_byte());
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), gtm_lo.to_byte(), gtm_hi.to_byte(), gtm_state->mode);
    }
#else
    if (gtm_state->utf8 == true) {
        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_ci(local_directory, ret_buf, uint32_value_n(isolate, max),
          *(UTF8_VALUE_TEMP_N(isolate, lo)), *(UTF8_VALUE_TEMP_N(isolate, hi)), gtm_state->mode);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (gtm_state->debug > LOW) {
            debug_log(">>   lo: ", gtm_lo.to_byte());
            debug_log(">>   hi: ", gtm_hi.to_byte());
        }

        uv_mutex_lock(&mutex_g);

        if (gtm_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];
                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        stat_buf = gtm_ci(local_directory, ret_buf, uint32_value_n(isolate, max),
          gtm_lo.to_byte(), gtm_hi.to_byte(), gtm_state->mode);
    }
#endif

    if (gtm_state->debug > LOW) {
        funlockfile(stderr);

        if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];
            cerr << strerror_r(errno, error, BUFSIZ);
        }
    }

    if (gtm_state->debug > LOW)
        debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    uv_mutex_unlock(&mutex_g);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::local_directory JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Value> json = json_method(json_string, "parse", gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
    } else {
        info.GetReturnValue().Set(Local<Array>::Cast(json));
    }

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::local_directory exit\n");

    return;
} // @end nodem::Gtm::local_directory method

/*
 * @method nodem::Gtm::retrieve
 * @summary Not yet implemented
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::retrieve(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::retrieve enter");

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    gtm_status_t stat_buf;
    gtm_char_t retrieve[] = "retrieve";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = retrieve;
    access.rtn_name.length = strlen(retrieve);
    access.handle = NULL;

    uv_mutex_lock(&mutex_g);

    stat_buf = gtm_cip(&access, ret_buf);
#else
    uv_mutex_lock(&mutex_g);

    stat_buf = gtm_ci(retrieve, ret_buf);
#endif

    if (gtm_state->debug > LOW)
        debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    uv_mutex_unlock(&mutex_g);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::retrieve JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
        return;
    } else {
        temp_object = to_object_n(isolate, json);
    }

    info.GetReturnValue().Set(temp_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::retrieve exit\n");

    return;
} // @end nodem::Gtm::retrieve method

/*
 * @method nodem::Gtm::update
 * @summary Not yet implemented
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::update(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::update enter");

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  call into " NODEM_DB);

    gtm_status_t stat_buf;
    gtm_char_t update[] = "update";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = update;
    access.rtn_name.length = strlen(update);
    access.handle = NULL;

    uv_mutex_lock(&mutex_g);

    stat_buf = gtm_cip(&access, ret_buf);
#else
    uv_mutex_lock(&mutex_g);

    stat_buf = gtm_ci(update, ret_buf);
#endif

    if (gtm_state->debug > LOW)
        debug_log(">>   stat_buf: ", stat_buf);

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, gtm_state));
        return;
    }

    if (gtm_state->debug > OFF)
        debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (gtm_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    uv_mutex_unlock(&mutex_g);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::update JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", gtm_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
        return;
    } else {
        temp_object = to_object_n(isolate, json);
    }

    info.GetReturnValue().Set(temp_object);

    if (gtm_state->debug > OFF)
        debug_log(">  Gtm::update exit\n");

    return;
} // @end nodem::Gtm::update method

// ***End Public APIs***

/*
 * @method nodem::Gtm::New
 * @summary The Gtm class constructor
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Gtm::New(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();

    if (info.IsConstructCall()) {
        Gtm* gtm = new Gtm();
        gtm->Wrap(info.This());

#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
        AddEnvironmentCleanupHook(isolate, cleanup_gtm, static_cast<void*>(gtm));
#endif

        info.GetReturnValue().Set(info.This());
    } else {
        GtmState* gtm_state = reinterpret_cast<GtmState*>(info.Data().As<External>()->Value());
        Local<Function> constructor = Local<Function>::New(isolate, gtm_state->constructor_p);

#if NODE_MAJOR_VERSION >=7 || NODE_MAJOR_VERSION == 6 && NODE_MINOR_VERSION >= 8
        MaybeLocal<Object> instance = constructor->NewInstance(isolate->GetCurrentContext());

        if (instance.IsEmpty()) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Unable to instantiate the Gtm class")));
        } else {
            info.GetReturnValue().Set(instance.ToLocalChecked());
        }
#else
        info.GetReturnValue().Set(constructor->NewInstance());
#endif
    }

    return;
} // @end nodem::Gtm::New method

/*
 * @method nodem::Gtm::Init
 * @summary Set the exports property when mumps.node is required
 * @param {Local<Object>} exports - A special object passed by the Node.js runtime
 * @returns {void}
 */
void Gtm::Init(Local<Object> exports)
{
    Isolate* isolate = Isolate::GetCurrent();

    GtmState* gtm_state = new GtmState(isolate, exports);

#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
    AddEnvironmentCleanupHook(isolate, cleanup_gtm_state, static_cast<void*>(gtm_state));
#endif

    Local<External> external_data = External::New(isolate, gtm_state);
    Local<FunctionTemplate> fn_template = FunctionTemplate::New(isolate, New, external_data);

    fn_template->SetClassName(new_string_n(isolate, "Gtm"));
    fn_template->InstanceTemplate()->SetInternalFieldCount(1);

    set_prototype_method_n(isolate, fn_template, "open", open, external_data);
    set_prototype_method_n(isolate, fn_template, "configure", configure, external_data);
    set_prototype_method_n(isolate, fn_template, "close", close, external_data);
    set_prototype_method_n(isolate, fn_template, "help", help, external_data);
    set_prototype_method_n(isolate, fn_template, "version", version, external_data);
    set_prototype_method_n(isolate, fn_template, "about", version, external_data);
    set_prototype_method_n(isolate, fn_template, "data", data, external_data);
    set_prototype_method_n(isolate, fn_template, "get", get, external_data);
    set_prototype_method_n(isolate, fn_template, "set", set, external_data);
    set_prototype_method_n(isolate, fn_template, "kill", kill, external_data);
    set_prototype_method_n(isolate, fn_template, "merge", merge, external_data);
    set_prototype_method_n(isolate, fn_template, "order", order, external_data);
    set_prototype_method_n(isolate, fn_template, "next", order, external_data);
    set_prototype_method_n(isolate, fn_template, "previous", previous, external_data);
    set_prototype_method_n(isolate, fn_template, "nextNode", next_node, external_data);
    set_prototype_method_n(isolate, fn_template, "next_node", next_node, external_data);
    set_prototype_method_n(isolate, fn_template, "previousNode", previous_node, external_data);
    set_prototype_method_n(isolate, fn_template, "previous_node", previous_node, external_data);
    set_prototype_method_n(isolate, fn_template, "increment", increment, external_data);
    set_prototype_method_n(isolate, fn_template, "lock", lock, external_data);
    set_prototype_method_n(isolate, fn_template, "unlock", unlock, external_data);
    set_prototype_method_n(isolate, fn_template, "function", function, external_data);
    set_prototype_method_n(isolate, fn_template, "procedure", procedure, external_data);
    set_prototype_method_n(isolate, fn_template, "routine", procedure, external_data);
    set_prototype_method_n(isolate, fn_template, "globalDirectory", global_directory, external_data);
    set_prototype_method_n(isolate, fn_template, "global_directory", global_directory, external_data);
    set_prototype_method_n(isolate, fn_template, "localDirectory", local_directory, external_data);
    set_prototype_method_n(isolate, fn_template, "local_directory", local_directory, external_data);
    set_prototype_method_n(isolate, fn_template, "retrieve", retrieve, external_data);
    set_prototype_method_n(isolate, fn_template, "update", update, external_data);

#if NODE_MAJOR_VERSION >= 3
    MaybeLocal<Function> maybe_function = fn_template->GetFunction(isolate->GetCurrentContext());
    Local<Function> local_function;

    if (maybe_function.IsEmpty()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Unable to construct the Gtm class")));
    } else {
        local_function = maybe_function.ToLocalChecked();
    }
#else
    Local<Function> local_function = fn_template->GetFunction();
#endif

    gtm_state->constructor_p.Reset(isolate, local_function);
    Local<Function> constructor = Local<Function>::New(isolate, gtm_state->constructor_p);

    set_n(isolate, exports, new_string_n(isolate, "Gtm"), constructor);
    if (strncmp(NODEM_DB, "YottaDB", 7) == 0)
        set_n(isolate, exports, new_string_n(isolate, "Ydb"), constructor);

    return;
} // @end nodem::Gtm::Init method

#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7
/*
 * @macro function NODE_MODULE_INIT
 * @summary Register the mumps.node module with Node.js in a context-aware way
 */
NODE_MODULE_INIT() {
    Gtm::Init(exports);
    return;
}
#else
/*
 * @macro NODE_MODULE
 * @summary Register the mumps.node module with Node.js
 */
NODE_MODULE(mumps, Gtm::Init)
#endif

} // @end nodem namespace
