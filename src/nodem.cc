/*
 * Package:    NodeM
 * File:       nodem.cc
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2024 Fourth Watch Software LC
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

#include "nodem.hh"
#include "compat.hh"
#include "gtm.hh"
#include "ydb.hh"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <algorithm>
#include <limits>

#define REVSE "\x1B[7m"
#define RESET "\x1B[0m"

using node::ObjectWrap;
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
using node::AddEnvironmentCleanupHook;
using node::GetCurrentEventLoop;
#endif
using v8::Array;
using v8::Boolean;
using v8::Context;
using v8::DEFAULT;
using v8::DontDelete;
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
#if NODE_MAJOR_VERSION >= 1
using v8::Name;
#endif
#if NODE_MAJOR_VERSION >= 6
using v8::NewStringType;
#endif
using v8::Number;
using v8::Object;
using v8::PropertyCallbackInfo;
#if NODE_MAJOR_VERSION >= 22
using v8::ReadOnly;
using v8::SideEffectType;
#endif
using v8::String;
using v8::TryCatch;
using v8::Value;
using std::boolalpha;
using std::cerr;
using std::cout;
using std::endl;
using std::isdigit;
using std::numeric_limits;
using std::string;
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

uv_mutex_t    mutex_g;
mode_t        mode_g = CANONICAL;
debug_t       debug_g = OFF;
nodem_state_t nodem_state_g = NOT_OPEN;
int           save_stdout_g = -1;
bool          utf8_g = true;
bool          auto_relink_g = false;

static bool   reset_term_g = false;
static bool   signal_sigint_g = true;
static bool   signal_sigquit_g = true;
static bool   signal_sigterm_g = true;
static struct termios term_attr_g;

enum {
    NONE      = 0,
    STRICT    = 1,
    INCREMENT = 2,
    TIMEOUT   = 4,
    NEXT      = 8,
    PREVIOUS  = 16,
    GLOBAL    = 32,
    LOCAL     = 64
};

static char   deprecated_g = NONE;

/*
 * @function nodem::clean_shutdown
 * @summary Handle a SIGINT/SIGQUIT/SIGTERM signal, by cleaning up everything, and exiting Node.js
 * @param {int} signal_num - The number of the caught signal
 * @returns {void}
 */
void clean_shutdown(const int signal_num)
{
    if (nodem_state_g == OPEN) {
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

    exit(EXIT_FAILURE);
} // @end nodem::clean_shutdown function

#if YDB_RELEASE >= 126
/*
 * @function {private} nodem::reset_handler
 * @summary Reset the SIGINT signal when running YottaDB r1.26 or newer (this is a hack)
 * @param {NodemState*} nodem_state - Per-thread state class containing the following members
 * @member {bool} reset_handler - Flag that controls whether to reset the signal handlers
 * @returns {void}
 */
inline static void reset_handler(NodemState* nodem_state)
{
    if (nodem_state->reset_handler == false && signal_sigint_g == true) {
        struct sigaction signal_attr;

        signal_attr.sa_handler = clean_shutdown;
        signal_attr.sa_flags = 0;

        sigfillset(&signal_attr.sa_mask);
        sigaction(SIGINT, &signal_attr, NULL);

        nodem_state->reset_handler = true;
    }

    return;
} // @end nodem::reset_handler function
#endif

#if NODEM_SIMPLE_API == 1
/*
 * @function {private} nodem::is_number
 * @summary Check if a value returned from YottaDB's SimpleAPI is a canonical number
 * @param {string} data - The data value to be tested
 * @returns {boolean} - Whether the data value is a canonical number or not
 */
inline static bool is_number(const string data)
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

    bool flag = false;
    size_t neg_cnt = count(data.begin(), data.end(), '-');
    size_t decp_cnt = count(data.begin(), data.end(), '.');

    if ((decp_cnt == 0 || decp_cnt == 1) && (neg_cnt == 0 || (neg_cnt == 1 && data[0] == '-'))) flag = true;
    if ((decp_cnt == 1 || neg_cnt == 1) && data.length() <= 1) flag = false;
    if (data.length() > 16 || data[data.length() - 1] == '.') flag = false;

    if (flag && !data.empty() && all_of(data.begin(), data.end(), [](char c) {return (isdigit(c) || c == '-' || c == '.');})) {
        if ((data[0] == '0' && data.length() > 1) || (decp_cnt == 1 && data[data.length() - 1] == '0')) {
            return false;
        }

        return true;
    } else {
        return false;
    }
} // @end nodem::is_number function
#endif

/*
 * @function {private} nodem::invalid_name
 * @summary If a variable name contains subscripts, it is not valid, and cannot be used
 * @param {char*} name - The name to test against
 * @returns {bool} - Whether the name is invalid
 */
inline static bool invalid_name(const char* name)
{
    if (strchr(name, '(') != NULL || strchr(name, ')') != NULL) return true;
    return false;
} // @end nodem::invalid_name function

/*
 * @function {private} nodem::invalid_local
 * @summary If a local variable name starts with v4w, it is not valid, and cannot be manipulated
 * @param {char*} name - The name to test against
 * @returns {bool} - Whether the local name is invalid
 */
inline static bool invalid_local(const char* name)
{
    if (strncmp(name, "v4w", 3) == 0) return true;
    return false;
} // @end nodem::invalid_local function

/*
 * @function {private} nodem::globalize_name
 * @summary If a variable name (or function/procedure) doesn't start with (or contain) the optional '^' character, add it
 * @param {Local<Value>} name - The name to be normalized for output
 * @param {NodemState*} nodem_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} [new_name|name] - A string containing the normalized name
 */
static Local<Value> globalize_name(const Local<Value> name, const NodemState* nodem_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_state->debug > MEDIUM) {
        debug_log(">>>    globalize_name enter");
        debug_log(">>>    name: ", *(UTF8_VALUE_TEMP_N(isolate, name)));
    }

    UTF8_VALUE_N(isolate, data_string, name);

    const gtm_char_t* data_name = *data_string;
    const gtm_char_t* char_ptr = strchr(data_name, '^');

    if (char_ptr == NULL) {
        Local<Value> new_name = concat_n(isolate, new_string_n(isolate, "^"), to_string_n(isolate, name));

        if (nodem_state->debug > MEDIUM) debug_log(">>>    globalize_name exit: ", *(UTF8_VALUE_TEMP_N(isolate, new_name)));

        return scope.Escape(new_name);
    }

    if (nodem_state->debug > MEDIUM) debug_log(">>>    globalize_name exit: ", *(UTF8_VALUE_TEMP_N(isolate, name)));

    return scope.Escape(name);
} // @end nodem::globalize_name function

/*
 * @function {private} nodem::localize_name
 * @summary If a variable name starts with the optional '^' character, strip it off for output
 * @param {Local<Value>} name - The name to be normalized for output
 * @param {NodemState*} nodem_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} [data_name|name] - A string containing the normalized name
 */
static Local<Value> localize_name(const Local<Value> name, const NodemState* nodem_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_state->debug > MEDIUM) {
        debug_log(">>>    localize_name enter");
        debug_log(">>>    name: ", *(UTF8_VALUE_TEMP_N(isolate, name)));
    }

    UTF8_VALUE_N(isolate, data_string, name);

    const gtm_char_t* data_name = *data_string;
    const gtm_char_t* char_ptr = strchr(data_name, '^');

    if (char_ptr != NULL && char_ptr - data_name == 0) {
        if (nodem_state->debug > MEDIUM) debug_log(">>>    localize_name exit: ", &data_name[1]);

        return scope.Escape(new_string_n(isolate, &data_name[1]));
    }

    if (nodem_state->debug > MEDIUM) debug_log(">>>    localize_name exit: ", *(UTF8_VALUE_TEMP_N(isolate, name)));

    return scope.Escape(name);
} // @end nodem::localize_name function

/*
 * @function {private} nodem::json_method
 * @summary Call a method on the built-in Node.js JSON object
 * @param {Local<Value>} data - A JSON string containing the data to parse or a JavaScript object to stringify
 * @param {string} type - The name of the method to call on JSON
 * @param {NodemState*} nodem_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} - An object containing the output data
 */
static Local<Value> json_method(Local<Value> data, const string &type, const NodemState* nodem_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_state->debug > MEDIUM) {
        debug_log(">>>    json_method enter");

        if (!data->IsObject()) debug_log(">>>    data: ", *(UTF8_VALUE_TEMP_N(isolate, data)));

        debug_log(">>>    type: ", type);
    }

    Local<Object> global = isolate->GetCurrentContext()->Global();
    Local<Object> json = to_object_n(isolate, get_n(isolate, global, new_string_n(isolate, "JSON")));
    Local<Function> method = Local<Function>::Cast(get_n(isolate, json, new_string_n(isolate, type.c_str())));

    if (nodem_state->debug > MEDIUM) debug_log(">>>    json_method exit");

    return scope.Escape(call_n(isolate, method, json, 1, &data));
} // @end nodem::json_method function

/*
 * @function {private} nodem::error_status
 * @summary Handle an error from the YottaDB/GT.M runtime
 * @param {gtm_char_t*} error - A character string representing the YottaDB/GT.M run-time error
 * @param {bool} position - Whether the API was called by positional arguments or not
 * @param {bool} async - Whether the API was called asynchronously or not
 * @param {NodemState*} nodem_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} result - An object containing the formatted error content
 */
static Local<Value> error_status(gtm_char_t* error, const bool position, const bool async, const NodemState* nodem_state)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_state->debug > MEDIUM) {
        debug_log(">>>    error_status enter");
        debug_log(">>>    error: ", error);
        debug_log(">>>    position: ", boolalpha, position);
        debug_log(">>>    async: ", boolalpha, async);
    }

    char* error_msg;
    const char* code = strtok_r(error, ",", &error_msg);

    // Handle SIGINT caught by YottaDB or GT.M
    if (strstr(error_msg, "%YDB-E-CTRAP") != NULL || strstr(error_msg, "%GTM-E-CTRAP") != NULL) clean_shutdown(SIGINT);

    Local<Object> result = Object::New(isolate);

    if (position && !async) {
        if (nodem_state->debug > MEDIUM) {
            debug_log(">>>    error_status exit");
            debug_log(">>>    error_msg: ", error_msg);
        }

        return scope.Escape(new_string_n(isolate, error_msg));
    } else {
        int error_code = atoi(code);

        set_n(isolate, result, new_string_n(isolate, "ok"), Boolean::New(isolate, false));
        set_n(isolate, result, new_string_n(isolate, "errorCode"), Number::New(isolate, error_code));
        set_n(isolate, result, new_string_n(isolate, "errorMessage"), new_string_n(isolate, error_msg));
    }

    if (nodem_state->debug > MEDIUM) {
        debug_log(">>>    error_status exit");

        Local<Value> result_string = json_method(result, "stringify", nodem_state);
        debug_log(">>>    result: ", *(UTF8_VALUE_TEMP_N(isolate, result_string)));
    }

    return scope.Escape(result);
} // @end nodem::error_status function

/*
 * @function {private} nodem::encode_arguments
 * @summary Encode an array of arguments for parsing in v4wNode.m
 * @param {Local<Value>} arguments - The array of subscripts or arguments to be encoded
 * @param {NodemState*} nodem_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @param {boolean} function <false> - Whether the arguments to encode are from the function or procedure call or not
 * @returns {Local<Value>} [Undefined|encoded_array] - The encoded array of subscripts or arguments, or Undefined if it has bad data
 */
static Local<Value> encode_arguments(const Local<Value> arguments, const NodemState* nodem_state, const bool function = false)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_state->debug > MEDIUM) {
        debug_log(">>>    encode_arguments enter");

        Local<Value> argument_string = json_method(arguments, "stringify", nodem_state);
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
            if (!function) return Undefined(isolate);

            Local<Object> object = to_object_n(isolate, data_test);
            Local<Value> type = get_n(isolate, object, new_string_n(isolate, "type"));
            Local<Value> value_test = get_n(isolate, object, new_string_n(isolate, "value"));
            Local<String> value = to_string_n(isolate, value_test);

            if (value_test->IsSymbol() || value_test->IsSymbolObject()) {
                return Undefined(isolate);
            } else if (type->StrictEquals(new_string_n(isolate, "reference"))) {
                if (!value_test->IsString()) return Undefined(isolate);
                if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);
                if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);

                Local<String> new_value = to_string_n(isolate, localize_name(value, nodem_state));
                Local<String> dot = new_string_n(isolate, ".");

                if (nodem_state->utf8 == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, new_value) + 1));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, new_value->Length() + 1));
                }

                new_data = concat_n(isolate, length, concat_n(isolate, colon, concat_n(isolate, dot, new_value)));
            } else if (type->StrictEquals(new_string_n(isolate, "variable"))) {
                if (!value_test->IsString()) return Undefined(isolate);
                if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);
                if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);

                Local<String> new_value = to_string_n(isolate, localize_name(value, nodem_state));

                if (nodem_state->utf8 == true) {
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
                    if (nodem_state->utf8 == true) {
                        length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, value) + 2));
                    } else {
                        length = to_string_n(isolate, Number::New(isolate, value->Length() + 2));
                    }

                    Local<String> quote = new_string_n(isolate, "\"");

                    new_data = concat_n(isolate, concat_n(isolate, length, concat_n(isolate, colon, quote)),
                               concat_n(isolate, value, quote));
                }
            } else {
                if (nodem_state->utf8 == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, data_value) + 2));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, data_value->Length() + 2));
                }

                Local<String> quote = new_string_n(isolate, "\"");

                new_data = concat_n(isolate, concat_n(isolate, length, concat_n(isolate, colon, quote)),
                           concat_n(isolate, data_value, quote));
            }
        } else {
            if (nodem_state->utf8 == true) {
                length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, data_value) + 2));
            } else {
                length = to_string_n(isolate, Number::New(isolate, data_value->Length() + 2));
            }

            Local<String> quote = new_string_n(isolate, "\"");

            new_data = concat_n(isolate, concat_n(isolate, length, concat_n(isolate, colon, quote)),
                       concat_n(isolate, data_value, quote));
        }

        set_n(isolate, encoded_array, i, new_data);
    }

    if (nodem_state->debug > MEDIUM) debug_log(">>>    encode_arguments exit: ", *(UTF8_VALUE_TEMP_N(isolate, encoded_array)));

    return scope.Escape(encoded_array);
} // @end nodem::encode_arguments function

#if NODEM_SIMPLE_API == 1
/*
 * @function {private} nodem::build_subscripts
 * @summary Build an array of subscritps for passing to the SimpleAPI
 * @param {Local<Value>} subscripts - The array of subscripts to be built
 * @param {bool&} error - If this is set to true, it signals an error with subscript data
 * @param {NodemState*} nodem_state - Per-thread state class containing the following members
 * @member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @returns {vector<string>} [build_array] - The built array of subscripts
 */
