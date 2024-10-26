v4wNode() ; 0.20.9 ; Oct 25, 2024@20:14
 ;
 ; Package:    NodeM
 ; File:       v4wNode.m
 ; Summary:    Call-in integration routine
 ; Maintainer: David Wicksell <dlw@linux.com>
 ;
 ; Written by David Wicksell <dlw@linux.com>
 ; Copyright © 2012-2024 Fourth Watch Software LC
 ;
 ; This program is free software: you can redistribute it and/or modify it
 ; under the terms of the GNU Affero General Public License (AGPL) as published
 ; by the Free Software Foundation, either version 3 of the License, or (at
 ; your option) any later version.
 ;
 ; This program is distributed in the hope that it will be useful, but WITHOUT
 ; ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 ; FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 ; for more details.
 ;
 ; You should have received a copy of the GNU Affero General Public License
 ; along with this program. If not, see http://www.gnu.org/licenses/.
 ;
 ;
 ; NOTE: Although this routine can be called directly, it is not a good idea; it
 ; is hard to use, and very clunky. It is written with the sole purpose of
 ; providing software integration between NodeM's C code and M code, via the
 ; YottaDB/GT.M Call-in interface. Although each function is documented, that is
 ; for internal maintenance and testing purposes, and there are no plans for any
 ; API usage documentation in the future.
 ;
 quit:$quit "Call an API entry point" write "Call an API entry point",! quit
 ;
 ;; @function {private} isNumber
 ;; @summary Returns true if data is number, false if not
 ;; @param {string} data - Input data to be tested; a single subscript, function or procedure argument, or data
 ;; @param {string} direction (input|output) - Processing control direction
 ;; @returns {number} (0|1) - Return code representing data type
isNumber:(data,direction)
 ; YottaDB/GT.M approximate (using number of digits, rather than number value) number limits:
 ;   - 47 digits before overflow (resulting in an overflow error)
 ;   - 18 digits of precision
 ; Node.js/JavaScript approximate (using number of digits, rather than number value) number limits:
 ;   - 309 digits before overflow (represented as the Infinity primitive)
 ;   - 21 digits before conversion to exponent notation
 ;   - 16 digits of precision
 if $get(v4wDebug,0)>2 do debugLog(">>>    isNumber enter:") zwrite data,direction use $principal
 ;
 if data'["E",data=+data do:$get(v4wDebug,0)>2 debugLog(">>>    isNumber exit: 1") quit 1
 else  if direction="input",data?.1"-"1"0"1"."1.N  do:$get(v4wDebug,0)>2 debugLog(">>>    isNumber exit: 1") quit 1
 else  do:$get(v4wDebug,0)>2 debugLog(">>>    isNumber exit: 0") quit 0
 ;; @end isNumber function
 ;
 ;; @function {private} isString
 ;; @summary Returns true if data is string (including whether it has surrounding quotes), false if not
 ;; @param {string} data - Input data to be tested; a single subscript, function or procedure argument, or data
 ;; @param {string} direction (input|output) - Processing control direction
 ;; @returns {number} (0|1|2|3) - Return code representing data type
