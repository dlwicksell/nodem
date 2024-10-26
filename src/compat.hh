/*
 * Package:    NodeM
 * File:       compat.hh
 * Summary:    Compatiblity macros and functions to manage/abstract V8 changes
 * Maintainer: David Wicksell <dlw@linux.com>
 *
 * Written by David Wicksell <dlw@linux.com>
 * Copyright © 2020-2024 Fourth Watch Software LC
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

#ifndef COMPAT_HH
#   define COMPAT_HH

#if NODE_MAJOR_VERSION >= 9
#   define UTF8_VALUE_TEMP_N(isolate, value) v8::String::Utf8Value(v8::Isolate::GetCurrent(), value)
#   define UTF8_VALUE_N(isolate, name, value) v8::String::Utf8Value name(v8::Isolate::GetCurrent(), value)
#else
#   define UTF8_VALUE_TEMP_N(isolate, value) v8::String::Utf8Value(value)
#   define UTF8_VALUE_N(isolate, name, value) v8::String::Utf8Value name(value)
#endif

namespace nodem {

/*
 * @template {private} nodem::get_n
 * @summary Return the value of a V8 object property
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {class Object&} object - The V8 object
 * @param {class Property&} property - The V8 object property
 * @returns {Local<Value>} - The V8 property value
 */
template <class Object, class Property>
inline static v8::Local<v8::Value> get_n(v8::Isolate* isolate, const Object& object, const Property& property)
{
#if NODE_MAJOR_VERSION >= 3
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);

    v8::MaybeLocal<v8::Value> maybe_property = object->Get(isolate->GetCurrentContext(), property);

    if (maybe_property.IsEmpty() || try_catch.HasCaught()) {
        isolate->ThrowException(try_catch.Exception());
        try_catch.Reset();

        return v8::Null(isolate);
    } else {
        return maybe_property.ToLocalChecked();
    }
#else
    return object->Get(property);
#endif
} // @end nodem::get_n template function

/*
 * @template {private} nodem::set_n
 * @summary Set the value of a V8 object property
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {class Object&} object - The V8 object
 * @param {class Property&} property - The V8 object property
 * @param {class Value&} value - The V8 property value
 * @returns {bool} - Whether the V8 property was set or not
 */
template <class Object, class Property, class Value>
inline static bool set_n(v8::Isolate* isolate, const Object& object, const Property& property, const Value& value)
{
#if NODE_MAJOR_VERSION >= 7
    return object->Set(isolate->GetCurrentContext(), property, value).ToChecked();
#else
    return object->Set(property, value);
#endif
} // @end nodem::set_n template function

/*
 * @template {private} nodem::delete_n
 * @summary Delete the value of a V8 object property
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {class Object&} object - The V8 object
 * @param {class Property&} property - The V8 object property
 * @returns {bool} - Whether the V8 property was deleted or not
template <class Object, class Property>
inline static bool delete_n(v8::Isolate* isolate, const Object& object, const Property& property)
{
#if NODE_MAJOR_VERSION >= 7
    return object->Delete(isolate->GetCurrentContext(), property).ToChecked();
#else
    return object->Delete(property);
#endif
} // @end nodem::template_n template function
 */

/*
 * @function {private} nodem::new_string_n
 * @summary Create a new V8 string
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {const char*} value - The value to set the string
 * @returns {Local<String>} - The new V8 string
 */
inline static v8::Local<v8::String> new_string_n(v8::Isolate* isolate, const char* value)
{
#if NODE_MAJOR_VERSION >= 3
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);

    v8::MaybeLocal<v8::String> maybe_string = v8::String::NewFromUtf8(isolate, value, v8::NewStringType::kNormal);

    if (maybe_string.IsEmpty() || try_catch.HasCaught()) {
        isolate->ThrowException(try_catch.Exception());
        try_catch.Reset();

        return v8::String::Empty(isolate);
    } else {
        return maybe_string.ToLocalChecked();
    }
#else
    return v8::String::NewFromUtf8(isolate, value);
#endif
} // @end nodem::new_string_n function

/*
 * @function {private} nodem::concat_n
 * @summary Create a new V8 string by concatenating two other V8 strings together
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<String>&} first - The first V8 string to concatenate
 * @param {Local<String>&} second - The second V8 string to concatenate
 * @returns {Local<String>} - The new V8 string
 */