static vector<string> build_subscripts(const Local<Value> subscripts, bool& error, NodemState* nodem_state)
{
    Isolate* isolate = Isolate::GetCurrent();

    if (nodem_state->debug > MEDIUM) {
        debug_log(">>>    build_subscripts enter");

        Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_state);
        debug_log(">>>    subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
    }

    Local<Array> subscripts_array = Local<Array>::Cast(subscripts);
    unsigned int length = subscripts_array->Length();

    string subs_data;
    vector<string> subs_array;
    Local<Value> data;

    for (unsigned int i = 0; i < length; i++) {
        data = get_n(isolate, subscripts_array, i);

        if (data->IsSymbol() || data->IsSymbolObject() || data->IsObject() || data->IsArray()) {
            error = true;

            return subs_array;
        }

        if (nodem_state->utf8 == true) {
            subs_data = *(UTF8_VALUE_TEMP_N(isolate, data));
        } else {
            NodemValue nodem_data {data};
            subs_data = nodem_data.to_byte();
        }

        if (nodem_state->mode == CANONICAL && data->IsNumber()) {
            if (subs_data.substr(0, 2) == "0.") subs_data = subs_data.substr(1, string::npos);
            if (subs_data.substr(0, 3) == "-0.") subs_data = "-" + subs_data.substr(2, string::npos);
        }

        if (nodem_state->debug > MEDIUM) debug_log(">>>    subs_data[", i, "]: ", subs_data);

        subs_array.push_back(subs_data);
    }

    if (nodem_state->debug > MEDIUM) debug_log(">>>    build_subscripts exit");

    return subs_array;
} // @end nodem::build_subscripts function
#endif

/*
 * @class nodem::NodemValue
 * @method {instance} to_byte
 * @summary Convert a UTF-8 encoded buffer to a byte encoded buffer
 * @returns {gtm_char_t*} A byte encoded buffer
 */
gtm_char_t* NodemValue::to_byte(void)
{
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 12)
    value->WriteOneByte(Isolate::GetCurrent(), buffer, 0, size);
#else
    value->WriteOneByte(buffer, 0, size);
#endif
    return reinterpret_cast<gtm_char_t*>(buffer);
} // @end NodemValue::to_byte method

/*
 * @class nodem::NodemValue
 * @method {class} from_byte
 * @summary Convert a byte encoded buffer to a UTF-8 encoded buffer
 * @param {gtm_char_t[]} buffer - A byte encoded buffer
 * @returns {Local<String>} A UTF-8 encoded buffer
 */
Local<String> NodemValue::from_byte(gtm_char_t buffer[])
{
    Isolate* isolate = Isolate::GetCurrent();

#if NODE_MAJOR_VERSION >= 6
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
} // @end NodemValue::from_byte method

/*
 * @function {private} nodem::version
 * @summary Return the about/version string
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from data call
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> version(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  version enter");

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
    }

    Local<String> nodem_version = new_string_n(isolate,
      "Node.js Adaptor for " NODEM_DB ": Version: " NODEM_VERSION " (ABI=" NODEM_STRING(NODE_MODULE_VERSION) ") [FWS]");

    Local<String> return_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        return_string = new_string_n(isolate, nodem_baton->result);
    } else {
        return_string = NodemValue::from_byte(nodem_baton->result);
    }

    Local<String> version_string = concat_n(isolate, nodem_version, concat_n(isolate, new_string_n(isolate, "; "), return_string));

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  version exit");

    if (nodem_state_g < OPEN) {
        return scope.Escape(nodem_version);
    } else {
        return scope.Escape(version_string);
    }
} // @end nodem::version function

/*
 * @function {private} nodem::data
 * @summary Check if global or local node has data and/or children or not
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from data call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> data(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  data enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    set_n(isolate, temp_object, new_string_n(isolate, "defined"), Number::New(isolate, atof(nodem_baton->result)));
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  data JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  data exit");
        return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "defined"),
              get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  data exit");

    return scope.Escape(return_object);
} // @end nodem::data function

/*
 * @function {private} nodem::get
 * @summary Return data from a global or local node, or an intrinsic special variable
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from get call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> get(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  get enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (nodem_baton->status == YDB_ERR_GVUNDEF || nodem_baton->status == YDB_ERR_LVUNDEF) {
        set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, false));
    } else {
        set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, true));
    }

    string data(nodem_baton->result);

    if (nodem_baton->nodem_state->mode == CANONICAL && is_number(data)) {
        set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(nodem_baton->result)));
    } else {
        if (nodem_baton->nodem_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, nodem_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), NodemValue::from_byte(nodem_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  get JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  get exit");
        return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "data")));
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "data"), get_n(isolate, temp_object, new_string_n(isolate, "data")));
        set_n(isolate, return_object, new_string_n(isolate, "defined"),
              get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  get exit");

    return scope.Escape(return_object);
} // @end nodem::get function

/*
 * @function {private} nodem::set
 * @summary Return data about the store of a global or local node, or an intrinsic special variable
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Local<Value>} data - V8 object containing the data to store in the node that was called
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> set(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  set enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);
    Local<Value> data_value = Local<Value>::New(isolate, nodem_baton->data_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }

        debug_log(">>   data: ", *(UTF8_VALUE_TEMP_N(isolate, data_value)));
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  set exit");
        Local<Value> ret_data = Undefined(isolate);
        return scope.Escape(ret_data);
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "data"), data_value);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  set exit");

    return scope.Escape(return_object);
} // @end nodem::set function

/*
 * @function {private} nodem::kill
 * @summary Return data about removing a global or global node, or a local or local node, or the entire local symbol table
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {bool} node_only - Whether the API was called on a single node only, or on a node and all its children
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> kill(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  kill enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (name->StrictEquals(new_string_n(isolate, "")) || nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  kill exit");
        Local<Value> ret_data = Undefined(isolate);
        return scope.Escape(ret_data);
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "nodeOnly"), Boolean::New(isolate, nodem_baton->node_only));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  kill exit");

    return scope.Escape(return_object);
} // @end nodem::kill function

/*
 * @function {private} nodem::merge
 * @summary Return data from a merge of a global or local array tree to another global or local array tree
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {Persistent<Value>} object_p - V8 object containing the input object
 * @member {bool} local - Whether the API was called on a from local variable, or a from global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> merge(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  merge enter");

    Local<Object> temp_object = Local<Object>::New(isolate, nodem_baton->object_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        Local<Value> object_string = json_method(temp_object, "stringify", nodem_baton->nodem_state);
        debug_log(">>   object_p: ", *(UTF8_VALUE_TEMP_N(isolate, object_string)));

        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
    }

    Local<Object> return_object = Object::New(isolate);

    set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
    set_n(isolate, return_object, new_string_n(isolate, "from"), get_n(isolate, temp_object, new_string_n(isolate, "from")));
    set_n(isolate, return_object, new_string_n(isolate, "to"), get_n(isolate, temp_object, new_string_n(isolate, "to")));

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  merge exit");

    return scope.Escape(return_object);
} // @end nodem::merge function

/*
 * @function {private} nodem::order
 * @summary Return data about the next global or local node at the same level
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from order call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> order(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  order enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(nodem_baton->result);

    if (nodem_baton->nodem_state->mode == CANONICAL && is_number(data)) {
        set_n(isolate, temp_object, new_string_n(isolate, "result"), Number::New(isolate, atof(nodem_baton->result)));
    } else {
        if (nodem_baton->nodem_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), new_string_n(isolate, nodem_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), NodemValue::from_byte(nodem_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  order JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  order exit");
        return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "result")));
    } else {
        Local<Value> result = get_n(isolate, temp_object, new_string_n(isolate, "result"));
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            set_n(isolate, new_subscripts, Number::New(isolate, new_subscripts->Length() - 1), result);
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), new_subscripts);
        }

        set_n(isolate, return_object, new_string_n(isolate, "result"), localize_name(result, nodem_baton->nodem_state));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  order exit");

    return scope.Escape(return_object);
} // @end nodem::order function

/*
 * @function {private} nodem::previous
 * @summary Return data about the previous global or local node at the same level
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from previous call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> previous(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  previous enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(nodem_baton->result);

    if (nodem_baton->nodem_state->mode == CANONICAL && is_number(data)) {
        set_n(isolate, temp_object, new_string_n(isolate, "result"), Number::New(isolate, atof(nodem_baton->result)));
    } else {
        if (nodem_baton->nodem_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), new_string_n(isolate, nodem_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "result"), NodemValue::from_byte(nodem_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  previous JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  previous exit");
        return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "result")));
    } else {
        Local<Value> result = get_n(isolate, temp_object, new_string_n(isolate, "result"));
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            set_n(isolate, new_subscripts, Number::New(isolate, new_subscripts->Length() - 1), result);
            set_n(isolate, return_object, new_string_n(isolate, "subscripts"), new_subscripts);
        }

        set_n(isolate, return_object, new_string_n(isolate, "result"), localize_name(result, nodem_baton->nodem_state));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  previous exit");

    return scope.Escape(return_object);
} // @end nodem::previous function

/*
 * @function {private} nodem::next_node
 * @summary Return the next global or local node, depth first
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from next_node call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {vector<string>} {ydb only} subs_array - The subscripts of the next node
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> next_node(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  next_node enter");

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (nodem_baton->status == YDB_NODE_END) {
        set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, false));
    } else {
        set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, true));
    }

    if (nodem_baton->status != YDB_NODE_END) {
        string data(nodem_baton->result);

        if (nodem_baton->nodem_state->mode == CANONICAL && is_number(data)) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(nodem_baton->result)));
        } else {
            if (nodem_baton->nodem_state->utf8 == true) {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, nodem_baton->result));
            } else {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), NodemValue::from_byte(nodem_baton->result));
            }
        }
    }

    Local<Array> subs_array = Array::New(isolate);

    if (nodem_baton->subs_array.size()) {
        for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
            if (nodem_baton->nodem_state->debug > LOW) debug_log(">>   subs_array[", i, "]: ", nodem_baton->subs_array[i]);

            if (nodem_baton->nodem_state->mode == CANONICAL && is_number(nodem_baton->subs_array[i])) {
                set_n(isolate, subs_array, i, Number::New(isolate, atof(nodem_baton->subs_array[i].c_str())));
            } else {
                if (nodem_baton->nodem_state->utf8 == true) {
                    set_n(isolate, subs_array, i, new_string_n(isolate, nodem_baton->subs_array[i].c_str()));
                } else {
                    set_n(isolate, subs_array, i, NodemValue::from_byte((gtm_char_t*) nodem_baton->subs_array[i].c_str()));
                }
            }
        }

        set_n(isolate, temp_object, new_string_n(isolate, "subscripts"), subs_array);
    }
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  next_node JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  next_node exit");

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));

        if (temp_subs->IsUndefined()) {
            return scope.Escape(Array::New(isolate));
        }

        return scope.Escape(temp_subs);
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));

        if (!temp_subs->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = get_n(isolate, temp_object, new_string_n(isolate, "data"));

        if (!temp_data->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "data"), temp_data);

        set_n(isolate, return_object, new_string_n(isolate, "defined"),
              get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  next_node exit");

    return scope.Escape(return_object);
} // @end nodem::next_node function

/*
 * @function {private} nodem::previous_node
 * @summary Return the previous global or local node, depth first
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from previous_node call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {vector<string>} {ydb only} subs_array - The subscripts of the previous node
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> previous_node(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  previous_node enter");

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (nodem_baton->status == YDB_NODE_END) {
        set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, false));
    } else {
        set_n(isolate, temp_object, new_string_n(isolate, "defined"), Boolean::New(isolate, true));
    }

    if (nodem_baton->status != YDB_NODE_END) {
        string data(nodem_baton->result);

        if (nodem_baton->nodem_state->mode == CANONICAL && is_number(data)) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(nodem_baton->result)));
        } else {
            if (nodem_baton->nodem_state->utf8 == true) {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, nodem_baton->result));
            } else {
                set_n(isolate, temp_object, new_string_n(isolate, "data"), NodemValue::from_byte(nodem_baton->result));
            }
        }
    }

    Local<Array> subs_array = Array::New(isolate);

    if (nodem_baton->subs_array.size()) {
        for (unsigned int i = 0; i < nodem_baton->subs_array.size(); i++) {
            if (nodem_baton->nodem_state->debug > LOW) debug_log(">>   subs_array[", i, "]: ", nodem_baton->subs_array[i]);

            if (nodem_baton->nodem_state->mode == CANONICAL && is_number(nodem_baton->subs_array[i])) {
                set_n(isolate, subs_array, i, Number::New(isolate, atof(nodem_baton->subs_array[i].c_str())));
            } else {
                if (nodem_baton->nodem_state->utf8 == true) {
                    set_n(isolate, subs_array, i, new_string_n(isolate, nodem_baton->subs_array[i].c_str()));
                } else {
                    set_n(isolate, subs_array, i, NodemValue::from_byte((gtm_char_t*) nodem_baton->subs_array[i].c_str()));
                }
            }
        }

        set_n(isolate, temp_object, new_string_n(isolate, "subscripts"), subs_array);
    }
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) {
        debug_log(">  previous_node JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));
    }

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    if (!get_n(isolate, temp_object, new_string_n(isolate, "status"))->IsUndefined()) return scope.Escape(temp_object);

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  previous_node exit");

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));

        if (temp_subs->IsUndefined()) {
            return scope.Escape(Array::New(isolate));
        }

        return scope.Escape(temp_subs);
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        Local<Value> temp_subs = get_n(isolate, temp_object, new_string_n(isolate, "subscripts"));
        if (!temp_subs->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = get_n(isolate, temp_object, new_string_n(isolate, "data"));

        if (!temp_data->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "data"), temp_data);

        set_n(isolate, return_object, new_string_n(isolate, "defined"),
              get_n(isolate, temp_object, new_string_n(isolate, "defined")));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  previous_node exit");

    return scope.Escape(return_object);
} // @end nodem::previous_node function

/*
 * @function {private} nodem::increment
 * @summary Return the value of an incremented or decremented number in a global or local node
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from increment call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {gtm_double_t} option - The increment value
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> increment(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  increment enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }

        debug_log(">>   increment: ", nodem_baton->option);
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(nodem_baton->result);

    if (nodem_baton->nodem_state->mode == CANONICAL && is_number(data)) {
        set_n(isolate, temp_object, new_string_n(isolate, "data"), Number::New(isolate, atof(nodem_baton->result)));
    } else {
        if (nodem_baton->nodem_state->utf8 == true) {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), new_string_n(isolate, nodem_baton->result));
        } else {
            set_n(isolate, temp_object, new_string_n(isolate, "data"), NodemValue::from_byte(nodem_baton->result));
        }
    }
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  increment JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  increment exit");
        return scope.Escape(get_n(isolate, temp_object, new_string_n(isolate, "data")));
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        set_n(isolate, return_object, new_string_n(isolate, "increment"), Number::New(isolate, nodem_baton->option));
        set_n(isolate, return_object, new_string_n(isolate, "data"), get_n(isolate, temp_object, new_string_n(isolate, "data")));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  increment exit");

    return scope.Escape(return_object);
} // @end nodem::increment function

/*
 * @function {private} nodem::lock
 * @summary Return data about an incremental lock of a global or local node
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {gtm_char_t*} result - Data returned from lock call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> lock(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  lock enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

#if NODEM_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    set_n(isolate, temp_object, new_string_n(isolate, "result"), Boolean::New(isolate, atoi(nodem_baton->result)));
#else
    Local<String> json_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, nodem_baton->result);
    } else {
        json_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  lock JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#   if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#   else
    TryCatch try_catch;
#   endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_baton->nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  lock exit");

        Local<Value> result = get_n(isolate, temp_object, new_string_n(isolate, "result"));
        return scope.Escape(result);
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);

        if (nodem_baton->option > -1) {
            set_n(isolate, return_object, new_string_n(isolate, "timeout"), Number::New(isolate, nodem_baton->option));
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "timeout"),
                  Number::New(isolate, numeric_limits<double>::infinity()));
        }

        set_n(isolate, return_object, new_string_n(isolate, "result"),
              get_n(isolate, temp_object, new_string_n(isolate, "result")));
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  lock exit");

    return scope.Escape(return_object);
} // @end nodem::lock function

/*
 * @function {private} nodem::unlock
 * @summary Return data about unlocking a global or local node, or releasing all locks
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, anything else is an error or message
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> unlock(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  unlock enter");

    Local<Value> subscripts = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   status: ", nodem_baton->status);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify", nodem_baton->nodem_state);
            debug_log(">>   subscripts: ", *(UTF8_VALUE_TEMP_N(isolate, subscript_string)));
        }
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = new_string_n(isolate, nodem_baton->name.c_str());

    if (name->StrictEquals(new_string_n(isolate, "")) || nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  unlock exit");
        Local<Value> ret_data = Undefined(isolate);
        return scope.Escape(ret_data);
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

        if (nodem_baton->local) {
            set_n(isolate, return_object, new_string_n(isolate, "local"), name);
        } else {
            set_n(isolate, return_object, new_string_n(isolate, "global"), localize_name(name, nodem_baton->nodem_state));
        }

        if (!subscripts->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "subscripts"), subscripts);
    }

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  unlock exit");

    return scope.Escape(return_object);
} // @end nodem::unlock function

/*
 * @function {private} nodem::function
 * @summary Return value from an arbitrary extrinsic function
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_char_t*} result - Data returned from function call
 * @member {gtm_uint_t*} info - Indirection limit on input - (0|1) Return data type on output; 0 is string, 1 is canonical number
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {string} relink - Whether to relink the function before calling it
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {bool} utf8 - UTF-8 character encoding; defaults to true
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> function(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  function enter");

    Local<Value> arguments = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   result: ", nodem_baton->result);
        debug_log(">>   info: ", nodem_baton->info);
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!arguments->IsUndefined()) {
            Local<Value> argument_string = json_method(arguments, "stringify", nodem_baton->nodem_state);
            debug_log(">>   arguments: ", *(UTF8_VALUE_TEMP_N(isolate, argument_string)));
        }

        debug_log(">>   relink: ", nodem_baton->relink);
    }

    Local<Value> ret_string;

    if (nodem_baton->nodem_state->utf8 == true) {
        ret_string = new_string_n(isolate, nodem_baton->result);
    } else {
        ret_string = NodemValue::from_byte(nodem_baton->result);
    }

    if (nodem_baton->info == 1) ret_string = to_number_n(isolate, ret_string);

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  function exit");
        return scope.Escape(ret_string);
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> function = new_string_n(isolate, nodem_baton->name.c_str());

    set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
    set_n(isolate, return_object, new_string_n(isolate, "function"), localize_name(function, nodem_baton->nodem_state));

    if (!arguments->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "arguments"), arguments);

    set_n(isolate, return_object, new_string_n(isolate, "autoRelink"), Boolean::New(isolate, nodem_baton->relink));
    set_n(isolate, return_object, new_string_n(isolate, "result"), ret_string);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  function exit");

    return scope.Escape(return_object);
} // @end nodem::function function

/*
 * @function {private} nodem::procedure
 * @summary Return value from an arbitrary procedure/routine
 * @param {NodemBaton*} nodem_baton - struct containing the following members
 * @member {gtm_uint_t} info - Indirection limit
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {string} relink - Whether to relink the procedure before calling it
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @nested-member {mode_t} mode - Data mode: STRING or CANONICAL; defaults to CANONICAL
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> procedure(NodemBaton* nodem_baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  procedure enter");

    Local<Value> arguments = Local<Value>::New(isolate, nodem_baton->arguments_p);

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   position: ", boolalpha, nodem_baton->position);
        debug_log(">>   local: ", boolalpha, nodem_baton->local);
        debug_log(">>   async: ", boolalpha, nodem_baton->async);
        debug_log(">>   name: ", nodem_baton->name);

        if (!arguments->IsUndefined()) {
            Local<Value> argument_string = json_method(arguments, "stringify", nodem_baton->nodem_state);
            debug_log(">>   arguments: ", *(UTF8_VALUE_TEMP_N(isolate, argument_string)));
        }

        debug_log(">>   relink: ", nodem_baton->relink);
    }

    if (nodem_baton->position) {
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  procedure exit");
        Local<Value> ret_data = Undefined(isolate);
        return scope.Escape(ret_data);
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> procedure = new_string_n(isolate, nodem_baton->name.c_str());

    set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

    if (nodem_baton->routine) {
        set_n(isolate, return_object, new_string_n(isolate, "routine"), localize_name(procedure, nodem_baton->nodem_state));
    } else {
        set_n(isolate, return_object, new_string_n(isolate, "procedure"), localize_name(procedure, nodem_baton->nodem_state));
    }

    if (!arguments->IsUndefined()) set_n(isolate, return_object, new_string_n(isolate, "arguments"), arguments);

    set_n(isolate, return_object, new_string_n(isolate, "autoRelink"), Boolean::New(isolate, nodem_baton->relink));

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  procedure exit");

    return scope.Escape(return_object);
} // @end nodem::procedure function

#if NODEM_SIMPLE_API == 1
/*
 * @function {private} nodem::transaction
 * @summary Call a JavaScript function within a YottaDB transaction
 * @param {void*} data - Cast in to a NodemBaton struct containing the following members
 * @member {NodemState*} nodem_state - Per-thread state class containing the following members
 * @nested-member {debug_t} debug - Debug mode: OFF, LOW, MEDIUM, or HIGH; defaults to OFF
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static int transaction(void *data)
{
    Isolate* isolate = Isolate::GetCurrent();

    NodemBaton* nodem_baton = (NodemBaton*) data;

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  transaction enter");

    if (nodem_baton->nodem_state->debug > LOW) {
        debug_log(">>   tp_level: ", nodem_baton->nodem_state->tp_level);
        debug_log(">>   tp_restart: ", nodem_baton->nodem_state->tp_restart);
    }

    if (nodem_baton->nodem_state->tp_restart == 3) {
        nodem_baton->nodem_state->tp_restart = 0;

        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  transaction exit: max restart");

        return YDB_TP_ROLLBACK;
    }

    Local<Value> value = call_n(isolate, Local<Function>::New(isolate, nodem_baton->callback_p), Null(isolate), 0, NULL);

    if (value->IsNull()) {
        if (nodem_baton->nodem_state->tp_level == 1) nodem_baton->nodem_state->tp_restart = 0;
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  transaction exit: error thrown");

        return YDB_TP_ROLLBACK;
    } else if (value->StrictEquals(new_string_n(isolate, "Rollback")) || value->StrictEquals(new_string_n(isolate, "rollback")) ||
      value->StrictEquals(new_string_n(isolate, "ROLLBACK")) || value->StrictEquals(Number::New(isolate, YDB_TP_ROLLBACK))) {
        nodem_baton->nodem_state->tp_restart = 0;

        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  transaction exit: rollback");

        return YDB_TP_ROLLBACK;
    } else if (value->StrictEquals(new_string_n(isolate, "Restart")) || value->StrictEquals(new_string_n(isolate, "restart")) ||
      value->StrictEquals(new_string_n(isolate, "RESTART")) || value->StrictEquals(Number::New(isolate, YDB_TP_RESTART))) {
        if (nodem_baton->nodem_state->tp_level == 1) nodem_baton->nodem_state->tp_restart++;
        if (nodem_baton->nodem_state->debug > OFF) debug_log(">  transaction exit: restart");

        return YDB_TP_RESTART;
    }

    nodem_baton->nodem_state->tp_restart = 0;

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  transaction exit: commit");

    return YDB_OK;
} // @end nodem::transaction function
#endif

#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
/*
 * @function {private} nodem::cleanup_nodem_state
 * @summary Delete heap resources after worker threads exit
 * @param {void*} class_name - The class instance to delete
 * @returns {void}
 */
