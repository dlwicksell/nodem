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
        '-pedantic'
      ],
      'conditions': [
        ['target_arch == "x64"', {
          'variables': {
            'gtm_dist%': '/usr/lib/fis-gtm/V6.0-003_x86_64'
          }
        }, {
          'variables': {
            'gtm_dist%': '/usr/lib/fis-gtm/V6.0-003_i686'
          }
        }]
      ],
      'variables': {
        'gtm_link%': '$(HOME)/lib/gtm',
        'gtm_path%': '/usr/lib/fis-gtm/current'
      },
      'include_dirs': [
        '<(gtm_dist)'
      ],
      'libraries': [
        '-L<(gtm_dist)',
        '-lgtmshr'
      ],
      'defines': [
        'GTM_VERSION=60'
      ],
      'ldflags': [
        '-Wl,-rpath,<(gtm_link),--enable-new-dtags',
        '-Wl,-rpath,<(gtm_dist),--enable-new-dtags',
        '-Wl,-rpath,<(gtm_path),--enable-new-dtags'
      ]
    }
  ]
}
