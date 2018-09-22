#
# binding.gyp - Nodem build script
#
# Written by David Wicksell <dlw@linux.com>
# Copyright © 2012-2016,2018 Fourth Watch Software LC
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU Affero General Public License (AGPL)
# as published by the Free Software Foundation, either version 3 of
# the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Affero General Public License for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see http://www.gnu.org/licenses/.
#

{
  'targets': [
    {
      'target_name': 'mumps',
      'type': 'loadable_module',
      'sources': [
        'src/mumps.cc',
        'src/gtm.cc',
        'src/ydb.cc'
      ],
      'cflags': [
        '-ansi',
        '-pedantic',
        '-error',
        '-std=c++11',
        '-Wno-deprecated-declarations',
        '-Wno-expansion-to-defined'
      ],
      'variables': {
        'gtm_dist%': '<!(if [ -n "$ydb_dist" ]; then echo $ydb_dist; else echo ${gtm_dist:-.}; fi)',
        'gtm_lib%': '<!(if [ -n "$ydb_dist" ]; then echo yottadb; else echo gtmshr; fi)'
      },
      'defines': [
        'GTM_CIP_API=<!(if [ "$(echo \'w $e($tr($p($zv," ",2),"V."),1,2)\' | <(gtm_dist)/mumps -dir | grep -Ev "^$|>")" -lt 55 ]; then echo 0; else echo 1; fi)',
        'YDB_SIMPLE_API=0'
      ],
      'include_dirs': [
        '<(gtm_dist)'
      ],
      'libraries': [
        '-L<(gtm_dist)',
        '-l<(gtm_lib)'
      ],
      'ldflags': [
        '-Wl,-rpath,<(gtm_dist),--enable-new-dtags'
      ]
    }
  ]
}