inline static void cleanup_nodem_state(void* class_name)
{
  delete static_cast<NodemState*>(class_name);
  return;
} // @end nodem::cleanup_nodem_state
#endif

/*
 * @function nodem::async_work
 * @summary Call in to YottaDB/GT.M asynchronously, via a Node.js worker thread
 * @param {uv_work_t*} request - A pointer to the NodemBaton structure for transferring data between the main and worker threads
 * @returns {void}
 */
void async_work(uv_work_t* request)
{
    NodemBaton* nodem_baton = static_cast<NodemBaton*>(request->data);

    if (nodem_baton->nodem_state->debug > LOW) debug_log(">>   async_work enter");
    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    nodem_baton->status = (*nodem_baton->nodem_function)(nodem_baton);

    if (nodem_baton->nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);
    if (nodem_baton->nodem_state->debug > LOW) debug_log(">>   async_work exit\n");

    return;
} // @end nodem::async_work function

/*
 * @function nodem::async_after
 * @summary Call in to the return functions, passing the data from YottaDB/GT.M, after receiving the data from the worker thread
 * @param {uv_work_t*} request - A pointer to the NodemBaton structure for transferring data between the main and worker threads
 * @returns {void}
 */
void async_after(uv_work_t* request, int status)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemBaton* nodem_baton = static_cast<NodemBaton*>(request->data);

    if (nodem_baton->nodem_state->debug > LOW) debug_log(">>   async_after enter: ", status);

    Local<Value> error_code = Null(isolate);
    Local<Value> return_object;
    Local<Value> error_object;

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->callback_p.Reset();
        nodem_baton->object_p.Reset();
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        delete[] nodem_baton->error;
        delete[] nodem_baton->result;
        delete nodem_baton;

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK && nodem_baton->status != YDB_ERR_GVUNDEF &&
               nodem_baton->status != YDB_ERR_LVUNDEF && nodem_baton->status != YDB_NODE_END) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (nodem_baton->nodem_state->debug > LOW) debug_log(">>   " NODEM_DB " error code: ", nodem_baton->status);

        error_object = error_status(nodem_baton->error, nodem_baton->position, nodem_baton->async, nodem_baton->nodem_state);

        error_code = Exception::Error(new_string_n(isolate, *(UTF8_VALUE_TEMP_N(isolate,
                     get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "errorMessage"))))));

        set_n(isolate, ((Object*) *error_code), new_string_n(isolate, "ok"),
              get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "ok")));

        set_n(isolate, ((Object*) *error_code), new_string_n(isolate, "errorCode"),
              get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "errorCode")));

        set_n(isolate, ((Object*) *error_code), new_string_n(isolate, "errorMessage"),
              get_n(isolate, ((Object*) *error_object), new_string_n(isolate, "errorMessage")));

        return_object = Undefined(isolate);
    } else {
        return_object = (*nodem_baton->ret_function)(nodem_baton);
    }

    Local<Value> argv[2] = {error_code, return_object};
    call_n(isolate, Local<Function>::New(isolate, nodem_baton->callback_p), Null(isolate), 2, argv);

    nodem_baton->callback_p.Reset();
    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    delete[] nodem_baton->error;
    delete[] nodem_baton->result;

    if (nodem_baton->nodem_state->debug > LOW) debug_log(">>   async_after exit\n");

    delete nodem_baton;

    return;
} // @end nodem::async_after function

// ***Begin Public APIs***

