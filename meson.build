project('dpdkflow', 'c',
  version         : '0.1.0',
  default_options : ['c_std=c17', 'warning_level=2'],
)

dpdk = dependency('libdpdk', required : true)

sources = files(
  'src/main.c',
  'src/rx_loop.c',
  'src/flow_table.c',
  'src/stats_server.c',
  'src/signal_handler.c',
)

executable('dpdkflow',
  sources      : sources,
  dependencies : [dpdk],
  install      : false,
)
