v4wNode() ;; 2017-01-08  12:18 PM
 ; A GT.M database driver for Node.js
 ;
 ; Written by David Wicksell <dlw@linux.com>
 ; Copyright © 2012-2017 Fourth Watch Software LC
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
 ;
 quit:$q "Call an API entry point" w "Call an API entry point" quit
 ;
 ;
construct:(glvn,subs) ;construct a global reference
 quit $s($e(glvn)="^":"",1:"^")_glvn_$s(subs'="":"("_subs_")",1:"")
 ;
 ;
iconvert:(data,mode) ;convert for input
 i '$g(mode),$l(data)<19 d
 . i $e(data,1,2)="0.",$e(data,2,$l(data))=+$e(data,2,$l(data)) s $e(data)=""
 . e  i $e(data,1,3)="-0.",$e(data,3,$l(data))=+$e(data,3,$l(data)) s $e(data,2)=""
 ;
 q data
 ;
 ;
iescape:(data) ;unescape quotes within a string
 n ndata
 ;
 i data["""" d
 . n i
 . ;
 . s ndata=""
 . ;
 . f i=1:1:$l(data) d
 . . i $e(data,i)="""" d
 . . . i i=1!(i=$l(data)) d
 . . . . s ndata=ndata_$e(data,i)
 . . . e  d
 . . . . s ndata=ndata_""""_$e(data,i)
 . . e  s ndata=ndata_$e(data,i)
 e  s ndata=data
 ;
 quit ndata
 ;
 ;
oconvert:(data,mode) ;convert for output
 i '$g(mode),$l(data)<19,data=+data d
 . i $e(data)="." s data=0_data
 . e  i $e(data,1,2)="-." s $e(data)="",data="-0"_data
 e  s data=""""_data_""""
 ;
 q data
 ;
 ;
oescape:(data) ;escape quotes or control characters within a string
 n ndata
 ;
 i data[""""!(data["\")!(data?.e1c.e) d
 . n charh,charl,i
 . ;
 . s ndata=""
 . ;
 . f i=1:1:$l(data) d
 . . i $e(data,i)=""""!($e(data,i)="\") s ndata=ndata_"\"_$e(data,i)
 . . e  i $e(data,i)?1c!($a($e(data,i))>127&($a($e(data,i))<256)&($zch="M")) d
 . . . s charh=$a($e(data,i))\16,charh=$e("0123456789abcdef",charh+1)
 . . . s charl=$a($e(data,i))#16,charl=$e("0123456789abcdef",charl+1)
 . . . s ndata=ndata_"\u00"_charh_charl
 . . e  s ndata=ndata_$e(data,i)
 e  s ndata=data
 ;
 quit ndata
 ;
 ;
parse:(subs,type,mode) ;parse an argument list or list of subscripts
 s subs=$g(subs)
 ;
 i subs'="" d
 . n num,sub,tmp
 . ;
 . s tmp=""
 . ;
 . f  q:subs=""  d
 . . s num=+subs
 . . s $e(subs,1,$l(num)+1)=""
 . . s sub=$e(subs,1,num)
 . . ;
 . . i type="input" d
 . . . s sub=$$iescape(sub)
 . . . s sub=$$iconvert(sub,mode)_","
 . . e  i type="output" d
 . . . s sub=$$oescape(sub)
 . . . s sub=$$oconvert(sub,mode)_","
 . . e  i type="pass" d
 . . . s $e(sub)=$tr($e(sub),"""","")
 . . . s $e(sub,$l(sub))=$tr($e(sub,$l(sub)),"""","")
 . . . ;
 . . . s sub=$$oescape(sub)
 . . . s sub=$$oconvert(sub,mode)_","
 . . s tmp=tmp_sub
 . . s $e(subs,1,num+1)=""
 . s subs=tmp
 s subs=$e(subs,1,$l(subs)-1)
 ;
 quit subs
 ;
 ;
data(glvn,subs,mode) ;check if global node has data or children
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n defined,globalname
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 s defined=$d(@globalname)
 ;
 s glvn=$$oescape(glvn) ;for extended references
 ;
 quit "{""ok"": 1, ""global"": """_glvn_""", ""defined"": "_defined_"}"
 ;
 ;
function(func,args,relink,mode) ;call an arbitrary extrinsic function
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n function,result
 ;
 s args=$$parse($g(args),"input",mode)
 ;
 ;link latest routine image containing function in auto-relinking mode
 i relink zl $tr($s(func["^":$p(func,"^",2),1:func),"%","_")
 ;
 s function=func_$s(args'="":"("_args_")",1:"")
 ;
 i function'["^" s function="^"_function
 ;
 d
 . n func,mode s @("result=$$"_function)
 ;
 s result=$$oescape(result)
 s result=$$oconvert(result,mode)
 ;
 quit "{""ok"": 1, ""function"": """_func_""", ""result"": "_result_"}"
 ;
 ;
get(glvn,subs,mode) ;get data from global node
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n data,defined,globalname,return
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 s data=$g(@globalname)
 ;
 s data=$$oescape(data)
 s data=$$oconvert(data,mode)
 s glvn=$$oescape(glvn) ;for extended references
 ;
 s defined=$d(@globalname)#10
 ;
 s return="{""ok"": 1, ""global"": """_glvn_""","
 s return=return_" ""data"": "_data_", ""defined"": "_defined_"}"
 ;
 quit return
 ;
 ;
globalDirectory(max,lo,hi) ;list the globals in a database, filtered or not
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n cnt,flag,global,return
 ;
 s max=$g(max,0)
 s cnt=1,flag=0
 ;
 i $g(lo)'="" s global="^"_lo
 e  s global="^%"
 ;
 i $g(hi)="" s hi=""
 e  s hi="^"_hi
 ;
 s return="["
 ;
 i $d(@global) d
 . s return=return_""""_$e(global,2,$l(global))_""", "
 . ;
 . i max=1 s flag=1 q
 . i max>1 s max=max-1
 ;
 f  s global=$o(@global) q:flag!(global="")!(global]]hi&(hi]""))  d
 . s return=return_""""_$e(global,2,$l(global))_""", "
 . ;
 . i max>0 s cnt=cnt+1 s:cnt>max flag=1
 ;
 s:$l(return)>2 return=$e(return,1,$l(return)-2)
 s return=return_"]"
 ;
 quit return
 ;
 ;
