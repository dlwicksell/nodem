/*
 * Package:    NodeM
 * File:       mumps.cc
 * Summary:    A YottaDB/GT.M database driver and binding for Node.js
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2012-2018 Fourth Watch Software LC
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

#include "mumps.h"
#include "gtm.h"
#include "ydb.h"

namespace nodem {

static struct termios term_attr_g;
static struct sigaction signal_attr_g;

static enum {CLOSED, NOT_OPEN, OPEN} gtm_state_g = NOT_OPEN;
static enum mode_t {STRICT, CANONICAL} mode_g = CANONICAL;

static bool reset_term_g = false;
static bool auto_relink_g = false;
static bool utf8_g = true;

debug_t debug_g = OFF;
uv_mutex_t mutex;

gtm_char_t msg_buffer_g[MSG_LEN];
gtm_char_t ret_buffer_g[RET_LEN];

using namespace v8;
using std::string;
using std::vector;
using std::cerr;
using std::cout;
using std::endl;

Persistent<Function> Gtm::constructor_p;

/*
 * @function {public} catch_interrupt
 * @summary Handle a SIGINT/SIGQUIT/SIGTERM signal, by cleaning up everything, and exiting Node.js
 * @param {int} signal_num - The number of the caught signal
 * @returns void
 */
inline void catch_interrupt(const int signal_num)
{
    if (debug_g > OFF) cerr << "\nDEBUG> catch_interrupt enter" << "\n";
    if (debug_g > OFF) cerr << "DEBUG> signal_num: " << signal_num << "\n";

    uv_mutex_lock(&mutex);
#if YDB_SIMPLE_API == 1
    if (ydb_exit() != YDB_OK) {
#else
    if (gtm_exit() != EXIT_SUCCESS) {
#endif
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        cerr << msg_buffer_g << endl;
        exit(EXIT_FAILURE);
    }
    uv_mutex_unlock(&mutex);

    if (reset_term_g == true) {
        term_attr_g.c_iflag |= ICRNL;
        term_attr_g.c_lflag |= (ICANON | ECHO);
    }

    if (isatty(STDIN_FILENO)) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &term_attr_g) == -1) {
            cerr << strerror(errno) << endl;
            exit(errno);
        }
    } else if (isatty(STDOUT_FILENO)) {
        if (tcsetattr(STDOUT_FILENO, TCSANOW, &term_attr_g) == -1) {
            cerr << strerror(errno) << endl;
            exit(errno);
        }
    } else if (isatty(STDERR_FILENO)) {
        if (tcsetattr(STDERR_FILENO, TCSANOW, &term_attr_g) == -1) {
            cerr << strerror(errno) << endl;
            exit(errno);
        }
    }

    if (debug_g > OFF) {
        if (signal_num == 2) {
            cerr << "DEBUG> handled SIGINT gracefully" << "\n";
        } else if (signal_num == 3) {
            cerr << "DEBUG> handled SIGQUIT gracefully" << "\n";
        } else if (signal_num == 15) {
            cerr << "DEBUG> handled SIGTERM gracefully" << "\n";
        }

        cerr << "DEBUG> catch_interrupt exit" << endl;
    }

    exit(signal_num);
} // @end catch_interrupt function

/*
 * @function {public} gtm_status
 * @summary Handle an error from the YottaDB/GT.M runtime
 * @param {gtm_char_t*} msg_buffer_g - A character string representing the YottaDB/GT.M runtime error
 * @param {bool} position - Whether the API was called by positional arguments or not
 * @param {bool} async - Whether the API was called asynchronously or not
 * @returns {Local<Value>} result - An object containing the formatted error content
 */
inline Local<Value> gtm_status(gtm_char_t *msg_buffer_g, const bool position, const bool async)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) cout << "\nDEBUG>>> gtm_status enter" << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> msg_buffer_g: " << msg_buffer_g << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> position: " << position << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> async: " << async << "\n";

    char *error_msg;
    char *code = strtok_r(msg_buffer_g, ",", &error_msg);

    unsigned int error_code = atoi(code);

    if (strstr(error_msg, "%YDB-E-CTRAP") != NULL || strstr(error_msg, "%GTM-E-CTRAP") != NULL) {
        catch_interrupt(SIGINT); // Handle SIGINT caught by YottaDB or GT.M
    }

    Local<Object> result = Object::New(isolate);

    if (position && !async) {
        return scope.Escape(String::NewFromUtf8(isolate, error_msg));
    } else if (mode_g == STRICT) {
        result->Set(String::NewFromUtf8(isolate, "ErrorMessage"), String::NewFromUtf8(isolate, error_msg));
        result->Set(String::NewFromUtf8(isolate, "ErrorCode"), Number::New(isolate, error_code));
        result->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 0));
    } else {
        result->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, false));
        result->Set(String::NewFromUtf8(isolate, "errorCode"), Number::New(isolate, error_code));
        result->Set(String::NewFromUtf8(isolate, "errorMessage"), String::NewFromUtf8(isolate, error_msg));
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> gtm_status exit" << "\n";

    return scope.Escape(result);
} // @end gtm_status function

/*
 * @function {private} invalid_local
 * @summary If a local name starts with v4w, it is not valid, and cannot be manipulated
 * @param {char*} name - The name to test against
 * @returns {bool} - Whether the local name is invalid
 */
inline static bool invalid_local(const char *name)
{
    if (debug_g > MEDIUM) cout << "\nDEBUG>>> invalid_local enter" << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> name: " << name << "\n";

    if (strncmp(name, "v4w", 3) == 0) {
        if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_local exit: " << true << "\n";
        return true;
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_local exit: " << false << "\n";

    return false;
} // @end invalid_local function

/*
 * @function {private} invalid_name
 * @summary If a name contains subscripts, it is not valid, and cannot be used
 * @param {char*} name - The name to test against
 * @returns {bool} - Whether the name is invalid
 */
inline static bool invalid_name(const char *name)
{
    if (debug_g > MEDIUM) cout << "\nDEBUG>>> invalid_name enter" << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> name: " << name << "\n";

    if (strchr(name, '(') != NULL || strchr(name, ')') != NULL) {
        if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_name exit: " << true << "\n";
        return true;
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> invalid_name exit: " << false << "\n";

    return false;
} // @end invalid_name function

/*
 * @function {private} globalize_name
 * @summary If a name doesn't start with the optional '^' character, add it for output
 * @param {Local<Value>} name - The name to be normalized for output
 * @returns {Local<Value>} [new_name|name] - A string containing the normalized name
 */
inline static Local<Value> globalize_name(const Local<Value> name)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) cout << "\nDEBUG>>> globalize_name enter" << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> name: " << *String::Utf8Value(name) << "\n";

    String::Utf8Value data_string(name);

    const gtm_char_t *data_name = *data_string;
    const gtm_char_t *char_ptr = strchr(data_name, '^');

    if (char_ptr == NULL) {
        Local<Value> new_name = String::Concat(String::NewFromUtf8(isolate, "^"), name->ToString());

        if (debug_g > MEDIUM) cout << "DEBUG>>> globalize_name exit: " << *String::Utf8Value(new_name) << "\n";

        return scope.Escape(new_name);
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> globalize_name exit: " << *String::Utf8Value(name) << "\n";

    return scope.Escape(name);
} // @end globalize_name function

/*
 * @function {private} localize_name
 * @summary If a name starts with the optional '^' character, strip it off for output
 * @param {Local<Value>} name - The name to be normalized for output
 * @returns {Local<Value>} [data_name|name] - A string containing the normalized name
 */
inline static Local<Value> localize_name(const Local<Value> name)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) cout << "\nDEBUG>>> localize_name enter" << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> name: " << *String::Utf8Value(name) << "\n";

    String::Utf8Value data_string(name);

    const gtm_char_t *data_name = *data_string;
    const gtm_char_t *char_ptr = strchr(data_name, '^');

    if (char_ptr != NULL && char_ptr - data_name == 0) {
        if (debug_g > MEDIUM) cout << "DEBUG>>> localize_name exit: " << &data_name[1] << "\n";

        return scope.Escape(String::NewFromUtf8(isolate, &data_name[1]));
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> localize_name exit: " << *String::Utf8Value(name) << "\n";

    return scope.Escape(name);
} // @end localize_name function

/*
 * @function {private} json_method
 * @summary Call a method on the built-in Node.js JSON object
 * @param {Local<Value>} data - A JSON string containing the data to parse or a JavaScript object to stringify
 * @param {string} type - The name of the method to call on JSON
 * @returns {Local<Value>} An object containing the output data
 */
inline static Local<Value> json_method(Local<Value> data, const string type)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) cout << "\nDEBUG>>> json_method enter" << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> data: " << *String::Utf8Value(data) << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> type: " << type << "\n";

    Local<Object> global = isolate->GetCurrentContext()->Global();
    Local<Object> JSON = global->Get(String::NewFromUtf8(isolate, "JSON"))->ToObject();
    Local<Function> JSON_method = Local<Function>::Cast(JSON->Get(String::NewFromUtf8(isolate, type.c_str())));

    if (debug_g > MEDIUM) cout << "DEBUG>>> json_method exit" << "\n";

    return scope.Escape(JSON_method->Call(JSON, 1, &data));
} // @end json_method function

/*
 * @function {private} encode_arguments
 * @summary Encode an array of arguments for parsing in v4wNode.m
 * @param {Local<Value>} arguments - The array of subscripts or arguments to be encoded
 * @returns {Local<Value>} [Undefined|encoded_array] - The encoded array of subscripts or arguments, or Undefined if it has bad data
 */
