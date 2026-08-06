src_inc = include_directories('../src')

test_flow_key = executable('test_flow_key',
  'test_flow_key.c',
  include_directories : src_inc,
  install : false,
)
test('flow_key_layout', test_flow_key)

test_ip_format = executable('test_ip_format',
  'test_ip_format.c',
  install : false,
)
test('ip_format', test_ip_format)
