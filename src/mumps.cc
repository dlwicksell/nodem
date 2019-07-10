/*
 * Package:    NodeM
 * File:       mumps.cc
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2019 Fourth Watch Software LC
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

#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>
#include "utility.h"
#include "mumps.h"
#include "gtm.h"
#include "ydb.h"

namespace nodem {

static bool auto_relink_g = false;
static bool reset_term_g = false;
static bool signal_sigint_g = true;
static bool signal_sigquit_g = true;
static bool signal_sigterm_g = true;
static bool utf8_g = true;

static char* nocenable_g = NULL;

static struct sigaction signal_attr_g;
static struct termios term_attr_g;

static enum {CLOSED, NOT_OPEN, OPEN} gtm_state_g = NOT_OPEN;
static enum mode_t {STRICT, CANONICAL} mode_g = CANONICAL;

debug_t debug_g = OFF;
uv_mutex_t mutex_g;

using namespace v8;
using std::cout;
using std::endl;
using std::string;
using std::vector;

Persistent<Function> Gtm::constructor_p;

/*
 * @function {public} clean_shutdown
 * @summary Handle a SIGINT/SIGQUIT/SIGTERM signal, by cleaning up everything, and exiting Node.js
 * @param {int} signal_num - The number of the caught signal
 * @returns void
 */
void clean_shutdown(const int signal_num)
{
    if (gtm_state_g == OPEN) {
        if (uv_mutex_trylock(&mutex_g) == 0) {
#if YDB_SIMPLE_API == 1
            if (ydb_exit() != YDB_OK) reset_term_g = true;
#else
            if (gtm_exit() != EXIT_SUCCESS) reset_term_g = true;
#endif
            uv_mutex_unlock(&mutex_g);
        } else {
            reset_term_g = true;
        }

        if (reset_term_g == true) {
            term_attr_g.c_iflag |= ICRNL;
            term_attr_g.c_lflag |= (ICANON | ECHO);
        }

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
} // @end clean_shutdown function

/*
 * @function {private} json_method
 * @summary Call a method on the built-in Node.js JSON object
 * @param {Local<Value>} data - A JSON string containing the data to parse or a JavaScript object to stringify
 * @param {string} type - The name of the method to call on JSON
 * @returns {Local<Value>} - An object containing the output data
 */
inline static Local<Value> json_method(Local<Value> data, const string type)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> json_method enter" << "\n";
        if (!data->IsObject()) cout << "DEBUG>>> data: " << *(UTF8_VALUE_TEMP_N(isolate, data)) << "\n";
        cout << "DEBUG>>> type: " << type << "\n";
    }

    Local<Object> global = isolate->GetCurrentContext()->Global();
    Local<Object> json = to_object_n(isolate, global->Get(String::NewFromUtf8(isolate, "JSON")));
    Local<Function> method = Local<Function>::Cast(json->Get(String::NewFromUtf8(isolate, type.c_str())));

    if (debug_g > MEDIUM) cout << "DEBUG>>> json_method exit" << "\n";

    return scope.Escape(call_n(isolate, method, json, 1, &data));
} // @end json_method function

/*
 * @function {private} error_status
 * @summary Handle an error from the YottaDB/GT.M runtime
 * @param {gtm_char_t*} msg_buf - A character string representing the YottaDB/GT.M runtime error
 * @param {bool} position - Whether the API was called by positional arguments or not
 * @param {bool} async - Whether the API was called asynchronously or not
 * @returns {Local<Value>} result - An object containing the formatted error content
 */
inline static Local<Value> error_status(gtm_char_t* msg_buf, const bool position, const bool async)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> error_status enter" << "\n";
        cout << "DEBUG>>> msg_buf: " << msg_buf << "\n";
        cout << "DEBUG>>> position: " << position << "\n";
        cout << "DEBUG>>> async: " << async << "\n";
    }

    char* error_msg;
    char* code = strtok_r(msg_buf, ",", &error_msg);

    unsigned int error_code = atoi(code);

    if (strstr(error_msg, "%YDB-E-CTRAP") != NULL || strstr(error_msg, "%GTM-E-CTRAP") != NULL) {
        clean_shutdown(SIGINT); // Handle SIGINT caught by YottaDB or GT.M
    }

    Local<Object> result = Object::New(isolate);

    if (position && !async) {
        if (debug_g > MEDIUM) {
            cout << "DEBUG>>> error_status exit" << "\n";
            cout << "DEBUG>>> error_msg: " << error_msg << "\n";
        }

        return scope.Escape(String::NewFromUtf8(isolate, error_msg));
    } else if (mode_g == STRICT) {
        result->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 0));
        result->Set(String::NewFromUtf8(isolate, "ErrorCode"), Number::New(isolate, error_code));
        result->Set(String::NewFromUtf8(isolate, "ErrorMessage"), String::NewFromUtf8(isolate, error_msg));
    } else {
        result->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, false));
        result->Set(String::NewFromUtf8(isolate, "errorCode"), Number::New(isolate, error_code));
        result->Set(String::NewFromUtf8(isolate, "errorMessage"), String::NewFromUtf8(isolate, error_msg));
    }

    if (debug_g > MEDIUM) {
        cout << "DEBUG>>> error_status exit" << "\n";

        Local<Value> result_string = json_method(result, "stringify");
        cout << "DEBUG>>> result: " << *(UTF8_VALUE_TEMP_N(isolate, result_string)) << "\n";
    }

    return scope.Escape(result);
} // @end error_status function

/*
 * @function {private} invalid_local
 * @summary If a local variable name starts with v4w, it is not valid, and cannot be manipulated
 * @param {char*} name - The name to test against
 * @returns {bool} - Whether the local name is invalid
 */
inline static bool invalid_local(const char* name)
{
    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> invalid_local enter" << "\n";
        cout << "DEBUG>>> name: " << name << "\n";
    }

    if (strncmp(name, "v4w", 3) == 0) {
        if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_local exit: " << true << "\n";

        return true;
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_local exit: " << false << "\n";

    return false;
} // @end invalid_local function

/*
 * @function {private} invalid_name
 * @summary If a variable name contains subscripts, it is not valid, and cannot be used
 * @param {char*} name - The name to test against
 * @returns {bool} - Whether the name is invalid
 */
inline static bool invalid_name(const char* name)
{
    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> invalid_name enter" << "\n";
        cout << "DEBUG>>> name: " << name << "\n";
    }

    if (strchr(name, '(') != NULL || strchr(name, ')') != NULL) {
        if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_name exit: " << true << "\n";

        return true;
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_name exit: " << false << "\n";

    return false;
} // @end invalid_name function

/*
 * @function {private} globalize_name
 * @summary If a variable name (or function/procedure) doesn't start with (or contain) the optional '^' character, add it for output
 * @param {Local<Value>} name - The name to be normalized for output
 * @returns {Local<Value>} [new_name|name] - A string containing the normalized name
 */
inline static Local<Value> globalize_name(const Local<Value> name)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> globalize_name enter" << "\n";
        cout << "DEBUG>>> name: " << *(UTF8_VALUE_TEMP_N(isolate, name)) << "\n";
    }

    UTF8_VALUE_N(isolate, data_string, name);

    const gtm_char_t* data_name = *data_string;
    const gtm_char_t* char_ptr = strchr(data_name, '^');

    if (char_ptr == NULL) {
        Local<Value> new_name = concat_n(isolate, String::NewFromUtf8(isolate, "^"), to_string_n(isolate, name));

        if (debug_g > MEDIUM) cout << "DEBUG>>> globalize_name exit: " << *(UTF8_VALUE_TEMP_N(isolate, new_name)) << "\n";

        return scope.Escape(new_name);
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> globalize_name exit: " << *(UTF8_VALUE_TEMP_N(isolate, name)) << "\n";

    return scope.Escape(name);
} // @end globalize_name function

/*
 * @function {private} localize_name
 * @summary If a variable name starts with the optional '^' character, strip it off for output
 * @param {Local<Value>} name - The name to be normalized for output
 * @returns {Local<Value>} [data_name|name] - A string containing the normalized name
 */
inline static Local<Value> localize_name(const Local<Value> name)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> localize_name enter" << "\n";
        cout << "DEBUG>>> name: " << *(UTF8_VALUE_TEMP_N(isolate, name)) << "\n";
    }

    UTF8_VALUE_N(isolate, data_string, name);

    const gtm_char_t* data_name = *data_string;
    const gtm_char_t* char_ptr = strchr(data_name, '^');

    if (char_ptr != NULL && char_ptr - data_name == 0) {
        if (debug_g > MEDIUM) cout << "DEBUG>>> localize_name exit: " << &data_name[1] << "\n";

        return scope.Escape(String::NewFromUtf8(isolate, &data_name[1]));
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> localize_name exit: " << *(UTF8_VALUE_TEMP_N(isolate, name)) << "\n";

    return scope.Escape(name);
} // @end localize_name function

/*
 * @function {private} is_number
 * @summary Check if a value returned from YottaDB's SimpleAPI is a canonical number
 * @param {string} data - The data value to be tested
 * @returns {boolean} - Whether the data value is a canonical number or not
 */
inline static bool is_number(const string data)
{
    /*
     * YottaDB approximate (using number of digits, rather than number value) number limits:
     *   - 47 digits before overflow (resulting in an overflow error)
     *   - 18 digits of precision
     * Node.js/JavaScript approximate (using number of digits, rather than number value) number limits:
     *   - 309 digits before overflow (represented as the Infinity primitive)
     *   - 21 digits before conversion to exponent notation
     *   - 16 digits of precision
     * This is why anything over 16 characters needs to be treated as a string
     */

    if (mode_g == STRICT) return false; // In strict mode, all data is treated as a string

    bool flag = false;
    size_t neg_cnt = count(data.begin(), data.end(), '-');
    size_t decp_cnt = count(data.begin(), data.end(), '.');

    if ((decp_cnt == 0 || decp_cnt == 1) && (neg_cnt == 0 || (neg_cnt == 1 && data[0] == '-'))) flag = true;
    if ((decp_cnt == 1 || neg_cnt == 1) && data.length() <= 1) flag = false;
    if (data.length() > 16 || data[data.length() - 1] == '.') flag = false;

    if (mode_g && flag && !data.empty() &&
            all_of(data.begin(), data.end(), [](char c) {return (std::isdigit(c) || c == '-' || c == '.');})) {
        if ((data[0] == '0' && data.length() > 1) || (decp_cnt == 1 && data[data.length() - 1] == '0')) {
            return false;
        } else {
            return true;
        }
    } else {
        return false;
    }
} // @end is_number function

/*
 * @function {private} encode_arguments
 * @summary Encode an array of arguments for parsing in v4wNode.m
 * @param {Local<Value>} arguments - The array of subscripts or arguments to be encoded
 * @param {boolean} function <false> - Whether the arguments to encode are from the function or procedure call or not
 * @returns {Local<Value>} [Undefined|encoded_array] - The encoded array of subscripts or arguments, or Undefined if it has bad data
 */
