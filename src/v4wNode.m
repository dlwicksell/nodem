v4wNode() ;;2018-09-17  12:57 PM
 ;
 ; Package:    NodeM
 ; File:       v4wNode.m
 ; Summary:    A YottaDB/GT.M database driver and binding for Node.js
 ; Maintainer: David Wicksell <dlw@linux.com>
 ;
 ; Written by David Wicksell <dlw@linux.com>
 ; Copyright © 2012-2018 Fourth Watch Software LC
 ;
 ; This program is free software: you can redistribute it and/or modify
 ; it under the terms of the GNU Affero General Public License (AGPL)
 ; as published by the Free Software Foundation, either version 3 of
 ; the License, or (at your option) any later version.
 ;
 ; This program is distributed in the hope that it will be useful,
 ; but WITHOUT ANY WARRANTY; without even the implied warranty of
 ; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 ; GNU Affero General Public License for more details.
 ;
 ; You should have received a copy of the GNU Affero General Public License
 ; along with this program. If not, see http://www.gnu.org/licenses/.
 ;
 quit:$quit "Call an API entry point" write "Call an API entry point" quit
 ;
 ;; @function {private} construct
 ;; @summary Construct a full global reference fit for use by indirection
 ;; @param {string} name - Global or local variable name, or function or procedure name
 ;; @param {string} args - Subscripts or arguments as a comma-separated list, empty string if none
 ;; @returns {string} - Global or local reference ready to be used by indirection