inline static Local<Value> encode_arguments(const Local<Value> arguments)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > MEDIUM) cout << "\nDEBUG>>> encode_arguments enter" << "\n";
    if (debug_g > MEDIUM) cout << "DEBUG>>> arguments: " << *String::Utf8Value(arguments) << "\n";

    Local<Array> argument_array = Local<Array>::Cast(arguments);
    Local<Array> encoded_array = Array::New(isolate);

    for (unsigned int i = 0; i < argument_array->Length(); i++) {
        Local<Value> data_test = argument_array->Get(i);
        Local<String> data = data_test->ToString();
        Local<String> colon = String::NewFromUtf8(isolate, ":");
        Local<String> length;
        Local<Value> new_data = Undefined(isolate);

        if (data_test->IsUndefined()) {
            new_data = String::NewFromUtf8(isolate, "0:");
        } else if (data_test->IsSymbol() || data_test->IsSymbolObject()) {
            return Undefined(isolate);
        } else if (data_test->IsNumber()) {
            length = Number::New(isolate, data->Length())->ToString();
            new_data = String::Concat(length, String::Concat(colon, data));
        } else if (data_test->IsObject()) {
            Local<Object> object = data_test->ToObject();
            Local<Value> type = object->Get(String::NewFromUtf8(isolate, "type"));
            Local<Value> value_test = object->Get(String::NewFromUtf8(isolate, "value"));
            Local<String> value = value_test->ToString();

            if (value_test->IsSymbol() || value_test->IsSymbolObject()) {
                return Undefined(isolate);
            } else if (type->StrictEquals(String::NewFromUtf8(isolate, "reference"))) {
                if (!value_test->IsString()) return Undefined(isolate);
                if (invalid_local(*String::Utf8Value(value))) return Undefined(isolate);
                if (invalid_name(*String::Utf8Value(value))) return Undefined(isolate);

                Local<String> new_value = localize_name(value)->ToString();
                Local<String> dot = String::NewFromUtf8(isolate, ".");
                length = Number::New(isolate, new_value->Length() + 1)->ToString();
                new_data = String::Concat(length, String::Concat(colon, String::Concat(dot, new_value)));
            } else if (type->StrictEquals(String::NewFromUtf8(isolate, "variable"))) {
                if (!value_test->IsString()) return Undefined(isolate);
                if (invalid_local(*String::Utf8Value(value))) return Undefined(isolate);
                if (invalid_name(*String::Utf8Value(value))) return Undefined(isolate);

                Local<String> new_value = localize_name(value)->ToString();
                length = Number::New(isolate, new_value->Length())->ToString();
                new_data = String::Concat(length, String::Concat(colon, new_value));
            } else if (type->StrictEquals(String::NewFromUtf8(isolate, "value"))) {
                if (value_test->IsUndefined()) {
                    new_data = String::NewFromUtf8(isolate, "0:");
                } else if (value_test->IsSymbol() || value_test->IsSymbolObject()) {
                    return Undefined(isolate);
                } else if (value_test->IsNumber()) {
                    length = Number::New(isolate, value->Length())->ToString();
                    new_data = String::Concat(length, String::Concat(colon, value));
                } else {
                    length = Number::New(isolate, value->Length() + 2)->ToString();
                    Local<String> quote = String::NewFromUtf8(isolate, "\"");
                    new_data = String::Concat(String::Concat(length, String::Concat(colon, quote)), String::Concat(value, quote));
                }
            } else {
                length = Number::New(isolate, data->Length() + 2)->ToString();
                Local<String> quote = String::NewFromUtf8(isolate, "\"");
                new_data = String::Concat(String::Concat(length, String::Concat(colon, quote)), String::Concat(data, quote));
            }
        } else {
            length = Number::New(isolate, data->Length() + 2)->ToString();
            Local<String> quote = String::NewFromUtf8(isolate, "\"");
            new_data = String::Concat(String::Concat(length, String::Concat(colon, quote)), String::Concat(data, quote));
        }

        encoded_array->Set(i, new_data);
    }

    if (debug_g > MEDIUM) cout << "DEBUG>>> encode_arguments exit" << "\n";

    return scope.Escape(encoded_array);
} // @end encode_arguments function

/*
 * @class GtmValue
 * @method {instance} to_byte
 * @summary Convert a UTF-8 encoded buffer to a byte encoded buffer
 * @returns {gtm_char_t*} A byte encoded buffer
 */
gtm_char_t* GtmValue::to_byte(void)
{
    value->WriteOneByte(buffer, 0, size);
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
    const uint8_t* byte_buffer = reinterpret_cast<const uint8_t*>(buffer);
    MaybeLocal<String> maybe_string = String::NewFromOneByte(isolate, byte_buffer, NewStringType::kNormal);

    if (maybe_string.IsEmpty()) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Unable to convert from a byte buffer")));
        return String::Empty(isolate);
    } else {
        return maybe_string.ToLocalChecked();
    }
#else
    return String::NewFromOneByte(isolate, reinterpret_cast<const uint8_t*>(buffer));
#endif
}

/*
 * @function {private} get_ret
 * @summary Return data from a global or local node, or an intrinsic special variable
 * @param {gtm_status_t} status - Return code; 0 is success, 1 is undefined node
 * @param {gtm_char_t} ret_buf - Data returned from get call
 * @param {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @param {bool} local - Whether the API was called on a local variable, or a global variable
 * @param {bool} async - Whether the API was called asynchronously, or synchronously
 * @param {string} glvn - The name of the global or local variable
 * @param {Local<Value>} subscripts - V8 object containing the subscripts that were called
 * @returns {Local<Value>} {scope escape} return_object - Data returned to Node.js
 */
static Local<Value> get_ret(gtm_status_t status, gtm_char_t ret_buf[], bool position, bool local, bool async, string glvn, Local<Value> subscripts)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> get_ret enter" << "\n";
    if (debug_g > LOW) cout << "DEBUG>> status: " << status << "\n";
    if (debug_g > LOW) cout << "DEBUG>> ret_buf: " << ret_buf << "\n";
    if (debug_g > LOW) cout << "DEBUG>> position: " << position << "\n";
    if (debug_g > LOW) cout << "DEBUG>> local: " << local << "\n";
    if (debug_g > LOW) cout << "DEBUG>> async: " << async << "\n";
    if (debug_g > LOW) cout << "DEBUG>> glvn: " << glvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subscripts) << "\n";

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);

    if (status == YDB_ERR_GVUNDEF || status == YDB_ERR_LVUNDEF) {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 0));
    } else {
        temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 1));
    }

    string test(ret_buf);
    bool flag = false;
    size_t neg_cnt = count(test.begin(), test.end(), '-');
    size_t decp_cnt = count(test.begin(), test.end(), '.');

    if ((decp_cnt == 0 || decp_cnt == 1) && (neg_cnt == 0 || (neg_cnt == 1 && test[0] == '-'))) flag = true;
    if ((decp_cnt == 1 || neg_cnt == 1) && test.length() <= 1) flag = false;

    if (mode_g && flag && !test.empty() && all_of(test.begin(), test.end(), [](char c) {return (std::isdigit(c) || c == '-' || c == '.');})) {
        temp_object->Set(String::NewFromUtf8(isolate, "data"), Number::New(isolate, atof(ret_buf)));
    } else {
        if (utf8_g == true) {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), String::NewFromUtf8(isolate, ret_buf));
        } else {
            temp_object->Set(String::NewFromUtf8(isolate, "data"), GtmValue::from_byte(ret_buf));
        }
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> get_ret JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, glvn.c_str());

    if (position) {
        if (debug_g > OFF) cout << "\nDEBUG> get_ret exit" << "\n";

        if (async) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "data")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "data")));
        }
    } else if (mode_g == STRICT) {
        if (async) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

            return_object->Set(String::NewFromUtf8(isolate, "data"), temp_object->Get(String::NewFromUtf8(isolate, "data")));
            return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
        } else {
            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
            return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
            return_object->Set(String::NewFromUtf8(isolate, "data"), temp_object->Get(String::NewFromUtf8(isolate, "data")));
        }
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "data"), temp_object->Get(String::NewFromUtf8(isolate, "data")));
        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    if (debug_g > OFF) cout << "\nDEBUG> get_ret exit" << "\n";

    return scope.Escape(return_object);
} // @end get_ret function

/*
 * @function {private} kill_ret
 * @summary Return data about removing a global or global node, or a local or local node, or the entire local symbol table
 * @param {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @param {bool} local - Whether the API was called on a local variable, or a global variable
 * @param {bool} async - Whether the API was called asynchronously, or synchronously
 * @param {string} glvn - The name of the global or local variable
 * @param {Local<Value>} subscripts - V8 object containing the subscripts that were called
 * @returns {Local<Value>} {scope escape} return_object - Data returned to Node.js
 */
static Local<Value> kill_ret(bool position, bool local, bool async, string glvn, Local<Value> subscripts)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> kill_ret enter" << "\n";
    if (debug_g > LOW) cout << "DEBUG>> position: " << position << "\n";
    if (debug_g > LOW) cout << "DEBUG>> local: " << local << "\n";
    if (debug_g > LOW) cout << "DEBUG>> async: " << async << "\n";
    if (debug_g > LOW) cout << "DEBUG>> glvn: " << glvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subscripts) << "\n";

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, glvn.c_str());

    if (name->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        Local<Value> ret_data = Undefined(isolate);

        if (mode_g == STRICT) {
            ret_data = String::NewFromUtf8(isolate, "0");
        } else {
            ret_data = Number::New(isolate, 0);
        }

        return scope.Escape(ret_data);
    } else if (position) {
        if (debug_g > OFF) cout << "\nDEBUG> kill_ret exit" << "\n";

        if (async) {
            if (mode_g == STRICT) {
                return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "0"));
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
            }

            return scope.Escape(return_object);
        } else {
            return scope.Escape(Number::New(isolate, 0));
        }
    } else if (mode_g == STRICT) {
        if (async) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

            return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "0"));
        } else {
            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
            return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
        }
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
    }

    if (debug_g > OFF) cout << "\nDEBUG> kill_ret exit" << "\n";

    return scope.Escape(return_object);
} // @end kill_ret function

/*
 * @function {private} order_ret
 * @summary Return data about the next global or local node at the same level
 * @param {gtm_char_t} ret_buf - Data returned from get call
 * @param {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @param {bool} local - Whether the API was called on a local variable, or a global variable
 * @param {bool} async - Whether the API was called asynchronously, or synchronously
 * @param {string} glvn - The name of the global or local variable
 * @param {Local<Value>} subscripts - V8 object containing the subscripts that were called
 * @returns {Local<Value>} {scope escape} return_object - Data returned to Node.js
 */
static Local<Value> order_ret(gtm_char_t ret_buf[], bool position, bool local, bool async, string glvn, Local<Value> subscripts)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> order_ret enter" << "\n";
    if (debug_g > LOW) cout << "DEBUG>> ret_buf: " << ret_buf << "\n";
    if (debug_g > LOW) cout << "DEBUG>> position: " << position << "\n";
    if (debug_g > LOW) cout << "DEBUG>> local: " << local << "\n";
    if (debug_g > LOW) cout << "DEBUG>> async: " << async << "\n";
    if (debug_g > LOW) cout << "DEBUG>> glvn: " << glvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subscripts) << "\n";

#if YDB_SIMPLE_API == 1
    Local<Object> temp_object = Object::New(isolate);
    temp_object->Set(String::NewFromUtf8(isolate, "defined"), Number::New(isolate, 1));

    string test(ret_buf);
    bool flag = false;
    size_t neg_cnt = count(test.begin(), test.end(), '-');
    size_t decp_cnt = count(test.begin(), test.end(), '.');

    if ((decp_cnt == 0 || decp_cnt == 1) && (neg_cnt == 0 || (neg_cnt == 1 && test[0] == '-'))) flag = true;
    if ((decp_cnt == 1 || neg_cnt == 1) && test.length() <= 1) flag = false;

    if (mode_g && flag && !test.empty() && all_of(test.begin(), test.end(), [](char c) {return (std::isdigit(c) || c == '-' || c == '.');})) {
        temp_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, atof(ret_buf)));
    } else {
        if (utf8_g == true) {
            temp_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, ret_buf));
        } else {
            temp_object->Set(String::NewFromUtf8(isolate, "result"), GtmValue::from_byte(ret_buf));
        }
    }
#else
    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buf);
    } else {
        json_string = GtmValue::from_byte(ret_buf);
    }

    if (debug_g > OFF) cout << "DEBUG> order_ret JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }
#endif

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, glvn.c_str());

    if (position) {
        if (debug_g > OFF) cout << "\nDEBUG> order_ret exit" << "\n";

        if (async) {
            return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "result")));

            return scope.Escape(return_object);
        } else {
            return scope.Escape(temp_object->Get(String::NewFromUtf8(isolate, "result")));
        }
    } else if (mode_g == STRICT) {
        if (async) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            Local<Value> result = temp_object->Get(String::NewFromUtf8(isolate, "result"));

            if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
                Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

                new_subscripts->Set(Number::New(isolate, new_subscripts->Length() -1), result);
                return_object->Set(String::NewFromUtf8(isolate, "subscripts"), new_subscripts);
            }

            return_object->Set(String::NewFromUtf8(isolate, "result"), result);
        } else {
            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            Local<Value> result = temp_object->Get(String::NewFromUtf8(isolate, "result"));

            if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
                Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

                new_subscripts->Set(Number::New(isolate, new_subscripts->Length() -1), result);
                return_object->Set(String::NewFromUtf8(isolate, "subscripts"), new_subscripts);
            }

            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
            return_object->Set(String::NewFromUtf8(isolate, "result"), result);
        }
    } else {
        Local<Value> result = temp_object->Get(String::NewFromUtf8(isolate, "result"));

        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            new_subscripts->Set(Number::New(isolate, new_subscripts->Length() -1), result);
            return_object->Set(String::NewFromUtf8(isolate, "subscripts"), new_subscripts);
        }

        return_object->Set(String::NewFromUtf8(isolate, "result"), result);
    }

    if (debug_g > OFF) cout << "\nDEBUG> order_ret exit" << "\n";

    return scope.Escape(return_object);
} // @end order_ret function

/*
 * @function {private} set_ret
 * @summary Return data about the store of a global or local node, or an intrinsic special variable
 * @param {bool} position - Whether the API was called by position, or with a specially-formatted JavaScript object
 * @param {bool} local - Whether the API was called on a local variable, or a global variable
 * @param {bool} async - Whether the API was called asynchronously, or synchronously
 * @param {string} glvn - The name of the global or local variable
 * @param {Local<Value>} subscripts - V8 object containing the subscripts that were called
 * @param {Local<Value>} data - V8 object containing the data to store in the node that was called
 * @returns {Local<Value>} {scope escape} return_object - Data returned to Node.js
 */
static Local<Value> set_ret(bool position, bool local, bool async, string glvn, Local<Value> subscripts, Local<Value> data)
{
    Isolate* isolate = Isolate::GetCurrent();
    EscapableHandleScope scope(isolate);

    if (debug_g > OFF) cout << "\nDEBUG> set_ret enter" << "\n";
    if (debug_g > LOW) cout << "DEBUG>> position: " << position << "\n";
    if (debug_g > LOW) cout << "DEBUG>> local: " << local << "\n";
    if (debug_g > LOW) cout << "DEBUG>> async: " << async << "\n";
    if (debug_g > LOW) cout << "DEBUG>> glvn: " << glvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subscripts) << "\n";
    if (debug_g > LOW) cout << "DEBUG>> data: " << *String::Utf8Value(data) << "\n";

    Local<Object> return_object = Object::New(isolate);
    Local<String> name = String::NewFromUtf8(isolate, glvn.c_str());

    if (position) {
        if (debug_g > OFF) cout << "\nDEBUG> set_ret exit" << "\n";

        if (async) {
            if (mode_g == STRICT) {
                return_object->Set(String::NewFromUtf8(isolate, "result"), String::NewFromUtf8(isolate, "0"));
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
            }

            return scope.Escape(return_object);
        } else {
            return scope.Escape(Number::New(isolate, 0));
        }
    } else if (mode_g == STRICT) {
        if (async) {
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

            return_object->Set(String::NewFromUtf8(isolate, "data"), data);
            return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
        } else {
            if (local) {
                return_object->Set(String::NewFromUtf8(isolate, "local"), name);
            } else {
                return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
            }

            if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

            return_object->Set(String::NewFromUtf8(isolate, "data"), data);
            return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
            return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
        }
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(name));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "data"), data);
        return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
    }

    if (debug_g > OFF) cout << "\nDEBUG> set_ret exit" << "\n";

    return scope.Escape(return_object);
} // @end set_ret function

/*
 * @function {public} async_work
 * @summary Call in to YottaDB/GT.M asynchronously, via a Node.js worker thread
 * @param {uv_work_t *} request - A pointer to the Baton structure for transferring data between the main thread and worker thread
 * @returns void
 */
void async_work(uv_work_t *request)
{
    if (debug_g > LOW) cout << "\nDEBUG>> async_work enter" << "\n";

    Baton* baton = static_cast<Baton*>(request->data);

    if (baton->api == "get") {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into get in the SimpleAPI interface" << "\n";

        baton->status = ydb::get(baton->ret_buf, baton->glvn, baton->subsarray);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into get in the Call-in interface" << "\n";

        baton->status = gtm::get(baton->ret_buf, baton->glvn, baton->subs, baton->mode);
#endif
    } else if (baton->api == "kill") {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into kill in the SimpleAPI interface" << "\n";

        baton->status = ydb::kill(baton->glvn, baton->subsarray);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into kill in the Call-in interface" << "\n";

        baton->status = gtm::kill(baton->glvn, baton->subs, baton->mode);
#endif
    } else if (baton->api == "order") {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into order in the SimpleAPI interface" << "\n";

        baton->status = ydb::order(baton->ret_buf, baton->glvn, baton->subsarray);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into order in the Call-in interface" << "\n";

        baton->status = gtm::order(baton->ret_buf, baton->glvn, baton->subs, baton->mode);
#endif
    } else if (baton->api == "set") {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into set in the SimpleAPI interface" << "\n";

        baton->status = ydb::set(baton->glvn, baton->subsarray, baton->value);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into set in the Call-in interface" << "\n";

        baton->status = gtm::set(baton->glvn, baton->subs, baton->value, baton->mode);
#endif
    }

    if (debug_g > LOW) cout << "DEBUG>> async_work exit" << "\n";

    return;
} // @end async_work function

/*
 * @function {public} async_after
 * @summary Call in to the return functions, passing the data from YottaDB/GT.M, after receiving the data from the worker thread
 * @param {uv_work_t *} request - A pointer to the Baton structure for transferring data between the main thread and worker thread
 * @returns void
 */
void async_after(uv_work_t *request, int status)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    if (debug_g > LOW) cout << "\nDEBUG>> async_after enter: " << status << "\n";

    Baton* baton = static_cast<Baton*>(request->data);

    Local<Value> error_code = Null(isolate);
    Local<Value> return_object = Object::New(isolate);
    Local<Value> error_object = Object::New(isolate);
    Local<Value> subscripts = Local<Value>::New(isolate, baton->subscripts_p);

    if (baton->status == -1) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
        return;
    } else if (baton->status != EXIT_SUCCESS) {
        if (debug_g > LOW) cout << "DEBUG>> " << NODEM_DB << " error code: " << baton->status << "\n";

        uv_mutex_lock(&mutex);
        gtm_zstatus(baton->msg_buf, MSG_LEN);
        uv_mutex_unlock(&mutex);

        if (mode_g == STRICT) {
            error_code = Number::New(isolate, 1);

            return_object = gtm_status(baton->msg_buf, baton->position, baton->async);
        } else {
            error_object = gtm_status(baton->msg_buf, baton->position, baton->async);

            error_code = Exception::Error(String::NewFromUtf8(isolate, *String::Utf8Value(((Object*)*error_object)->Get(String::NewFromUtf8(isolate, "errorMessage")))));
            ((Object*)*error_code)->Set(String::NewFromUtf8(isolate, "ok"), ((Object*)*error_object)->Get(String::NewFromUtf8(isolate, "ok")));
            ((Object*)*error_code)->Set(String::NewFromUtf8(isolate, "errorCode"), ((Object*)*error_object)->Get(String::NewFromUtf8(isolate, "errorCode")));
            ((Object*)*error_code)->Set(String::NewFromUtf8(isolate, "errorMessage"), ((Object*)*error_object)->Get(String::NewFromUtf8(isolate, "errorMessage")));

            return_object = Undefined(isolate);
        }
    } else {
        if (baton->api == "get") {
            if (debug_g > LOW) cout << "DEBUG>> call into get_ret" << "\n";

            return_object = get_ret(baton->status, baton->ret_buf, baton->position, baton->local, baton->async, baton->glvn, subscripts);
        } else if (baton->api == "kill") {
            if (debug_g > LOW) cout << "DEBUG>> call into kill_ret" << "\n";

            return_object = kill_ret(baton->position, baton->local, baton->async, baton->glvn, subscripts);
        } else if (baton->api == "order") {
            if (debug_g > LOW) cout << "DEBUG>> call into order_ret" << "\n";

            return_object = order_ret(baton->ret_buf, baton->position, baton->local, baton->async, baton->glvn, subscripts);
        } else if (baton->api == "set") {
            if (debug_g > LOW) cout << "DEBUG>> call into set_ret" << "\n";

            Local<Value> data = Local<Value>::New(isolate, baton->data_p);

            return_object = set_ret(baton->position, baton->local, baton->async, baton->glvn, subscripts, data);
        }

        if (mode_g == STRICT) error_code = Number::New(isolate, 0);
    }

    Local<Value> argv[2] = {error_code, return_object};
    Local<Function>::New(isolate, baton->callback_p)->Call(Null(isolate), 2, argv);

    baton->callback_p.Reset();
    baton->subscripts_p.Reset();
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

    if (args[0]->IsObject() && args[0]->ToObject()->Has(String::NewFromUtf8(isolate, "resetTerminal"))) {
        reset_term_g = args[0]->ToObject()->Get(String::NewFromUtf8(isolate, "resetTerminal"))->BooleanValue();
    }

    if (debug_g > LOW) cout << "DEBUG>> resetTerminal: " << reset_term_g << "\n";

    uv_mutex_lock(&mutex);
