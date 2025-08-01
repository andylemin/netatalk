run_command(
    flex,
    '-o', 'spotlight_rawquery_lexer.c',
    'spotlight_rawquery_lexer.l',
    check: true,
)
run_command(
    bison,
    '-d',
    '-o', 'sparql_parser.c',
    'sparql_parser.y',
    check: true,
)

srp_sources = ['sparql_map.c', 'sparql_parser.c', 'spotlight_rawquery_lexer.c']

spotlight_deps = [
    bstring,
    glib,
    iniparser,
    talloc,
    sparql,
]

if have_iconv
    spotlight_deps += iconv
endif

if use_mysql_backend
    spotlight_deps += mysqlclient
endif

executable(
    'srp',
    srp_sources,
    include_directories: root_includes,
    link_with: libatalk,
    dependencies: spotlight_deps,
    c_args: ['-DMAIN'],
    install: false,
)

spotlight_sources = [
    'sparql_map.c',
    'sparql_parser.c',
    'spotlight_rawquery_lexer.c',
]

libspotlight = static_library(
    'libspotlight',
    spotlight_sources,
    dependencies: [
        bstring,
        glib,
        iniparser,
        talloc,
        sparql,
    ],
    include_directories: root_includes,
    c_args: ['-DDBUS_API_SUBJECT_TO_CHANGE', statedir],
    install: false,
)
