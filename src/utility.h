/*
 * Package:    NodeM
 * File:       utility.h
 * Summary:    Utility functions and macros to manage V8 changes
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2019 Fourth Watch Software LC
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

#ifndef UTILITY_H
#define UTILITY_H

#include <node.h>

namespace nodem {

#if NODE_MAJOR_VERSION >= 9
    #define UTF8_VALUE_TEMP_N(isolate, value) v8::String::Utf8Value(v8::Isolate::GetCurrent(), value)
    #define UTF8_VALUE_N(isolate, name, value) v8::String::Utf8Value name(v8::Isolate::GetCurrent(), value)
#else
    #define UTF8_VALUE_TEMP_N(isolate, value) v8::String::Utf8Value(value)
    #define UTF8_VALUE_N(isolate, name, value) v8::String::Utf8Value name(value)
#endif

inline v8::Local<v8::String> concat_n(v8::Isolate *isolate, v8::Local<v8::String> first, v8::Local<v8::String> second)
{
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 12
    return v8::String::Concat(isolate, first, second);
#else
    return v8::String::Concat(first, second);
#endif
} // @end concat_n function

inline v8::Local<v8::Value> call_n(v8::Isolate *isolate, v8::Local<v8::Function> method,
  v8::Local<v8::Value> json, int num, v8::Local<v8::Value>* data)
{
#if NODE_MAJOR_VERSION >= 3
    return method->Call(isolate->GetCurrentContext(), json, num, data).ToLocalChecked();
#else
    return method->Call(json, num, data);
#endif
} // @end call_n function

inline v8::Local<v8::Function> get_function_n(v8::Isolate *isolate, v8::Local<v8::FunctionTemplate> function)
{
#if NODE_MAJOR_VERSION >= 3
    return function->GetFunction(isolate->GetCurrentContext()).ToLocalChecked();
#else
    return function->GetFunction();
#endif
} // @end get_function_n function

inline bool has_n(v8::Isolate *isolate, v8::Local<v8::Object> object, v8::Local<v8::String> string)
{
#if NODE_MAJOR_VERSION >= 7
    return object->Has(isolate->GetCurrentContext(), string).ToChecked();
#else
    return object->Has(string);
#endif
} // @end has_n function

inline v8::Local<v8::String> to_string_n(v8::Isolate *isolate, v8::Local<v8::Value> name)
{
#if NODE_MAJOR_VERSION >= 9
    return name->ToString(isolate->GetCurrentContext()).ToLocalChecked();
#else
    return name->ToString();
#endif
} // @end to_string_n function

inline v8::Local<v8::Number> to_number_n(v8::Isolate *isolate, v8::Local<v8::Value> name)
{
#if NODE_MAJOR_VERSION >= 7 || NODE_MAJOR_VERSION == 6 && NODE_MINOR_VERSION >= 8
    return name->ToNumber(isolate->GetCurrentContext()).ToLocalChecked();
#else
    return name->ToNumber();
#endif
} // @end to_number_n function

inline v8::Local<v8::Object> to_object_n(v8::Isolate *isolate, v8::Local<v8::Value> name)
{
#if NODE_MAJOR_VERSION >= 9
    return name->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
#else
    return name->ToObject();
#endif
} // @end to_object_n function

inline bool boolean_value_n(v8::Isolate *isolate, v8::Local<v8::Value> value)
{
#if NODE_MAJOR_VERSION >= 12
    return value->BooleanValue(isolate);
#elif NODE_MAJOR_VERSION >= 7
    return value->BooleanValue(isolate->GetCurrentContext()).ToChecked();
#else
    return value->BooleanValue();
#endif
} // @end boolean_value_n function

inline int number_value_n(v8::Isolate *isolate, v8::Local<v8::Value> value)
{
#if NODE_MAJOR_VERSION >= 7
    return value->NumberValue(isolate->GetCurrentContext()).ToChecked();
#else
    return value->NumberValue();
#endif
} // @end number_value_n function

inline unsigned int uint32_value_n(v8::Isolate *isolate, v8::Local<v8::Value> value)
{
#if NODE_MAJOR_VERSION >= 7
    return value->Uint32Value(isolate->GetCurrentContext()).ToChecked();
#else
    return value->Uint32Value();
#endif
} // @end uint32_value_n function

inline int utf8_length_n(v8::Isolate *isolate, v8::Local<v8::String> string)
{
#if NODE_MAJOR_VERSION >= 11 || NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 12
    return string->Utf8Length(isolate);
#else
    return string->Utf8Length();
#endif
} // @end utf8_length_n function

} // @end namespace nodem

#endif // @end UTILITY_H