#if YDB_SIMPLE_API == 1
    if (ydb_exit() != YDB_OK) {
#else
    if (gtm_exit() != EXIT_SUCCESS) {
#endif
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }
    uv_mutex_unlock(&mutex);

    gtm_state_g = CLOSED;

    if (reset_term_g == true) {
        term_attr_g.c_iflag |= ICRNL;
        term_attr_g.c_lflag |= (ISIG | ECHO);
    }

    if (isatty(STDIN_FILENO)) {
        if (tcsetattr(STDIN_FILENO, TCSANOW, &term_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        }
    } else if (isatty(STDOUT_FILENO)) {
        if (tcsetattr(STDOUT_FILENO, TCSANOW, &term_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        }
    } else if (isatty(STDERR_FILENO)) {
        if (tcsetattr(STDERR_FILENO, TCSANOW, &term_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        }
    }

    if (mode_g == STRICT) {
        args.GetReturnValue().Set(String::NewFromUtf8(isolate, "1"));
    } else {
        args.GetReturnValue().Set(Boolean::New(isolate, true));
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

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));
    bool local = false;

    if (glvn->IsUndefined()) {
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_data[] = "data";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_data;
    access.rtn_name.length = 4;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_data, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_data, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::data JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), glvn);
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
        }

        if (!subscripts->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), subscripts);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::data exit" << endl;
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

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> function = arg_object->Get(String::NewFromUtf8(isolate, "function"));

    if (function->IsUndefined()) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'function' property")));
        return;
    } else if (!function->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Function must be a string")));
        return;
    } else if (function->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Function must not be an empty string")));
        return;
    }

    uint32_t relink = auto_relink_g;

    if (arg_object->Has(String::NewFromUtf8(isolate, "autoRelink"))) {
        relink = arg_object->Get(String::NewFromUtf8(isolate, "autoRelink"))->BooleanValue();
    }

    Local<Value> arguments = arg_object->Get(String::NewFromUtf8(isolate, "arguments"));
    Local<Value> arg = Undefined(isolate);

    if (arguments->IsUndefined()) {
        arg = String::Empty(isolate);
    } else if (arguments->IsArray()) {
        arg = encode_arguments(arguments);

        if (arg->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Property 'arguments' contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'arguments' must be an array")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> relink: " << relink << "\n";
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    Local<Value> name = globalize_name(function);

    gtm_status_t stat_buf;
    gtm_char_t gtm_function[] = "function";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_function;
    access.rtn_name.length = 8;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> function: " << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << *String::Utf8Value(arg) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(arg), relink, mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {arg};

        if (debug_g > LOW) cout << "DEBUG>> function: " << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << gtm_args.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_args.to_byte(), relink, mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> function: " << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << *String::Utf8Value(arg) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_function, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(arg), relink, mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {arg};

        if (debug_g > LOW) cout << "DEBUG>> function: " << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << gtm_args.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_function, ret_buffer_g, gtm_name.to_byte(), gtm_args.to_byte(), relink, mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::function JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        return_object->Set(String::NewFromUtf8(isolate, "function"), localize_name(function));

        if (!arguments->IsUndefined()) {
            return_object->Set(String::NewFromUtf8(isolate, "arguments"), temp_object->Get(String::NewFromUtf8(isolate, "arguments")));
        }
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        return_object->Set(String::NewFromUtf8(isolate, "function"), localize_name(function));

        if (!arguments->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "arguments"), arguments);
    }

    return_object->Set(String::NewFromUtf8(isolate, "result"), temp_object->Get(String::NewFromUtf8(isolate, "result")));

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::function exit" << endl;
    return;
} // @end Gtm::function method