inline static v8::Local<v8::String> concat_n(v8::Isolate* isolate, const v8::Local<v8::String>& first,
  const v8::Local<v8::String>& second)
{
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 12)
    return v8::String::Concat(isolate, first, second);
#else
    return v8::String::Concat(first, second);
#endif
} // @end nodem::concat_n function

/*
 * @function {private} nodem::has_n
 * @summary Test for the existence of a property in a V8 object
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Object>&} object - The object to test
 * @param {Local<String>&} property - The property to test for
 * @returns {bool} - Whether the V8 object contains the property or not
 */
inline static bool has_n(v8::Isolate* isolate, const v8::Local<v8::Object>& object, const v8::Local<v8::String>& property)
{
#if NODE_MAJOR_VERSION >= 7
    return object->Has(isolate->GetCurrentContext(), property).ToChecked();
#else
    return object->Has(property);
#endif
} // @end nodem::has_n function

/*
 * @function {private} nodem::to_string_n
 * @summary Convert a V8 value to a V8 string
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Value>&} value - The V8 value to set the string to
 * @returns {Local<String>} - The converted V8 string
 */
inline static v8::Local<v8::String> to_string_n(v8::Isolate* isolate, const v8::Local<v8::Value>& value)
{
#if NODE_MAJOR_VERSION >= 9
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);

    v8::MaybeLocal<v8::String> maybe_string = value->ToString(isolate->GetCurrentContext());

    if (maybe_string.IsEmpty() || try_catch.HasCaught()) {
        isolate->ThrowException(try_catch.Exception());
        try_catch.Reset();

        return v8::String::Empty(isolate);
    } else {
        return maybe_string.ToLocalChecked();
    }
#else
    return value->ToString();
#endif
} // @end nodem::to_string_n function

/*
 * @function {private} nodem::to_number_n
 * @summary Convert a V8 value to a V8 number
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Value>&} value - The V8 value to set the number to
 * @returns {Local<Number>} - The converted V8 string
 */
inline static v8::Local<v8::Number> to_number_n(v8::Isolate* isolate, const v8::Local<v8::Value>& value)
{
#if NODE_MAJOR_VERSION >= 6
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);

    v8::MaybeLocal<v8::Number> maybe_number = value->ToNumber(isolate->GetCurrentContext());

    if (maybe_number.IsEmpty() || try_catch.HasCaught()) {
        isolate->ThrowException(try_catch.Exception());
        try_catch.Reset();

        return v8::Number::New(isolate, 0);
    } else {
        return maybe_number.ToLocalChecked();
    }
#else
    return value->ToNumber();
#endif
} // @end nodem::to_number_n function

/*
 * @function {private} nodem::to_object_n
 * @summary Convert a V8 value to a V8 object
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Value>&} value - The value to set the object to
 * @returns {Local<Object>} - The converted V8 object
 */
inline static v8::Local<v8::Object> to_object_n(v8::Isolate* isolate, const v8::Local<v8::Value>& value)
{
#if NODE_MAJOR_VERSION >= 9
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);

    v8::MaybeLocal<v8::Object> maybe_object = value->ToObject(isolate->GetCurrentContext());

    if (maybe_object.IsEmpty() || try_catch.HasCaught()) {
        isolate->ThrowException(try_catch.Exception());
        try_catch.Reset();

        return v8::Object::New(isolate);
    } else {
        return maybe_object.ToLocalChecked();
    }
#else
    return value->ToObject();
#endif
} // @end nodem::to_object_n function

/*
 * @function {private} nodem::boolean_value_n
 * @summary Convert a V8 value to a bool
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Value>} value - The value to set as a bool
 * @returns {bool} - The new bool
 */
inline static bool boolean_value_n(v8::Isolate* isolate, const v8::Local<v8::Value>& value)
{
#if NODE_MAJOR_VERSION >= 12
    return value->BooleanValue(isolate);
#elif NODE_MAJOR_VERSION >= 7
    return value->BooleanValue(isolate->GetCurrentContext()).ToChecked();
#else
    return value->BooleanValue();
#endif
} // @end nodem::boolean_value_n function

