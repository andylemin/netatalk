util_sources = [
    'atalk_addr.c',
    'bprint.c',
    'cnid.c',
    'fault.c',
    'getiface.c',
    'gettok.c',
    'locking.c',
    'logger.c',
    'netatalk_conf.c',
    'queue.c',
    'server_child.c',
    'server_ipc.c',
    'server_lock.c',
    'socket.c',
    'strdicasecmp.c',
    'unix.c',
]

libutil = static_library(
    'util',
    util_sources,
    include_directories: root_includes,
    dependencies: [bstring, iniparser],
    c_args: [
        confdir,
        messagedir,
        afpdpwfile,
        uamdir,
        statedir,
        dversion,
    ],
    install: false,
)