/*
 * @method nodem::Nodem::open
 * @summary Open a connection with YottaDB/GT.M
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::open(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->pid != nodem_state->tid) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection must be managed by main thread")));
        return;
    } else if (nodem_state_g == CLOSED) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection cannot be reopened")));
        return;
    } else if (nodem_state_g == OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection already open")));
        return;
    }

    char* relink = getenv("NODEM_AUTO_RELINK");

    if (relink != NULL) auto_relink_g = nodem_state->auto_relink = static_cast<bool>(atoi(relink));

    if (info[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, info[0]);

        if (has_n(isolate, arg_object, new_string_n(isolate, "debug"))) {
            UTF8_VALUE_N(isolate, debug, get_n(isolate, arg_object, new_string_n(isolate, "debug")));

            if (strcasecmp(*debug, "off") == 0) {
                debug_g = nodem_state->debug = OFF;
            } else if (strcasecmp(*debug, "low") == 0) {
                debug_g = nodem_state->debug = LOW;
            } else if (strcasecmp(*debug, "medium") == 0) {
                debug_g = nodem_state->debug = MEDIUM;
            } else if (strcasecmp(*debug, "high") == 0) {
                debug_g = nodem_state->debug = HIGH;
            } else {
                debug_g = nodem_state->debug = static_cast<debug_t>
                  (uint32_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "debug"))));

                if (nodem_state->debug < 0) {
                    debug_g = nodem_state->debug = OFF;
                } else if (nodem_state->debug > 3) {
                    debug_g = nodem_state->debug = HIGH;
                }
            }
        }

        if (nodem_state->debug > OFF) {
            debug_log(">  Nodem::open enter");

            char* debug_display = (char*) "off";

            if (nodem_state->debug == LOW) {
                debug_display = (char*) "low";
            } else if (nodem_state->debug == MEDIUM) {
                debug_display = (char*) "medium";
            } else {
                debug_display = (char*) "high";
            }

            debug_log(">  debug: ", debug_display);
        }

        Local<Value> global_dir = get_n(isolate, arg_object, new_string_n(isolate, "globalDirectory"));

        if (global_dir->IsUndefined()) global_dir = get_n(isolate, arg_object, new_string_n(isolate, "namespace"));

        if (!global_dir->IsUndefined() && global_dir->IsString()) {
            if (nodem_state->debug > LOW) debug_log(">>   globalDirectory: ", *(UTF8_VALUE_TEMP_N(isolate, global_dir)));

#if NODEM_SIMPLE_API == 1
            if (setenv("ydb_gbldir", *(UTF8_VALUE_TEMP_N(isolate, global_dir)), 1) == -1) {
#else
            if (setenv("gtmgbldir", *(UTF8_VALUE_TEMP_N(isolate, global_dir)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        Local<Value> routines_path = get_n(isolate, arg_object, new_string_n(isolate, "routinesPath"));

        if (!routines_path->IsUndefined() && routines_path->IsString()) {
            if (nodem_state->debug > LOW) debug_log(">>   routinesPath: ", *(UTF8_VALUE_TEMP_N(isolate, routines_path)));

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
            if (nodem_state->debug > LOW) debug_log(">>   callinTable: ", *(UTF8_VALUE_TEMP_N(isolate, callin_table)));

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

        if (addr->IsUndefined()) {
            addr = get_n(isolate, arg_object, new_string_n(isolate, "ip_address"));

            if (!addr->IsUndefined()) {
                if (!addr->IsString()) {
                    isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "ip_address must be a string")));
                    return;
                }

                debug_log(">>   ip_address: ", *(UTF8_VALUE_TEMP_N(isolate, addr)), " [DEPRECATED - Use ipAddress instead]");
            }
        } else {
            if (!addr->IsString()) {
                isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "ipAddress must be a string")));
                return;
            }

            if (nodem_state->debug > LOW) debug_log(">>   ipAddress: ", *(UTF8_VALUE_TEMP_N(isolate, addr)));
        }

        Local<Value> port = get_n(isolate, arg_object, new_string_n(isolate, "tcpPort"));

        if (port->IsUndefined()) {
            port = get_n(isolate, arg_object, new_string_n(isolate, "tcp_port"));

            if (!port->IsUndefined()) {
                if (!port->IsNumber() && !port->IsString()) {
                    isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "tcp_port must be a number or string")));
                    return;
                }

                debug_log(">>   tcp_port: ", *(UTF8_VALUE_TEMP_N(isolate, port)), " [DEPRECATED - Use tcpPort instead]");
            }
        } else {
            if (!port->IsNumber() && !port->IsString()) {
                isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "tcpPort must be a number or string")));
                return;
            }

            if (nodem_state->debug > LOW) debug_log(">>   tcpPort: ", *(UTF8_VALUE_TEMP_N(isolate, port)));
        }

        if (!addr->IsUndefined() || !port->IsUndefined()) {
            if (addr->IsUndefined()) addr = Local<Value>::New(isolate, new_string_n(isolate, "127.0.0.1"));
            if (port->IsUndefined()) port = Local<Value>::New(isolate, new_string_n(isolate, "6789"));

            Local<String> gtcm_port = concat_n(isolate, new_string_n(isolate, ":"), to_string_n(isolate, port));
            Local<Value> gtcm_nodem = concat_n(isolate, to_string_n(isolate, addr), gtcm_port);

#if NODEM_SIMPLE_API == 1
            if (nodem_state->debug > LOW) debug_log(">>   ydb_cm_NODEM: ", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)));

            if (setenv("ydb_cm_NODEM", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)), 1) == -1) {
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
#else
            if (nodem_state->debug > LOW) debug_log(">>   GTCM_NODEM: ", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)));

            if (setenv("GTCM_NODEM", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)), 1) == -1) {
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
#endif
        }

        if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink"))) {
            nodem_state->auto_relink = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));
            auto_relink_g = nodem_state->auto_relink;
        }

        if (nodem_state->debug > LOW) debug_log(">>   autoRelink: ", boolalpha, nodem_state->auto_relink);

        UTF8_VALUE_N(isolate, nodem_mode, get_n(isolate, arg_object, new_string_n(isolate, "mode")));

        if (strcasecmp(*nodem_mode, "strict") == 0) {
            mode_g = nodem_state->mode = STRING;

            if (nodem_state->debug > OFF || !(deprecated_g & STRICT)) {
                deprecated_g |= STRICT;
                debug_log(">>   mode: strict [DEPRECATED - Use string instead]");
            }
        } else if (strcasecmp(*nodem_mode, "string") == 0) {
            mode_g = nodem_state->mode = STRING;

            if (nodem_state->debug > LOW) debug_log(">>   mode: string");
        } else if (strcasecmp(*nodem_mode, "canonical") == 0) {
            mode_g = nodem_state->mode = CANONICAL;

            if (nodem_state->debug > LOW) debug_log(">>   mode: canonical");
        } else if (nodem_state->debug > LOW) {
            if (nodem_state->mode == STRING) {
                debug_log(">>   mode: string");
            } else {
                debug_log(">>   mode: canonical");
            }
        }

        Local<Value> charset = get_n(isolate, arg_object, new_string_n(isolate, "charset"));

        if (charset->IsUndefined()) charset = get_n(isolate, arg_object, new_string_n(isolate, "encoding"));

        UTF8_VALUE_N(isolate, data_charset, charset);

        if (strcasecmp(*data_charset, "m") == 0 || strcasecmp(*data_charset, "binary") == 0 ||
          strcasecmp(*data_charset, "ascii") == 0) {
            utf8_g = nodem_state->utf8 = false;
        } else if (strcasecmp(*data_charset, "utf-8") == 0 || strcasecmp(*data_charset, "utf8") == 0) {
            utf8_g = nodem_state->utf8 = true;
        }

        if (nodem_state->debug > LOW) {
            char* encoding = (char*) "utf-8";

            if (nodem_state->utf8 == false) encoding = (char*) "m";

            debug_log(">>   charset: ", encoding);
        }

        if (has_n(isolate, arg_object, new_string_n(isolate, "signalHandler"))) {
            if (get_n(isolate, arg_object, new_string_n(isolate, "signalHandler"))->IsObject()) {
                Local<Object> signal_handler =
                  to_object_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "signalHandler")));

                if (has_n(isolate, signal_handler, new_string_n(isolate, "SIGINT"))) {
                    signal_sigint_g = boolean_value_n(isolate, get_n(isolate, signal_handler, new_string_n(isolate, "SIGINT")));
                } else if (has_n(isolate, signal_handler, new_string_n(isolate, "sigint"))) {
                    signal_sigint_g = boolean_value_n(isolate, get_n(isolate, signal_handler, new_string_n(isolate, "sigint")));
                }

                if (has_n(isolate, signal_handler, new_string_n(isolate, "SIGQUIT"))) {
                    signal_sigquit_g = boolean_value_n(isolate, get_n(isolate, signal_handler, new_string_n(isolate, "SIGQUIT")));
                } else if (has_n(isolate, signal_handler, new_string_n(isolate, "sigquit"))) {
                    signal_sigquit_g = boolean_value_n(isolate, get_n(isolate, signal_handler, new_string_n(isolate, "sigquit")));
                }

                if (has_n(isolate, signal_handler, new_string_n(isolate, "SIGTERM"))) {
                    signal_sigterm_g = boolean_value_n(isolate, get_n(isolate, signal_handler, new_string_n(isolate, "SIGTERM")));
                } else if (has_n(isolate, signal_handler, new_string_n(isolate, "sigterm"))) {
                    signal_sigterm_g = boolean_value_n(isolate, get_n(isolate, signal_handler, new_string_n(isolate, "sigterm")));
                }
            } else {
                Local<Value> signal_handler = get_n(isolate, arg_object, new_string_n(isolate, "signalHandler"));

                signal_sigint_g = signal_sigquit_g = signal_sigterm_g = boolean_value_n(isolate, signal_handler);
            }

            if (nodem_state->debug > LOW) {
                debug_log(">>   SIGINT: ", boolalpha, signal_sigint_g);
                debug_log(">>   SIGQUIT: ", boolalpha, signal_sigquit_g);
                debug_log(">>   SIGTERM: ", boolalpha, signal_sigterm_g);
            }
        }

        Local<Value> threadpool_size = get_n(isolate, arg_object, new_string_n(isolate, "threadpoolSize"));

        if (!threadpool_size->IsUndefined() && (threadpool_size->IsNumber() || threadpool_size->IsString())) {
            if (nodem_state->debug > LOW) debug_log(">>   threadpoolSize: ", *(UTF8_VALUE_TEMP_N(isolate, threadpool_size)));

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
        if (sigaction(SIGINT, NULL, &nodem_state->signal_attr) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot retrieve SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, NULL, &nodem_state->signal_attr) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot retrieve SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, NULL, &nodem_state->signal_attr) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot retrieve SIGTERM handler")));
            return;
        }
    }

    if ((save_stdout_g = dup(STDOUT_FILENO)) == -1) {
        char error[BUFSIZ];

        cerr << strerror_r(errno, error, BUFSIZ);
    }

    uv_mutex_lock(&mutex_g);

    if (nodem_state->debug > LOW) {
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

        if (nodem_state->debug > LOW) {
            funlockfile(stderr);

            if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }
        }

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
        return;
    }

    if (nodem_state->debug > LOW) {
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

    gtm_status_t status;

    uv_mutex_lock(&mutex_g);

    gtm_char_t debug[] = "debug";

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = debug;
    access.rtn_name.length = strlen(debug);
    access.handle = NULL;

    status = gtm_cip(&access, nodem_state->debug);
#else
    status = gtm_ci(debug, nodem_state->debug);
#endif

    if (nodem_state->debug > LOW) debug_log(">>   status: ", status);

    if (status != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
        return;
    }

    uv_mutex_unlock(&mutex_g);

    nodem_state_g = OPEN;

    Local<Object> result = Object::New(isolate);
    set_n(isolate, result, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
    set_n(isolate, result, new_string_n(isolate, "pid"), Number::New(isolate, nodem_state->pid));
    set_n(isolate, result, new_string_n(isolate, "tid"), Number::New(isolate, nodem_state->tid));

    info.GetReturnValue().Set(result);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::open exit\n");

    return;
} // @end nodem::Nodem::open method

/*
 * @method nodem::Nodem::configure
 * @summary Configure per-thread parameters of the YottaDB/GT.M connection
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::configure(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    if (info.Length() >= 1 && !info[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = Object::New(isolate);

    if (info[0]->IsObject()) arg_object = to_object_n(isolate, info[0]);

    if (has_n(isolate, arg_object, new_string_n(isolate, "debug"))) {
        UTF8_VALUE_N(isolate, debug, get_n(isolate, arg_object, new_string_n(isolate, "debug")));

        if (strcasecmp(*debug, "off") == 0) {
            nodem_state->debug = OFF;
        } else if (strcasecmp(*debug, "low") == 0) {
            nodem_state->debug = LOW;
        } else if (strcasecmp(*debug, "medium") == 0) {
            nodem_state->debug = MEDIUM;
        } else if (strcasecmp(*debug, "high") == 0) {
            nodem_state->debug = HIGH;
        } else {
            nodem_state->debug = static_cast<debug_t>
              (uint32_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "debug"))));

            if (nodem_state->debug < 0) {
                nodem_state->debug = OFF;
            } else if (nodem_state->debug > 3) {
                nodem_state->debug = HIGH;
            }
        }
    }

    if (nodem_state->debug > OFF) {
        debug_log(">  Nodem::configure enter");

        char* debug_display = (char*) "off";

        if (nodem_state->debug == LOW) {
            debug_display = (char*) "low";
        } else if (nodem_state->debug == MEDIUM) {
            debug_display = (char*) "medium";
        } else {
            debug_display = (char*) "high";
        }

        debug_log(">  debug: ", debug_display);
    }

    if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink"))) {
        nodem_state->auto_relink = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));
    }

    if (nodem_state->debug > LOW) debug_log(">>   autoRelink: ", boolalpha, nodem_state->auto_relink);

    if (has_n(isolate, arg_object, new_string_n(isolate, "mode"))) {
        UTF8_VALUE_N(isolate, nodem_mode, get_n(isolate, arg_object, new_string_n(isolate, "mode")));

        if (strcasecmp(*nodem_mode, "strict") == 0) {
            nodem_state->mode = STRING;

            if (nodem_state->debug > OFF || !(deprecated_g & STRICT)) {
                deprecated_g |= STRICT;
                debug_log(">>   mode: strict [DEPRECATED - Use string instead]");
            }
        } else if (strcasecmp(*nodem_mode, "string") == 0) {
            nodem_state->mode = STRING;
        } else if (strcasecmp(*nodem_mode, "canonical") == 0) {
            nodem_state->mode = CANONICAL;
        }
    }

    if (nodem_state->debug > LOW) {
        if (nodem_state->mode == STRING) {
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
            nodem_state->utf8 = false;
        } else if (strcasecmp(*data_charset, "utf-8") == 0 || strcasecmp(*data_charset, "utf8") == 0) {
            nodem_state->utf8 = true;
        }
    } else if (has_n(isolate, arg_object, new_string_n(isolate, "encoding"))) {
        Local<Value> encoding = get_n(isolate, arg_object, new_string_n(isolate, "encoding"));
        UTF8_VALUE_N(isolate, data_encoding, encoding);

        if (strcasecmp(*data_encoding, "m") == 0 || strcasecmp(*data_encoding, "binary") == 0 ||
          strcasecmp(*data_encoding, "ascii") == 0) {
            nodem_state->utf8 = false;
        } else if (strcasecmp(*data_encoding, "utf-8") == 0 || strcasecmp(*data_encoding, "utf8") == 0) {
            nodem_state->utf8 = true;
        }
    }

    if (nodem_state->debug > LOW) {
        char* charset = (char*) "utf-8";

        if (nodem_state->utf8 == false) charset = (char*) "m";

        debug_log(">>   charset: ", charset);
    }

    if (has_n(isolate, arg_object, new_string_n(isolate, "debug"))) {
        gtm_status_t status;

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        gtm_char_t debug[] = "debug";

#if NODEM_CIP_API == 1
        ci_name_descriptor access;

        access.rtn_name.address = debug;
        access.rtn_name.length = strlen(debug);
        access.handle = NULL;

        status = gtm_cip(&access, nodem_state->debug);
#else
        status = gtm_ci(debug, nodem_state->debug);
#endif

        if (nodem_state->debug > LOW) debug_log(">>   status: ", status);

        if (status != EXIT_SUCCESS) {
            gtm_char_t msg_buf[ERR_LEN];
            gtm_zstatus(msg_buf, ERR_LEN);

            if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);

            info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
            return;
        }

        if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);
    }

    Local<Object> result = Object::New(isolate);
    set_n(isolate, result, new_string_n(isolate, "ok"), Boolean::New(isolate, true));
    set_n(isolate, result, new_string_n(isolate, "pid"), Number::New(isolate, nodem_state->pid));
    set_n(isolate, result, new_string_n(isolate, "tid"), Number::New(isolate, nodem_state->tid));

    info.GetReturnValue().Set(result);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::configure exit\n");

    return;
} // @end nodem::Nodem::configure method

/*
 * @method nodem::Nodem::close
 * @summary Close a connection with YottaDB/GT.M
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::close(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::close enter");

    if (nodem_state->pid != nodem_state->tid) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection must be managed by main thread")));
        return;
    } else if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    uv_mutex_lock(&mutex_g);

    if (info[0]->IsObject() && has_n(isolate, to_object_n(isolate, info[0]), new_string_n(isolate, "resetTerminal"))) {
        reset_term_g = boolean_value_n(isolate, get_n(isolate, to_object_n(isolate, info[0]),
                       new_string_n(isolate, "resetTerminal")));
    }

    if (nodem_state->debug > LOW) debug_log(">>   resetTerminal: ", boolalpha, reset_term_g);

#if NODEM_SIMPLE_API == 1
    if (ydb_exit() != YDB_OK) {
#else
    if (gtm_exit() != EXIT_SUCCESS) {
#endif
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
        return;
    }

    nodem_state_g = CLOSED;

    if (unistd_close(save_stdout_g) == -1) {
        char error[BUFSIZ];

        cerr << strerror_r(errno, error, BUFSIZ);
    }

    uv_mutex_unlock(&mutex_g);

    if (signal_sigint_g == true) {
        if (sigaction(SIGINT, &nodem_state->signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, &nodem_state->signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Cannot initialize SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, &nodem_state->signal_attr, NULL) == -1) {
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

    info.GetReturnValue().Set(Undefined(isolate));

    if (nodem_state->debug > OFF) debug_log(">  Nodem::close exit\n");

    return;
} // @end nodem::Nodem::close method

/*
 * @method nodem::Nodem::help
 * @summary Built-in help menu for Nodem methods
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::help(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "open"))) {
        cout << REVSE "open" RESET " method: "
            "Open connection to " NODEM_DB " - all methods, except for help and version, require an open connection\n\n"
            "Required arguments:\n"
            "None\n\n"
            "Optional arguments:\n"
            "{\n"
            "\tglobalDirectory|namespace:\t{string} <none>,\n"
            "\troutinesPath:\t\t\t{string} <none>,\n"
            "\tcallinTable:\t\t\t{string} <none>,\n"
            "\tipAddress:\t\t\t{string} <none>,\n"
            "\ttcpPort:\t\t\t{number} <none>,\n"
            "\tcharset|encoding:\t\t{string} [<utf8|utf-8>|m|binary|ascii]/i,\n"
            "\tmode:\t\t\t\t{string} [<canonical>|string]/i,\n"
            "\tautoRelink:\t\t\t{boolean} <false>,\n"
            "\tdebug:\t\t\t\t{boolean} <false>|{string} [<off>|low|medium|high]/i|{number} [<0>|1|2|3],\n"
            "\tthreadpoolSize:\t\t\t{number} [1-1024] <4>,\n"
            "\tsignalHandler:\t\t\t{boolean} <true>|{object}\n"
            "\t{\n"
            "\t\tsigint|SIGINT:\t\t{boolean} <true>,\n"
            "\t\tsigterm|SIGTERM:\t{boolean} <true>,\n"
            "\t\tsigquit|SIGQUIT:\t{boolean} <true>\n"
            "\t}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tpid:\t\t\t\t{number},\n"
            "\ttid:\t\t\t\t{number}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the open method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "configure"))) {
        cout << REVSE "configure" RESET " method: "
            "Configure per-thread parameters of the connection to " NODEM_DB "\n\n"
            "Required arguments:\n"
            "None\n\n"
            "Optional arguments:\n"
            "{\n"
            "\tcharset|encoding:\t\t{string} [<utf8|utf-8>|m|binary|ascii]/i,\n"
            "\tmode:\t\t\t\t{string} [<canonical>|string]/i,\n"
            "\tautoRelink:\t\t\t{boolean} <false>,\n"
            "\tdebug:\t\t\t\t{boolean} <false>|{string} [<off>|low|medium|high]/i|{number} [<0>|1|2|3]\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tpid:\t\t\t\t{number},\n"
            "\ttid:\t\t\t\t{number}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the configure method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "close"))) {
        cout << REVSE "close" RESET " method: "
            "Close connection to " NODEM_DB " - once closed, cannot be reopened in the current process\n\n"
            "Required arguments:\n"
            "None\n\n"
            "Optional arguments:\n"
            "{\n"
            "\tresetTerminal:\t\t\t{boolean} <false>\n"
            "}\n\n"
            "Returns on success:\n"
            "{undefined}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the close method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "version"))) {
        cout << REVSE "version" RESET " or " REVSE "about" RESET " method: "
            "Display Nodem version - includes " NODEM_DB " version if connection has been established\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments:\n"
            "None\n\n"
            "Returns on success:\n"
            "{string}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the version/about method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "data"))) {
        cout << REVSE "data" RESET " method: "
            "Retrieve information about the existence of data and/or children in global or local variables\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tdefined:\t\t\t{number} [0|1|10|11]\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{number} [0|1|10|11]\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the data method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "get"))) {
        cout << REVSE "get" RESET " method: "
            "Retrieve the data stored at a global or local node, or in an intrinsic special variable (ISV)\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tdata:\t\t\t\t{number|string},\n"
            "\tdefined:\t\t\t{boolean}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|$ISV|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{number|string}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the get method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "set"))) {
        cout << REVSE "set" RESET " method: "
            "Store data in a global or local node, or in an intrinsic special variable (ISV)\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}},\n"
            "\tdata:\t\t\t\t(required) {number|string}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tdata:\t\t\t\t{number|string}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|$ISV|local, [subscripts+], data\n\n"
            "Returns on success:\n"
            "{undefined}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the set method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "kill"))) {
        cout << REVSE "kill" RESET " method: "
            "Remove data stored in a global or global node, or in a local or local node, or remove all local variables\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Required arguments:\n"
            "None - Without an argument, will clear the entire local symbol table for that process\n\n"
            "Returns on success:\n"
            "{undefined}\n\n"
            "Optional arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}},\n"
            "\tnodeOnly:\t\t\t(optional) {boolean} <false>\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tnodeOnly:\t\t\t{boolean}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{undefined}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the kill method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "merge"))) {
        cout << REVSE "merge" RESET " method: "
            "Copy the data from all of the nodes in a global or local tree, to another global or local tree\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Required arguments:\n"
            "{\n"
            "\tfrom:\n"
            "\t{\n"
            "\t\tglobal|local:\t\t(required) {string},\n"
            "\t\tsubscripts:\t\t(optional) {array {number|string}}\n"
            "\t},\n"
            "\tto:\n"
            "\t{\n"
            "\t\tglobal|local:\t\t(required) {string},\n"
            "\t\tsubscripts:\t\t(optional) {array {number|string}}\n"
            "\t}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tfrom:\n"
            "\t{\n"
            "\t\tglobal|local:\t\t{string},\n"
            "\t\tsubscripts:\t\t{array {number|string}}\n"
            "\t},\n"
            "\tto:\n"
            "\t{\n"
            "\t\tglobal|local:\t\t{string},\n"
            "\t\tsubscripts:\t\t{array {number|string}}\n"
            "\t}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the merge method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "order"))) {
        cout << REVSE "order" RESET " or " REVSE "next" RESET " method: "
            "Retrieve the next node, at the current subscript level\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tresult:\t\t\t\t{number|string}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{number|string}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the order/next method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "previous"))) {
        cout << REVSE "previous" RESET " method: "
            "Retrieve the previous node, at the current subscript level\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tresult:\t\t\t\t{number|string}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{number|string}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the previous method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "nextNode"))) {
        cout << REVSE "nextNode" RESET " method: "
            "Retrieve the next node, regardless of subscript level\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tdata:\t\t\t\t{number|string},\n"
            "\tdefined:\t\t\t{boolean}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{array {number|string}}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the nextNode method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "previousNode"))) {
        cout << REVSE "previousNode" RESET " method: "
            "Retrieve the previous node, regardless of subscript level\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tdata:\t\t\t\t{number|string},\n"
            "\tdefined:\t\t\t{boolean}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{array {number|string}}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the previousNode method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "increment"))) {
        cout << REVSE "increment" RESET " method: "
            "Atomically increment or decrement a global or local data node\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}},\n"
            "\tincrement:\t\t\t(optional) {number} <1>\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\tincrement:\t\t\t{number},\n"
            "\tdata:\t\t\t\t{number|string}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{number|string}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the increment method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "lock"))) {
        cout << REVSE "lock" RESET " method: "
            "Lock a global or local tree, or individual node, incrementally - locks are advisory, not mandatory\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}},\n"
            "\ttimeout:\t\t\t(optional) {number} <Infinity>\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}},\n"
            "\ttimeout:\t\t\t{number},\n"
            "\tresult:\t\t\t\t{boolean}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{boolean}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the lock method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "unlock"))) {
        cout << REVSE "unlock" RESET " method: "
            "Unlock a global or local tree, or individual node, incrementally; or release all locks held by a process\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Required arguments:\n"
            "None - Without an argument, will clear the entire lock table for that process\n\n"
            "Returns on success:\n"
            "{undefined}\n\n"
            "Optional arguments - via object:\n"
            "{\n"
            "\tglobal|local:\t\t\t(required) {string},\n"
            "\tsubscripts:\t\t\t(optional) {array {number|string}}\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tglobal|local:\t\t\t{string},\n"
            "\tsubscripts:\t\t\t{array {number|string}}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "^global|local, [subscripts+]\n\n"
            "Returns on success:\n"
            "{undefined}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the unlock method, please refer to the README.md file\n"
            << endl;
#if NODEM_SIMPLE_API == 1
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "transaction"))) {
        cout << REVSE "transaction" RESET " method: "
            "Run a function containing Nodem API calls as an ACID transaction in YottaDB - synchronous only\n\n"
            "Required arguments:\n"
            "{function} - A JavaScript function, taking no arguments, which will be run in a YottaDB transaction\n\n"
            "Optional arguments - via object:\n"
            "{\n"
            "\tvariables:\t\t\t{array {string}},\n"
            "\ttype:\t\t\t\t{string} Batch|batch|BATCH\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tstatusCode:\t\t\t{number},\n"
            "\tstatusMessage:\t\t\t{string}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            " - tpRollback and tpRestart are provided as convenience properties on the instance object\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the transaction method, please refer to the README.md file\n"
            << endl;
#endif
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "function"))) {
        cout << REVSE "function" RESET " method: "
            "Call a " NODEM_DB " extrinsic function\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tfunction:\t\t\t(required) {string},\n"
            "\targuments:\t\t\t(optional) {array {number|string|empty}},\n"
            "\tautoRelink:\t\t\t(optional) {boolean} <false>\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tfunction:\t\t\t{string},\n"
            "\targuments:\t\t\t{array {number|string|empty}},\n"
            "\tautoRelink:\t\t\t{boolean},\n"
            "\tresult:\t\t\t\t{number|string}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "function, [arguments+]\n\n"
            "Returns on success:\n"
            "{number|string}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the function method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "procedure"))) {
        cout << REVSE "procedure" RESET " or " REVSE "routine" RESET " method: "
            "Call a " NODEM_DB " routine label\n"
            " - Passing a function, taking two arguments (error and result), as the last argument, calls the API asynchronously\n"
            " - Callbacks return `error === {null}` on success, and `result === {undefined}` on failure\n\n"
            "Arguments - via object:\n"
            "{\n"
            "\tprocedure|routine:\t\t(required) {string},\n"
            "\targuments:\t\t\t(optional) {array {number|string|empty}},\n"
            "\tautoRelink:\t\t\t(optional) {boolean} <false>\n"
            "}\n\n"
            "Returns on success:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} true,\n"
            "\tprocedure|routine:\t\t{string},\n"
            "\targuments:\t\t\t{array {number|string|empty}},\n"
            "\tautoRelink:\t\t\t{boolean}\n"
            "}\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            "Arguments - via position:\n"
            "procedure, [arguments+]\n\n"
            "Returns on success:\n"
            "{undefined}\n\n"
            "Returns on failure:\n"
            "{Error object}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the procedure/routine method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "globalDirectory"))) {
        cout << REVSE "globalDirectory" RESET " method: "
            "List globals stored in the database\n\n"
            "Required arguments:\n"
            "None - Without an argument, will list all the globals stored in the database\n\n"
            "Optional arguments:\n"
            "{\n"
            "\tmax:\t\t\t\t{number},\n"
            "\tlo:\t\t\t\t{string},\n"
            "\thi:\t\t\t\t{string}\n"
            "}\n\n"
            "Returns on success:\n"
            "[\n"
            "\t<global variable name>*\t\t{string}\n"
            "]\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the globalDirectory method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "localDirectory"))) {
        cout << REVSE "localDirectory" RESET " method: "
            "List local variables stored in the symbol table\n\n"
            "Required arguments:\n"
            "None - Without an argument, will list all the variables in the local symbol table\n\n"
            "Optional arguments:\n"
            "{\n"
            "\tmax:\t\t\t\t{number},\n"
            "\tlo:\t\t\t\t{string},\n"
            "\thi:\t\t\t\t{string}\n"
            "}\n\n"
            "Returns on success:\n"
            "[\n"
            "\t<local variable name>*\t\t{string}\n"
            "]\n\n"
            "Returns on failure:\n"
            "{\n"
            "\tok:\t\t\t\t{boolean} false,\n"
            "\terrorCode:\t\t\t{number},\n"
            "\terrorMessage:\t\t\t{string}\n"
            "}\n\n"
            " - Some failures can result in thrown exceptions and/or stack traces\n"
            "For more information about the localDirectory method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "retrieve"))) {
        cout << REVSE "retrieve" RESET " method: "
            "Retrieve a global or local tree structure as an object - NOT YET IMPLEMENTED\n"
            << endl;
    } else if (to_string_n(isolate, info[0])->StrictEquals(new_string_n(isolate, "update"))) {
        cout << REVSE "update" RESET " method: "
            "Store an object as a global or local tree structure - NOT YET IMPLEMENTED\n"
            << endl;
    } else {
        cout << REVSE "NodeM" RESET " API Help Menu - Methods:\n\n"
            "open\t\t\tOpen connection to " NODEM_DB " - all methods, except for help and version, require an open connection\n"
            "configure\t\tConfigure per-thread parameters of the connection to " NODEM_DB "\n"
            "close\t\t\tClose connection to " NODEM_DB " - once closed, cannot be reopened in the current process\n"
            "version\t\t\tDisplay Nodem version - includes " NODEM_DB " version if connection has been established (AKA about)\n"
            "data\t\t\tRetrieve information about the existence of data and/or children in global or local variables\n"
            "get\t\t\tRetrieve the data stored at a global or local node, or in an intrinsic special variable (ISV)\n"
            "set\t\t\tStore data in a global or local node, or in an intrinsic special variable (ISV)\n"
            "kill\t\t\tRemove data stored in a global or global node, or in a local or local node; or remove all local variables\n"
            "merge\t\t\tCopy the data from all of the nodes in a global or local tree, to another global or local tree\n"
            "order\t\t\tRetrieve the next node, at the current subscript level (AKA next)\n"
            "previous\t\tRetrieve the previous node, at the current subscript level\n"
            "nextNode\t\tRetrieve the next node, regardless of subscript level\n"
            "previousNode\t\tRetrieve the previous node, regardless of subscript level\n"
            "increment\t\tAtomically increment or decrement a global or local data node\n"
            "lock\t\t\tLock a global or local tree, or individual node, incrementally - locks are advisory, not mandatory\n"
            "unlock\t\t\tUnlock a global or local tree, or individual node, incrementally; or release all locks held by a process\n"
#if NODEM_SIMPLE_API == 1
            "transaction\t\tRun a function containing Nodem API calls as an ACID transaction in YottaDB - synchronous only\n"
#endif
            "function\t\tCall a " NODEM_DB " extrinsic function\n"
            "procedure\t\tCall a " NODEM_DB " routine label (AKA routine)\n"
            "globalDirectory\t\tList globals stored in the database\n"
            "localDirectory\t\tList local variables stored in the symbol table\n"
            "retrieve\t\tRetrieve a global or local tree structure as an object - NOT YET IMPLEMENTED\n"
            "update\t\t\tStore an object as a global or local tree structure - NOT YET IMPLEMENTED\n\n"
            "For more information about each method, call help with the method name as an argument\n"
            << endl;
    }

    info.GetReturnValue().Set(new_string_n(isolate, "NodeM - Copyright (C) 2012-2024 Fourth Watch Software LC"));
    return;
} // @end nodem::Nodem::help method

/*
 * @method nodem::Nodem::version
 * @summary Return the about/version string
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::version(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::version enter");

    bool async = false;

    if (info[0]->IsFunction()) {
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
    }

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[0]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, Undefined(isolate));
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = NODEM_VERSION;
    nodem_baton->async = async;
    nodem_baton->status = 0;
    nodem_baton->nodem_function = &gtm::version;
    nodem_baton->ret_function = &nodem::version;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into ", NODEM_DB);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::version exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from ", NODEM_DB);

    if (nodem_baton->status != EXIT_SUCCESS) {
        isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(nodem_baton->error, true, async, nodem_state))));
        info.GetReturnValue().Set(Undefined(isolate));

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into version");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::version exit\n");

    return;
} // @end nodem::Nodem::version

/*
 * @method nodem::Nodem::data
 * @summary Check if global or local node has data and/or children or not
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::data(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::data enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::data;
#else
    nodem_baton->nodem_function = &gtm::data;
#endif
    nodem_baton->ret_function = &nodem::data;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::data exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error( to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into data");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::data exit\n");

    return;
} // @end nodem::Nodem::data method

/*
 * @method nodem::Nodem::get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::get(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::get enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::get;
#else
    nodem_baton->nodem_function = &gtm::get;
#endif
    nodem_baton->ret_function = &nodem::get;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::get exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK && nodem_baton->status != YDB_ERR_GVUNDEF && nodem_baton->status != YDB_ERR_LVUNDEF) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into get");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::get exit\n");

    return;
} // @end nodem::Nodem::get method

/*
 * @method nodem::Nodem::set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::set(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::set enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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

    data_node = encode_arguments(data_array, nodem_state);
#endif

    if (data_node->IsSymbol() || data_node->IsSymbolObject() || data_node->IsObject() ||
      data_node->IsArray() || data_node->IsUndefined()) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Property 'data' contains invalid data")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub, value;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
        value = *(UTF8_VALUE_TEMP_N(isolate, data_node));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};
        NodemValue nodem_data_node {data_node};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
        value = nodem_data_node.to_byte();
    }

#if NODEM_SIMPLE_API == 1
    if (nodem_state->mode == CANONICAL && data_value->IsNumber()) {
        if (value.substr(0, 2) == "0.") value = value.substr(1, string::npos);
        if (value.substr(0, 3) == "-0.") value = "-" + value.substr(2, string::npos);
    }
#endif

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, data_value);
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->value = value;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::set;
#else
    nodem_baton->nodem_function = &gtm::set;
#endif
    nodem_baton->ret_function = &nodem::set;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::set exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into set");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::set exit\n");

    return;
} // @end nodem::Nodem::set method

/*
 * @method nodem::Nodem::kill
 * @summary Kill a global or local, or global or local node, or remove the entire symbol table
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::kill(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::kill enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
    }

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;
    bool node_only = false;

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

        if (has_n(isolate, arg_object, new_string_n(isolate, "nodeOnly"))) {
            node_only = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "nodeOnly")));
        }
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
        if (test[0] != '^') local = true;
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
            subs_array = build_subscripts(subscripts, error, nodem_state);

            if (error) {
                isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
                return;
            }
#else
            subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->node_only = node_only;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::kill;
#else
    nodem_baton->nodem_function = &gtm::kill;
#endif
    nodem_baton->ret_function = &nodem::kill;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::kill exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into kill");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::kill exit\n");

    return;
} // @end nodem::Nodem::kill method

/*
 * @method nodem::Nodem::merge
 * @summary Merge a global or local array tree to another global or local array tree
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::merge(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::merge enter");

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an argument")));
        return;
    } else if (!info[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = to_object_n(isolate, info[0]);
    Local<Value> from_object = get_n(isolate, arg_object, new_string_n(isolate, "from"));
    Local<Value> to_object = get_n(isolate, arg_object, new_string_n(isolate, "to"));
    bool from_local = false;
    bool to_local = false;

    if (!has_n(isolate, arg_object, new_string_n(isolate, "from"))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'from' property")));
        return;
    } else if (!from_object->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "'from' property must be an object")));
        return;
    }

    Local<Object> from = to_object_n(isolate, from_object);

    if (!has_n(isolate, arg_object, new_string_n(isolate, "to"))) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply a 'to' property")));
        return;
    } else if (!to_object->IsObject()) {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "'to' property must be an object")));
        return;
    }

    Local<Object> to = to_object_n(isolate, to_object);
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
        from_subs = encode_arguments(from_subscripts, nodem_state);

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
        to_subs = encode_arguments(to_subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, from_glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' in 'from' is an invalid name")));
            return;
        }

        from_name_msg = ">>   from_local: ";
        from_name = localize_name(from_glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, from_name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' in 'from' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, from_glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'global' in 'from' is an invalid name")));
            return;
        }

        from_name_msg = ">>   from_global: ";
        from_name = globalize_name(from_glvn, nodem_state);
    }

    const char* to_name_msg;
    Local<Value> to_name;

    if (to_local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, to_glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' in 'to' is an invalid name")));
            return;
        }

        to_name_msg = ">>   to_local: ";
        to_name = localize_name(to_glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, to_name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'local' in 'to' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, to_glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'global' in 'to' is an invalid name")));
            return;
        }

        to_name_msg = ">>   to_global: ";
        to_name = globalize_name(to_glvn, nodem_state);
    }

    string from_gvn, from_sub, to_gvn, to_sub;

    if (nodem_state->utf8 == true) {
        from_gvn = *(UTF8_VALUE_TEMP_N(isolate, from_name));
        from_sub = *(UTF8_VALUE_TEMP_N(isolate, from_subs));
        to_gvn = *(UTF8_VALUE_TEMP_N(isolate, to_name));
        to_sub = *(UTF8_VALUE_TEMP_N(isolate, to_subs));

        if (nodem_state->debug > LOW) {
            debug_log(from_name_msg, from_gvn);
            debug_log(">>   from_subscripts: ", from_sub);
            debug_log(to_name_msg, to_gvn);
            debug_log(">>   to_subscripts: ", to_sub);
        }
    } else {
        NodemValue gtm_from_name {from_name};
        NodemValue gtm_from_subs {from_subs};
        NodemValue gtm_to_name {to_name};
        NodemValue gtm_to_subs {to_subs};

        from_gvn = gtm_from_name.to_byte();
        from_sub = gtm_from_subs.to_byte();
        to_gvn = gtm_to_name.to_byte();
        to_sub = gtm_to_subs.to_byte();

        if (nodem_state->debug > LOW) {
            debug_log(from_name_msg, from_gvn);
            debug_log(">>   from_subscripts: ", from_sub);
            debug_log(to_name_msg, to_gvn);
            debug_log(">>   to_subscripts: ", to_sub);
        }
    }

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->object_p.Reset(isolate, arg_object);
    nodem_baton->arguments_p.Reset(isolate, Undefined(isolate));
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = from_gvn;
    nodem_baton->args = from_sub;
    nodem_baton->to_name = to_gvn;
    nodem_baton->to_args = to_sub;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = from_local;
    nodem_baton->status = 0;
    nodem_baton->nodem_function = &gtm::merge;
    nodem_baton->ret_function = &nodem::merge;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::merge exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

    if (nodem_baton->status != EXIT_SUCCESS) {
        info.GetReturnValue().Set(error_status(nodem_baton->error, false, async, nodem_state));

        nodem_baton->object_p.Reset();
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into merge");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->object_p.Reset();
    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::merge exit\n");

    return;
} // @end nodem::Nodem::merge method

/*
 * @method nodem::Nodem::order
 * @summary Return the next global or local node at the same level
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::order(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::order enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::order;
#else
    nodem_baton->nodem_function = &gtm::order;
#endif
    nodem_baton->ret_function = &nodem::order;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::order exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK && nodem_baton->status != YDB_NODE_END) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into order");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::order exit\n");

    return;
} // @end nodem::Nodem::order method

/*
 * @method nodem::Nodem::previous
 * @summary Return the previous global or local node at the same level
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::previous(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::previous enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::previous;
#else
    nodem_baton->nodem_function = &gtm::previous;
#endif
    nodem_baton->ret_function = &nodem::previous;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::previous exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK && nodem_baton->status != YDB_NODE_END) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into previous");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::previous exit\n");

    return;
} // @end nodem::Nodem::previous method

/*
 * @method nodem::Nodem::next_node_deprecated
 * @summary Calls nodem::next_node after logging that this method is deprecated
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::next_node_deprecated(const FunctionCallbackInfo<Value>& info)
{
    if (reinterpret_cast<NodemState*>(info.Data().As<External>()->Value())->debug > OFF || !(deprecated_g & NEXT)) {
        deprecated_g |= NEXT;
        debug_log(">  next_node [DEPRECATED - Use nextNode instead]");
    }

    return Nodem::next_node(info);
}

/*
 * @method nodem::Nodem::next_node
 * @summary Return the next global or local node, depth first
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::next_node(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::next_node enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, Undefined(isolate));
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::next_node;
#else
    nodem_baton->nodem_function = &gtm::next_node;
#endif
    nodem_baton->ret_function = &nodem::next_node;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::next_node exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK && nodem_baton->status != YDB_NODE_END) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into next_node");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::next_node exit\n");

    return;
} // @end nodem::Nodem::next_node method

/*
 * @method nodem::Nodem::previous_node_deprecated
 * @summary Calls nodem::previous_node after logging that this method is deprecated
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::previous_node_deprecated(const FunctionCallbackInfo<Value>& info)
{
    if (reinterpret_cast<NodemState*>(info.Data().As<External>()->Value())->debug > OFF || !(deprecated_g & PREVIOUS)) {
        deprecated_g |= PREVIOUS;
        debug_log(">  previous_node [DEPRECATED - Use previousNode instead]");
    }

    return Nodem::previous_node(info);
}

/*
 * @method nodem::Nodem::previous_node
 * @summary Same as Nodem::next_node, only in reverse
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::previous_node(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::previous_node enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, Undefined(isolate));
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::previous_node;
#else
    nodem_baton->nodem_function = &gtm::previous_node;
#endif
    nodem_baton->ret_function = &nodem::previous_node;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::previous_node exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK && nodem_baton->status != YDB_NODE_END) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into previous_node");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::previous_node exit\n");

    return;
} // @end nodem::Nodem::previous_node method

/*
 * @method nodem::Nodem::increment
 * @summary Increment or decrement the number in a global or local node
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::increment(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::increment enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        } else if (args_cnt > 1) {
            increment = info[1];

            if (!increment->IsUndefined() && !(deprecated_g & INCREMENT)) {
                deprecated_g |= INCREMENT;
                debug_log(">>   increment by-position [DEPRECATED - Use increment property instead]");
            }
        }

        // Make sure JavaScript numbers that M won't recognize are changed to 0
        string test = *(UTF8_VALUE_TEMP_N(isolate, increment));

        if (!all_of(test.begin(), test.end(), [](char c) {return (isdigit(c) || c == '-' || c == '.');})) {
            increment = Number::New(isolate, 0);
        } else if (!increment->IsNumber()) {
            increment = Number::New(isolate, 0);
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->option = number_value_n(isolate, increment);
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::increment;
#else
    nodem_baton->nodem_function = &gtm::increment;
#endif
    nodem_baton->ret_function = &nodem::increment;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::increment exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into increment");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::increment exit\n");

    return;
} // @end nodem::Nodem::increment method

/*
 * @method nodem::Nodem::lock
 * @summary Lock a global or local node, incrementally
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::lock(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::lock enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        } else if (args_cnt > 1) {
            timeout = info[1];

            if (!timeout->IsUndefined() && !(deprecated_g & TIMEOUT)) {
                deprecated_g |= TIMEOUT;
                debug_log(">>   timeout by-position [DEPRECATED - Use timeout property instead]");
            }
        }

        // Make sure JavaScript numbers that M won't recognize are changed to 0
        string test = *(UTF8_VALUE_TEMP_N(isolate, timeout));

        if (!all_of(test.begin(), test.end(), [](char c) {return (isdigit(c) || c == '-' || c == '.');})) {
            if (test == "Infinity") {
                timeout = Number::New(isolate, -1);
            } else {
                timeout = Number::New(isolate, 0);
            }
        } else if (!timeout->IsNumber() || number_value_n(isolate, timeout) < -1) {
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
        if (test[0] != '^') local = true;
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
        subs_array = build_subscripts(subscripts, error, nodem_state);

        if (error) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
            return;
        }
#else
        subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->option = number_value_n(isolate, timeout);
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::lock;
#else
    nodem_baton->nodem_function = &gtm::lock;
#endif
    nodem_baton->ret_function = &nodem::lock;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::lock exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into lock");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::lock exit\n");

    return;
} // @end nodem::Nodem::lock method

/*
 * @method nodem::Nodem::unlock
 * @summary Unlock a global or local node, incrementally, or release all locks
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::unlock(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::unlock enter");

#if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
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
        if (test[0] != '^') local = true;
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
            subs_array = build_subscripts(subscripts, error, nodem_state);

            if (error) {
                isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Subscripts contain invalid data")));
                return;
            }
#else
            subs = encode_arguments(subscripts, nodem_state);

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
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = ">>   local: ";
        name = localize_name(glvn, nodem_state);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = ">>   global: ";
        name = globalize_name(glvn, nodem_state);
    }

    string gvn, sub;

    if (nodem_state->utf8 == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_subs {subs};

        gvn = nodem_name.to_byte();
        sub = nodem_subs.to_byte();
    }

    if (nodem_state->debug > LOW) {
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

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, subscripts);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = gvn;
    nodem_baton->args = sub;
    nodem_baton->subs_array = subs_array;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_SIMPLE_API == 1
    nodem_baton->nodem_function = &ydb::unlock;
#else
    nodem_baton->nodem_function = &gtm::unlock;
#endif
    nodem_baton->ret_function = &nodem::unlock;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   mode: ", nodem_state->mode);

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::unlock exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

#if NODEM_SIMPLE_API == 1
    if (nodem_baton->status == -1) {
        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(new_string_n(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (nodem_baton->status != YDB_OK) {
#else
    if (nodem_baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into unlock");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::unlock exit\n");

    return;
} // @end nodem::Nodem::unlock method

#if NODEM_SIMPLE_API == 1
/*
 * @method nodem::Nodem::transaction
 * @summary Call a JavaScript function within a YottaDB transaction
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::transaction(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::transaction enter");

#   if YDB_RELEASE >= 126
    reset_handler(nodem_state);
#   endif

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    unsigned int args_cnt = info.Length();
    Local<Value> variables = Undefined(isolate);

    ydb_buffer_t vars_array[YDB_MAX_SUBS];
    unsigned int vars_size;
    string mode;

    if (args_cnt > 2) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Only two arguments are allowed")));
        return;
#   if NODE_MAJOR_VERSION >= 8 || (NODE_MAJOR_VERSION == 7 && NODE_MINOR_VERSION >= 6)
    } else if (info[0]->IsAsyncFunction()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Async function is not allowed")));
        return;
#   endif
    } else if (!info[0]->IsFunction()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function is required for first argument")));
        return;
    } else {
        mode = "NODEM";
        vars_size = 0;

        if (args_cnt == 2) {
            if (!info[1]->IsObject()) {
                isolate->ThrowException(Exception::Error(new_string_n(isolate, "Argument must be an object")));
                return;
            } else {
                Local<Object> arg_object = to_object_n(isolate, info[1]);
                Local<Value> type = get_n(isolate, arg_object, new_string_n(isolate, "type"));

                if (type->StrictEquals(new_string_n(isolate, "Batch")) || type->StrictEquals(new_string_n(isolate, "batch")) ||
                    type->StrictEquals(new_string_n(isolate, "BATCH"))) mode = "BATCH";

                variables = get_n(isolate, arg_object, new_string_n(isolate, "variables"));

                if (!variables->IsUndefined() && !variables->IsArray()) {
                    isolate->ThrowException(Exception::Error(new_string_n(isolate, "Variables must be in an array")));
                    return;
                }

                Local<Array> variables_array = Local<Array>::Cast(variables);
                vars_size = variables_array->Length();
                string vars_name;

                if (vars_size > YDB_MAX_SUBS) {
                    isolate->ThrowException(Exception::Error(new_string_n(
                      isolate, "Max of " NODEM_STRING(YDB_MAX_SUBS) "variables may be passed")));

                    return;
                }

                if (nodem_state->debug > LOW) debug_log(">>   vars_size: ", vars_size);

                for (unsigned int i = 0; i < vars_size; i++) {
                    vars_name = *(UTF8_VALUE_TEMP_N(isolate, get_n(isolate, variables_array, i)));

                    if (vars_name[0] == '^' || vars_name[0] == '$') {
                        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Variables must be local")));
                        return;
                    }

                    if (nodem_state->debug > LOW) debug_log(">>   vars_name[", i, "]: ", vars_name[i]);

                    vars_array[i].len_alloc = vars_array[i].len_used = vars_name.length();
                    vars_array[i].buf_addr = (char*) vars_name.c_str();
                }
            }
        }
    }

    if (nodem_state->debug > LOW) debug_log(">>   mode: ", mode);

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    nodem_baton = &new_baton;
    nodem_baton->request.data = nodem_baton;
    nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[0]));
    nodem_baton->nodem_state = nodem_state;
    nodem_baton->error = nodem_state->error;

    if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);
    if (nodem_state->debug > LOW) debug_log(">>   tp_level: ", nodem_state->tp_level);
    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    nodem_state->tp_level++;

    ydb_status_t status = ydb_tp_s(nodem::transaction, nodem_baton, mode.c_str(), vars_size, vars_array);

    nodem_state->tp_level--;

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);
    if (nodem_state->debug > LOW) debug_log(">>   tp_level: ", nodem_state->tp_level);
    if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);

    nodem_baton->callback_p.Reset();

    Local<Object> return_object = Object::New(isolate);

    set_n(isolate, return_object, new_string_n(isolate, "ok"), Boolean::New(isolate, true));

    if (status == YDB_OK) {
        set_n(isolate, return_object, new_string_n(isolate, "statusCode"), Number::New(isolate, status));
        set_n(isolate, return_object, new_string_n(isolate, "statusMessage"), new_string_n(isolate, "Commit"));

        info.GetReturnValue().Set(return_object);
    } else if (status == YDB_TP_ROLLBACK) {
        set_n(isolate, return_object, new_string_n(isolate, "statusCode"), Number::New(isolate, status));
        set_n(isolate, return_object, new_string_n(isolate, "statusMessage"), new_string_n(isolate, "Rollback"));

        info.GetReturnValue().Set(return_object);
    } else if (status == YDB_TP_RESTART) {
        set_n(isolate, return_object, new_string_n(isolate, "statusCode"), Number::New(isolate, status));
        set_n(isolate, return_object, new_string_n(isolate, "statusMessage"), new_string_n(isolate, "Restart"));

        info.GetReturnValue().Set(return_object);
    } else {
        ydb_zstatus(nodem_baton->error, ERR_LEN);

        info.GetReturnValue().Set(error_status(nodem_baton->error, false, false, nodem_state));
    }

    if (nodem_state->debug > OFF) debug_log(">  Nodem::transaction exit\n");

    return;
} // @end nodem::Nodem::transaction method
#endif

/*
 * @method nodem::Nodem::function
 * @summary Call an arbitrary extrinsic function
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::function(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::function enter");

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> function;
    Local<Value> arguments = Undefined(isolate);
    uint32_t relink = nodem_state->auto_relink;
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

        if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink"))) {
            relink = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));
        }
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
        args = encode_arguments(arguments, nodem_state, true);

        if (args->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Arguments contain invalid data")));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'arguments' must contain an array")));
        return;
    }

    Local<Value> name = globalize_name(function, nodem_state);

    string func_s, args_s;

    if (nodem_state->utf8 == true) {
        func_s = *(UTF8_VALUE_TEMP_N(isolate, name));
        args_s = *(UTF8_VALUE_TEMP_N(isolate, args));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_args {args};

        func_s = nodem_name.to_byte();
        args_s = nodem_args.to_byte();
    }

    if (nodem_state->debug > LOW) {
        debug_log(">>   function: ", func_s);
        debug_log(">>   arguments: ", args_s);
    }

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, arguments);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = func_s;
    nodem_baton->args = args_s;
    nodem_baton->relink = relink;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->status = 0;
#if NODEM_YDB == 1 && YDB_RELEASE >= 124
    nodem_baton->info = 32754;  // Subtract 12 for v4wResult=$$
#else
    nodem_baton->info = 8180;   // Subtract 12 for v4wResult=$$
#endif
    nodem_baton->nodem_function = &gtm::function;
    nodem_baton->ret_function = &nodem::function;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    if (nodem_state->debug > LOW) {
        debug_log(">>   relink: ", relink);
        debug_log(">>   mode: ", nodem_state->mode);
        debug_log(">>   info: ", nodem_baton->info);
    }

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::function exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

    if (nodem_baton->status != EXIT_SUCCESS) {
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into function");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::function exit\n");

    return;
} // @end nodem::Nodem::function method

/*
 * @method nodem::Nodem::procedure
 * @summary Call an arbitrary procedure/routine
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::procedure(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::procedure enter");

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_cnt = info.Length();

    if (info[args_cnt - 1]->IsFunction()) {
        --args_cnt;
        async = true;

        if (nodem_state->tp_level > 0) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Asynchronous call not allowed within a transaction")));
            return;
        }
    }

    if (args_cnt == 0) {
        isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> procedure;
    Local<Value> arguments = Undefined(isolate);
    uint32_t relink = nodem_state->auto_relink;
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

        if (has_n(isolate, arg_object, new_string_n(isolate, "autoRelink"))) {
            relink = boolean_value_n(isolate, get_n(isolate, arg_object, new_string_n(isolate, "autoRelink")));
        }
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
        args = encode_arguments(arguments, nodem_state, true);

        if (args->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(new_string_n(isolate, "Arguments contain invalid data")));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(new_string_n(isolate, "Property 'arguments' must contain an array")));
        return;
    }

    Local<Value> name = globalize_name(procedure, nodem_state);

    string proc_s, args_s;

    if (nodem_state->utf8 == true) {
        proc_s = *(UTF8_VALUE_TEMP_N(isolate, name));
        args_s = *(UTF8_VALUE_TEMP_N(isolate, args));
    } else {
        NodemValue nodem_name {name};
        NodemValue nodem_args {args};

        proc_s = nodem_name.to_byte();
        args_s = nodem_args.to_byte();
    }

    if (nodem_state->debug > LOW) {
        debug_log(">>   procedure: ", proc_s);
        debug_log(">>   arguments: ", args_s);
    }

    NodemBaton* nodem_baton;
    NodemBaton new_baton;

    if (async) {
        nodem_baton = new NodemBaton();

        nodem_baton->callback_p.Reset(isolate, Local<Function>::Cast(info[args_cnt]));

        nodem_baton->error = new gtm_char_t[ERR_LEN];
        nodem_baton->result = new gtm_char_t[RES_LEN];
    } else {
        nodem_baton = &new_baton;

        nodem_baton->callback_p.Reset();

        nodem_baton->error = nodem_state->error;
        nodem_baton->result = nodem_state->result;
    }

    nodem_baton->request.data = nodem_baton;
    nodem_baton->arguments_p.Reset(isolate, arguments);
    nodem_baton->data_p.Reset(isolate, Undefined(isolate));
    nodem_baton->name = proc_s;
    nodem_baton->args = args_s;
    nodem_baton->relink = relink;
    nodem_baton->mode = nodem_state->mode;
    nodem_baton->async = async;
    nodem_baton->local = local;
    nodem_baton->position = position;
    nodem_baton->routine = routine;
    nodem_baton->status = 0;
#if NODEM_YDB == 1 && YDB_RELEASE >= 124
    nodem_baton->info = 32766;
#else
    nodem_baton->info = 8192;
#endif
    nodem_baton->nodem_function = &gtm::procedure;
    nodem_baton->ret_function = &nodem::procedure;
    nodem_baton->nodem_state = nodem_state;

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    if (nodem_state->debug > LOW) {
        debug_log(">>   relink: ", relink);
        debug_log(">>   mode: ", nodem_state->mode);
        debug_log(">>   info: ", nodem_baton->info);
    }

    if (async) {
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
        uv_queue_work(GetCurrentEventLoop(isolate), &nodem_baton->request, async_work, async_after);
#else
        uv_queue_work(uv_default_loop(), &nodem_baton->request, async_work, async_after);
#endif

        if (nodem_state->debug > OFF) debug_log(">  Nodem::procedure exit\n");

        info.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    nodem_baton->status = nodem_baton->nodem_function(nodem_baton);

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

    if (nodem_baton->status != EXIT_SUCCESS) {
        if (position) {
            isolate->ThrowException(Exception::Error(
              to_string_n(isolate, error_status(nodem_baton->error, position, async, nodem_state))));

            info.GetReturnValue().Set(Undefined(isolate));
        } else {
            info.GetReturnValue().Set(error_status(nodem_baton->error, position, async, nodem_state));
        }

        nodem_baton->arguments_p.Reset();
        nodem_baton->data_p.Reset();

        return;
    }

    if (nodem_state->debug > LOW) debug_log(">>   call into procedure");

    Local<Value> return_object = nodem_baton->ret_function(nodem_baton);

    nodem_baton->arguments_p.Reset();
    nodem_baton->data_p.Reset();

    info.GetReturnValue().Set(return_object);

    if (nodem_state->debug > OFF) debug_log(">  Nodem::procedure exit\n");

    return;
} // @end nodem::Nodem::procedure method

/*
 * @method nodem::Nodem::global_directory_deprecated
 * @summary Calls nodem::global_directory after logging that this method is deprecated
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::global_directory_deprecated(const FunctionCallbackInfo<Value>& info)
{
    if (reinterpret_cast<NodemState*>(info.Data().As<External>()->Value())->debug > OFF || !(deprecated_g & GLOBAL)) {
        deprecated_g |= GLOBAL;
        debug_log(">  global_directory [DEPRECATED - Use globalDirectory instead]");
    }

    return Nodem::global_directory(info);
}

/*
 * @method nodem::Nodem::global_directory
 * @summary List the globals in a database, with optional filters
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::global_directory(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::global_directory enter");

    if (nodem_state_g < OPEN) {
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

        if (number_value_n(isolate, max) < 0) max = Number::New(isolate, 0);

        lo = get_n(isolate, arg_object, new_string_n(isolate, "lo"));

        if (lo->IsUndefined() || !lo->IsString()) lo = String::Empty(isolate);

        hi = get_n(isolate, arg_object, new_string_n(isolate, "hi"));

        if (hi->IsUndefined() || !hi->IsString()) hi = String::Empty(isolate);
    } else {
        max = Number::New(isolate, 0);
        lo = String::Empty(isolate);
        hi = String::Empty(isolate);
    }

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    if (nodem_state->debug > LOW) {
        debug_log(">>   mode: ", nodem_state->mode);
        debug_log(">>   max: ", uint32_value_n(isolate, max));
    }

    gtm_status_t status;
    gtm_char_t global_directory[] = "global_directory";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = global_directory;
    access.rtn_name.length = strlen(global_directory);
    access.handle = NULL;

    if (nodem_state->utf8 == true) {
        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolate, lo)),
                 *(UTF8_VALUE_TEMP_N(isolate, hi)), nodem_state->mode);
    } else {
        NodemValue nodem_lo {lo};
        NodemValue nodem_hi {hi};

        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", nodem_lo.to_byte());
            debug_log(">>   hi: ", nodem_hi.to_byte());
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), nodem_lo.to_byte(), nodem_hi.to_byte(), nodem_state->mode);
    }
#else
    if (nodem_state->utf8 == true) {
        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_ci(global_directory, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolae, lo)),
                 *(UTF8_VALUE_TEMP_N(isolate, hi)), nodem_state->mode);
    } else {
        NodemValue nodem_lo {lo};
        NodemValue nodem_hi {hi};

        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", nodem_lo.to_byte());
            debug_log(">>   hi: ", nodem_hi.to_byte());
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_ci(global_directory, ret_buf, uint32_value_n(isolate, max),
                 nodem_lo.to_byte(), nodem_hi.to_byte(), nodem_state->mode);
    }
#endif

    if (nodem_state->debug > LOW) {
        funlockfile(stderr);

        if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        debug_log(">>   status: ", status);
    }

    if (status != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
        return;
    }

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = NodemValue::from_byte(ret_buf);
    }

    if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);
    if (nodem_state->debug > OFF) debug_log(">  Nodem::global_directory JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Value> json = json_method(json_string, "parse", nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
    } else {
        info.GetReturnValue().Set(Local<Array>::Cast(json));
    }

    if (nodem_state->debug > OFF) debug_log(">  Nodem::global_directory exit\n");

    return;
} // @end nodem::Nodem::global_directory method

/*
 * @method nodem::Nodem::local_directory_deprecated
 * @summary Calls nodem::local_directory after logging that this method is deprecated
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::local_directory_deprecated(const FunctionCallbackInfo<Value>& info)
{
    if (reinterpret_cast<NodemState*>(info.Data().As<External>()->Value())->debug > OFF || !(deprecated_g & LOCAL)) {
        deprecated_g |= LOCAL;
        debug_log(">  local_directory [DEPRECATED - Use localDirectory instead]");
    }

    return Nodem::local_directory(info);
}

/*
 * @method nodem::Nodem::local_directory
 * @summary List the local variables in the symbol table, with optional filters
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::local_directory(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::local_directory enter");

    if (nodem_state_g < OPEN) {
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

        if (number_value_n(isolate, max) < 0) max = Number::New(isolate, 0);

        lo = get_n(isolate, arg_object, new_string_n(isolate, "lo"));

        if (lo->IsUndefined() || !lo->IsString()) lo = String::Empty(isolate);

        hi = get_n(isolate, arg_object, new_string_n(isolate, "hi"));

        if (hi->IsUndefined() || !hi->IsString()) hi = String::Empty(isolate);
    } else {
        max = Number::New(isolate, 0);
        lo = String::Empty(isolate);
        hi = String::Empty(isolate);
    }

    if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, lo)))) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'lo' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, lo)))) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'lo' is an invalid name")));
        return;
    }

    if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, hi)))) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'hi' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, hi)))) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Property 'hi' is an invalid name")));
        return;
    }

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    if (nodem_state->debug > LOW) {
        debug_log(">>   mode: ", nodem_state->mode);
        debug_log(">>   max: ", uint32_value_n(isolate, max));
    }

    gtm_status_t status;
    gtm_char_t local_directory[] = "local_directory";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = local_directory;
    access.rtn_name.length = strlen(local_directory);
    access.handle = NULL;

    if (nodem_state->utf8 == true) {
        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolate, lo)),
                 *(UTF8_VALUE_TEMP_N(isolate, hi)), nodem_state->mode);
    } else {
        NodemValue nodem_lo {lo};
        NodemValue nodem_hi {hi};

        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", nodem_lo.to_byte());
            debug_log(">>   hi: ", nodem_hi.to_byte());
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), nodem_lo.to_byte(), nodem_hi.to_byte(), nodem_state->mode);
    }
#else
    if (nodem_state->utf8 == true) {
        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", *(UTF8_VALUE_TEMP_N(isolate, lo)));
            debug_log(">>   hi: ", *(UTF8_VALUE_TEMP_N(isolate, hi)));
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_ci(local_directory, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolate, lo)),
                 *(UTF8_VALUE_TEMP_N(isolate, hi)), nodem_state->mode);
    } else {
        NodemValue nodem_lo {lo};
        NodemValue nodem_hi {hi};

        if (nodem_state->debug > LOW) {
            debug_log(">>   lo: ", nodem_lo.to_byte());
            debug_log(">>   hi: ", nodem_hi.to_byte());
        }

        if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

        if (nodem_state->debug > LOW) {
            if (dup2(STDERR_FILENO, STDOUT_FILENO) == -1) {
                char error[BUFSIZ];

                cerr << strerror_r(errno, error, BUFSIZ);
            }

            flockfile(stderr);
        }

        status = gtm_ci(local_directory, ret_buf, uint32_value_n(isolate, max),
                 nodem_lo.to_byte(), nodem_hi.to_byte(), nodem_state->mode);
    }
#endif

    if (nodem_state->debug > LOW) {
        funlockfile(stderr);

        if (dup2(save_stdout_g, STDOUT_FILENO) == -1) {
            char error[BUFSIZ];

            cerr << strerror_r(errno, error, BUFSIZ);
        }

        debug_log(">>   status: ", status);
    }

    if (status != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
        return;
    }

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = NodemValue::from_byte(ret_buf);
    }

    if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);
    if (nodem_state->debug > OFF) debug_log(">  Nodem::local_directory JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Value> json = json_method(json_string, "parse", nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
    } else {
        info.GetReturnValue().Set(Local<Array>::Cast(json));
    }

    if (nodem_state->debug > OFF) debug_log(">  Nodem::local_directory exit\n");

    return;
} // @end nodem::Nodem::local_directory method

/*
 * @method nodem::Nodem::retrieve
 * @summary Not yet implemented
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::retrieve(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    const NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::retrieve enter");

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    gtm_status_t status;
    gtm_char_t retrieve[] = "retrieve";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = retrieve;
    access.rtn_name.length = strlen(retrieve);
    access.handle = NULL;

    if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

    status = gtm_cip(&access, ret_buf);
#else
    if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

    status = gtm_ci(retrieve, ret_buf);
#endif

    if (nodem_state->debug > LOW) debug_log(">>   status: ", status);

    if (status != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
        return;
    }

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = NodemValue::from_byte(ret_buf);
    }

    if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);
    if (nodem_state->debug > OFF) debug_log(">  Nodem::retrieve JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
    } else {
        info.GetReturnValue().Set(to_object_n(isolate, json));
    }

    if (nodem_state->debug > OFF) debug_log(">  Nodem::retrieve exit\n");

    return;
} // @end nodem::Nodem::retrieve method

/*
 * @method nodem::Nodem::update
 * @summary Not yet implemented
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::update(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    const NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());

    if (nodem_state->debug > OFF) debug_log(">  Nodem::update enter");

    if (nodem_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, NODEM_DB " connection is not open")));
        return;
    }

    if (nodem_state->debug > OFF) debug_log(">  call into " NODEM_DB);

    gtm_status_t status;
    gtm_char_t update[] = "update";

    static gtm_char_t ret_buf[RES_LEN];

#if NODEM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = update;
    access.rtn_name.length = strlen(update);
    access.handle = NULL;

    if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

    status = gtm_cip(&access, ret_buf);
#else
    if (nodem_state->tp_level == 0) uv_mutex_lock(&mutex_g);

    status = gtm_ci(update, ret_buf);
#endif

    if (nodem_state->debug > LOW) debug_log(">>   status: ", status);

    if (status != EXIT_SUCCESS) {
        gtm_char_t msg_buf[ERR_LEN];
        gtm_zstatus(msg_buf, ERR_LEN);

        if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);

        info.GetReturnValue().Set(error_status(msg_buf, false, false, nodem_state));
        return;
    }

    if (nodem_state->debug > OFF) debug_log(">  return from " NODEM_DB);

    Local<String> json_string;

    if (nodem_state->utf8 == true) {
        json_string = new_string_n(isolate, ret_buf);
    } else {
        json_string = NodemValue::from_byte(ret_buf);
    }

    if (nodem_state->tp_level == 0) uv_mutex_unlock(&mutex_g);
    if (nodem_state->debug > OFF) debug_log(">  Nodem::update JSON string: ", *(UTF8_VALUE_TEMP_N(isolate, json_string)));

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse", nodem_state);

    if (try_catch.HasCaught()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Function has missing or invalid JSON data")));
        info.GetReturnValue().Set(try_catch.Exception());
    } else {
        info.GetReturnValue().Set(to_object_n(isolate, json));
    }

    if (nodem_state->debug > OFF) debug_log(">  Nodem::update exit\n");

    return;
} // @end nodem::Nodem::update method

// ***End Public APIs***

#if NODEM_SIMPLE_API == 1
/*
 * @method nodem::Nodem::restart
 * @summary The Nodem class getter for tpRestart
 * @param {Local<String>} property - The class property to access with the getter
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
#   if NODE_MAJOR_VERSION >= 23
void Nodem::restart(const FunctionCallbackInfo<Value>& info)
#   else
void Nodem::restart(Local<String> property, const PropertyCallbackInfo<Value>& info)
#   endif
{
    Isolate* isolate = Isolate::GetCurrent();

    Nodem* nodem = ObjectWrap::Unwrap<Nodem>(info.Holder());

    info.GetReturnValue().Set(Number::New(isolate, nodem->tp_restart));
}

/*
 * @method nodem::Nodem::rollback
 * @summary The Nodem class getter for tpRollback
 * @param {Local<String>} property - The class property to access with the getter
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
#   if NODE_MAJOR_VERSION >= 23
void Nodem::rollback(const FunctionCallbackInfo<Value>& info)
#   else
void Nodem::rollback(Local<String> property, const PropertyCallbackInfo<Value>& info)
#   endif
{
    Isolate* isolate = Isolate::GetCurrent();

    Nodem* nodem = ObjectWrap::Unwrap<Nodem>(info.Holder());

    info.GetReturnValue().Set(Number::New(isolate, nodem->tp_rollback));
}
#endif

/*
 * @method nodem::Nodem::New
 * @summary The Nodem class constructor
 * @param {FunctionCallbackInfo<Value>&} info - A special object passed by the Node.js runtime, including passed arguments
 * @returns {void}
 */