static Local<Value> encode_arguments(const Local<Value> arguments, const bool function = false)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> encode_arguments enter" << "\n";

        Local<Value> argument_string = json_method(arguments, "stringify");
        cout << "DEBUG>>> arguments: " << *(UTF8_VALUE_TEMP_N(isolate, argument_string)) << "\n";
    }

    Local<Array> argument_array = Local<Array>::Cast(arguments);
    Local<Array> encoded_array = Array::New(isolate);

    for (unsigned int i = 0; i < argument_array->Length(); i++) {
        Local<Value> data_test = argument_array->Get(i);
        Local<String> data = to_string_n(isolate, data_test);
        Local<String> colon = String::NewFromUtf8(isolate, ":");
        Local<String> length;
        Local<Value> new_data = Undefined(isolate);

        if (data_test->IsUndefined()) {
            new_data = String::NewFromUtf8(isolate, "0:");
        } else if (data_test->IsSymbol() || data_test->IsSymbolObject()) {
            return Undefined(isolate);
        } else if (data_test->IsNumber()) {
            length = to_string_n(isolate, Number::New(isolate, data->Length()));
            new_data = concat_n(isolate, length, concat_n(isolate, colon, data));
        } else if (data_test->IsObject()) {
            if (!function) return Undefined(isolate);

            Local<Object> object = to_object_n(isolate, data_test);
            Local<Value> type = object->Get(String::NewFromUtf8(isolate, "type"));
            Local<Value> value_test = object->Get(String::NewFromUtf8(isolate, "value"));
            Local<String> value = to_string_n(isolate, value_test);

            if (value_test->IsSymbol() || value_test->IsSymbolObject()) {
                return Undefined(isolate);
            } else if (type->StrictEquals(String::NewFromUtf8(isolate, "reference"))) {
                if (!value_test->IsString()) return Undefined(isolate);
                if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);
                if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);

                Local<String> new_value = to_string_n(isolate, localize_name(value));
                Local<String> dot = String::NewFromUtf8(isolate, ".");

                if (utf8_g == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, new_value) + 1));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, new_value->Length() + 1));
                }

                new_data = concat_n(isolate, length, concat_n(isolate, colon, concat_n(isolate, dot, new_value)));
            } else if (type->StrictEquals(String::NewFromUtf8(isolate, "variable"))) {
                if (!value_test->IsString()) return Undefined(isolate);
                if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);
                if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, value)))) return Undefined(isolate);

                Local<String> new_value = to_string_n(isolate, localize_name(value));

                if (utf8_g == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, new_value)));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, new_value->Length()));
                }

                new_data = concat_n(isolate, length, concat_n(isolate, colon, new_value));
            } else if (type->StrictEquals(String::NewFromUtf8(isolate, "value"))) {
                if (value_test->IsUndefined()) {
                    new_data = String::NewFromUtf8(isolate, "0:");
                } else if (value_test->IsSymbol() || value_test->IsSymbolObject()) {
                    return Undefined(isolate);
                } else if (value_test->IsNumber()) {
                    length = to_string_n(isolate, Number::New(isolate, value->Length()));
                    new_data = concat_n(isolate, length, concat_n(isolate, colon, value));
                } else {
                    if (utf8_g == true) {
                        length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, value) + 2));
                    } else {
                        length = to_string_n(isolate, Number::New(isolate, value->Length() + 2));
                    }

                    Local<String> quote = String::NewFromUtf8(isolate, "\"");
                    new_data = concat_n(isolate, concat_n(isolate, length,
                      concat_n(isolate, colon, quote)), concat_n(isolate, value, quote));
                }
            } else {
                if (utf8_g == true) {
                    length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, data) + 2));
                } else {
                    length = to_string_n(isolate, Number::New(isolate, data->Length() + 2));
                }

                Local<String> quote = String::NewFromUtf8(isolate, "\"");
                new_data = concat_n(isolate, concat_n(isolate, length,
                  concat_n(isolate, colon, quote)), concat_n(isolate, data, quote));
            }
        } else {
            if (utf8_g == true) {
                length = to_string_n(isolate, Number::New(isolate, utf8_length_n(isolate, data) + 2));
            } else {
                length = to_string_n(isolate, Number::New(isolate, data->Length() + 2));
            }

            Local<String> quote = String::NewFromUtf8(isolate, "\"");
            new_data = concat_n(isolate, concat_n(isolate, length,
              concat_n(isolate, colon, quote)), concat_n(isolate, data, quote));
        }

        encoded_array->Set(i, new_data);
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> encode_arguments exit: " << *(UTF8_VALUE_TEMP_N(isolate, encoded_array)) << "\n";

    return scope.Escape(encoded_array);
} // @end encode_arguments function

#if YDB_SIMPLE_API == 1
/*
 * @function {private} build_subscripts
 * @summary Build an array of subscritps for passing to the SimpleAPI
 * @param {Local<Value>} subscripts - The array of subscripts to be built
 * @param {bool&} error - If this is set to true, it signals an error with subscript data
 * @returns {vector<string>} [build_array] - The built array of subscripts
 */
static vector<string> build_subscripts(const Local<Value> subscripts, bool& error)
{
    if (debug_g > MEDIUM) {
        cout << "\nDEBUG>>> build_subscripts enter" << "\n";

        Local<Value> subscript_string = json_method(subscripts, "stringify");
        cout << "DEBUG>>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
    }

    Local<Array> subscripts_array = Local<Array>::Cast(subscripts);
    unsigned int length = subscripts_array->Length();

    string subs_data;
    vector<string> subs_array;

    for (unsigned int i = 0; i < length; i++) {
        Local<Value> data = subscripts_array->Get(i);

        if (data->IsSymbol() || data->IsSymbolObject() || data->IsObject() || data->IsArray()) {
            error = true;

            return subs_array;
        }

        if (utf8_g == true) {
            subs_data = *(UTF8_VALUE_TEMP_N(isolate, data));
        } else {
            GtmValue gtm_data {data};
            subs_data = gtm_data.to_byte();
        }

        if (mode_g == CANONICAL && data->IsNumber()) {
            if (subs_data.substr(0, 2) == "0.") subs_data = subs_data.substr(1, string::npos);
            if (subs_data.substr(0, 3) == "-0.") subs_data = "-" + subs_data.substr(2, string::npos);
        }

        if (debug_g > MEDIUM) cout << "DEBUG>>> subs_data[" << i << "]: " << subs_data << "\n";

        subs_array.push_back(subs_data);
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> build_subscripts exit" << "\n";

    return subs_array;
} // @end build_subscripts function
#endif

/*
 * @class GtmValue
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
}

/*
 * @class GtmValue
 * @method {class} from_byte
 * @summary Convert a byte encoded buffer to a UTF-8 encoded buffer
 * @param {gtm_char_t[]} buffer - A byte encoded buffer
 * @returns {Local<String>} A UTF-8 encoded buffer
 */
Local<String> GtmValue::from_byte(gtm_char_t buffer[])
{
    Isolate* isolate = Isolate::GetCurrent();

#if NODE_MAJOR_VERSION == 6 && NODE_MINOR_VERSION >= 8 || NODE_MAJOR_VERSION >= 7
    MaybeLocal<String> string = String::NewFromOneByte(isolate, reinterpret_cast<const uint8_t*>(buffer), NewStringType::kNormal);

    if (string.IsEmpty()) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Unable to convert from a byte buffer")));
        return String::Empty(isolate);
    } else {
        return string.ToLocalChecked();
    }
#else
    return String::NewFromOneByte(isolate, reinterpret_cast<const uint8_t*>(buffer));
#endif
}

/*
 * @function {private} data_return
 * @summary Check if global or local node has data and/or children or not
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from data call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> data_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> data_return enter" << "\n";

    Local<Value> subscripts = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify");
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
        }
    }

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    string data(baton->ret_buf);

    temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, atof(baton->ret_buf)));
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> data_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> data_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "defined")));
        }
    } else {
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    if (debug_g > OFF) cout << "\nDEBUG> data_return exit" << "\n";

    return scope.Escape(return_object);
} // @end data_return function

/*
 * @function {private} function_return
 * @summary Return value from an arbitrary extrinsic function
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from function call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {string} relink - Whether to relink the function before calling it
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> function_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> function_return enter" << "\n";

    Local<Value> arguments = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        Local<Value> argument_string = json_method(arguments, "stringify");
        cout << "DEBUG>> arguments: " << *(UTF8_VALUE_TEMP_N(isolate, argument_string)) << "\n";

        cout << "DEBUG>> relink: " << baton->relink << "\n";
    }

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> function_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> function = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> function_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "result")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "result")));
        }
    } else {
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        return_object->Set(String::NewFromUtf8(isolate, "function"), localize_name(function));

        if (!arguments->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "arguments"), arguments);

        return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "result")));
    }

    if (debug_g > OFF) cout << "\nDEBUG> function_return exit" << "\n";

    return scope.Escape(return_object);
} // @end function_return function

/*
 * @function {private} get_return
 * @summary Return data from a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @member {gtm_char_t} ret_buf - Data returned from get call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> get_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> get_return enter" << "\n";

    Local<Value> subscripts = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> status: " << baton->status << "\n";
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify");
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
        }
    }

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (baton->status == YDB_ERR_GVUNDEF || baton->status == YDB_ERR_LVUNDEF) {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 0));
    } else {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 1));
    }

    string data(baton->ret_buf);

    if (is_number(data)) {
        temp_object->Set(String::NewFromUtf8(isolate, "data"), Number::New(isolate, atof(baton->ret_buf)));
    } else {
        if (utf8_g == true) {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), String::NewFromUtf8(isolate, baton->ret_buf));
        } else {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), GtmValue::from_byte(baton->ret_buf));
        }
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> get_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> get_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "data")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "data")));
        }
    } else {
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "data"), temp_object->Get(String::NewFromUtf8(isolate, "data")));
        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    if (debug_g > OFF) cout << "\nDEBUG> get_return exit" << "\n";

    return scope.Escape(return_object);
} // @end get_return function

/*
 * @function {private} increment_return
 * @summary Increment or decrement the number in a global or local node
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @member {gtm_char_t} ret_buf - Data returned from increment call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {gtm_double_t} increment - Number to increment or decrement by
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> increment_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> increment_return enter" << "\n";

    Local<Value> subscripts = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> status: " << baton->status << "\n";
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify");
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
        }
    }

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    string data(baton->ret_buf);

    if (is_number(data)) {
        temp_object->Set(String::NewFromUtf8(isolate, "data"), Number::New(isolate, atof(baton->ret_buf)));
    } else {
        if (utf8_g == true) {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), String::NewFromUtf8(isolate, baton->ret_buf));
        } else {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), GtmValue::from_byte(baton->ret_buf));
        }
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> increment_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> increment_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "data")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "data")));
        }
    } else {
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "data"), temp_object->Get(String::NewFromUtf8(isolate, "data")));
    }

    if (debug_g > OFF) cout << "\nDEBUG> increment_return exit" << "\n";

    return scope.Escape(return_object);
} // @end increment_return function

/*
 * @function {private} kill_return
 * @summary Return data about removing a global or global node, or a local or local node, or the entire local symbol table
 * @param {Baton*} baton - struct containing the following members
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {int32_t} node_only - Whether the API was called on a single node only, or on a node and all its children
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> kill_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> kill_return enter" << "\n";

    Local<Value> subscripts = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify");
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
        }
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (name->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        Local<Value> ret_data = Undefined(isolate);

        if (mode_g == STRICT) ret_data = Number::New(isolate, 0);

        return scope.Escape(ret_data);
    } else if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> kill_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "0"));

            return scope.Escape(return_object);
        } else if (mode_g == STRICT) {
            return scope.Escape(Number::New(isolate, 0));
        } else {
            Local<Value> ret_data = Undefined(isolate);

            return scope.Escape(ret_data);
        }
    } else {
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "0"));
        } else if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
        }

        if (baton->node_only > -1) {
            return_object->Set(String::NewFromUtf8(isolate, "nodeOnly"), Boolean::New(isolate, baton->node_only));
        }
    }

    if (debug_g > OFF) cout << "\nDEBUG> kill_return exit" << "\n";

    return scope.Escape(return_object);
} // @end kill_return function

/*
 * @function {private} next_node_return
 * @summary Return the next global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @member {gtm_char_t} ret_buf - Data returned from next_node call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {vector<string>} {ydb only} subs_array - The subscripts of the next node
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> next_node_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> next_node_return enter" << "\n";

    if (debug_g > LOW) {
        cout << "DEBUG>> status: " << baton->status << "\n";
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";
    }

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (baton->status == YDB_NODE_END) {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 0));
    } else {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 1));
    }

    if (baton->status != YDB_NODE_END) {
        string data(baton->ret_buf);

        if (is_number(data)) {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), Number::New(isolate, atof(baton->ret_buf)));
        } else {
            if (utf8_g == true) {
                temp_object->Set(String::NewFromUtf8(isolate, "data"), String::NewFromUtf8(isolate, baton->ret_buf));
            } else {
                temp_object->Set(String::NewFromUtf8(isolate, "data"), GtmValue::from_byte(baton->ret_buf));
            }
        }
    }

    Local<Array> subs_array = Array::New(isolate);

    if (baton->subs_array.size()) {
        for (unsigned int i = 0; i < baton->subs_array.size(); i++) {
            if (debug_g > LOW) cout << "DEBUG>> subs_array[" << i << "]: " << baton->subs_array[i] << "\n";

            if (is_number(baton->subs_array[i])) {
                subs_array->Set(i, Number::New(isolate, atof(baton->subs_array[i].c_str())));
            } else {
                if (utf8_g == true) {
                    subs_array->Set(i, String::NewFromUtf8(isolate, baton->subs_array[i].c_str()));
                } else {
                    subs_array->Set(i, GtmValue::from_byte((gtm_char_t*) baton->subs_array[i].c_str()));
                }
            }
        }

        temp_object->Set(String::NewFromUtf8(isolate, "subscripts"), subs_array);
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> next_node_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> next_node_return exit" << "\n";

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));

        if (baton->async && mode_g == STRICT) {
            if (temp_subs->IsUndefined()) {
                return_object->Set(String::NewFromUtf8(isolate, "result"), Array::New(isolate));
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "result"), temp_subs);
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
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));
        if (!temp_subs->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = temp_object->Get(String::NewFromUtf8(isolate, "data"));
        if (!temp_data->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "data"), temp_data);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    if (debug_g > OFF) cout << "\nDEBUG> next_node_return exit" << "\n";

    return scope.Escape(return_object);
} // @end next_node_return function

/*
 * @function {private} order_return
 * @summary Return data about the next global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from order call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> order_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> order_return enter" << "\n";

    Local<Value> subscripts = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify");
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
        }
    }

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 1));

    string data(baton->ret_buf);

    if (is_number(data)) {
        temp_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, atof(baton->ret_buf)));
    } else {
        if (utf8_g == true) {
            temp_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, baton->ret_buf));
        } else {
            temp_object->Set(String::NewFromUtf8(isolate, "result"), GtmValue::from_byte(baton->ret_buf));
        }
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> order_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> order_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "result")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "result")));
        }
    } else {
        Local<Value> result = temp_object->Get(String::NewFromUtf8(isolate, "result"));

        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            new_subscripts->Set(Number::New(isolate, new_subscripts->Length() - 1), result);
            return_object->Set(String::NewFromUtf8(isolate, "subscripts"), new_subscripts);
        }

        return_object->Set(String::NewFromUtf8(isolate, "result"), localize_name(result));
    }

    if (debug_g > OFF) cout << "\nDEBUG> order_return exit" << "\n";

    return scope.Escape(return_object);
} // @end order_return function

/*
 * @function {private} previous_return
 * @summary Return data about the previous global or local node at the same level
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_char_t} ret_buf - Data returned from previous call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> previous_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> previous_return enter" << "\n";

    Local<Value> subscripts = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify");
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
        }
    }

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 1));

    string data(baton->ret_buf);

    if (is_number(data)) {
        temp_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, atof(baton->ret_buf)));
    } else {
        if (utf8_g == true) {
            temp_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, baton->ret_buf));
        } else {
            temp_object->Set(String::NewFromUtf8(isolate, "result"), GtmValue::from_byte(baton->ret_buf));
        }
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> previous_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> previous_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "result")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "result")));
        }
    } else {
        Local<Value> result = temp_object->Get(String::NewFromUtf8(isolate, "result"));

        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            new_subscripts->Set(Number::New(isolate, new_subscripts->Length() - 1), result);
            return_object->Set(String::NewFromUtf8(isolate, "subscripts"), new_subscripts);
        }

        return_object->Set(String::NewFromUtf8(isolate, "result"), localize_name(result));
    }

    if (debug_g > OFF) cout << "\nDEBUG> previous_return exit" << "\n";

    return scope.Escape(return_object);
} // @end previous_return function

/*
 * @function {private} previous_node_return
 * @summary Return the previous global or local node, depth first
 * @param {Baton*} baton - struct containing the following members
 * @member {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @member {gtm_char_t} ret_buf - Data returned from previous_node call
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {vector<string>} {ydb only} subs_array - The subscripts of the previous node
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> previous_node_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> previous_node_return enter" << "\n";

    if (debug_g > LOW) {
        cout << "DEBUG>> status: " << baton->status << "\n";
        cout << "DEBUG>> ret_buf: " << baton->ret_buf << "\n";
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";
    }

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (baton->status == YDB_NODE_END) {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 0));
    } else {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 1));
    }

    if (baton->status != YDB_NODE_END) {
        string data(baton->ret_buf);

        if (is_number(data)) {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), Number::New(isolate, atof(baton->ret_buf)));
        } else {
            if (utf8_g == true) {
                temp_object->Set(String::NewFromUtf8(isolate, "data"), String::NewFromUtf8(isolate, baton->ret_buf));
            } else {
                temp_object->Set(String::NewFromUtf8(isolate, "data"), GtmValue::from_byte(baton->ret_buf));
            }
        }
    }

    Local<Array> subs_array = Array::New(isolate);

    if (baton->subs_array.size()) {
        for (unsigned int i = 0; i < baton->subs_array.size(); i++) {
            if (debug_g > LOW) cout << "DEBUG>> subs_array[" << i << "]: " << baton->subs_array[i] << "\n";

            if (is_number(baton->subs_array[i])) {
                subs_array->Set(i, Number::New(isolate, atof(baton->subs_array[i].c_str())));
            } else {
                if (utf8_g == true) {
                    subs_array->Set(i, String::NewFromUtf8(isolate, baton->subs_array[i].c_str()));
                } else {
                    subs_array->Set(i, GtmValue::from_byte((gtm_char_t*) baton->subs_array[i].c_str()));
                }
            }
        }

        temp_object->Set(String::NewFromUtf8(isolate, "subscripts"), subs_array);
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, baton->ret_buf);
    } else {
        json_string = GtmValue::from_byte(baton->ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> previous_node_return JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        return scope.Escape(try_catch.Exception());
    } else {
        temp_object = to_object_n(isolate, json);
    }
#endif

    if (!temp_object->Get(String::NewFromUtf8(isolate, "status"))->IsUndefined()) return scope.Escape(temp_object);

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> previous_node_return exit" << "\n";

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));

        if (baton->async && mode_g == STRICT) {
            if (temp_subs->IsUndefined()) {
                return_object->Set(String::NewFromUtf8(isolate, "result"), Array::New(isolate));
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "result"), temp_subs);
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
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));
        if (!temp_subs->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = temp_object->Get(String::NewFromUtf8(isolate, "data"));
        if (!temp_data->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "data"), temp_data);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    if (debug_g > OFF) cout << "\nDEBUG> previous_node_return exit" << "\n";

    return scope.Escape(return_object);
} // @end previous_node_return function

/*
 * @function {private} procedure_return
 * @summary Return value from an arbitrary procedure/subroutine
 * @param {Baton*} baton - struct containing the following members
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @member {string} relink - Whether to relink the procedure before calling it
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> procedure_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> procedure_return enter" << "\n";

    Local<Value> arguments = Local<Value>::New(isolate, baton->arguments_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        Local<Value> argument_string = json_method(arguments, "stringify");
        cout << "DEBUG>> arguments: " << *(UTF8_VALUE_TEMP_N(isolate, argument_string)) << "\n";

        cout << "DEBUG>> relink: " << baton->relink << "\n";
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> procedure = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> procedure_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), String::Empty(isolate));

            return scope.Escape(return_object);
        } else if (mode_g == STRICT) {
            return scope.Escape(String::Empty(isolate));
        } else {
            Local<Value> ret_data = Undefined(isolate);

            return scope.Escape(ret_data);
        }
    } else {
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->routine) {
            return_object->Set(String::NewFromUtf8(isolate, "routine"), localize_name(procedure));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "procedure"), localize_name(procedure));
        }

        if (!arguments->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "arguments"), arguments);

        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), String::Empty(isolate));
        }
    }

    if (debug_g > OFF) cout << "\nDEBUG> procedure_return exit" << "\n";

    return scope.Escape(return_object);
} // @end procedure_return function

/*
 * @function {private} set_return
 * @summary Return data about the store of a global or local node, or an intrinsic special variable
 * @param {Baton*} baton - struct containing the following members
 * @member {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @member {bool} local - Whether the API was called on a local variable, or a global variable
 * @member {bool} async - Whether the API was called asynchronously, or synchronously
 * @member {string} name - The name of the global or local variable
 * @member {Local<Value>} data - V8 object containing the data to store in the node that was called
 * @member {Persistent<Value>} arguments_p - V8 object containing the subscripts that were called
 * @returns {Local<Value>} return_object - Data returned to Node.js
 */
