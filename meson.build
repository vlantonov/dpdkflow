project('dpdkflow', 'c',
  version         : '0.1.1',
  default_options : ['c_std=c17', 'warning_level=2'],
)
# Expose POSIX/GNU extensions; required by sigaction, ssize_t, usleep, and
# DPDK headers (rte_ring_elem.h uses ssize_t).
add_project_arguments('-D_GNU_SOURCE', language : 'c')
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

subdir('tests')
