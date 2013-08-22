node() ;;2013-08-22  12:41 PM
 ;
 ; Written by David Wicksell <dlw@linux.com>
 ; Copyright © 2012,2013 Fourth Watch Software, LC
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
 n globalname,subscripts
 ;
 s subscripts=$$parse(.subs,"i")
 s globalname="^"_glvn_$s(subscripts'="":"("_subscripts_")",1:"")
 ;
 quit globalname
 ;
 ;
convert:(data,dir,type) ;convert to numbers or strings
 n ndata
 ;
 i dir="i" d
 . i data=+data s ndata=data
 . e  i $e(data,1,2)="0.",$e(data,2,$l(data))=+$e(data,2,$l(data)) d
 . . s $e(data)="",ndata=data
 . e  i type="sub" s ndata=""""_data_""""
 . e  s ndata=data
 e  i dir="o" d
 . i data=+data d
 . . i $e(data)="." s ndata=0_data
 . . e  s ndata=data
 . e  i type="data" s ndata=""""_data_""""
 . e  s ndata=data
 ;
 q ndata
 ;
 ;
escape:(data) ;escape quotes or control characters within a string
 n i,charh,charl,ndata
 ;
 i data[""""!(data["\")!(data?.e1c.e) d
 . s ndata=""
 . f i=1:1:$l(data) d
 . . i ($e(data,i)="""")!($e(data,i)="\") s ndata=ndata_"\"_$e(data,i)
 . . e  i $e(data,i)?1c d
 . . . s charh=$a($e(data,i))\16,charh=$e("0123456789abcdef",charh+1)
 . . . s charl=$a($e(data,i))#16,charl=$e("0123456789abcdef",charl+1)
 . . . s ndata=ndata_"\u00"_charh_charl
 . . e  s ndata=ndata_$e(data,i)
 e  s ndata=data
 ;
 quit ndata
 ;
 ;
parse:(subs,dir) ;parse an argument list or list of subscripts
 n i,num,sub,temp,tmp
 ;
 s (temp,tmp)=""
 ;
 i '$d(subs)#10 s subs=""
 i subs'="" d
 . f  q:subs=""  d
 . . s num=+subs,$e(subs,1,$l(num)+1)=""
 . . s sub=$e(subs,1,num)
 . . i sub["""" d
 . . . f i=1:1:$l(sub) d
 . . . . i $e(sub,i)="""" s tmp=tmp_""""_$e(sub,i)
 . . . . e  s tmp=tmp_$e(sub,i)
 . . . ;
 . . . s sub=tmp
 . . s sub=$$convert(sub,dir,"sub")_","
 . . ;
 . . s temp=temp_sub,$e(subs,1,num+1)=""
 . s subs=temp
 s subs=$e(subs,1,$l(subs)-1)
 ;
 quit subs
 ;
 ;
data(glvn,subs) ;find out if global node has data or children
 n globalname,defined,ok,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,subs)
 s defined=$d(@globalname)
 ;
 s ok=1
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""defined"": "_defined_"}"
 ;
 quit return
 ;
 ;
function(func,args) ;call an arbitrary extrinsic function
 n dev,function,nargs,ok,result,return
 ;
 i '$d(args)#10 s args=""
 s:args'="" nargs=$$parse(args,"o")
 s:args'="" args=$$parse(args,"i")
 ;
 s function=func_$s(args'="":"("_args_")",1:"")
 x "s result=$$"_function
 ;
 s result=$$escape(result)
 s result=$$convert(result,"o","data")
 ;
 s ok=1
 ;
 s return="{""ok"": "_ok_", ""function"": """_func_""","
 i $g(args)'="" s return=return_" ""arguments"": ["_nargs_"],"
 s return=return_" ""result"": "_result_"}"
 ;
 quit return
 ;
 ;
get(glvn,subs) ;get one node of a global
 n data,defined,globalname,ok,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,subs)
 ;
 s data=$g(@globalname)
 ;
 s data=$$escape(data)
 s data=$$convert(data,"o","data")
 ;
 s defined=$d(@globalname)#10
 s ok=1
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""data"": "_data_", ""defined"": "_defined_"}"
 ;
 quit return
 ;
 ;
globalDirectory(max,lo,hi) ;list the globals in a database, filtered or not
 n flag,cnt,global,return
 ;
 i '$d(max)#10 s max=0
 ;
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
 . i max=1 s flag=1 q
 . s:max>1 max=max-1
 ;
 f  s global=$o(@global) q:flag!(global="")!(global]]hi&(hi]""))  d
 . s return=return_""""_$e(global,2,$l(global))_""", "
 . i max>0 s cnt=cnt+1 s:cnt>max flag=1
 ;
 s:$l(return)>2 return=$e(return,1,$l(return)-2)
 s return=return_"]"
 ;
 quit return
 ;
 ;
increment(glvn,subs,incr) ;increment the number in a global node
 n globalname,increment,ok,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,subs)
 s increment=$i(@globalname,$g(incr,1))
 ;
 s ok=1
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""data"": "_increment_"}"
 ;
 quit return
 ;
 ;
kill(glvn,subs) ;kill a global or global node
 n globalname,ok,result,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,subs)
 k @globalname
 ;
 s ok=1,result=0
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""result"": "_result_"}"
 ;
 quit return
 ;
 ;