static Local<Value> set_return(Baton* baton)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> set_return enter" << "\n";

    Local<Value> subscripts = Local<Value>::New(isolate, baton->arguments_p);
    Local<Value> data = Local<Value>::New(isolate, baton->data_p);

    if (debug_g > LOW) {
        cout << "DEBUG>> position: " << baton->position << "\n";
        cout << "DEBUG>> local: " << baton->local << "\n";
        cout << "DEBUG>> async: " << baton->async << "\n";
        cout << "DEBUG>> name: " << baton->name << "\n";

        if (!subscripts->IsUndefined()) {
            Local<Value> subscript_string = json_method(subscripts, "stringify");
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subscript_string)) << "\n";
        }

        cout << "DEBUG>> data: " << *(UTF8_VALUE_TEMP_N(isolate, data)) << "\n";
    }

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, baton->name.c_str());

    if (baton->position) {
        if (debug_g > OFF) cout << "\nDEBUG> set_return exit" << "\n";

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "0"));

            return scope.Escape(return_object);
        } else if (mode_g == STRICT) {
            return scope.Escape(Number::New(isolate, 0));
        } else {
            Local<Value> ret_data = Undefined(isolate);

            return scope.Escape(ret_data);
        }
    } else {
        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        }

        if (baton->local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "data"), data);

        if (baton->async && mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "0"));
        } else if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
        }
    }

    if (debug_g > OFF) cout << "\nDEBUG> set_return exit" << "\n";

    return scope.Escape(return_object);
} // @end set_return function

/*
 * @function {public} async_work
 * @summary Call in to YottaDB/GT.M asynchronously, via a Node.js worker thread
 * @param {uv_work_t*} request - A pointer to the Baton structure for transferring data between the main thread and worker thread
 * @returns void
 */
void async_work(uv_work_t* request)
{
    if (debug_g > LOW) cout << "\nDEBUG>> async_work enter" << "\n";

    Baton* baton = static_cast<Baton*>(request->data);

    baton->status = (*baton->function)(baton);

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";
    if (debug_g > LOW) cout << "DEBUG>> async_work exit" << "\n";

    return;
} // @end async_work function

/*
 * @function {public} async_after
 * @summary Call in to the return functions, passing the data from YottaDB/GT.M, after receiving the data from the worker thread
 * @param {uv_work_t*} request - A pointer to the Baton structure for transferring data between the main thread and worker thread
 * @returns void
 */
void async_after(uv_work_t* request, int status)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > LOW) cout << "\nDEBUG>> async_after enter: " << status << "\n";

    Baton* baton = static_cast<Baton*>(request->data);

    Local<Value> error_code = Null(isolate);
    Local<Value> return_object;
    Local<Value> error_object = Object::New(isolate);

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->callback_p.Reset();
        baton->arguments_p.Reset();
        baton->data_p.Reset();
        delete baton;

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK && baton->status != YDB_ERR_GVUNDEF && baton->status != YDB_ERR_LVUNDEF) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (debug_g > LOW) cout << "DEBUG>> " << NODEM_DB << " error code: " << baton->status << "\n";

        if (mode_g == STRICT) {
            error_code = Number::New(isolate, 1);

            return_object = error_status(baton->msg_buf, baton->position, baton->async);
        } else {
            error_object = error_status(baton->msg_buf, baton->position, baton->async);

            error_code = Exception::Error(String::NewFromUtf8(isolate,
                    *(UTF8_VALUE_TEMP_N(isolate, ((Object*) *error_object)->Get(String::NewFromUtf8(isolate, "errorMessage"))))));
            ((Object*) *error_code)->Set(String::NewFromUtf8(isolate, "ok"),
                    ((Object*) *error_object)->Get(String::NewFromUtf8(isolate, "ok")));
            ((Object*) *error_code)->Set(String::NewFromUtf8(isolate, "errorCode"),
                    ((Object*) *error_object)->Get(String::NewFromUtf8(isolate, "errorCode")));
            ((Object*) *error_code)->Set(String::NewFromUtf8(isolate, "errorMessage"),
                    ((Object*) *error_object)->Get(String::NewFromUtf8(isolate, "errorMessage")));

            return_object = Undefined(isolate);
        }
    } else {
        return_object = (*baton->function_return)(baton);

        if (mode_g == STRICT) error_code = Number::New(isolate, 0);
    }

    Local<Value> argv[2] = {error_code, return_object};
    call_n(isolate, Local<Function>::New(isolate, baton->callback_p), Null(isolate), 2, argv);

    baton->callback_p.Reset();
    baton->arguments_p.Reset();
    baton->data_p.Reset();
    delete baton;

    if (debug_g > LOW) cout << "DEBUG>> async_after exit" << "\n";

    return;
} // @end async_after function

// ***Begin Public APIs***

