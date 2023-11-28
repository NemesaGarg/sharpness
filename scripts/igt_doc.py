#!/usr/bin/env python3
# pylint: disable=C0301
# SPDX-License-Identifier: (GPL-2.0 OR MIT)

## Copyright (C) 2023    Intel Corporation                 ##
## Author: Mauro Carvalho Chehab <mchehab@kernel.org>      ##
##                                                         ##
## Allow keeping inlined test documentation and validate   ##
## if the documentation is kept updated.                   ##

"""Maintain test plan and test implementation documentation on IGT."""

import argparse
import sys

from test_list import TestList

def main():
    """
    Main logic
    """

    igt_build_path = 'build'

    parser = argparse.ArgumentParser(description = "Print formatted kernel documentation to stdout.",
                                    formatter_class = argparse.ArgumentDefaultsHelpFormatter,
                                    epilog = 'If no action specified, assume --rest.')
    parser.add_argument("--config", required = True,
                        help="JSON file describing the test plan template")
    parser.add_argument("--rest",
                        help="Output documentation from the source files in REST file.")
    parser.add_argument("--per-test", action="store_true",
                        help="Modifies ReST output to print subtests per test.")
    parser.add_argument("--to-json",
                        help="Output test documentation in JSON format as TO_JSON file")
    parser.add_argument("--show-subtests", action="store_true",
                        help="Shows the name of the documented subtests in alphabetical order.")
    parser.add_argument("--sort-field",
                        help="modify --show-subtests to sort output based on SORT_FIELD value")
    parser.add_argument("--filter-field", nargs='*',
                        help="filter subtests based on regular expressions given by FILTER_FIELD=~'regex'")
    parser.add_argument("--check-testlist", action="store_true",
                        help="Compare documentation against IGT built tests.")
    parser.add_argument("--include-plan", action="store_true",
                        help="Include test plans, if any.")
    parser.add_argument("--igt-build-path",
                        help="Path to the IGT build directory. Used by --check-testlist.",
                        default=igt_build_path)
    parser.add_argument("--gen-testlist",
                        help="Generate documentation at the GEN_TESTLIST directory, using SORT_FIELD to split the tests. Requires --sort-field.")
    parser.add_argument('--files', nargs='+',
                        help="File name(s) to be processed")

    parse_args = parser.parse_args()

    tests = TestList(config_fname = parse_args.config,
                        include_plan = parse_args.include_plan,
                        file_list = parse_args.files,
                        igt_build_path = parse_args.igt_build_path)

    if parse_args.filter_field:
        for filter_expr in parse_args.filter_field:
            tests.add_filter(filter_expr)

    run = False
    if parse_args.show_subtests:
        run = True
        tests.show_subtests(parse_args.sort_field)

    if parse_args.check_testlist:
        run = True
        tests.check_tests()

    if parse_args.gen_testlist:
        run = True
        if not parse_args.sort_field:
            sys.exit("Need a field to split the testlists")
        tests.gen_testlist(parse_args.gen_testlist, parse_args.sort_field)

    if parse_args.to_json:
        run = True
        tests.print_json(parse_args.to_json)

    if not run or parse_args.rest:
        if parse_args.per_test:
            tests.print_rest_flat(parse_args.rest)
        else:
            tests.print_nested_rest(parse_args.rest)

if __name__ == '__main__':
    main()