construct:(name,args)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> construct:",! zwrite name,args
 quit name_$select(args'="":"("_args_")",1:"")
 ;; @end construct
 ;
 ;; @function {private} constructFunction
 ;; @summary Construct a full function or procedure reference to get around the 8192 indirection limit
 ;; @param {string} func - Function or procedure name
 ;; @param {string} args - Arguments as a comma-separated list, empty string if none
 ;; @param {reference} {(string|number)[]} tempArgs - Temporary argument array, used to get around the indirection limit
 ;; @returns {string} function - Function or procedure reference ready to be used by indirection
constructFunction:(func,args,tempArgs)
 new i,global,function
 set global="^global("_args_")",function=""
 ;
 for i=1:1:$qlength(global) do
 . set tempArgs(i)=$qsubscript(global,i)
 . set function=function_",v4wTempArgs("_i_")"
 ;
 set $zextract(function)=""
 set function=func_"("_function_")"
 ;
 quit function
 ;; @end constructFunction
 ;
 ;; @function {private} inputConvert
 ;; @summary Convert input data coming from Node.js for use with M
 ;; @param {string} data - Input data to be converted; a single subscript, function or procedure argument, or data
 ;; @param {number} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @param {number} node (0|1) - Data type; 0 is subscripts or arguments, 1 is data node
 ;; @returns {string} data - Converted input; a number or string ready to access M
inputConvert:(data,mode,node)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> inputConvert enter:",! zwrite data,mode,node
 ;
 if mode,'$$isString(data,"input") do
 . if $zextract(data,1,2)="0." set $zextract(data)=""
 . else  if $zextract(data,1,3)="-0." set $zextract(data,2)=""
 else  if node,$$isString(data,"input")=3 set $zextract(data)="",$zextract(data,$zlength(data))=""
 else  if 'node,$$isString(data,"input")<2,data'="" set data=""""_data_""""
 ;
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> inputConvert exit:",! zwrite data
 quit data
 ;; @end inputConvert
 ;
 ;; @function {private} inputEscape
 ;; @summary Escape input data coming from Node.js for use with M
 ;; @param {string} data - Input data to be escaped; a single subscript, function or procedure argument, or data
 ;; @param {number} node (0|1) - Data type; 0 is subscripts or arguments, 1 is data node
 ;; @returns {string} data - Escaped input; a string with quotes ready to access M
inputEscape:(data,node)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> inputEscape enter:",! zwrite data,node
 ;
 if 'node,data["""" do
 . new i,newData
 . set newData=""
 . ;
 . for i=2:1:$zlength(data)-1 do
 . . if $zextract(data,i)="""" set newData=newData_""""_$zextract(data,i)
 . . else  set newData=newData_$zextract(data,i)
 . set data=$zextract(data)_newData_$zextract(data,$zlength(data))
 ;
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> inputEscape exit:",! zwrite data
 quit data
 ;; @end inputEscape
 ;
 ;; @function {private} isNumber
 ;; @summary Returns true if data is number, false if not
 ;; @param {string} data - Input data to be tested; a single subscript, function or procedure argument, or data
 ;; @param {string} direction (input|output) - Processing control direction
 ;; @returns {number} (0|1) - Return code representing data type
isNumber:(data,direction)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> isNumber enter:",! zwrite data,direction
 ;
 if data'["E",data=+data write:$get(v4wDebug,0)>2 "DEBUG>>> isNumber: 1",! quit 1
 else  if direction="input",data?.1"-"1"0"1"."1.N  write:$get(v4wDebug,0)>2 "DEBUG>>> isNumber: 1",! quit 1
 else  write:$get(v4wDebug,0)>2 "DEBUG>>> isNumber: 0",! quit 0
 ;; @end isNumber
 ;
 ;; @function {private} isString
 ;; @summary Returns true if data is string (including whether it has surrounding quotes), false if not
 ;; @param {string} data - Input data to be tested; a single subscript, function or procedure argument, or data
 ;; @param {string} direction (input|output) - Processing control direction
 ;; @returns {number} (0|1|2) - Return code representing data type
isString:(data,direction)
 ; YottaDB/GT.M approximate (using number of digits, rather than number value) number limits:
 ;   - 47 digits before overflow (resulting in an overflow error)
 ;   - 18 digits of precision
 ; Node.js/JavaScript approximate (using number of digits, rather than number value) number limits:
 ;   - 309 digits before overflow (represented as the Infinity primitive)
 ;   - 21 digits before conversion to exponent notation
 ;   - 16 digits of precision
 ; This is why anything over 15 characters needs to be treated as a string
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> isString enter:",! zwrite data,direction
 ;
 if ($zextract(data)="""")&($zextract(data,$zlength(data))="""") write:$get(v4wDebug,0)>2 "isString: 3",! quit 3
 else  if $zlength(data)>15 write:$get(v4wDebug,0)>2 "DEBUG>>> isString: 1",! quit 1
 else  if direction="input",data["e+" write:$get(v4wDebug,0)>2 "DEBUG>>> isString: 1",! quit 1
 else  if $$isNumber(data,direction) write:$get(v4wDebug,0)>2 "isString: 0",! quit 0
 else  write:$get(v4wDebug,0)>2 "isString: 2",! quit 2
 ;; @end isString
 ;
 ;; @function {private} outputConvert
 ;; @summary Convert output data coming from M for use with Node.js
 ;; @param {string} data - Output data to be converted; a single subscript, function or procedure argument, or data
 ;; @param {number} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} data - Converted output; a number or string ready to return to Node.js
outputConvert:(data,mode)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> outputConvert enter:",! zwrite data,mode
 ;
 if mode,'$$isString(data,"output") do
 . if $zextract(data)="." set data=0_data
 . else  if $zextract(data,1,2)="-." set $zextract(data)="",data="-0"_data
 else  set data=""""_data_""""
 ;
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> outputConvert exit:",! zwrite data
 quit data
 ;; @end outputConvert
 ;
 ;; @function {private} outputEscape
 ;; @summary Escape output data coming from M for use with Node.js
 ;; @param {string} data - Output data to be escaped; a single subscript, function or procedure argument, or data
 ;; @returns {string} data - Escaped output; a string with quotes ready to return to Node.js
outputEscape:(data)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> outputEscape enter:",! zwrite data
 ;
 if (data["""")!(data["\")!(data?.e1c.e) do
 . new i,charHigh,charLow,newData
 . set newData=""
 . ;
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
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> outputEscape exit:",! zwrite data
 quit data
 ;; @end outputEscape
 ;
 ;; @subroutine {private} parse
 ;; @summary Transform an encoded string (subscripts or arguments) in to an M array
 ;; @param {string} inputString - Input string to be transformed
 ;; @param {reference} {(string|number)[]} outputArray - Output array to be returned
 ;; @param {number} encode (0|1) - Whether subscripts or arguments are encoded, 0 is no, 1 is yes
 ;; @param {number} last (0|1) - Whether to ignore the last subscript (for merge in strict mode), 0 is no, 1 is yes
 ;; @returns {void}
parse:(inputString,outputArray,encode,last)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> parse enter:",! zwrite inputString,encode,last
 ;
 if inputString="" set outputArray(1)="" do  quit
 . if $get(v4wDebug,0)>2 write !,"DEBUG>>> parse exit:",! zwrite outputArray
 ;
 kill outputArray
 if encode do
 . new i,length
 . for i=1:1 quit:inputString=""  do
 . . set length=+inputString
 . . set $zextract(inputString,1,$zlength(length)+1)=""
 . . set outputArray(i)=$zextract(inputString,1,length)
 . . set $zextract(inputString,1,length+1)=""
 . if last kill outputArray(i-1)
 else  do
 . set outputArray(1)=inputString
 . set inputString=""
 ;
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> parse exit:",! if $data(outputArray) zwrite outputArray
 quit
 ;; @end parse
 ;
 ;; @function {private} process
 ;; @summary Process an encoded string of subscripts, arguments, or an unencoded data node
 ;; @param {string} inputString - Input string to be transformed
 ;; @param {string} direction (input|output) - Processing control direction
 ;; @param {number} mode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @param {number} node (0|1) - Data type; 0 is subscripts or arguments, 1 is data node
 ;; @param {number} encode (0|1) - Whether subscripts or arguments are encoded, 0 is no, 1 is yes
 ;; @param {number} last (0|1) - Whether to ignore the last subscript (for merge in strict mode), 0 is no, 1 is yes
 ;; @returns {string} outputString - Output string ready for use by the APIs
process:(inputString,direction,mode,node,encode,last)
 set mode=$get(mode,1)
 set node=$get(node,0)
 set encode=$get(encode,1)
 set last=$get(last,0)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> process enter:",! zwrite inputString,direction,mode,node,encode,last
 ;
 new array,num,outputString
 set outputString=""
 if inputString="" do  quit outputString
 . if node set outputString=""""""
 . if $get(v4wDebug,0)>2 write !,"DEBUG>>> process exit:",! zwrite outputString
 ;
 do parse(inputString,.array,encode,last)
 ;
 set num=0
 for  set num=$order(array(num)) quit:num=""  do
 . if direction="input" do
 . . set array(num)=$$inputEscape(array(num),node)
 . . set array(num)=$$inputConvert(array(num),mode,node)
 . else  if direction="output" do
 . . set array(num)=$$outputEscape(array(num))
 . . set array(num)=$$outputConvert(array(num),mode)
 . else  if direction="pass" do
 . . set $zextract(array(num))=$ztranslate($zextract(array(num)),"""","")
 . . set $zextract(array(num),$zlength(array(num)))=$ztranslate($zextract(array(num),$zlength(array(num))),"""","")
 . . set array(num)=$$outputEscape(array(num))
 . . set array(num)=$$outputConvert(array(num),mode)
 ;
 do stringify(.array,.outputString)
 ;
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> process exit:",! zwrite outputString
 quit outputString
 ;; @end process
 ;
 ;; @subroutine {private} stringify
 ;; @summary Transform an M array containing subscripts, arguments, or data, in to a string ready for use by the APIs
 ;; @param {(string|number)[]} inputArray - Input array to be transformed
 ;; @param {reference} {string} outputString - Output string to returned
 ;; @returns {void}
stringify:(inputArray,outputString)
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> stringify enter:",! zwrite inputArray
 ;
 if $data(inputArray)<10 set outputString="" do  quit
 . if $get(v4wDebug,0)>2 write !,"DEBUG>>> stringify exit:",! zwrite outputString
 ;
 new num
 set num=0,outputString=""
 for  set num=$order(inputArray(num)) quit:num=""  set outputString=outputString_","_inputArray(num)
 set $zextract(outputString)=""
 ;
 if $get(v4wDebug,0)>2 write !,"DEBUG>>> stringify exit:",! zwrite outputString
 quit
 ;; @end stringify
 ;
 ;; ***Begin Public APIs***
 ;;
 ;; These APIs are part of the integration code, called by the C call-in interface (gtm_cip or gtm_ci)
 ;; They may be called from MUMPS code directly, for unit testing
 ;
 ;; @subroutine {public} debug
 ;; @summary Set debugging level, defaults to off
 ;; @param {number} level (0|1|2|3) - Debugging level, 0 is off, 1 is low, 2 is medium, 3 is high
 ;; @returns void
debug(level)
 set v4wDebug=$get(level,0)
 quit
 ;; @end debug
 ;
 ;; @function {public} data
 ;; @summary Check if global or local node has data and/or children or not
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - $data value; 0 for no data nor children, 1 for data, 10 for children, 11 for data and children
data(v4wGlvn,v4wSubs,v4wNode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wNode=$get(v4wNode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> data enter:",! zwrite v4wGlvn,v4wSubs,v4wNode
 ;
 new v4wDefined,v4wName
 set v4wSubs=$$process(v4wSubs,"input",v4wNode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> data:",! zwrite v4wName
 ;
 set v4wDefined=$data(@v4wName)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> data exit:",! zwrite v4wDefined use $principal
 quit "{""defined"":"_v4wDefined_"}"
 ;; @end data
 ;
 ;; @function {public} function
 ;; @summary Call an arbitrary extrinsic function
 ;; @param {string} v4wFunc - The name of the function to call
 ;; @param {string} v4wArgs - Arguments represented as a string, encoded with argument lengths
 ;; @param {number} v4wRelink (0|1) - Whether to relink the function to be called, if it has changed, defaults to off
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The return value of the function call
function(v4wFunc,v4wArgs,v4wRelink,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wRelink=$get(v4wRelink,0)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> function enter:",! zwrite v4wFunc,v4wArgs,v4wRelink,v4wMode
 ;
 new v4wFunction,v4wInputArgs,v4wResult
 set v4wInputArgs=$$process(v4wArgs,"input",v4wMode)
 ;
 ; Link latest routine image containing function in auto-relinking mode
 if v4wRelink zlink $ztranslate($select(v4wFunc["^":$zpiece(v4wFunc,"^",2),1:v4wFunc),"%","_")
 set v4wFunction=$$construct(v4wFunc,v4wInputArgs)
 ;
 ; Construct a full function reference to get around the 8192 indirection limit
 if $zlength(v4wFunction)>8183 new v4wTempArgs set v4wFunction=$$constructFunction(v4wFunc,v4wInputArgs,.v4wTempArgs)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> function:",! zwrite v4wFunction
 ;
 do
 . new v4wArgs,v4wDebug,v4wFunc,v4wInputArgs,v4wMode,v4wRelink
 . set @("v4wResult=$$"_v4wFunction)
 ;
 set v4wResult=$$process(v4wResult,"output",v4wMode,1,0)
 ;
 if v4wMode do  quit "{""result"":"_v4wResult_"}"
 . if $get(v4wDebug,0)>1 write !,"DEBUG>> function exit:",! zwrite v4wResult use $principal
 ;
 set v4wReturn="{"
 if v4wArgs'="" set v4wReturn=v4wReturn_"""arguments"":["_$$process(v4wArgs,"pass",v4wMode)_"],"
 set v4wReturn=v4wReturn_"""result"":"_v4wResult_"}"
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> function exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end function
 ;
 ;; @function {public} get
 ;; @summary Get data from a global or local node
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The value of the data node, and whether it was defined or not
get(v4wGlvn,v4wSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> get enter:",! zwrite v4wGlvn,v4wSubs,v4wMode
 ;
 new v4wData,v4wDefined,v4wName
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> get:",! zwrite v4wName
 ;
 if $zextract(v4wName)="$" do
 . set v4wDefined=1
 . xecute "set v4wName="_v4wName
 . set v4wData=$$process(v4wName,"output",v4wMode,1,0)
 else  do
 . set v4wDefined=$data(@v4wName)#10
 . set v4wData=$$process($get(@v4wName),"output",v4wMode,1,0)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> get exit:",! zwrite v4wDefined,v4wData use $principal
 quit "{""defined"":"_v4wDefined_",""data"":"_v4wData_"}"
 ;; @end get
 ;
 ;; @function {public} globalDirectory
 ;; @summary List the globals in a database, with optional filters
 ;; @param {number} v4wMax - The maximum size of the return array
 ;; @param {string} v4wLo - The low end of a range of globals in the return array, inclusive
 ;; @param {string} v4wHi - The high end of a range of globals in the return array, inclusive
 ;; @returns {string} {JSON} - An array of globals
globalDirectory(v4wMax,v4wLo,v4wHi)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMax=$get(v4wMax,0)
 set v4wLo=$get(v4wLo)
 set v4wHi=$get(v4wHi)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> globalDirectory enter:",! zwrite v4wMax,v4wLo,v4wHi
 new v4wCnt,v4wFlag,v4wName,v4wReturn
 set v4wCnt=1,v4wFlag=0
 ;
 if ($get(v4wLo)="")!($$isNumber(v4wLo,"input")) set v4wName="^%"
 else  set v4wName=$select($zextract(v4wLo)="^":"",1:"^")_v4wLo
 ;
 if ($get(v4wHi)="")!($$isNumber(v4wHi,"input")) set v4wHi=""
 else  set v4wHi=$select($zextract(v4wHi)="^":"",1:"^")_v4wHi
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> globalDirectory:",! zwrite v4wLo,v4wHi,v4wName
 ;
 set v4wReturn="["
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
 if $get(v4wDebug,0)>1 write !,"DEBUG>> globalDirectory exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end globalDirectory
 ;
 ;; @function {public} increment
 ;; @summary Increment or decrement the number in a global or local node
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wIncr - The number to increment/decrement, defaults to 1
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The new value of the data node that was incremented/decremented
increment(v4wGlvn,v4wSubs,v4wIncr,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wIncr=$get(v4wIncr,1)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> increment enter:",! zwrite v4wGlvn,v4wSubs,v4wIncr,v4wMode
 ;
 new v4wData,v4wName
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> increment:",! zwrite v4wName
 ;
 set v4wData=$$process($increment(@v4wName,v4wIncr),"output",v4wMode,1,0)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> increment exit:",! zwrite v4wData use $principal
 quit "{""data"":"_v4wData_"}"
 ;; @end increment
 ;
 ;; @subroutine {public} kill
 ;; @summary Kill a global or global node, or a local or local node, or the entire local symbol table
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns void
kill(v4wGlvn,v4wSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> kill enter:",! zwrite v4wGlvn,v4wSubs,v4wMode
 ;
 new v4wName
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> kill:",! zwrite v4wName
 ;
 if v4wGlvn="" kill (v4wDebug)
 else  kill @v4wName
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> kill exit",! use $principal
 quit
 ;; @end kill
 ;
 ;; @function {public} localDirectory
 ;; @summary List the local variables in the symbol table, with optional filters
 ;; @param {number} v4wMax - The maximum size of the return array
 ;; @param {string} v4wLo - The low end of a range of local variables in the return array, inclusive
 ;; @param {string} v4wHi - The high end of a range of local variables in the return array, inclusive
 ;; @returns {string} {JSON} - An array of local variables
localDirectory(v4wMax,v4wLo,v4wHi)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMax=$get(v4wMax,0)
 set v4wLo=$get(v4wLo)
 set v4wHi=$get(v4wHi)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> localDirectory enter:",! zwrite v4wMax,v4wLo,v4wHi
 new v4wCnt,v4wFlag,v4wName,v4wReturn
 set v4wCnt=1,v4wFlag=0
 ;
 if ($get(v4wLo)="")!($$isNumber(v4wLo,"input")) set v4wName="%"
 else  set v4wName=v4wLo if $zextract(v4wLo)="^" set $zextract(v4wName)=""
 ;
 if ($get(v4wHi)="")!($$isNumber(v4wHi,"input")) set v4wHi=""
 else  if $zextract(v4wHi)="^" set $zextract(v4wHi)=""
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> localDirectory:",! zwrite v4wLo,v4wHi,v4wName
 ;
 set v4wReturn="["
 if $data(@v4wName) do
 . set v4wReturn=v4wReturn_""""_v4wName_""","
 . if v4wMax=1 set v4wFlag=1 quit
 . if v4wMax>1 set v4wMax=v4wMax-1
 ;
 for  set v4wName=$order(@v4wName) quit:(v4wFlag)!(v4wName="")!((v4wName]]v4wHi)&(v4wHi]""))  do
 . if $zextract(v4wName,1,3)="v4w" quit
 . set v4wReturn=v4wReturn_""""_v4wName_""","
 . if v4wMax>0 set v4wCnt=v4wCnt+1 set:v4wCnt>v4wMax v4wFlag=1
 ;
 if $zlength(v4wReturn)>2 set v4wReturn=$zextract(v4wReturn,1,$zlength(v4wReturn)-1)
 set v4wReturn=v4wReturn_"]"
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> localDirectory exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end localDirectory
 ;
 ;; @function {public} lock
 ;; @summary Lock a global or local node, incrementally
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wTimeout - The time to wait for the lock, or -1 to wait forever
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - Returns whether the lock was acquired or not
lock(v4wGlvn,v4wSubs,v4wTimeout,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wTimeout=$get(v4wTimeout,-1)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> lock enter:",! zwrite v4wGlvn,v4wSubs,v4wTimeout,v4wMode
 ;
 new v4wInputSubs,v4wName,v4wResult,v4wReturn
 set v4wInputSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wInputSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> lock:",! zwrite v4wName
 ;
 set v4wResult=0
 ;
 if v4wTimeout=-1 do  ; If no timeout is passed by user, a -1 is passed
 . lock +@v4wName
 . if $test set v4wResult=1
 else  do
 . lock +@v4wName:v4wTimeout
 . if $test set v4wResult=1
 ;
 if v4wMode do  quit "{""result"":"_v4wResult_"}"
 . if $get(v4wDebug,0)>1 write !,"DEBUG>> lock exit:",! zwrite v4wResult use $principal
 ;
 set v4wReturn="{"
 if v4wSubs'="" set v4wReturn=v4wReturn_"""subscripts"":["_$$process(v4wSubs,"pass",v4wMode)_"],"
 set v4wReturn=v4wReturn_"""result"":"_v4wResult_"}"
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> lock exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end lock
 ;
 ;; @function {public} merge
 ;; @summary Merge a global or local array node to another global or local array node
 ;; @param {string} v4wFromGlvn - Global or local variable to merge from
 ;; @param {string} v4wFromSubs - From subscripts represented as a string, encoded with subscript lengths
 ;; @param {string} v4wToGlvn - Global or local variable to merge to
 ;; @param {string} v4wToSubs - To subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} -
merge(v4wFromGlvn,v4wFromSubs,v4wToGlvn,v4wToSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> merge enter:",! zwrite v4wFromGlvn,v4wFromSubs,v4wToGlvn,v4wToSubs,v4wMode
 ;
 new v4wFromName,v4wFromInputSubs,v4wReturn,v4wToName,v4wToInputSubs
 set v4wFromInputSubs=$$process(v4wFromSubs,"input",v4wMode)
 set v4wFromName=$$construct(v4wFromGlvn,v4wFromInputSubs)
 set v4wToInputSubs=$$process(v4wToSubs,"input",v4wMode)
 set v4wToName=$$construct(v4wToGlvn,v4wToInputSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> merge:",! zwrite v4wFromName,v4wToName
 ;
 merge @v4wToName=@v4wFromName
 ;
 if v4wMode do  quit "{}"
 . if $get(v4wDebug,0)>1 write !,"DEBUG>> merge exit",! use $principal
 ;
 set v4wReturn="{"
 if (v4wFromSubs'="")!(v4wToSubs'="") do
 . set v4wReturn=v4wReturn_"""subscripts"":["
 . if v4wFromSubs'="" set v4wReturn=v4wReturn_$$process(v4wFromSubs,"pass",v4wMode)_","
 . if $zextract(v4wToGlvn)="^" set $zextract(v4wToGlvn)=""
 . set v4wReturn=v4wReturn_""""_v4wToGlvn_""""
 . if v4wToSubs'="" set v4wReturn=v4wReturn_","_$$process(v4wToSubs,"pass",v4wMode,,,1)
 . if $zextract(v4wReturn,$zlength(v4wReturn))="," set $zextract(v4wReturn,$zlength(v4wReturn))=""
 . set v4wReturn=v4wReturn_"]"
 set v4wReturn=v4wReturn_"}"
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> merge exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end merge
 ;
 ;; @function {public} nextNode
 ;; @summary Return the next global or local node, depth first
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The next or previous data node
nextNode(v4wGlvn,v4wSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> nextNode enter:",! zwrite v4wGlvn,v4wSubs,v4wMode
 ;
 new v4wData,v4wDefined,v4wName,v4wNewSubscripts,v4wResult,v4wReturn
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> nextNode:",! zwrite v4wName
 ;
 set v4wResult=$query(@v4wName)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> nextNode:",! zwrite v4wResult
 ;
 if v4wResult="" set v4wDefined=0
 else  set v4wDefined=1
 ;
 if v4wDefined do
 . new i,sub
 . set v4wData=$$process($get(@v4wResult),"output",v4wMode,1,0)
 . ;
 . if $zextract(v4wResult)="^" set $zextract(v4wResult)=""
 . set v4wNewSubscripts=""
 . ;
 . for i=1:1:$qlength(v4wResult) do
 . . set sub=$$process($qsubscript(v4wResult,i),"output",v4wMode,,0)
 . . set v4wNewSubscripts=v4wNewSubscripts_","_sub
 . set $zextract(v4wNewSubscripts)=""
 ;
 set v4wReturn="{"
 if v4wDefined,v4wNewSubscripts'="" set v4wReturn=v4wReturn_"""subscripts"":["_v4wNewSubscripts_"],"
 set v4wReturn=v4wReturn_"""defined"":"_v4wDefined
 if v4wDefined set v4wReturn=v4wReturn_",""data"":"_v4wData_"}"
 else  set v4wReturn=v4wReturn_"}"
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> nextNode exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end nextNode
 ;
 ;; @function {public} order
 ;; @summary Return the next global or local node at the same level
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - The next or previous data node
order(v4wGlvn,v4wSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> order enter:",! zwrite v4wGlvn,v4wSubs,v4wMode
 ;
 new v4wName,v4wResult
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> order:",! zwrite v4wName
 ;
 set v4wResult=$order(@v4wName)
 ;
 if v4wSubs="",$zextract(v4wResult)="^" set $zextract(v4wResult)=""
 set v4wResult=$$process(v4wResult,"output",v4wMode,1,0)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> order exit:",! zwrite v4wResult use $principal
 quit "{""result"":"_v4wResult_"}"
 ;; @end order
 ;
 ;; @function {public} previous
 ;; @summary Same as order, only in reverse
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - Returns the previous node, via calling order function and passing a -1 to the order argument
previous(v4wGlvn,v4wSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> previous enter:",! zwrite v4wGlvn,v4wSubs,v4wMode
 ;
 new v4wName,v4wResult
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> previous:",! zwrite v4wName
 ;
 set v4wResult=$order(@v4wName,-1)
 ;
 if v4wSubs="",$zextract(v4wResult)="^" set $zextract(v4wResult)=""
 set v4wResult=$$process(v4wResult,"output",v4wMode,1,0)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> previous exit:",! zwrite v4wResult use $principal
 quit "{""result"":"_v4wResult_"}"
 ;; @end previous
 ;
 ;; @function {public} previousNode
 ;; @summary Return the previous global or local node, depth first
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns {string} {JSON} - Returns the previous node, via calling nextNode function and passing a -1 to the order argument
previousNode(v4wGlvn,v4wSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 ; Handle reverse $query not existing in this M implementation version (150373074: %GTM-E-INVSVN, Invalid special variable name)
 set $etrap="if $ecode["",Z150373074,"" set $ecode="""" quit ""{""""status"""":""""previous_node not yet implemented""""}"""
 set $ecode=""
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> previousNode enter:",! zwrite v4wGlvn,v4wSubs,v4wMode use $principal
 ;
 new v4wData,v4wDefined,v4wName,v4wNewSubscripts,v4wResult,v4wReturn,v4wYottaVersion
 set v4wYottaVersion=$zpiece($zyrelease," ",2),$zextract(v4wYottaVersion)=""
 ;
 if v4wYottaVersion<1.10 quit "{""status"":""previous_node not yet implemented""}"
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> previousNode:",! zwrite v4wName
 ;
 set v4wResult=$query(@v4wName,-1)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> previousNode:",! zwrite v4wResult
 ;
 if v4wResult="" set v4wDefined=0
 else  set v4wDefined=1
 ;
 if v4wDefined do
 . new i,sub
 . set v4wData=$$process($get(@v4wResult),"output",v4wMode,1,0)
 . ;
 . if $zextract(v4wResult)="^" set $zextract(v4wResult)=""
 . set v4wNewSubscripts=""
 . ;
 . for i=1:1:$qlength(v4wResult) do
 . . set sub=$$process($qsubscript(v4wResult,i),"output",v4wMode,,0)
 . . set v4wNewSubscripts=v4wNewSubscripts_","_sub
 . set $zextract(v4wNewSubscripts)=""
 ;
 set v4wReturn="{"
 if v4wDefined,v4wNewSubscripts'="" set v4wReturn=v4wReturn_"""subscripts"":["_v4wNewSubscripts_"],"
 set v4wReturn=v4wReturn_"""defined"":"_v4wDefined
 if v4wDefined set v4wReturn=v4wReturn_",""data"":"_v4wData_"}"
 else  set v4wReturn=v4wReturn_"}"
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> previousNode exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end previousNode
 ;
 ;; @subroutine {public} procedure
 ;; @summary Call an arbitrary procedure/subroutine
 ;; @param {string} v4wProc - The name of the procedure to call
 ;; @param {string} v4wArgs - Arguments represented as a string, encoded with argument lengths
 ;; @param {number} v4wRelink (0|1) - Whether to relink the procedure/subroutine to be called, if it has changed, defaults to off
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns void
procedure(v4wProc,v4wArgs,v4wRelink,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wRelink=$get(v4wRelink,0)
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> procedure enter:",! zwrite v4wProc,v4wArgs,v4wRelink,v4wMode
 ;
 new v4wInputArgs,v4wProcedure
 set v4wInputArgs=$$process(v4wArgs,"input",v4wMode)
 ;
 ; Link latest routine image containing procedure/subroutine in auto-relinking mode
 if v4wRelink zlink $ztranslate($select(v4wProc["^":$zpiece(v4wProc,"^",2),1:v4wProc),"%","_")
 set v4wProcedure=$$construct(v4wProc,v4wInputArgs)
 ;
 ; Construct a full procedure reference to get around the 8192 indirection limit
 if $zlength(v4wProcedure)>8183 new v4wTempArgs set v4wProcedure=$$constructFunction(v4wProc,v4wInputArgs,.v4wTempArgs)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> procedure:",! zwrite v4wProcedure
 ;
 do
 . new v4wArgs,v4wDebug,v4wInputArgs,v4wMode,v4wProc,v4wRelink
 . do @v4wProcedure
 ;
 if v4wMode do  quit "{}"
 . if $get(v4wDebug,0)>1 write !,"DEBUG>> procedure exit",! use $principal
 ;
 set v4wReturn="{"
 if v4wArgs'="" set v4wReturn=v4wReturn_"""arguments"":["_$$process(v4wArgs,"pass",v4wMode)_"]"
 set v4wReturn=v4wReturn_"}"
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> procedure exit:",! zwrite v4wReturn use $principal
 quit v4wReturn
 ;; @end procedure
 ;
 ;; @function {public} retrieve
 ;; @summary Not yet implemented
 ;; @returns {string} {JSON} - A message that the API is not yet implemented
retrieve()
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 quit "{""status"":""retrieve not yet implemented""}"
 ;; @end retrieve
 ;
 ;; @subroutine {public} set
 ;; @summary Set a global or local node
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {string} v4wData - Data to store in the database node or local variable node
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns void
set(v4wGlvn,v4wSubs,v4wData,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> set enter:",! zwrite v4wGlvn,v4wSubs,v4wData,v4wMode
 ;
 new v4wName
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 set v4wData=$$process(v4wData,"input",v4wMode,1)
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> set:",! zwrite v4wName,v4wData
 ;
 if $zextract(v4wName)="$" do
 . xecute "set $"_$zextract(v4wName,2,$zlength(v4wName))_"="_$$process(v4wData,"output",v4wMode,1,0)
 else  do
 . set @v4wName=v4wData
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> set exit",! use $principal
 quit
 ;; @end set
 ;
 ;; @function {public} unlock
 ;; @summary Unlock a global or local node, incrementally, or release all locks
 ;; @param {string} v4wGlvn - Global or local variable
 ;; @param {string} v4wSubs - Subscripts represented as a string, encoded with subscript lengths
 ;; @param {number} v4wMode (0|1) - Data mode; 0 is strict mode, 1 is canonical mode
 ;; @returns void
unlock(v4wGlvn,v4wSubs,v4wMode)
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 set v4wMode=$get(v4wMode,1)
 if $get(v4wDebug,0)>1 write !,"DEBUG>> unlock enter:",! zwrite v4wGlvn,v4wSubs,v4wMode
 ;
 new v4wName
 ;
 if $get(v4wGlvn)="" lock  do  quit
 . if $get(v4wDebug,0)>1 write !,"DEBUG>> unlock exit: unlock all",! use $principal
 ;
 set v4wSubs=$$process(v4wSubs,"input",v4wMode)
 set v4wName=$$construct(v4wGlvn,v4wSubs)
 ;
 lock -@v4wName
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> unlock exit:",! zwrite v4wName use $principal
 quit
 ;; @end unlock
 ;
 ;; @function {public} update
 ;; @summary Not yet implemented
 ;; @returns {string} {JSON} - A message that the API is not yet implemented
update()
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 set ($ecode,$etrap)="" ; Turn off defaut error trap
 ;
 quit "{""status"":""update not yet implemented""}"
 ;; @end update
 ;
 ;; @function {public} version
 ;; @summary Return the about/version string
 ;; @returns {string} {JSON} - The YottaDB and/or GT.M version
version()
 use $principal:ctrap="$zchar(3)" ; Catch SIGINT and pass to mumps.cc for handling
 ; Handle $zyrelease not existing in this M implementation version (150373074: %GTM-E-INVSVN, Invalid special variable name)
 set $etrap="if $ecode["",Z150373074,"" set $ecode="""" quit ""GT.M version: ""_gtmVersion"
 set $ecode=""
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> version enter",! use $principal
 ;
 new gtmVersion,yottaVersion
 set gtmVersion=$zpiece($zversion," ",2),$zextract(gtmVersion)=""
 set yottaVersion=$zpiece($zyrelease," ",2),$zextract(yottaVersion)=""
 ;
 if $get(v4wDebug,0)>1 write !,"DEBUG>> version exit:",! zwrite return use $principal
 quit "GT.M version: "_gtmVersion_"; YottaDB version: "_yottaVersion
 ;; @end version