lock(glvn,subs) ;lock a global node, incrementally
 n globalname,ok,result,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,subs)
 l +@globalname:0
 ;
 s ok=1
 i $t s result="1"
 e  s result="0"
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""result"": """_result_"""}"
 ;
 quit return
 ;
 ;
merge(tglvn,tsubs,fglvn,fsubs) ;merge an array node to another array node
 n tglobalname,fglobalname,ok,result,return
 ;
 s tglobalname=$$construct(tglvn,.tsubs)
 s fglobalname=$$construct(fglvn,.fsubs)
 ;
 m @tglobalname=@fglobalname
 ;
 s ok=1,result=1
 ;
 s return="{""ok"": "_ok_", ""global"": """_fglvn_""","
 i fsubs'="",tsubs'="" d
 . s return=return_" ""subscripts"": ["_fsubs_", """_tglvn_""""
 . s tsubs=$s($l(tsubs,",")>1:", "_$p(tsubs,",",1,$l(tsubs,",")-1),1:"")
 . s return=return_tsubs_"],"
 s return=return_" ""result"": """_result_"""}"
 ;
 quit return
 ;
 ;
nextNode(glvn,subs) ;
 quit "{""status"": ""next_node not yet implemented""}"
 ;
 ;
order(glvn,subs,order) ;return the next global node at the same level
 n globalname,defined,ok,result,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,subs)
 ;
 i $g(order)=-1 s result=$o(@globalname,-1)
 e  s result=$o(@globalname)
 ;
 i $e(result)="^" s $e(result)=""
 ;
 s result=$$escape(result)
 s result=$$convert(result,"o","data")
 ;
 s ok=1
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""result"": "_result_"}"
 ;
 quit return
 ;
 ;
previous(glvn,subs) ;same as order, only in reverse
 i '$d(subs)#10 s subs=""
 ;
 quit $$order(glvn,subs,-1)
 ;
 ;
previousNode(glvn,subs) ;
 quit "{""status"": ""previous_node not yet implemented""}"
 ;
 ;
retrieve() ;
 quit "{""status"": ""retrieve not yet implemented""}"
 ;
 ;
set(glvn,subs,data) ;set a global node
 n globalname,ok,result,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,subs)
 ;
 s data=$$escape(data)
 s data=$$convert(data,"i","data")
 ;
 s @globalname=data
 ;
 s ok=1,result=0
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""result"": "_result_"}"
 ;
 quit return
 ;
 ;
unlock(glvn,subs) ;unlock a global node, incrementally; or release all locks
 n globalname,ok,result,return
 ;
 i '$d(subs)#10 s subs=""
 ;
 s globalname=$$construct(glvn,.subs)
 i glvn=""&(subs="") d
 . l  s return="""0"""
 e  d
 . l -@globalname
 ;
 s ok=1,result=0
 ;
 i $g(return)'="" quit return
 ;
 s return="{""ok"": "_ok_", ""global"": """_glvn_""","
 s return=return_" ""result"": """_result_"""}"
 ;
 quit return
 ;
 ;
update() ;
 quit "{""status"": ""update not yet implemented""}"
 ;
 ;
version() ;return the version string
 quit "Node.js Adaptor for GT.M: Version: 0.3.0 (FWSLC); "_$zv
 ;
