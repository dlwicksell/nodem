{
  'targets': [
    {
      'target_name': 'mumps',
      'sources': [
        'src/mumps.cc'
      ],
      'conditions': [
        ['target_arch == "x64"', {
          'variables': {
            'gtm_dist%': '/opt/lsb-gtm/6.0-001_x8664'
          }
        }, {
          'variables': {
            'gtm_dist%': '/opt/lsb-gtm/6.0-001_i686'
          }
        }]
      ],
      'variables': {
        'gtm_link%': '/home/vista/lib/gtm'
      },
      'include_dirs': [
        '<(gtm_dist)'
      ],
      'libraries': [
        '-L<(gtm_dist)',
        '-lgtmshr'
      ],
      'ldflags': [
        '-Wl,-rpath,<(gtm_dist),--enable-new-dtags',
        '-Wl,-rpath,<(gtm_link),--enable-new-dtags'
      ]
    }
  ]
}