/*
 * @method {public} Gtm::get
 * @summary Get data from a global or local node
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

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = args[0]->ToObject();
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

        string test = *String::Utf8Value(glvn);
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
#if YDB_SIMPLE_API == 1
    vector<string> subsarray;
    string subsdata;
#endif

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        Local<Array> subscripts_array = Local<Array>::Cast(subscripts);
        unsigned int length = subscripts_array->Length();

        for (unsigned int i = 0; i < length; i++) {
            Local<Value> data = subscripts_array->Get(i);

            if (utf8_g == true) {
                subsdata = *String::Utf8Value(data);
            } else {
                GtmValue gtm_data {data};
                subsdata = gtm_data.to_byte();
            }

            subsarray.push_back(subsdata);
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf = 0;
    Local<Value> return_object = Object::New(isolate);
    string gvn, sub;

    if (utf8_g == true) {
        gvn = *String::Utf8Value(name);
        sub = *String::Utf8Value(subs);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) cout << name_msg << gvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << sub << endl;

    if (async) {
        Local<Function> callback = Local<Function>::Cast(args[args_count]);

        Baton *baton = new Baton();

        baton->request.data = baton;
        baton->api = "get";
        baton->callback_p.Reset(isolate, callback);
        baton->subscripts_p.Reset(isolate, subscripts);
        baton->glvn = gvn;
        baton->subs = sub;
#if YDB_SIMPLE_API == 1
        baton->subsarray = subsarray;
#endif
        baton->mode = mode_g;
        baton->status = stat_buf;
        baton->async = async;
        baton->local = local;
        baton->position = position;

        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    } else {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into get in the SimpleAPI interface" << "\n";

        stat_buf = ydb::get(ret_buffer_g, gvn, subsarray);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into get in the Call-in interface" << "\n";

        stat_buf = gtm::get(ret_buffer_g, gvn, sub, mode_g);
#endif

        if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

#if YDB_SIMPLE_API == 1
        if (stat_buf == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        } else if (stat_buf != EXIT_SUCCESS && stat_buf != YDB_ERR_GVUNDEF && stat_buf != YDB_ERR_LVUNDEF) {
#else
        if (stat_buf != EXIT_SUCCESS) {
#endif
            uv_mutex_lock(&mutex);
            gtm_zstatus(msg_buffer_g, MSG_LEN);
            uv_mutex_unlock(&mutex);

            if (position) {
                isolate->ThrowException(Exception::Error(gtm_status(msg_buffer_g, position, async)->ToString()));
                args.GetReturnValue().Set(Undefined(isolate));
                return;
            } else {
                args.GetReturnValue().Set(gtm_status(msg_buffer_g, position, async));
                return;
            }
        }

        if (debug_g > LOW) cout << "DEBUG>> call into get_ret" << "\n";

        return_object = get_ret(stat_buf, ret_buffer_g, position, local, async, gvn, subscripts);

        if (debug_g > OFF) cout << "DEBUG> Gtm::get exit" << endl;

        args.GetReturnValue().Set(return_object);
        return;
    }
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
        Local<Object> arg_object = args[0]->ToObject();

        max = arg_object->Get(String::NewFromUtf8(isolate, "max"));

        if (max->IsUndefined() || !max->IsNumber() || max->NumberValue() < 0) max = Number::New(isolate, 0);

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
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";
    if (debug_g > LOW) cout << "DEBUG>> max: " << max->Uint32Value() << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_global_directory[] = "global_directory";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_global_directory;
    access.rtn_name.length = 16;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> lo: " << *String::Utf8Value(lo) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << *String::Utf8Value(hi) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, max->Uint32Value(), *String::Utf8Value(lo), *String::Utf8Value(hi), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, max->Uint32Value(), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> lo: " << *String::Utf8Value(lo) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << *String::Utf8Value(hi) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_global_directory, ret_buffer_g, max->Uint32Value(), *String::Utf8Value(lo), *String::Utf8Value(hi), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_global_directory, ret_buffer_g, max->Uint32Value(), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::global_directory JSON string: " << *String::Utf8Value(json_string) << "\n";

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

    if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "version"))) {
        cout << "version or about method:\n"
            "\tDisplay version information - includes database version if connection has been established\n"
            "\n\tArguments: {None}\n"
            "\n\tReturns on success: {string}\n"
            "\n\tReturns on failure: Should never fail\n"
            "\n\tFor more information about the version method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "open"))) {
        cout << "open method:\n"
            "\tOpen connection to the database - all other methods except for version require an open database connection\n"
            "\n\tRequired arguments: {None}\n"
            "\n\tOptional argument:\n"
            "\t{\n"
            "\t\tglobalDirectory|namespace:\t{string} <none>,\n"
            "\t\troutinesPath:\t\t\t{string} <none>,\n"
            "\t\tcallinTable:\t\t\t{string} <none>,\n"
            "\t\tipAddress|ip_address:\t\t{string} <none>,\n"
            "\t\ttcpPort|tcp_port:\t\t{number|string} <none>,\n"
            "\t\tcharset:\t\t\t{string} [<utf8|utf-8>|m|binary|ascii]/i,\n"
            "\t\tmode:\t\t\t\t{string} [<canonical>|strict]/i,\n"
            "\t\tautoRelink:\t\t\t{boolean} <false>,\n"
            "\t\tdebug:\t\t\t\t{boolean} <false>|{string} [<off>|low|medium|high]/i|{number} [<0>|1|2|3]\n"
            "\t}\n"
            "\n\tReturns on success:\n"
            "\t{\n"
            "\t\tok:\t\t{boolean} true|{number} 1,\n"
            "\t\tresult:\t\t{number} 1,\n"
            "\t\tgtm_pid:\t{number}|{string}\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\t\t- Failures from bad environment set-ups result in internal errors from " NODEM_DB "\n"
            "\n\tFor more information about the open method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "close"))) {
        cout << "close method:\n"
            "\tClose connection to the database - once closed, cannot be re-opened during the current process\n"
            "\n\tRequired arguments: {None}\n"
            "\n\tOptional argument:\n"
            "\t{\n"
            "\t\tresetTerminal: {boolean} <false>\n"
            "\t}\n"
            "\n\tReturns on success: {number|string} 1\n"
            "\n\tReturns on failure: Should never fail\n"
            "\n\tFor more information about the close method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "data"))) {
        cout << "data method:\n"
            "\tDisplay information about the existence of data and/or children in local variables or globals\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the data method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "get"))) {
        cout << "get method:\n"
            "\tRetrieve the data stored at a local or global node\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the get method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "set"))) {
        cout << "set method:\n"
            "\tStore data in a local or global node\n"
            "\n\tRequired argument:\n"
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
            "\t\tresult:\t\t{number} 0\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the set method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "kill"))) {
        cout << "kill method:\n"
            "\tRemove data stored in a local or global node, or remove the entire local symbol table\n"
            "\n\tRequired arguments: {None} - Without an argument, will clear the entire local symbol table for that process\n"
            "\n\tOptional argument:\n"
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
            "\tOR: {number|string} 0\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the kill method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "order"))) {
        cout << "order or next method:\n"
            "\tRetrieve the next node, at the current subscript depth\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the order/next method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "previous"))) {
        cout << "previous method:\n"
            "\tRetrieve the previous node, at the current subscript depth\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the previous method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "nextNode"))) {
        cout << "nextNode or next_node method:\n"
            "\tRetrieve the next node, regardless of subscript depth\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the nextNode/next_node method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "previousNode"))) {
        cout << "previousNode or previous_node method:\n"
            "\tRetrieve the previous node, regardless of subscript depth\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the previousNode/previous_node method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "merge"))) {
        cout << "merge method:\n"
            "\tCopy an entire data tree, or sub-tree, from a local array or global, to another local array or global\n"
            "\n\tRequired argument:\n"
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
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "increment"))) {
        cout << "increment method:\n"
            "\tAtomically increment a local or global data node\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the increment method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "lock"))) {
        cout << "lock method:\n"
            "\tLock a local or global tree, or sub-tree, or individual node - locks are advisory, not mandatory\n"
            "\n\tRequired argument:\n"
            "\t{\n"
            "\t\tglobal|local:\t{required} {string},\n"
            "\t\tsubscripts:\t{optional} {array {string|number}},\n"
            "\t\ttimeout:\t{optional} {number}\n"
            "\t}\n"
            "\n\tOptional argument: Timeout {number} as second argument\n"
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
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "unlock"))) {
        cout << "unlock method:\n"
            "\tUnlock a local or global tree, or sub-tree, or individual node, or release all locks held by process\n"
            "\n\tRequired arguments: {None} - Without an argument, will clear the entire lock table for that process\n"
            "\n\tOptional argument:\n"
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
            "\tOR: {number|string} 0\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the unlock method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "globalDirectory"))) {
        cout << "globalDirectory or global_directory method:\n"
            "\tList globals stored in the database\n"
            "\n\tRequired arguments: {None} - Without an argument, will list all the globals stored in the database\n"
            "\n\tOptional argument:\n"
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
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "localDirectory"))) {
        cout << "localDirectory or local_directory method:\n"
            "\tList local variables stored in the symbol table\n"
            "\n\tRequired arguments: {None} - Without an argument, will list all the variables in the local symbol table\n"
            "\n\tOptional argument:\n"
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
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "function"))) {
        cout << "function method:\n"
            "\tCall an extrinsic function in " NODEM_DB " code\n"
            "\n\tRequired argument:\n"
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
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the function method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "procedure"))) {
        cout << "procedure or routine method:\n"
            "\tCall a procedure/routine/subroutine label in " NODEM_DB " code\n"
            "\n\tRequired argument:\n"
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
            "\t\tresult:\t\t\t{string|number} 0\n"
            "\t}\n"
            "\n\tReturns on failure:\n"
            "\t{\n"
            "\t\tok:\t\t\t\t{boolean} false|{number} 0,\n"
            "\t\terrorCode|ErrorCode:\t\t{number},\n"
            "\t\terrorMessage|ErrorMessage:\t{string}\n"
            "\t}\n"
            "\t- Failures from bad user input can result in thrown exception messages or stack traces\n"
            "\n\tFor more information about the procedure/routine method, please refer to the README.md file\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "retrieve"))) {
        cout << "retrieve method:\n"
            "\tRetrieve a local or global tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            << endl;
    } else if (args[0]->ToString()->StrictEquals(String::NewFromUtf8(isolate, "update"))) {
        cout << "update method:\n"
            "\tStore an object in a local or global tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            << endl;
    } else {
        cout << "NodeM: Gtm class help menu - methods:\n"
            "\nversion\t\tDisplay version information - includes database version if connection has been established\n"
            "open\t\tOpen connection to the database - all other methods except for version require an open database connection\n"
            "close\t\tClose connection to the database - once closed, cannot be re-opened during the current process\n"
            "data\t\tDisplay information about the existence of data and/or children in local variables or globals\n"
            "get\t\tRetrieve the data stored at a local or global node\n"
            "set\t\tStore data in a local or global node\n"
            "kill\t\tRemove data stored in a local or global node; or remove the entire local symbol table\n"
            "order\t\tRetrieve the next node, at the current subscript depth\n"
            "previous\tRetrieve the previous node, at the current subscript depth\n"
            "nextNode\tRetrieve the next node, regardless of subscript depth\n"
            "previousNode\tRetrieve the previous node, regardless of subscript depth\n"
            "merge\t\tCopy an entire data tree, or sub-tree, from a local array or global, to another local array or global\n"
            "increment\tAtomically increment a local or global data node\n"
            "lock\t\tLock a local or global tree, or sub-tree, or individual node - locks are advisory, not mandatory\n"
            "unlock\t\tUnlock a local or global tree, or sub-tree, or individual node; or release all locks held by process\n"
            "globalDirectory\tList globals stored in the database\n"
            "localDirectory\tList local variables stored in the symbol table\n"
            "function\tCall an extrinsic function in " NODEM_DB " code\n"
            "procedure\tCall a procedure/routine/subroutine label in " NODEM_DB " code\n"
            "retrieve\tRetrieve a local or global tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            "update\t\tStore an object in a local or global tree or sub-tree structure - NOT YET IMPLEMENTED\n"
            "\nFor more information about each method, call help with the method name as an argument\n"
            << endl;
    }

    args.GetReturnValue().Set(String::NewFromUtf8(isolate, "NodeM - Copyright (C) 2012-2018 Fourth Watch Software LC"));
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

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));
    bool local = false;

    if (glvn->IsUndefined()) {
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

    Local<Value> increment = Undefined(isolate);

    if (arg_object->Get(String::NewFromUtf8(isolate, "increment"))->IsUndefined()) {
        increment = Number::New(isolate, 1);
    } else {
#if NODE_MAJOR_VERSION >= 1
        increment = arg_object->Get(String::NewFromUtf8(isolate, "increment"))->ToNumber(isolate);
#else
        increment = arg_object->Get(String::NewFromUtf8(isolate, "increment"))->ToNumber();
#endif
    }

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> increment: " << increment->NumberValue() << "\n";
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_increment[] = "increment";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_increment;
    access.rtn_name.length = 9;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), increment->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), increment->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_increment, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), increment->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_increment, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), increment->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::increment JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
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

    return_object->Set(String::NewFromUtf8(isolate, "data"), temp_object->Get(String::NewFromUtf8(isolate, "data")));

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::increment exit" << endl;
    return;
} // @end Gtm::increment method

/*
 * @method {public} Gtm::kill
 * @summary Kill a global or local, or global or local node
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

    if (args[0]->IsObject()) {
        Local<Object> arg_object = args[0]->ToObject();
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
            return;
        }

        subscripts = arg_object->Get(String::NewFromUtf8(isolate, "subscripts"));
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

        string test = *String::Utf8Value(glvn);
        if (test[0] != '^') local = true;
    }

    Local<Value> subs = String::Empty(isolate);
#if YDB_SIMPLE_API == 1
    vector<string> subsarray;
    string subsdata;
#endif

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
            Local<Array> subscripts_array = Local<Array>::Cast(subscripts);
            unsigned int length = subscripts_array->Length();

            for (unsigned int i = 0; i < length; i++) {
                Local<Value> data = subscripts_array->Get(i);

                if (utf8_g == true) {
                    subsdata = *String::Utf8Value(data);
                } else {
                    GtmValue gtm_data {data};
                    subsdata = gtm_data.to_byte();
                }

                subsarray.push_back(subsdata);
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf = 0;
    Local<Value> return_object = Object::New(isolate);
    string gvn, sub;

    if (utf8_g == true) {
        gvn = *String::Utf8Value(name);
        sub = *String::Utf8Value(subs);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) cout << name_msg << gvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << sub << endl;

    if (async) {
        Local<Function> callback = Local<Function>::Cast(args[args_count]);

        Baton *baton = new Baton();

        baton->request.data = baton;
        baton->api = "kill";
        baton->callback_p.Reset(isolate, callback);
        baton->subscripts_p.Reset(isolate, subscripts);
        baton->glvn = gvn;
        baton->subs = sub;
#if YDB_SIMPLE_API == 1
        baton->subsarray = subsarray;
#endif
        baton->mode = mode_g;
        baton->status = stat_buf;
        baton->async = async;
        baton->local = local;
        baton->position = position;

        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    } else {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into kill in the SimpleAPI interface" << "\n";

        stat_buf = ydb::kill(gvn, subsarray);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into kill in the Call-in interface" << "\n";

        stat_buf = gtm::kill(gvn, sub, mode_g);
#endif

        if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

        if (stat_buf == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        } else if (stat_buf != EXIT_SUCCESS) {
            uv_mutex_lock(&mutex);
            gtm_zstatus(msg_buffer_g, MSG_LEN);
            uv_mutex_unlock(&mutex);

            if (position) {
                isolate->ThrowException(Exception::Error(gtm_status(msg_buffer_g, position, async)->ToString()));
                args.GetReturnValue().Set(Undefined(isolate));
                return;
            } else {
                args.GetReturnValue().Set(gtm_status(msg_buffer_g, position, async));
                return;
            }
        }

        if (debug_g > LOW) cout << "DEBUG>> call into kill_ret" << "\n";

        return_object = kill_ret(position, local, async, gvn, subscripts);

        if (debug_g > OFF) cout << "DEBUG> Gtm::kill exit" << endl;

        args.GetReturnValue().Set(return_object);
        return;
    }
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
        Local<Object> arg_object = args[0]->ToObject();

        max = arg_object->Get(String::NewFromUtf8(isolate, "max"));

        if (max->IsUndefined() || !max->IsNumber() || max->NumberValue() < 0) max = Number::New(isolate, 0);

        lo = arg_object->Get(String::NewFromUtf8(isolate, "lo"));

        if (lo->IsUndefined() || !lo->IsString()) lo = String::Empty(isolate);

        hi = arg_object->Get(String::NewFromUtf8(isolate, "hi"));

        if (hi->IsUndefined() || !hi->IsString()) hi = String::Empty(isolate);
    } else {
        max = Number::New(isolate, 0);
        lo = String::Empty(isolate);
        hi = String::Empty(isolate);
    }

    if (invalid_local(*String::Utf8Value(lo))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'lo' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*String::Utf8Value(lo))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'lo' is an invalid name")));
        return;
    }

    if (invalid_local(*String::Utf8Value(hi))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'hi' cannot begin with 'v4w'")));
        return;
    }

    if (invalid_name(*String::Utf8Value(hi))) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'hi' is an invalid name")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";
    if (debug_g > LOW) cout << "DEBUG>> max: " << max->Uint32Value() << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_local_directory[] = "local_directory";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_local_directory;
    access.rtn_name.length = 15;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> lo: " << *String::Utf8Value(lo) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << *String::Utf8Value(hi) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, max->Uint32Value(), *String::Utf8Value(lo), *String::Utf8Value(hi), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, max->Uint32Value(), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> lo: " << *String::Utf8Value(lo) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << *String::Utf8Value(hi) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_local_directory, ret_buffer_g, max->Uint32Value(), *String::Utf8Value(lo), *String::Utf8Value(hi), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_lo {lo};
        GtmValue gtm_hi {hi};

        if (debug_g > LOW) cout << "DEBUG>> lo: " << gtm_lo.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> hi: " << gtm_hi.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_local_directory, ret_buffer_g, max->Uint32Value(), gtm_lo.to_byte(), gtm_hi.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::local_directory JSON string: " << *String::Utf8Value(json_string) << "\n";

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

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));
    bool local = false;

    if (glvn->IsUndefined()) {
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

    Local<Value> timeout = Undefined(isolate);

    if (args.Length() > 1) {
#if NODE_MAJOR_VERSION >= 1
        timeout = args[1]->ToNumber(isolate);
#else
        timeout = args[1]->ToNumber();
#endif

        if (timeout->NumberValue() < 0) timeout = Number::New(isolate, 0);
    } else if (arg_object->Has(String::NewFromUtf8(isolate, "timeout"))) {
#if NODE_MAJOR_VERSION >= 1
        timeout = arg_object->Get(String::NewFromUtf8(isolate, "timeout"))->ToNumber(isolate);
#else
        timeout = arg_object->Get(String::NewFromUtf8(isolate, "timeout"))->ToNumber();
#endif

        if (timeout->NumberValue() < 0) timeout = Number::New(isolate, 0);
    } else {
        timeout = Number::New(isolate, -1);
    }

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> timeout: " << timeout->NumberValue() << "\n";
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_lock[] = "lock";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_lock;
    access.rtn_name.length = 4;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), timeout->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), timeout->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_lock, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), timeout->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_lock, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), timeout->NumberValue(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::lock JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
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
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, 1));

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

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> from_test = arg_object->Get(String::NewFromUtf8(isolate, "from"));
    Local<Value> to_test = arg_object->Get(String::NewFromUtf8(isolate, "to"));
    bool from_local = false;
    bool to_local = false;

    if (!arg_object->Has(String::NewFromUtf8(isolate, "from"))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'from' property")));
        return;
    } else if (!from_test->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "'from' property must be an object")));
        return;
    }

    Local<Object> from = from_test->ToObject();

    if (!arg_object->Has(String::NewFromUtf8(isolate, "to"))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'to' property")));
        return;
    } else if (!to_test->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "'to' property must be an object")));
        return;
    }

    Local<Object> to = to_test->ToObject();
    Local<Value> from_glvn = from->Get(String::NewFromUtf8(isolate, "global"));

    if (from_glvn->IsUndefined()) {
        from_glvn = from->Get(String::NewFromUtf8(isolate, "local"));

        if (from_glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need a 'global' or 'local' property in your 'from' object")));
            return;
        } else {
            from_local = true;
        }
    }

    if (!from_glvn->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Global in 'from' must be a string")));
        return;
    } else if (from_glvn->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Global in 'from' must not be an empty string")));
        return;
    }

    Local<Value> to_glvn = to->Get(String::NewFromUtf8(isolate, "global"));

    if (to_glvn->IsUndefined()) {
        to_glvn = to->Get(String::NewFromUtf8(isolate, "local"));

        if (to_glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need a 'global' or 'local' property in your 'to' object")));
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
            Local<String> error_message = String::NewFromUtf8(isolate, "Property 'subscripts' in 'from' object contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' in 'from' must be an array")));
        return;
    }

    Local<Value> to_subscripts = to->Get(String::NewFromUtf8(isolate, "subscripts"));
    Local<Value> to_subs = Undefined(isolate);

    if (to_subscripts->IsUndefined()) {
        to_subs = String::Empty(isolate);
    } else if (to_subscripts->IsArray()) {
        to_subs = encode_arguments(to_subscripts);

        if (to_subs->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Property 'subscripts' in 'to' object contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'subscripts' in 'to' must be an array")));
        return;
    }

    const char *from_name_msg;
    Local<Value> from_name = Undefined(isolate);

    if (from_local) {
        if (invalid_name(*String::Utf8Value(from_glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        from_name_msg = "DEBUG>> from_local: ";
        from_name = localize_name(from_glvn);

        if (invalid_local(*String::Utf8Value(from_name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' in 'from' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(from_glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        from_name_msg = "DEBUG>> from_global: ";
        from_name = globalize_name(from_glvn);
    }

    const char *to_name_msg;
    Local<Value> to_name = Undefined(isolate);

    if (to_local) {
        if (invalid_name(*String::Utf8Value(to_glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        to_name_msg = "DEBUG>> to_local: ";
        to_name = localize_name(to_glvn);

        if (invalid_local(*String::Utf8Value(to_name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' in 'to' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(to_glvn))) {
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

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_merge;
    access.rtn_name.length = 5;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << from_name_msg << *String::Utf8Value(from_name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> from_subscripts: " << *String::Utf8Value(from_subs) << "\n";
        if (debug_g > LOW) cout << to_name_msg << *String::Utf8Value(to_name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> to_subscripts: " << *String::Utf8Value(to_subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(from_name), *String::Utf8Value(from_subs), *String::Utf8Value(to_name), *String::Utf8Value(to_subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_from_name {from_name};
        GtmValue gtm_from_subs {from_subs};
        GtmValue gtm_to_name {to_name};
        GtmValue gtm_to_subs {to_subs};

        if (debug_g > LOW) cout << from_name_msg << gtm_from_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> from_subscripts: " << gtm_from_subs.to_byte() << "\n";
        if (debug_g > LOW) cout << to_name_msg << gtm_to_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> to_subscripts: " << gtm_to_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_from_name.to_byte(), gtm_from_subs.to_byte(), gtm_to_name.to_byte(), gtm_to_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << from_name_msg << *String::Utf8Value(from_name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> from_subscripts: " << *String::Utf8Value(from_subs) << "\n";
        if (debug_g > LOW) cout << to_name_msg << *String::Utf8Value(to_name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> to_subscripts: " << *String::Utf8Value(to_subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_merge, ret_buffer_g, *String::Utf8Value(from_name), *String::Utf8Value(from_subs), *String::Utf8Value(to_name), *String::Utf8Value(to_subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_from_name {from_name};
        GtmValue gtm_from_subs {from_subs};
        GtmValue gtm_to_name {to_name};
        GtmValue gtm_to_subs {to_subs};

        if (debug_g > LOW) cout << from_name_msg << gtm_from_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> from_subscripts: " << gtm_from_subs.to_byte() << "\n";
        if (debug_g > LOW) cout << to_name_msg << gtm_to_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> to_subscripts: " << gtm_to_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_merge, ret_buffer_g, gtm_from_name.to_byte(), gtm_from_subs.to_byte(), gtm_to_name.to_byte(), gtm_to_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::merge JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
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

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));
    bool local = false;

    if (glvn->IsUndefined()) {
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_next_node[] = "next_node";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_next_node;
    access.rtn_name.length = 9;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_next_node, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_next_node, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::next_node JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
        }

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));
        if (!temp_subs->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subs);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));

        Local<Value> temp_data = temp_object->Get(String::NewFromUtf8(isolate, "data"));
        if (!temp_data->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "data"), temp_data);
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
        }

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));
        if (!temp_subs->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = temp_object->Get(String::NewFromUtf8(isolate, "data"));
        if (!temp_data->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "data"), temp_data);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::next_node exit" << endl;
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

    char *relink = getenv("NODEM_AUTO_RELINK");
    if (relink != NULL) auto_relink_g = static_cast<bool>(atoi(relink));

    if (args[0]->IsObject()) {
        Local<Object> arg_object = args[0]->ToObject();

        if (arg_object->Has(String::NewFromUtf8(isolate, "debug"))) {
            String::Utf8Value debug(arg_object->Get(String::NewFromUtf8(isolate, "debug")));

            if (strcasecmp(*debug, "off") == 0) {
                debug_g = OFF;
            } else if (strcasecmp(*debug, "low") == 0) {
                debug_g = LOW;
            } else if (strcasecmp(*debug, "medium") == 0) {
                debug_g = MEDIUM;
            } else if (strcasecmp(*debug, "high") == 0) {
                debug_g = HIGH;
            } else {
                debug_g = static_cast<debug_t>(arg_object->Get(String::NewFromUtf8(isolate, "debug"))->Uint32Value());
            }
        }

        if (debug_g > OFF) cout << "\nDEBUG> Gtm::open enter" << endl;
        if (debug_g > LOW) cout << "DEBUG>> debug: " << debug_g << "\n";

        Local<Value> global_directory = arg_object->Get(String::NewFromUtf8(isolate, "globalDirectory"));

        if (global_directory->IsUndefined()) global_directory = arg_object->Get(String::NewFromUtf8(isolate, "namespace"));

        if (!global_directory->IsUndefined() && global_directory->IsString()) {
            if (debug_g > LOW) cout << "DEBUG>> globalDirectory: " << *String::Utf8Value(global_directory) << "\n";

#ifdef LIBYOTTADB_TYPES_H
            if (setenv("ydb_gbldir", *String::Utf8Value(global_directory), 1) == -1) {
#else
            if (setenv("gtmgbldir", *String::Utf8Value(global_directory), 1) == -1) {
#endif
                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
                return;
            }
        }

        Local<Value> routines_path = arg_object->Get(String::NewFromUtf8(isolate, "routinesPath"));

        if (!routines_path->IsUndefined() && routines_path->IsString()) {
            if (debug_g > LOW) cout << "DEBUG>> routinesPath: " << *String::Utf8Value(routines_path) << "\n";

            if (setenv("gtmroutines", *String::Utf8Value(routines_path), 1) == -1) {
                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
                return;
            }
        }

        Local<Value> callin_table = arg_object->Get(String::NewFromUtf8(isolate, "callinTable"));

        if (!callin_table->IsUndefined() && callin_table->IsString()) {
            if (debug_g > LOW) cout << "DEBUG>> callinTable: " << *String::Utf8Value(callin_table) << "\n";

            if (setenv("GTMCI", *String::Utf8Value(callin_table), 1) == -1) {
                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
                return;
            }
        }

        Local<Value> addr = arg_object->Get(String::NewFromUtf8(isolate, "ipAddress"));
        const char *addrMsg;

        if (addr->IsUndefined()) {
            addr = arg_object->Get(String::NewFromUtf8(isolate, "ip_address"));
            addrMsg = "ip_address must be a string";
        } else {
            addrMsg = "ipAddress must be a string";
        }

        Local<Value> port = arg_object->Get(String::NewFromUtf8(isolate, "tcpPort"));
        const char *portMsg;

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
                Local<String> gtcm_port = String::Concat(String::NewFromUtf8(isolate, ":"), port->ToString());
                gtcm_nodem = String::Concat(addr->ToString(), gtcm_port);
            } else {
                isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, portMsg)));
                return;
            }

            if (debug_g > LOW) cout << "DEBUG>> GTMCM_NODEM: " << *String::Utf8Value(gtcm_nodem) << "\n";

            if (setenv("GTCM_NODEM", *String::Utf8Value(gtcm_nodem), 1) == -1) {
                isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
                return;
            }
        }

        if (arg_object->Has(String::NewFromUtf8(isolate, "autoRelink"))) {
            auto_relink_g = arg_object->Get(String::NewFromUtf8(isolate, "autoRelink"))->BooleanValue();
        }

        if (debug_g > LOW) cout << "DEBUG>> autoRelink: " << auto_relink_g << "\n";

        String::Utf8Value nodem_mode(arg_object->Get(String::NewFromUtf8(isolate, "mode")));

        if (strcasecmp(*nodem_mode, "strict") == 0) mode_g = STRICT;

        if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

        String::Utf8Value data_charset(arg_object->Get(String::NewFromUtf8(isolate, "charset")));

        if (strcasecmp(*data_charset, "m") == 0 || strcasecmp(*data_charset, "binary") == 0 || strcasecmp(*data_charset, "ascii") == 0) {
            utf8_g = false;
        } else if (strcasecmp(*data_charset, "utf-8") == 0 || strcasecmp(*data_charset, "utf8") == 0) {
            utf8_g = true;
        }

        if (debug_g > LOW) cout << "DEBUG>> charset: " << utf8_g << endl;
    }

    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &term_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        }
    } else if (isatty(STDOUT_FILENO)) {
        if (tcgetattr(STDOUT_FILENO, &term_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        }
    } else if (isatty(STDERR_FILENO)) {
        if (tcgetattr(STDERR_FILENO, &term_attr_g) == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        }
    }

    uv_mutex_lock(&mutex);
#if YDB_SIMPLE_API == 1
    if (ydb_init() != YDB_OK) {
#else
    if (gtm_init() != EXIT_SUCCESS) {
#endif
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }
    uv_mutex_unlock(&mutex);

    gtm_state_g = OPEN;

    if (sigemptyset(&signal_attr_g.sa_mask) == -1) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot empty signal handler")));
        return;
    }

    signal_attr_g.sa_handler = catch_interrupt;
    signal_attr_g.sa_flags = 0;

    if (sigaction(SIGINT, &signal_attr_g, NULL) == -1) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGINT handler")));
        return;
    }

    if (sigaction(SIGTERM, &signal_attr_g, NULL) == -1) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGTERM handler")));
        return;
    }

    if (sigaction(SIGQUIT, &signal_attr_g, NULL) == -1) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Cannot initialize SIGQUIT handler")));
        return;
    }

    if (debug_g > LOW) {
        gtm_status_t stat_buf;
        gtm_char_t gtm_debug[] = "debug";

#if GTM_CIP_API == 1
        ci_name_descriptor access;

        access.rtn_name.address = gtm_debug;
        access.rtn_name.length = 5;
        access.handle = NULL;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, debug_g);
        uv_mutex_unlock(&mutex);
#else
        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_debug, debug_g);
        uv_mutex_unlock(&mutex);
#endif

        if (stat_buf != EXIT_SUCCESS) {
            uv_mutex_lock(&mutex);
            gtm_zstatus(msg_buffer_g, MSG_LEN);
            uv_mutex_unlock(&mutex);

            args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
            return;
        }
    }

    Local<Object> result = Object::New(isolate);

    if (mode_g == STRICT) {
        result->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        result->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 1));
        result->Set(String::NewFromUtf8(isolate, "gtm_pid"), Number::New(isolate, getpid())->ToString());
    } else {
        result->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));
        result->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 1));
        result->Set(String::NewFromUtf8(isolate, "gtm_pid"), Number::New(isolate, getpid()));
    }

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::open exit" << endl;

    args.GetReturnValue().Set(result);
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

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = args[0]->ToObject();
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

        string test = *String::Utf8Value(glvn);
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
#if YDB_SIMPLE_API == 1
    vector<string> subsarray;
    string subsdata;
#endif

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        Local<Array> subscripts_array = Local<Array>::Cast(subscripts);
        unsigned int length = subscripts_array->Length();

        for (unsigned int i = 0; i < length; i++) {
            Local<Value> data = subscripts_array->Get(i);

            if (utf8_g == true) {
                subsdata = *String::Utf8Value(data);
            } else {
                GtmValue gtm_data {data};
                subsdata = gtm_data.to_byte();
            }

            subsarray.push_back(subsdata);
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf = 0;
    Local<Value> return_object = Object::New(isolate);
    string gvn, sub;

    if (utf8_g == true) {
        gvn = *String::Utf8Value(name);
        sub = *String::Utf8Value(subs);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
    }

    if (debug_g > LOW) cout << name_msg << gvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << sub << endl;

    if (async) {
        Local<Function> callback = Local<Function>::Cast(args[args_count]);

        Baton *baton = new Baton();

        baton->request.data = baton;
        baton->api = "order";
        baton->callback_p.Reset(isolate, callback);
        baton->subscripts_p.Reset(isolate, subscripts);
        baton->glvn = gvn;
        baton->subs = sub;
#if YDB_SIMPLE_API == 1
        baton->subsarray = subsarray;
#endif
        baton->mode = mode_g;
        baton->status = stat_buf;
        baton->async = async;
        baton->local = local;
        baton->position = position;

        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    } else {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into order in the SimpleAPI interface" << "\n";

        stat_buf = ydb::order(ret_buffer_g, gvn, subsarray);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into order in the Call-in interface" << "\n";

        stat_buf = gtm::order(ret_buffer_g, gvn, sub, mode_g);
#endif

        if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

        if (stat_buf == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        } else if (stat_buf != EXIT_SUCCESS) {
            uv_mutex_lock(&mutex);
            gtm_zstatus(msg_buffer_g, MSG_LEN);
            uv_mutex_unlock(&mutex);

            if (position) {
                isolate->ThrowException(Exception::Error(gtm_status(msg_buffer_g, position, async)->ToString()));
                args.GetReturnValue().Set(Undefined(isolate));
                return;
            } else {
                args.GetReturnValue().Set(gtm_status(msg_buffer_g, position, async));
                return;
            }
        }

        if (debug_g > LOW) cout << "DEBUG>> call into order_ret" << "\n";

        return_object = order_ret(ret_buffer_g, position, local, async, gvn, subscripts);

        if (debug_g > OFF) cout << "DEBUG> Gtm::order exit" << endl;

        args.GetReturnValue().Set(return_object);
        return;
    }
} // @end Gtm::order method

/*
 * @method {public} Gtm::previous
 * @summary Same as order, only in reverse
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

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));
    bool local = false;

    if (glvn->IsUndefined()) {
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_previous[] = "previous";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_previous;
    access.rtn_name.length = 8;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_previous, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_previous, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::previous JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        Local<Value> result = temp_object->Get(String::NewFromUtf8(isolate, "result"));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), glvn);
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            new_subscripts->Set(Number::New(isolate, new_subscripts->Length() -1), result);
            return_object->Set(String::NewFromUtf8(isolate, "subscripts"), new_subscripts);
        }

        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));
        return_object->Set(String::NewFromUtf8(isolate, "result"), result);
    } else {
        Local<Value> result = temp_object->Get(String::NewFromUtf8(isolate, "result"));

        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
        }

        if (!subscripts->IsUndefined() && Local<Array>::Cast(subscripts)->Length() > 0) {
            Local<Array> new_subscripts = Local<Array>::Cast(subscripts);

            new_subscripts->Set(Number::New(isolate, new_subscripts->Length() -1), result);
            return_object->Set(String::NewFromUtf8(isolate, "subscripts"), new_subscripts);
        }

        return_object->Set(String::NewFromUtf8(isolate, "result"), result);
    }

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::previous exit" << endl;
    return;
} // @end Gtm::previous method

/*
 * @method {public} Gtm::previous_node
 * @summary Same as nextNode, only in reverse
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

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));
    bool local = false;

    if (glvn->IsUndefined()) {
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'global' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf;
    gtm_char_t gtm_previous_node[] = "previous_node";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_previous_node;
    access.rtn_name.length = 13;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_previous_node, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_previous_node, ret_buffer_g, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::previous_node JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    if (temp_object->Has(String::NewFromUtf8(isolate, "status"))) {
        args.GetReturnValue().Set(temp_object);
        return;
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
        }

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));
        if (!temp_subs->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subs);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));

        Local<Value> temp_data = temp_object->Get(String::NewFromUtf8(isolate, "data"));
        if (!temp_data->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "data"), temp_data);
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (local) {
            return_object->Set(String::NewFromUtf8(isolate, "local"), name);
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "global"), localize_name(glvn));
        }

        Local<Value> temp_subs = temp_object->Get(String::NewFromUtf8(isolate, "subscripts"));
        if (!temp_subs->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "subscripts"), temp_subs);

        Local<Value> temp_data = temp_object->Get(String::NewFromUtf8(isolate, "data"));
        if (!temp_data->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "data"), temp_data);

        return_object->Set(String::NewFromUtf8(isolate, "defined"), temp_object->Get(String::NewFromUtf8(isolate, "defined")));
    }

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::previous_node exit" << endl;
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

    if (args.Length() == 0) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply an argument")));
        return;
    } else if (!args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Argument must be an object")));
        return;
    }

    Local<Object> arg_object = args[0]->ToObject();
    Local<Value> procedure = arg_object->Get(String::NewFromUtf8(isolate, "procedure"));
    bool routine = false;

    if (procedure->IsUndefined()) {
        procedure = arg_object->Get(String::NewFromUtf8(isolate, "routine"));

        if (procedure->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'procedure' or 'routine' property")));
            return;
        } else {
            routine = true;
        }
    }

    if (!procedure->IsString()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Procedure must be a string")));
        return;
    } else if (procedure->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Procedure must not be an empty string")));
        return;
    }

    uint32_t relink = auto_relink_g;

    if (arg_object->Has(String::NewFromUtf8(isolate, "autoRelink"))) {
        relink = arg_object->Get(String::NewFromUtf8(isolate, "autoRelink"))->BooleanValue();
    }

    Local<Value> arguments = arg_object->Get(String::NewFromUtf8(isolate, "arguments"));
    Local<Value> arg = Undefined(isolate);

    if (arguments->IsUndefined()) {
        arg = String::Empty(isolate);
    } else if (arguments->IsArray()) {
        arg = encode_arguments(arguments);

        if (arg->IsUndefined()) {
            Local<String> error_message = String::NewFromUtf8(isolate, "Property 'arguments' contains invalid data");
            isolate->ThrowException(Exception::SyntaxError(error_message));
            return;
        }
    } else {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Property 'arguments' must be an array")));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> relink: " << relink << "\n";
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    Local<Value> name = globalize_name(procedure);

    gtm_status_t stat_buf;
    gtm_char_t gtm_procedure[] = "procedure";

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_procedure;
    access.rtn_name.length = 9;
    access.handle = NULL;

    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> procedure: " << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << *String::Utf8Value(arg) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(arg), relink, mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {arg};

        if (debug_g > LOW) cout << "DEBUG>> procedure: " << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << gtm_args.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, ret_buffer_g, gtm_name.to_byte(), gtm_args.to_byte(), relink, mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << "DEBUG>> procedure: " << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << *String::Utf8Value(arg) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_procedure, ret_buffer_g, *String::Utf8Value(name), *String::Utf8Value(arg), relink, mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_args {arg};

        if (debug_g > LOW) cout << "DEBUG>> procedure: " << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> arguments: " << gtm_args.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_procedure, ret_buffer_g, gtm_name.to_byte(), gtm_args.to_byte(), relink, mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::procedure JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    Local<Object> return_object = Object::New(isolate);

    if (mode_g == STRICT) {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Number::New(isolate, 1));

        if (routine) {
            return_object->Set(String::NewFromUtf8(isolate, "routine"), localize_name(procedure));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "procedure"), localize_name(procedure));
        }

        if (!arguments->IsUndefined()) {
            return_object->Set(String::NewFromUtf8(isolate, "arguments"), temp_object->Get(String::NewFromUtf8(isolate, "arguments")));
        }

        return_object->Set(String::NewFromUtf8(isolate, "result"), String::Empty(isolate));
    } else {
        return_object->Set(String::NewFromUtf8(isolate, "ok"), Boolean::New(isolate, true));

        if (routine) {
            return_object->Set(String::NewFromUtf8(isolate, "routine"), localize_name(procedure));
        } else {
            return_object->Set(String::NewFromUtf8(isolate, "procedure"), localize_name(procedure));
        }

        if (!arguments->IsUndefined()) return_object->Set(String::NewFromUtf8(isolate, "arguments"), arguments);

        return_object->Set(String::NewFromUtf8(isolate, "result"), Number::New(isolate, 0));
    }

    args.GetReturnValue().Set(return_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::procedure exit" << endl;
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

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_retrieve;
    access.rtn_name.length = 8;
    access.handle = NULL;

    uv_mutex_lock(&mutex);
    stat_buf = gtm_cip(&access, ret_buffer_g);
    uv_mutex_unlock(&mutex);
#else
    uv_mutex_lock(&mutex);
    stat_buf = gtm_ci(gtm_retrieve, ret_buffer_g);
    uv_mutex_unlock(&mutex);
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::retrieve JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
    }

    args.GetReturnValue().Set(temp_object);

    if (debug_g > OFF) cout << "\nDEBUG> Gtm::retrieve exit" << endl;
    return;
} // @end Gtm::retrieve method

/*
 * @method {public} Gtm::set
 * @summary Set a global or local node
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

    Local<Value> glvn = Undefined(isolate);
    Local<Value> subscripts = Undefined(isolate);
    Local<Value> data, data_node;
    bool local = false;
    bool position = false;

    if (args[0]->IsObject()) {
        Local<Object> arg_object = args[0]->ToObject();
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

        string test = *String::Utf8Value(glvn);
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
#if YDB_SIMPLE_API == 1
    vector<string> subsarray;
    string subsdata;
#endif

    if (subscripts->IsUndefined()) {
        subs = String::Empty(isolate);
    } else if (subscripts->IsArray()) {
#if YDB_SIMPLE_API == 1
        Local<Array> subscripts_array = Local<Array>::Cast(subscripts);
        unsigned int length = subscripts_array->Length();

        for (unsigned int i = 0; i < length; i++) {
            Local<Value> data = subscripts_array->Get(i);

            if (utf8_g == true) {
                subsdata = *String::Utf8Value(data);
            } else {
                GtmValue gtm_data {data};
                subsdata = gtm_data.to_byte();
            }

            subsarray.push_back(subsdata);
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Local cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Global is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> global: ";
        name = globalize_name(glvn);
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;
    if (debug_g > LOW) cout << "DEBUG>> mode: " << mode_g << "\n";

    gtm_status_t stat_buf = 0;
    Local<Value> return_object = Object::New(isolate);
    string gvn, sub, value;

    if (utf8_g == true) {
        gvn = *String::Utf8Value(name);
        sub = *String::Utf8Value(subs);
        value = *String::Utf8Value(data_node);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};
        GtmValue gtm_data {data_node};

        gvn = gtm_name.to_byte();
        sub = gtm_subs.to_byte();
        value = gtm_data.to_byte();
    }

    if (debug_g > LOW) cout << name_msg << gvn << "\n";
    if (debug_g > LOW) cout << "DEBUG>> subscripts: " << sub << endl;
    if (debug_g > LOW) cout << "DEBUG>> data: " << value << endl;

    if (async) {
        Local<Function> callback = Local<Function>::Cast(args[args_count]);

        Baton *baton = new Baton();

        baton->request.data = baton;
        baton->api = "set";
        baton->callback_p.Reset(isolate, callback);
        baton->subscripts_p.Reset(isolate, subscripts);
        baton->data_p.Reset(isolate, data);
        baton->glvn = gvn;
        baton->subs = sub;
        baton->value = value;
#if YDB_SIMPLE_API == 1
        baton->subsarray = subsarray;
#endif
        baton->mode = mode_g;
        baton->status = stat_buf;
        baton->async = async;
        baton->local = local;
        baton->position = position;

        uv_queue_work(uv_default_loop(), &baton->request, async_work, async_after);

        args.GetReturnValue().Set(Undefined(isolate));
        return;
    } else {
#if YDB_SIMPLE_API == 1
        if (debug_g > LOW) cout << "DEBUG>> call into set in the SimpleAPI interface" << "\n";

        stat_buf = ydb::set(gvn, subsarray, value);
#else
        if (debug_g > LOW) cout << "DEBUG>> call into set in the Call-in interface" << "\n";

        stat_buf = gtm::set(gvn, sub, value, mode_g);
#endif

        if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

        if (stat_buf == -1) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, strerror(errno))));
            return;
        } else if (stat_buf != EXIT_SUCCESS) {
            uv_mutex_lock(&mutex);
            gtm_zstatus(msg_buffer_g, MSG_LEN);
            uv_mutex_unlock(&mutex);

            if (position) {
                isolate->ThrowException(Exception::Error(gtm_status(msg_buffer_g, position, async)->ToString()));
                args.GetReturnValue().Set(Undefined(isolate));
                return;
            } else {
                args.GetReturnValue().Set(gtm_status(msg_buffer_g, position, async));
                return;
            }
        }

        if (debug_g > LOW) cout << "DEBUG>> call into set_ret" << "\n";

        return_object = set_ret(position, local, async, gvn, subscripts, data);

        if (debug_g > OFF) cout << "DEBUG> Gtm::set exit" << endl;

        args.GetReturnValue().Set(return_object);
        return;
    }
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
        Local<Object> arg_object = args[0]->ToObject();
        glvn = arg_object->Get(String::NewFromUtf8(isolate, "global"));

        if (glvn->IsUndefined()) {
            glvn = arg_object->Get(String::NewFromUtf8(isolate, "local"));
            local = true;
        }

        if (glvn->IsUndefined()) {
            isolate->ThrowException(Exception::SyntaxError(String::NewFromUtf8(isolate, "Need to supply a 'global' or 'local' property")));
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

    const char *name_msg;
    Local<Value> name = Undefined(isolate);

    if (local) {
        if (invalid_name(*String::Utf8Value(glvn))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' is an invalid name")));
            return;
        }

        name_msg = "DEBUG>> local: ";
        name = localize_name(glvn);

        if (invalid_local(*String::Utf8Value(name))) {
            isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Property 'local' cannot begin with 'v4w'")));
            return;
        }
    } else {
        if (invalid_name(*String::Utf8Value(glvn))) {
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
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_cip(&access, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#else
    if (utf8_g == true) {
        if (debug_g > LOW) cout << name_msg << *String::Utf8Value(name) << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << *String::Utf8Value(subs) << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_unlock, *String::Utf8Value(name), *String::Utf8Value(subs), mode_g);
        uv_mutex_unlock(&mutex);
    } else {
        GtmValue gtm_name {name};
        GtmValue gtm_subs {subs};

        if (debug_g > LOW) cout << name_msg << gtm_name.to_byte() << "\n";
        if (debug_g > LOW) cout << "DEBUG>> subscripts: " << gtm_subs.to_byte() << endl;

        uv_mutex_lock(&mutex);
        stat_buf = gtm_ci(gtm_unlock, gtm_name.to_byte(), gtm_subs.to_byte(), mode_g);
        uv_mutex_unlock(&mutex);
    }
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    if (name->StrictEquals(String::NewFromUtf8(isolate, ""))) {
        Local<Value> ret_data = Undefined(isolate);

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

#if GTM_CIP_API == 1
    ci_name_descriptor access;

    access.rtn_name.address = gtm_update;
    access.rtn_name.length = 6;
    access.handle = NULL;

    uv_mutex_lock(&mutex);
    stat_buf = gtm_cip(&access, ret_buffer_g);
    uv_mutex_unlock(&mutex);
#else
    uv_mutex_lock(&mutex);
    stat_buf = gtm_ci(gtm_update, ret_buffer_g);
    uv_mutex_unlock(&mutex);
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> json_string;

    if (utf8_g == true) {
        json_string = String::NewFromUtf8(isolate, ret_buffer_g);
    } else {
        json_string = GtmValue::from_byte(ret_buffer_g);
    }

    if (debug_g > OFF) cout << "DEBUG> Gtm::update JSON string: " << *String::Utf8Value(json_string) << "\n";

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
        temp_object = json->ToObject();
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

    Local<String> nodem_version = String::NewFromUtf8(isolate, "Node.js Adaptor for " NODEM_DB ": Version: " NODEM_VERSION " (FWS)");

    if (gtm_state_g < OPEN) {
        args.GetReturnValue().Set(nodem_version);
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> call into " NODEM_DB << endl;

    gtm_status_t stat_buf;
    gtm_char_t gtm_version[] = "version";

#if GTM_CIP_API == 1
    ci_name_descriptor version;

    version.rtn_name.address = gtm_version;
    version.rtn_name.length = 7;
    version.handle = NULL;

    uv_mutex_lock(&mutex);
    stat_buf = gtm_cip(&version, ret_buffer_g);
    uv_mutex_unlock(&mutex);
#else
    uv_mutex_lock(&mutex);
    stat_buf = gtm_ci(gtm_version, ret_buffer_g);
    uv_mutex_unlock(&mutex);
#endif

    if (stat_buf != EXIT_SUCCESS) {
        uv_mutex_lock(&mutex);
        gtm_zstatus(msg_buffer_g, MSG_LEN);
        uv_mutex_unlock(&mutex);

        args.GetReturnValue().Set(gtm_status(msg_buffer_g, false, false));
        return;
    }

    if (debug_g > OFF) cout << "\nDEBUG> return from " NODEM_DB << "\n";

    Local<String> ret_string = String::NewFromUtf8(isolate, ret_buffer_g);
    Local<String> version_string = String::Concat(nodem_version, String::Concat(String::NewFromUtf8(isolate, "; "), ret_string));

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
        Gtm* object = new Gtm();
        object->Wrap(args.This());

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
 * @param {FunctionCallbackInfo<Value>&} args - A special object passed by the Node.js runtime, including passed arguments
 * @returns void
 */