void Nodem::New(const FunctionCallbackInfo<Value>& info)
{
    Isolate* isolate = Isolate::GetCurrent();

    if (info.IsConstructCall()) {
        Nodem* nodem = new Nodem();
        nodem->Wrap(info.This());

        info.GetReturnValue().Set(info.This());
    } else {
        NodemState* nodem_state = reinterpret_cast<NodemState*>(info.Data().As<External>()->Value());
        Local<Function> constructor = Local<Function>::New(isolate, nodem_state->constructor_p);

#if NODE_MAJOR_VERSION >= 6
        MaybeLocal<Object> instance = constructor->NewInstance(isolate->GetCurrentContext());

        if (instance.IsEmpty()) {
            isolate->ThrowException(Exception::Error(new_string_n(isolate, "Unable to instantiate the Nodem class")));
        } else {
            info.GetReturnValue().Set(instance.ToLocalChecked());
        }
#else
        info.GetReturnValue().Set(constructor->NewInstance());
#endif
    }

    return;
} // @end nodem::Nodem::New method

/*
 * @method nodem::Nodem::Init
 * @summary Set the exports property when nodem.node is required
 * @param {Local<Object>} exports - A special object passed by the Node.js runtime
 * @returns {void}
 */
void Nodem::Init(Local<Object> exports)
{
    Isolate* isolate = Isolate::GetCurrent();

    NodemState* nodem_state = new NodemState(isolate, exports);

#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
    AddEnvironmentCleanupHook(isolate, cleanup_nodem_state, static_cast<void*>(nodem_state));
#endif

    Local<External> external_data = External::New(isolate, nodem_state);
    Local<FunctionTemplate> fn_template = FunctionTemplate::New(isolate, New, external_data);

    fn_template->SetClassName(new_string_n(isolate, "Nodem"));
#if NODEM_SIMPLE_API == 1
#   if NODE_MAJOR_VERSION >= 23
    Local<Name> tpRestart = new_string_n(isolate, "tpRestart");
    Local<FunctionTemplate> restart_tpl = FunctionTemplate::New(isolate, restart);
    fn_template->InstanceTemplate()->SetAccessorProperty(tpRestart, restart_tpl);

    Local<Name> tpRollback = new_string_n(isolate, "tpRollback");
    Local<FunctionTemplate> rollback_tpl = FunctionTemplate::New(isolate, rollback);
    fn_template->InstanceTemplate()->SetAccessorProperty(tpRollback, rollback_tpl);
#   elif NODE_MAJOR_VERSION >= 22
    fn_template->InstanceTemplate()->SetAccessor(new_string_n(isolate, "tpRestart"),
                 restart, nullptr, Local<Value>(), ReadOnly, SideEffectType::kHasNoSideEffect);

    fn_template->InstanceTemplate()->SetAccessor(new_string_n(isolate, "tpRollback"),
                 rollback, nullptr, Local<Value>(), ReadOnly, SideEffectType::kHasNoSideEffect);
#   else
    fn_template->InstanceTemplate()->SetAccessor(new_string_n(isolate, "tpRestart"),
                 restart, nullptr, Local<Value>(), DEFAULT, DontDelete);

    fn_template->InstanceTemplate()->SetAccessor(new_string_n(isolate, "tpRollback"),
                 rollback, nullptr, Local<Value>(), DEFAULT, DontDelete);
#   endif
#endif
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
    set_prototype_method_n(isolate, fn_template, "next_node", next_node_deprecated, external_data);
    set_prototype_method_n(isolate, fn_template, "previousNode", previous_node, external_data);
    set_prototype_method_n(isolate, fn_template, "previous_node", previous_node_deprecated, external_data);
    set_prototype_method_n(isolate, fn_template, "increment", increment, external_data);
    set_prototype_method_n(isolate, fn_template, "lock", lock, external_data);
    set_prototype_method_n(isolate, fn_template, "unlock", unlock, external_data);
#if NODEM_SIMPLE_API == 1
    set_prototype_method_n(isolate, fn_template, "transaction", transaction, external_data);
#endif
    set_prototype_method_n(isolate, fn_template, "function", function, external_data);
    set_prototype_method_n(isolate, fn_template, "procedure", procedure, external_data);
    set_prototype_method_n(isolate, fn_template, "routine", procedure, external_data);
    set_prototype_method_n(isolate, fn_template, "globalDirectory", global_directory, external_data);
    set_prototype_method_n(isolate, fn_template, "global_directory", global_directory_deprecated, external_data);
    set_prototype_method_n(isolate, fn_template, "localDirectory", local_directory, external_data);
    set_prototype_method_n(isolate, fn_template, "local_directory", local_directory_deprecated, external_data);
    set_prototype_method_n(isolate, fn_template, "retrieve", retrieve, external_data);
    set_prototype_method_n(isolate, fn_template, "update", update, external_data);

#if NODE_MAJOR_VERSION >= 3
    MaybeLocal<Function> maybe_function = fn_template->GetFunction(isolate->GetCurrentContext());
    Local<Function> local_function;

    if (maybe_function.IsEmpty()) {
        isolate->ThrowException(Exception::Error(new_string_n(isolate, "Unable to construct the Nodem class")));
    } else {
        local_function = maybe_function.ToLocalChecked();
    }
#else
    Local<Function> local_function = fn_template->GetFunction();
#endif

    nodem_state->constructor_p.Reset(isolate, local_function);
    Local<Function> constructor = Local<Function>::New(isolate, nodem_state->constructor_p);

    set_n(isolate, exports, new_string_n(isolate, "Gtm"), constructor);
#if NODEM_YDB == 1
    set_n(isolate, exports, new_string_n(isolate, "Ydb"), constructor);
#endif

    return;
} // @end nodem::Nodem::Init method

#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 7)
/*
 * @macro function NODE_MODULE_INIT
 * @summary Register the nodem.node module with Node.js in a context-aware way
 */
NODE_MODULE_INIT() {
    Nodem::Init(exports);
    return;
}
#else
/*
 * @macro NODE_MODULE
 * @summary Register the nodem.node module with Node.js
 */
NODE_MODULE(nodem, Nodem::Init)
#endif

} // @end nodem namespace