/*
 * @function {private} nodem::number_value_n
 * @summary Convert a V8 value to a double
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Value>} value - The value to set as a double
 * @returns {double} - The new double
 */
inline static double number_value_n(v8::Isolate* isolate, const v8::Local<v8::Value>& value)
{
#if NODE_MAJOR_VERSION >= 7
    return value->NumberValue(isolate->GetCurrentContext()).ToChecked();
#else
    return value->NumberValue();
#endif
} // @end nodem::number_value_n function

/*
 * @function {private} nodem::uint32_value_n
 * @summary Convert a V8 value to an unsigned int
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Value>} value - The value to set as an unsigned int
 * @returns {unsigned int} - The new unsigned int
 */
inline static unsigned int uint32_value_n(v8::Isolate* isolate, const v8::Local<v8::Value>& value)
{
#if NODE_MAJOR_VERSION >= 7
    return value->Uint32Value(isolate->GetCurrentContext()).ToChecked();
#else
    return value->Uint32Value();
#endif
} // @end nodem::uint32_value_n function

/*
 * @function {private} nodem::utf8_length_n
 * @summary Return the length of a UTF8 string
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<String>} string - The string whose length is returned
 * @returns {int} - The length of the V8 string
 */
inline static int utf8_length_n(v8::Isolate* isolate, const v8::Local<v8::String>& string)
{
#if NODE_MAJOR_VERSION >= 11 || (NODE_MAJOR_VERSION == 10 && NODE_MINOR_VERSION >= 12)
    return string->Utf8Length(isolate);
#else
    return string->Utf8Length();
#endif
} // @end nodem::utf8_length_n function

/*
 * @function {private} nodem::call_n
 * @summary Call a V8/JavaScript function or method
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<Function>&} method - The JavaScript function or method to call
 * @param {Local<Value>&} json - The Node.js JSON object from the global context, or a V8 Null
 * @param {const int} num - The number of arguments to pass to the JavaScript function or method
 * @param {Local<Value>*} data - The arguments to pass to the JavaScript function or method
 * @returns {Local<String>} - The return value from the function or method invocation
 */
inline static v8::Local<v8::Value> call_n(v8::Isolate* isolate, const v8::Local<v8::Function>& method,
  const v8::Local<v8::Value>& json, const int num, v8::Local<v8::Value>* data)
{
#if NODE_MAJOR_VERSION >= 3
    v8::TryCatch try_catch(isolate);
    try_catch.SetVerbose(true);

    v8::MaybeLocal<v8::Value> maybe_value = method->Call(isolate->GetCurrentContext(), json, num, data);

    if (maybe_value.IsEmpty() || try_catch.HasCaught()) {
        isolate->ThrowException(try_catch.Exception());
        try_catch.Reset();

        return v8::Null(isolate);
    } else {
        return maybe_value.ToLocalChecked();
    }
#else
    return method->Call(json, num, data);
#endif
} // @end nodem::call_n function

/*
 * @function {private} nodem::set_prototype_method_n
 * @summary Add Nodem class methods and external per-thread data to the Gtm/Ydb JavaScript function prototypes
 * @param {Isolate*} isolate - The current V8 isolate
 * @param {Local<FunctionTemplate>} func_template - The function template
 * @param {const char*} method_name - The Nodem class method name
 * @param {FunctionCallback} method_callback - The Nodem class method implementation
 * @param {Local<Value>} external_data - The external per-thread data struct to add to the prototype
 * @returns {void}
 */
inline static void set_prototype_method_n(v8::Isolate* isolate, v8::Local<v8::FunctionTemplate> func_template,
  const char* method_name, v8::FunctionCallback method_callback, v8::Local<v8::Value> external_data)
{
    v8::Local<v8::Signature> signature = v8::Signature::New(isolate, func_template);
    v8::Local<v8::FunctionTemplate> tpl = v8::FunctionTemplate::New(isolate, method_callback, external_data, signature);
    v8::Local<v8::String> func = new_string_n(isolate, method_name);

    tpl->SetClassName(func);
    func_template->PrototypeTemplate()->Set(func, tpl);

    return;
} // @end nodem::set_prototype_method_n function

} // @end namespace nodem

#endif // @end COMPAT_HH