void Gtm::Init(Local<Object> exports)
{
    Isolate* isolate = Isolate::GetCurrent();
    HandleScope scope(isolate);

    Local<FunctionTemplate> func_tpl = FunctionTemplate::New(isolate, Gtm::New);

    func_tpl->SetClassName(String::NewFromUtf8(isolate, "Gtm"));
    func_tpl->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(func_tpl, "about", version);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "close", close);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "data", data);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "function", function);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "get", get);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "globalDirectory", global_directory);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "global_directory", global_directory);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "help", help);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "increment", increment);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "kill", kill);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "localDirectory", local_directory);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "local_directory", local_directory);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "lock", lock);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "merge", merge);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "next", order);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "nextNode", next_node);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "next_node", next_node);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "open", open);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "order", order);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "previous", previous);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "previousNode", previous_node);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "previous_node", previous_node);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "procedure", procedure);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "retrieve", retrieve);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "routine", procedure);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "set", set);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "unlock", unlock);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "update", update);
    NODE_SET_PROTOTYPE_METHOD(func_tpl, "version", version);

    constructor_p.Reset(isolate, func_tpl->GetFunction());
    Local<Function> constructor = Local<Function>::New(isolate, constructor_p);

    exports->Set(String::NewFromUtf8(isolate, "Gtm"), constructor);
    if (strcmp(NODEM_DB, "YottaDB") == 0) exports->Set(String::NewFromUtf8(isolate, "Ydb"), constructor);
} // @end Gtm::Init method

/*
 * @MACRO {public} NODE_MODULE
 * @summary Register the mumps.node module with Node.js
 */
NODE_MODULE(mumps, Gtm::Init)

} // @end nodem namespace
