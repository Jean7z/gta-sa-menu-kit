#!/usr/bin/env python3
"""Genera el bundle de CAs como C compilable (sin ficheros ni rutas en runtime).

Fuente: cacert.pem de curl (https://curl.se/ca/cacert.pem, bundle Mozilla).

Uso:
    curl -sL https://curl.se/ca/cacert.pem -o /tmp/cacert.pem
    ./gen_ca.py /tmp/cacert.pem ca_bundle.c
"""
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    pem = open(sys.argv[1], "r", encoding="utf-8").read().splitlines()

    out, inside = [], False
    for line in pem:
        if line.startswith("-----BEGIN CERTIFICATE"):
            inside = True
        if inside:
            out.append(line)
        if line.startswith("-----END CERTIFICATE"):
            inside = False
    if not out:
        print("no PEM certificates in " + sys.argv[1])
        return 1

    with open(sys.argv[2], "w", encoding="ascii") as f:
        f.write("/* Generated from cacert.pem by gen_ca.py - do not edit by hand.\n")
        f.write("   Mozilla CA bundle: %d roots. */\n"
                % sum(1 for l in out if l.startswith("-----BEGIN CERTIFICATE")))
        f.write('const char radio_ca_pem[] =\n')
        for line in out:
            f.write('    "%s\\n"\n' % line.replace("\\", "\\\\").replace('"', '\\"'))
        f.write("    ;\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