isString:(data,direction)
 ; The note under isNumber is why anything over 16 characters needs to be treated as a string
 if $get(v4wDebug,0)>2 do debugLog(">>>    isString enter:") zwrite data,direction use $principal
 ;
 if ($zextract(data)="""")&($zextract(data,$zlength(data))="""") do:$get(v4wDebug,0)>2 debugLog(">>>    isString exit: 3") quit 3
 else  if $zlength(data)>16 do:$get(v4wDebug,0)>2 debugLog(">>>    isString exit: 1") quit 1
 else  if (direction="input")&((data["e+")!(data["e-")) do:$get(v4wDebug,0)>2 debugLog(">>>    isString exit: 1") quit 1
 else  if $$isNumber(data,direction) do:$get(v4wDebug,0)>2 debugLog(">>>    isString exit: 0") quit 0
 else  do:$get(v4wDebug,0)>2 debugLog(">>>    isString exit: 2") quit 2
 ;; @end isString function
 ;
 ;; @function {private} construct
 ;; @summary Construct a full global reference fit for use by indirection
 ;; @param {string} name - Global or local variable name, or function or procedure name
 ;; @param {string} args - Subscripts or arguments as a comma-separated list, empty string if none
 ;; @returns {string} - Global or local reference ready to be used by indirection
construct:(name,args)
 if $get(v4wDebug,0)>2 do debugLog(">>>    construct:") zwrite name,args use $principal
 quit name_$select(args'="":"("_args_")",1:"")
 ;; @end construct function
 ;
 ;; @function {private} constructFunction
 ;; @summary Construct a full function or procedure reference to get around the 8192 indirection limit
 ;; @param {string} func - Function or procedure name
 ;; @param {string} args - Arguments as a comma-separated list, empty string if none
 ;; @param {reference} {(string|number)[]} tempArgs - Temporary argument array, used to get around the indirection limit
 ;; @returns {string} function - Function or procedure reference ready to be used by indirection
constructFunction:(func,args,tempArgs)
 if args="" quit func
 if $get(v4wDebug,0)>2 do debugLog(">>>    constructFunction enter:") zwrite func,args,tempArgs use $principal
 ;
 new global,function
 set global="^global("_args_")",function=""
 ;
 new i
 for i=1:1:$qlength(global) do
 . set tempArgs(i)=$qsubscript(global,i)
 . set function=function_",v4wTempArgs("_i_")"
 ;
 set $zextract(function)=""
 set function=func_"("_function_")"
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    constructFunction exit:") zwrite data use $principal
 quit function
 ;; @end constructFunction function
 ;
 ;; @function {private} inputConvert
 ;; @summary Convert input data coming from Node.js for use with M
 ;; @param {string} data - Input data to be converted; a single subscript, function or procedure argument, or data
 ;; @param {number} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @param {number} type (0|1) - Data type; 0 is subscripts or arguments, 1 is data node
 ;; @returns {string} data - Converted input; a number or string ready to access M
inputConvert:(data,mode,type)
 if $get(v4wDebug,0)>2 do debugLog(">>>    inputConvert enter:") zwrite data,mode,type use $principal
 ;
 if mode,'$$isString(data,"input") do
 . if $zextract(data,1,2)="0." set $zextract(data)=""
 . else  if $zextract(data,1,3)="-0." set $zextract(data,2)=""
 else  if type,$$isString(data,"input")=3 set $zextract(data)="",$zextract(data,$zlength(data))=""
 else  if 'type,$$isString(data,"input")<2,data'="" set data=""""_data_""""
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    inputConvert exit:") zwrite data use $principal
 quit data
 ;; @end inputConvert function
 ;
 ;; @function {private} inputEscape
 ;; @summary Escape input data coming from Node.js for use with M
 ;; @param {string} data - Input data to be escaped; a single subscript, function or procedure argument, or data
 ;; @param {number} type (0|1) - Data type; 0 is subscripts or arguments, 1 is data node
 ;; @returns {string} data - Escaped input; a string with quotes ready to access M
inputEscape:(data,type)
 if $get(v4wDebug,0)>2 do debugLog(">>>    inputEscape enter:") zwrite data,type use $principal
 ;
 if 'type,data["""" do
 . new newData
 . set newData=""
 . ;
 . new i
 . for i=2:1:$zlength(data)-1 do
 . . if $zextract(data,i)="""" set newData=newData_""""_$zextract(data,i)
 . . else  set newData=newData_$zextract(data,i)
 . set data=$zextract(data)_newData_$zextract(data,$zlength(data))
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    inputEscape exit:") zwrite data use $principal
 quit data
 ;; @end inputEscape function
 ;
 ;; @function {private} outputConvert
 ;; @summary Convert output data coming from M for use with Node.js
 ;; @param {string} data - Output data to be converted; a single subscript, function or procedure argument, or data
 ;; @param {number} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} data - Converted output; a number or string ready to return to Node.js
outputConvert:(data,mode)
 if $get(v4wDebug,0)>2 do debugLog(">>>    outputConvert enter:") zwrite data,mode use $principal
 ;
 if mode,'$$isString(data,"output") do
 . if $zextract(data)="." set data=0_data
 . else  if $zextract(data,1,2)="-." set $zextract(data)="",data="-0"_data
 else  set data=""""_data_""""
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    outputConvert exit:") zwrite data use $principal
 quit data
 ;; @end outputConvert function
 ;
 ;; @function {private} outputEscape
 ;; @summary Escape output data coming from M for use with Node.js
 ;; @param {string} data - Output data to be escaped; a single subscript, function or procedure argument, or data
 ;; @returns {string} data - Escaped output; a string with quotes ready to return to Node.js
outputEscape:(data)
 if $get(v4wDebug,0)>2 do debugLog(">>>    outputEscape enter:") zwrite data use $principal
 ;
 if (data["""")!(data["\")!(data?.e1c.e) do
 . new newData
 . set newData=""
 . ;
 . new i,charHigh,charLow
 . for i=1:1:$zlength(data) do
 . . if ($zextract(data,i)="""")!($zextract(data,i)="\") do
 . . . set newData=newData_"\"_$zextract(data,i)
 . . else  if $zextract(data,i)?1c,$zascii($zextract(data,i))'>127 do
 . . . set charHigh=$zascii($zextract(data,i))\16,charHigh=$zextract("0123456789abcdef",charHigh+1)
 . . . set charLow=$zascii($zextract(data,i))#16,charLow=$zextract("0123456789abcdef",charLow+1)
 . . . set newData=newData_"\u00"_charHigh_charLow
 . . else  set newData=newData_$zextract(data,i)
 . ;
 . set data=newData
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    outputEscape exit:") zwrite data use $principal
 quit data
 ;; @end outputEscape function
 ;
 ;; @label {private} parse
 ;; @summary Transform an encoded string (subscripts or arguments) in to an M array
 ;; @param {string} inputString - Input string to be transformed
 ;; @param {reference} {(string|number)[]} outputArray - Output array to be returned
 ;; @param {number} encode (0|1) - Whether subscripts or arguments are encoded, 0 is no, 1 is yes
 ;; @returns {void}
parse:(inputString,outputArray,encode)
 if $get(v4wDebug,0)>2 do debugLog(">>>    parse enter:") zwrite inputString,encode use $principal
 ;
 if inputString="" set outputArray(1)="" do  quit
 . if $get(v4wDebug,0)>2 do debugLog(">>>    parse exit:") zwrite outputArray use $principal
 ;
 kill outputArray
 if encode do
 . new i,length
 . for i=1:1 quit:inputString=""  do
 . . set length=+inputString
 . . set $zextract(inputString,1,$zlength(length)+1)=""
 . . set outputArray(i)=$zextract(inputString,1,length)
 . . set $zextract(inputString,1,length+1)=""
 else  do
 . set outputArray(1)=inputString
 . set inputString=""
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    parse exit:") zwrite:$data(outputArray) outputArray use $principal
 quit
 ;; @end parse label
 ;
 ;; @label {private} stringify
 ;; @summary Transform an M array containing subscripts, arguments, or data, in to a string ready for use by the APIs
 ;; @param {(string|number)[]} inputArray - Input array to be transformed
 ;; @param {reference} {string} outputString - Output string to be returned
 ;; @returns {void}
stringify:(inputArray,outputString)
 if $get(v4wDebug,0)>2 do debugLog(">>>    stringify enter:") zwrite inputArray use $principal
 ;
 if $data(inputArray)<10 set outputString="" do  quit
 . if $get(v4wDebug,0)>2 do debugLog(">>>    stringify exit:") zwrite outputString use $principal
 ;
 new num
 set num=0,outputString=""
 for  set num=$order(inputArray(num)) quit:num=""  set outputString=outputString_","_inputArray(num)
 set $zextract(outputString)=""
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    stringify exit:") zwrite outputString use $principal
 quit
 ;; @end stringify label
 ;
 ;; @function {private} process
 ;; @summary Process an encoded string of subscripts, arguments, or an unencoded data node
 ;; @param {string} inputString - Input string to be transformed
 ;; @param {string} direction (input|output) - Processing control direction
 ;; @param {number} mode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @param {number} type (0|1) - Data type; 0 is subscripts or arguments, 1 is data node
 ;; @param {number} encode (0|1) - Whether subscripts or arguments are encoded, 0 is no, 1 is yes
 ;; @returns {string} outputString - Output string ready for use by the APIs
process:(inputString,direction,mode,type,encode)
 set mode=$get(mode,1)
 set type=$get(type,0)
 set encode=$get(encode,1)
 if $get(v4wDebug,0)>2 do debugLog(">>>    process enter:") zwrite inputString,direction,mode,type,encode use $principal
 ;
 new outputString
 set outputString=""
 if inputString="" do  quit outputString
 . if type set outputString=""""""
 . if $get(v4wDebug,0)>2 do debugLog(">>>    process exit:") zwrite outputString use $principal
 ;
 new array
 do parse(inputString,.array,encode)
 ;
 new num
 set num=0
 for  set num=$order(array(num)) quit:num=""  do
 . if direction="input" do
 . . set array(num)=$$inputEscape(array(num),type)
 . . set array(num)=$$inputConvert(array(num),mode,type)
 . else  if direction="output" do
 . . set array(num)=$$outputEscape(array(num))
 . . set array(num)=$$outputConvert(array(num),mode)
 ;
 do stringify(.array,.outputString)
 ;
 if $get(v4wDebug,0)>2 do debugLog(">>>    process exit:") zwrite outputString use $principal
 quit outputString
 ;; @end process function
 ;
 ;; @label {private} debugLog
 ;; @summary Output debug logging messages
 ;; @param {string} msg - Message to log
 ;; @returns {void}
debugLog:(msg)
 write "[M "_$job_"] DEBUG"_$get(msg),! use $principal
 ;
 quit
 ;; @end debugLog label
 ;
 ;; ***Begin Integration APIs***
 ;;
 ;; These APIs are part of the integration code, called by the C Call-in interface (gtm_cip or gtm_ci)
 ;; They may be called from M code directly, for unit testing
 ;
 ;; @label debug
 ;; @summary Set debugging level, defaults to off
 ;; @param {number} level (0|1|2|3) - Debugging level, 0 is off, 1 is low, 2 is medium, 3 is high
 ;; @returns {void}
debug(level)
 ; Set principal device during Nodem::open call, for proper signal handling
 use $principal:ctrap=$zchar(3) ; Catch SIGINT and pass to nodem.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wDebug=$get(level,0)
 quit
 ;; @end debug label
 ;
 ;; @function version
 ;; @summary Return the about/version string
 ;; @param {string} v4wVersion - Nodem version string from nodem.hh/nodem.cc used to confirm correct integration
 ;; @returns {string} - The YottaDB or GT.M version
version(v4wVersion)
 ; Handle $zyrelease not existing in this M implementation version (150373074: %GTM-E-INVSVN, Invalid special variable name)
 set $ecode=""
 set $etrap="if $ecode["",Z150373074,"" set $ecode="""",$etrap="""" quit ""GT.M Version: ""_v4wGtmVersion"
 ;
 set v4wVersion=$get(v4wVersion,"UNKNOWN")
 if $get(v4wDebug,0)>1 do debugLog(">>   version enter") zwrite v4wVersion use $principal
 ;
 new v4wNodeVersion
 set v4wNodeVersion=$piece($text(^v4wNode)," ; ",2)
 ;
 if $get(v4wDebug,0)>0&(v4wVersion'="UNKNOWN") do
 . if v4wVersion=v4wNodeVersion do debugLog(">  NodeM version "_v4wVersion_" matches v4wNode version "_v4wNodeVersion)
 . else  do debugLog(">  NodeM version "_v4wVersion_" does not match v4wNode version "_v4wNodeVersion)
 ;
 new v4wGtmVersion
 set v4wGtmVersion=$zpiece($zversion," ",2),$zextract(v4wGtmVersion)=""
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   version exit")
 ;
 new v4wYdbVersion
 set v4wYdbVersion=$zpiece($zyrelease," ",2),$zextract(v4wYdbVersion)=""
 ;
 quit "YottaDB Version: "_v4wYdbVersion
 ;; @end version function
 ;
 ;; @function data
 ;; @summary Check if global or local node has data and/or children or not
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - $data value; 0 for no data nor children, 1 for data, 10 for children, 11 for data and children
data(v4wGlvn,v4wSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   data enter:") zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   data:") zwrite v4wName use $principal
 ;
 new v4wDefined
 set v4wDefined=$data(@v4wName)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   data exit:") zwrite v4wDefined use $principal
 quit "{""defined"":"_v4wDefined_"}"
 ;; @end data function
 ;
 ;; @function get
 ;; @summary Get data from a global or local node, or an intrinsic special variable
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The value of the data node, and whether it was defined or not
get(v4wGlvn,v4wSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $zextract(v4wGlvn)="$" set v4wSubs="" ; SimpleAPI ignores subscripts with ISVs, so we will too
 if $get(v4wDebug,0)>1 do debugLog(">>   get enter:") zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   get:") zwrite v4wName use $principal
 ;
 new v4wData,v4wDefined
 ;
 if $zextract(v4wName)="$" do
 . set v4wDefined=1
 . xecute "set v4wName="_v4wName
 . set v4wData=$$process(v4wName,"output",v4wMode,1,0)
 else  do
 . set v4wDefined=$data(@v4wName)#10
 . set v4wData=$$process($get(@v4wName),"output",v4wMode,1,0)
 ;
 set v4wDefined=$select(v4wDefined=1:"true",1:"false")
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   get exit:") zwrite v4wDefined,v4wData use $principal
 quit "{""defined"":"_v4wDefined_",""data"":"_v4wData_"}"
 ;; @end get function
 ;
 ;; @label set
 ;; @summary Set a global or local node, or an intrinsic special variable
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {string} v4wData - Data to store in the database node or local variable node
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {void}
set(v4wGlvn,v4wSubs,v4wData,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $zextract(v4wGlvn)="$" set v4wSubs="" ; SimpleAPI ignores subscripts with ISVs, so we will too
 if $get(v4wDebug,0)>1 do debugLog(">>   set enter:") zwrite v4wGlvn,v4wSubs,v4wData,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 set v4wData=$$process(v4wData,"input",v4wMode,1)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   set:") zwrite v4wName,v4wData use $principal
 ;
 if $zextract(v4wName)="$" do
 . xecute "set $"_$zextract(v4wName,2,$zlength(v4wName))_"="_$$process(v4wData,"output",v4wMode,1,0)
 else  do
 . set @v4wName=v4wData
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   set exit")
 quit
 ;; @end set label
 ;
 ;; @label kill
 ;; @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wType (0|1) - Whether to kill only the node, or also kill child subscripts; defaults to include children
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {void}
kill(v4wGlvn,v4wSubs,v4wType,v4wMode)
 set v4wMode=$get(v4wMode,1)
 set v4wType=$get(v4wType,0)
 if $get(v4wDebug,0)>1 do debugLog(">>   kill enter:") zwrite v4wGlvn,v4wSubs,v4wType,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   kill:") zwrite v4wName use $principal
 ;
 if v4wGlvn="" kill (v4wDebug)
 else  if v4wType zkill @v4wName
 else  kill @v4wName
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   kill exit")
 quit
 ;; @end kill label
 ;
 ;; @function merge
 ;; @summary Merge a global or local array tree to another global or local array tree
 ;; @param {string} v4wFromGlvn - Global or local variable to merge from
 ;; @param {string} v4wFromSubs - From subscripts represented as a string, encoded with subscript lengths
 ;; @param {string} v4wToGlvn - Global or local variable to merge to
 ;; @param {string} v4wToSubs - To subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The global or local variables, and subscripts from each side of the merge
merge(v4wFromGlvn,v4wFromSubs,v4wToGlvn,v4wToSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   merge enter:") zwrite v4wFromGlvn,v4wFromSubs,v4wToGlvn,v4wToSubs,v4wMode use $principal
 ;
 new v4wFromInputSubs
 set v4wFromInputSubs=$$process(v4wFromSubs,"input",v4wMode)
 ;
 new v4wFromName
 set v4wFromName=$$construct(v4wFromGlvn,v4wFromInputSubs)
 ;
 new v4wToInputSubs
 set v4wToInputSubs=$$process(v4wToSubs,"input",v4wMode)
 ;
 new v4wToName
 set v4wToName=$$construct(v4wToGlvn,v4wToInputSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   merge:") zwrite v4wFromName,v4wToName use $principal
 ;
 merge @v4wToName=@v4wFromName
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   merge exit")
 quit
 ;; @end merge function
 ;
 ;; @function order
 ;; @summary Return the next global or local node at the same level
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The next or previous data node
order(v4wGlvn,v4wSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   order enter:") zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   order:") zwrite v4wName use $principal
 ;
 new v4wResult
 set v4wResult=$order(@v4wName)
 ;
 for  quit:(v4wSubs'="")!($zextract(v4wResult,1,3)'="v4w")  set v4wResult=$order(@v4wResult)
 ;
 set v4wResult=$$process(v4wResult,"output",v4wMode,1,0)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   order exit:") zwrite v4wResult use $principal
 quit "{""result"":"_v4wResult_"}"
 ;; @end order function
 ;
 ;; @function previous
 ;; @summary Same as order, only in reverse
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - Returns the previous node, via calling order function and passing a -1 to the order argument
previous(v4wGlvn,v4wSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   previous enter:") zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   previous:") zwrite v4wName use $principal
 ;
 new v4wResult
 set v4wResult=$order(@v4wName,-1)
 ;
 for  quit:(v4wSubs'="")!($zextract(v4wResult,1,3)'="v4w")  set v4wResult=$order(@v4wResult,-1)
 ;
 set v4wResult=$$process(v4wResult,"output",v4wMode,1,0)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   previous exit:") zwrite v4wResult use $principal
 quit "{""result"":"_v4wResult_"}"
 ;; @end previous function
 ;
 ;; @function nextNode
 ;; @summary Return the next global or local node, depth first
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The next or previous data node
nextNode(v4wGlvn,v4wSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   nextNode enter:") zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   nextNode:") zwrite v4wName use $principal
 ;
 new v4wResult
 set v4wResult=$query(@v4wName)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   nextNode:") zwrite v4wResult use $principal
 ;
 new v4wDefined
 if v4wResult="" set v4wDefined=0
 else  set v4wDefined=1
 ;
 new v4wData,v4wNewSubscripts
 ;
 if v4wDefined do
 . set v4wData=$$process($get(@v4wResult),"output",v4wMode,1,0)
 . ;
 . if $zextract(v4wResult)="^" set $zextract(v4wResult)=""
 . set v4wNewSubscripts=""
 . ;
 . new i,sub
 . for i=1:1:$qlength(v4wResult) do
 . . set sub=$$process($qsubscript(v4wResult,i),"output",v4wMode,,0)
 . . set v4wNewSubscripts=v4wNewSubscripts_","_sub
 . set $zextract(v4wNewSubscripts)=""
 ;
 new v4wReturn
 set v4wReturn="{"
 ;
 if v4wDefined,v4wNewSubscripts'="" set v4wReturn=v4wReturn_"""subscripts"":["_v4wNewSubscripts_"],"
 set v4wReturn=v4wReturn_"""defined"":"_$select(v4wDefined=1:"true",1:"false")
 if v4wDefined set v4wReturn=v4wReturn_",""data"":"_v4wData_"}"
 else  set v4wReturn=v4wReturn_"}"
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   nextNode exit:") zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end nextNode function
 ;
 ;; @function previousNode
 ;; @summary Return the previous global or local node, depth first
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - Returns the previous node, via calling nextNode function and passing a -1 to the order argument
previousNode(v4wGlvn,v4wSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 ;
 ; Handle reverse $query not existing in this M implementation version (150373642: %GTM-E-RPARENMISSING, Right parenthesis expected)
 set $ecode="",$etrap="if $ecode["",Z150373642,"" set $ecode="""",$etrap="""" quit $$reverseQuery(v4wName,v4wMode)"
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   previousNode enter:") zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   previousNode:") zwrite v4wName use $principal
 ;
 new v4wResult
 set v4wResult=$query(@v4wName,-1)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   previousNode:") zwrite v4wResult use $principal
 ;
 new v4wDefined
 if v4wResult="" set v4wDefined=0
 else  set v4wDefined=1
 ;
 new v4wData,v4wNewSubscripts
 ;
 if v4wDefined do
 . set v4wData=$$process($get(@v4wResult),"output",v4wMode,1,0)
 . ;
 . if $zextract(v4wResult)="^" set $zextract(v4wResult)=""
 . set v4wNewSubscripts=""
 . ;
 . new i,sub
 . for i=1:1:$qlength(v4wResult) do
 . . set sub=$$process($qsubscript(v4wResult,i),"output",v4wMode,,0)
 . . set v4wNewSubscripts=v4wNewSubscripts_","_sub
 . set $zextract(v4wNewSubscripts)=""
 ;
 new v4wReturn
 set v4wReturn="{"
 ;
 if v4wDefined,v4wNewSubscripts'="" set v4wReturn=v4wReturn_"""subscripts"":["_v4wNewSubscripts_"],"
 set v4wReturn=v4wReturn_"""defined"":"_$select(v4wDefined=1:"true",1:"false")
 if v4wDefined set v4wReturn=v4wReturn_",""data"":"_v4wData_"}"
 else  set v4wReturn=v4wReturn_"}"
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   previousNode exit:") zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end previousNode function
 ;
 ;; @function reverseQuery
 ;; @summary Return the previous global or local node, depth first
 ;; @param {string} v4wName - Global or local query reference
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - Returns the previous node
reverseQuery(v4wName,v4wMode)
 if $get(v4wDebug,0)>1 do debugLog(">>   reverseQuery enter:") zwrite v4wName,v4wMode use $principal
 ;
 new v4wFlag,v4wResult
 set v4wFlag=0,v4wResult=v4wName
 set:$qlength(v4wResult)=0 v4wResult=""
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   reverseQuery:") zwrite v4wResult use $principal
 ;
 new v4wI,v4wPrevious,v4wSave
 for v4wI=1:1:$qlength(v4wName) quit:v4wFlag  do
 . set v4wPrevious=$order(@v4wResult,-1)
 . set v4wResult=$name(@v4wResult,$qlength(v4wResult)-1)
 . ;
 . if v4wPrevious'="" do  set v4wFlag=1
 . . set v4wResult=$name(@v4wResult@(v4wPrevious))
 . . ;
 . . for  set v4wSave=v4wResult,v4wResult=$query(@v4wResult) quit:v4wResult=""  quit:$name(@v4wResult)=$name(@v4wName)
 . . set v4wResult=v4wSave
 . else  if $data(@v4wResult)#2 set v4wFlag=1
 if $qlength(v4wResult)=0,$data(@v4wResult)#2=0 set v4wResult=""
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   reverseQuery:") zwrite v4wResult use $principal
 ;
 new v4wDefined
 if v4wResult="" set v4wDefined=0
 else  set v4wDefined=1
 ;
 new v4wData,v4wNewSubscripts
 ;
 if v4wDefined do
 . set v4wData=$$process($get(@v4wResult),"output",v4wMode,1,0)
 . ;
 . if $zextract(v4wResult)="^" set $zextract(v4wResult)=""
 . set v4wNewSubscripts=""
 . ;
 . new i,sub
 . for i=1:1:$qlength(v4wResult) do
 . . set sub=$$process($qsubscript(v4wResult,i),"output",v4wMode,,0)
 . . set v4wNewSubscripts=v4wNewSubscripts_","_sub
 . set $zextract(v4wNewSubscripts)=""
 ;
 new v4wReturn
 set v4wReturn="{"
 ;
 if v4wDefined,v4wNewSubscripts'="" set v4wReturn=v4wReturn_"""subscripts"":["_v4wNewSubscripts_"],"
 set v4wReturn=v4wReturn_"""defined"":"_$select(v4wDefined=1:"true",1:"false")
 if v4wDefined set v4wReturn=v4wReturn_",""data"":"_v4wData_"}"
 else  set v4wReturn=v4wReturn_"}"
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   reverseQuery exit:") zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end reverseQuery function
 ;
 ;; @function increment
 ;; @summary Increment or decrement the number in a global or local node
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wIncr - The number to increment/decrement, defaults to 1
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The new value of the data node that was incremented/decremented
increment(v4wGlvn,v4wSubs,v4wIncr,v4wMode)
 set v4wIncr=$get(v4wIncr,1)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   increment enter:") zwrite v4wGlvn,v4wSubs,v4wIncr,v4wMode use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   increment:") zwrite v4wName use $principal
 ;
 new v4wData
 set v4wData=$$process($increment(@v4wName,v4wIncr),"output",v4wMode,1,0)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   increment exit:") zwrite v4wData use $principal
 quit "{""data"":"_v4wData_"}"
 ;; @end increment function
 ;
 ;; @function lock
 ;; @summary Lock a global or local node, incrementally
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wTimeout - The time to wait for the lock, or -1 to wait forever
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {string} {JSON} - Returns whether the lock was acquired or not
lock(v4wGlvn,v4wSubs,v4wTimeout,v4wMode)
 set v4wTimeout=$get(v4wTimeout,-1)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   lock enter:") zwrite v4wGlvn,v4wSubs,v4wTimeout,v4wMode use $principal
 ;
 new v4wInputSubs
 set v4wInputSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wInputSubs)
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   lock:") zwrite v4wName use $principal
 ;
 new v4wResult
 set v4wResult=0
 ;
 if v4wTimeout=-1 do  ; If no timeout is passed by user, a -1 is passed
 . lock +@v4wName
 . set v4wResult=1
 else  do
 . lock +@v4wName:v4wTimeout
 . if $test set v4wResult=1
 ;
 set v4wResult=$select(v4wResult=1:"true",1:"false")
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   lock exit:") zwrite v4wResult use $principal
 quit "{""result"":"_v4wResult_"}"
 ;; @end lock function
 ;
 ;; @label unlock
 ;; @summary Unlock a global or local node, incrementally, or release all locks
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @returns {void}
unlock(v4wGlvn,v4wSubs,v4wMode)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   unlock enter:") zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 if $get(v4wGlvn)="" lock  do  quit
 . if $get(v4wDebug,0)>1 do debugLog(">>   unlock exit: unlock all")
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 ;
 new v4wName
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 lock -@v4wName
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   unlock exit:") zwrite v4wName use $principal
 quit
 ;; @end unlock label
 ;
 ;; @function function
 ;; @summary Call an arbitrary extrinsic function
 ;; @param {string} v4wFunc - The name of the function to call
 ;; @param {string} v4wArgs - Arguments represented as a string, encoded with argument lengths
 ;; @param {number} v4wRelink (0|1) - Whether to relink the function to be called, if it has changed, defaults to off
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @param {number} v4wInfo - Indirection limit on input - (0|1) Return data type on output; 0 is string, 1 is canonical number
 ;; @returns {string} - The return value of the function call
function(v4wFunc,v4wArgs,v4wRelink,v4wMode,v4wInfo)
 set v4wRelink=$get(v4wRelink,0)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   function enter:") zwrite v4wFunc,v4wArgs,v4wRelink,v4wMode,v4wInfo use $principal
 ;
 new v4wInputArgs
 set v4wInputArgs=$$process(v4wArgs,"input",v4wMode)
 ;
 ; Link latest routine image containing function in auto-relinking mode
 if v4wRelink zlink $ztranslate($select(v4wFunc["^":$zpiece(v4wFunc,"^",2),1:v4wFunc),"%","_")
 new v4wFunction
 set v4wFunction=$$construct(v4wFunc,v4wInputArgs)
 ;
 ; Construct a full function reference to get around the 8192 or 32766 indirection limit
 if $zlength(v4wFunction)>v4wInfo new v4wTempArgs set v4wFunction=$$constructFunction(v4wFunc,v4wInputArgs,.v4wTempArgs)
 if $get(v4wDebug,0)>1 do debugLog(">>   function:") zwrite v4wFunction use $principal
 ;
 new v4wResult
 ;
 do
 . ; Set principal device during Nodem::function call, for proper signal handling
 . use $principal:ctrap=$zchar(3) ; Catch SIGINT and pass to nodem.cc for handling
 . set ($ecode,$etrap)="" ; Turn off defaut error trap
 . new v4wArgs,v4wDebug,v4wFunc,v4wInfo,v4wInputArgs,v4wMode,v4wRelink
 . set @("v4wResult=$$"_v4wFunction)
 ;
 if v4wMode,'$$isString(v4wResult,"output") set v4wInfo=1
 else  set v4wInfo=0
 if $get(v4wDebug,0)>1 do debugLog(">>   function exit:") zwrite v4wInfo,v4wResult use $principal
 ;
 ; Reset principal device after coming back from user code
 use $principal:ctrap=$zchar(3) ; Catch SIGINT and pass to nodem.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 quit v4wResult
 ;; @end function function
 ;
 ;; @label procedure
 ;; @summary Call an arbitrary procedure/routine
 ;; @param {string} v4wProc - The name of the procedure to call
 ;; @param {string} v4wArgs - Arguments represented as a string, encoded with argument lengths
 ;; @param {number} v4wRelink (0|1) - Whether to relink the procedure/routine to be called, if it has changed, defaults to off
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is string mode, 1 is canonical mode
 ;; @param {number} v4wInfo - Indirection limit
 ;; @returns {void}
procedure(v4wProc,v4wArgs,v4wRelink,v4wMode,v4wInfo)
 set v4wRelink=$get(v4wRelink,0)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 do debugLog(">>   procedure enter:") zwrite v4wProc,v4wArgs,v4wRelink,v4wMode,v4wInfo use $principal
 ;
 new v4wInputArgs
 set v4wInputArgs=$$process(v4wArgs,"input",v4wMode)
 ;
 ; Link latest routine image containing procedure/routine in auto-relinking mode
 if v4wRelink zlink $ztranslate($select(v4wProc["^":$zpiece(v4wProc,"^",2),1:v4wProc),"%","_")
 ;
 new v4wProcedure
 set v4wProcedure=$$construct(v4wProc,v4wInputArgs)
 ;
 ; Construct a full procedure reference to get around the 8192 or 32766 indirection limit
 if $zlength(v4wProcedure)>v4wInfo new v4wTempArgs set v4wProcedure=$$constructFunction(v4wProc,v4wInputArgs,.v4wTempArgs)
 if $get(v4wDebug,0)>1 do debugLog(">>   procedure:") zwrite v4wProcedure use $principal
 ;
 do
 . ; Set principal device during Nodem::function call, for proper signal handling
 . use $principal:ctrap=$zchar(3) ; Catch SIGINT and pass to nodem.cc for handling
 . set ($ecode,$etrap)="" ; Turn off defaut error trap
 . new v4wArgs,v4wDebug,v4wInfo,v4wInputArgs,v4wMode,v4wProc,v4wRelink
 . do @v4wProcedure
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   procedure exit")
 ;
 ; Reset principal device after coming back from user code
 use $principal:ctrap=$zchar(3) ; Catch SIGINT and pass to nodem.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 quit
 ;; @end procedure label
 ;
 ;; @function globalDirectory
 ;; @summary List the globals in a database, with optional filters
 ;; @param {number} v4wMax - The maximum size of the return array
 ;; @param {string} v4wLo - The low end of a range of globals in the return array, inclusive
 ;; @param {string} v4wHi - The high end of a range of globals in the return array, inclusive
 ;; @returns {string} {JSON} - An array of globals
globalDirectory(v4wMax,v4wLo,v4wHi)
 set v4wMax=$get(v4wMax,0)
 set v4wLo=$get(v4wLo)
 set v4wHi=$get(v4wHi)
 if $get(v4wDebug,0)>1 do debugLog(">>   globalDirectory enter:") zwrite v4wMax,v4wLo,v4wHi use $principal
 ;
 new v4wCnt,v4wFlag
 set v4wCnt=1,v4wFlag=0
 ;
 new v4wName
 if ($get(v4wLo)="")!($$isNumber(v4wLo,"input")) set v4wName="^%"
 else  set v4wName=$select($zextract(v4wLo)="^":"",1:"^")_v4wLo
 ;
 if ($get(v4wHi)="")!($$isNumber(v4wHi,"input")) set v4wHi=""
 else  set v4wHi=$select($zextract(v4wHi)="^":"",1:"^")_v4wHi
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   globalDirectory:") zwrite v4wLo,v4wHi,v4wName use $principal
 ;
 new v4wReturn
 set v4wReturn="["
 ;
 if $data(@v4wName) do
 . set v4wReturn=v4wReturn_""""_$zextract(v4wName,2,$zlength(v4wName))_""","
 . if v4wMax=1 set v4wFlag=1 quit
 . if v4wMax>1 set v4wMax=v4wMax-1
 ;
 for  set v4wName=$order(@v4wName) quit:(v4wFlag)!(v4wName="")!((v4wName]]v4wHi)&(v4wHi]""))  do
 . set v4wReturn=v4wReturn_""""_$zextract(v4wName,2,$zlength(v4wName))_""","
 . if v4wMax>0 set v4wCnt=v4wCnt+1 set:v4wCnt>v4wMax v4wFlag=1
 ;
 if $zlength(v4wReturn)>2 set v4wReturn=$zextract(v4wReturn,1,$zlength(v4wReturn)-1)
 set v4wReturn=v4wReturn_"]"
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   globalDirectory exit:") zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end globalDirectory function
 ;
 ;; @function localDirectory
 ;; @summary List the local variables in the symbol table, with optional filters
 ;; @param {number} v4wMax - The maximum size of the return array
 ;; @param {string} v4wLo - The low end of a range of local variables in the return array, inclusive
 ;; @param {string} v4wHi - The high end of a range of local variables in the return array, inclusive
 ;; @returns {string} {JSON} - An array of local variables
localDirectory(v4wMax,v4wLo,v4wHi)
 set v4wMax=$get(v4wMax,0)
 set v4wLo=$get(v4wLo)
 set v4wHi=$get(v4wHi)
 if $get(v4wDebug,0)>1 do debugLog(">>   localDirectory enter:") zwrite v4wMax,v4wLo,v4wHi use $principal
 ;
 new v4wCnt,v4wFlag
 set v4wCnt=1,v4wFlag=0
 ;
 new v4wName
 if ($get(v4wLo)="")!($$isNumber(v4wLo,"input")) set v4wName="%"
 else  set v4wName=v4wLo if $zextract(v4wLo)="^" set $zextract(v4wName)=""
 ;
 if ($get(v4wHi)="")!($$isNumber(v4wHi,"input")) set v4wHi=""
 else  if $zextract(v4wHi)="^" set $zextract(v4wHi)=""
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   localDirectory:") zwrite v4wLo,v4wHi,v4wName use $principal
 ;
 new v4wReturn
 set v4wReturn="["
 ;
 if $data(@v4wName) do
 . set v4wReturn=v4wReturn_""""_v4wName_""","
 . if v4wMax=1 set v4wFlag=1 quit
 . if v4wMax>1 set v4wMax=v4wMax-1
 ;
 for  set v4wName=$order(@v4wName) quit:(v4wFlag)!(v4wName="")!((v4wName]]v4wHi)&(v4wHi]""))  do
 . if $zextract(v4wName,1,3)="v4w" quit  ; Do not allow manipulation of internal namespace symbols
 . set v4wReturn=v4wReturn_""""_v4wName_""","
 . if v4wMax>0 set v4wCnt=v4wCnt+1 set:v4wCnt>v4wMax v4wFlag=1
 ;
 if $zlength(v4wReturn)>2 set v4wReturn=$zextract(v4wReturn,1,$zlength(v4wReturn)-1)
 set v4wReturn=v4wReturn_"]"
 ;
 if $get(v4wDebug,0)>1 do debugLog(">>   localDirectory exit:") zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end localDirectory function
 ;
 ;; @function retrieve
 ;; @summary Not yet implemented
 ;; @returns {string} {JSON} - A message that the API is not yet implemented
retrieve()
 quit "{""ok"":false,""status"":""retrieve not yet implemented""}"
 ;; @end retrieve function
 ;
 ;; @function update
 ;; @summary Not yet implemented
 ;; @returns {string} {JSON} - A message that the API is not yet implemented
update()
 quit "{""ok"":false,""status"":""update not yet implemented""}"
 ;; @end update function
 ;
 ;; ***End Integration APIs***
