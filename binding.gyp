#
# Package:    NodeM
# File:       binding.gyp
# Summary:    Nodem build script
# Maintainer: David Wicksell <dlw@linux.com>
#
# Written by David Wicksell <dlw@linux.com>
# Copyright © 2012-2016,2018-2020,2022 Fourth Watch Software LC
#
# This program is free software: you can redistribute it and/or modify it
# under the terms of the GNU Affero General Public License (AGPL) as published
# by the Free Software Foundation, either version 3 of the License, or (at
# your option) any later version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
# for more details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program. If not, see http://www.gnu.org/licenses/.

{
  'targets': [
    {
      'target_name': 'nodem',
      'type': 'loadable_module',
      'sources': [
        'src/nodem.cc',
        'src/gtm.cc',
        'src/ydb.cc'
      ],
      'cflags': [
        '-error',
        '-pedantic',
        '-std=c++11',
        '-Wno-cast-function-type',
        '-Wno-expansion-to-defined'
      ],
      'variables': {
        'dist%': '<!(if [ -n "$ydb_dist" ]; then echo $ydb_dist; else echo ${gtm_dist:-.}; fi)',
        'lib%': '<!(if [ -n "$ydb_dist" -a -e "$ydb_dist/ydb" ]; then echo yottadb; else echo gtmshr; fi)'
      },
      'defines': [
        'NODEM_YDB=<!(if grep -iq yottadb <(dist)/gtm; then echo 1; else echo 0; fi)',
        'NODEM_CIP_API=<!(if [ "$(echo \'w +$tr($p($zv," ",2),"V.-")\' | <(dist)/mumps -dir | grep -Ev "^$|>")" -ge 54002 ]; then echo 1; else echo 0; fi)',
        'NODEM_SIMPLE_API=<!(if [ -e "<(dist)/ydb" ]; then echo 1; else echo 0; fi)'
      ],
      'include_dirs': [
        '<(dist)'
      ],
      'libraries': [
        '-L<(dist)',
        '-l<(lib)'
      ],
      'ldflags': [
        '-Wl,-rpath,<(dist),--enable-new-dtags'
      ],
      'configurations': {
        'Release': {
          'cflags': [
            '-Wno-deprecated-declarations'
          ],
          'ldflags': [
            '-Wl,-s'
          ]
        },
        'Debug': {
          'cflags': [
            '-fno-omit-frame-pointer'
          ]
        }
      }
    }
  ]
}
