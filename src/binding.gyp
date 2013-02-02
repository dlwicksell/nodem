{
  'targets': [
    {
      'target_name': 'mumps',
      'sources': [
        'src/mumps.cc'
      ],
      'variables': {
        'gtm_root%': '/opt/lsb-gtm/6.0-001_x8664'
      },
      'libraries': [
        '-L<(gtm_root)',
        '-lgtmshr'
      ],
      'include_dirs': [
        '<(gtm_root)'
      ]
    }
  ]
}