/*
 * @method {public} Gtm::close
 * @summary Close a connection with YottaDB/GT.M
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::close(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::close enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    if (args[0]->IsObject() && has_n(isolate, to_object_n(isolate, args[0]), String::NewFromUtf8(isolate, "resetTerminal"))) {
        reset_term_g = boolean_value_n(isolate, to_object_n(isolate, args[0])->Get(String::NewFromUtf8(isolate, "resetTerminal")));
    }

    if (debug_g > LOW) cout << "DEBUG>> resetTerminal: " << reset_term_g << "\n";

    uv_mutex_lock(&mutex_g);

#if YDB_SIMPLE_API == 1
    if (ydb_exit() != YDB_OK) {
#else
    if (gtm_exit() != EXIT_SUCCESS) {
#endif
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
    } else {
        gtm_state_g = CLOSED;
    }

    uv_mutex_unlock(&mutex_g);

    if (signal_sigint_g == true) {
        if (sigaction(SIGINT, &signal_attr_g, NULL) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, &signal_attr_g, NULL) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, &signal_attr_g, NULL) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGTERM handler")));
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

            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDOUT_FILENO)) {
        if (tcsetattr(STDOUT_FILENO, TCSANOW, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDERR_FILENO)) {
        if (tcsetattr(STDERR_FILENO, TCSANOW, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    }

    if (signal_sigint_g == true && nocenable_g != NULL) {
        if (setenv("gtm_nocenable", nocenable_g, 1) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    }

    if (mode_g == STRICT) {
        args.GetReturnValue().Set(String::NewFromUtf8(isolate, "1"));
    } else {
        args.GetReturnValue().Set(Undefined(isolate));
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::close exit" << endl;

    return;
} // @end Gtm::close method

/*
 * @method {public} Gtm::data
 * @summary Check if global or local node has data and/or children or not
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::data(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::data enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    } else {
        glvn = args[0];

        if (args_count > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, subscripts);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::data;
#else
    baton->function = &gtm::data;
#endif
    baton->function_return = &data_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::data(baton);
#else
    baton->status = gtm::data(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into data_return" << "\n";

    Local<Value> return_object = data_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::data exit" << endl;

    return;
} // @end Gtm::data method

/*
 * @method {public} Gtm::function
 * @summary Call an arbitrary extrinsic function
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::function(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::function enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> function;
    Local<Value> arguments = Undefined(isolate);
    uint32_t relink = auto_relink_g;
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        function = arg_object->Get(String::NewFromUtf8(isolate, "function"));

        if (function->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'function' property")));
            return;
        }

        arguments = arg_object->Get(String::NewFromUtf8(isolate, "arguments"));

        if (has_n(isolate, arg_object, String::NewFromUtf8(isolate, "autoRelink"))) {
            relink = boolean_value_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "autoRelink")));
        }
    } else {
        function = args[0];

        if (args_count > 1) {
            Local<Array> temp_arguments = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_arguments->Set(i - 1, args[i]);
            }

            arguments = temp_arguments;
        }

        position = true;
    }

    if (!function->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Function must be a string")));
        return;
    } else if (function->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Function must not be an empty string")));
        return;
    }

    Local<Value> arg = Undefined(isolate);

    if (arguments->IsUndefined()) {
        arg = String::Empty(isolate);
    } else if (arguments->IsArray()) {
        arg = encode_arguments(arguments, true);

        if (arg->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Arguments contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'arguments' must be an array")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    Local<Value> name = globalize_name(function);

    string func_s, args_s;

    if (utf8_g == true) {
        func_s = *(UTF8_VALUE_TEMP_N(isolate, name));
        args_s = *(UTF8_VALUE_TEMP_N(isolate, arg));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {arg};

        func_s = gtm_name.to_byte();
        args_s = gtm_args.to_byte();
    }

    if (debug_g > LOW) {
        cout << "DEBUG>> function: " << func_s << "\n";
        cout << "DEBUG>> arguments: " << args_s << endl;
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, arguments);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = func_s;
    baton->args = args_s;
    baton->relink = relink;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
    baton->function = &gtm::function;
    baton->function_return = &function_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    if (debug_g > LOW) {
        cout << "DEBUG>> relink: " << relink << "\n";
        cout << "DEBUG>> mode: " << mode_g << "\n";
    }

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    baton->status = gtm::function(baton);

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    if (baton->status != EXIT_SUCCESS) {
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into function_return" << "\n";

    Local<Value> return_object = function_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::function exit" << endl;

    return;
} // @end Gtm::function method

/*
 * @method {public} Gtm::get
 * @summary Get data from a global or local node, or an intrinsic special variable
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::get(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::get enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    } else {
        glvn = args[0];

        if (args_count > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, subscripts);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::get;
#else
    baton->function = &gtm::get;
#endif
    baton->function_return = &get_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::get(baton);
#else
    baton->status = gtm::get(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK && baton->status != YDB_ERR_GVUNDEF && baton->status != YDB_ERR_LVUNDEF) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into get_return" << "\n";

    Local<Value> return_object = get_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::get exit" << endl;

    return;
} // @end Gtm::get method

/*
 * @method {public} Gtm::global_directory
 * @summary List the globals in a database, with optional filters
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::global_directory(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::global_directory enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    Local<Value> max, lo, hi = Undefined(isolate);

    if (args.Length() > 0 && !args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    } else if (args.Length() > 0) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);

        max = arg_object->Get(String::NewFromUtf8(isolate, "max"));

        if (max->IsUndefined() || !max->IsNumber() || number_value_n(isolate, max) < 0) max = Number::New(isolate, 0);

        lo = arg_object->Get(String::NewFromUtf8(isolate, "lo"));

        if (lo->IsUndefined() || !lo->IsString()) lo = String::Empty(isolate);

        hi = arg_object->Get(String::NewFromUtf8(isolate, "hi"));

        if (hi->IsUndefined() || !hi->IsString()) hi = String::Empty(isolate);
    } else {
        max = Number::New(isolate, 0);
        lo = String::Empty(isolate);
        hi = String::Empty(isolate);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    if (debug_g > LOW) {
        cout << "DEBUG>> mode: " << mode_g << "\n";
        cout << "DEBUG>> max: " << uint32_value_n(isolate, max) << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_global_directory[] = "global_directory";

    static gtm_char_t ret_buf[RET_LEN];

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_global_directory;
    access.rtn_name.length = 16;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << *(UTF8_VALUE_TEMP_N(isolate, lo)) << "\n";
            cout << "DEBUG>> hi: " << *(UTF8_VALUE_TEMP_N(isolate, hi)) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolate, lo)),
          *(UTF8_VALUE_TEMP_N(isolate, hi)), mode_g);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
            cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << *String::Utf8Value(lo) << "\n";
            cout << "DEBUG>> hi: " << *String::Utf8Value(hi) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_global_directory, ret_buf, uint32_value_n(isolate, max),
          *String::Utf8Value(lo), *String::Utf8Value(hi), mode_g);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
            cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_global_directory, ret_buf, uint32_value_n(isolate, max), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::global_directory JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        args.GetReturnValue().Set(try_catch.Exception());
    } else {
        args.GetReturnValue().Set(Local<Array>::Cast(json));
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::global_directory exit" << endl;

    return;
} // @end Gtm::global_directory method

/*
 * @method {public} Gtm::help
 * @summary Built-in help menu for Gtm methods
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::help(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "version"))) {
        cout << "version or about method:\n"
            "\tDisplay version information - includes database version if connection has been established\n"
            "\n\tArguments: {None}\n"
            "\n\tReturns on success: {string}\n"
            "\n\tReturns on failure: Should never fail\n"
            "\n\tFor more information about the version/about method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "open"))) {
        cout << "open method:\n"
            "\tOpen connection to the database - all other methods, except for version, require an open database connection\n"
            "\n\tRequired arguments: {None}\n"
            "\n\tOptional arguments:\n"
            "\t{\n"
            "\t\tglobalDirectory|namespace:\t{string} <none>,\n"
            "\t\troutinesPath:\t\t\t{string} <none>,\n"
            "\t\tcallinTable:\t\t\t{string} <none>,\n"
            "\t\tipAddress|ip_address:\t\t{string} <none>,\n"
            "\t\ttcpPort|tcp_port:\t\t{number|string} <none>,\n"
            "\t\tcharset:\t\t\t{string} [<utf8|utf-8>|m|binary|ascii]/i,\n"
            "\t\tmode:\t\t\t\t{string} [<canonical>|strict]/i,\n"
            "\t\tautoRelink:\t\t\t{boolean} <false>,\n"
            "\t\tdebug:\t\t\t\t{boolean} <false>|{string} [<off>|low|medium|high]/i|{number} [<0>|1|2|3],\n"
            "\t\tsignalHandler:\t\t\t{boolean} <true>|{object {boolean} sigint,sigterm,sigquit/i} [<true>|false] [<1>|0]\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tresult:\t\t{optional} {number} 1,\n"
            "\t\tpid|gtm_pid:\t{number}|{string}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\t- Failures from bad environment set-ups result in internal errors from " NODEM_DB "\n"
            "\n\tFor more information about the open method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "close"))) {
        cout << "close method:\n"
            "\tClose connection to the database - once closed, cannot be reopened during the current process\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "data"))) {
        cout << "data method:\n"
            "\tDisplay information about the existence of data and/or children in global or local variables\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "get"))) {
        cout << "get method:\n"
            "\tRetrieve the data stored at a global or local node, or intrinsic special variable (ISV)\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
            "\t\tdefined:\t{number} [0|1]\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "set"))) {
        cout << "set method:\n"
            "\tStore data in a global or local node, or intrinsic special variable (ISV)\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
            "\t{undefined}|{number} 0\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the set method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "kill"))) {
        cout << "kill method:\n"
            "\tRemove data stored in a global or global node, or in a local or local node, or remove all local variables\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
            "\t{undefined}|{number} 0\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the kill method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "order"))) {
        cout << "order or next method:\n"
            "\tRetrieve the next node, at the current subscript depth\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "previous"))) {
        cout << "previous method:\n"
            "\tRetrieve the previous node, at the current subscript depth\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "nextNode"))) {
        cout << "nextNode or next_node method:\n"
            "\tRetrieve the next node, regardless of subscript depth\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
            "\t\tdefined:\t{number} [0|1]\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "previousNode"))) {
        cout << "previousNode or previous_node method:\n"
            "\tRetrieve the previous node, regardless of subscript depth\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
            "\t\tdefined:\t{number} [0|1]\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "merge"))) {
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "increment"))) {
        cout << "increment method:\n"
            "\tAtomically increment or decrement a global or local data node\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
            "\t^global|local, [subscripts+], increment\n"
            "\n\tReturns on success:\n"
            "\t{string|number}\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the increment method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "lock"))) {
        cout << "lock method:\n"
            "\tLock a local or global tree, or sub-tree, or individual node - locks are advisory, not mandatory\n"
            "\n\tRequired arguments:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\ttimeout:\t{optional} {number}\n"
            "\t}\n"
            "\n\tOptional arguments: Timeout {number} as second argument\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the lock method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "unlock"))) {
        cout << "unlock method:\n"
            "\tUnlock a local or global tree, or sub-tree, or individual node, or release all locks held by process\n"
            "\n\tRequired arguments: {None} - Without an argument, will clear the entire lock table for that process\n"
            "\tReturns on success: {number}|{string} 0\n"
            "\n\tOptional arguments:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}}\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tglobal|local:\t{string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\tresult:\t\t{number} 0\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the unlock method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "globalDirectory"))) {
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "localDirectory"))) {
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "function"))) {
        cout << "function method:\n"
            "\tCall an extrinsic function in " NODEM_DB " code\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "procedure"))) {
        cout << "procedure or routine method:\n"
            "\tCall a procedure/routine/subroutine label in " NODEM_DB " code\n"
            "\tPassing a function, with two arguments (error and result), as the last argument, will call the API asynchronously\n"
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
            "\t\tresult:\t\t\t{optional} {string} 0\n"
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
            "\t{undefined}|{string} 0\n"
            "\n\tReturns on failure:\n"
            "\t{exception string}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the procedure/routine method, please refer to the README.md file\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "retrieve"))) {
        cout << "retrieve method:\n"
            "\tRetrieve a local or global tree or sub-tree structure as an object - NOT YET IMPLEMENTED\n"
            << endl;
    } else if (to_string_n(isolate, args[0])->StrictEquals(String::NewFromUtf8(isolate, "update"))) {
        cout << "update method:\n"
            "\tStore an object in a local or global tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            << endl;
    } else {
#if YDB_IMPLEMENTATION == 1
        cout << "NodeM: Ydb and Gtm object API help menu - methods:\n"
#else
        cout << "NodeM: Gtm object API help menu - methods:\n"
#endif
            "\nversion\t\tDisplay version information - includes database version if connection has been established (AKA about)\n"
            "open\t\tOpen connection to the database - all other methods, except for version, require an open database connection\n"
            "close\t\tClose connection to the database - once closed, cannot be reopened during the current process\n"
            "data\t\tDisplay information about the existence of data and/or children in globals or local variables\n"
            "get\t\tRetrieve the data stored at a global or local node, or intrinsic special variable (ISV)\n"
            "set\t\tStore data in a global or local node, or intrinsic special variable (ISV)\n"
            "kill\t\tRemove data stored in a global or global node, or in a local or local node; or remove all local variables\n"
            "order\t\tRetrieve the next node, at the current subscript depth (AKA next)\n"
            "previous\tRetrieve the previous node, at the current subscript depth\n"
            "nextNode\tRetrieve the next node, regardless of subscript depth\n"
            "previousNode\tRetrieve the previous node, regardless of subscript depth\n"
            "merge\t\tCopy an entire data tree, or sub-tree, from a global or local array, to another global or local array\n"
            "increment\tAtomically increment a global or local data node\n"
            "lock\t\tLock a global or local tree, or sub-tree, or individual node - locks are advisory, not mandatory\n"
            "unlock\t\tUnlock a global or local tree, or sub-tree, or individual node; or release all locks held by process\n"
            "globalDirectory\tList globals stored in the database\n"
            "localDirectory\tList local variables stored in the symbol table\n"
            "function\tCall an extrinsic function in " NODEM_DB " code\n"
            "procedure\tCall a procedure/routine/subroutine label in " NODEM_DB " code (AKA routine)\n"
            "retrieve\tRetrieve a global or local tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            "update\t\tStore an object in a global or local tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            "\nFor more information about each method, call help with the method name as an argument\n"
            << endl;
    }

    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "NodeM - Copyright (C) 2012-2019 Fourth Watch Software LC"));
    return;
} // @end Gtm::help method

/*
 * @method {public} Gtm::increment
 * @summary Increment or decrement the number in a global or local node
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::increment(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::increment enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    Local<Value> increment = Number::New(isolate, 1);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));

        if (!arg_object->Get(String::NewFromUtf8(isolate, "increment"))->IsUndefined()) {
            increment = to_number_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "increment")));
        }
    } else {
        glvn = args[0];
        if (args_count > 1) increment = args[args_count - 1];

        if (args_count > 2) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 2);

            for (unsigned int i = 1; i < args_count - 1; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif

        cout << "DEBUG>> increment: " << number_value_n(isolate, increment) << "\n";
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, subscripts);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->incr = number_value_n(isolate, increment);
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::increment;
#else
    baton->function = &gtm::increment;
#endif
    baton->function_return = &increment_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::increment(baton);
#else
    baton->status = gtm::increment(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into increment_return" << "\n";

    Local<Value> return_object = increment_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::increment exit" << endl;

    return;
} // @end Gtm::increment method

/*
 * @method {public} Gtm::kill
 * @summary Kill a global or local, or global or local node, or remove the entire symbol table
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::kill(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::kill enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;
    int32_t node_only = -1;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));

        if (has_n(isolate, arg_object, String::NewFromUtf8(isolate, "nodeOnly"))) {
            node_only = boolean_value_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "nodeOnly")));
        }
    } else if (args_count > 0) {
        glvn = args[0];

        if (args_count > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_subscripts->Set(i - 1, args[i]);
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
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    } else if (glvn->IsUndefined()) {
        glvn = String::Empty(isolate);
        local = true;
    } else {
        if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
            bool error = false;
            subs_array = build_subscripts(subscripts, error);

            if (error) {
                Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
                isolate->ThrowException(Exception::SyntaxError(error_message));
                return;
            }
#else
            subs = encode_arguments(subscripts);

            if (subs->IsUndefined()) {
                Local<String> error_message = String::NewFromUtf8(isolate, "Property 'subscripts' contains invalid data");
                isolate->ThrowException(Exception::SyntaxError(error_message));
                return;
            }
#endif
        } else if (!subscripts->IsUndefined()) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
            return;
        }
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, subscripts);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->node_only = node_only;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::kill;
#else
    baton->function = &gtm::kill;
#endif
    baton->function_return = &kill_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::kill(baton);
#else
    baton->status = gtm::kill(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into kill_return" << "\n";

    Local<Value> return_object = kill_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::kill exit" << endl;

    return;
} // @end Gtm::kill method

/*
 * @method {public} Gtm::local_directory
 * @summary List the local variables in the symbol table, with optional filters
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::local_directory(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::local_directory enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    Local<Value> max, lo, hi = Undefined(isolate);

    if (args.Length() > 0 && !args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    } else if (args.Length() > 0) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);

        max = arg_object->Get(String::NewFromUtf8(isolate, "max"));

        if (max->IsUndefined() || !max->IsNumber() || number_value_n(isolate, max) < 0) max = Number::New(isolate, 0);

        lo = arg_object->Get(String::NewFromUtf8(isolate, "lo"));

        if (lo->IsUndefined() || !lo->IsString()) lo = String::Empty(isolate);

        hi = arg_object->Get(String::NewFromUtf8(isolate, "hi"));

        if (hi->IsUndefined() || !hi->IsString()) hi = String::Empty(isolate);
    } else {
        max = Number::New(isolate, 0);
        lo = String::Empty(isolate);
        hi = String::Empty(isolate);
    }

    if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, lo)))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'lo' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, lo)))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'lo' is an invalid name")));
        return;
    }

    if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, hi)))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'hi' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, hi)))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'hi' is an invalid name")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    if (debug_g > LOW) {
        cout << "DEBUG>> mode: " << mode_g << "\n";
        cout << "DEBUG>> max: " << uint32_value_n(isolate, max) << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_local_directory[] = "local_directory";

    static gtm_char_t ret_buf[RET_LEN];

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_local_directory;
    access.rtn_name.length = 15;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << *(UTF8_VALUE_TEMP_N(isolate, lo)) << "\n";
            cout << "DEBUG>> hi: " << *(UTF8_VALUE_TEMP_N(isolate, hi)) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), *(UTF8_VALUE_TEMP_N(isolate, lo)),
          *(UTF8_VALUE_TEMP_N(isolate, hi)), mode_g);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
            cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, uint32_value_n(isolate, max), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << *String::Utf8Value(lo) << "\n";
            cout << "DEBUG>> hi: " << *String::Utf8Value(hi) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_local_directory, ret_buf, uint32_value_n(isolate, max),
          *String::Utf8Value(lo), *String::Utf8Value(hi), mode_g);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) {
            cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
            cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_local_directory, ret_buf, uint32_value_n(isolate, max), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::local_directory JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        args.GetReturnValue().Set(try_catch.Exception());
    } else {
        args.GetReturnValue().Set(Local<Array>::Cast(json));
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::local_directory exit" << endl;

    return;
} // @end Gtm::local_directory method

/*
 * @method {public} Gtm::lock
 * @summary Lock a global or local node, incrementally
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::lock(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::lock enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = to_object_n(isolate, args[0]);
    Local<Value> glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));
    bool local = false;

    if (glvn->IsUndefined()) {
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        } else {
            local = true;
        }
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    Local<Value> subs = Undefined(isolate);

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Property 'subscripts' contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    Local<Value> timeout;

    if (args.Length() > 1) {
        timeout = to_number_n(isolate, args[1]);

        if (number_value_n(isolate, timeout) < 0) timeout = Number::New(isolate, 0);
    } else if (has_n(isolate, arg_object, String::NewFromUtf8(isolate, "timeout"))) {
        timeout = to_number_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "timeout")));

        if (number_value_n(isolate, timeout) < 0) timeout = Number::New(isolate, 0);
    } else {
        timeout = Number::New(isolate, -1);
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    if (debug_g > LOW) {
        cout << "DEBUG>> timeout: " << number_value_n(isolate, timeout) << "\n";
        cout << "DEBUG>> mode: " << mode_g << "\n";
    }

    gtm_status_t stat_buf;
    gtm_char_t gtm_lock[] = "lock";

    static gtm_char_t ret_buf[RET_LEN];

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_lock;
    access.rtn_name.length = 4;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << name_msg << *(UTF8_VALUE_TEMP_N(isolate, name)) << "\n";
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subs)) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, *(UTF8_VALUE_TEMP_N(isolate, name)), *(UTF8_VALUE_TEMP_N(isolate, subs)),
          number_value_n(isolate, timeout), mode_g);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) {
            cout << name_msg << gtm_name.to_byte() << "\n";
            cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, gtm_name.to_byte(), gtm_subs.to_byte(),
          number_value_n(isolate, timeout), mode_g);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << name_msg << *String::Utf8Value(name) << "\n";
            cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_lock, ret_buf, *String::Utf8Value(name), *String::Utf8Value(subs),
          number_value_n(isolate, timeout), mode_g);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) {
            cout << name_msg << gtm_name.to_byte() << "\n";
            cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_lock, ret_buf, gtm_name.to_byte(), gtm_subs.to_byte(),
          number_value_n(isolate, timeout), mode_g);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::lock JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        args.GetReturnValue().Set(try_catch.Exception());
        return;
    } else {
        temp_object = to_object_n(isolate, json);
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), glvn);
        }

        if (!subscripts->IsUndefined()) {
            Local<Value> temp_subscripts = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));

            if (!temp_subscripts->IsUndefined()) {
                return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subscripts);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);
            }
        }
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);
    }

    return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "result")));

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::lock exit" << endl;

    return;
} // @end Gtm::lock method

/*
 * @method {public} Gtm::merge
 * @summary Merge an global or local array node to another global or local array node
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::merge(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::merge enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = to_object_n(isolate, args[0]);
    Local<Value> from_test = arg_object->Get(String::NewFromUtf8(isolate, "from"));
    Local<Value> to_test = arg_object->Get(String::NewFromUtf8(isolate, "to"));
    bool from_local = false;
    bool to_local = false;

    if (!has_n(isolate, arg_object, String::NewFromUtf8(isolate, "from"))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'from' property")));
        return;
    } else if (!from_test->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "'from' property must be an object")));
        return;
    }

    Local<Object> from = to_object_n(isolate, from_test);

    if (!has_n(isolate, arg_object, String::NewFromUtf8(isolate, "to"))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'to' property")));
        return;
    } else if (!to_test->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "'to' property must be an object")));
        return;
    }

    Local<Object> to = to_object_n(isolate, to_test);
    Local<Value> from_glvn = from->Get(String::NewFromUtf8(isolate, "global"));

    if (from_glvn->IsUndefined()) {
        from_glvn = from->Get(String::NewFromUtf8(isolate, "local"));

        if (from_glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need a 'global' or 'local' property in your 'from' object")));
            return;
        } else {
            from_local = true;
        }
    }

    if (!from_glvn->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global in 'from' must be a string")));
        return;
    } else if (from_glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                "Global in 'from' must not be an empty string")));
        return;
    }

    Local<Value> to_glvn = to->Get(String::NewFromUtf8(isolate, "global"));

    if (to_glvn->IsUndefined()) {
        to_glvn = to->Get(String::NewFromUtf8(isolate, "local"));

        if (to_glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need a 'global' or 'local' property in your 'to' object")));
            return;
        } else {
            to_local = true;
        }
    }

    if (!to_glvn->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global in 'to' must be a string")));
        return;
    } else if (to_glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global in 'to' must not be an empty string")));
        return;
    }

    Local<Value> from_subscripts = from->Get(String::NewFromUtf8(isolate, "subscripts"));
    Local<Value> from_subs = Undefined(isolate);

    if (from_subscripts->IsUndefined()) {
        from_subs = String::Empty(isolate);
    } else if (from_subscripts->IsArray()) {
        from_subs = encode_arguments(from_subscripts);

        if (from_subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate,
                    "Property 'subscripts' in 'from' object contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate,
                "Property 'subscripts' in 'from' must be an array")));
        return;
    }

    Local<Value> to_subscripts = to->Get(String::NewFromUtf8(isolate, "subscripts"));
    Local<Value> to_subs = Undefined(isolate);

    if (to_subscripts->IsUndefined()) {
        to_subs = String::Empty(isolate);
    } else if (to_subscripts->IsArray()) {
        to_subs = encode_arguments(to_subscripts);

        if (to_subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate,
                    "Property 'subscripts' in 'to' object contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate,
                "Property 'subscripts' in 'to' must be an array")));
        return;
    }

    const char* from_name_msg;
    Local<Value> from_name;

    if (from_local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, from_glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        from_name_msg = "DEBUG>> from_local: ";
        from_name = localize_name(from_glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, from_name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate,
              "Property 'local' in 'from' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, from_glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        from_name_msg = "DEBUG>> from_global: ";
        from_name = globalize_name(from_glvn);
    }

    const char* to_name_msg;
    Local<Value> to_name;

    if (to_local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, to_glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        to_name_msg = "DEBUG>> to_local: ";
        to_name = localize_name(to_glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, to_name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate,
                    "Property 'local' in 'to' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, to_glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        to_name_msg = "DEBUG>> to_global: ";
        to_name = globalize_name(to_glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_merge[] = "merge";

    static gtm_char_t ret_buf[RET_LEN];

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_merge;
    access.rtn_name.length = 5;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << from_name_msg << *(UTF8_VALUE_TEMP_N(isolate, from_name)) << "\n";
            cout << "DEBUG>> from_subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, from_subs)) << "\n";
            cout << to_name_msg << *(UTF8_VALUE_TEMP_N(isolate, to_name)) << "\n";
            cout << "DEBUG>> to_subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, to_subs)) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, *(UTF8_VALUE_TEMP_N(isolate, from_name)), *(UTF8_VALUE_TEMP_N(isolate, from_subs)),
          *(UTF8_VALUE_TEMP_N(isolate, to_name)), *(UTF8_VALUE_TEMP_N(isolate, to_subs)), mode_g);
    } else {
        GtmValue gtm_from_name {from_name};
        GtmValue gtm_from_subs {from_subs};
        GtmValue gtm_to_name {to_name};
        GtmValue gtm_to_subs {to_subs};

        if (debug_g > LOW) {
            cout << from_name_msg << gtm_from_name.to_byte() << "\n";
            cout << "DEBUG>> from_subscripts: " << gtm_from_subs.to_byte() << "\n";
            cout << to_name_msg << gtm_to_name.to_byte() << "\n";
            cout << "DEBUG>> to_subscripts: " << gtm_to_subs.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, ret_buf, gtm_from_name.to_byte(), gtm_from_subs.to_byte(),
          gtm_to_name.to_byte(), gtm_to_subs.to_byte(), mode_g);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << from_name_msg << *String::Utf8Value(from_name) << "\n";
            cout << "DEBUG>> from_subscripts: " << *String::Utf8Value(from_subs) << "\n";
            cout << to_name_msg << *String::Utf8Value(to_name) << "\n";
            cout << "DEBUG>> to_subscripts: " << *String::Utf8Value(to_subs) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_merge, ret_buf, *String::Utf8Value(from_name), *String::Utf8Value(from_subs),
          *String::Utf8Value(to_name), *String::Utf8Value(to_subs), mode_g);
    } else {
        GtmValue gtm_from_name {from_name};
        GtmValue gtm_from_subs {from_subs};
        GtmValue gtm_to_name {to_name};
        GtmValue gtm_to_subs {to_subs};

        if (debug_g > LOW) {
            cout << from_name_msg << gtm_from_name.to_byte() << "\n";
            cout << "DEBUG>> from_subscripts: " << gtm_from_subs.to_byte() << "\n";
            cout << to_name_msg << gtm_to_name.to_byte() << "\n";
            cout << "DEBUG>> to_subscripts: " << gtm_to_subs.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_merge, ret_buf, gtm_from_name.to_byte(), gtm_from_subs.to_byte(),
          gtm_to_name.to_byte(), gtm_to_subs.to_byte(), mode_g);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::merge JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        args.GetReturnValue().Set(try_catch.Exception());
        return;
    } else {
        temp_object = to_object_n(isolate, json);
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

        if (from_local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), from_name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(from_glvn));
        }

        if (!from_subscripts->IsUndefined() || !to_subscripts->IsUndefined()) {
            Local<Value> temp_subscripts = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));

            if (!temp_subscripts->IsUndefined()) {
                return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subscripts);
            } else {
                if (!from_subscripts->IsUndefined()) {
                    return_object->Set(String::NewFromUtf8(isolate, "subscripts"), from_subscripts);
                } else {
                    return_object->Set(String::NewFromUtf8(isolate, "subscripts"), to_subscripts);
                }
            }
        }

        return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "1"));
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (from_local) {
            from->Set(String::NewFromUtf8(isolate, "local"), from_name);
        } else {
            from->Set(String::NewFromUtf8(isolate, "global"), localize_name(from_glvn));
        }

        return_object->Set(String::NewFromUtf8(isolate, "from"), from);

        if (to_local) {
            to->Set(String::NewFromUtf8(isolate, "local"), to_name);
        } else {
            to->Set(String::NewFromUtf8(isolate, "global"), localize_name(to_glvn));
        }

        return_object->Set(String::NewFromUtf8(isolate, "to"), to);
        return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 1));
    }

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::merge exit" << endl;

    return;
} // @end Gtm::merge method

/*
 * @method {public} Gtm::next_node
 * @summary Return the next global or local node, depth first
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::next_node(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::next_node enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    } else {
        glvn = args[0];

        if (args_count > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, Undefined(isolate));
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::next_node;
#else
    baton->function = &gtm::next_node;
#endif
    baton->function_return = &next_node_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::next_node(baton);
#else
    baton->status = gtm::next_node(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK && baton->status != YDB_NODE_END) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into next_node_return" << "\n";

    Local<Value> return_object = next_node_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::next_node exit" << endl;

    return;
} // @end Gtm::next_node method

/*
 * @method {public} Gtm::open
 * @summary Open a connection with YottaDB/GT.M
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::open(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (gtm_state_g == CLOSED) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection cannot be reopened")));
        return;
    } else if (gtm_state_g == OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection already open")));
        return;
    }

    char* relink = getenv("NODEM_AUTO_RELINK");
    if (relink != NULL) auto_relink_g = static_cast<bool>(atoi(relink));

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);

        if (has_n(isolate, arg_object, String::NewFromUtf8(isolate, "debug"))) {
            UTF8_VALUE_N(isolate, debug, arg_object->Get(String::NewFromUtf8(isolate, "debug")));

            if (strcasecmp(*debug, "off") == 0) {
                debug_g = OFF;
            } else if (strcasecmp(*debug, "low") == 0) {
                debug_g = LOW;
            } else if (strcasecmp(*debug, "medium") == 0) {
                debug_g = MEDIUM;
            } else if (strcasecmp(*debug, "high") == 0) {
                debug_g = HIGH;
            } else {
                debug_g = static_cast<debug_t>(uint32_value_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "debug"))));
            }
        }

        if (debug_g > OFF) cout << "\nDEBUG> Gtm::open enter" << endl;
        if (debug_g > LOW) cout << "DEBUG>> debug: " << debug_g << "\n";

        Local<Value> global_directory = arg_object->Get(String::NewFromUtf8(isolate, "globalDirectory"));

        if (global_directory->IsUndefined()) global_directory = arg_object->Get(String::NewFromUtf8(isolate, "namespace"));

        if (!global_directory->IsUndefined() && global_directory->IsString()) {
            if (debug_g > LOW) cout << "DEBUG>> globalDirectory: " << *(UTF8_VALUE_TEMP_N(isolate, global_directory)) << "\n";

#if YDB_SIMPLE_API == 1
            if (setenv("ydb_gbldir", *(UTF8_VALUE_TEMP_N(isolate, global_directory)), 1) == -1) {
#else
            if (setenv("gtmgbldir", *(UTF8_VALUE_TEMP_N(isolate, global_directory)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        Local<Value> routines_path = arg_object->Get(String::NewFromUtf8(isolate, "routinesPath"));

        if (!routines_path->IsUndefined() && routines_path->IsString()) {
            if (debug_g > LOW) cout << "DEBUG>> routinesPath: " << *(UTF8_VALUE_TEMP_N(isolate, routines_path)) << "\n";

#if YDB_SIMPLE_API == 1
            if (setenv("ydb_routines", *(UTF8_VALUE_TEMP_N(isolate, routines_path)), 1) == -1) {
#else
            if (setenv("gtmroutines", *(UTF8_VALUE_TEMP_N(isolate, routines_path)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        Local<Value> callin_table = arg_object->Get(String::NewFromUtf8(isolate, "callinTable"));

        if (!callin_table->IsUndefined() && callin_table->IsString()) {
            if (debug_g > LOW) cout << "DEBUG>> callinTable: " << *(UTF8_VALUE_TEMP_N(isolate, callin_table)) << "\n";

#if YDB_SIMPLE_API == 1
            if (setenv("ydb_ci", *(UTF8_VALUE_TEMP_N(isolate, callin_table)), 1) == -1) {
#else
            if (setenv("GTMCI", *(UTF8_VALUE_TEMP_N(isolate, callin_table)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        Local<Value> addr = arg_object->Get(String::NewFromUtf8(isolate, "ipAddress"));
        const char* addrMsg;

        if (addr->IsUndefined()) {
            addr = arg_object->Get(String::NewFromUtf8(isolate, "ip_address"));
            addrMsg = "ip_address must be a string";
        } else {
            addrMsg = "ipAddress must be a string";
        }

        Local<Value> port = arg_object->Get(String::NewFromUtf8(isolate, "tcpPort"));
        const char* portMsg;

        if (port->IsUndefined()) {
            port = arg_object->Get(String::NewFromUtf8(isolate, "tcp_port"));
            portMsg = "tcp_port must be a number or string";
        } else {
            portMsg = "tcpPort must be a number or string";
        }

        if (!addr->IsUndefined() || !port->IsUndefined()) {
            Local<Value> gtcm_nodem;

            if (addr->IsUndefined()) addr = Local<Value>::New(isolate, String::NewFromUtf8(isolate, "127.0.0.1"));

            if (!addr->IsString()) {
                isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, addrMsg)));
                return;
            }

            if (port->IsUndefined()) port = Local<Value>::New(isolate, String::NewFromUtf8(isolate, "6789"));

            if (port->IsNumber() || port->IsString()) {
                Local<String> gtcm_port = concat_n(isolate, String::NewFromUtf8(isolate, ":"), to_string_n(isolate, port));
                gtcm_nodem = concat_n(isolate, to_string_n(isolate, addr), gtcm_port);
            } else {
                isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, portMsg)));
                return;
            }

#if YDB_SIMPLE_API == 1
            if (debug_g > LOW) cout << "DEBUG>> ydb_cm_NODEM: " << *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)) << "\n";

            if (setenv("ydb_cm_NODEM", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)), 1) == -1) {
#else
            if (debug_g > LOW) cout << "DEBUG>> GTCM_NODEM: " << *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)) << "\n";

            if (setenv("GTCM_NODEM", *(UTF8_VALUE_TEMP_N(isolate, gtcm_nodem)), 1) == -1) {
#endif
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }

        if (has_n(isolate, arg_object, String::NewFromUtf8(isolate, "autoRelink"))) {
            auto_relink_g = boolean_value_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "autoRelink")));
        }

        if (debug_g > LOW) cout << "DEBUG>> autoRelink: " << auto_relink_g << "\n";

        UTF8_VALUE_N(isolate, nodem_mode, arg_object->Get(String::NewFromUtf8(isolate, "mode")));

        if (strcasecmp(*nodem_mode, "strict") == 0) mode_g = STRICT;

        if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

        UTF8_VALUE_N(isolate, data_charset, arg_object->Get(String::NewFromUtf8(isolate, "charset")));

        if (strcasecmp(*data_charset, "m") == 0 || strcasecmp(*data_charset, "binary") == 0 ||
                strcasecmp(*data_charset, "ascii") == 0) {
            utf8_g = false;
        } else if (strcasecmp(*data_charset, "utf-8") == 0 || strcasecmp(*data_charset, "utf8") == 0) {
            utf8_g = true;
        }

        if (debug_g > LOW) cout << "DEBUG>> charset: " << utf8_g << endl;

        if (has_n(isolate, arg_object, String::NewFromUtf8(isolate, "signalHandler"))) {
            if (arg_object->Get(String::NewFromUtf8(isolate, "signalHandler"))->IsObject()) {
                Local<Object> signal_handler = to_object_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "signalHandler")));

                if (has_n(isolate, signal_handler, String::NewFromUtf8(isolate, "sigint"))) {
                    signal_sigint_g = boolean_value_n(isolate, signal_handler->Get(String::NewFromUtf8(isolate, "sigint")));
                } else if (has_n(isolate, signal_handler, String::NewFromUtf8(isolate, "SIGINT"))) {
                    signal_sigint_g = boolean_value_n(isolate, signal_handler->Get(String::NewFromUtf8(isolate, "SIGINT")));
                }

                if (has_n(isolate, signal_handler, String::NewFromUtf8(isolate, "sigquit"))) {
                    signal_sigquit_g = boolean_value_n(isolate, signal_handler->Get(String::NewFromUtf8(isolate, "sigquit")));
                } else if (has_n(isolate, signal_handler, String::NewFromUtf8(isolate, "SIGQUIT"))) {
                    signal_sigquit_g = boolean_value_n(isolate, signal_handler->Get(String::NewFromUtf8(isolate, "SIGQUIT")));
                }

                if (has_n(isolate, signal_handler, String::NewFromUtf8(isolate, "sigterm"))) {
                    signal_sigterm_g = boolean_value_n(isolate, signal_handler->Get(String::NewFromUtf8(isolate, "sigterm")));
                } else if (has_n(isolate, signal_handler, String::NewFromUtf8(isolate, "SIGTERM"))) {
                    signal_sigterm_g = boolean_value_n(isolate, signal_handler->Get(String::NewFromUtf8(isolate, "SIGTERM")));
                }
            } else {
                Local<Value> signal_handler = arg_object->Get(String::NewFromUtf8(isolate, "signalHandler"));

                signal_sigint_g = signal_sigquit_g = signal_sigterm_g = boolean_value_n(isolate, signal_handler);
            }

            if (debug_g > LOW) {
                cout << "DEBUG>> sigint: " << signal_sigint_g << endl;
                cout << "DEBUG>> sigquit: " << signal_sigquit_g << endl;
                cout << "DEBUG>> sigterm: " << signal_sigterm_g << endl;
            }
        }
    }

    if (signal_sigint_g == true) {
        nocenable_g = getenv("gtm_nocenable");

        if (nocenable_g != NULL) {
            if (setenv("gtm_nocenable", "0", 1) == -1) {
                char error[BUFSIZ];

                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
                return;
            }
        }
    }

    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDOUT_FILENO)) {
        if (tcgetattr(STDOUT_FILENO, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    } else if (isatty(STDERR_FILENO)) {
        if (tcgetattr(STDERR_FILENO, &term_attr_g) == -1) {
            char error[BUFSIZ];

            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
            return;
        }
    }

    if (signal_sigint_g == true) {
        if (sigaction(SIGINT, NULL, &signal_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot retrieve SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, NULL, &signal_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot retrieve SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, NULL, &signal_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot retrieve SIGTERM handler")));
            return;
        }
    }

#if GTM_CIP_API == 1
    gtm::debug_access_g.rtn_name.address = gtm::gtm_debug_g;
    gtm::debug_access_g.rtn_name.length = 5;
    gtm::debug_access_g.handle = NULL;

    gtm::function_access_g.rtn_name.address = gtm::gtm_function_g;
    gtm::function_access_g.rtn_name.length = 8;
    gtm::function_access_g.handle = NULL;

    gtm::procedure_access_g.rtn_name.address = gtm::gtm_procedure_g;
    gtm::procedure_access_g.rtn_name.length = 9;
    gtm::procedure_access_g.handle = NULL;

#if YDB_SIMPLE_API == 0
    gtm::data_access_g.rtn_name.address = gtm::gtm_data_g;
    gtm::data_access_g.rtn_name.length = 4;
    gtm::data_access_g.handle = NULL;

    gtm::get_access_g.rtn_name.address = gtm::gtm_get_g;
    gtm::get_access_g.rtn_name.length = 3;
    gtm::get_access_g.handle = NULL;

    gtm::increment_access_g.rtn_name.address = gtm::gtm_increment_g;
    gtm::increment_access_g.rtn_name.length = 9;
    gtm::increment_access_g.handle = NULL;

    gtm::kill_access_g.rtn_name.address = gtm::gtm_kill_g;
    gtm::kill_access_g.rtn_name.length = 4;
    gtm::kill_access_g.handle = NULL;

    gtm::next_node_access_g.rtn_name.address = gtm::gtm_next_node_g;
    gtm::next_node_access_g.rtn_name.length = 9;
    gtm::next_node_access_g.handle = NULL;

    gtm::order_access_g.rtn_name.address = gtm::gtm_order_g;
    gtm::order_access_g.rtn_name.length = 5;
    gtm::order_access_g.handle = NULL;

    gtm::previous_access_g.rtn_name.address = gtm::gtm_previous_g;
    gtm::previous_access_g.rtn_name.length = 8;
    gtm::previous_access_g.handle = NULL;

    gtm::previous_node_access_g.rtn_name.address = gtm::gtm_previous_node_g;
    gtm::previous_node_access_g.rtn_name.length = 13;
    gtm::previous_node_access_g.handle = NULL;

    gtm::set_access_g.rtn_name.address = gtm::gtm_set_g;
    gtm::set_access_g.rtn_name.length = 3;
    gtm::set_access_g.handle = NULL;
#endif
#endif

    uv_mutex_lock(&mutex_g);
#if YDB_SIMPLE_API == 1
    if (ydb_init() != YDB_OK) {
#else
    if (gtm_init() != EXIT_SUCCESS) {
#endif
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    gtm_state_g = OPEN;

    struct sigaction signal_attr;

    if (signal_sigint_g == true || signal_sigquit_g == true || signal_sigterm_g == true) {
        signal_attr.sa_handler = clean_shutdown;
        signal_attr.sa_flags = 0;

        if (sigfillset(&signal_attr.sa_mask) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot set mask for signal handler")));
            return;
        }
    }

    if (signal_sigint_g == true) {
        if (sigaction(SIGINT, &signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGINT handler")));
            return;
        }
    }

    if (signal_sigquit_g == true) {
        if (sigaction(SIGQUIT, &signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGQUIT handler")));
            return;
        }
    }

    if (signal_sigterm_g == true) {
        if (sigaction(SIGTERM, &signal_attr, NULL) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGTERM handler")));
            return;
        }
    }

    if (debug_g > LOW) {
        gtm_status_t stat_buf;

        uv_mutex_lock(&mutex_g);
#if GTM_CIP_API == 1
        stat_buf = gtm_cip(&gtm::debug_access_g, debug_g);
#else
        stat_buf = gtm_ci(gtm::gtm_debug_g, debug_g);
#endif
        if (stat_buf != EXIT_SUCCESS) {
            gtm_char_t msg_buf[MSG_LEN];
            gtm_zstatus(msg_buf, MSG_LEN);
            uv_mutex_unlock(&mutex_g);

            args.GetReturnValue().Set(error_status(msg_buf, false, false));
            return;
        } else {
            uv_mutex_unlock(&mutex_g);
        }
    }

    Local<Object> result = Object::New(isolate);

    if (mode_g == STRICT) {
        result->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        result->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 1));
        result->Set(String::NewFromUtf8(isolate, "gtm_pid"), to_string_n(isolate, Number::New(isolate, getpid())));
    } else {
        result->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        result->Set(String::NewFromUtf8(isolate, "pid"), Number::New(isolate, getpid()));
    }

    args.GetReturnValue().Set(result);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::open exit" << endl;

    return;
} // @end Gtm::open method

/*
 * @method {public} Gtm::order
 * @summary Return the next global or local node at the same level
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::order(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::order enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    } else {
        glvn = args[0];

        if (args_count > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, subscripts);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::order;
#else
    baton->function = &gtm::order;
#endif
    baton->function_return = &order_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::order(baton);
#else
    baton->status = gtm::order(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK && baton->status != YDB_NODE_END) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into order_return" << "\n";

    Local<Value> return_object = order_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::order exit" << endl;

    return;
} // @end Gtm::order method

/*
 * @method {public} Gtm::previous
 * @summary Return the previous global or local node at the same level
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::previous(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::previous enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    } else {
        glvn = args[0];

        if (args_count > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, subscripts);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::previous;
#else
    baton->function = &gtm::previous;
#endif
    baton->function_return = &previous_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::previous(baton);
#else
    baton->status = gtm::previous(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK && baton->status != YDB_NODE_END) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into previous_return" << "\n";

    Local<Value> return_object = previous_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::previous exit" << endl;

    return;
} // @end Gtm::previous method

/*
 * @method {public} Gtm::previous_node
 * @summary Same as Gtm::next_node, only in reverse
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::previous_node(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::previous_node enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    } else {
        glvn = args[0];

        if (args_count > 1) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub;

    if (utf8_g == true) {
        gvn = *(UTF8_VALUE_TEMP_N(isolate, name));
        sub = *(UTF8_VALUE_TEMP_N(isolate, subs));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << endl;
#endif
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, Undefined(isolate));
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = gvn;
    baton->args = sub;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::previous_node;
#else
    baton->function = &gtm::previous_node;
#endif
    baton->function_return = &previous_node_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::previous_node(baton);
#else
    baton->status = gtm::previous_node(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK && baton->status != YDB_NODE_END) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into previous_node" << "\n";

    Local<Value> return_object = previous_node_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::previous_node exit" << endl;

    return;
} // @end Gtm::previous_node method

/*
 * @method {public} Gtm::procedure
 * @summary Call an arbitrary procedure/subroutine
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::procedure(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::procedure enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> procedure;
    Local<Value> arguments = Undefined(isolate);
    uint32_t relink = auto_relink_g;
    bool local = false;
    bool position = false;
    bool routine = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        procedure = arg_object->Get(String::NewFromUtf8(isolate, "procedure"));

        if (procedure->IsUndefined()) {
            procedure = arg_object->Get(String::NewFromUtf8(isolate, "routine"));

            if (procedure->IsUndefined()) {
                isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                        "Need to supply a 'procedure' or 'routine' property")));
                return;
            } else {
                routine = true;
            }
        }

        arguments = arg_object->Get(String::NewFromUtf8(isolate, "arguments"));

        if (has_n(isolate, arg_object, String::NewFromUtf8(isolate, "autoRelink"))) {
            relink = boolean_value_n(isolate, arg_object->Get(String::NewFromUtf8(isolate, "autoRelink")));
        }
    } else {
        procedure = args[0];

        if (args_count > 1) {
            Local<Array> temp_arguments = Array::New(isolate, args_count - 1);

            for (unsigned int i = 1; i < args_count; i++) {
                temp_arguments->Set(i - 1, args[i]);
            }

            arguments = temp_arguments;
        }

        position = true;
    }

    if (!procedure->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Procedure must be a string")));
        return;
    } else if (procedure->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Procedure must not be an empty string")));
        return;
    }

    Local<Value> arg = Undefined(isolate);

    if (arguments->IsUndefined()) {
        arg = String::Empty(isolate);
    } else if (arguments->IsArray()) {
        arg = encode_arguments(arguments, true);

        if (arg->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Arguments contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'arguments' must be an array")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    Local<Value> name = globalize_name(procedure);

    string proc_s, args_s;

    if (utf8_g == true) {
        proc_s = *(UTF8_VALUE_TEMP_N(isolate, name));
        args_s = *(UTF8_VALUE_TEMP_N(isolate, arg));
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {arg};

        proc_s = gtm_name.to_byte();
        args_s = gtm_args.to_byte();
    }

    if (debug_g > LOW) {
        cout << "DEBUG>> procedure: " << proc_s << "\n";
        cout << "DEBUG>> arguments: " << args_s << endl;
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, arguments);
    baton->data_p.Reset(isolate, Undefined(isolate));
    baton->name = proc_s;
    baton->args = args_s;
    baton->relink = relink;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->routine = routine;
    baton->status = 0;
    baton->function = &gtm::procedure;
    baton->function_return = &procedure_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    if (debug_g > LOW) {
        cout << "DEBUG>> relink: " << relink << "\n";
        cout << "DEBUG>> mode: " << mode_g << "\n";
    }

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

    baton->status = gtm::procedure(baton);

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    if (baton->status != EXIT_SUCCESS) {
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into procedure_return" << "\n";

    Local<Value> return_object = procedure_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::procedure exit" << endl;

    return;
} // @end Gtm::procedure method

/*
 * @method {public} Gtm::retrieve
 * @summary Not yet implemented
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::retrieve(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::retrieve enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    gtm_status_t stat_buf;
    gtm_char_t gtm_retrieve[] = "retrieve";

    static gtm_char_t ret_buf[RET_LEN];

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_retrieve;
    access.rtn_name.length = 8;
    access.handle = NULL;

    uv_mutex_lock(&mutex_g);
    stat_buf = gtm_cip(&access, ret_buf);
#else
    uv_mutex_lock(&mutex_g);
    stat_buf = gtm_ci(gtm_retrieve, ret_buf);
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_lock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::retrieve JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        args.GetReturnValue().Set(try_catch.Exception());
        return;
    } else {
        temp_object = to_object_n(isolate, json);
    }

    args.GetReturnValue().Set(temp_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::retrieve exit" << endl;

    return;
} // @end Gtm::retrieve method

/*
 * @method {public} Gtm::set
 * @summary Set a global or local node, or an intrinsic special variable
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::set(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::set enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    bool async = false;
    unsigned int args_count = args.Length();

    if (args[args_count - 1]->IsFunction()) {
        --args_count;
        async = true;
    }

    if (args_count == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an additional argument")));
        return;
    }

    Local<Value> glvn;
    Local<Value> subscripts = Undefined(isolate);
    Local<Value> data, data_node;
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
        data = arg_object->Get(String::NewFromUtf8(isolate, "data"));
    } else {
        glvn = args[0];
        data = args[args_count - 1];

        if (args_count > 2) {
            Local<Array> temp_subscripts = Array::New(isolate, args_count - 2);

            for (unsigned int i = 1; i < args_count - 1; i++) {
                temp_subscripts->Set(i - 1, args[i]);
            }

            subscripts = temp_subscripts;
        }

        position = true;

        string test = *(UTF8_VALUE_TEMP_N(isolate, glvn));
        if (test[0] != '^') local = true;
    }

    if (!glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        if (local) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Local must not be an empty string")));
        } else {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global must not be an empty string")));
        }

        return;
    }

    Local<Value> subs = Undefined(isolate);
    vector<string> subs_array;

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        bool error = false;
        subs_array = build_subscripts(subscripts, error);

        if (error) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#else
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Subscripts contain invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
#endif
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    if (data->IsUndefined()) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'data' property")));
        return;
    }

    Local<Array> data_array = Array::New(isolate, 1);
    data_array->Set(0, data);

#if YDB_SIMPLE_API == 1
    data_node = data_array;
#else
    data_node = encode_arguments(data_array);
#endif

    if (data_node->IsUndefined()) {
        Local<String> error_message = String::NewFromUtf8(isolate, "Property 'data' contains invalid data");
        isolate->ThrowException(Exception::SyntaxError(error_message));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    string gvn, sub, value;

    if (utf8_g == true) {
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

#if YDB_SIMPLE_API == 1
    if (mode_g == CANONICAL && data->IsNumber()) {
        if (value.substr(0, 2) == "0.") value = value.substr(1, string::npos);
        if (value.substr(0, 3) == "-0.") value = "-" + value.substr(2, string::npos);
    }
#endif

    if (debug_g > LOW) {
        cout << name_msg << gvn << "\n";
#if YDB_SIMPLE_API == 1
        if (subs_array.size()) {
            for (unsigned int i = 0; i < subs_array.size(); i++) {
                cout << "DEBUG>> subscripts[" << i << "]: " << subs_array[i] << "\n";
            }
        }
#else
        cout << "DEBUG>> subscripts: " << sub << "\n";
#endif
        cout << "DEBUG>> data: " << value << endl;
    }

    Baton* baton;

    if (async) {
        baton = new Baton();

        baton->callback_p.Reset(isolate, Local<Function>::Cast(args[args_count]));
    } else {
        static Baton new_baton;
        baton = &new_baton;

        baton->callback_p.Reset();
    }

    baton->request.data = baton;
    baton->arguments_p.Reset(isolate, subscripts);
    baton->data_p.Reset(isolate, data);
    baton->name = gvn;
    baton->args = sub;
    baton->value = value;
    baton->subs_array = subs_array;
    baton->mode = mode_g;
    baton->async = async;
    baton->local = local;
    baton->position = position;
    baton->status = 0;
#if YDB_SIMPLE_API == 1
    baton->function = &ydb::set;
#else
    baton->function = &gtm::set;
#endif
    baton->function_return = &set_return;

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    if (async) {
        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    }

#if YDB_SIMPLE_API == 1
    baton->status = ydb::set(baton);
#else
    baton->status = gtm::set(baton);
#endif

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
    if (baton->status == -1) {
        baton->arguments_p.Reset();
        baton->data_p.Reset();

        char error[BUFSIZ];

        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror_r(errno, error, BUFSIZ))));
        return;
    } else if (baton->status != YDB_OK) {
#else
    if (baton->status != EXIT_SUCCESS) {
#endif
        if (position) {
            isolate->ThrowException(Exception::Error(to_string_n(isolate, error_status(baton->msg_buf, position, async))));
            args.GetReturnValue().Set(Undefined(isolate));
        } else {
            args.GetReturnValue().Set(error_status(baton->msg_buf, position, async));
        }

        baton->arguments_p.Reset();
        baton->data_p.Reset();

        return;
    }

    if (debug_g > LOW) cout << "DEBUG>> call into set_return" << "\n";

    Local<Value> return_object = set_return(baton);

    baton->arguments_p.Reset();
    baton->data_p.Reset();

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "DEBUG> Gtm::set exit" << endl;

    return;
} // @end Gtm::set method

/*
 * @method {public} Gtm::unlock
 * @summary Unlock a global or local node, incrementally, or release all locks
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::unlock(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::unlock enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;

    if (args.Length() > 0 && !args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    } else if (args.Length() > 0) {
        Local<Object> arg_object = to_object_n(isolate, args[0]);
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate,
                    "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
    }

    if (!glvn->IsUndefined() && !glvn->IsString()) {
        if (local) {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Local must be a string")));
        } else {
            isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global must be a string")));
        }

        return;
    } else if (glvn->IsUndefined()) {
        glvn = String::Empty(isolate);
        local = true;
    }

    Local<Value> subs = String::Empty(isolate);

    if (subscripts->IsArray()) {
        subs = encode_arguments(subscripts);

        if (subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Property 'subscripts' contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else if (!subscripts->IsUndefined()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' must be an array")));
        return;
    }

    const char* name_msg;
    Local<Value> name;

    if (local) {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*(UTF8_VALUE_TEMP_N(isolate, name)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*(UTF8_VALUE_TEMP_N(isolate, glvn)))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_unlock[] = "unlock";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_unlock;
    access.rtn_name.length = 6;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << name_msg << *(UTF8_VALUE_TEMP_N(isolate, name)) << "\n";
            cout << "DEBUG>> subscripts: " << *(UTF8_VALUE_TEMP_N(isolate, subs)) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, *(UTF8_VALUE_TEMP_N(isolate, name)), *(UTF8_VALUE_TEMP_N(isolate, subs)), mode_g);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) {
            cout << name_msg << gtm_name.to_byte() << "\n";
            cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_cip(&access, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) {
            cout << name_msg << *String::Utf8Value(name) << "\n";
            cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_unlock, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) {
            cout << name_msg << gtm_name.to_byte() << "\n";
            cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;
        }

        uv_mutex_lock(&mutex_g);
        stat_buf = gtm_ci(gtm_unlock, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    if (name->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        Local<Value> ret_data;

        if (mode_g == STRICT) {
            ret_data = String::NewFromUtf8(isolate, "0");
        } else {
            ret_data = Number::New(isolate, 0);
        }

        args.GetReturnValue().Set(ret_data);
    } else {
        Local<Object> return_object = Object::New(isolate);

        if (mode_g == STRICT) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), glvn);
            }
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
            }
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));

        args.GetReturnValue().Set(return_object);
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::unlock exit" << endl;

    return;
} // @end Gtm::unlock method

/*
 * @method {public} Gtm::update
 * @summary Not yet implemented
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::update(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::update enter" << "\n";

    if (gtm_state_g < OPEN) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, NODEM_DB " database connection is not open")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    gtm_status_t stat_buf;
    gtm_char_t gtm_update[] = "update";

    static gtm_char_t ret_buf[RET_LEN];

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_update;
    access.rtn_name.length = 6;
    access.handle = NULL;

    uv_mutex_lock(&mutex_g);
    stat_buf = gtm_cip(&access, ret_buf);
#else
    uv_mutex_lock(&mutex_g);
    stat_buf = gtm_ci(gtm_update, ret_buf);
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::update JSON string: " << *(UTF8_VALUE_TEMP_N(isolate, json_string)) << "\n";

#if NODE_MAJOR_VERSION >= 1
    TryCatch try_catch(isolate);
#else
    TryCatch try_catch;
#endif

    Local<Object> temp_object;
    Local<Value> json = json_method(json_string, "parse");

    if (try_catch.HasCaught()) {
        args.GetReturnValue().Set(try_catch.Exception());
        return;
    } else {
        temp_object = to_object_n(isolate, json);
    }

    args.GetReturnValue().Set(temp_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::update exit" << endl;

    return;
} // @end Gtm::update method

/*
 * @method {public} Gtm::version
 * @summary Return the about/version string
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::version(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::version enter" << "\n";

    Local<String> nodem_version = String::NewFromUtf8(isolate,
            "Node.js Adaptor for " NODEM_DB ": Version: " NODEM_VERSION " (FWS)");

    if (gtm_state_g < OPEN) {
        args.GetReturnValue().Set(nodem_version);
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    gtm_status_t stat_buf;
    gtm_char_t gtm_version[] = "version";

    static gtm_char_t ret_buf[RET_LEN];

#if GTM_CIP_API == 1
    ci_name_descriptor version;

    version.rtn_name.address = gtm_version;
    version.rtn_name.length = 7;
    version.handle = NULL;

    uv_mutex_lock(&mutex_g);
    stat_buf = gtm_cip(&version, ret_buf);
#else
    uv_mutex_lock(&mutex_g);
    stat_buf = gtm_ci(gtm_version, ret_buf);
#endif

    if (stat_buf != EXIT_SUCCESS) {
        gtm_char_t msg_buf[MSG_LEN];
        gtm_zstatus(msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex_g);

        args.GetReturnValue().Set(error_status(msg_buf, false, false));
        return;
    } else {
        uv_mutex_unlock(&mutex_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> ret_string = String::NewFromUtf8(isolate, ret_buf);
    Local<String> version_string = concat_n(isolate, nodem_version,
      concat_n(isolate, String::NewFromUtf8(isolate, "; "), ret_string));

    args.GetReturnValue().Set(version_string);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::version exit" << endl;

    return;
} // @end Gtm::version & Gtm::about method

/*
 * @method {public} Gtm::New
 * @summary The Gtm class constructor
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::New(const FunctionCallbackInfo<Value>& args)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (args.IsConstructCall()) {
        Gtm* gtm = new Gtm();
        gtm->Wrap(args.This());

        args.GetReturnValue().Set(args.This());
        return;
    } else {
        Local<Function> constructor = Local<Function>::New(isolate, constructor_p);

#if NODE_MAJOR_VERSION == 6 && NODE_MINOR_VERSION >= 8 || NODE_MAJOR_VERSION >= 7
        MaybeLocal<Object> maybe_instance = constructor->NewInstance(isolate->GetCurrentContext());
        Local<Object> instance;

        if (maybe_instance.IsEmpty()) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Unable to instantiate the Gtm class")));
        } else {
            instance = maybe_instance.ToLocalChecked();
            args.GetReturnValue().Set(instance);
        }
#else
        args.GetReturnValue().Set(constructor->NewInstance());
#endif
        return;
    }
} // @end Gtm::New method

/*
 * @method {public} Gtm::Init
 * @summary Set the exports property when mumps.node is required
 * @param {Local<Object>} exports - A special object passed by the Node.js runtime
 * @returns void
 */
