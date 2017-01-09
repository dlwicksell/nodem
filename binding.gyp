#
# binding.gyp - Nodem build script
#
# Written by David Wicksell <dlw@linux.com>
# Copyright © 2012-2016 Fourth Watch Software LC
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
        'src/mumps.cc'
      ],
      'cflags': [
        '-ansi',
        '-pedantic',
        '-Wno-deprecated-declarations'
      ],
      'conditions': [
        ['target_arch == "x64"', {
          'variables': {
            'gtm_build%': '/usr/lib/x86_64-linux-gnu/fis-gtm/V6.2-002A-2build1_x86_64',
            'gtm_opt%': '/opt/lsb-fis/gtm/V6.3-000A_x86_64'
          }
        }, {
          'variables': {
            'gtm_build%': '/usr/lib/fis-gtm/V6.0-003_i686',
            'gtm_opt%': '/opt/lsb-fis/gtm/V6.0-003_i686'
          }
        }]
      ],
      'variables': {
        'gtm_dist%': '$(gtm_dist)',
        'gtm_link%': '$(HOME)/lib/gtm',
        'gtm_path%': '/usr/lib/fis-gtm/current'
      },
      'include_dirs': [
        '<(gtm_dist)',
        '<(gtm_link)',
        '<(gtm_path)',
        '<(gtm_build)',
        '<(gtm_opt)'
      ],
      'libraries': [
        '-L<(gtm_dist)',
        '-L<(gtm_link)',
        '-L<(gtm_path)',
        '-L<(gtm_build)',
        '-L<(gtm_opt)',
        '-lgtmshr'
      ],
      'defines': [
        'GTM_VERSION=63'
      ],
      'ldflags': [
        '-Wl,-rpath,<(gtm_dist),--enable-new-dtags',
        '-Wl,-rpath,<(gtm_link),--enable-new-dtags',
        '-Wl,-rpath,<(gtm_path),--enable-new-dtags',
        '-Wl,-rpath,<(gtm_build),--enable-new-dtags',
        '-Wl,-rpath,<(gtm_opt),--enable-new-dtags'
      ]
    }
  ]
}