increment(glvn,subs,incr,mode) ;increment the number in a global node
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n globalname,increment
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 ;
 s increment=$i(@globalname,$g(incr,1))
 ;
 s increment=$$oescape(increment)
 s increment=$$oconvert(increment,mode)
 s glvn=$$oescape(glvn) ;for extended references
 ;
 quit "{""ok"": 1, ""global"": """_glvn_""", ""data"": "_increment_"}"
 ;
 ;
kill(glvn,subs,mode) ;kill a global or global node
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n globalname
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 ;
 s glvn=$$oescape(glvn) ;for extended references
 ;
 k @globalname
 ;
 quit "{""ok"": 1, ""global"": """_glvn_""", ""result"": ""0""}"
 ;
 ;
lock(glvn,subs,timeout,mode) ;lock a global node, incrementally
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n globalname,result
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 ;
 s result="0"
 ;
 i timeout=-1 d  ;if no timeout is passed by user, a -1 is passed
 . l +@globalname
 . i $t s result="1"
 e  d
 . l +@globalname:timeout
 . i $t s result="1"
 ;
 s glvn=$$oescape(glvn) ;for extended references
 ;
 quit "{""ok"": 1, ""global"": """_glvn_""", ""result"": """_result_"""}"
 ;
 ;
merge(fglvn,fsubs,tglvn,tsubs,mode) ;merge an array node to another array node
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n fglobalname,fosubs,return,tglobalname,tosubs
 ;
 ;process for output without going through M
 s fosubs=$$parse(fsubs,"pass",mode)
 s tosubs=$$parse(tsubs,"pass",mode)
 ;
 s fsubs=$$parse(fsubs,"input",mode)
 s fglobalname=$$construct(fglvn,fsubs)
 ;
 s tsubs=$$parse(tsubs,"input",mode)
 s tglobalname=$$construct(tglvn,tsubs)
 ;
 m @tglobalname=@fglobalname
 ;
 s fglvn=$$oescape(fglvn) ;for extended references
 s tglvn=$$oescape(tglvn) ;for extended references
 ;
 s return="{""ok"": 1, ""global"": """_fglvn_""","
 ;
 i fosubs'=""!(tosubs'="") d
 . s return=return_" ""subscripts"": ["
 . i fosubs'="" s return=return_fosubs_", "
 . s return=return_""""_tglvn_""""
 . i tosubs'="" s return=return_", "_tosubs
 . s return=return_"],"
 ;
 s return=return_" ""result"": ""1""}"
 ;
 quit return
 ;
 ;
nextNode(glvn,subs,mode) ;return the next global node, depth first
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n data,defined,globalname,nsubs,result,return
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 ;
 s result=$q(@globalname)
 ;
 i result="" s defined=0
 e  s defined=1
 ;
 i defined d
 . n sub
 . ;
 . s data=@result
 . ;
 . s data=$$oescape(data)
 . s data=$$oconvert(data,mode)
 . ;
 . i $e(result)="^" s $e(result)=""
 . ;
 . s nsubs=""
 . ;
 . f i=1:1:$ql(result) d
 . . s sub=$$oescape($qs(result,i))
 . . s sub=$$oconvert(sub,mode)
 . . ;
 . . s nsubs=nsubs_", "_sub
 . ;
 . s $e(nsubs,1,2)=""
 ;
 s glvn=$$oescape(glvn) ;for extended references
 ;
 s return="{""ok"": 1, ""global"": """_glvn_""","
 ;
 i defined,nsubs'="" s return=return_" ""subscripts"": ["_nsubs_"],"
 ;
 s return=return_" ""defined"": "_defined
 ;
 i defined s return=return_", ""data"": "_data_"}"
 e  s return=return_"}"
 ;
 ;
 quit return
 ;
 ;
order(glvn,subs,mode,order) ;return the next global node at the same level
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n globalname,result
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 ;
 i $g(order)=-1 s result=$o(@globalname,-1)
 e  s result=$o(@globalname)
 ;
 i subs="",$e(result)="^" s $e(result)=""
 ;
 s result=$$oescape(result)
 s result=$$oconvert(result,mode)
 s glvn=$$oescape(glvn) ;for extended references
 ;
 quit "{""ok"": 1, ""global"": """_glvn_""", ""result"": "_result_"}"
 ;
 ;
previous(glvn,subs,mode) ;same as order, only in reverse
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 quit $$order(glvn,$g(subs),mode,-1)
 ;
 ;
previousNode() ;same as nextNode, only in reverse
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 quit "{""status"": ""previous_node not yet implemented""}"
 ;
 ;
procedure(proc,args,relink,mode) ;call an arbitrary procedure/subroutine
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n procedure,result
 ;
 s args=$$parse($g(args),"input",mode)
 ;
 ;link latest routine image containing procedure/subroutine in auto-relinking mode
 i relink zl $tr($s(proc["^":$p(proc,"^",2),1:proc),"%","_")
 ;
 s procedure=proc_$s(args'="":"("_args_")",1:"")
 ;
 i procedure'["^" s procedure="^"_procedure
 ;
 d
 . n proc d @procedure
 ;
 s return="{""ok"": 1, ""procedure"": """_proc_"""}"
 ;
 quit return
 ;
 ;
retrieve() ;not yet implemented
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 quit "{""status"": ""retrieve not yet implemented""}"
 ;
 ;
set(glvn,subs,data,mode) ;set a global node
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n globalname
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 ;
 s data=$$iescape(data)
 s data=$$iconvert(data,mode)
 ;
 s $e(data)=$tr($e(data),"""","")
 s $e(data,$l(data))=$tr($e(data,$l(data)),"""","")
 ;
 s @globalname=data
 ;
 s data=$$oescape(data)
 s data=$$oconvert(data,mode)
 s glvn=$$oescape(glvn) ;for extended references
 ;
 quit "{""ok"": 1, ""global"": """_glvn_""", ""data"": "_data_", ""result"": ""0""}"
 ;
 ;
unlock(glvn,subs,mode) ;unlock a global node, incrementally, or release all locks
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n globalname
 ;
 s subs=$$parse($g(subs),"input",mode)
 s globalname=$$construct(glvn,subs)
 ;
 i glvn=""&(subs="") l  quit """0"""
 e  l -@globalname
 ;
 s glvn=$$oescape(glvn) ;for extended references
 ;
 quit "{""ok"": 1, ""global"": """_glvn_""", ""result"": ""0""}"
 ;
 ;
update() ;not yet implemented
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 quit "{""status"": ""update not yet implemented""}"
 ;
 ;
version() ;return the version string
 u $p:ctrap="$c(3)" ;handle a Ctrl-C/SIGINT, while in GT.M, in a clean manner
 ;
 n version
 ;
 s version=$zv
 s $p(version," ")=""
 ;
 quit "Node.js Adaptor for GT.M: Version: 0.9.0 (FWSLC); GT.M version:"_version
 ;