void Gtm::Init(Local<Object> exports)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    Local<FunctionTemplate> func_template = FunctionTemplate::New(isolate, Gtm::New);

    func_template->SetClassName(String::NewFromUtf8(isolate, "Gtm"));
    func_template->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(func_template, "about", version);
    NODE_SET_PROTOTYPE_METHOD(func_template, "close", close);
    NODE_SET_PROTOTYPE_METHOD(func_template, "data", data);
    NODE_SET_PROTOTYPE_METHOD(func_template, "function", function);
    NODE_SET_PROTOTYPE_METHOD(func_template, "get", get);
    NODE_SET_PROTOTYPE_METHOD(func_template, "globalDirectory", global_directory);
    NODE_SET_PROTOTYPE_METHOD(func_template, "global_directory", global_directory);
    NODE_SET_PROTOTYPE_METHOD(func_template, "help", help);
    NODE_SET_PROTOTYPE_METHOD(func_template, "increment", increment);
    NODE_SET_PROTOTYPE_METHOD(func_template, "kill", kill);
    NODE_SET_PROTOTYPE_METHOD(func_template, "localDirectory", local_directory);
    NODE_SET_PROTOTYPE_METHOD(func_template, "local_directory", local_directory);
    NODE_SET_PROTOTYPE_METHOD(func_template, "lock", lock);
    NODE_SET_PROTOTYPE_METHOD(func_template, "merge", merge);
    NODE_SET_PROTOTYPE_METHOD(func_template, "next", order);
    NODE_SET_PROTOTYPE_METHOD(func_template, "nextNode", next_node);
    NODE_SET_PROTOTYPE_METHOD(func_template, "next_node", next_node);
    NODE_SET_PROTOTYPE_METHOD(func_template, "open", open);
    NODE_SET_PROTOTYPE_METHOD(func_template, "order", order);
    NODE_SET_PROTOTYPE_METHOD(func_template, "previous", previous);
    NODE_SET_PROTOTYPE_METHOD(func_template, "previousNode", previous_node);
    NODE_SET_PROTOTYPE_METHOD(func_template, "previous_node", previous_node);
    NODE_SET_PROTOTYPE_METHOD(func_template, "procedure", procedure);
    NODE_SET_PROTOTYPE_METHOD(func_template, "retrieve", retrieve);
    NODE_SET_PROTOTYPE_METHOD(func_template, "routine", procedure);
    NODE_SET_PROTOTYPE_METHOD(func_template, "set", set);
    NODE_SET_PROTOTYPE_METHOD(func_template, "unlock", unlock);
    NODE_SET_PROTOTYPE_METHOD(func_template, "update", update);
    NODE_SET_PROTOTYPE_METHOD(func_template, "version", version);

    constructor_p.Reset(isolate, get_function_n(isolate, func_template));
    Local<Function> constructor = Local<Function>::New(isolate, constructor_p);

    exports->Set(String::NewFromUtf8(isolate, "Gtm"), constructor);
    if (strncmp(NODEM_DB, "YottaDB", 7) == 0) exports->Set(String::NewFromUtf8(isolate, "Ydb"), constructor);
} // @end Gtm::Init method

/*
 * @MACRO {public} NODE_MODULE
 * @summary Register the mumps.node module with Node.js
 */
NODE_MODULE(mumps, Gtm::Init)

} // @end nodem namespace
